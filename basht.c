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
#include <unistd.h>

#include "bashtypes.h"
#include "shell.h"
#include "jobs.h"		/* FORK_* flags */

#include <readline/readline.h>

#include "basht.h"

int basht_active = 0;
int basht_tty = -1;

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
  int    w_need_ws;		/* awaiting 4-byte winsize header  */
  int    w_hinted;		/* windowing failed; hint printed  */
  char   wdir[64];		/* mkdtemp dir holding the fifos   */
  size_t nbytes;		/* total stdout bytes read         */
  unsigned char pre[2048];	/* raw stdout prefix, for replay   */
  size_t prelen;
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
  cp->w_out = cp->w_in = -1;
  cp->windowed = 0;
  if (cp->wdir[0])
    {
      snprintf (path, sizeof path, "%s/o", cp->wdir);
      unlink (path);
      snprintf (path, sizeof path, "%s/i", cp->wdir);
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
   interactive children qualify (no comsub/procsub). */
void
basht_fork_prepare (const char *command, int flags)
{
  int i;
  struct basht_cap *cp;

  if (pend_slot >= 0)		/* stale (fork never completed) */
    {
      cap_free (&caps[pend_slot]);
      pend_slot = -1;
    }
  if (basht_active == 0 || (flags & (FORK_COMSUB | FORK_PROCSUB)))
    return;

  for (i = 0; i < BASHT_MAX_CAPS; i++)
    if (caps[i].m_in < 0 && caps[i].m_out < 0 && caps[i].m_err < 0)
      break;
  if (i == BASHT_MAX_CAPS)
    return;			/* table full: run uncaptured */
  cp = &caps[i];

  memset (cp, 0, sizeof *cp);
  cp->pid = -1;
  cp->w_out = cp->w_in = -1;
  cp->m_in = open_master (pend_sin, sizeof pend_sin);
  cp->m_out = open_master (pend_sout, sizeof pend_sout);
  cp->m_err = open_master (pend_serr, sizeof pend_serr);
  if (cp->m_in < 0 || cp->m_out < 0 || cp->m_err < 0)
    {
      cap_free (cp);
      return;
    }

  cap_name (command, cp->out.name, sizeof cp->out.name);
  if (cp->out.name[0] == '(')	/* subshell command text */
    strcpy (cp->out.name, "sub");
  strcpy (cp->err.name, cp->out.name);
  cp->out.id = cp->err.id = next_instance (cp->out.name);
  cp->out.mark = 0;
  cp->err.mark = '!';
  cp->out.lines = cp->err.lines = &cp->lines;
  pend_slot = i;
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

/* ---- auto-window: full-screen tasks move to a terminal window ---

   When a task enables the alternate screen, basht stops filtering
   it, spawns a terminal window running this same binary in
   --basht-bridge mode, replays the task's small raw prefix (which
   ends at the trigger sequence: detection happens at the program's
   first breath), and from then on shuttles bytes between the
   task's ptys and the window over a fifo pair. Bytes the task
   writes while the window is starting simply wait in the kernel
   pty queue -- basht stops reading, and the backed-up pty is all
   the flow control needed. */

static char self_exe[256];

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
  char fo[96], fi[96];
  char termbuf[256];
  char *targv[16];
  int ntargv;
  pid_t wpid;
  unsigned char rep[sizeof cp->pre];
  size_t replen;
  const char *termcmd;

  if (cp->windowed || cp->pid <= 0)
    return 0;
  /* only at the task's first breath: prefix complete, nothing
     displayed yet -- outside that, restarting or attaching would
     lose state, so just hint */
  if (cp->nbytes > sizeof cp->pre || cp->lines != 0)
    return 0;
  termcmd = getenv ("BASHT_TERMINAL");
  if (termcmd == 0 || *termcmd == '\0')
    {
      if (getenv ("DISPLAY") == 0 && getenv ("WAYLAND_DISPLAY") == 0)
	return 0;		/* no GUI to open a window on */
      termcmd = "gnome-terminal --";
    }

  strcpy (cp->wdir, "/tmp/basht.XXXXXX");
  if (mkdtemp (cp->wdir) == 0)
    {
      cp->wdir[0] = '\0';
      return 0;
    }
  snprintf (fo, sizeof fo, "%s/o", cp->wdir);
  snprintf (fi, sizeof fi, "%s/i", cp->wdir);
  if (mkfifo (fo, 0600) < 0 || mkfifo (fi, 0600) < 0)
    {
      cap_window_close (cp);
      return 0;
    }
  /* O_RDWR on the outbound fifo: never blocks at open, and since we
     hold a reader it can never raise SIGPIPE at us; the bridge
     still sees EOF when we close it. Inbound is a plain
     nonblocking reader. */
  cp->w_out = open (fo, O_RDWR | O_NONBLOCK);
  cp->w_in = open (fi, O_RDONLY | O_NONBLOCK);
  if (cp->w_out < 0 || cp->w_in < 0)
    {
      cap_window_close (cp);
      return 0;
    }
  fcntl (cp->w_out, F_SETFD, FD_CLOEXEC);
  fcntl (cp->w_in, F_SETFD, FD_CLOEXEC);

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

  /* replay the prefix (it ends at the trigger); the window's
     terminal interprets it natively */
  replen = scrub_queries (cp->pre, cp->prelen, rep);
  basht_write_all (cp->w_out, rep, replen);

  cp->windowed = 1;
  cp->w_was = 1;
  cp->w_need_ws = 1;
  basht_display_event (&cp->out, "full screen: moved to a window");
  return 1;
}

/* Shuttle for a windowed task: window keyboard -> task stdin
   (after the 4-byte winsize header), task stdout/stderr -> window,
   gated on fifo room so a stalled window backs up into the kernel
   pty queue instead of into basht. */
static void
shuttle_windowed (struct basht_cap *cp)
{
  unsigned char buf[2048];
  ssize_t n;

  /* keyboard and winsize from the bridge */
  while (cp->w_in >= 0)
    {
      n = read (cp->w_in, buf, sizeof buf);
      if (n > 0)
	{
	  unsigned char *p = buf;
	  if (cp->w_need_ws && n >= 4)
	    {
	      struct winsize ws;
	      memset (&ws, 0, sizeof ws);
	      ws.ws_row = (buf[0] << 8) | buf[1];
	      ws.ws_col = (buf[2] << 8) | buf[3];
	      if (ws.ws_row > 0 && ws.ws_col > 0)
		{
		  ioctl (cp->m_in, TIOCSWINSZ, &ws);
		  ioctl (cp->m_out, TIOCSWINSZ, &ws);
		  ioctl (cp->m_err, TIOCSWINSZ, &ws);
		  fg_signal (cp->pid, SIGWINCH);
		}
	      cp->w_need_ws = 0;
	      p += 4;
	      n -= 4;
	    }
	  if (n > 0 && cp->m_in >= 0)
	    basht_write_all (cp->m_in, p, (size_t)n);
	  continue;
	}
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
	break;
      if (n < 0 && errno == EINTR)
	continue;
      if (n == 0 && cp->w_need_ws)
	break;			/* bridge not connected yet */
      /* window closed: hang up the task; if it survives, its
	 output returns to the tagged console */
      fg_signal (cp->pid, SIGHUP);
      cap_window_close (cp);
      basht_display_event (&cp->out, "window closed");
      return;
    }

  /* task output to the window, only as fast as the fifo drains */
  for (int which = 0; which < 2; which++)
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
	      done += basht_filter_bytes (&cp->out, buf + done,
					  (size_t)n - done, &trig);
	      if (trig && window_try (cp))
		return;		/* rest of chunk was in the prefix */
	      if (trig && cp->w_hinted == 0)
		{
		  cp->w_hinted = 1;
		  basht_display_event (&cp->out,
		      "full-screen program; try: gnome-terminal -- %s",
		      cp->out.name);
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
   (displayed as "[name:N<] text"), Enter ships it to the job's in
   pty, ^C/^Z become killpg(SIGINT/SIGTSTP) on the job's process
   group, ^D sends VEOF. Returns 1 if it pumped (caller should then
   reap with WNOHANG), 0 to fall back to a blocking wait. */

static struct termios fg_tio_save;
static int   fg_raw = 0;
static int   fg_stdin_open = 1;
static pid_t fg_pid = -1;
static BASHT_STREAM fg_rly;

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
   RLY's pending line (displayed "[name:N<] prompt typed-text"),
   Enter ships the typed part to the task's in pty, backspace edits,
   ^C/^Z signal the task's process group, ^D forwards VEOF. Shared
   by the foreground pump and the prompt-time relay. */
static void
relay_chars (struct basht_cap *cp, BASHT_STREAM *rly,
	     const char *buf, ssize_t n)
{
  for (ssize_t k = 0; k < n; k++)
    {
      unsigned char c = (unsigned char)buf[k];
      if (c == 0x03)		/* ^C */
	fg_signal (cp->pid, SIGINT);
      else if (c == 0x1a)		/* ^Z */
	fg_signal (cp->pid, SIGTSTP);
      else if (c == 0x04)		/* ^D */
	{
	  char e = 0x04;
	  if (cp->m_in >= 0)
	    basht_write_all (cp->m_in, &e, 1);
	}
      else if (c == '\r' || c == '\n')
	{
	  relay_prefix (cp, rly);
	  basht_display_line (rly, rly->lb.data, rly->lb.len);
	  if (cp->m_in >= 0)
	    {
	      rly->lb.data[rly->lb.len] = '\n';
	      basht_write_all (cp->m_in, rly->lb.data + rly->lb.fix,
			       rly->lb.len - rly->lb.fix + 1);
	    }
	  rly->lb.len = rly->lb.cur = rly->lb.fix = 0;
	  basht_display_partial (rly);
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

      /* relay line empty again: the task's own partial (the prompt
	 that earned it the console) returns to the bottom line */
      if (rly->lb.len == 0)
	{
	  if (cp->out.lb.len > 0)
	    basht_display_partial (&cp->out);
	  else if (cp->err.lb.len > 0)
	    basht_display_partial (&cp->err);
	}
    }
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
      memset (&fg_rly, 0, sizeof fg_rly);
      strcpy (fg_rly.name, cp->out.name);
      fg_rly.id = cp->out.id;
      fg_rly.mark = '<';
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
	  if (cp->m_in >= 0)
	    basht_write_all (cp->m_in, &e, 1);
	  fg_stdin_open = 0;
	}
      else
	relay_chars (cp, &fg_rly, buf, n);
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
  if (fg_rly.lb.len > fg_rly.lb.fix)
    dbg ("fg_end stuff-back %d chars '%.*s'",
	 (int)(fg_rly.lb.len - fg_rly.lb.fix),
	 (int)(fg_rly.lb.len - fg_rly.lb.fix),
	 fg_rly.lb.data + fg_rly.lb.fix);
  for (i = fg_rly.lb.fix; i < fg_rly.lb.len; i++)
    rl_stuff_char ((unsigned char)fg_rly.lb.data[i]);
  fg_rly.lb.len = fg_rly.lb.cur = fg_rly.lb.fix = 0;
  basht_display_stream_gone (&fg_rly);
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
  basht_write_all (caps[found].m_in, text, strlen (text));
  basht_write_all (caps[found].m_in, "\n", 1);
  return 0;
}

/* ---- prompt-time relay: input follows the incomplete-line rule --

   While readline is reading the shell's command line, a captured
   task may take the console by printing a prompt (a character on an
   incomplete line). Ownership routes the keyboard: that task gets
   the keystrokes, through the same relay as the foreground pump,
   until it completes a line and the console reverts to the shell.
   The shell's own prompt is just task 0's incomplete line -- the
   shell lives by the same rule. */

static BASHT_STREAM prompt_rly;
static int rslot = -1;		/* caps[] slot the relay is bound to */

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

/* The relay's task went away or completed its line with input still
   pending: the typed characters become readline type-ahead, exactly
   as in basht_fg_end. Returns the first pending char (the caller
   hands it to readline now; the rest are queued behind it) or -1. */
static int
prompt_relay_drop (void)
{
  int c = -1;

  if (prompt_rly.lb.len > prompt_rly.lb.fix)
    {
      size_t fix = prompt_rly.lb.fix;
      dbg ("prompt relay stuff-back %d chars '%.*s'",
	   (int)(prompt_rly.lb.len - fix),
	   (int)(prompt_rly.lb.len - fix), prompt_rly.lb.data + fix);
      c = (unsigned char)prompt_rly.lb.data[fix];
      for (size_t i = fix + 1; i < prompt_rly.lb.len; i++)
	rl_stuff_char ((unsigned char)prompt_rly.lb.data[i]);
    }
  prompt_rly.lb.len = prompt_rly.lb.cur = prompt_rly.lb.fix = 0;
  basht_display_stream_gone (&prompt_rly);
  rslot = -1;
  return c;
}

/* Bind the relay to CP (a task just took, or stole, the console).
   A half-typed line for a previous task becomes type-ahead. */
static void
prompt_relay_bind (struct basht_cap *cp)
{
  if (rslot == (int)(cp - caps) && prompt_rly.pid == cp->pid)
    return;
  for (size_t i = prompt_rly.lb.fix; i < prompt_rly.lb.len; i++)
    rl_stuff_char ((unsigned char)prompt_rly.lb.data[i]);
  memset (&prompt_rly, 0, sizeof prompt_rly);
  strcpy (prompt_rly.name, cp->out.name);
  prompt_rly.id = cp->out.id;
  prompt_rly.mark = '<';
  prompt_rly.pid = cp->pid;
  prompt_rly.lines = cp->out.lines;
  rslot = (int)(cp - caps);
  dbg ("prompt relay bind [%s:%d] pid=%d", prompt_rly.name,
       prompt_rly.id, (int)prompt_rly.pid);
}

/* readline's character source. Instead of rl_event_hook (whose
   wait loop spins forever on non-tty EOF), we select over stdin and
   the task-0 master: output drains the moment it lands in the pty,
   and end-of-input reaches readline as a real EOF via rl_getc. */
static int
basht_getc (FILE *stream)
{
  int fd = fileno (stream);

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
	 its line: it becomes readline type-ahead now */
      if (rslot >= 0 && prompt_target () == 0)
	{
	  int c = prompt_relay_drop ();
	  if (c >= 0)
	    {
	      dbg ("getc(stuffed) %s", dbgch (c));
	      return c;
	    }
	}

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
		  prompt_relay_bind (cp);
		  relay_chars (cp, &prompt_rly, buf, n);
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
      caps[i].w_out = caps[i].w_in = -1;
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

  rl_getc_function = basht_getc;

  {
    const char *p = getenv ("BASHT_DEBUG");
    if (p && *p)
      {
	dbglog = fopen (p, "a");
	dbg ("=== basht %d up ===", (int)getpid ());
      }
  }

  basht_active = 1;
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
  if (pend_slot >= 0)
    {
      /* captured child: attach this task's pty slaves */
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
   basht's ptys. This end goes raw, reports its window size (4-byte
   header), then shuttles: fifo from basht -> our stdout (the real
   terminal draws it), our keyboard -> fifo to basht. Exits when
   basht closes the outbound fifo (task ended), which closes the
   window. */
int
basht_bridge_main (const char *opath, const char *ipath)
{
  int ofd, ifd, r;
  struct termios save, t;
  int have_tio;
  struct winsize ws;
  unsigned char hdr[4];
  unsigned char buf[4096];
  ssize_t n;

  signal (SIGPIPE, SIG_IGN);
  ofd = open (opath, O_RDONLY | O_NONBLOCK);
  ifd = open (ipath, O_WRONLY);
  if (ofd < 0 || ifd < 0)
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
  basht_write_all (ifd, hdr, 4);

  for (;;)
    {
      fd_set rf;
      FD_ZERO (&rf);
      FD_SET (0, &rf);
      FD_SET (ofd, &rf);
      r = select (ofd + 1, &rf, 0, 0, 0);
      if (r < 0)
	{
	  if (errno == EINTR)
	    continue;
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
