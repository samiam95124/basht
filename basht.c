/* basht.c -- task 0: the shell's own I/O runs through the
   multiplexer like any job's (phase 1 of basht_plan.md).

   At interactive startup the real terminal is dup'd to a private fd
   (the only fd the display layer writes), and a pty is allocated
   whose slave replaces the shell's stdout and stderr. Everything
   the shell prints -- prompt echo via readline, builtin output,
   error messages, job notifications -- flows into that pty, through
   the filter, and out as tagged "[bash:0]" lines. The drain hook
   runs from rl_event_hook while readline waits, and basht_drain()
   is called after each accepted line so the command echo appears
   before the command's own output.

   Phase 1 leaves children on the real terminal (stock semantics):
   basht_child_stdio() restores the real tty onto fds 1/2 in each
   forked child. Later phases give jobs their own pty trios. */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "bashtypes.h"
#include "shell.h"
#include "jobs.h"		/* FORK_* flags */

#include <readline/readline.h>
#include <readline/history.h>

#include "basht.h"

int basht_active = 0;
int basht_tty = -1;

/* set by execute_cmd.c just before make_child: the next fork is a
   sole command, eligible for a session of its own (issue #15) */
int basht_fork_solo = 0;

/* set in the child when it became a session leader owning its
   capture pty; jobs.c skips its setpgid then (EPERM for a leader,
   and the pgid is already right) */
int basht_child_sess = 0;

/* BASHT_DEBUG=<file>: append a trace of everything that touches the
   input path -- for hunting rare lost-keystroke reports. */
static FILE *dbglog;

static void
dbg (const char *fmt, ...)
{
  va_list ap;

  if (dbglog == 0)
    return;
  va_start (ap, fmt);
  vfprintf (dbglog, fmt, ap);
  va_end (ap);
  fputc ('\n', dbglog);
  fflush (dbglog);
}

static const char *
dbgch (int c)
{
  static char b[8];

  if (c == EOF)
    return "<EOF>";
  if (c >= 0x21 && c <= 0x7e)
    snprintf (b, sizeof b, "%c", c);
  else
    snprintf (b, sizeof b, "\\x%02x", (unsigned char)c);
  return b;
}

static BASHT_STREAM self;	/* task 0 */
static int self_master = -1;	/* master side of task 0's pty */

/* This process is the multiplexing shell, the one whose fds 1/2 are
   task 0's pty and whose forks become tasks. Cleared in every
   forked child (basht_child_stdio), because a subshell's own
   children are its internal plumbing, not tasks: a command
   substitution redirects fd 1 to its capture pipe, and a child
   captured onto a task pty there would send the substitution's
   output to the display and hand the caller an empty string. */
static int self_shell;

/* Captured tasks. Every job-table child (foreground or background;
   not comsub/procsub) gets an in, an out and an err pty; the
   masters are drained here and displayed as tagged lines. Pipes and
   redirections applied after fork override the slaves, so only
   terminal-bound streams are captured. */
#define BASHT_MAX_CAPS 64

struct basht_cap {
  int m_in, m_out, m_err;	/* master fds; -1 = closed */
  pid_t pid;
  int lines;			/* per-task displayed-line counter */
  BASHT_STREAM out, err;
  /* auto-window: a task that enables the alternate screen is moved
     into its own terminal window, bridged over a fifo pair */
  int    windowed;		/* output now shuttles to the window */
  int    w_was;			/* has been windowed: the console
				   keyboard relay stays released   */
  int    w_out, w_in;		/* fifos: to bridge / from bridge  */
  int    w_ws;			/* fifo: winsize reports, 4 bytes
				   each, at bridge startup and on
				   every window resize             */
  int    w_up;			/* first report seen: bridge is up */
  int    w_hinted;		/* windowing failed; hint printed  */
  time_t w_t0;			/* window spawned; bridge deadline */
  char   wcmd[24];		/* terminal launched, for messages */
  char   wdir[64];		/* mkdtemp dir holding the fifos   */
  size_t nfilt;			/* stdout bytes already filtered   */
  size_t nbytes;		/* total stdout bytes read         */
  unsigned char pre[2048];	/* raw stdout prefix, for replay   */
  size_t prelen;
  int    sess;			/* task owns its pty as controlling
				   terminal: signal keys go through
				   its line discipline, not killpg */
};
static struct basht_cap caps[BASHT_MAX_CAPS];

/* Task ids are per name: [hello:2] is the second hello started this
   session. Instance counts never reset, so an id always denotes the
   same task for the whole session. */
static struct {
  char name[BASHT_NAME_MAX];
  int n;
} insts[BASHT_MAX_CAPS];
static int insts_used;
static int overflow_id = 1000;	/* name table full: unique-ish ids */

static int
next_instance (const char *name)
{
  int i;

  for (i = 0; i < insts_used; i++)
    if (strcmp (insts[i].name, name) == 0)
      return ++insts[i].n;
  if (insts_used < BASHT_MAX_CAPS)
    {
      strcpy (insts[insts_used].name, name);
      insts[insts_used].n = 1;
      return insts[insts_used++].n;
    }
  return ++overflow_id;
}

/* pending capture between basht_fork_prepare() and the fork */
static int  pend_slot = -1;
static int  pend_solo;
static char pend_sin[64], pend_sout[64], pend_serr[64];

static int
open_master (char *sname, size_t cap)
{
  int m;
  char *p;

  m = posix_openpt (O_RDWR | O_NOCTTY);
  if (m < 0)
    return -1;
  if (grantpt (m) < 0 || unlockpt (m) < 0 || (p = ptsname (m)) == 0)
    {
      close (m);
      return -1;
    }
  strncpy (sname, p, cap - 1);
  sname[cap - 1] = '\0';
  fcntl (m, F_SETFD, FD_CLOEXEC);
  fcntl (m, F_SETFL, O_NONBLOCK);
  return m;
}

/* basename of the first word of COMMAND, for the tag */
static void
cap_name (const char *command, char *out, size_t cap)
{
  const char *p, *e, *b, *q;
  size_t n;

  p = command ? command : "";
  while (*p == ' ' || *p == '\t')
    p++;
  for (e = p; *e && *e != ' ' && *e != '\t'; e++)
    ;
  b = p;
  for (q = p; q < e; q++)
    if (*q == '/')
      b = q + 1;
  n = (size_t)(e - b);
  if (n == 0)
    {
      strncpy (out, "job", cap);
      out[cap - 1] = '\0';
      return;
    }
  if (n > cap - 1)
    n = cap - 1;
  memcpy (out, b, n);
  out[n] = '\0';
}

static void
cap_window_close (struct basht_cap *cp)
{
  char path[96];

  if (cp->w_out >= 0)
    close (cp->w_out);
  if (cp->w_in >= 0)
    close (cp->w_in);
  if (cp->w_ws >= 0)
    close (cp->w_ws);
  cp->w_out = cp->w_in = cp->w_ws = -1;
  cp->windowed = 0;
  if (cp->wdir[0])
    {
      snprintf (path, sizeof path, "%s/o", cp->wdir);
      unlink (path);
      snprintf (path, sizeof path, "%s/i", cp->wdir);
      unlink (path);
      snprintf (path, sizeof path, "%s/w", cp->wdir);
      unlink (path);
      rmdir (cp->wdir);
      cp->wdir[0] = '\0';
    }
}

static void
cap_free (struct basht_cap *cp)
{
  if (cp->m_in >= 0)
    close (cp->m_in);
  if (cp->m_out >= 0)
    close (cp->m_out);
  if (cp->m_err >= 0)
    close (cp->m_err);
  cp->m_in = cp->m_out = cp->m_err = -1;
  cap_window_close (cp);
  basht_display_stream_gone (&cp->out);
  basht_display_stream_gone (&cp->err);
  cp->out.id = 0;
  cp->pid = -1;
}

static struct basht_cap *
cap_by_pid (pid_t pid)
{
  int i;

  for (i = 0; i < BASHT_MAX_CAPS; i++)
    if (caps[i].pid == pid && caps[i].out.id != 0)
      return &caps[i];
  return 0;
}

/* Called in the parent just before fork. Decides whether this child
   is captured and allocates its ptys. Only async, job-table,
   interactive children of the multiplexing shell itself qualify --
   no comsub/procsub, and nothing forked by a subshell, whose fds
   are already the caller's plumbing. */
void
basht_fork_prepare (const char *command, int flags)
{
  int i, solo;
  struct basht_cap *cp;

  solo = basht_fork_solo;
  basht_fork_solo = 0;		/* consumed either way */
  if (pend_slot >= 0)		/* stale (fork never completed) */
    {
      cap_free (&caps[pend_slot]);
      pend_slot = -1;
    }
  if (basht_active == 0 || self_shell == 0
      || (flags & (FORK_COMSUB | FORK_PROCSUB)))
    return;

  for (i = 0; i < BASHT_MAX_CAPS; i++)
    if (caps[i].m_in < 0 && caps[i].m_out < 0 && caps[i].m_err < 0)
      break;
  if (i == BASHT_MAX_CAPS)
    return;			/* table full: run uncaptured */
  cp = &caps[i];

  memset (cp, 0, sizeof *cp);
  cp->pid = -1;
  cp->w_out = cp->w_in = cp->w_ws = -1;
  cp->m_in = open_master (pend_sin, sizeof pend_sin);
  cp->m_out = open_master (pend_sout, sizeof pend_sout);
  cp->m_err = open_master (pend_serr, sizeof pend_serr);
  if (cp->m_in < 0 || cp->m_out < 0 || cp->m_err < 0)
    {
      cap_free (cp);
      return;
    }

  /* a console task starts at the real terminal's size */
  {
    struct winsize ws;
    if (basht_tty >= 0 && ioctl (basht_tty, TIOCGWINSZ, &ws) == 0
	&& ws.ws_row > 0)
      {
	ioctl (cp->m_in, TIOCSWINSZ, &ws);
	ioctl (cp->m_out, TIOCSWINSZ, &ws);
	ioctl (cp->m_err, TIOCSWINSZ, &ws);
      }
  }

  cap_name (command, cp->out.name, sizeof cp->out.name);
  if (cp->out.name[0] == '(')	/* subshell command text */
    strcpy (cp->out.name, "sub");
  strcpy (cp->err.name, cp->out.name);
  cp->out.id = cp->err.id = next_instance (cp->out.name);
  cp->out.mark = 0;
  cp->err.mark = '!';
  cp->out.lines = cp->err.lines = &cp->lines;
  cp->sess = solo;		/* parent's copy of the decision */
  pend_solo = solo;		/* the child's copy, via fork */
  pend_slot = i;
}

