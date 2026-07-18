/* WASM syscall stubs for Emacs.

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

/* This file provides stub implementations of system calls that are
   impossible in the WebAssembly/browser environment. These stubs
   return appropriate error codes (ENOSYS) or no-op where safe.

   For full subprocess functionality, connect to a remote backend
   server via WebSocket that can execute processes on a real system.  */

#ifdef __EMSCRIPTEN__

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <netdb.h>
#include <emscripten/stack.h>

/* Define WASM_STUB_VERBOSE to enable console warnings when stubs are called */
#ifdef WASM_STUB_VERBOSE
#include <emscripten.h>
#define WASM_STUB_WARN(name) \
  EM_ASM({ console.warn('WASM stub called: ' + UTF8ToString($0)); }, name)
#else
#define WASM_STUB_WARN(name) ((void)0)
#endif

/* ============================================================
   Process Management Stubs

   These operations are fundamentally impossible in WASM without
   a remote backend. They return ENOSYS (Function not implemented).
   ============================================================ */

/* Fork is impossible in single-threaded browser environment.
   Returns -1 with ENOSYS.  */
pid_t
wasm_stub_fork (void)
{
  WASM_STUB_WARN("fork");
  errno = ENOSYS;
  return -1;
}

/* vfork is also impossible.  */
pid_t
wasm_stub_vfork (void)
{
  WASM_STUB_WARN("vfork");
  errno = ENOSYS;
  return -1;
}

/* execve cannot replace current process in WASM.  */
int
wasm_stub_execve (const char *path, char *const argv[], char *const envp[])
{
  WASM_STUB_WARN("execve");
  errno = ENOSYS;
  return -1;
}

/* execvp wrapper.  */
int
wasm_stub_execvp (const char *file, char *const argv[])
{
  WASM_STUB_WARN("execvp");
  errno = ENOSYS;
  return -1;
}

/* waitpid - no child processes to wait for.  */
pid_t
wasm_stub_waitpid (pid_t pid, int *status, int options)
{
  WASM_STUB_WARN("waitpid");
  errno = ECHILD;  /* No child processes */
  return -1;
}

/* wait - no child processes.  */
pid_t
wasm_stub_wait (int *status)
{
  WASM_STUB_WARN("wait");
  errno = ECHILD;
  return -1;
}

/* kill - cannot send signals to other processes.  */
int
wasm_stub_kill (pid_t pid, int sig)
{
  WASM_STUB_WARN("kill");

  /* Allow sending signals to self (our own PID) for compatibility.
     This enables C-g interrupt handling within Emacs.  */
  if (pid == getpid () || pid == 0)
    {
      /* Raise signal in current process.  */
      return raise (sig);
    }

  errno = ESRCH;  /* No such process */
  return -1;
}

/* killpg - cannot send signals to process groups.  */
int
wasm_stub_killpg (pid_t pgrp, int sig)
{
  WASM_STUB_WARN("killpg");
  errno = ESRCH;
  return -1;
}

/* ============================================================
   Signal Handling Stubs

   WASM has limited signal support. We provide minimal stubs
   that track signal masks but don't actually deliver signals.
   ============================================================ */

/* Global signal mask (for tracking purposes only).  */
static sigset_t wasm_signal_mask;
static int wasm_signal_mask_initialized = 0;

static void
ensure_signal_mask_init (void)
{
  if (!wasm_signal_mask_initialized)
    {
      sigemptyset (&wasm_signal_mask);
      wasm_signal_mask_initialized = 1;
    }
}

