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

#pragma once

struct curl_slist;
struct event;
struct event_base;


typedef struct TransportHttpLongPolling {
  void* restrict                curl_multi;
  struct curl_slist* restrict   http_headers;
  struct event_base* restrict   ev_base;
  struct event* restrict        ev_timer;
  int                           active_conn_count;
  char* restrict                url;
  TransportRecvCallback         recv_callback;
  void*                         recv_callback_ctx;
  int                           log_messages;
} TransportHttpLongPolling;


void transport_http_long_polling_create(TransportHttpLongPolling* restrict transport, struct event_base* restrict ev_base, const char* restrict url, TransportRecvCallback callback, void* callback_ctx, int log_messages);
void transport_http_long_polling_destroy(TransportHttpLongPolling* transport);
void transport_http_long_polling_request(TransportHttpLongPolling* restrict transport, const char* restrict msg);
