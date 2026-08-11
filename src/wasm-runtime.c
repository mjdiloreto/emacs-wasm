/* Private emacswasm runtime bridge.

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

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>
#include <emscripten/em_asm.h>

#include "lisp.h"

static char *launch_mode;
static char *launch_profile;
static _Atomic bool stop_requested;
static bool dispatch_failed;

static bool
identifier_char_p (char c, bool first)
{
  return ((c >= 'a' && c <= 'z')
          || (c >= '0' && c <= '9')
          || (!first && c == '-'));
}

static bool
identifier_p (const char *value)
{
  if (!value || !*value || *value == '-')
    return false;
  bool after_hyphen = false;
  for (const char *p = value; *p; p++)
    {
      if (!identifier_char_p (*p, p == value) || (after_hyphen && *p == '-'))
        return false;
      after_hyphen = *p == '-';
    }
  return !after_hyphen;
}

static const char *
option_value (int argc, char **argv, int *index, const char *name)
{
  size_t length = strlen (name);
  const char *argument = argv[*index];
  if (!strcmp (argument, name))
    {
      if (*index + 1 >= argc)
        fatal ("Option '%s' requires an argument", name);
      *index += 1;
      return argv[*index];
    }
  if (!strncmp (argument, name, length) && argument[length] == '=')
    return argument + length + 1;
  return NULL;
}

void
emacswasm_init_launch (int argc, char **argv, bool required)
{
  const char *mode = NULL;
  const char *profile = NULL;
  int mode_count = 0;
  int profile_count = 0;
  for (int i = 1; i < argc; i++)
    {
      const char *value = option_value (argc, argv, &i, "--emacswasm-mode");
      if (value)
        {
          mode = value;
          mode_count++;
          continue;
        }
      value = option_value (argc, argv, &i, "--emacswasm-profile");
      if (value)
        {
          profile = value;
          profile_count++;
        }
    }

  if (!required && mode_count == 0 && profile_count == 0)
    return;
  if (mode_count != 1 || profile_count != 1)
    fatal ("emacswasm requires exactly one --emacswasm-mode and one --emacswasm-profile");
  if (strcmp (mode, "interactive") && strcmp (mode, "headless")
      && strcmp (mode, "batch"))
    fatal ("Invalid emacswasm mode '%s'", mode);
  if (!identifier_p (profile))
    fatal ("Invalid emacswasm profile '%s'", profile);

  xfree (launch_mode);
  xfree (launch_profile);
  launch_mode = xstrdup (mode);
  launch_profile = xstrdup (profile);
  atomic_store (&stop_requested, false);
}

bool
emacswasm_launch_p (void)
{
  return launch_mode && launch_profile;
}

bool
emacswasm_headless_p (void)
{
  return launch_mode && !strcmp (launch_mode, "headless");
}

void
emacswasm_platform_bootstrap (void)
{
  if (!emacswasm_launch_p ())
    return;
  Lisp_Object load = intern_c_string ("load");
  Lisp_Object bootstrap = intern_c_string ("emacswasm-platform-bootstrap");
  calln (load, build_string ("/usr/share/emacswasm/platform/emacswasm.el"),
         Qnil, Qnil, Qt, Qt);
  call0 (bootstrap);
}

void
emacswasm_report_exit (int code)
{
  if (!emacswasm_launch_p ())
    return;
  /* Interactive Emacs runs behind PROXY_TO_PTHREAD.  Marshal callbacks to
     the loader realm, whose Module owns the private runtime hooks.  In the
     portable engine that realm is already the current Worker or Node realm.  */
  MAIN_THREAD_EM_ASM ({
    var callback = Module['emacswasmExit'];
    if (typeof callback === 'function') callback($0);
  }, code);
}

