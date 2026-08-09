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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
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
  BASHT_STREAM out, err;
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
cap_free (struct basht_cap *cp)
{
  if (cp->m_in >= 0)
    close (cp->m_in);
  if (cp->m_out >= 0)
    close (cp->m_out);
  if (cp->m_err >= 0)
    close (cp->m_err);
  cp->m_in = cp->m_out = cp->m_err = -1;
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
    caps[pend_slot].pid = pid;
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
	basht_filter_bytes (&self, buf, (size_t)n);
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
	  basht_filter_bytes (ts, buf, (size_t)n);
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

static void
drain_caps (void)
{
  int i;

  for (i = 0; i < BASHT_MAX_CAPS; i++)
    {
      drain_cap_fd (&caps[i].m_out, &caps[i].out);
      drain_cap_fd (&caps[i].m_err, &caps[i].err);
      /* both output streams gone: the task is over; release */
      if (caps[i].m_out < 0 && caps[i].m_err < 0 && caps[i].m_in >= 0)
	cap_free (&caps[i]);
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

  if (pg > 0)
    killpg (pg, sig);
  else
    kill (pid, sig);
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
      fg_pid = pid;
      fg_stdin_open = 1;
      memset (&fg_rly, 0, sizeof fg_rly);
      strcpy (fg_rly.name, cp->out.name);
      fg_rly.id = cp->out.id;
      fg_rly.mark = '<';
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
	for (ssize_t k = 0; k < n; k++)
	  {
	    unsigned char c = (unsigned char)buf[k];
	    if (c == 0x03)		/* ^C */
	      fg_signal (pid, SIGINT);
	    else if (c == 0x1a)		/* ^Z */
	      fg_signal (pid, SIGTSTP);
	    else if (c == 0x04)		/* ^D */
	      {
		char e = 0x04;
		if (cp->m_in >= 0)
		  basht_write_all (cp->m_in, &e, 1);
	      }
	    else if (c == '\r' || c == '\n')
	      {
		basht_display_line (&fg_rly, fg_rly.lb.data, fg_rly.lb.len);
		if (cp->m_in >= 0)
		  {
		    fg_rly.lb.data[fg_rly.lb.len] = '\n';
		    basht_write_all (cp->m_in, fg_rly.lb.data,
				     fg_rly.lb.len + 1);
		  }
		fg_rly.lb.len = fg_rly.lb.cur = 0;
		basht_display_partial (&fg_rly);
	      }
	    else if (c == 0x7f || c == '\b')
	      {
		if (fg_rly.lb.len > 0)
		  fg_rly.lb.cur = --fg_rly.lb.len;
		basht_display_partial (&fg_rly);
	      }
	    else if (c == '\t' || c >= 0x20)
	      {
		if (fg_rly.lb.len < BASHT_LINEBUF_CAP - 2)
		  {
		    fg_rly.lb.data[fg_rly.lb.len++] = (char)c;
		    fg_rly.lb.cur = fg_rly.lb.len;
		  }
		basht_display_partial (&fg_rly);
	      }
	    /* other control characters are dropped */
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
  for (i = 0; i < fg_rly.lb.len; i++)
    rl_stuff_char ((unsigned char)fg_rly.lb.data[i]);
  fg_rly.lb.len = fg_rly.lb.cur = 0;
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
	}
      tv.tv_sec = 0;
      tv.tv_usec = 200000;
      r = select (maxfd + 1, &rf, 0, 0, &tv);
      if (r > 0 && FD_ISSET (fd, &rf))
	return rl_getc (stream);
      if (r < 0 && errno != EINTR)
	return rl_getc (stream);
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
  basht_display_set_default (&self);

  for (int i = 0; i < BASHT_MAX_CAPS; i++)
    {
      caps[i].m_in = caps[i].m_out = caps[i].m_err = -1;
      caps[i].pid = -1;
    }

  rl_getc_function = basht_getc;

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
