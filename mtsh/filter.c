/* filter.c — stage 1: escape stripping; stage 2: line assembly.
 *
 * tsh is CLI-only: escape sequences are never passed through to the
 * real terminal. Stage 2 runs each stream as a tiny one-line
 * terminal (cursor motion via \r, \b, CSI C/D; erase via CSI K) so
 * that echoed input editing and progress-bar rewrites reconstruct
 * correctly; everything else is stripped. Filter state lives in the
 * linebuf so sequences split across read boundaries are held, not
 * leaked.
 */

#include "mtsh.h"

#include <string.h>

#define WRAP_MARK " [wrapped]"

static void emit(const struct job *j, struct linebuf *lb, char mark)
{
    display_line(j, mark, lb->data, lb->len);
    lb->len = 0;
    lb->cur = 0;
}

static void putch(const struct job *j, struct linebuf *lb, char mark,
                  char c)
{
    /* On overflow force a line break; never drop bytes silently. */
    if (lb->cur >= LINEBUF_CAP - sizeof WRAP_MARK) {
        memcpy(lb->data + lb->len, WRAP_MARK, sizeof WRAP_MARK - 1);
        lb->len += sizeof WRAP_MARK - 1;
        emit(j, lb, mark);
    }
    lb->data[lb->cur++] = c; /* overwrite, or append at end */
    if (lb->cur > lb->len)
        lb->len = lb->cur;
}

void filter_bytes(const struct job *j, struct linebuf *lb,
                  const unsigned char *buf, size_t n, char mark)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        switch (lb->fs) {
        case F_NORMAL:
            if (c == 0x1b) {
                lb->fs = F_ESC;
            } else if (c == '\n') {
                emit(j, lb, mark);
            } else if (c == '\r') {
                lb->cur = 0;
            } else if (c == '\b') {
                if (lb->cur > 0)
                    lb->cur--;
            } else if (c == '\t') {
                putch(j, lb, mark, '\t');
            } else if (c < 0x20 || c == 0x7f) {
                ; /* delete remaining C0 controls and DEL */
            } else {
                putch(j, lb, mark, (char)c);
            }
            break;
        case F_ESC:
            if (c == '[') {
                lb->fs = F_CSI;
                lb->csi_n = 0;
                lb->csi_more = 0;
            } else if (c == ']') {
                lb->fs = F_OSC;
            } else if (c >= 0x20 && c <= 0x2f) {
                ; /* intermediate byte (e.g. ESC ( B); stay */
            } else {
                lb->fs = F_NORMAL; /* two-char escape: consume both */
            }
            break;
        case F_CSI:
            if (c >= '0' && c <= '9' && !lb->csi_more) {
                lb->csi_n = lb->csi_n * 10 + (c - '0');
            } else if (c == ';') {
                lb->csi_more = 1;
            } else if (c >= 0x40 && c <= 0x7e) { /* final byte */
                size_t nn = lb->csi_n > 0 ? (size_t)lb->csi_n : 1;
                size_t tail = lb->len - lb->cur;
                if (c == 'K') {        /* erase to end of line */
                    lb->len = lb->cur;
                } else if (c == 'C') { /* cursor right */
                    lb->cur = lb->cur + nn > lb->len ? lb->len
                                                     : lb->cur + nn;
                } else if (c == 'D') { /* cursor left */
                    lb->cur = nn > lb->cur ? 0 : lb->cur - nn;
                } else if (c == 'G') { /* cursor to column (1-based) */
                    lb->cur = nn - 1 > lb->len ? lb->len : nn - 1;
                } else if (c == 'P') { /* delete chars at cursor */
                    if (nn > tail)
                        nn = tail;
                    memmove(lb->data + lb->cur,
                            lb->data + lb->cur + nn, tail - nn);
                    lb->len -= nn;
                } else if (c == '@') { /* insert blanks at cursor */
                    if (nn > LINEBUF_CAP - 1 - lb->len)
                        nn = LINEBUF_CAP - 1 - lb->len;
                    memmove(lb->data + lb->cur + nn,
                            lb->data + lb->cur, tail);
                    memset(lb->data + lb->cur, ' ', nn);
                    lb->len += nn;
                } else if (c == 'X') { /* erase chars in place */
                    memset(lb->data + lb->cur, ' ',
                           nn > tail ? tail : nn);
                }
                lb->fs = F_NORMAL;
            }
            /* else: other parameter/intermediate bytes, consume */
            break;
        case F_OSC:
            if (c == 0x07)
                lb->fs = F_NORMAL; /* BEL terminator */
            else if (c == 0x1b)
                lb->fs = F_OSC_ESC;
            break;
        case F_OSC_ESC:
            if (c == '\\')
                lb->fs = F_NORMAL; /* ST terminator */
            else if (c != 0x1b)
                lb->fs = F_OSC;
            break;
        }
    }
    /* whoever wrote on an incomplete line last owns the bottom line */
    display_partial(j, lb, mark);
}

/* Stream EOF: a final unterminated line must not be lost. */
void filter_flush(const struct job *j, struct linebuf *lb, char mark)
{
    if (lb->len > 0)
        emit(j, lb, mark);
    lb->fs = F_NORMAL;
    display_partial(j, lb, mark); /* releases bottom-line ownership */
}