/* sigprocmask - track signal mask but don't actually block.  */
int
wasm_stub_sigprocmask (int how, const sigset_t *set, sigset_t *oldset)
{
  ensure_signal_mask_init ();

  if (oldset)
    *oldset = wasm_signal_mask;

  if (set)
    {
      switch (how)
        {
        case SIG_BLOCK:
          /* Add signals from set to mask */
          for (int i = 1; i < NSIG; i++)
            if (sigismember (set, i))
              sigaddset (&wasm_signal_mask, i);
          break;
        case SIG_UNBLOCK:
          /* Remove signals from set from mask */
          for (int i = 1; i < NSIG; i++)
            if (sigismember (set, i))
              sigdelset (&wasm_signal_mask, i);
          break;
        case SIG_SETMASK:
          wasm_signal_mask = *set;
          break;
        default:
          errno = EINVAL;
          return -1;
        }
    }

  return 0;
}

/* sigsuspend - cannot actually suspend, just return immediately.  */
int
wasm_stub_sigsuspend (const sigset_t *mask)
{
  WASM_STUB_WARN("sigsuspend");
  errno = EINTR;  /* Pretend we were interrupted */
  return -1;
}

/* Direct alias for linker resolution (pthreads libc doesn't provide this).  */
int
sigsuspend (const sigset_t *mask)
{
  return wasm_stub_sigsuspend (mask);
}

/* sigaction - store handlers but delivery is limited.  */
static struct sigaction wasm_signal_handlers[NSIG];

int
wasm_stub_sigaction (int signum, const struct sigaction *act,
                     struct sigaction *oldact)
{
  if (signum < 0 || signum >= NSIG)
    {
      errno = EINVAL;
      return -1;
    }

  if (oldact)
    *oldact = wasm_signal_handlers[signum];

  if (act)
    wasm_signal_handlers[signum] = *act;

  return 0;
}

/* ============================================================
   PTY/TTY Stubs

   Terminal operations are impossible without a real terminal.
   In WASM, we use xterm.js or similar for terminal emulation.
   ============================================================ */

/* ioctl - limited support for terminal operations.
   Note: This is a simplified stub. Full ioctl would need more cases.  */

/* Define window size struct if not available.  */
#ifndef TIOCGWINSZ
struct winsize {
  unsigned short ws_row;
  unsigned short ws_col;
  unsigned short ws_xpixel;
  unsigned short ws_ypixel;
};
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#endif

/* tcgetattr - get terminal attributes.  */
int
wasm_stub_tcgetattr (int fd, struct termios *termios_p)
{
  WASM_STUB_WARN("tcgetattr");

  if (termios_p)
    {
      /* Return default terminal settings.  */
      memset (termios_p, 0, sizeof (*termios_p));
      termios_p->c_iflag = ICRNL | IXON;
      termios_p->c_oflag = OPOST | ONLCR;
      termios_p->c_cflag = CS8 | CREAD;
      termios_p->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE;
    }

  return 0;
}

/* tcsetattr - set terminal attributes (no-op in WASM).  */
int
wasm_stub_tcsetattr (int fd, int optional_actions, const struct termios *termios_p)
{
  WASM_STUB_WARN("tcsetattr");
  /* Pretend success - xterm.js handles actual terminal behavior.  */
  return 0;
}

/* tcdrain - drain output (no-op).  */
int
wasm_stub_tcdrain (int fd)
{
  return 0;
}

/* tcflush - flush I/O (no-op).  */
int
wasm_stub_tcflush (int fd, int queue_selector)
{
  return 0;
}

/* openpty - pseudo-terminal allocation is impossible in WASM.
   Remote backend provides PTY functionality via WebSocket.  */
int
wasm_stub_openpty (int *amaster, int *aslave, char *name,
                   const struct termios *termp,
                   const struct winsize *winp)
{
  WASM_STUB_WARN("openpty");
  errno = ENOSYS;
  return -1;
}

/* forkpty - combines fork and openpty, both impossible.  */
pid_t
wasm_stub_forkpty (int *amaster, char *name,
                   const struct termios *termp,
                   const struct winsize *winp)
{
  WASM_STUB_WARN("forkpty");
  errno = ENOSYS;
  return -1;
}

/* ============================================================
   File/Directory Stubs

   Most file operations work via Emscripten's virtual filesystem.
   These stubs handle operations that need special treatment.
   ============================================================ */