/* jobs.c asks: is this child taking its own session? The parent
   half of the setpgid race dance must be skipped for it -- winning
   that race would make the child a process-group leader, and a
   group leader can never setsid. */
int
basht_sess_child (pid_t pid)
{
  struct basht_cap *cp = cap_by_pid (pid);

  return cp != 0 && cp->sess;
}

/* Called in the parent right after fork. PID < 0: fork failed, free
   the pending capture. PID > 0: capture is live. (In the child this
   is not called; basht_child_stdio consumes the pending state.) */
void
basht_fork_done (pid_t pid)
{
  if (pend_slot < 0 || pid == 0)
    return;
  if (pid < 0)
    cap_free (&caps[pend_slot]);
  else
    {
      caps[pend_slot].pid = pid;
      caps[pend_slot].out.pid = caps[pend_slot].err.pid = pid;
    }
  pend_slot = -1;
}

/* Read whatever the task-0 master has. Returns only when the pty is
   momentarily empty. */
static void
drain_self (void)
{
  unsigned char buf[4096];
  ssize_t n;

  for (;;)
    {
      fd_set rf;
      struct timeval tv;

      FD_ZERO (&rf);
      FD_SET (self_master, &rf);
      tv.tv_sec = tv.tv_usec = 0;
      if (select (self_master + 1, &rf, 0, 0, &tv) <= 0)
	break;
      n = read (self_master, buf, sizeof buf);
      if (n > 0)
	basht_filter_bytes (&self, buf, (size_t)n, 0);
      else if (n < 0 && (errno == EINTR || errno == EAGAIN))
	continue;
      else
	break;			/* EOF/EIO cannot happen: we hold the slave */
    }
}

/* Drain one capture master (non-blocking). EOF/EIO means the child
   side is gone: flush the partial line and close. */
static void
drain_cap_fd (int *fd, BASHT_STREAM *ts)
{
  unsigned char buf[4096];
  ssize_t n;

  while (*fd >= 0)
    {
      n = read (*fd, buf, sizeof buf);
      if (n > 0)
	{
	  basht_filter_bytes (ts, buf, (size_t)n, 0);
	  continue;
	}
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
	return;
      if (n < 0 && errno == EINTR)
	continue;
      basht_filter_flush (ts);	/* EOF or EIO */
      close (*fd);
      *fd = -1;
      return;
    }
}

static void fg_signal (pid_t, int);
static void basht_cycle_owner (int);
static pid_t fg_pid;		/* defined with the fg pump below */

/* The in pty's master carries the tty driver's echo of input basht
   writes and, for a sessioned task, whatever the task itself
   writes to /dev/tty (ssh and sudo prompts, getpass). Show it all
   through the task's stdout stream, except in canonical-with-ECHO
   mode: there it is the driver echoing a line the relay already
   displayed. Always drained either way, so the queue cannot back
   up. */
static void
drain_cap_in (struct basht_cap *cp)
{
  unsigned char buf[512];
  struct termios t;
  int show;
  ssize_t n;

  if (cp->m_in < 0)
    return;
  show = tcgetattr (cp->m_in, &t) == 0
	 && ((t.c_lflag & ICANON) == 0 || (t.c_lflag & ECHO) == 0);
  for (;;)
    {
      n = read (cp->m_in, buf, sizeof buf);
      if (n > 0)
	{
	  if (show)
	    basht_filter_bytes (&cp->out, buf, (size_t)n, 0);
	  continue;
	}
      if (n < 0 && errno == EINTR)
	continue;
      return;			/* EAGAIN; EOF/EIO is out/err's cue */
    }
}

/* ---- auto-window: full-screen tasks move to a terminal window ---

   When a task enables the alternate screen, basht stops filtering
   it, spawns a terminal window running this same binary in
   --basht-bridge mode, replays the task's small raw prefix (which
   ends at the trigger sequence: detection happens at the program's
   first breath), and from then on shuttles bytes between the
   task's ptys and the window over a fifo pair. Bytes the task
   writes while the window is starting simply wait in the kernel
   pty queue -- basht stops reading, and the backed-up pty is all
   the flow control needed. Should the window never report in (the
   terminal emulator missing or failing), the task falls back to
   the tagged console with nothing lost. */

static char self_exe[256];

/* keyboard routing, defined with the foreground pump below */
static int  cap_stdin_raw (struct basht_cap *, int *);
static void relay_raw (struct basht_cap *, const char *, ssize_t, int,
		       int);

/* Is NAME an executable somewhere on PATH? */
static int
path_has (const char *name)
{
  const char *p = getenv ("PATH");
  char buf[512];
  size_t len, n = strlen (name);

  if (p == 0)
    p = "/usr/local/bin:/usr/bin:/bin";
  while (*p)
    {
      const char *e = strchr (p, ':');
      len = e ? (size_t)(e - p) : strlen (p);
      if (len > 0 && len + n + 2 < sizeof buf)
	{
	  memcpy (buf, p, len);
	  buf[len] = '/';
	  strcpy (buf + len + 1, name);
	  if (access (buf, X_OK) == 0)
	    return 1;
	}
      p += len;
      if (*p == ':')
	p++;
    }
  return 0;
}

/* The command that opens a terminal window: $BASHT_TERMINAL, or the
   first emulator found on PATH -- each candidate with the flags that
   make it run the appended bridge invocation. Returns 0 when there
   is no way to open a window here. */
static const char *
term_command (void)
{
  static const char *cand[] = {
    "gnome-terminal --", "ptyxis --", "konsole -e",
    "xfce4-terminal -x", "kitty", "alacritty -e", "foot",
    "wezterm start --", "xterm -e", 0
  };
  const char *env = getenv ("BASHT_TERMINAL");
  char name[64];
  int i, k;

  if (env && *env)
    return env;
  if (getenv ("DISPLAY") == 0 && getenv ("WAYLAND_DISPLAY") == 0)
    return 0;			/* no GUI to open a window on */
  for (i = 0; cand[i]; i++)
    {
      for (k = 0; cand[i][k] && cand[i][k] != ' '
	   && k < (int)sizeof name - 1; k++)
	name[k] = cand[i][k];
      name[k] = '\0';
      if (path_has (name))
	return cand[i];
    }
  return 0;
}

/* `term' builtin: open a terminal window running ARGV. Shares
   term_command with the auto-window, so an explicit `term' and a
   task that went full-screen always land in the same emulator.

   The window is detached: setsid in the child gives it its own
   session, so it does not take ^C aimed at the shell's foreground
   process group and does not die with the shell's terminal. Its
   stdio is inherited, which under a live multiplexer means the
   emulator's own diagnostics arrive tagged as task 0's output.
   Nothing waits for it -- bash's own reaper collects the child
   when the emulator exits. */
pid_t
basht_term_spawn (char **argv)
{
  char termbuf[256];
  char *targv[128];
  const int maxargv = (int)(sizeof targv / sizeof targv[0]) - 1;
  int ntargv = 0, i;
  const char *termcmd;
  char *tok;
  pid_t pid;

  termcmd = term_command ();
  if (termcmd == 0)
    return BASHT_TERM_NOTERM;

  strncpy (termbuf, termcmd, sizeof termbuf - 1);
  termbuf[sizeof termbuf - 1] = '\0';
  for (tok = strtok (termbuf, " \t"); tok; tok = strtok (0, " \t"))
    {
      if (ntargv >= 16)
	return BASHT_TERM_TOOMANY;
      targv[ntargv++] = tok;
    }
  if (ntargv == 0)
    return BASHT_TERM_NOTERM;

  /* $BASHT_TERMINAL is taken verbatim by term_command, and the
     PATH probe only ever checked the candidates: make sure this
     one is really runnable before forking, so a typo is an error
     here rather than a window that never appears. */
  if (strchr (targv[0], '/')
      ? access (targv[0], X_OK) != 0
      : path_has (targv[0]) == 0)
    return BASHT_TERM_NOTERM;

  for (i = 0; argv && argv[i]; i++)
    {
      if (ntargv >= maxargv)
	return BASHT_TERM_TOOMANY;
      targv[ntargv++] = argv[i];
    }
  targv[ntargv] = 0;

  pid = fork ();
  if (pid < 0)
    return BASHT_TERM_FORK;
  if (pid == 0)
    {
      setsid ();
      execvp (targv[0], targv);
      _exit (127);
    }
  dbg ("term: %s -> pid %d", targv[0], (int)pid);
  return pid;
}

/* Replay the prefix minus terminal queries (CSI ... n/c): the new
   terminal would answer them and the answers would arrive as
   phantom keystrokes. */
static size_t
scrub_queries (const unsigned char *in, size_t n, unsigned char *out)
{
  size_t i = 0, o = 0;

  while (i < n)
    {
      if (in[i] == 0x1b && i + 1 < n && in[i + 1] == '[')
	{
	  size_t j = i + 2;
	  while (j < n && !(in[j] >= 0x40 && in[j] <= 0x7e))
	    j++;
	  if (j < n && (in[j] == 'n' || in[j] == 'c'))
	    {
	      i = j + 1;	/* drop the query */
	      continue;
	    }
	  /* copy the whole sequence (or partial tail) */
	  if (j < n)
	    j++;
	  memcpy (out + o, in + i, j - i);
	  o += j - i;
	  i = j;
	  continue;
	}
      out[o++] = in[i++];
    }
  return o;
}

