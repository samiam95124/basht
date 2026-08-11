/* basht_display.c -- the single point that writes to the real
   terminal. Lifted from mtsh display.c; see basht.h.

   The bottom console line is owned by whichever stream last wrote a
   character on an incomplete line. Completed lines print "through"
   it: erase (CR, spaces, CR), write the tagged line (scrolling),
   then basht_display_sync() redraws the owner's partial beneath.
   When no stream owns it, the default stream (task 0 -- the shell's
   own echo, i.e. the prompt) is drawn. */

#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "basht.h"

static int term_fd = 1;

static const BASHT_STREAM *bot;	   /* bottom-line owner, if any */
static const BASHT_STREAM *def;	   /* default owner: task 0     */
static size_t shown;		   /* chars drawn on bottom line */
static int dirty;

void
basht_write_all (int fd, const void *buf, size_t n)
{
  const char *p = buf;

  while (n > 0)
    {
      ssize_t w = write (fd, p, n);
      if (w < 0)
	{
	  if (errno == EINTR)
	    continue;
	  return;
	}
      p += w;
      n -= (size_t)w;
    }
}

void
basht_display_set_terminal (int fd)
{
  term_fd = fd;
}

void
basht_display_set_default (const BASHT_STREAM *ts)
{
  if (def == ts)
    return;
  def = ts;
  dirty = 1;
}

/* The bottom-line owner under the incomplete-line rule, or 0 when
   no stream is mid-line. Input routing follows this: the owner's
   task receives the keyboard. */
const BASHT_STREAM *
basht_display_owner (void)
{
  return bot;
}

/* Manual selection (task cycling): force the owner. 0 falls back
   to the default stream at the next sync. */
void
basht_display_set_owner (const BASHT_STREAM *ts)
{
  if (bot == ts)
    return;
  bot = ts;
  dirty = 1;
}

/* PST1: the tag template, the preamble's own prompt string. 0
   means the built-in default. Fields: \n program name, \i instance
   (the stream mark -- '!' stderr, '*' event -- rides just after
   it), \t task process id, \l line number; \\ a backslash; other
   escapes pass through literally. The rendered tag is followed by
   one separating space; an empty template renders untagged lines. */
static const char *tagfmt;

void
basht_display_set_tagfmt (const char *fmt)
{
  tagfmt = fmt;
}

/* Render the tag for TS. LINENO is the completed line's number, or
   the number the pending bottom line will get. */
static int
make_tag (char *out, size_t cap, const BASHT_STREAM *ts, int lineno)
{
  const char *f = tagfmt ? tagfmt : "[\\n:\\i:\\t:\\l]";
  size_t o = 0;
  int w;

  while (*f && o + 2 < cap)
    {
      if (*f != '\\' || f[1] == '\0')
	{
	  out[o++] = *f++;
	  continue;
	}
      f++;			/* at the escape letter */
      w = 0;
      switch (*f)
	{
	case 'n':
	  w = snprintf (out + o, cap - o - 2, "%s", ts->name);
	  break;
	case 'i':
	  if (ts->mark)
	    w = snprintf (out + o, cap - o - 2, "%d%c", ts->id, ts->mark);
	  else
	    w = snprintf (out + o, cap - o - 2, "%d", ts->id);
	  break;
	case 't':
	  w = snprintf (out + o, cap - o - 2, "%ld", (long)ts->pid);
	  break;
	case 'l':
	  w = snprintf (out + o, cap - o - 2, "%d", lineno);
	  break;
	case '\\':
	  out[o++] = '\\';
	  break;
	default:		/* unknown escape: literal */
	  out[o++] = '\\';
	  out[o++] = *f;
	  break;
	}
      if (w > 0)
	o += (size_t)w <= cap - o - 2 ? (size_t)w : cap - o - 2;
      f++;
    }
  if (o > 0)
    out[o++] = ' ';		/* empty template: untagged lines */
  out[o] = '\0';
  return (int)o;
}

