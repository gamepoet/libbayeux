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

#pragma once
#ifndef BAYEUX_H
#define BAYEUX_H

#ifdef __cplusplus
extern "C" {
#endif

// libevent2
struct event_base;


typedef struct BayeuxClient BayeuxClient;

typedef struct BayeuxClientOpts {
  int                 interval_ms;  /* (milliseconds) interval at which the client will try to reconnect to the server when the connection is lost */
  int                 timeout_ms;   /* (milliseconds) if the given amount of time has passed without data from the server, drop the connection and try to reconnect */
  struct event_base*  ev_base;      /* libevent2 event base for the client to use. if NULL, the client will make its own */
  int                 log_messages; /* (bool) log messages to stdout */
} BayeuxClientOpts;

typedef void (*BayeuxEventCallback)(const char* message, void* context);


/* fills the given opts with default values */
void bayeux_client_opts_defaults(BayeuxClientOpts* opts);

/* creates a client for the given endpoint and begins establishing a connection */
BayeuxClient* bayeux_client_create(const char* endpoint, const BayeuxClientOpts* opts);

/* destroys the given client */
void bayeux_client_destroy(BayeuxClient* client);

/* subscribes to the given channel */
void bayeux_client_subscribe(BayeuxClient* client, const char* channel, BayeuxEventCallback callback, void* context);

/* unsubscribes from the given channel */
void bayeux_client_unsubscribe(BayeuxClient* client, const char* channel, BayeuxEventCallback callback, void* context);

/* publishes data on the given channel */
void bayeux_client_publish(BayeuxClient* client, const char* channel, const char* data);

/* gets the libevent2 event_base used by the client. */
struct event_base* bayeux_client_get_event_base(BayeuxClient* client);

#ifdef __cplusplus
}
#endif

#endif /* BAYEUX_H */
