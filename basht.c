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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include "bashtypes.h"
#include "shell.h"

#include <readline/readline.h>

#include "basht.h"

int basht_active = 0;
int basht_tty = -1;

static BASHT_STREAM self;	/* task 0 */
static int self_master = -1;	/* master side of task 0's pty */

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

void
basht_drain (void)
{
  if (basht_active == 0)
    return;
  fflush (stdout);
  fflush (stderr);
  drain_self ();
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
  dup2 (basht_tty, 1);
  dup2 (basht_tty, 2);
}