/* chown - ownership changes not supported in virtual FS.  */
int
wasm_stub_chown (const char *path, uid_t owner, gid_t group)
{
  WASM_STUB_WARN("chown");
  /* Pretend success - ownership is meaningless in browser.  */
  return 0;
}

/* fchown - file descriptor version.  */
int
wasm_stub_fchown (int fd, uid_t owner, gid_t group)
{
  WASM_STUB_WARN("fchown");
  return 0;
}

/* lchown - symbolic link version.  */
int
wasm_stub_lchown (const char *path, uid_t owner, gid_t group)
{
  WASM_STUB_WARN("lchown");
  return 0;
}

/* mknod - special files not supported.  */
int
wasm_stub_mknod (const char *path, mode_t mode, dev_t dev)
{
  WASM_STUB_WARN("mknod");
  errno = ENOSYS;
  return -1;
}

/* ============================================================
   User/Group Stubs

   Browser has no concept of users/groups. Return fake values.
   ============================================================ */

/* getuid - return fake UID 1000.  */
uid_t
wasm_stub_getuid (void)
{
  return 1000;
}

/* getgid - return fake GID 1000.  */
gid_t
wasm_stub_getgid (void)
{
  return 1000;
}

/* geteuid - return fake effective UID.  */
uid_t
wasm_stub_geteuid (void)
{
  return 1000;
}

/* getegid - return fake effective GID.  */
gid_t
wasm_stub_getegid (void)
{
  return 1000;
}

/* setuid - pretend success.  */
int
wasm_stub_setuid (uid_t uid)
{
  return 0;
}

/* setgid - pretend success.  */
int
wasm_stub_setgid (gid_t gid)
{
  return 0;
}

/* ============================================================
   Resource Limit Stubs
   ============================================================ */

/* getrlimit - return fake resource limits.  */
int
wasm_stub_getrlimit (int resource, struct rlimit *rlim)
{
  if (rlim)
    {
      rlim->rlim_cur = RLIM_INFINITY;
      rlim->rlim_max = RLIM_INFINITY;
    }
  return 0;
}

/* setrlimit - pretend success.  */
int
wasm_stub_setrlimit (int resource, const struct rlimit *rlim)
{
  return 0;
}

/* __syscall_prlimit64 - strong override of Emscripten's weak stub.

   Emscripten's musl routes getrlimit/setrlimit through SYS_prlimit64,
   whose default handler (system/lib/libc/emscripten_syscall_stubs.c)
   answers correctly but prints "warning: unsupported syscall:
   __syscall_prlimit64" on every call in the assertions-enabled libc.
   Emacs reads RLIMIT_STACK (emacs.c) and RLIMIT_NOFILE (process.c) at
   startup, so the warning appears on every boot.  Emscripten declares
   its stub weak, so this strong definition shadows it at link time;
   the behavior below mirrors the original — reads report the real
   wasm stack and Emscripten's fd table size, writes are refused.  */
int
__syscall_prlimit64 (int pid, int resource, intptr_t new_limit,
                     intptr_t old_limit)
{
  struct rlimit *old = (struct rlimit *) old_limit;
  if (new_limit)
    return -EPERM;
  if (old)
    {
      if (resource == RLIMIT_NOFILE)
        {
          /* See FS.MAX_OPEN_FDS in Emscripten's src/lib/libfs.js.  */
          old->rlim_cur = 4096;
          old->rlim_max = 4096;
        }
      else if (resource == RLIMIT_STACK)
        {
          /* The wasm stack grows down: base > end, size = base - end.  */
          uintptr_t end = emscripten_stack_get_end ();
          uintptr_t base = emscripten_stack_get_base ();
          old->rlim_cur = base - end;
          old->rlim_max = base - end;
        }
      else
        {
          old->rlim_cur = RLIM_INFINITY;
          old->rlim_max = RLIM_INFINITY;
        }
    }
  return 0;
}