static int
window_try (struct basht_cap *cp)
{
  char fo[96], fi[96], fw[96];
  char termbuf[256];
  char *targv[16];
  int ntargv, k;
  pid_t wpid;
  const char *termcmd;

  if (cp->windowed || cp->w_hinted || cp->pid <= 0)
    return 0;
  /* only at the task's first breath: prefix complete, nothing
     displayed yet -- outside that, restarting or attaching would
     lose state, so just hint */
  if (cp->nbytes > sizeof cp->pre || cp->lines != 0)
    return 0;
  termcmd = term_command ();
  if (termcmd == 0)
    return 0;

  strcpy (cp->wdir, "/tmp/basht.XXXXXX");
  if (mkdtemp (cp->wdir) == 0)
    {
      cp->wdir[0] = '\0';
      return 0;
    }
  snprintf (fo, sizeof fo, "%s/o", cp->wdir);
  snprintf (fi, sizeof fi, "%s/i", cp->wdir);
  snprintf (fw, sizeof fw, "%s/w", cp->wdir);
  if (mkfifo (fo, 0600) < 0 || mkfifo (fi, 0600) < 0
      || mkfifo (fw, 0600) < 0)
    {
      cap_window_close (cp);
      return 0;
    }
  /* O_RDWR on the outbound fifo: never blocks at open, and since we
     hold a reader it can never raise SIGPIPE at us; the bridge
     still sees EOF when we close it. Inbound (keyboard) and winsize
     are plain nonblocking readers. */
  cp->w_out = open (fo, O_RDWR | O_NONBLOCK);
  cp->w_in = open (fi, O_RDONLY | O_NONBLOCK);
  cp->w_ws = open (fw, O_RDONLY | O_NONBLOCK);
  if (cp->w_out < 0 || cp->w_in < 0 || cp->w_ws < 0)
    {
      cap_window_close (cp);
      return 0;
    }
  fcntl (cp->w_out, F_SETFD, FD_CLOEXEC);
  fcntl (cp->w_in, F_SETFD, FD_CLOEXEC);
  fcntl (cp->w_ws, F_SETFD, FD_CLOEXEC);

  /* terminal command: split on blanks, append bridge invocation */
  strncpy (termbuf, termcmd, sizeof termbuf - 1);
  termbuf[sizeof termbuf - 1] = '\0';
  ntargv = 0;
  for (char *tok = strtok (termbuf, " \t"); tok && ntargv < 11;
       tok = strtok (0, " \t"))
    targv[ntargv++] = tok;
  targv[ntargv++] = self_exe;
  targv[ntargv++] = (char *)"--basht-bridge";
  targv[ntargv++] = fo;
  targv[ntargv++] = fi;
  targv[ntargv++] = fw;
  targv[ntargv] = 0;

  wpid = fork ();
  if (wpid < 0)
    {
      cap_window_close (cp);
      return 0;
    }
  if (wpid == 0)
    {
      execvp (targv[0], targv);
      _exit (127);
    }

  /* the launch is a wager until the bridge's first winsize report:
     nothing is replayed or announced yet, and the task's output
     waits in the kernel pty queue -- shuttle_windowed either
     completes the move on that report or gives the console back
     when the deadline passes */
  for (k = 0; termcmd[k] && termcmd[k] != ' '
       && k < (int)sizeof cp->wcmd - 1; k++)
    cp->wcmd[k] = termcmd[k];
  cp->wcmd[k] = '\0';
  cp->w_t0 = time (0);
  cp->windowed = 1;
  return 1;
}

/* Shuttle for a windowed task: window keyboard -> task stdin,
   winsize reports -> TIOCSWINSZ + SIGWINCH (at bridge startup and
   on every window resize), task stdout/stderr -> window, gated on
   fifo room so a stalled window backs up into the kernel pty queue
   instead of into basht. */
static void
shuttle_windowed (struct basht_cap *cp)
{
  unsigned char buf[2048];
  ssize_t n;

  /* winsize reports: 4 bytes each, latest wins; the first one is
     also the sign the bridge is up -- only then is the prefix
     replayed and the move announced */
  while (cp->w_ws >= 0)
    {
      unsigned char wsb[4];
      n = read (cp->w_ws, wsb, sizeof wsb);
      if (n == 4)
	{
	  struct winsize ws;
	  memset (&ws, 0, sizeof ws);
	  ws.ws_row = (wsb[0] << 8) | wsb[1];
	  ws.ws_col = (wsb[2] << 8) | wsb[3];
	  if (ws.ws_row > 0 && ws.ws_col > 0)
	    {
	      ioctl (cp->m_in, TIOCSWINSZ, &ws);
	      ioctl (cp->m_out, TIOCSWINSZ, &ws);
	      ioctl (cp->m_err, TIOCSWINSZ, &ws);
	      fg_signal (cp->pid, SIGWINCH);
	    }
	  if (cp->w_up == 0)
	    {
	      /* replay the prefix (it ends at the trigger); the
		 window's terminal interprets it natively */
	      unsigned char rep[sizeof cp->pre];
	      size_t replen = scrub_queries (cp->pre, cp->prelen, rep);
	      basht_write_all (cp->w_out, rep, replen);
	      cp->w_up = 1;
	      cp->w_was = 1;
	      basht_display_event (&cp->out,
				   "full screen: moved to a window");
	    }
	  continue;
	}
      if (n < 0 && errno == EINTR)
	continue;
      break;			/* EAGAIN, EOF, or short read */
    }

  /* no report by the deadline: the terminal failed to launch (not
     installed, exec error, no display server). Give the console
     back -- everything the task wrote is still in the prefix and
     the kernel pty queue, so nothing is lost; the unfiltered tail
     of the prefix goes through the filter now. */
  if (cp->w_up == 0 && time (0) - cp->w_t0 >= 5)
    {
      size_t k = cp->nfilt;
      char cmd[sizeof cp->wcmd];

      strcpy (cmd, cp->wcmd);
      cap_window_close (cp);
      cp->w_hinted = 1;		/* don't wager on this terminal again */
      basht_display_event (&cp->out,
	  "window failed (%s); staying on the console -- "
	  "set BASHT_TERMINAL", cmd);
      if (k < cp->prelen)
	basht_filter_bytes (&cp->out, cp->pre + k, cp->prelen - k, 0);
      return;
    }

  /* keyboard from the bridge: onto whichever pty the task reads
     keys from; the bridge's terminal runs with ISIG off, so signal
     keys arrive as bytes and are routed as the console's are */
  while (cp->w_in >= 0)
    {
      n = read (cp->w_in, buf, sizeof buf);
      if (n > 0)
	{
	  int isig;
	  cap_stdin_raw (cp, &isig);
	  relay_raw (cp, (const char *)buf, n, isig, 0);
	  continue;
	}
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
	break;
      if (n < 0 && errno == EINTR)
	continue;
      if (n == 0 && cp->w_up == 0)
	break;			/* bridge not connected yet */
      /* window closed: hang up the task; if it survives, its
	 output returns to the tagged console */
      fg_signal (cp->pid, SIGHUP);
      cap_window_close (cp);
      basht_display_event (&cp->out, "window closed");
      return;
    }

  /* task output to the window, only as fast as the fifo drains; not
     at all until the bridge is up (the prefix must land first) */
  for (int which = 0; cp->w_up && which < 2; which++)
    {
      int *fd = which ? &cp->m_err : &cp->m_out;
      while (*fd >= 0 && cp->w_out >= 0)
	{
	  fd_set wf;
	  struct timeval tv;
	  FD_ZERO (&wf);
	  FD_SET (cp->w_out, &wf);
	  tv.tv_sec = tv.tv_usec = 0;
	  if (select (cp->w_out + 1, 0, &wf, 0, &tv) <= 0)
	    break;		/* fifo full: kernel pty holds it */
	  n = read (*fd, buf, sizeof buf);
	  if (n > 0)
	    {
	      basht_write_all (cp->w_out, buf, (size_t)n);
	      continue;
	    }
	  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
	    break;
	  if (n < 0 && errno == EINTR)
	    continue;
	  close (*fd);		/* task side gone */
	  *fd = -1;
	}
    }
}

/* stdout drain with full-screen detection: raw bytes are copied
   into the replay prefix before filtering, so at the moment of
   detection the prefix holds everything the task ever wrote,
   trigger included. */
static void
drain_cap_out (struct basht_cap *cp)
{
  unsigned char buf[4096];
  ssize_t n;

  while (cp->m_out >= 0 && cp->windowed == 0)
    {
      n = read (cp->m_out, buf, sizeof buf);
      if (n > 0)
	{
	  size_t done = 0;
	  if (cp->prelen < sizeof cp->pre)
	    {
	      size_t k = sizeof cp->pre - cp->prelen;
	      if (k > (size_t)n)
		k = (size_t)n;
	      memcpy (cp->pre + cp->prelen, buf, k);
	      cp->prelen += k;
	    }
	  cp->nbytes += (size_t)n;
	  while (done < (size_t)n)
	    {
	      int trig = 0;
	      size_t took = basht_filter_bytes (&cp->out, buf + done,
						(size_t)n - done, &trig);
	      done += took;
	      cp->nfilt += took;
	      if (trig && window_try (cp))
		return;		/* rest of chunk was in the prefix */
	      if (trig && cp->w_hinted == 0)
		{
		  const char *tc = term_command ();
		  cp->w_hinted = 1;
		  if (tc)
		    basht_display_event (&cp->out,
			"full-screen program; try: %s %s",
			tc, cp->out.name);
		  else
		    basht_display_event (&cp->out,
			"full-screen program; set BASHT_TERMINAL "
			"to move it to a window");
		}
	    }
	  continue;
	}
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
	return;
      if (n < 0 && errno == EINTR)
	continue;
      basht_filter_flush (&cp->out);
      close (cp->m_out);
      cp->m_out = -1;
      return;
    }
}

static void
drain_caps (void)
{
  int i;

  for (i = 0; i < BASHT_MAX_CAPS; i++)
    {
      struct basht_cap *cp = &caps[i];
      if (cp->out.id == 0)
	continue;
      if (cp->windowed)
	shuttle_windowed (cp);
      if (cp->windowed == 0)
	{
	  drain_cap_out (cp);
	  if (cp->windowed)	/* moved just now */
	    continue;
	  drain_cap_fd (&cp->m_err, &cp->err);
	  drain_cap_in (cp);
	}
      /* both output streams gone: the task is over; release */
      if (cp->m_out < 0 && cp->m_err < 0 && cp->m_in >= 0)
	cap_free (cp);
    }
}

void
basht_drain (void)
{
  if (basht_active == 0)
    return;
  /* PST1 -- the tag template -- is an ordinary shell variable,
     re-read like the prompt strings are: a change shows on the
     next line displayed */
  basht_display_set_tagfmt (get_string_value ("PST1"));

  /* real-terminal resize: mirror onto the self pty and every
     console task's trio; the foreground task also gets the
     SIGWINCH the tty driver would have sent it (windowed tasks
     size from their own window instead) */
  {
    static struct winsize last;
    struct winsize ws;

    if (ioctl (basht_tty, TIOCGWINSZ, &ws) == 0
	&& ws.ws_row > 0 && ws.ws_col > 0
	&& (ws.ws_row != last.ws_row || ws.ws_col != last.ws_col))
      {
	last = ws;
	if (self_master >= 0)
	  ioctl (self_master, TIOCSWINSZ, &ws);
	for (int i = 0; i < BASHT_MAX_CAPS; i++)
	  {
	    struct basht_cap *cp = &caps[i];
	    if (cp->out.id == 0 || cp->windowed || cp->w_was)
	      continue;
	    if (cp->m_in >= 0)
	      ioctl (cp->m_in, TIOCSWINSZ, &ws);
	    if (cp->m_out >= 0)
	      ioctl (cp->m_out, TIOCSWINSZ, &ws);
	    if (cp->m_err >= 0)
	      ioctl (cp->m_err, TIOCSWINSZ, &ws);
	    if (cp->pid > 0 && cp->pid == fg_pid)
	      fg_signal (cp->pid, SIGWINCH);
	  }
      }
  }

  fflush (stdout);
  fflush (stderr);
  drain_self ();
  drain_caps ();
  basht_display_sync ();
}

