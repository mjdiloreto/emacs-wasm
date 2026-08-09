/* Mediated synchronous fetch support for the Emscripten build.

Copyright (C) 2026 Free Software Foundation, Inc.

This file is part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.  */

#include <config.h>

#ifdef __EMSCRIPTEN__

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten/em_asm.h>
#include <emscripten/fetch.h>

#include "lisp.h"
#include "coding.h"

enum
  {
    WASM_FETCH_MAX_METHOD_BYTES = 31,
    WASM_FETCH_MAX_REQUEST_BYTES = 8 * 1024 * 1024
  };

static AVOID
wasm_fetch_signal (Lisp_Object condition, const char *message)
{
  xsignal1 (condition, build_string (message));
}

static bool
wasm_fetch_has_nul (Lisp_Object string)
{
  return memchr (SDATA (string), '\0', SBYTES (string)) != NULL;
}

static bool
wasm_fetch_header_name_p (Lisp_Object name)
{
  if (SBYTES (name) == 0 || wasm_fetch_has_nul (name))
    return false;

  for (ptrdiff_t i = 0; i < SBYTES (name); i++)
    {
      unsigned char c = SDATA (name)[i];
      if (!(isalnum (c)
            || c == '!' || c == '#' || c == '$' || c == '%'
            || c == '&' || c == '\'' || c == '*' || c == '+'
            || c == '-' || c == '.' || c == '^' || c == '_'
            || c == '`' || c == '|' || c == '~'))
        return false;
    }
  return true;
}

static bool
wasm_fetch_header_value_p (Lisp_Object value)
{
  if (wasm_fetch_has_nul (value))
    return false;
  for (ptrdiff_t i = 0; i < SBYTES (value); i++)
    if (SDATA (value)[i] == '\r' || SDATA (value)[i] == '\n')
      return false;
  return true;
}

