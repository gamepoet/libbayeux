// Copyright (c) 2012, Ben Scott.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//   * Redistributions of source code must retain the above copyright
//     notice, this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in the
//     documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "transport.h"
#include "transport_http_long_polling.h"
#include "log.h"
#include "str.h"
#include <curl/curl.h>
#include <event2/event.h>
#include <json/json.h>
#include <stdlib.h>
#include <string.h>


//
// types
//

typedef struct SocketInfo {
  TransportHttpLongPolling* restrict  http;
  curl_socket_t                       sock_fd;
  int                                 action;
  int                                 timeout;
  struct event* restrict              ev;
} SocketInfo;

typedef struct ConnInfo {
  TransportHttpLongPolling* restrict  http;
  CURL* restrict                      curl_easy;
  char*                               data;
  size_t                              data_capacity;
  size_t                              data_count;
  char                                error[CURL_ERROR_SIZE];
} ConnInfo;


//
// local functions
//

static void socket_create(curl_socket_t s, CURL* curl, int what, TransportHttpLongPolling* http);
static void socket_destroy(SocketInfo* sock);
static void socket_set(SocketInfo* sock, curl_socket_t s, CURL* curl, int what, TransportHttpLongPolling* http);


//------------------------------------------------------------------------------
static void check_curl_mcode(int mcode, const char* msg) {
  if (mcode != CURLM_OK) {
    const char* err_str;
    switch (mcode) {
      case CURLM_BAD_HANDLE:          err_str = "CURLM_BAD_HANDLE";         break;
      case CURLM_BAD_EASY_HANDLE:     err_str = "CURLM_BAD_EASY_HANDLE";    break;
      case CURLM_BAD_SOCKET:          err_str = "CURLM_BAD_SOCKET";         break;
      case CURLM_CALL_MULTI_PERFORM:  err_str = "CURLM_CALL_MULTI_PERFORM"; break;
      case CURLM_INTERNAL_ERROR:      err_str = "CURLM_INTERNAL_ERROR";     break;
      case CURLM_LAST:                err_str = "CURLM_LAST";               break;
      case CURLM_OUT_OF_MEMORY:       err_str = "CURLM_OUT_OF_MEMORY";      break;
      case CURLM_UNKNOWN_OPTION:      err_str = "CURLM_UNKNOWN_OPTION";     break;
      default:                        err_str = "CURLM_unknown";            break;
    }
    log_info("%s returns %s", msg, err_str);
  }
}

//------------------------------------------------------------------------------
static void check_curl_multi(TransportHttpLongPolling* http) {
//  log_info("active connections: %d", http->active_conn_count);

  CURLMsg* msg;
  int msgs_left;
  while (NULL != (msg = curl_multi_info_read(http->curl_multi, &msgs_left))) {
    if (msg->msg == CURLMSG_DONE) {
      // this handle is done, cleanup
      char* url;
      ConnInfo* conn;
      CURL* easy    = msg->easy_handle;
      curl_easy_getinfo(easy, CURLINFO_PRIVATE, &conn);
      curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &url);
      curl_multi_remove_handle(http->curl_multi, easy);
      curl_easy_cleanup(easy);

      if (http->log_messages) {
        log_info("recv: %s\n", conn->data);
      }

      // convert to json
      json_object* msgs = json_tokener_parse(conn->data);

      // notify callback
      http->recv_callback(msgs, http->recv_callback_ctx);

      json_object_put(msgs);

      // cleanup memory
      free(conn->data);
      free(conn);
    }
  }
}

//------------------------------------------------------------------------------
// called by curl when the socket status changes
static int on_curl_multi_socket(CURL* curl, curl_socket_t s, int what, void* ctx, void* sock_ctx) {
  TransportHttpLongPolling* http = (TransportHttpLongPolling*)ctx;
  SocketInfo* sock = (SocketInfo*)sock_ctx;

  if (what == CURL_POLL_REMOVE) {
    socket_destroy(sock);
  }
  else {
    if (!sock) {
      socket_create(s, curl, what, http);
    }
    else {
      socket_set(sock, s, curl, what, http);
    }
  }

  return 0;
}

//------------------------------------------------------------------------------
// called by curl when it wants to update the timer
static int on_curl_multi_timer(CURLM* multi, long timeout_ms, void* ctx) {
  TransportHttpLongPolling* http = (TransportHttpLongPolling*)ctx;
//  log_info("setting timeout to %ldms", timeout_ms);

  struct timeval tv;
  tv.tv_sec   = timeout_ms / 1000;
  tv.tv_usec  = (timeout_ms % 1000) * 1000;
  evtimer_add(http->ev_timer, &tv);
  return 0;
}

//------------------------------------------------------------------------------
static size_t on_curl_write(char* buf, size_t size, size_t count, void* ctx) {
  ConnInfo* conn = (ConnInfo*)ctx;

  // resize if needed
  size_t bytes = size * count;
  size_t size_needed = conn->data_count + bytes + 1;  // +1 for null terminator
  if (size_needed > conn->data_capacity) {
    conn->data_capacity = (size_needed + 4093) & ~4093;
    conn->data          = (char*)realloc(conn->data, conn->data_capacity);
  }

  // copy
  memcpy(conn->data + conn->data_count, buf, bytes);
  conn->data_count += bytes;

  // ensure data is null-terminated
  conn->data[conn->data_count] = 0;

  return bytes;
}

