/*
 * Copyright (c) 2012, Ben Scott.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "bayeux.h"
#include "json_util.h"
#include "log.h"
#include "transport.h"
#include "str.h"
#include <curl/curl.h>
#include <event2/event.h>
#include <json/json.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


//
// macros
//

#define ARR_SIZEOF(x)     (sizeof(x) / sizeof(*(x)))


//
// constants
//

#define BATCH_ID_INVALID  0xffffffff
#define MSG_ID_INVALID    0xffffffff

typedef enum EMsgType {
  MSG_TYPE_CONNECT,
  MSG_TYPE_EVENT,
  MSG_TYPE_HANDSHAKE,
  MSG_TYPE_PUBLISH,
  MSG_TYPE_SUBSCRIBE,
  MSG_TYPE_UNSUBSCRIBE,
} EMsgType;

// NOTE: keep order in sync with the s_reconnect_strs string table
typedef enum EReconnect {
  RECONNECT_HANDSHAKE,
  RECONNECT_NONE,
  RECONNECT_RETRY,
} EReconnect;

typedef enum EState {
  STATE_UNCONNECTED,
  STATE_CONNECTING,
  STATE_CONNECTED,
} EState;


//
// types
//

typedef void (*RecvFunc)(BayeuxClient* restrict client, json_object* restrict msg);

typedef struct Advice {
  EReconnect  reconnect;
  int         interval_ms;
  int         timeout_ms;
} Advice;

typedef struct Subscription {
  BayeuxEventCallback   callback;
  void*                 context;
} Subscription;

typedef struct Channel {
  char* restrict          name;
  Subscription* restrict  subscriptions;
  int                     capacity;
  int                     count;
} Channel;

typedef struct MsgQueue {
  json_object** restrict  msgs;
  uint32_t* restrict      batch_ids;
  uint32_t* restrict      msg_ids;
  RecvFunc* restrict      funcs;
  int                     capacity;
  int                     count;
} MsgQueue;

struct BayeuxClient {
  Advice                      advice;
  char* restrict              endpoint;
  char* restrict              id;
  uint32_t                    batch_id;
  uint32_t                    message_id;
  EState                      state;
  Transport* restrict         transport;

  struct event* restrict      ev_timeout_connect;
  struct event* restrict      ev_timeout_flush;
  struct event* restrict      ev_timeout_handshake;
  struct event_base* restrict ev_base;
  bool                        owns_ev_base;

  Channel**                   channels;
  int                         channel_capacity;
  int                         channel_count;

  MsgQueue                    outbox;
  int                         handshake_msg_index;
  bool                        connect_msg_in_flight;
};


//
// local vars
//

// NOTE: keep order in sync with the EReconnect enum
static const char* const s_reconnect_strs[] = {
  "handshake",
  "none",
  "retry"
};


//
// local functions
//

static void issue_connect(BayeuxClient* restrict client);
static void issue_handshake(BayeuxClient* restrict client);
static uint32_t next_batch_id(BayeuxClient* client);
static uint32_t next_message_id(BayeuxClient* client);
static void recv_event(BayeuxClient* restrict client, json_object* restrict msg);
static void schedule_connect(BayeuxClient* client);
static void schedule_flush(BayeuxClient* client);


//------------------------------------------------------------------------------
static void channels_subscribe(BayeuxClient* restrict client, const char* restrict name, BayeuxEventCallback callback, void* ctx) {
  // find or create the channel
  Channel* channel = NULL;
  for (int index = 0, count = client->channel_count; index < count; ++index) {
    if (!strcmp(client->channels[index]->name, name)) {
      channel = client->channels[index];
      break;
    }
  }

  if (!channel) {
    // resize if necessary
    if (client->channel_count == client->channel_capacity) {
      client->channel_capacity += 64;
      client->channels = (Channel**)realloc(client->channels, client->channel_capacity * sizeof(Channel*));
    }

    // alloc the channel
    channel = (Channel*)malloc(sizeof(Channel));
    channel->name           = strdup(name);
    channel->capacity       = 4;
    channel->count          = 0;
    channel->subscriptions  = (Subscription*)malloc(channel->capacity * sizeof(Subscription));

    // add the channel to the list
    client->channels[client->channel_count] = channel;
    ++client->channel_count;
  }

  // subscribe to the channel
  if (channel->count == channel->capacity) {
    channel->capacity       = (channel->capacity + 15) & ~15;
    channel->subscriptions  = (Subscription*)realloc(channel->subscriptions, channel->capacity * sizeof(Subscription));
  }
  Subscription* sub = channel->subscriptions + channel->count;
  ++channel->count;
  sub->callback = callback;
  sub->context  = ctx;
}

//------------------------------------------------------------------------------
static void channel_deliver(BayeuxClient* restrict client, const char* restrict target_channel, json_object* restrict msg_data) {
  const char* data_str = json_object_to_json_string(msg_data);

  // find the matching channels
  for (int chan_index = 0, chan_count = client->channel_count; chan_index < chan_count; ++chan_index) {
    Channel* restrict channel = client->channels[chan_index];

    // TODO: support channel globbing
    if (!strcmp(target_channel, channel->name)) {
      for (int sub_index = 0, sub_count = channel->count; sub_index < sub_count; ++sub_index) {
        Subscription* restrict sub = channel->subscriptions + sub_index;

        sub->callback(data_str, sub->context);
      }
    }
  }
}

//------------------------------------------------------------------------------
static void handle_advice(BayeuxClient* restrict client, json_object* restrict msg) {
  // extract the advice object (if any)
  json_object* advice = json_object_object_get(msg, "advice");
  if (advice) {
    // reconnect
    const char* reconnect_str;
    if (json_get_string(&reconnect_str, advice, "reconnect")) {
      bool found = false;
      for (int index = 0; index < ARR_SIZEOF(s_reconnect_strs); ++index) {
        if (!strcmp(reconnect_str, s_reconnect_strs[index])) {
          found = true;
          EReconnect new_val = (EReconnect)index;
          if (new_val != client->advice.reconnect) {
//            log_info("advice reconnect: %s", s_reconnect_strs[index]);
            client->advice.reconnect = new_val;
            break;
          }
        }
      }
      if (!found) {
        log_info("unknown reconnect advice: '%s'", reconnect_str);
      }
    }

    // interval
    int interval_ms;
    if (json_get_int(&interval_ms, advice, "interval")) {
//      if (interval_ms != client->advice.interval_ms) {
//        log_info("advice interval: %d", interval_ms);
//      }
      client->advice.interval_ms = interval_ms;
    }
  }

  // does the server want us to redo the handshake?
  if (client->advice.reconnect == RECONNECT_HANDSHAKE) {
    // reset the state
    client->state = STATE_UNCONNECTED;
    schedule_connect(client);
  }
}

//------------------------------------------------------------------------------
static json_object* json_make_supported_conn_types() {
  json_object* arr = json_object_new_array();
  json_object_array_add(arr, json_object_new_string("long-polling"));
  return arr;
}

//------------------------------------------------------------------------------
static bool json_get_msg_id(uint32_t* restrict id, json_object* restrict msg) {
  const char* id_str;
  if (json_get_string(&id_str, msg, "id")) {
    *id = (uint32_t)strtoul(id_str, NULL, 16);
    return true;
  }
  else {
    *id = MSG_ID_INVALID;
    return false;
  }
}

//------------------------------------------------------------------------------
static uint32_t json_set_msg_id(json_object* msg, BayeuxClient* client) {
  uint32_t msg_id = next_message_id(client);

  char msg_id_str[9];
  snprintf(msg_id_str, sizeof(msg_id_str), "%x", msg_id);
  msg_id_str[sizeof(msg_id_str) - 1] = 0;
  json_set_string(msg, "id", msg_id_str);

  return msg_id;
}

//------------------------------------------------------------------------------
static void msg_queue_create(MsgQueue* queue) {
  queue->capacity     = 64;
  queue->count        = 0;
  queue->msgs         = (json_object**)malloc(queue->capacity * sizeof(json_object*));
  queue->batch_ids    = (uint32_t*)malloc(queue->capacity * sizeof(uint32_t));
  queue->msg_ids      = (uint32_t*)malloc(queue->capacity * sizeof(uint32_t));
  queue->funcs        = (RecvFunc*)malloc(queue->capacity * sizeof(RecvFunc));
}

//------------------------------------------------------------------------------
static void msg_queue_destroy(MsgQueue* queue) {
  free(queue->msgs);
  free(queue->batch_ids);
  free(queue->msg_ids);
  free(queue->funcs);
  memset(queue, 0, sizeof(MsgQueue));
}

//------------------------------------------------------------------------------
static int msg_queue_find_by_msg_id(const MsgQueue* queue, uint32_t id) {
  const uint32_t* restrict ids = queue->msg_ids;
  for (int index = 0, count = queue->count; index < count; ++index) {
    if (id == ids[index]) {
      return index;
    }
  }
  return -1;
}

//------------------------------------------------------------------------------
// gives each message a new message id and builds up the json string
static void msg_queue_flush(Str** restrict msg_json_out, MsgQueue* restrict queue, BayeuxClient* restrict client) {
  int count = queue->count;
  if (count == 0) {
    return;
  }

  uint32_t batch_id = next_batch_id(client);

  Str* msg_json = *msg_json_out;
  msg_json = str_cat(msg_json, "[");

  for (int index = 0, count = queue->count; index < count; ++index) {
    // only accumulate messages that are not already in flight (denoted by the
    // fact that they do not currently have an assigned message id)
    if (queue->msg_ids[index] == MSG_ID_INVALID) {
      // assign the msg id
      json_object* restrict msg = queue->msgs[index];
      uint32_t msg_id = json_set_msg_id(msg, client);
      queue->msg_ids[index] = msg_id;

      // set the client id
      if (client->id) {
        json_set_string(msg, "clientId", client->id);
      }

      // associated the batch id
      queue->batch_ids[index] = batch_id;

      // encode as json
      if (str_len(msg_json) > 1) {
        msg_json = str_cat(msg_json, ",");
      }
      msg_json = str_cat(msg_json, json_object_to_json_string(msg));
    }
  }

  msg_json = str_cat(msg_json, "]");
  *msg_json_out = msg_json;
}

//------------------------------------------------------------------------------
static void msg_queue_flush_one(Str** restrict msg_json_out, MsgQueue* restrict queue, BayeuxClient* restrict client, int index) {
  // don't bother if the msg is already in flight
  if (queue->msg_ids[index] != MSG_ID_INVALID) {
    return;
  }

  uint32_t batch_id = next_batch_id(client);

  Str* msg_json = *msg_json_out;
  msg_json = str_cat(msg_json, "[");

  // assign the msg id
  json_object* restrict msg = queue->msgs[index];
  uint32_t msg_id = json_set_msg_id(msg, client);
  queue->msg_ids[index] = msg_id;

  // set the client id
  if (client->id) {
    json_set_string(msg, "clientId", client->id);
  }

  // associated the batch id
  queue->batch_ids[index] = batch_id;

  // encode as json
  msg_json = str_cat(msg_json, json_object_to_json_string(msg));

  msg_json = str_cat(msg_json, "]");
  *msg_json_out = msg_json;
}

//------------------------------------------------------------------------------
static int msg_queue_push(MsgQueue* restrict queue, json_object* restrict msg, RecvFunc func) {
  if (queue->count >= queue->capacity) {
    queue->capacity     = queue->count + 64;
    queue->msgs         = (json_object**)realloc(queue->msgs, queue->capacity * sizeof(json_object*));
    queue->batch_ids    = (uint32_t*)realloc(queue->batch_ids, queue->capacity * sizeof(uint32_t));
    queue->msg_ids      = (uint32_t*)realloc(queue->msg_ids, queue->capacity * sizeof(uint32_t));
    queue->funcs        = (RecvFunc*)realloc(queue->funcs, queue->capacity * sizeof(RecvFunc));
  }

  // incref msg_queue
  json_object_get(msg);

  int idx = queue->count;
  queue->msgs[idx]      = msg;
  queue->batch_ids[idx] = BATCH_ID_INVALID;
  queue->msg_ids[idx]   = MSG_ID_INVALID;
  queue->funcs[idx]     = func;
  queue->count          = idx + 1;

  return idx;
}

//------------------------------------------------------------------------------
static void msg_queue_remove_ordered(MsgQueue* queue, int remove_index) {
  // decref msg_queue
  json_object* msg = queue->msgs[remove_index];
  json_object_put(msg);

  int move_count = queue->count - remove_index - 1;
  if (remove_index < queue->count - 1) {
    memmove(queue->msgs + remove_index,       queue->msgs + remove_index + 1,       move_count * sizeof(json_object*));
    memmove(queue->batch_ids + remove_index,  queue->batch_ids + remove_index + 1,  move_count * sizeof(uint32_t));
    memmove(queue->msg_ids + remove_index,    queue->msg_ids + remove_index + 1,    move_count * sizeof(uint32_t));
    memmove(queue->funcs + remove_index,      queue->funcs + remove_index + 1,      move_count * sizeof(RecvFunc));
  }

  --queue->count;
}

//------------------------------------------------------------------------------
static uint32_t next_batch_id(BayeuxClient* client) {
  uint32_t id = client->batch_id;
  ++client->batch_id;
  if (client->batch_id == BATCH_ID_INVALID) {
    ++client->batch_id;
  }
  return id;
}

//-----------------------------------------------------------------------------
static uint32_t next_message_id(BayeuxClient* client) {
  uint32_t id = client->message_id;
  ++client->message_id;
  if (client->message_id == MSG_ID_INVALID) {
    ++client->message_id;
  }
  return id;
}

//------------------------------------------------------------------------------
static void on_ev_timeout_connect(evutil_socket_t sock_fd, short what, void* ctx) {
  BayeuxClient* client = (BayeuxClient*)ctx;
  issue_connect(client);
}

//------------------------------------------------------------------------------
static void on_ev_timeout_flush(evutil_socket_t sock_fd, short what, void* ctx) {
  BayeuxClient* client = (BayeuxClient*)ctx;

  Str* msg_json = str_alloc(4096);
  if (client->handshake_msg_index != -1) {
    msg_queue_flush_one(&msg_json, &client->outbox, client, client->handshake_msg_index);
  }
  else {
    msg_queue_flush(&msg_json, &client->outbox, client);
  }
  transport_send(client->transport, msg_json);
  str_free(msg_json);
}

//------------------------------------------------------------------------------
static void on_ev_timeout_handshake(evutil_socket_t sock_fd, short what, void* ctx) {
  BayeuxClient* client = (BayeuxClient*)ctx;
  issue_handshake(client);
}

//------------------------------------------------------------------------------
// called when there was a transport error and the messages were not sent
static void on_batch_send_err(BayeuxClient* client, uint32_t batch_id) {
  uint32_t* restrict batch_ids  = client->outbox.batch_ids;
  uint32_t* restrict msg_ids    = client->outbox.msg_ids;

  for (int index = 0, count = client->outbox.count; index < count; ++index) {
    if (batch_ids[index] == batch_id) {
      // clear the batch and msg ids so this message will get resent in the next batch
      batch_ids[index]  = BATCH_ID_INVALID;
      msg_ids[index]    = MSG_ID_INVALID;
    }
  }

  schedule_flush(client);
}

//------------------------------------------------------------------------------
static void on_msg_recv(BayeuxClient* client, json_object* msg) {
  handle_advice(client, msg);

  // check if the msg is an event delivery msg
  if (NULL != json_object_object_get(msg, "data")) {
    recv_event(client, msg);
  }
  else {
    // server MUST send back a matching msg id
    uint32_t msg_id;
    if (!json_get_msg_id(&msg_id, msg)) {
      log_info("on_msg_recv: missing field 'id'");
      log_info("NOT IMPLEMENTED - need to cleanup or retry");
      return;
    }

    int msg_index = msg_queue_find_by_msg_id(&client->outbox, msg_id);
    if (msg_index == -1) {
      log_info("on_msg_recv: unknown msg id: '%x'", msg_id);
      log_info("NOT IMPLEMENTED - need to cleanup or retry");
      return;
    }

    RecvFunc func = client->outbox.funcs[msg_index];
    func(client, msg);

    // release the message
    msg_queue_remove_ordered(&client->outbox, msg_index);
  }
}

//----------------------------------------------------------------------------
static void on_batch_recv(json_object* msgs, void* ctx) {
  BayeuxClient* client = (BayeuxClient*)ctx;
  for (int index = 0, count = json_object_array_length(msgs); index < count; ++index) {
    json_object* msg = json_object_array_get_idx(msgs, index);
    on_msg_recv(client, msg);
  }

  if (client->outbox.count > 0) {
    schedule_flush(client);
  }
}

//------------------------------------------------------------------------------
static void schedule_connect(BayeuxClient* client) {
  if (client->message_id >= 0x6) {
    return;
  }

  if (0 == evtimer_pending(client->ev_timeout_connect, NULL)) {
    struct timeval tv = {
      client->advice.interval_ms / 1000000,
      client->advice.interval_ms % 1000000
    };
    evtimer_add(client->ev_timeout_connect, &tv);
  }
}
//------------------------------------------------------------------------------
static void schedule_flush(BayeuxClient* client) {
  if (0 == evtimer_pending(client->ev_timeout_flush, NULL)) {
    struct timeval tv = { 0, 0 };
    evtimer_add(client->ev_timeout_flush, &tv);
  }
}

//------------------------------------------------------------------------------
static json_object* make_connect() {
  // MUST contain:
  //  - channel
  //  - clientId                  (added when sent to transport)
  //  - connectionType
  // MAY contain:
  //  - ext                       (unused)
  //  - id                        (added when sent to transport)

  json_object* msg = json_object_new_object();
  json_set_string(msg, "channel",         "/meta/connect");
  json_set_string(msg, "connectionType",  "long-polling");
  return msg;
}

//------------------------------------------------------------------------------
static json_object* make_handshake() {
  // MUST contain:
  //  - channel
  //  - version
  //  - supportedConnectionTypes
  // MAY contain:
  //  - minimumVersion            (unused)
  //  - ext                       (unused)
  //  - id                        (added when sent to transport)

  json_object* msg = json_object_new_object();
  json_set_string(msg, "channel",                   "/meta/handshake");
  json_set_string(msg, "version",                   "1.0");
  json_set_object(msg, "supportedConnectionTypes",  json_make_supported_conn_types());
  return msg;
}

//------------------------------------------------------------------------------
static json_object* make_publish(const char* channel, const char* data) {
  // MUST contain:
  //  - channel
  //  - data
  // MAY contain:
  //  - clientId      (added when sent to transport)
  //  - id            (added when sent to transport)
  //  - ext           (unused)

  // TODO: don't parse the provided data just to convert back to a string :(
  json_object* data_json = json_tokener_parse(data);

  json_object* msg = json_object_new_object();
  json_set_string(msg, "channel", channel);
  json_set_object(msg, "data",    data_json);
  return msg;
}

//------------------------------------------------------------------------------
static json_object* make_subscribe(const char* channel) {
  // MUST contain:
  //  - channel
  //  - clientId      (added when sent to transport)
  //  - subscription
  // MAY contain:
  //  - ext           (unused)
  //  - id            (added when sent to transport)

  json_object* msg = json_object_new_object();
  json_set_string(msg, "channel",       "/meta/subscribe");
  json_set_string(msg, "subscription",  channel);
  return msg;
}

//------------------------------------------------------------------------------
static json_object* make_unsubscribe(const char* channel) {
  // MUST contain:
  //  - channel
  //  - clientId      (added when sent to transport)
  //  - subscription
  // MAY contain:
  //  - ext           (unused)
  //  - id            (added when sent to transport)

  json_object* msg = json_object_new_object();
  json_set_string(msg, "channel",       "/meta/unsubscribe");
  json_set_string(msg, "subscription",  channel);
  return msg;
}

//------------------------------------------------------------------------------
static void recv_connect(BayeuxClient* restrict client, json_object* restrict msg) {
  // schedule another connect message
  client->connect_msg_in_flight = false;
  schedule_connect(client);
}

//------------------------------------------------------------------------------
static void recv_event(BayeuxClient* restrict client, json_object* restrict msg) {
  const char* channel;
  if (!json_get_string(&channel, msg, "channel")) {
    log_info("bad msg: no channel. discarding");
    return;
  }

  json_object* data;
  data = json_object_object_get(msg, "data");
  if (!data) {
    log_info("bad msg: no data. discarding");
    return;
  }

//  log_info("event on %s", channel);

  channel_deliver(client, channel, data);
}

//------------------------------------------------------------------------------
static void recv_handshake_error(BayeuxClient* restrict client, json_object* restrict msg) {
  // spit out the error message (if any)
  const char* err_msg;
  if (json_get_string(&err_msg, msg, "error")) {
    log_info("err detail: %s", err_msg);
  }

  // schedule a future handshake attempt
  struct timeval tv = { client->advice.timeout_ms, 0 };
  evtimer_add(client->ev_timeout_handshake, &tv);

  // new state: UNCONNECTED
  client->state = STATE_UNCONNECTED;
}

//------------------------------------------------------------------------------
static void recv_handshake_success(BayeuxClient* restrict client, json_object* restrict msg) {
  // MUST contain:
  //  - channel
  //  - version
  //  - supportedConnectionTypes
  //  - clientId
  //  - successful
  // MAY contain:
  //  - minimumVersion
  //  - advice
  //  - ext
  //  - id
  //  - authSuccessful

  // ensure the channel is correct
  const char* channel;
  if (!json_get_string(&channel, msg, "channel")) {
    log_info("recv handshake: missing channel");
    recv_handshake_error(client, msg);
    return;
  }
  if (0 != strcmp(channel, "/meta/handshake")) {
    log_info("recv handshake: wrong channel");
    recv_handshake_error(client, msg);
    return;
  }

  // extract the client id
  const char* client_id;
  if (!json_get_string(&client_id, msg, "clientId")) {
    log_info("recv handshake: missing 'clientId'");
    recv_handshake_error(client, msg);
    return;
  }
  free(client->id);
  client->id = strdup(client_id);

  // new state: CONNECTED
  client->state = STATE_CONNECTED;

//  // issue the connect
//  json_object* msg_connect = make_connect();
//  msg_queue(client, MSG_TYPE_CONNECT, msg_connect);
//  json_object_put(msg_connect);
}

//------------------------------------------------------------------------------
static void recv_handshake(BayeuxClient* restrict client, json_object* restrict msg) {
  client->handshake_msg_index = -1;

  // extract the successful field
  bool success;
  if (!json_get_bool(&success, msg, "successful")) {
    log_info("bad msg: 'successful' field. discarding");
    return;
  }

  if (success) {
    recv_handshake_success(client, msg);
  }
  else {
    recv_handshake_error(client, msg);
  }
}

//------------------------------------------------------------------------------
static void recv_publish(BayeuxClient* restrict client, json_object* restrict msg) {
}

//------------------------------------------------------------------------------
static void recv_subscribe(BayeuxClient* restrict client, json_object* restrict msg) {
  // extract the successful field
  bool success;
  if (!json_get_bool(&success, msg, "successful")) {
    log_info("bad msg: 'successful' field. discarding");
    return;
  }

  // TODO: unsubscribe if not successful
}

//------------------------------------------------------------------------------
static void recv_unsubscribe(BayeuxClient* restrict client, json_object* restrict msg) {
}

//------------------------------------------------------------------------------
static void issue_connect(BayeuxClient* restrict client) {
  // ensure a handshake gets sent first if needed
  issue_handshake(client);

  // bail if the server has requested no more reconnects
  if (client->advice.reconnect == RECONNECT_NONE) {
    return;
  }

  // bail if a connection request is already in flight
  if (client->connect_msg_in_flight) {
    return;
  }

  client->connect_msg_in_flight = true;

  // send a connect
  json_object* msg = make_connect();
  msg_queue_push(&client->outbox, msg, &recv_connect);
  json_object_put(msg);

  schedule_flush(client);
}

//------------------------------------------------------------------------------
static void issue_handshake(BayeuxClient* restrict client) {
  // bail if the handshake has already happened
  if (client->state != STATE_UNCONNECTED) {
    return;
  }

  // bail if the server has requested no more reconnects
  if (client->advice.reconnect == RECONNECT_NONE) {
    return;
  }

  // new state: CONNECTING
  client->state = STATE_CONNECTING;

  // send a handshake
  json_object* msg = make_handshake();
  client->handshake_msg_index = msg_queue_push(&client->outbox, msg, &recv_handshake);
  json_object_put(msg);

  schedule_flush(client);
}


//
// exported functions
//

//-----------------------------------------------------------------------------
void bayeux_client_opts_defaults(BayeuxClientOpts* opts) {
  if (!opts) {
    return;
  }
  opts->interval_ms   = 5000;
  opts->timeout_ms    = 60000;
  opts->ev_base       = NULL;
  opts->log_messages  = 0;
}

//------------------------------------------------------------------------------
BayeuxClient* bayeux_client_create(const char* endpoint, const BayeuxClientOpts* opts) {
  BayeuxClient* client = (BayeuxClient*)malloc(sizeof(BayeuxClient));
  client->advice.reconnect      = RECONNECT_RETRY;
  client->advice.interval_ms    = opts->interval_ms;
  client->advice.timeout_ms     = opts->timeout_ms;
  client->endpoint              = strdup(endpoint);
  client->id                    = NULL;
  client->batch_id              = 0;
  client->message_id            = 0;
  client->state                 = STATE_UNCONNECTED;
  client->handshake_msg_index   = -1;
  client->connect_msg_in_flight = false;
  if (opts->ev_base) {
    client->ev_base             = opts->ev_base;
  }
  else {
    client->ev_base               = event_base_new();
  }
  client->ev_timeout_connect    = evtimer_new(client->ev_base, &on_ev_timeout_connect, client);
  client->ev_timeout_flush      = evtimer_new(client->ev_base, &on_ev_timeout_flush, client);
  client->ev_timeout_handshake  = evtimer_new(client->ev_base, &on_ev_timeout_handshake, client);
  client->owns_ev_base          = NULL != opts->ev_base;

  client->channels              = NULL;
  client->channel_capacity      = 0;
  client->channel_count         = 0;

  msg_queue_create(&client->outbox);

  client->transport           = transport_open(
    endpoint,
    TRANSPORT_TYPE_HTTP_LONG_POLLING,
    client->ev_base,
    &on_batch_recv,
    client,
    opts->log_messages
    );

  return client;
}

//------------------------------------------------------------------------------
void bayeux_client_destroy(BayeuxClient* client) {
  if (!client) {
    return;
  }

  transport_close(client->transport);

  event_free(client->ev_timeout_handshake);
  event_free(client->ev_timeout_flush);
  event_free(client->ev_timeout_connect);
  if (client->owns_ev_base) {
    event_base_free(client->ev_base);
  }

  msg_queue_destroy(&client->outbox);

  free(client->endpoint);
  free(client);
}

//------------------------------------------------------------------------------
void bayeux_client_publish(BayeuxClient* client, const char* channel, const char* data) {
  issue_connect(client);

  // make the message
  json_object* msg = make_publish(channel, data);
  msg_queue_push(&client->outbox, msg, &recv_publish);
  json_object_put(msg);

  schedule_flush(client);
}

//------------------------------------------------------------------------------
void bayeux_client_subscribe(BayeuxClient* client, const char* channel, BayeuxEventCallback callback, void* context) {
  // register the subscription
  channels_subscribe(client, channel, callback, context);

  // connect if not already connected
  issue_connect(client);

  // issue the subscribe message
  json_object* msg = make_subscribe(channel);
  msg_queue_push(&client->outbox, msg, &recv_subscribe);
  json_object_put(msg);

  schedule_flush(client);
}

//------------------------------------------------------------------------------
void bayeux_client_unsubscribe(BayeuxClient* client, const char* channel, BayeuxEventCallback callback, void* context) {
  issue_connect(client);

  // TODO: unregister callback on the channel

  json_object* msg = make_unsubscribe(channel);
  msg_queue_push(&client->outbox, msg, &recv_unsubscribe);
  json_object_put(msg);

  schedule_flush(client);
}

//------------------------------------------------------------------------------
struct event_base* bayeux_client_get_event_base(BayeuxClient* client) {
  return client->ev_base;
}