static bool
wasm_fetch_method_p (Lisp_Object method)
{
  ptrdiff_t bytes = SBYTES (method);
  if (bytes == 0 || bytes > WASM_FETCH_MAX_METHOD_BYTES
      || wasm_fetch_has_nul (method))
    return false;
  for (ptrdiff_t i = 0; i < bytes; i++)
    {
      unsigned char c = SDATA (method)[i];
      if (!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z'))
        return false;
    }
  return true;
}

static void
wasm_fetch_close_unwind (void *response)
{
  emscripten_fetch_close (response);
}

static void
wasm_fetch_free_headers_unwind (void *headers)
{
  emscripten_fetch_free_unpacked_response_headers (headers);
}

static Lisp_Object
wasm_fetch_make_header_value (const char *value)
{
  const char *begin = value;
  while (*begin == ' ' || *begin == '\t')
    begin++;
  const char *end = begin + strlen (begin);
  while (end > begin
         && (end[-1] == ' ' || end[-1] == '\t'
             || end[-1] == '\r' || end[-1] == '\n'))
    end--;
  return make_unibyte_string (begin, end - begin);
}

DEFUN ("wasm-fetch-url", Fwasm_fetch_url, Swasm_fetch_url, 1, 4, 0,
       doc: /* Fetch URL synchronously through the host-mediated WASM policy.
METHOD defaults to "GET".  HEADERS is an alist of string pairs, and BODY is
nil or a unibyte string.  Return a plist with keys `:status', `:url',
`:headers', and `:body'.  HTTP error statuses are returned normally.

Signal `wasm-fetch-denied' when host policy rejects the request,
`wasm-fetch-unavailable' when this runtime has no browser XHR transport, and
`wasm-fetch-error' for malformed input, transport, or response failures.  */)
  (Lisp_Object url, Lisp_Object method, Lisp_Object headers, Lisp_Object body)
{
  specpdl_ref count = SPECPDL_INDEX ();
  USE_SAFE_ALLOCA;
  const char **request_headers;
  ptrdiff_t header_count = 0;

  if (!STRINGP (url) || wasm_fetch_has_nul (url))
    wasm_fetch_signal (Qwasm_fetch_error,
                       "Fetch URL must be a string without embedded NULs");

  if (NILP (method))
    method = build_string ("GET");
  if (!STRINGP (method) || !wasm_fetch_method_p (method))
    wasm_fetch_signal (Qwasm_fetch_error,
                       "Fetch method must be a short ASCII alphabetic string");

  if (!(NILP (body) || STRINGP (body)))
    wasm_fetch_signal (Qwasm_fetch_error,
                       "Fetch body must be nil or a unibyte string");
  if (STRINGP (body) && STRING_MULTIBYTE (body))
    wasm_fetch_signal (Qwasm_fetch_error,
                       "Fetch body must be a unibyte string");
  if (STRINGP (body) && SBYTES (body) > WASM_FETCH_MAX_REQUEST_BYTES)
    wasm_fetch_signal (Qwasm_fetch_error, "Fetch request body is too large");

  Lisp_Object tail = headers;
  while (CONSP (tail))
    {
      Lisp_Object pair = XCAR (tail);
      if (!CONSP (pair) || !STRINGP (XCAR (pair))
          || !STRINGP (XCDR (pair)))
        wasm_fetch_signal (Qwasm_fetch_error,
                           "Fetch headers must be an alist of string pairs");
      if (!wasm_fetch_header_name_p (XCAR (pair)))
        wasm_fetch_signal (Qwasm_fetch_error, "Malformed fetch header name");
      if (!wasm_fetch_header_value_p (XCDR (pair)))
        wasm_fetch_signal (Qwasm_fetch_error, "Malformed fetch header value");
      if (header_count == PTRDIFF_MAX)
        memory_full (SIZE_MAX);
      header_count++;
      tail = XCDR (tail);
    }
  if (!NILP (tail))
    wasm_fetch_signal (Qwasm_fetch_error, "Fetch headers must be a proper list");

  if (header_count > (PTRDIFF_MAX - 1) / 2)
    memory_full (SIZE_MAX);
  SAFE_NALLOCA (request_headers, 1, 2 * header_count + 1);

  ptrdiff_t header_index = 0;
  for (tail = headers; CONSP (tail); tail = XCDR (tail))
    {
      Lisp_Object pair = XCAR (tail);
      Lisp_Object name = ENCODE_UTF_8 (XCAR (pair));
      Lisp_Object value = ENCODE_UTF_8 (XCDR (pair));
      char *name_copy;
      char *value_copy;
      SAFE_ALLOCA_STRING (name_copy, name);
      SAFE_ALLOCA_STRING (value_copy, value);
      request_headers[header_index++] = name_copy;
      request_headers[header_index++] = value_copy;
    }
  request_headers[header_index] = NULL;

  Lisp_Object url_utf8 = ENCODE_UTF_8 (url);
  Lisp_Object method_utf8 = ENCODE_UTF_8 (method);
  char *url_copy;
  char *method_copy;
  SAFE_ALLOCA_STRING (url_copy, url_utf8);
  SAFE_ALLOCA_STRING (method_copy, method_utf8);
  for (char *p = method_copy; *p; p++)
    *p = toupper ((unsigned char) *p);

  int transport_available = EM_ASM_INT ({
    return typeof XMLHttpRequest === 'function' ? 1 : 0;
  });
  if (!transport_available)
    wasm_fetch_signal (Qwasm_fetch_unavailable,
                       "Browser XMLHttpRequest transport is unavailable");

  uintptr_t normalized_address = MAIN_THREAD_EM_ASM_INT ({
    try {
      var authorize = Module['fetchPolicyAuthorize'];
      if (typeof authorize !== 'function')
        return 0;
      var pairs = [];
      for (var i = 0; i < $3; i++) {
        var offset = ($2 >> 2) + 2 * i;
        pairs.push([UTF8ToString(HEAPU32[offset]),
                    UTF8ToString(HEAPU32[offset + 1])]);
      }
      var decision = authorize({
        url: UTF8ToString($0),
        method: UTF8ToString($1),
        headers: pairs,
        bodyBytes: $4
      });
      if (!decision || decision.allowed !== true
          || typeof decision.url !== 'string')
        return 0;
      var bytes = lengthBytesUTF8(decision.url) + 1;
      var result = _malloc(bytes);
      if (!result)
        return 0;
      stringToUTF8(decision.url, result, bytes);
      return result;
    } catch (error) {
      console.error('[emacs-wasm] fetch policy error:', error);
      return 0;
    }
  }, url_copy, method_copy, request_headers, (int) header_count,
     STRINGP (body) ? (int) SBYTES (body) : 0);
  if (normalized_address == 0)
    wasm_fetch_signal (Qwasm_fetch_denied, "Fetch request denied by host policy");

  char *normalized_url = (char *) normalized_address;
  record_unwind_protect_ptr (free, normalized_url);

  emscripten_fetch_attr_t attr;
  emscripten_fetch_attr_init (&attr);
  strcpy (attr.requestMethod, method_copy);
  attr.attributes = (EMSCRIPTEN_FETCH_LOAD_TO_MEMORY
                     | EMSCRIPTEN_FETCH_SYNCHRONOUS);
  attr.withCredentials = false;
  attr.requestHeaders = request_headers;
  if (STRINGP (body))
    {
      attr.requestData = SSDATA (body);
      attr.requestDataSize = SBYTES (body);
    }

  emscripten_fetch_t *response = emscripten_fetch (&attr, normalized_url);
  if (response == NULL)
    wasm_fetch_signal (Qwasm_fetch_error, "Fetch transport returned no response");
  record_unwind_protect_ptr (wasm_fetch_close_unwind, response);

  if (response->readyState != 4 || response->status == 0)
    wasm_fetch_signal (Qwasm_fetch_error, "Fetch transport failed");
  if (response->responseUrl == NULL)
    wasm_fetch_signal (Qwasm_fetch_error, "Fetch response has no final URL");
  if (response->numBytes > PTRDIFF_MAX
      || (response->numBytes > 0 && response->data == NULL))
    wasm_fetch_signal (Qwasm_fetch_error, "Malformed fetch response body");
  if (response->numBytes > UINT32_MAX)
    wasm_fetch_signal (Qwasm_fetch_error, "Fetch response is too large");

  int response_decision = MAIN_THREAD_EM_ASM_INT ({
    try {
      var validate = Module['fetchPolicyValidateResponse'];
      if (typeof validate !== 'function')
        return -1;
      var decision = validate(UTF8ToString($0), $1);
      if (decision && decision.allowed === true)
        return 1;
      return decision && decision.reason === 'response-too-large' ? -2 : -1;
    } catch (error) {
      console.error('[emacs-wasm] fetch response policy error:', error);
      return -1;
    }
  }, response->responseUrl, (uint32_t) response->numBytes);
  if (response_decision == -2)
    wasm_fetch_signal (Qwasm_fetch_error, "Fetch response is too large");
  if (response_decision != 1)
    wasm_fetch_signal (Qwasm_fetch_denied,
                       "Fetch response denied by host policy");

  Lisp_Object response_headers = Qnil;
  size_t raw_headers_length
    = emscripten_fetch_get_response_headers_length (response);
  if (raw_headers_length > 0)
    {
      if (raw_headers_length == SIZE_MAX)
        memory_full (SIZE_MAX);
      char *raw_headers = SAFE_ALLOCA (raw_headers_length + 1);
      emscripten_fetch_get_response_headers (response, raw_headers,
                                             raw_headers_length + 1);
      char **unpacked = emscripten_fetch_unpack_response_headers (raw_headers);
      if (unpacked == NULL)
        wasm_fetch_signal (Qwasm_fetch_error,
                           "Could not unpack fetch response headers");
      record_unwind_protect_ptr (wasm_fetch_free_headers_unwind, unpacked);
      ptrdiff_t pairs = 0;
      while (unpacked[2 * pairs] != NULL)
        {
          if (unpacked[2 * pairs + 1] == NULL)
            wasm_fetch_signal (Qwasm_fetch_error,
                               "Malformed fetch response headers");
          pairs++;
        }
      for (ptrdiff_t i = pairs; i > 0; i--)
        response_headers
          = Fcons (Fcons (build_unibyte_string (unpacked[2 * (i - 1)]),
                          wasm_fetch_make_header_value
                            (unpacked[2 * (i - 1) + 1])),
                   response_headers);
    }

  Lisp_Object response_body
    = response->numBytes == 0
      ? empty_unibyte_string
      : make_unibyte_string (response->data, response->numBytes);
  Lisp_Object result
    = listn (8,
             QCwasm_fetch_status, make_fixnum (response->status),
             QCwasm_fetch_url, build_unibyte_string (response->responseUrl),
             QCwasm_fetch_headers, response_headers,
             QCwasm_fetch_body, response_body);
  return SAFE_FREE_UNBIND_TO (count, result);
}

void
syms_of_wasm_fetch (void)
{
  DEFSYM (Qwasm_fetch_error, "wasm-fetch-error");
  DEFSYM (Qwasm_fetch_denied, "wasm-fetch-denied");
  DEFSYM (Qwasm_fetch_unavailable, "wasm-fetch-unavailable");
  define_error (Qwasm_fetch_error, "WASM fetch error", Qerror);
  define_error (Qwasm_fetch_denied, "WASM fetch denied", Qwasm_fetch_error);
  define_error (Qwasm_fetch_unavailable, "WASM fetch unavailable",
                Qwasm_fetch_error);

  DEFSYM (QCwasm_fetch_status, ":status");
  DEFSYM (QCwasm_fetch_url, ":url");
  DEFSYM (QCwasm_fetch_headers, ":headers");
  DEFSYM (QCwasm_fetch_body, ":body");

  defsubr (&Swasm_fetch_url);
}

#endif /* __EMSCRIPTEN__ */
