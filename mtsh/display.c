/* display.c — the single point that writes to the real terminal.
 * Nothing else in mtsh writes there.
 *
 * The bottom line of the console is owned by whichever stream last
 * wrote a character on an incomplete line. Completed lines print
 * "through" it: erase the bottom line (CR, spaces, CR), write the
 * tagged completed line (scrolling the screen), then display_sync()
 * redraws the owner's partial line beneath. When no stream owns the
 * bottom line, the default stream (task 0 — the shell's own echo,
 * which is what the user sees as the prompt) is drawn.
 */

#include "mtsh.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int term_fd = 1; /* the real terminal, once task 0 exists */

static const struct job     *bot_job; /* bottom-line owner, if any */
static const struct linebuf *bot_lb;
static char                  bot_mark;
static const struct job     *def_job; /* default owner: task 0 */
static const struct linebuf *def_lb;
static size_t shown; /* characters currently drawn on the bottom line */
static int    dirty; /* bottom line needs redrawing */

void write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        p += w;
        n -= (size_t)w;
    }
}

void display_set_terminal(int fd)
{
    term_fd = fd;
}

void display_set_default(const struct job *j, const struct linebuf *lb)
{
    def_job = j;
    def_lb = lb;
    dirty = 1;
}

void display_raw(const char *s)
{
    write_all(term_fd, s, strlen(s));
}

static int make_tag(char *out, size_t cap, const struct job *j,
                    char mark)
{
    if (mark)
        return snprintf(out, cap, "[%s:%d%c] ", j->name, j->id, mark);
    return snprintf(out, cap, "[%s:%d] ", j->name, j->id);
}

/* CR, as many spaces as are drawn, CR. */
static void erase_bottom(void)
{
    char pad[128];

    if (shown == 0)
        return;
    memset(pad, ' ', sizeof pad);
    write_all(term_fd, "\r", 1);
    for (size_t n = shown; n > 0;) {
        size_t k = n < sizeof pad ? n : sizeof pad;
        write_all(term_fd, pad, k);
        n -= k;
    }
    write_all(term_fd, "\r", 1);
    shown = 0;
}

/* One tagged completed line, one write: "[name:N] text\n". */
void display_line(const struct job *j, char mark,
                  const char *text, size_t len)
{
    char out[LINEBUF_CAP + JOB_NAME_MAX + 32];
    int t = make_tag(out, sizeof out, j, mark);

    if (t < 0)
        return;
    erase_bottom();
    dirty = 1; /* owner's partial goes back up at the next sync */

    size_t off = (size_t)t;
    if (len > sizeof out - off - 1)
        len = sizeof out - off - 1;
    memcpy(out + off, text, len);
    out[off + len] = '\n';
    write_all(term_fd, out, off + len + 1);
}

void display_event(const struct job *j, const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    display_line(j, '*', msg, strlen(msg));
}

/* A stream's partial line changed: it takes (or, now empty, gives
   up) ownership of the bottom line. Drawing is deferred to
   display_sync() so a drain pass redraws at most once. */
void display_partial(const struct job *j, const struct linebuf *lb,
                     char mark)
{
    if (lb->len > 0) {
        bot_job = j;
        bot_lb = lb;
        bot_mark = mark;
    } else if (bot_lb == lb) {
        bot_job = NULL;
        bot_lb = NULL;
    }
    dirty = 1;
}

/* The owning job is being freed: never draw its tag again. */
void display_job_gone(const struct job *j)
{
    if (bot_job == j) {
        bot_job = NULL;
        bot_lb = NULL;
        dirty = 1;
    }
}

void display_sync(void)
{
    if (!dirty)
        return;
    dirty = 0;

    const struct job     *j  = bot_job != NULL ? bot_job : def_job;
    const struct linebuf *lb = bot_lb != NULL ? bot_lb : def_lb;
    char mark = bot_lb != NULL ? bot_mark : 0;

    erase_bottom();
    if (j == NULL || lb == NULL)
        return;

    char out[LINEBUF_CAP + JOB_NAME_MAX + 32];
    int t = make_tag(out, sizeof out, j, mark);
    if (t < 0)
        return;
    size_t off = (size_t)t;
    size_t len = lb->len;
    if (len > sizeof out - off)
        len = sizeof out - off;
    memcpy(out + off, lb->data, len);
    write_all(term_fd, out, off + len);
    shown = off + len;

    /* park the terminal cursor at the stream's cursor column */
    if (lb->cur < len) {
        char bs[128];
        memset(bs, '\b', sizeof bs);
        for (size_t n = len - lb->cur; n > 0;) {
            size_t k = n < sizeof bs ? n : sizeof bs;
            write_all(term_fd, bs, k);
            n -= k;
        }
    }
}