//------------------------------------------------------------------------------
// called by libevent when a socket is ready
static void on_ev_event(int fd, short kind, void* ctx) {
  TransportHttpLongPolling* http = (TransportHttpLongPolling*)ctx;

  int action = 0;
  if (kind & EV_READ) {
    action |= CURL_CSELECT_IN;
  }
  if (kind & EV_WRITE) {
    action |= CURL_CSELECT_OUT;
  }

  CURLMcode mcode = curl_multi_socket_action(
    http->curl_multi,
    fd,
    action,
    &http->active_conn_count
    );
  check_curl_mcode(mcode, "on_ev_event: curl_multi_socket_action");
  check_curl_multi(http);

  if (http->active_conn_count <= 0) {
//    log_info("last conn done. stopping timeout");
    if (evtimer_pending(http->ev_timer, NULL)) {
      evtimer_del(http->ev_timer);
    }
  }
}

//------------------------------------------------------------------------------
// called by libevent when the timeout expires
static void on_ev_timer(int fd, short kind, void* ctx) {
  TransportHttpLongPolling* http = (TransportHttpLongPolling*)ctx;

  // tell curl the timeout expired on the socket
  CURLMcode mcode = curl_multi_socket_action(
    http->curl_multi,
    CURL_SOCKET_TIMEOUT,
    0,
    &http->active_conn_count
    );
  check_curl_mcode(mcode, "on_curl_multi_timer: curl_multi_socket_action");
  check_curl_multi(http);
}

//------------------------------------------------------------------------------
static void socket_create(curl_socket_t s, CURL* curl, int what, TransportHttpLongPolling* http) {
  SocketInfo* sock = (SocketInfo*)calloc(sizeof(SocketInfo), 1);
  sock->http = http;
  socket_set(sock, s, curl, what, http);
  curl_multi_assign(http->curl_multi, s, sock);
}

//------------------------------------------------------------------------------
static void socket_destroy(SocketInfo* sock) {
  if (sock && sock->ev) {
    event_del(sock->ev);
  }
  free(sock);
}

//------------------------------------------------------------------------------
static void socket_set(SocketInfo* sock, curl_socket_t sock_fd, CURL* curl, int what, TransportHttpLongPolling* http) {
  int kind = EV_PERSIST;
  if (what & CURL_POLL_IN) {
    kind |= EV_READ;
  }
  if (what & CURL_POLL_OUT) {
    kind |= EV_WRITE;
  }

  sock->sock_fd = sock_fd;
  sock->action  = what;
  if (sock->ev) {
    event_del(sock->ev);
  }
  sock->ev = event_new(http->ev_base, sock_fd, kind, &on_ev_event, http);
  event_add(sock->ev, NULL);
}


//
// functions
//

//------------------------------------------------------------------------------
void transport_http_long_polling_create(TransportHttpLongPolling* restrict transport, struct event_base* restrict ev_base, const char* restrict url, TransportRecvCallback callback, void* callback_ctx, int log_messages) {
  transport->ev_base            = ev_base;
  transport->ev_timer           = evtimer_new(ev_base, &on_ev_timer, transport);
  transport->active_conn_count  = 0;
  transport->url                = strdup(url);
  transport->recv_callback      = callback;
  transport->recv_callback_ctx  = callback_ctx;
  transport->log_messages       = log_messages;

  transport->http_headers       = curl_slist_append(NULL, "Content-Type: application/json");

  // setup multi curl
  transport->curl_multi = curl_multi_init();
  curl_multi_setopt(transport->curl_multi, CURLMOPT_SOCKETFUNCTION, &on_curl_multi_socket);
  curl_multi_setopt(transport->curl_multi, CURLMOPT_SOCKETDATA,     transport);
  curl_multi_setopt(transport->curl_multi, CURLMOPT_TIMERFUNCTION,  &on_curl_multi_timer);
  curl_multi_setopt(transport->curl_multi, CURLMOPT_TIMERDATA,      transport);
}

//------------------------------------------------------------------------------
void transport_http_long_polling_destroy(TransportHttpLongPolling* transport) {
  // TODO: remove easy handles and clean them up

  curl_multi_cleanup(transport->curl_multi);
  curl_slist_free_all(transport->http_headers);
  evtimer_del(transport->ev_timer);
  free(transport->url);

  // zero everything out
  memset(transport, 0, sizeof(TransportHttpLongPolling));
}

//------------------------------------------------------------------------------
void transport_http_long_polling_request(TransportHttpLongPolling* restrict transport, const char* restrict msg) {
  // create an http post request
  ConnInfo* conn = (ConnInfo*)calloc(1, sizeof(ConnInfo));

  conn->curl_easy    = curl_easy_init();
  if (!conn->curl_easy) {
    log_info("curl_easy_init() failed");
    free(conn);
    return;
  }
  conn->http = transport;

  curl_easy_setopt(conn->curl_easy, CURLOPT_COPYPOSTFIELDS, msg);
  curl_easy_setopt(conn->curl_easy, CURLOPT_ERRORBUFFER,    conn->error);
  curl_easy_setopt(conn->curl_easy, CURLOPT_HTTPHEADER,     transport->http_headers);
  curl_easy_setopt(conn->curl_easy, CURLOPT_POST,           1);
  curl_easy_setopt(conn->curl_easy, CURLOPT_PRIVATE,        conn);
  curl_easy_setopt(conn->curl_easy, CURLOPT_URL,            transport->url);
  curl_easy_setopt(conn->curl_easy, CURLOPT_WRITEDATA,      conn);
  curl_easy_setopt(conn->curl_easy, CURLOPT_WRITEFUNCTION,  &on_curl_write);

  if (transport->log_messages) {
    log_info("send: %s\n", msg);
  }

  CURLMcode mcode = curl_multi_add_handle(transport->curl_multi, conn->curl_easy);
  check_curl_mcode(mcode, "new conn: curl_multi_add_handle");
}
