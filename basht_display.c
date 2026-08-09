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

/* Tag: [name:n pid line] with the stream mark, if any, after n.
   LINENO is the completed line's number, or the number the pending
   bottom line will get. */
static int
make_tag (char *out, size_t cap, const BASHT_STREAM *ts, int lineno)
{
  if (ts->mark)
    return snprintf (out, cap, "[%s:%d%c %ld %d] ", ts->name, ts->id,
		     ts->mark, (long)ts->pid, lineno);
  return snprintf (out, cap, "[%s:%d %ld %d] ", ts->name, ts->id,
		   (long)ts->pid, lineno);
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
