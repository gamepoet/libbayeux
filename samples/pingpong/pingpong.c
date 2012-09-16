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

#include <bayeux.h>
#include <event2/event.h>
#include <stdio.h>
#include <stdlib.h>

static const char* DEFAULT_ENDPOINT = "http://localhost:5000/faye";

static struct event_base* s_ev_base;
static struct event*      s_ev_timer_ping;
static struct event*      s_ev_timer_pong;
static int                s_ping_count;
static int                s_pong_count;

//------------------------------------------------------------------------------
static void signal_handler(int sig) {
  if (sig == SIGINT) {
    printf("exiting ...");
    event_base_loopbreak(s_ev_base);
  }
}

//------------------------------------------------------------------------------
static void send_ping(BayeuxClient* client) {
  char msg[64];
  snprintf(msg, sizeof(msg), "{\"ping\": %d}", s_ping_count);
  ++s_ping_count;
  bayeux_client_publish(client, "/ping", msg);
}

//------------------------------------------------------------------------------
static void send_pong(BayeuxClient* client) {
  char msg[64];
  snprintf(msg, sizeof(msg), "{\"pong\": %d}", s_pong_count);
  ++s_pong_count;
  bayeux_client_publish(client, "/pong", msg);
}

//------------------------------------------------------------------------------
static void on_timeout_send_ping(evutil_socket_t sock_fd, short what, void* ctx) {
  send_ping((BayeuxClient*)ctx);
}

//------------------------------------------------------------------------------
static void on_timeout_send_pong(evutil_socket_t sock_fd, short what, void* ctx) {
  send_pong((BayeuxClient*)ctx);
}

//------------------------------------------------------------------------------
static void ping(const char* message, void* context) {
  printf("/ping: %s\n", message);

  struct timeval tv = { 1, 0 };
  evtimer_add(s_ev_timer_pong, &tv);
}

//------------------------------------------------------------------------------
static void pong(const char* message, void* context) {
  printf("/pong: %s\n", message);

  struct timeval tv = { 1, 0 };
  evtimer_add(s_ev_timer_ping, &tv);
}

//------------------------------------------------------------------------------
int main(int argc, char** argv) {
  const char* endpoint = DEFAULT_ENDPOINT;
  if (argc > 1) {
    endpoint = argv[1];
  }

  printf("connecting to %s\n", endpoint);
  BayeuxClientOpts opts;
  bayeux_client_opts_defaults(&opts);

  BayeuxClient* client;
  client = bayeux_client_create(endpoint, &opts);

  s_ev_base = bayeux_client_get_event_base(client);
  s_ev_timer_ping = evtimer_new(s_ev_base, &on_timeout_send_ping, client);
  s_ev_timer_pong = evtimer_new(s_ev_base, &on_timeout_send_pong, client);

  bayeux_client_subscribe(client, "/ping", &ping, client);
  bayeux_client_subscribe(client, "/pong", &pong, client);
  send_ping(client);

  signal(SIGINT, &signal_handler);
  event_base_dispatch(s_ev_base);
  signal(SIGINT, SIG_DFL);

  event_free(s_ev_timer_pong);
  event_free(s_ev_timer_ping);

  bayeux_client_destroy(client);

  return 0;
}
