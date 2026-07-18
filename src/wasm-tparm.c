/* wasm-tparm.c --- terminfo parameterized-string interpreter for WASM.

   The browser build has no terminfo library.  wasm-stubs.c plays the
   capability DATABASE (fixed whitelists of what xterm.js supports);
   this file is the %-language INTERPRETER that instantiates the
   parameterized capability strings term.c feeds through tparam()
   (src/terminfo.c) -> tparm().

   Deliberately free of Emscripten headers so the host cc can compile
   it for the native unit test (emacs-build-wasm
   scripts/test/test-tparm.c), which asserts the exact strings term.c
   uses.

   Calling convention: Emacs's only tparm caller is tparam() in
   src/terminfo.c, which always passes exactly four int arguments; we
   read exactly four (slots p5..p9 stay 0).  Do not call this tparm
   with fewer arguments.

   Supported operators: %% %p1..%p9 %d %c %x %X %o (with optional
   printf width, e.g. %2d %02d) %{n} %'c' %+ %- %* %/ %m %& %| %^
   %= %> %< %A %O %! %~ %i %P[a-zA-Z] %g[a-zA-Z] and the conditional
   %? %t %e %; (nesting-aware; a dangling %; with no open %? is a
   no-op, which Emacs's truecolor strings "...%p3%d%;m" rely on).
   String operators (%s %l) have no string stack here and expand to
   nothing: no capability this build serves uses them.  */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TPARM_OUT_SIZE 512
#define TPARM_STACK_SIZE 32

static char tparm_out[TPARM_OUT_SIZE];

struct tparm_ctx
{
  long stack[TPARM_STACK_SIZE];
  int sp;
  size_t len;
};

/* %Pa..%Pz then %PA..%PZ.  */
static long tparm_vars[52];

static void
tp_putc (struct tparm_ctx *c, char ch)
{
  if (c->len < TPARM_OUT_SIZE - 1)
    tparm_out[c->len++] = ch;
}

static void
tp_puts (struct tparm_ctx *c, const char *s)
{
  while (*s)
    tp_putc (c, *s++);
}

static void
tp_push (struct tparm_ctx *c, long v)
{
  if (c->sp < TPARM_STACK_SIZE)
    c->stack[c->sp++] = v;
}

/* Popping an empty stack yields 0, as in ncurses.  */
static long
tp_pop (struct tparm_ctx *c)
{
  return c->sp > 0 ? c->stack[--c->sp] : 0;
}

static int
tp_varslot (char ch)
{
  if (ch >= 'a' && ch <= 'z')
    return ch - 'a';
  if (ch >= 'A' && ch <= 'Z')
    return 26 + (ch - 'A');
  return -1;
}

/* Skip a conditional branch.  S points just past the %t (or %e) whose
   branch is being skipped.  Returns the position just past the %e
   opening the else-branch (when TO_ELSE and one exists at this
   nesting level) or just past the %; closing the conditional (or end
   of string).  Nested %?..%; groups are stepped over whole; %'c' and
   %{n} are skipped so a quoted ';' or '}' cannot derail the scan.  */
static const char *
tp_skip_branch (const char *s, int to_else)
{
  int level = 0;
  while (*s)
    {
      char op;
      if (*s++ != '%')
        continue;
      op = *s ? *s++ : 0;
      if (op == '?')
        level++;
      else if (op == ';')
        {
          if (level == 0)
            return s;
          level--;
        }
      else if (op == 'e' && level == 0 && to_else)
        return s;
      else if (op == '\'')
        {
          if (*s)
            s++;                /* the quoted char */
          if (*s == '\'')
            s++;                /* closing quote */
        }
      else if (op == '{')
        {
          while (*s && *s != '}')
            s++;
          if (*s)
            s++;
        }
    }
  return s;
}

static void
tp_print_num (struct tparm_ctx *c, const char *spec, char conv, long v)
{
  char fmt[16];
  char num[64];
  int n = snprintf (fmt, sizeof fmt, "%%%sl%c", spec, conv);
  if (n < 0 || n >= (int) sizeof fmt)
    return;
  snprintf (num, sizeof num, fmt, v);
  tp_puts (c, num);
}

