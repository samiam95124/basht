/* job.c — job table, pty allocation, spawn. */

#include "mtsh.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct job jobs[MAX_JOBS];
static int next_id = 1;

/* Allocate one pty master; copy the slave path out (ptsname()'s
   buffer is static and we need three at once). */
int pty_master(char *sname, size_t cap)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0) {
        fprintf(stderr, "mtsh: posix_openpt: %s\n", strerror(errno));
        return -1;
    }
    char *p;
    if (grantpt(m) < 0 || unlockpt(m) < 0 || (p = ptsname(m)) == NULL) {
        fprintf(stderr, "mtsh: pty setup: %s\n", strerror(errno));
        close(m);
        return -1;
    }
    snprintf(sname, cap, "%s", p);
    if (fcntl(m, F_SETFD, FD_CLOEXEC) < 0) {
        fprintf(stderr, "mtsh: fcntl: %s\n", strerror(errno));
        close(m);
        return -1;
    }
    return m;
}

/* Nothing ever reads the out/err ptys back, so echo must be off or
   every output byte would bounce around the line discipline. */
static void echo_off(int m)
{
    struct termios t;
    if (tcgetattr(m, &t) == 0) {
        t.c_lflag &= ~ECHO;
        tcsetattr(m, TCSANOW, &t);
    }
}

struct job *job_spawn(char **argv)
{
    struct job *j = NULL;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].id == 0) {
            j = &jobs[i];
            break;
        }
    }
    if (j == NULL) {
        fprintf(stderr, "mtsh: job table full\n");
        return NULL;
    }
    memset(j, 0, sizeof *j);
    j->m_in = j->m_out = j->m_err = -1;

    char s_in[64], s_out[64], s_err[64];
    if ((j->m_in  = pty_master(s_in,  sizeof s_in))  < 0 ||
        (j->m_out = pty_master(s_out, sizeof s_out)) < 0 ||
        (j->m_err = pty_master(s_err, sizeof s_err)) < 0) {
        job_free(j);
        return NULL; /* report and refuse the job; tsh lives on */
    }
    echo_off(j->m_out);
    echo_off(j->m_err);
    /* in pty stays cooked with ISIG (that's what makes ^C work),
       but echo off — nothing ever reads the echoes back and they
       would eventually fill the kernel buffer. */
    echo_off(j->m_in);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "mtsh: fork: %s\n", strerror(errno));
        job_free(j);
        return NULL;
    }

    if (pid == 0) {
        /* Child: new session, input pty becomes controlling tty so
           the line discipline can deliver ^C (ISIG) to us alone. */
        if (setsid() < 0)
            _exit(126);
        int fin = open(s_in, O_RDWR);
        if (fin < 0) {
            fprintf(stderr, "mtsh: open %s: %s\n", s_in, strerror(errno));
            _exit(126);
        }
        (void)ioctl(fin, TIOCSCTTY, 0); /* may already be ctty */
        int fout = open(s_out, O_RDWR | O_NOCTTY);
        int ferr = open(s_err, O_RDWR | O_NOCTTY);
        if (fout < 0 || ferr < 0) {
            fprintf(stderr, "mtsh: open pty slave: %s\n", strerror(errno));
            _exit(126);
        }
        if (dup2(fin, 0) < 0 || dup2(fout, 1) < 0 || dup2(ferr, 2) < 0)
            _exit(126);
        if (fin  > 2) close(fin);
        if (fout > 2) close(fout);
        if (ferr > 2) close(ferr);
        /* masters are FD_CLOEXEC; exec drops them */
        execvp(argv[0], argv);
        fprintf(stderr, "mtsh: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    j->pid = pid;
    j->id = next_id++;
    const char *base = strrchr(argv[0], '/');
    snprintf(j->name, sizeof j->name, "%s", base ? base + 1 : argv[0]);
    j->running = 1;
    return j;
}

struct job *job_get(int idx)
{
    return &jobs[idx];
}

struct job *job_find(int id)
{
    if (id <= 0)
        return NULL;
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].id == id)
            return &jobs[i];
    return NULL;
}

struct job *job_find_pid(pid_t pid)
{
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].id != 0 && jobs[i].pid == pid)
            return &jobs[i];
    return NULL;
}

void job_free(struct job *j)
{
    display_job_gone(j);
    if (j->m_in  >= 0) close(j->m_in);
    if (j->m_out >= 0) close(j->m_out);
    if (j->m_err >= 0) close(j->m_err);
    j->m_in = j->m_out = j->m_err = -1;
    j->id = 0;
}