/* A command line was accepted: display its echo, then hand the
   console over (serial semantics — no shell entry line is drawn
   while a command runs; basht_getc takes it back at next prompt). */
void
basht_command_begin (void)
{
  if (basht_active == 0)
    return;
  basht_drain ();
  basht_display_set_default (0);
  basht_display_sync ();
  dbg ("command_begin (line accepted)");
}

/* ---- phase 3: the foreground pump --------------------------------

   Called from wait_for's wait loop in place of blocking in
   waitpid. Keeps every pty draining while a foreground job runs,
   and relays the real keyboard to the job: the terminal goes raw
   (ISIG off), typed characters build the job's pending input line
   under the job's own tag, Enter ships it to the job's in
   pty, ^C/^Z become killpg(SIGINT/SIGTSTP) on the job's process
   group, ^D sends VEOF. Returns 1 if it pumped (caller should then
   reap with WNOHANG), 0 to fall back to a blocking wait. */

static struct termios fg_tio_save;
static int   fg_raw = 0;
static int   fg_stdin_open = 1;
static pid_t fg_pid = -1;
static BASHT_STREAM fg_rly;

/* Manual keyboard selection (alt-arrows) during a foreground
   episode: 0 = the foreground task (the default), &self = the
   shell (keystrokes become readline type-ahead for the next
   prompt), else a background task's stream. Validated against the
   caps table on every use; reset at episode boundaries. */
static const BASHT_STREAM *manual_sel;
static pid_t manual_sel_pid;
static struct basht_linebuf fg_ta_esc;	/* type-ahead escape state */
static BASHT_STREAM ta_rly;	/* the shell's visible type-ahead
				   line: typed while the shell is
				   selected during a foreground
				   episode, queued to readline on
				   Enter (and at episode end) */
static int ta_hist = -1;	/* history browse cursor; -1 = the
				   line being typed */
static char ta_stash[BASHT_LINEBUF_CAP];
static size_t ta_stash_len;	/* typed line saved while browsing */

static void
ta_set (const char *s, size_t len)
{
  if (len > BASHT_LINEBUF_CAP - 2)
    len = BASHT_LINEBUF_CAP - 2;
  memcpy (ta_rly.lb.data, s, len);
  ta_rly.lb.len = ta_rly.lb.cur = len;
  basht_display_partial (&ta_rly);
}

/* Up/Down on the shell's type-ahead line browse the shell history,
   the way the prompt would; readline itself is not running while
   the shell waits on a foreground job. */
static void
ta_history (int dir)
{
  HIST_ENTRY **hl = history_list ();

  if (hl == 0 || history_length == 0)
    return;
  if (ta_hist < 0)
    {
      if (dir > 0)
	return;			/* nothing below the current line */
      memcpy (ta_stash, ta_rly.lb.data, ta_rly.lb.len);
      ta_stash_len = ta_rly.lb.len;
      ta_hist = history_length - 1;
    }
  else if (dir < 0)
    {
      if (ta_hist > 0)
	ta_hist--;
    }
  else if (++ta_hist >= history_length)
    {
      ta_hist = -1;		/* below the list: the typed line */
      ta_set (ta_stash, ta_stash_len);
      return;
    }
  ta_set (hl[ta_hist]->line, strlen (hl[ta_hist]->line));
}

/* the prompt-time relay, defined with its router further down */
static BASHT_STREAM prompt_rly;
static int rslot;
static void prompt_relay_bind (struct basht_cap *);
static int  cap_key_fd (struct basht_cap *);
static int  key_signals_here (struct basht_cap *, int);

static void
fg_signal (pid_t pid, int sig)
{
  pid_t pg = getpgid (pid);

  dbg ("fg_signal sig=%d pid=%d pg=%d (self pg=%d)",
       sig, (int)pid, (int)pg, (int)getpgrp ());
  if (pg > 0)
    killpg (pg, sig);
  else
    kill (pid, sig);
}

/* Starting a relay line: seed it with the task's own partial (the
   prompt), so the input line reads "enter a line: typed text" just
   as echo on a shared terminal would. The prefix is display-only:
   never shipped, never backspaced over. */
static void
relay_prefix (struct basht_cap *cp, BASHT_STREAM *rly)
{
  const struct basht_linebuf *src = 0;
  size_t n;

  if (rly->lb.len > 0 || rly->lb.fix > 0)
    return;			/* line already started */
  if (cp->out.lb.len > 0)
    src = &cp->out.lb;
  else if (cp->err.lb.len > 0)
    src = &cp->err.lb;
  if (src == 0)
    return;
  n = src->len;
  if (n > BASHT_LINEBUF_CAP / 2)	/* keep room for typing */
    n = BASHT_LINEBUF_CAP / 2;
  memcpy (rly->lb.data, src->data, n);
  rly->lb.len = rly->lb.cur = rly->lb.fix = n;
}

/* Raw keyboard bytes become relay input for CP: printables build
   RLY's pending line, displayed under the task's own tag (same
   width, so typing never shifts the line) as "[name:N] prompt
   typed-text". Enter ships the typed part to the task's in pty;
   the record that scrolls up is the task's line as a shared
   terminal would have shown it -- prompt plus echoed input, ended
   by Enter's echo -- so the task's pending line is completed by it
   and the task's next output starts fresh. Backspace edits, ^C/^Z
   signal the task's process group, ^D forwards VEOF. Escape
   sequences are swallowed, never typed into the line; when CYCLES
   is set (prompt-time relay) alt-left/right (CSI 1;3 D/C) cycle
   the console between the inputting tasks. Shared by the
   foreground pump and the prompt-time relay. */
/* Escape-sequence scanner shared by every keyboard sink: sequences
   are swallowed, never typed into a line; alt-left/right (CSI 1;3
   D/C) cycle the console when CYCLES is set. ST holds the parse
   state (only fs and the csi fields are used; BF_OSC is borrowed
   to mean "SS3 pending" -- relay buffers never meet the output
   filter, so there is no clash). Returns KESC_NONE for an
   ordinary byte, KESC_EAT for sequence bytes, KESC_UP/DOWN for a
   plain up/down arrow (CSI/SS3 A/B), which the caller may act on. */
#define KESC_NONE 0
#define KESC_EAT  1
#define KESC_UP   2
#define KESC_DOWN 3

static int
key_escape (struct basht_linebuf *st, unsigned char c, int cycles)
{
  if (st->fs == BF_ESC)
    {
      if (c == '[')
	{
	  st->fs = BF_CSI;
	  st->csi_n = st->csi_n2 = 0;
	  st->csi_more = 0;
	}
      else if (c == 'O')
	st->fs = BF_OSC;	/* SS3: one key byte follows */
      else
	st->fs = BF_NORMAL;	/* ESC x: swallow both */
      return KESC_EAT;
    }
  if (st->fs == BF_OSC)
    {
      st->fs = BF_NORMAL;
      if (c == 'A')
	return KESC_UP;
      if (c == 'B')
	return KESC_DOWN;
      return KESC_EAT;
    }
  if (st->fs == BF_CSI)
    {
      if (c >= '0' && c <= '9')
	{
	  if (st->csi_more)
	    st->csi_n2 = st->csi_n2 * 10 + (c - '0');
	  else
	    st->csi_n = st->csi_n * 10 + (c - '0');
	}
      else if (c == ';')
	st->csi_more = 1;
      else if (c >= 0x40 && c <= 0x7e)
	{
	  st->fs = BF_NORMAL;
	  if (cycles && st->csi_n == 1 && st->csi_n2 == 3
	      && (c == 'C' || c == 'D'))
	    {
	      basht_cycle_owner (c == 'C' ? 1 : -1);
	      return KESC_EAT;
	    }
	  if (st->csi_n == 0 && st->csi_more == 0)
	    {
	      if (c == 'A')
		return KESC_UP;
	      if (c == 'B')
		return KESC_DOWN;
	    }
	}
      return KESC_EAT;
    }
  if (c == 0x1b)
    {
      st->fs = BF_ESC;
      return KESC_EAT;
    }
  return KESC_NONE;
}

static void
relay_chars (struct basht_cap *cp, BASHT_STREAM *rly,
	     const char *buf, ssize_t n, int cycles)
{
  int fd = cap_key_fd (cp);

  for (ssize_t k = 0; k < n; k++)
    {
      unsigned char c = (unsigned char)buf[k];
      if (key_escape (&rly->lb, c, cycles))
	continue;
      if (c == 0x03 || c == 0x1a)	/* ^C, ^Z */
	{
	  if (key_signals_here (cp, fd))
	    fg_signal (cp->pid, c == 0x03 ? SIGINT : SIGTSTP);
	  else
	    basht_write_all (fd, &c, 1);
	}
      else if (c == 0x04)		/* ^D */
	{
	  char e = 0x04;
	  if (fd >= 0)
	    basht_write_all (fd, &e, 1);
	}
      else if (c == '\r' || c == '\n')
	{
	  relay_prefix (cp, rly);
	  basht_display_line (rly, rly->lb.data, rly->lb.len);
	  if (fd >= 0)
	    {
	      rly->lb.data[rly->lb.len] = '\n';
	      basht_write_all (fd, rly->lb.data + rly->lb.fix,
			       rly->lb.len - rly->lb.fix + 1);
	    }
	  rly->lb.len = rly->lb.cur = rly->lb.fix = 0;
	  basht_display_partial (rly);
	  /* Enter's echo ends the task's pending line on a shared
	     terminal, and the record just displayed IS that line:
	     complete it, so the task's next output starts fresh */
	  if (cp->out.lb.len > 0)
	    {
	      cp->out.lb.len = cp->out.lb.cur = 0;
	      basht_display_partial (&cp->out);
	    }
	  else if (cp->err.lb.len > 0)
	    {
	      cp->err.lb.len = cp->err.lb.cur = 0;
	      basht_display_partial (&cp->err);
	    }
	}
      else if (c == 0x7f || c == '\b')
	{
	  if (rly->lb.len > rly->lb.fix)
	    rly->lb.cur = --rly->lb.len;
	  basht_display_partial (rly);
	}
      else if (c == '\t' || c >= 0x20)
	{
	  dbg ("relay %s", dbgch (c));
	  relay_prefix (cp, rly);
	  if (rly->lb.len < BASHT_LINEBUF_CAP - 2)
	    {
	      rly->lb.data[rly->lb.len++] = (char)c;
	      rly->lb.cur = rly->lb.len;
	    }
	  basht_display_partial (rly);
	}
      /* other control characters are dropped */
    }
}