/* ============================================================
   Networking Stubs (for non-HTTP operations)

   HTTP/WebSocket work via browser APIs.
   Raw sockets are impossible.
   ============================================================ */

/* gethostbyname - DNS is handled by browser fetch API.  */
struct hostent *
wasm_stub_gethostbyname (const char *name)
{
  WASM_STUB_WARN("gethostbyname");
  /* Return NULL - use browser's fetch/WebSocket for network.  */
  return NULL;
}

/* socket - raw sockets impossible in browser.  */
int
wasm_stub_socket (int domain, int type, int protocol)
{
  WASM_STUB_WARN("socket");
  /* Emscripten provides WebSocket support for SOCK_STREAM.
     For other types, return error.  */
  errno = ENOSYS;
  return -1;
}

/* ============================================================
   Symbol aliasing

   Map standard library calls to our stubs when compiled
   with Emscripten.
   ============================================================ */

/* Note: Emscripten provides some of these functions.
   Only define aliases for functions we need to override.
   Uncomment as needed based on build errors.  */

/*
#pragma weak fork = wasm_stub_fork
#pragma weak vfork = wasm_stub_vfork
#pragma weak execve = wasm_stub_execve
#pragma weak waitpid = wasm_stub_waitpid
#pragma weak kill = wasm_stub_kill
*/

/* ============================================================
   Termcap/Terminfo Stubs

   Emacs requires termcap functions for terminal handling.
   In WASM, xterm.js provides the terminal - we just need
   to satisfy the linker and provide sensible defaults.
   ============================================================ */

/* Global termcap variables that some code expects.  */
// char *wasm_BC = NULL;  /* Backspace character string */
// char *wasm_UP = NULL;  /* Cursor up string */
// char PC = 0;           /* Pad character */
// short ospeed = 0;      /* Output speed (baud rate) */

/* tputs - output a termcap string with padding.
   In WASM we just output the string directly.
   Note: Emacs declares tputs as returning void (see tparam.h),
   so we must match that signature exactly for WASM compatibility.  */
void
tputs (const char *str, int affcnt, int (*putc_func)(int))
{
  if (!str)
    return;

  while (*str)
    {
      if (putc_func)
        putc_func (*str);
      str++;
    }
}

/* tgoto - generate cursor motion string.
   Return a static buffer with the capability string.  */
char *
tgoto (const char *cap, int col, int row)
{
  static char result[64];

  /* ANSI escape sequence for cursor positioning.  */
  snprintf (result, sizeof(result), "\033[%d;%dH", row + 1, col + 1);
  return result;
}

/* tgetent - load terminal entry.
   Pretend we found a "dumb" terminal.  */
int
tgetent (char *bp, const char *name)
{
  /* Return 1 to indicate success.  */
  return 1;
}

/* tgetstr - get string capability.
   Return ANSI escape sequences for common capabilities.  */
