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

static void
putch (BASHT_STREAM *ts, char c)
{
  struct basht_linebuf *lb = &ts->lb;

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

void
basht_filter_bytes (BASHT_STREAM *ts, const unsigned char *buf, size_t n)
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
	    emit (ts);
	  else if (c == '\r')
	    lb->cur = 0;
	  else if (c == '\b')
	    {
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
	    }
	  else if (c == ']')
	    lb->fs = BF_OSC;
	  else if (c >= 0x20 && c <= 0x2f)
	    ;			/* intermediate byte; stay */
	  else
	    lb->fs = BF_NORMAL;	/* two-char escape: consume both */
	  break;
	case BF_CSI:
	  if (c >= '0' && c <= '9' && lb->csi_more == 0)
	    lb->csi_n = lb->csi_n * 10 + (c - '0');
	  else if (c == ';')
	    lb->csi_more = 1;
	  else if (c >= 0x40 && c <= 0x7e)
	    {			/* final byte */
	      size_t nn = lb->csi_n > 0 ? (size_t)lb->csi_n : 1;
	      size_t tail = lb->len - lb->cur;
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
}

/* Stream EOF: a final unterminated line must not be lost. */
void
basht_filter_flush (BASHT_STREAM *ts)
{
  if (ts->lb.len > 0)
    emit (ts);
  ts->lb.fs = BF_NORMAL;
  basht_display_partial (ts);	/* releases bottom-line ownership */
}