/* The pty the task takes its keyboard from. Normally the in pty;
   but a task that has put its err (or out) pty into non-canonical
   mode while the in pty stays canonical is reading keys THERE:
   less (since 581) opens ttyname(2) rather than /dev/tty, and
   older pagers fall back to fd 2 when /dev/tty fails -- on a real
   terminal all three are one device, under basht they are three
   ptys, and the keystrokes must land on the one being read (man
   froze: q went to the in pty, less listened on the err pty). A
   pty pair shares one termios, so the master sees the slave's
   settings. -1 when the task has no pty left to type at. */
static int
cap_key_fd (struct basht_cap *cp)
{
  struct termios t;
  int fds[3], i;

  fds[0] = cp->m_in;
  fds[1] = cp->m_err;
  fds[2] = cp->m_out;
  for (i = 0; i < 3; i++)
    if (fds[i] >= 0 && tcgetattr (fds[i], &t) == 0
	&& (t.c_lflag & ICANON) == 0)
      return fds[i];
  return cp->m_in;
}

/* The task's keyboard discipline, read from its keyboard pty.
   Nonzero = non-canonical: the task reads keystrokes, not lines --
   readline programs (nested shells, ssh, REPLs) and pagers live
   here. ISIG is reported so signal keys can be handled as the tty
   driver would have. */
static int
cap_stdin_raw (struct basht_cap *cp, int *isig)
{
  struct termios t;
  int fd = cap_key_fd (cp);

  if (fd < 0 || tcgetattr (fd, &t) < 0)
    {
      if (isig)
	*isig = 1;
      return 0;
    }
  if (isig)
    *isig = (t.c_lflag & ISIG) != 0;
  return (t.c_lflag & ICANON) == 0;
}

/* Where a signal key typed at CP goes. The in pty is the task's
   controlling terminal when it is sessioned, and its line
   discipline delivers ^C/^Z to ITS foreground group (a nested
   shell's running job, not the shell); any other keyboard pty is
   not a controlling terminal -- ISIG there would signal an empty
   foreground group -- and unsessioned tasks have none, so those
   get killpg, as the real terminal's driver would have. Nonzero =
   emulate with killpg. */
static int
key_signals_here (struct basht_cap *cp, int fd)
{
  return cp->sess == 0 || fd < 0 || fd != cp->m_in;
}

/* Raw pass-through (issue #9): the task asked for keystrokes, so
   it gets them unedited -- escape sequences included; its own echo
   paints the input line, through the display machinery that
   already shows its partials. Only basht's console keys
   (alt-left/right, stripped from the burst; CYCLES clear leaves
   them alone -- a window's keyboard has no console to cycle) are
   withheld. While the slave keeps ISIG, ^C/^Z act on the process
   group exactly as the tty driver would; with ISIG off they
   forward as bytes. */
static void
relay_raw (struct basht_cap *cp, const char *buf, ssize_t n, int isig,
	   int cycles)
{
  ssize_t k = 0, mark = 0;
  int fd = cap_key_fd (cp);
  int emul = isig && key_signals_here (cp, fd);

  while (k < n)
    {
      unsigned char c = (unsigned char)buf[k];
      if (cycles && c == 0x1b && k + 5 < n && buf[k + 1] == '['
	  && buf[k + 2] == '1' && buf[k + 3] == ';' && buf[k + 4] == '3'
	  && (buf[k + 5] == 'C' || buf[k + 5] == 'D'))
	{
	  if (k > mark && fd >= 0)
	    basht_write_all (fd, buf + mark, (size_t)(k - mark));
	  basht_cycle_owner (buf[k + 5] == 'C' ? 1 : -1);
	  k += 6;
	  mark = k;
	  continue;
	}
      if (emul && (c == 0x03 || c == 0x1a))
	{
	  if (k > mark && fd >= 0)
	    basht_write_all (fd, buf + mark, (size_t)(k - mark));
	  fg_signal (cp->pid, c == 0x03 ? SIGINT : SIGTSTP);
	  k += 1;
	  mark = k;
	  continue;
	}
      k++;
    }
  if (k > mark && fd >= 0)
    basht_write_all (fd, buf + mark, (size_t)(k - mark));
}

/* Keyboard target during a foreground episode: the foreground cap
   CP unless a manual selection overrides it and is still valid.
   Returns 0 for the shell (type-ahead). */
static struct basht_cap *
fg_target (struct basht_cap *cp)
{
  int i;

  if (manual_sel == 0)
    return cp;
  if (manual_sel == &self)
    return 0;
  for (i = 0; i < BASHT_MAX_CAPS; i++)
    {
      struct basht_cap *bp = &caps[i];
      if ((manual_sel == &bp->out || manual_sel == &bp->err)
	  && bp->pid == manual_sel_pid && bp->out.id != 0
	  && bp->m_in >= 0
	  && (bp == cp || bp->out.lb.len > 0 || bp->err.lb.len > 0))
	return bp;
    }
  manual_sel = 0;		/* the selected task went away */
  return cp;
}

int
basht_fg_pump (pid_t pid)
{
  struct basht_cap *cp;
  fd_set rf;
  struct timeval tv;
  int maxfd, r, i;

  if (basht_active == 0)
    return 0;

  cp = pid > 0 ? cap_by_pid (pid) : 0;

  /* a windowed (or once-windowed) foreground task gets its keyboard
     from the window, never the console: stand down the relay
     (typed-ahead chars become the next prompt's input as usual) */
  if (cp && (cp->windowed || cp->w_was))
    {
      if (fg_raw && fg_pid == pid)
	{
	  tcsetattr (0, TCSANOW, &fg_tio_save);
	  fg_raw = 0;
	  fg_pid = -1;
	  fg_rly.lb.len = fg_rly.lb.cur = 0;
	  basht_display_stream_gone (&fg_rly);
	}
      cp = 0;
    }

  /* new foreground episode with a captured job: go raw and relay */
  if (cp && cp->m_in >= 0 && fg_pid != pid)
    {
      if (fg_raw == 0 && isatty (0) && tcgetattr (0, &fg_tio_save) == 0)
	{
	  struct termios t = fg_tio_save;
	  t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
	  t.c_cc[VMIN] = 1;
	  t.c_cc[VTIME] = 0;
	  if (tcsetattr (0, TCSANOW, &t) == 0)
	    fg_raw = 1;
	}
      dbg ("fg episode start pid=%d", (int)pid);
      fg_pid = pid;
      fg_stdin_open = 1;
      manual_sel = 0;
      fg_ta_esc.fs = BF_NORMAL;
      ta_hist = -1;
      memset (&ta_rly, 0, sizeof ta_rly);
      strcpy (ta_rly.name, "bash");
      ta_rly.pid = self.pid;
      ta_rly.lines = self.lines;
      memset (&fg_rly, 0, sizeof fg_rly);
      strcpy (fg_rly.name, cp->out.name);
      fg_rly.id = cp->out.id;
      fg_rly.pid = cp->pid;
      fg_rly.lines = cp->out.lines;
    }

  basht_drain ();

  FD_ZERO (&rf);
  maxfd = -1;
  if (fg_raw && fg_stdin_open)
    {
      FD_SET (0, &rf);
      maxfd = 0;
    }
  if (self_master >= 0)
    {
      FD_SET (self_master, &rf);
      if (self_master > maxfd)
	maxfd = self_master;
    }
  for (i = 0; i < BASHT_MAX_CAPS; i++)
    {
      if (caps[i].m_out >= 0)
	{
	  FD_SET (caps[i].m_out, &rf);
	  if (caps[i].m_out > maxfd)
	    maxfd = caps[i].m_out;
	}
      if (caps[i].m_err >= 0)
	{
	  FD_SET (caps[i].m_err, &rf);
	  if (caps[i].m_err > maxfd)
	    maxfd = caps[i].m_err;
	}
      if (caps[i].windowed && caps[i].w_in >= 0)
	{
	  FD_SET (caps[i].w_in, &rf);
	  if (caps[i].w_in > maxfd)
	    maxfd = caps[i].w_in;
	}
    }
  tv.tv_sec = 0;
  tv.tv_usec = 200000;
  r = select (maxfd + 1, &rf, 0, 0, &tv);

  if (r > 0 && fg_raw && fg_stdin_open && FD_ISSET (0, &rf) && cp)
    {
      char buf[256];
      ssize_t n = read (0, buf, sizeof buf);
      if (n == 0)
	{
	  char e = 0x04;
	  int fd = cap_key_fd (cp);
	  if (fd >= 0)
	    basht_write_all (fd, &e, 1);
	  fg_stdin_open = 0;
	}
      else if (n > 0)
	{
	  struct basht_cap *tcp = fg_target (cp);
	  int isig;
	  if (tcp == cp)
	    {
	      if (cap_stdin_raw (cp, &isig))
		{
		  /* line-mode text typed before the task went raw
		     belongs to the raw reader */
		  int fd = cap_key_fd (cp);
		  if (fg_rly.lb.len > fg_rly.lb.fix && fd >= 0)
		    basht_write_all (fd,
				     fg_rly.lb.data + fg_rly.lb.fix,
				     fg_rly.lb.len - fg_rly.lb.fix);
		  if (fg_rly.lb.len > 0)
		    {
		      fg_rly.lb.len = fg_rly.lb.cur = fg_rly.lb.fix = 0;
		      basht_display_stream_gone (&fg_rly);
		    }
		  relay_raw (cp, buf, n, isig, 1);
		}
	      else
		relay_chars (cp, &fg_rly, buf, n, 1);
	    }
	  else if (tcp)
	    {
	      /* a background task is selected: same relay the
		 prompt-time router uses */
	      if (cap_stdin_raw (tcp, &isig))
		relay_raw (tcp, buf, n, isig, 1);
	      else
		{
		  prompt_relay_bind (tcp);
		  relay_chars (tcp, &prompt_rly, buf, n, 1);
		}
	    }
	  else
	    {
	      /* the shell is selected: typing builds a visible
		 pending line, queued to readline as type-ahead on
		 Enter -- it echoes and runs when the prompt
		 returns. Job-control keys still act on the
		 foreground job (ISIG is off). */
	      for (ssize_t k = 0; k < n; k++)
		{
		  unsigned char c = (unsigned char)buf[k];
		  int e = key_escape (&fg_ta_esc, c, 1);
		  if (e == KESC_UP || e == KESC_DOWN)
		    {
		      ta_history (e == KESC_UP ? -1 : 1);
		      continue;
		    }
		  if (e)
		    continue;
		  if (c == 0x03)
		    fg_signal (pid, SIGINT);
		  else if (c == 0x1a)
		    fg_signal (pid, SIGTSTP);
		  else if (c == '\r' || c == '\n')
		    {
		      for (size_t j = 0; j < ta_rly.lb.len; j++)
			rl_stuff_char ((unsigned char)ta_rly.lb.data[j]);
		      rl_stuff_char ('\n');
		      ta_rly.lb.len = ta_rly.lb.cur = 0;
		      ta_hist = -1;
		      basht_display_partial (&ta_rly);
		      basht_display_set_owner (&self);
		    }
		  else if (c == 0x7f || c == '\b')
		    {
		      if (ta_rly.lb.len > 0)
			ta_rly.lb.cur = --ta_rly.lb.len;
		      basht_display_partial (&ta_rly);
		    }
		  else if (c == '\t' || c >= 0x20)
		    {
		      if (ta_rly.lb.len < BASHT_LINEBUF_CAP - 2)
			{
			  ta_rly.lb.data[ta_rly.lb.len++] = (char)c;
			  ta_rly.lb.cur = ta_rly.lb.len;
			}
		      basht_display_partial (&ta_rly);
		    }
		}
	    }
	}
    }

  basht_drain ();
  return 1;
}