DEFUN ("emacswasm-launch-facts", Femacswasm_launch_facts,
       Semacswasm_launch_facts, 0, 0, 0,
       doc: /* Return the private runtime launch mode and profile.
The result is a two-element list of strings.  This primitive is private to
the co-shipped emacswasm platform Lisp.  */)
  (void)
{
  if (!emacswasm_launch_p ())
    return Qnil;
  return list2 (build_string (launch_mode), build_string (launch_profile));
}

DEFUN ("emacswasm-ready", Femacswasm_ready, Semacswasm_ready, 0, 0, 0,
       doc: /* Notify the private runtime that platform and profile startup are ready.  */)
  (void)
{
  MAIN_THREAD_EM_ASM ({
    var callback = Module['emacswasmReady'];
    if (typeof callback === 'function') callback();
  });
  return Qt;
}

DEFUN ("emacswasm-stop-requested-p", Femacswasm_stop_requested_p,
       Semacswasm_stop_requested_p, 0, 0, 0,
       doc: /* Return non-nil when the private host requested orderly shutdown.  */)
  (void)
{
  return atomic_load (&stop_requested) ? Qt : Qnil;
}

DEFUN ("emacswasm-register-dispatcher", Femacswasm_register_dispatcher,
       Semacswasm_register_dispatcher, 1, 1, 0,
       doc: /* Register FUNCTION as the serial headless byte dispatcher.  */)
  (Lisp_Object function)
{
  if (!FUNCTIONP (function))
    wrong_type_argument (Qfunctionp, function);
  Vemacswasm_dispatcher = function;
  return function;
}

static Lisp_Object
dispatch_call (void *argument)
{
  Lisp_Object input = *(Lisp_Object *) argument;
  return calln (Vemacswasm_dispatcher, input);
}

static Lisp_Object
dispatch_handler (enum nonlocal_exit type, Lisp_Object value)
{
  (void) type;
  dispatch_failed = true;
  return value;
}

EMSCRIPTEN_KEEPALIVE int
emacswasm_dispatch (const uint8_t *input, size_t input_length,
                    uintptr_t *output_pointer, uint32_t *output_length)
{
  if (!emacswasm_headless_p () || NILP (Vemacswasm_dispatcher)
      || !output_pointer || !output_length || input_length > PTRDIFF_MAX)
    return 1;
  Lisp_Object argument = make_unibyte_string ((const char *) input,
                                               input_length);
  dispatch_failed = false;
  Lisp_Object response = internal_catch_all (dispatch_call, &argument,
                                             dispatch_handler);
  if (dispatch_failed)
    return 2;
  if (!STRINGP (response) || STRING_MULTIBYTE (response)
      || SBYTES (response) > UINT32_MAX)
    return 3;
  size_t length = SBYTES (response);
  void *copy = malloc (length ? length : 1);
  if (!copy)
    return 4;
  if (length)
    memcpy (copy, SDATA (response), length);
  *output_pointer = (uintptr_t) copy;
  *output_length = length;
  return 0;
}

EMSCRIPTEN_KEEPALIVE void
emacswasm_free_response (uintptr_t pointer)
{
  free ((void *) pointer);
}

EMSCRIPTEN_KEEPALIVE void
emacswasm_request_stop (void)
{
  atomic_store (&stop_requested, true);
  /* A portable headless instance has returned from main and has no Emacs
     command loop to poll the flag.  Calls arrive serially in that same
     Worker realm, so complete the normal kill-emacs path synchronously.  */
  if (initialized && emacswasm_headless_p ())
    Fkill_emacs (make_fixnum (0), Qnil);
}

void
syms_of_wasm_runtime (void)
{
  defsubr (&Semacswasm_launch_facts);
  defsubr (&Semacswasm_ready);
  defsubr (&Semacswasm_stop_requested_p);
  defsubr (&Semacswasm_register_dispatcher);
  DEFVAR_LISP ("emacswasm--dispatcher", Vemacswasm_dispatcher,
               doc: /* Private registered headless byte dispatcher.  */);
  Vemacswasm_dispatcher = Qnil;
}

#endif /* __EMSCRIPTEN__ */
