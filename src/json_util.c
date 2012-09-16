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

#include <json/json.h>
#include <stdbool.h>


//
// functions
//

//-----------------------------------------------------------------------------
bool json_get_bool(bool* restrict val, json_object* restrict json, const char* restrict key) {
  json_object* json_val = json_object_object_get(json, key);
  if (!json_val) {
    return false;
  }
  if (!json_object_is_type(json_val, json_type_boolean)) {
    return false;
  }
  *val = json_object_get_boolean(json_val);
  return true;
}

//-----------------------------------------------------------------------------
bool json_get_int(int* restrict val, json_object* restrict json, const char* restrict key) {
  json_object* json_val = json_object_object_get(json, key);
  if (!json_val) {
    return false;
  }
  if (!json_object_is_type(json_val, json_type_int)) {
    return false;
  }
  *val = json_object_get_int(json_val);
  return true;
}

//-----------------------------------------------------------------------------
bool json_get_string(const char** restrict val, json_object* restrict json, const char* restrict key) {
  json_object* json_val = json_object_object_get(json, key);
  if (!json_val) {
    return false;
  }
  if (!json_object_is_type(json_val, json_type_string)) {
    return false;
  }
  *val = json_object_get_string(json_val);
  return true;
}

//------------------------------------------------------------------------------
void json_set_object(json_object* restrict obj, const char* restrict key, json_object* restrict value) {
  json_object_object_add(obj, key, value);
}

//------------------------------------------------------------------------------
void json_set_string(json_object* restrict obj, const char* restrict key, const char* restrict value) {
  json_object_object_add(obj, key, json_object_new_string(value));
}