/* The foreground wait is over: restore the terminal, drop the
   pending-input display. Characters the user had typed for the job
   that were never shipped (the job died mid-line) become type-ahead
   for the next prompt, exactly as on a shared terminal: they are
   pushed into readline's input queue, not discarded. */
void
basht_fg_end (void)
{
  size_t i;

  if (basht_active == 0)
    return;
  if (fg_raw)
    {
      tcsetattr (0, TCSANOW, &fg_tio_save);
      fg_raw = 0;
    }
  fg_pid = -1;
  manual_sel = 0;
  fg_ta_esc.fs = BF_NORMAL;
  if (fg_rly.lb.len > fg_rly.lb.fix)
    dbg ("fg_end stuff-back %d chars '%.*s'",
	 (int)(fg_rly.lb.len - fg_rly.lb.fix),
	 (int)(fg_rly.lb.len - fg_rly.lb.fix),
	 fg_rly.lb.data + fg_rly.lb.fix);
  for (i = fg_rly.lb.fix; i < fg_rly.lb.len; i++)
    rl_stuff_char ((unsigned char)fg_rly.lb.data[i]);
  fg_rly.lb.len = fg_rly.lb.cur = fg_rly.lb.fix = 0;
  basht_display_stream_gone (&fg_rly);
  /* a half-typed shell line queues too: it reappears, editable,
     at the prompt about to be drawn */
  for (i = 0; i < ta_rly.lb.len; i++)
    rl_stuff_char ((unsigned char)ta_rly.lb.data[i]);
  ta_rly.lb.len = ta_rly.lb.cur = 0;
  ta_hist = -1;
  basht_display_stream_gone (&ta_rly);
  basht_drain ();
}

/* `feed' builtin: one line of text to a task's stdin. INST <= 0
   means "the only live task with this name"; returns -2 if that is
   ambiguous, -1 if there is no match, 0 on success. */
int
basht_send_input (const char *name, int inst, const char *text)
{
  int i, found = -1;

  for (i = 0; i < BASHT_MAX_CAPS; i++)
    if (caps[i].m_in >= 0 && caps[i].out.id != 0
	&& strcmp (caps[i].out.name, name) == 0
	&& (inst <= 0 || caps[i].out.id == inst))
      {
	if (inst <= 0 && found >= 0)
	  return -2;
	found = i;
      }
  if (found < 0)
    return -1;
  {
    int fd = cap_key_fd (&caps[found]);
    if (fd < 0)
      return -1;
    basht_write_all (fd, text, strlen (text));
    basht_write_all (fd, "\n", 1);
  }
  return 0;
}

/* `clear' builtin: the operator clears the console. Only the
   parent interactive shell may touch the real terminal; in a
   forked child (subshell, background) or a non-interactive shell
   this returns 0 and the builtin falls back to writing the ANSI
   sequence to stdout -- which on a task pty is filtered out
   (tasks may not clear the console), and on a plain terminal is
   stock clear(1) behavior. KEEP nonzero preserves scrollback. */
int
basht_console_clear (int keep)
{
  if (basht_active == 0 || getpid () != self.pid)
    return 0;
  basht_drain ();
  basht_display_clear (keep);
  basht_display_sync ();
  return 1;
}

/* ---- prompt-time relay: input follows the incomplete-line rule --

   While readline is reading the shell's command line, a captured
   task may take the console by printing a prompt (a character on an
   incomplete line). Ownership routes the keyboard: that task gets
   the keystrokes, through the same relay as the foreground pump,
   until it completes a line and the console reverts to the shell.
   The shell's own prompt is just task 0's incomplete line -- the
   shell lives by the same rule. */

/* prompt_rly and rslot (the caps[] slot the relay is bound to,
   -1 = none) are declared with the foreground pump state above;
   rslot is set to -1 at init. */

/* The captured task, if any, that should receive keystrokes typed
   while readline is reading: the bottom-line owner's task, while it
   is still mid-line with an open in pty. 0 = input is the shell's. */
static struct basht_cap *
prompt_target (void)
{
  const BASHT_STREAM *own = basht_display_owner ();
  struct basht_cap *cp = 0;

  if (own == 0)
    return 0;
  if (own == &prompt_rly)
    {
      if (rslot < 0)
	return 0;
      cp = &caps[rslot];
      if (cp->pid != prompt_rly.pid || cp->out.id != prompt_rly.id)
	return 0;
    }
  else
    {
      for (int i = 0; i < BASHT_MAX_CAPS; i++)
	if (own == &caps[i].out || own == &caps[i].err)
	  {
	    cp = &caps[i];
	    break;
	  }
    }
  if (cp == 0 || cp->out.id == 0 || cp->m_in < 0)
    return 0;
  if (cp->windowed || cp->w_was)
    return 0;			/* keyboard comes from the window */
  if (cp->out.lb.len == 0 && cp->err.lb.len == 0)
    return 0;			/* line completed: console released */
  return cp;
}

/* The relay's bound task died or completed its line: its pending
   typed text expires. Never park text in readline's input queue
   while readline is active -- queued characters are consumed
   between the physical bytes of any escape sequence readline is
   mid-way through reading, shredding both (an up-arrow after a
   stuffed line yields "ESC t" plus a literal "[A"). At the prompt
   the text goes into readline's line buffer instead: visible,
   editable, correctly ordered. */
static void
prompt_relay_drop (void)
{
  char txt[BASHT_LINEBUF_CAP];
  size_t len = 0;

  if (prompt_rly.lb.len > prompt_rly.lb.fix)
    {
      len = prompt_rly.lb.len - prompt_rly.lb.fix;
      memcpy (txt, prompt_rly.lb.data + prompt_rly.lb.fix, len);
      txt[len] = '\0';
      dbg ("prompt relay drop-back %d chars '%s'", (int)len, txt);
      rl_point = rl_end;
      rl_insert_text (txt);
    }
  prompt_rly.lb.len = prompt_rly.lb.cur = prompt_rly.lb.fix = 0;
  basht_display_stream_gone (&prompt_rly);
  rslot = -1;
  if (len)
    {
      const BASHT_STREAM *own = basht_display_owner ();
      if (own == 0 || own == &self)
	rl_redisplay ();	/* incremental: draws just the new
				   text after the prompt already on
				   the shell's line */
    }
}

/* Is the relay's bound task gone, or done with its line? Pending
   input expires then; while the task merely lost the console
   (another task or the shell was selected) it keeps its half-typed
   line and shows it again when reselected. */
static int
rslot_dead (void)
{
  struct basht_cap *cp;

  if (rslot < 0)
    return 0;
  cp = &caps[rslot];
  if (cp->pid != prompt_rly.pid || cp->out.id != prompt_rly.id
      || cp->m_in < 0)
    return 1;
  if (cp->out.lb.len == 0 && cp->err.lb.len == 0)
    return 1;			/* line completed */
  return 0;
}

/* Bind the relay to CP (a task just took, or stole, the console).
   A half-typed line for a previous task becomes type-ahead. */
static void
prompt_relay_bind (struct basht_cap *cp)
{
  if (rslot == (int)(cp - caps) && prompt_rly.pid == cp->pid)
    return;
  if (prompt_rly.lb.len > prompt_rly.lb.fix)
    {
      /* typing switched tasks with a line half-typed: the old text
	 becomes shell type-ahead. Queue it only while readline is
	 dormant (foreground episode); at the prompt it goes into
	 the line buffer -- see prompt_relay_drop. */
      if (fg_pid > 0)
	{
	  for (size_t i = prompt_rly.lb.fix; i < prompt_rly.lb.len; i++)
	    rl_stuff_char ((unsigned char)prompt_rly.lb.data[i]);
	}
      else
	{
	  char txt[BASHT_LINEBUF_CAP];
	  size_t len = prompt_rly.lb.len - prompt_rly.lb.fix;
	  memcpy (txt, prompt_rly.lb.data + prompt_rly.lb.fix, len);
	  txt[len] = '\0';
	  rl_point = rl_end;
	  rl_insert_text (txt);
	}
    }
  memset (&prompt_rly, 0, sizeof prompt_rly);
  strcpy (prompt_rly.name, cp->out.name);
  prompt_rly.id = cp->out.id;
  prompt_rly.pid = cp->pid;
  prompt_rly.lines = cp->out.lines;
  rslot = (int)(cp - caps);
  dbg ("prompt relay bind [%s:%d] pid=%d", prompt_rly.name,
       prompt_rly.id, (int)prompt_rly.pid);
}

/* Alt-Left/Right: cycle the console between the inputting tasks --
   every stream currently mid-line, the shell included (always
   inputting while readline reads; during a foreground episode it
   takes keystrokes as type-ahead for the next prompt). The
   foreground task is always in the ring, prompt or not. At the
   prompt, selection is a forced instance of the ownership rule and
   the next task to write on an incomplete line steals the console
   back; during a foreground episode the selection holds until the
   selected task goes away or the episode ends. */
