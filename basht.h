/* basht.h -- line-multiplexed tagged display for bash (the tsh model).

   Lifted from the mtsh prototype (see tsh_spec.md / basht_plan.md in
   the parent repo). Every output stream is filtered (escape strip +
   one-line terminal emulation), assembled into whole lines, and
   displayed with an identity tag. The bottom console line is owned
   by whichever stream last wrote a character on an incomplete line;
   completed lines erase it, print through, and the owner's partial
   is redrawn. The shell itself is task 0: its own stdout/stderr run
   through a pty like any job's would. */

#ifndef _BASHT_H_
#define _BASHT_H_

#include <stddef.h>
#include <sys/types.h>

#define BASHT_LINEBUF_CAP 8192
#define BASHT_NAME_MAX    64

enum basht_filt_state {
  BF_NORMAL, BF_ESC, BF_CSI, BF_OSC, BF_OSC_ESC
};

/* One stream's partial line, run as a tiny one-line terminal:
   \r and \b move the cursor, printables overwrite, CSI K/C/D/G/P/@/X
   erase/move/edit, so echoed input editing and progress rewrites
   reconstruct correctly. State survives read boundaries. */
struct basht_linebuf {
  char   data[BASHT_LINEBUF_CAP];
  size_t len;                   /* logical line length            */
  size_t cur;                   /* cursor position, always <= len */
  size_t fix;                   /* immutable prefix: relay lines
                                   carry the task's echoed prompt
                                   here; never shipped or edited.
                                   Always 0 for filtered streams. */
  enum basht_filt_state fs;
  int    csi_n;                 /* first CSI numeric parameter    */
  int    csi_more;              /* saw ';' -- later params ignored */
  int    csi_priv;              /* saw '?' (DEC private mode)     */
};

typedef struct basht_stream {
  char name[BASHT_NAME_MAX];
  int  id;
  char mark;                    /* 0 stdout, '!' stderr, '*' event */
  pid_t pid;                    /* shown in the tag; kill-able */
  int  *lines;                  /* shared per-task line counter:
                                   ++ on each completed line */
  struct basht_linebuf lb;
} BASHT_STREAM;

/* basht_filter.c. basht_filter_bytes consumes BUF and returns how
   many bytes it processed -- always N unless TRIGGER is non-null
   and a full-screen escape (alternate-screen enable) is seen, in
   which case *TRIGGER is set and filtering stops just past the
   sequence so the caller can move the task into a window. */
size_t basht_filter_bytes (BASHT_STREAM *, const unsigned char *,
                           size_t, int *trigger);
void basht_filter_flush (BASHT_STREAM *);

/* basht_display.c -- the single writer to the real terminal */
void basht_display_line (const BASHT_STREAM *, const char *, size_t);
void basht_display_event (const BASHT_STREAM *, const char *, ...);
void basht_display_partial (const BASHT_STREAM *);
void basht_display_stream_gone (const BASHT_STREAM *);
void basht_display_sync (void);
void basht_display_set_default (const BASHT_STREAM *);
const BASHT_STREAM *basht_display_owner (void);
void basht_display_set_terminal (int);
void basht_write_all (int, const void *, size_t);

/* basht.c -- task 0, drain loop hooks */
extern int basht_active;        /* multiplexer is up */
extern int basht_tty;           /* the real terminal, display-only */
void basht_init (void);         /* interactive shells only; safe to
                                   call when not applicable */
void basht_drain (void);        /* poll+drain+sync; never blocks */
void basht_command_begin (void); /* echo accepted line, drop prompt */
void basht_child_stdio (void);  /* forked child: attach capture ptys
                                   (async jobs) or restore the real
                                   tty on fds 1/2 */
void basht_fork_prepare (const char *, int); /* parent, pre-fork  */
void basht_fork_done (pid_t);                /* parent, post-fork */
int  basht_fg_pump (pid_t);     /* fg wait loop: drain + relay; 1 if
                                   pumped (reap WNOHANG), 0 = block */
void basht_fg_end (void);       /* fg wait over: restore terminal */
int  basht_send_input (const char *, int, const char *);
                                /* `feed' builtin: name, instance
                                   (<=0 = sole live match), text */
int  basht_bridge_main (const char *, const char *);
                                /* --basht-bridge: run inside the
                                   spawned terminal window */

#endif /* _BASHT_H_ */
