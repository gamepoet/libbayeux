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

#include "str.h"
#include <stdlib.h>
#include <string.h>


//
// macros
//

#define STR_HDR(s)  ((StrHdr *)(s - sizeof(StrHdr)))


//
// types
//

typedef struct StrHdr {
  unsigned int  capacity;
  unsigned int  len;
  char          buf[];
} StrHdr;


//
// local functions
//

//------------------------------------------------------------------------------
static Str* str_expect(Str* restrict s, unsigned int len) {
  StrHdr* hdr = STR_HDR(s);

  unsigned int need = hdr->len + len + 1;
  if (need < hdr->capacity) {
    return s;
  }

  // small strings just double the size; large strings align(2048)
  if (need < 2048) {
    need = need * 2;
  }
  else {
    need = (need + 2047) & ~2047;
  }

  hdr = (StrHdr*)realloc(hdr, sizeof(StrHdr) + need);
  hdr->capacity = need;
  return hdr->buf;
}


//
// functions
//

//------------------------------------------------------------------------------
Str* str_alloc(unsigned int capacity) {
  capacity += 1;
  StrHdr* hdr = (StrHdr*)malloc(sizeof(StrHdr) + capacity);
  if (hdr) {
    hdr->len      = 0;
    hdr->capacity = capacity;
    hdr->buf[0]   = 0;
  }
  return hdr->buf;
}

//------------------------------------------------------------------------------
Str* str_cat(Str* restrict s, const char* restrict src) {
  unsigned int src_len = (unsigned int)strlen(src);
  s = str_expect(s, src_len);
  if (s) {
    StrHdr* restrict hdr = STR_HDR(s);
    memcpy(s + hdr->len, src, src_len);
    hdr->len += src_len;
    s[hdr->len] = 0;
  }
  return s;
}

//------------------------------------------------------------------------------
void str_clear(Str* s) {
  StrHdr* restrict hdr = STR_HDR(s);
  hdr->len = 0;
  hdr->buf[0] = 0;
}

//------------------------------------------------------------------------------
void str_free(Str* s) {
  if (s) {
    StrHdr* hdr = STR_HDR(s);
    free(hdr);
  }
}

//------------------------------------------------------------------------------
unsigned int str_len(Str* s) {
  StrHdr* restrict hdr = STR_HDR(s);
  return hdr->len;
}

//------------------------------------------------------------------------------
Str* str_reserve(Str* s, size_t capacity) {
  StrHdr* hdr = STR_HDR(s);
  if (hdr->capacity < capacity) {
    s = str_expect(s, (unsigned int)(capacity - hdr->capacity));
  }
  return s;
}