static void
basht_cycle_owner (int dir)
{
  const BASHT_STREAM *own = basht_display_owner ();
  const BASHT_STREAM *cand[BASHT_MAX_CAPS + 1];
  int slot[BASHT_MAX_CAPS + 1];
  int in_fg = fg_pid > 0;
  int n = 0, cur = -1, next, i;

  /* the shell first: at the prompt, bot == 0 means input already
     goes to readline, so it counts as the shell's position */
  cand[n] = &self;
  slot[n] = -1;
  if (in_fg ? manual_sel == &self : (own == 0 || own == &self))
    cur = n;
  n++;

  for (i = 0; i < BASHT_MAX_CAPS; i++)
    {
      struct basht_cap *cp = &caps[i];
      int is_fg = in_fg && cp->pid == fg_pid;
      if (cp->out.id == 0 || cp->m_in < 0 || cp->windowed || cp->w_was)
	continue;
      if (is_fg == 0 && cp->out.lb.len == 0 && cp->err.lb.len == 0)
	continue;
      cand[n] = cp->out.lb.len > 0 ? &cp->out
	      : cp->err.lb.len > 0 ? &cp->err : &cp->out;
      slot[n] = i;
      if (in_fg
	  ? (manual_sel ? manual_sel == &cp->out || manual_sel == &cp->err
			: is_fg)
	  : (own == &cp->out || own == &cp->err
	     || (own == &prompt_rly && rslot == i)))
	cur = n;
      n++;
    }

  if (n < 2)
    return;			/* nothing to switch to */
  next = cur < 0 ? (dir > 0 ? 0 : n - 1) : (cur + dir + n) % n;
  if (next == cur)
    return;

  /* a task being left keeps its half-typed line (each task owns
     its input buffer); it shows again when the task is reselected */

  manual_sel = cand[next];
  manual_sel_pid = slot[next] < 0 ? -1 : caps[slot[next]].pid;
  dbg ("cycle owner %+d -> %s", dir,
       slot[next] < 0 ? "bash" : caps[slot[next]].out.name);

  /* show the destination's own pending input line, if it has one:
     the relay buffer carrying half-typed text outranks the task's
     bare partial */
  {
    const BASHT_STREAM *show = cand[next];
    if (slot[next] < 0)
      {
	if (in_fg && ta_rly.lb.len > 0)
	  show = &ta_rly;
      }
    else if (in_fg && caps[slot[next]].pid == fg_pid
	     && fg_rly.lb.len > 0)
      show = &fg_rly;
    else if (rslot == slot[next] && prompt_rly.lb.len > 0)
      show = &prompt_rly;
    basht_display_set_owner (show);
  }
  basht_display_sync ();
}

/* the readline ends of the same keys, for when the shell owns the
   console and the sequence arrives through rl_getc */
static int
rl_basht_cycle_right (int count, int key)
{
  if (basht_active)
    basht_cycle_owner (1);
  return 0;
}

static int
rl_basht_cycle_left (int count, int key)
{
  if (basht_active)
    basht_cycle_owner (-1);
  return 0;
}

/* readline's character source. Instead of rl_event_hook (whose
   wait loop spins forever on non-tty EOF), we select over stdin and
   the task-0 master: output drains the moment it lands in the pty,
   and end-of-input reaches readline as a real EOF via rl_getc. */
static int
basht_getc (FILE *stream)
{
  int fd = fileno (stream);

  /* stood down (`task 0'): the hook stays installed, but readline
     reads the real terminal directly, as it would in stock bash */
  if (basht_active == 0)
    return rl_getc (stream);

  /* the console is ours while readline is reading: the task-0 tag
     line (the prompt) may own the bottom of the screen again */
  basht_display_set_default (&self);

  for (;;)
    {
      fd_set rf;
      struct timeval tv;
      int r, maxfd;

      basht_drain ();

      /* relay input pending for a task that vanished or completed
	 its line expires into the shell's command line; while the
	 task merely lost the console, it keeps its half-typed line */
      if (rslot >= 0 && rslot_dead ())
	prompt_relay_drop ();

      FD_ZERO (&rf);
      FD_SET (fd, &rf);
      maxfd = fd;
      if (self_master >= 0)
	{
	  FD_SET (self_master, &rf);
	  if (self_master > maxfd)
	    maxfd = self_master;
	}
      for (int i = 0; i < BASHT_MAX_CAPS; i++)
	{
	  if (caps[i].m_out >= 0)
	    {
	      FD_SET (caps[i].m_out, &rf);
	      if (caps[i].m_out > maxfd)
		maxfd = caps[i].m_out;
	    }
	  if (caps[i].m_err >= 0)
	    {
	      FD_SET (caps[i].m_err, &rf);
	      if (caps[i].m_err > maxfd)
		maxfd = caps[i].m_err;
	    }
	  if (caps[i].windowed && caps[i].w_in >= 0)
	    {
	      FD_SET (caps[i].w_in, &rf);
	      if (caps[i].w_in > maxfd)
		maxfd = caps[i].w_in;
	    }
	}
      tv.tv_sec = 0;
      tv.tv_usec = 200000;
      r = select (maxfd + 1, &rf, 0, 0, &tv);
      if (r > 0 && FD_ISSET (fd, &rf))
	{
	  struct basht_cap *cp = prompt_target ();
	  if (cp)
	    {
	      /* the console owner is a task mid-line: these
		 keystrokes are its input, not the shell's */
	      char buf[256];
	      ssize_t n = read (fd, buf, sizeof buf);
	      if (n > 0)
		{
		  int isig;
		  if (cap_stdin_raw (cp, &isig))
		    {
		      /* line-mode text typed before the task went
			 raw: the raw reader gets those bytes now */
		      if (rslot == (int)(cp - caps))
			{
			  int kfd = cap_key_fd (cp);
			  if (prompt_rly.lb.len > prompt_rly.lb.fix
			      && kfd >= 0)
			    basht_write_all (kfd,
				prompt_rly.lb.data + prompt_rly.lb.fix,
				prompt_rly.lb.len - prompt_rly.lb.fix);
			  prompt_rly.lb.len = prompt_rly.lb.cur =
			    prompt_rly.lb.fix = 0;
			  basht_display_stream_gone (&prompt_rly);
			  rslot = -1;
			}
		      relay_raw (cp, buf, n, isig, 1);
		    }
		  else
		    {
		      prompt_relay_bind (cp);
		      relay_chars (cp, &prompt_rly, buf, n, 1);
		    }
		  basht_display_sync ();
		  continue;
		}
	      /* EOF or error: readline's problem */
	    }
	  int c = rl_getc (stream);
	  dbg ("getc %s", dbgch (c));
	  return c;
	}
      if (r < 0 && errno != EINTR)
	{
	  int c = rl_getc (stream);
	  dbg ("getc(e) %s", dbgch (c));
	  return c;
	}
      if (r < 0)
	{
	  /* EINTR: in stock bash the signal interrupts the read
	     inside rl_getc, which post-processes it at once. Here
	     our select takes the EINTR instead, and looping without
	     that post-processing leaves a ^C caught-but-pending
	     until the NEXT keystroke -- whose character is then
	     destroyed with the line the deferred abort throws away
	     (^C then `hi' ran `i'). Do what rl_getc's EINTR path
	     does: fold the caught signal in, then let the event
	     hook bash armed abort the prompt line now. */
	  if (rl_pending_signal ())
	    rl_check_signals ();
	  if (rl_signal_event_hook)
	    (*rl_signal_event_hook) ();
	}
      /* timeout, EINTR, or pty traffic: drain and wait again */
    }
}

void
basht_init (void)
{
  int m, s, t;
  char *sname;

  if (basht_active || interactive_shell == 0)
    return;

  /* Directly captured by an outer basht: our stdin IS the pty it
     marked in the environment. Stay plain -- the outer multiplexer
     displays this shell as a task, and its relay drives our
     readline. A basht in a real terminal window (its own pty,
     unmarked) multiplexes as usual. */
  {
    const char *cap = getenv ("BASHT_CAPTURED");
    if (cap && *cap)
      {
	char *tn = ttyname (0);
	if (tn && strcmp (tn, cap) == 0)
	  return;
      }
  }

  t = dup (fileno (stderr));
  if (t < 0)
    return;
  fcntl (t, F_SETFD, FD_CLOEXEC);

  m = posix_openpt (O_RDWR | O_NOCTTY);
  if (m < 0)
    {
      close (t);
      return;
    }
  if (grantpt (m) < 0 || unlockpt (m) < 0 || (sname = ptsname (m)) == 0)
    {
      close (m);
      close (t);
      return;
    }
  fcntl (m, F_SETFD, FD_CLOEXEC);
  s = open (sname, O_RDWR | O_NOCTTY);
  if (s < 0 || dup2 (s, 1) < 0 || dup2 (s, 2) < 0)
    {
      if (s >= 0)
	close (s);
      close (m);
      close (t);
      return;
    }
  if (s > 2)
    close (s);

  basht_tty = t;
  self_master = m;
  basht_display_set_terminal (t);
  setvbuf (stdout, 0, _IOLBF, 0);
  setvbuf (stderr, 0, _IONBF, 0);

  memset (&self, 0, sizeof self);
  strcpy (self.name, "bash");
  self.id = 0;
  self.pid = getpid ();
  {
    static int self_lines;
    self.lines = &self_lines;
  }
  basht_display_set_default (&self);

  for (int i = 0; i < BASHT_MAX_CAPS; i++)
    {
      caps[i].m_in = caps[i].m_out = caps[i].m_err = -1;
      caps[i].w_out = caps[i].w_in = caps[i].w_ws = -1;
      caps[i].pid = -1;
    }

  {
    ssize_t k = readlink ("/proc/self/exe", self_exe,
			  sizeof self_exe - 1);
    if (k > 0)
      self_exe[k] = '\0';
    else
      strcpy (self_exe, "bash");
  }

  rslot = -1;
  rl_getc_function = basht_getc;
  rl_bind_keyseq ("\033[1;3C", rl_basht_cycle_right);	/* alt-right */
  rl_bind_keyseq ("\033[1;3D", rl_basht_cycle_left);	/* alt-left  */

  {
    const char *p = getenv ("BASHT_DEBUG");
    if (p && *p)
      {
	dbglog = fopen (p, "a");
	dbg ("=== basht %d up ===", (int)getpid ());
      }
  }

  /* SHLVL-style nesting witness, for the guard above */
  {
    const char *lvl = getenv ("BASHT_LEVEL");
    char buf[16];
    SHELL_VAR *v;

    snprintf (buf, sizeof buf, "%d", (lvl ? atoi (lvl) : 0) + 1);
    v = bind_variable ("BASHT_LEVEL", buf, 0);
    if (v)
      {
	VSETATTR (v, att_exported);
	array_needs_making = 1;
      }
  }

  self_shell = 1;
  basht_active = 1;
}