char *
tparm (const char *str, ...)
{
  struct tparm_ctx c = { { 0 }, 0, 0 };
  long p[10] = { 0 };
  const char *s;
  va_list ap;
  int i;

  if (!str)
    {
      tparm_out[0] = '\0';
      return tparm_out;
    }

  va_start (ap, str);
  for (i = 1; i <= 4; i++)      /* terminfo.c's tparam passes 4 ints */
    p[i] = va_arg (ap, int);
  va_end (ap);

  s = str;
  while (*s)
    {
      char op;
      if (*s != '%')
        {
          tp_putc (&c, *s++);
          continue;
        }
      s++;                      /* past '%' */
      op = *s ? *s++ : 0;
      if (!op)
        break;

      /* Optional printf-style width/pad before a numeric conversion:
         %2d %02d %3d etc.  */
      if ((op >= '0' && op <= '9') || op == '.')
        {
          char spec[8];
          int si = 0;
          while (si < 4 && ((op >= '0' && op <= '9') || op == '.'))
            {
              spec[si++] = op;
              op = *s ? *s++ : 0;
              if (!op)
                break;
            }
          spec[si] = '\0';
          if (op == 'd' || op == 'x' || op == 'X' || op == 'o')
            tp_print_num (&c, spec, op, tp_pop (&c));
          continue;
        }

      switch (op)
        {
        case '%':
          tp_putc (&c, '%');
          break;
        case 'd':
        case 'x':
        case 'X':
        case 'o':
          tp_print_num (&c, "", op, tp_pop (&c));
          break;
        case 'c':
          tp_putc (&c, (char) tp_pop (&c));
          break;
        case 's':
        case 'l':
          /* No string stack; nothing this build serves uses these.  */
          break;
        case 'p':
          if (*s >= '1' && *s <= '9')
            tp_push (&c, p[*s++ - '0']);
          break;
        case '{':
          {
            long v = 0;
            int neg = 0;
            if (*s == '-')
              {
                neg = 1;
                s++;
              }
            while (*s >= '0' && *s <= '9')
              v = v * 10 + (*s++ - '0');
            if (*s == '}')
              s++;
            tp_push (&c, neg ? -v : v);
          }
          break;
        case '\'':
          if (*s)
            {
              tp_push (&c, (unsigned char) *s++);
              if (*s == '\'')
                s++;
            }
          break;
        case '+':
        case '-':
        case '*':
        case '/':
        case 'm':
        case '&':
        case '|':
        case '^':
        case '=':
        case '>':
        case '<':
        case 'A':
        case 'O':
          {
            long y = tp_pop (&c);
            long x = tp_pop (&c);
            long r = 0;
            switch (op)
              {
              case '+': r = x + y; break;
              case '-': r = x - y; break;
              case '*': r = x * y; break;
              case '/': r = y ? x / y : 0; break;
              case 'm': r = y ? x % y : 0; break;
              case '&': r = x & y; break;
              case '|': r = x | y; break;
              case '^': r = x ^ y; break;
              case '=': r = x == y; break;
              case '>': r = x > y; break;
              case '<': r = x < y; break;
              case 'A': r = x && y; break;
              case 'O': r = x || y; break;
              }
            tp_push (&c, r);
          }
          break;
        case '!':
          tp_push (&c, !tp_pop (&c));
          break;
        case '~':
          tp_push (&c, ~tp_pop (&c));
          break;
        case 'i':
          p[1]++;
          p[2]++;
          break;
        case 'P':
          {
            int slot = tp_varslot (*s);
            if (slot >= 0)
              {
                tparm_vars[slot] = tp_pop (&c);
                s++;
              }
          }
          break;
        case 'g':
          {
            int slot = tp_varslot (*s);
            if (slot >= 0)
              {
                tp_push (&c, tparm_vars[slot]);
                s++;
              }
          }
          break;
        case '?':
          break;
        case 't':
          if (tp_pop (&c) == 0)
            s = tp_skip_branch (s, 1);
          break;
        case 'e':
          /* A taken then-branch just ended: skip the else-branch.  */
          s = tp_skip_branch (s, 0);
          break;
        case ';':
          /* Also reached for the dangling %; in Emacs's truecolor
             strings -- a no-op either way.  */
          break;
        default:
          break;                /* unknown operator: ignore */
        }
    }

  tparm_out[c.len] = '\0';
  return tparm_out;
}
