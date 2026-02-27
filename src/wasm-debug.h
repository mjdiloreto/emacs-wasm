/* wasm-debug.h — WASM debug tracing macros.

Copyright (C) 2025 Free Software Foundation, Inc.

This file is part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.  */

/* Define WASM_DEBUG at compile time to enable trace output
   (e.g., CFLAGS="-DWASM_DEBUG").  When disabled, all trace
   macros compile to (void) 0 with zero runtime cost.  */

#ifndef WASM_DEBUG_H
#define WASM_DEBUG_H

#ifdef __EMSCRIPTEN__
# ifdef WASM_DEBUG
extern void emscripten_console_error (const char *);
#  define WASM_TRACE(msg) emscripten_console_error (msg)
#  define WASM_TRACE_FMT(buf, sz, fmt, ...) \
     do { snprintf (buf, sz, fmt, __VA_ARGS__);	\
          emscripten_console_error (buf); } while (0)
# else
#  define WASM_TRACE(msg) ((void) 0)
#  define WASM_TRACE_FMT(buf, sz, fmt, ...) ((void) 0)
# endif
#else
# define WASM_TRACE(msg) ((void) 0)
# define WASM_TRACE_FMT(buf, sz, fmt, ...) ((void) 0)
#endif

#endif /* WASM_DEBUG_H */
