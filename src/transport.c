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
#include "json_util.h"
#include "log.h"
#include "str.h"
#include "transport_http_long_polling.h"
#include <event2/event.h>
#include <json/json.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>


//
// types
//

struct Transport {
  ETransportType              type;
  union {
    TransportHttpLongPolling  long_polling;
  } impl;
};


//
// functions
//

//------------------------------------------------------------------------------
Transport* transport_open(const char* restrict uri,
                          ETransportType type,
                          struct event_base* restrict ev_base,
                          TransportRecvCallback callback,
                          void* callback_context,
                          int log_messages) {
  Transport* transport = (Transport*)malloc(sizeof(Transport));
  transport->type = type;

  switch (type) {
    case TRANSPORT_TYPE_HTTP_LONG_POLLING:
      transport_http_long_polling_create(&transport->impl.long_polling, ev_base, "http://localhost:5000/faye", callback, callback_context, log_messages);
      break;

    default:
      log_info("invalid transport type: %d", type);
      free(transport);
  }

  return transport;
}

//------------------------------------------------------------------------------
void transport_close(Transport* transport) {
  switch (transport->type) {
    case TRANSPORT_TYPE_HTTP_LONG_POLLING:
      transport_http_long_polling_destroy(&transport->impl.long_polling);
      break;

    default:
      log_info("invalid transport type: %d", transport->type);
      break;
  }
  free(transport);
}

//------------------------------------------------------------------------------
void transport_send(Transport* restrict transport, const char* restrict msg) {
  switch (transport->type) {
    case TRANSPORT_TYPE_HTTP_LONG_POLLING:
      transport_http_long_polling_request(&transport->impl.long_polling, msg);
      break;
      
    default:
      log_info("invalid transport type: %d", transport->type);
      break;
  }
}