/* ---- `task' builtin: stand the multiplexer down and back up -----

   Tasking is on by default -- basht_init runs at shell startup.
   `task 0' puts the real terminal back on fds 1/2 and clears
   basht_active, which is the flag every hook already tests: jobs
   fork uncaptured, the keyboard relay and the fg pump step aside,
   and jobs.c hands the terminal to foreground jobs again. The
   result is stock bash. `task 1' undoes it.

   The self pty survives the gap. Its slave is only reachable
   through fds 1/2 (basht_init closes the open once it has dup2'd
   them), so standing down saves those as close-on-exec dups before
   the real tty goes over them -- the same trick
   basht_exec_prepare uses -- and standing back up is a pair of
   dup2s rather than a second init. That matters: re-running
   basht_init would allocate a fresh pty, renumber task 0 and bump
   BASHT_LEVEL.

   Captured tasks cannot outlive the multiplexer. Nothing would
   drain their masters and they would wedge as soon as a pty buffer
   filled, so a stand-down is refused while any are live rather
   than silently stranding them. */
static int susp_out = -1, susp_err = -1;

static int
live_caps (void)
{
  int i, n = 0;

  for (i = 0; i < BASHT_MAX_CAPS; i++)
    if (caps[i].out.id != 0)
      n++;
  return n;
}

int
basht_set_tasking (int on)
{
  if (on)
    {
      if (basht_active)
	return BASHT_TASK_OK;

      /* stood down earlier in this shell: swap the self pty back */
      if (susp_out >= 0 && susp_err >= 0 && self.pid == getpid ())
	{
	  dup2 (susp_out, 1);
	  dup2 (susp_err, 2);
	  close (susp_out);
	  close (susp_err);
	  susp_out = susp_err = -1;
	  setvbuf (stdout, 0, _IOLBF, 0);
	  setvbuf (stderr, 0, _IONBF, 0);
	  basht_display_set_terminal (basht_tty);
	  basht_display_set_default (&self);
	  basht_active = 1;
	  dbg ("tasking on (resumed)");
	  return BASHT_TASK_OK;
	}

      /* never came up here: try now. Fails for the shells that
	 have no multiplexer to give -- non-interactive, or already
	 captured by an outer basht. */
      basht_init ();
      return basht_active ? BASHT_TASK_OK : BASHT_TASK_FAILED;
    }

  if (basht_active == 0)
    return BASHT_TASK_OK;
  if (getpid () != self.pid)
    return BASHT_TASK_NOTSHELL;
  if (live_caps ())
    return BASHT_TASK_BUSY;

  basht_drain ();
  fflush (stdout);
  fflush (stderr);

  /* give the console back: erase the bottom line and leave no tag
     behind, so the first stock prompt starts on a clean row */
  basht_display_set_owner (0);
  basht_display_set_default (0);
  basht_display_sync ();

  susp_out = fcntl (1, F_DUPFD_CLOEXEC, 10);
  susp_err = fcntl (2, F_DUPFD_CLOEXEC, 10);
  if (susp_out < 0 || susp_err < 0)
    {
      if (susp_out >= 0)
	close (susp_out);
      if (susp_err >= 0)
	close (susp_err);
      susp_out = susp_err = -1;
      return BASHT_TASK_FAILED;
    }
  dup2 (basht_tty, 1);
  dup2 (basht_tty, 2);
  setvbuf (stdout, 0, _IOLBF, 0);
  setvbuf (stderr, 0, _IONBF, 0);
  basht_active = 0;
  dbg ("tasking off (stood down)");
  return BASHT_TASK_OK;
}

/* The exec builtin is about to replace this shell (issue #5): fds
   1/2 hold the self-pty slave, whose master dies with this image
   (FD_CLOEXEC), so anything the exec'd program writes there is
   lost -- an exec'd basht would wire its whole display into the
   dead pty. Put the real terminal back on 1/2; the exec'd program
   starts as if launched from a plain terminal. Saved dups (also
   close-on-exec) let a failed exec put the slave back, since the
   shell lives on. No-op in subshells: their fds are the child
   plumbing basht_child_stdio already arranged. */
static int exec_save_out = -1, exec_save_err = -1;

void
basht_exec_prepare (void)
{
  if (basht_active == 0 || basht_tty < 0 || getpid () != self.pid)
    return;
  basht_drain ();
  fflush (stdout);
  fflush (stderr);
  exec_save_out = fcntl (1, F_DUPFD_CLOEXEC, 10);
  exec_save_err = fcntl (2, F_DUPFD_CLOEXEC, 10);
  dup2 (basht_tty, 1);
  dup2 (basht_tty, 2);
  dbg ("exec_prepare: real tty on 1/2");
}

void
basht_exec_failed (void)
{
  if (basht_active == 0 || getpid () != self.pid)
    return;
  if (exec_save_out >= 0)
    {
      dup2 (exec_save_out, 1);
      close (exec_save_out);
      exec_save_out = -1;
    }
  if (exec_save_err >= 0)
    {
      dup2 (exec_save_err, 2);
      close (exec_save_err);
      exec_save_err = -1;
    }
  dbg ("exec_failed: pty slave restored on 1/2");
}

/* In a forked child (phase 1): put the real terminal back on fds
   1/2 so external commands behave exactly as in stock bash. Pipes
   and redirections are applied by the caller afterward and override
   this, same as they would the inherited fds. */
void
basht_child_stdio (void)
{
  if (basht_active == 0 || basht_tty < 0)
    return;
  /* Only the multiplexing shell's own children are tasks. Forked
     from a subshell, fds 1/2 are whatever that subshell arranged
     -- a command substitution's capture pipe, most importantly --
     and are not ours to redirect. */
  if (self_shell == 0)
    return;
  self_shell = 0;		/* this child's own forks are not tasks */
  if (pend_slot >= 0)
    {
      /* captured child: attach this task's pty slaves. A sole
	 command becomes a session leader with the in pty as its
	 controlling terminal, so /dev/tty resolves to the task's
	 own pty (less, man, ssh password prompts) and a nested
	 shell gets real job control (issue #15). Runs before the
	 job-control setpgid in make_child, so setsid cannot fail;
	 basht_child_sess tells jobs.c to skip that setpgid. */
      int sess = pend_solo && setsid () >= 0;
      int in = open (pend_sin, O_RDWR | O_NOCTTY);
      int o = open (pend_sout, O_RDWR | O_NOCTTY);
      int e = open (pend_serr, O_RDWR | O_NOCTTY);
      if (in >= 0 && o >= 0 && e >= 0)
	{
	  dup2 (in, 0);
	  dup2 (o, 1);
	  dup2 (e, 2);
	  if (in > 2)
	    close (in);
	  if (o > 2)
	    close (o);
	  if (e > 2)
	    close (e);
	  if (sess)
	    {
	      ioctl (0, TIOCSCTTY, 0);
	      tcsetpgrp (0, getpid ());
	      basht_child_sess = 1;
	    }
	  /* mark the exec environment: a basht finding its stdin
	     to BE the marked pty knows it is a captured task and
	     stays plain (see basht_init) */
	  {
	    extern char **export_env;
	    char kv[96];
	    int n;

	    snprintf (kv, sizeof kv, "BASHT_CAPTURED=%s", pend_sin);
	    n = strvec_len (export_env);
	    export_env = strvec_resize (export_env, n + 2);
	    export_env[n] = savestring (kv);
	    export_env[n + 1] = 0;
	  }
	  return;
	}
      if (in >= 0)
	close (in);
      if (o >= 0)
	close (o);
      if (e >= 0)
	close (e);
      /* fall through: run on the real terminal */
    }
  dup2 (basht_tty, 1);
  dup2 (basht_tty, 2);
}

/* ---- --basht-bridge: runs inside the spawned terminal window ----

   The window is a viewport onto a task that keeps running on
   basht's ptys. This end goes raw, reports its window size (4
   bytes on the winsize fifo, at startup and again on every
   SIGWINCH -- the resize follows the task), then shuttles: fifo
   from basht -> our stdout (the real terminal draws it), our
   keyboard -> fifo to basht. Exits when basht closes the outbound
   fifo (task ended), which closes the window. */

static volatile sig_atomic_t bridge_winch;

static void
bridge_sigwinch (int sig)
{
  bridge_winch = 1;
}

static void
bridge_send_ws (int wfd)
{
  struct winsize ws;
  unsigned char hdr[4];

  memset (&ws, 0, sizeof ws);
  if (ioctl (0, TIOCGWINSZ, &ws) < 0 || ws.ws_row == 0)
    {
      ws.ws_row = 24;
      ws.ws_col = 80;
    }
  hdr[0] = ws.ws_row >> 8;
  hdr[1] = ws.ws_row & 0xff;
  hdr[2] = ws.ws_col >> 8;
  hdr[3] = ws.ws_col & 0xff;
  basht_write_all (wfd, hdr, 4);
}

int
basht_bridge_main (const char *opath, const char *ipath,
		   const char *wpath)
{
  int ofd, ifd, wfd, r;
  struct termios save, t;
  int have_tio;
  unsigned char buf[4096];
  ssize_t n;

  signal (SIGPIPE, SIG_IGN);
  signal (SIGWINCH, bridge_sigwinch);
  ofd = open (opath, O_RDONLY | O_NONBLOCK);
  ifd = open (ipath, O_WRONLY);
  wfd = open (wpath, O_WRONLY);
  if (ofd < 0 || ifd < 0 || wfd < 0)
    return 1;
  fcntl (ofd, F_SETFL, 0);	/* back to blocking */

  have_tio = tcgetattr (0, &save) == 0;
  if (have_tio)
    {
      t = save;
      t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
      t.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
      t.c_cc[VMIN] = 1;
      t.c_cc[VTIME] = 0;
      tcsetattr (0, TCSANOW, &t);
    }

  bridge_send_ws (wfd);

  for (;;)
    {
      fd_set rf;
      if (bridge_winch)
	{
	  bridge_winch = 0;
	  bridge_send_ws (wfd);
	}
      FD_ZERO (&rf);
      FD_SET (0, &rf);
      FD_SET (ofd, &rf);
      r = select (ofd + 1, &rf, 0, 0, 0);
      if (r < 0)
	{
	  if (errno == EINTR)
	    continue;		/* SIGWINCH lands here */
	  break;
	}
      if (FD_ISSET (ofd, &rf))
	{
	  n = read (ofd, buf, sizeof buf);
	  if (n <= 0)
	    break;		/* task over: close the window */
	  basht_write_all (1, buf, (size_t)n);
	}
      if (FD_ISSET (0, &rf))
	{
	  n = read (0, buf, sizeof buf);
	  if (n <= 0)
	    break;
	  basht_write_all (ifd, buf, (size_t)n);
	}
    }

  if (have_tio)
    tcsetattr (0, TCSANOW, &save);
  return 0;
}