/* CR, as many spaces as are drawn, CR. */
static void
erase_bottom (void)
{
  char pad[128];

  if (shown == 0)
    return;
  memset (pad, ' ', sizeof pad);
  basht_write_all (term_fd, "\r", 1);
  for (size_t n = shown; n > 0;)
    {
      size_t k = n < sizeof pad ? n : sizeof pad;
      basht_write_all (term_fd, pad, k);
      n -= k;
    }
  basht_write_all (term_fd, "\r", 1);
  shown = 0;
}

/* One tagged completed line, one write: "[name:N] text\n". */
void
basht_display_line (const BASHT_STREAM *ts, const char *text, size_t len)
{
  char out[BASHT_LINEBUF_CAP + BASHT_NAME_MAX + 64];
  int ln = ts->lines ? ++(*ts->lines) : 0;
  int t = make_tag (out, sizeof out, ts, ln);

  if (t < 0)
    return;
  erase_bottom ();
  dirty = 1;			/* owner's partial redraws at next sync */

  size_t off = (size_t)t;
  if (len > sizeof out - off - 1)
    len = sizeof out - off - 1;
  memcpy (out + off, text, len);
  out[off + len] = '\n';
  basht_write_all (term_fd, out, off + len + 1);
}

void
basht_display_event (const BASHT_STREAM *ts, const char *fmt, ...)
{
  BASHT_STREAM ev;
  char msg[256];
  va_list ap;

  va_start (ap, fmt);
  vsnprintf (msg, sizeof msg, fmt, ap);
  va_end (ap);

  ev = *ts;			/* same identity, event mark */
  ev.mark = '*';
  basht_display_line (&ev, msg, strlen (msg));
}

/* Operator clear (the `clear' builtin): wipe the screen -- and the
   scrollback, unless KEEP -- and home the cursor. The bottom line
   redraws at the next sync. Tasks cannot reach this: escapes in
   task output are filtered; clearing the shared console is the
   operator's decision alone. */
void
basht_display_clear (int keep)
{
  if (keep == 0)
    basht_write_all (term_fd, "\033[3J", 4);
  basht_write_all (term_fd, "\033[H\033[2J", 7);
  shown = 0;
  dirty = 1;
}

/* A stream's partial line changed: it takes (or, now empty, gives
   up) bottom-line ownership. Drawing is deferred to sync so one
   drain pass redraws at most once. */
void
basht_display_partial (const BASHT_STREAM *ts)
{
  if (ts->lb.len > 0)
    bot = ts;
  else if (bot == ts)
    bot = 0;
  dirty = 1;
}

/* The owning stream is going away: never draw its tag again. */
void
basht_display_stream_gone (const BASHT_STREAM *ts)
{
  if (bot == ts)
    {
      bot = 0;
      dirty = 1;
    }
  if (def == ts)
    def = 0;
}

void
basht_display_sync (void)
{
  const BASHT_STREAM *ts;

  if (dirty == 0)
    return;
  dirty = 0;

  ts = bot ? bot : def;
  erase_bottom ();
  if (ts == 0)
    return;

  char out[BASHT_LINEBUF_CAP + BASHT_NAME_MAX + 64];
  int t = make_tag (out, sizeof out, ts,
		    ts->lines ? *ts->lines + 1 : 0);
  if (t < 0)
    return;
  size_t off = (size_t)t;
  size_t len = ts->lb.len;
  if (len > sizeof out - off)
    len = sizeof out - off;
  memcpy (out + off, ts->lb.data, len);
  basht_write_all (term_fd, out, off + len);
  shown = off + len;

  /* park the terminal cursor at the stream's cursor column */
  if (ts->lb.cur < len)
    {
      char bs[128];
      memset (bs, '\b', sizeof bs);
      for (size_t n = len - ts->lb.cur; n > 0;)
	{
	  size_t k = n < sizeof bs ? n : sizeof bs;
	  basht_write_all (term_fd, bs, k);
	  n -= k;
	}
    }
}
