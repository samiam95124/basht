#ifndef MTSH_H
#define MTSH_H

#include <stddef.h>
#include <sys/types.h>

#define LINEBUF_CAP  8192
#define MAX_JOBS     64
#define JOB_NAME_MAX 64

/* Escape-filter state, kept per stream so sequences split across
   read() boundaries never leak fragments into the display. */
enum filt_state {
    F_NORMAL,   /* passing printable bytes            */
    F_ESC,      /* seen ESC                           */
    F_CSI,      /* inside ESC [ ... final 0x40-0x7e   */
    F_OSC,      /* inside ESC ] ... BEL or ESC \      */
    F_OSC_ESC   /* inside OSC, seen ESC, expecting \  */
};

/* One stream's partial line. The filter runs it as a tiny one-line
   terminal: \r and \b move the cursor, printables overwrite, CSI
   K/C/D erase/move — so echoed input editing and progress-bar
   rewrites render correctly when the line is shown live. */
struct linebuf {
    char            data[LINEBUF_CAP];
    size_t          len;      /* logical line length            */
    size_t          cur;      /* cursor position, always <= len */
    enum filt_state fs;
    int             csi_n;    /* first CSI numeric parameter    */
    int             csi_more; /* saw ';' — later params ignored */
};

struct job {
    int    id;                  /* 1, 2, 3, ...; 0 = slot free    */
    char   name[JOB_NAME_MAX];  /* basename of first command word */
    pid_t  pid;
    int    m_in, m_out, m_err;  /* pty master fds; -1 when closed */
    struct linebuf out_buf, err_buf;
    int    running;
    int    exit_status;
};

/* job.c */
int         pty_master(char *sname, size_t cap); /* alloc one master */
struct job *job_spawn(char **argv);
void        job_free(struct job *j);
struct job *job_get(int idx);        /* slot 0..MAX_JOBS-1; id==0 = free */
struct job *job_find(int id);
struct job *job_find_pid(pid_t pid);

/* filter.c — stage 1 (escape strip) + stage 2 (line assembly).
   mark: 0 = stdout, '!' = stderr, '*' = lifecycle event. */
void filter_bytes(const struct job *j, struct linebuf *lb,
                  const unsigned char *buf, size_t n, char mark);
void filter_flush(const struct job *j, struct linebuf *lb, char mark);

/* display.c — the single point that writes to the real terminal.
   The bottom line of the screen is owned by whichever stream last
   wrote a character on an incomplete line; completed lines erase
   it, print through, and display_sync() redraws the owner's
   partial. When nobody owns it, the default stream (task 0 — the
   shell's own echo, i.e. the prompt) is drawn instead. */
void display_line(const struct job *j, char mark,
                  const char *text, size_t len);
void display_event(const struct job *j, const char *fmt, ...);
void display_partial(const struct job *j, const struct linebuf *lb,
                     char mark);
void display_job_gone(const struct job *j);
void display_sync(void);
void display_set_default(const struct job *j, const struct linebuf *lb);
void write_all(int fd, const void *buf, size_t n);

/* The display layer writes only to the real terminal, kept on a
   private fd once mtsh's own stdout/stderr are rerouted into its
   task-0 pty. */
void display_set_terminal(int fd);
void display_raw(const char *s);

#endif