char *
tgetstr (const char *id, char **area)
{
  static char buf[64];
  const char *result = NULL;

  if (!id)
    return NULL;

  /* Map common termcap capabilities to ANSI sequences.  */
  if (strcmp (id, "cm") == 0)        /* Cursor motion */
    result = "\033[%i%d;%dH";
  else if (strcmp (id, "cl") == 0)   /* Clear screen */
    result = "\033[H\033[2J";
  else if (strcmp (id, "ce") == 0)   /* Clear to end of line */
    result = "\033[K";
  else if (strcmp (id, "cd") == 0)   /* Clear to end of display */
    result = "\033[J";
  else if (strcmp (id, "so") == 0)   /* Standout begin */
    result = "\033[7m";
  else if (strcmp (id, "se") == 0)   /* Standout end */
    result = "\033[27m";
  else if (strcmp (id, "us") == 0)   /* Underline begin */
    result = "\033[4m";
  else if (strcmp (id, "ue") == 0)   /* Underline end */
    result = "\033[24m";
  else if (strcmp (id, "md") == 0)   /* Bold begin */
    result = "\033[1m";
  else if (strcmp (id, "me") == 0)   /* All attributes off */
    result = "\033[0m";
  else if (strcmp (id, "up") == 0)   /* Cursor up */
    result = "\033[A";
  else if (strcmp (id, "do") == 0)   /* Cursor down */
    result = "\033[B";
  else if (strcmp (id, "nd") == 0)   /* Cursor right (non-destructive) */
    result = "\033[C";
  else if (strcmp (id, "le") == 0)   /* Cursor left */
    result = "\033[D";
  else if (strcmp (id, "ho") == 0)   /* Home cursor */
    result = "\033[H";
  else if (strcmp (id, "vi") == 0)   /* Cursor invisible */
    result = "\033[?25l";
  else if (strcmp (id, "ve") == 0)   /* Cursor visible */
    result = "\033[?25h";
  else if (strcmp (id, "ti") == 0)   /* Start program that uses cursor motion */
    result = "\033[?1049h";
  else if (strcmp (id, "te") == 0)   /* End program that uses cursor motion */
    result = "\033[?1049l";
  else if (strcmp (id, "ks") == 0)   /* Keypad start */
    result = "\033[?1h\033=";
  else if (strcmp (id, "ke") == 0)   /* Keypad end */
    result = "\033[?1l\033>";
  else if (strcmp (id, "bc") == 0)   /* Backspace char */
    result = "\b";
  else if (strcmp (id, "pc") == 0)   /* Pad character */
    result = "\0";
  else if (strcmp (id, "op") == 0)   /* Reset fg/bg to defaults (orig pair).
                                        term.c gates ALL color support on
                                        this capability existing.  */
    result = "\033[39;49m";
  else if (strcmp (id, "AF") == 0)   /* ANSI set foreground, 256-color
                                        (fallback; the Tc probe upgrades
                                        both to direct RGB) */
    result = "\033[38;5;%p1%dm";
  else if (strcmp (id, "AB") == 0)   /* ANSI set background, 256-color */
    result = "\033[48;5;%p1%dm";
  else
    return NULL;

  if (result)
    {
      strncpy (buf, result, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';
      if (area && *area)
        {
          strcpy (*area, buf);
          result = *area;
          *area += strlen (buf) + 1;
        }
      else
        {
          result = buf;
        }
    }

  return (char *)result;
}

/* tgetnum - get numeric capability.  */
int
tgetnum (const char *id)
{
  if (!id)
    return -1;

  if (strcmp (id, "co") == 0)        /* Number of columns */
    {
      const char *cols = getenv ("COLUMNS");
      return cols ? atoi (cols) : 80;
    }
  else if (strcmp (id, "li") == 0)   /* Number of lines */
    {
      const char *lines = getenv ("LINES");
      return lines ? atoi (lines) : 24;
    }
  else if (strcmp (id, "sg") == 0)   /* Standout glitch */
    return 0;
  else if (strcmp (id, "ug") == 0)   /* Underline glitch */
    return 0;
  else if (strcmp (id, "Co") == 0)   /* Max colors.  Baseline value; the
                                        Tc probe in term.c raises
                                        TN_max_colors to 16777216.  */
    return 256;

  return -1;  /* Unknown capability */
}

/* tgetflag - get boolean capability.  */
int
tgetflag (const char *id)
{
  if (!id)
    return 0;

  if (strcmp (id, "am") == 0)        /* Automatic margins */
    return 1;
  else if (strcmp (id, "bs") == 0)   /* Terminal can backspace */
    return 1;
  else if (strcmp (id, "bw") == 0)   /* Backspace wraps */
    return 0;
  else if (strcmp (id, "xn") == 0)   /* Newline ignored after 80 cols */
    return 1;
  else if (strcmp (id, "ms") == 0)   /* Safe to move in standout mode */
    return 1;
  else if (strcmp (id, "km") == 0)   /* Has meta key */
    return 1;
  else if (strcmp (id, "mi") == 0)   /* Safe to move in insert mode */
    return 1;

  return 0;  /* Unknown or false */
}

/* ============================================================
   Missing libc function stubs

   Some newer libc functions aren't available in Emscripten.
   ============================================================ */

/* explicit_memset - like memset but won't be optimized away.
   Used for secure memory clearing.  */
void *
explicit_memset (void *s, int c, size_t n)
{
  volatile unsigned char *p = s;
  while (n--)
    *p++ = (unsigned char)c;
  return s;
}

/* memset_explicit - C23 version of explicit_memset.  */
void *
memset_explicit (void *s, int c, size_t n)
{
  return explicit_memset (s, c, n);
}

/* ============================================================
   Additional terminfo/ncurses stubs
   ============================================================ */

/* tigetstr - get string capability from terminfo database.
   Like tgetstr, a fixed whitelist of what xterm.js supports.  String
   literals are returned directly: term.c never frees tigetstr
   results.  */
char *
tigetstr (const char *capname)
{
  if (!capname)
    return NULL;

  if (strcmp (capname, "Smulx") == 0)
    /* Styled underlines (kitty protocol; xterm.js renders SGR 4:n).
       Its presence also makes term.c install the standard
       underline-COLOR sequence (SGR 58) -- see init_tty.  */
    return "\x1b[4:%p1%dm";
  if (strcmp (capname, "smxx") == 0)   /* Strike-through */
    return "\033[9m";

  /* Unknown capability.  */
  return NULL;
}

/* tigetflag - get boolean capability from terminfo database.  */
int
tigetflag (const char *capname)
{
  if (!capname)
    return -1;

  /* Tc (tmux's de-facto truecolor flag): xterm.js renders 24-bit SGR
     38;2/48;2 natively.  This is what flips init_tty into direct-RGB
     mode: TN_max_colors = 16777216, TF_rgb_separate = 1.  */
  if (strcmp (capname, "Tc") == 0)
    return 1;
  /* RGB is probed before Tc; report "present but false" so the probe
     chain falls through to Tc rather than the RGB code path (whose
     escape-sequence format xterm.js does not prefer).  */
  if (strcmp (capname, "RGB") == 0)
    return 0;

  /* Unknown capability.  */
  return -1;
}

/* tparm lives in wasm-tparm.c: a real terminfo %-language
   interpreter.  (The old stub here returned the format string
   uninstantiated, which would have emitted literal "%p1%d" into the
   terminal the moment any parameterized capability existed.)  */

/* ============================================================
   PTY Data Availability Check

   Check if the xterm-pty PTY slave has readable data.
   Used by tty_read_avail_input to avoid blocking read() calls.
   ============================================================ */

#include <emscripten.h>

/* Check if the xterm-pty PTY has data available for reading.
   Returns 1 if data is available, 0 otherwise.
   Uses select() with timeout 0 — this is proxied to the main thread
   where xterm-pty's poll handler checks PTY.readable.  */

#include <sys/select.h>

EMSCRIPTEN_KEEPALIVE
int
wasm_pty_has_data (void)
{
  fd_set rfds;
  struct timeval tv;

  FD_ZERO (&rfds);
  FD_SET (STDIN_FILENO, &rfds);

  /* Zero timeout = non-blocking poll.  */
  tv.tv_sec = 0;
  tv.tv_usec = 0;

  int ret = select (STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
  return ret > 0 && FD_ISSET (STDIN_FILENO, &rfds);
}

/* sysinfo - get system information (Linux-specific).  */
struct sysinfo_stub { long uptime; unsigned long loads[3]; unsigned long totalram; };
int
sysinfo (struct sysinfo_stub *info)
{
  /* Return error - not available in WASM.  */
  errno = ENOSYS;
  return -1;
}

#endif /* __EMSCRIPTEN__ */
