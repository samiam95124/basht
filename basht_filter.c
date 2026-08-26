/* basht_filter.c -- stage 1: escape stripping; stage 2: line
   assembly with one-line terminal emulation. Lifted from mtsh
   filter.c; see basht.h. Escape sequences are never passed through
   to the real terminal. */

#include "config.h"

#include <string.h>

#include "basht.h"

#define WRAP_MARK " [wrapped]"

static void
emit (BASHT_STREAM *ts)
{
  basht_display_line (ts, ts->lb.data, ts->lb.len);
  ts->lb.len = ts->lb.cur = 0;
}

/* The cursor left this line since the last edit (cursor
   addressing, save/restore, scrolling): whatever was assembled is
   an in-place paint the one-line model cannot place -- a progress
   bar drawn at the screen bottom, say. It stays displayed as the
   partial until the next edit, which starts a fresh line instead
   of concatenating. */
static void
fresh (struct basht_linebuf *lb)
{
  if (lb->moved)
    {
      lb->len = lb->cur = 0;
      lb->moved = 0;
    }
}

static void
putch (BASHT_STREAM *ts, char c)
{
  struct basht_linebuf *lb = &ts->lb;

  fresh (lb);
  /* on overflow force a line break; never drop bytes silently */
  if (lb->cur >= BASHT_LINEBUF_CAP - sizeof WRAP_MARK)
    {
      memcpy (lb->data + lb->len, WRAP_MARK, sizeof WRAP_MARK - 1);
      lb->len += sizeof WRAP_MARK - 1;
      emit (ts);
    }
  lb->data[lb->cur++] = c;      /* overwrite, or append at end */
  if (lb->cur > lb->len)
    lb->len = lb->cur;
}

size_t
basht_filter_bytes (BASHT_STREAM *ts, const unsigned char *buf,
		    size_t n, int *trigger)
{
  struct basht_linebuf *lb = &ts->lb;
  size_t i;

  for (i = 0; i < n; i++)
    {
      unsigned char c = buf[i];
      switch (lb->fs)
	{
	case BF_NORMAL:
	  if (c == 0x1b)
	    lb->fs = BF_ESC;
	  else if (c == '\n')
	    {
	      /* a newline on a line the cursor already left has no
		 content of its own: discard the paint, emit nothing */
	      if (lb->moved)
		fresh (lb);
	      else
		emit (ts);
	    }
	  else if (c == '\r')
	    {
	      fresh (lb);
	      lb->cur = 0;
	    }
	  else if (c == '\b')
	    {
	      fresh (lb);
	      if (lb->cur > 0)
		lb->cur--;
	    }
	  else if (c == '\t')
	    putch (ts, '\t');
	  else if (c < 0x20 || c == 0x7f)
	    ;			/* delete remaining C0 controls and DEL */
	  else
	    putch (ts, (char)c);
	  break;
	case BF_ESC:
	  if (c == '[')
	    {
	      lb->fs = BF_CSI;
	      lb->csi_n = 0;
	      lb->csi_more = 0;
	      lb->csi_priv = 0;
	    }
	  else if (c == ']')
	    lb->fs = BF_OSC;
	  else if (c >= 0x20 && c <= 0x2f)
	    ;			/* intermediate byte; stay */
	  else
	    {
	      /* restore cursor / index / reverse index: row change */
	      if (c == '8' || c == 'D' || c == 'M')
		lb->moved = 1;
	      lb->fs = BF_NORMAL;	/* two-char escape: consume both */
	    }
	  break;
	case BF_CSI:
	  if (c >= '0' && c <= '9' && lb->csi_more == 0)
	    lb->csi_n = lb->csi_n * 10 + (c - '0');
	  else if (c == ';')
	    lb->csi_more = 1;
	  else if (c == '?')
	    lb->csi_priv = 1;
	  else if (c >= 0x40 && c <= 0x7e)
	    {			/* final byte */
	      size_t nn = lb->csi_n > 0 ? (size_t)lb->csi_n : 1;
	      size_t tail;
	      /* row-changing motions and scrolls: the cursor leaves
		 this line (progress bars repaint via these) */
	      if (c == 'A' || c == 'B' || c == 'E' || c == 'F'
		  || c == 'H' || c == 'J' || c == 'S' || c == 'T'
		  || c == 'd' || c == 'f' || c == 'r')
		lb->moved = 1;
	      /* in-line edits land on the line the cursor is on now */
	      else if (c == 'K' || c == 'C' || c == 'D' || c == 'G'
		       || c == 'P' || c == '@' || c == 'X')
		fresh (lb);
	      tail = lb->len - lb->cur;
	      if (c == 'K')	/* erase to end of line */
		lb->len = lb->cur;
	      else if (c == 'C') /* cursor right */
		lb->cur = lb->cur + nn > lb->len ? lb->len : lb->cur + nn;
	      else if (c == 'D') /* cursor left */
		lb->cur = nn > lb->cur ? 0 : lb->cur - nn;
	      else if (c == 'G') /* cursor to column (1-based) */
		lb->cur = nn - 1 > lb->len ? lb->len : nn - 1;
	      else if (c == 'P') /* delete chars at cursor */
		{
		  if (nn > tail)
		    nn = tail;
		  memmove (lb->data + lb->cur, lb->data + lb->cur + nn,
			   tail - nn);
		  lb->len -= nn;
		}
	      else if (c == '@') /* insert blanks at cursor */
		{
		  if (nn > BASHT_LINEBUF_CAP - 1 - lb->len)
		    nn = BASHT_LINEBUF_CAP - 1 - lb->len;
		  memmove (lb->data + lb->cur + nn, lb->data + lb->cur,
			   tail);
		  memset (lb->data + lb->cur, ' ', nn);
		  lb->len += nn;
		}
	      else if (c == 'X') /* erase chars in place */
		memset (lb->data + lb->cur, ' ', nn > tail ? tail : nn);
	      lb->fs = BF_NORMAL;
	      /* full-screen program: the alternate screen (vim, less,
		 htop) or focus tracking, which only a program driving
		 the whole terminal asks for -- inline TUIs (the claude
		 CLI) never leave the primary screen but do want focus
		 events. Let the caller move it into a window. */
	      if (trigger && c == 'h' && lb->csi_priv
		  && (lb->csi_n == 1049 || lb->csi_n == 1047
		      || lb->csi_n == 47 || lb->csi_n == 1004))
		{
		  *trigger = 1;
		  basht_display_partial (ts);
		  return i + 1;
		}
	    }
	  /* else: other parameter/intermediate bytes, consume */
	  break;
	case BF_OSC:
	  if (c == 0x07)
	    lb->fs = BF_NORMAL;	/* BEL terminator */
	  else if (c == 0x1b)
	    lb->fs = BF_OSC_ESC;
	  break;
	case BF_OSC_ESC:
	  if (c == '\\')
	    lb->fs = BF_NORMAL;	/* ST terminator */
	  else if (c != 0x1b)
	    lb->fs = BF_OSC;
	  break;
	}
    }
  /* whoever wrote on an incomplete line last owns the bottom line */
  basht_display_partial (ts);
  return n;
}

/* Stream EOF: a final unterminated line must not be lost. */
void
basht_filter_flush (BASHT_STREAM *ts)
{
  if (ts->lb.moved)
    ts->lb.len = ts->lb.cur = 0;	/* abandoned paint: drop it */
  else if (ts->lb.len > 0)
    emit (ts);
  ts->lb.moved = 0;
  ts->lb.fs = BF_NORMAL;
  basht_display_partial (ts);	/* releases bottom-line ownership */
}
