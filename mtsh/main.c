/* main.c — readline loop, tokenizer, builtins, both event-loop
 * phases (§8): prompt phase via rl_event_hook, foreground phase as
 * a select() loop. Background jobs print through in both.
 */

#include "mtsh.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <readline/history.h>
#include <readline/readline.h>

#define MAX_ARGS 256
#define VEOF_CH  0x04

/* readline's prompt is empty: the visible prompt is task 0's tag,
   drawn by the display layer as the default bottom-line owner. */
static const char *PROMPT = "";

static volatile sig_atomic_t got_sigint;
static int stdin_gone; /* stdin hit EOF while readline was waiting */

/* mtsh itself is task 0: its own stdout/stderr are rerouted into a
   pty and drained, filtered and tagged exactly like any job's. Only
   the display layer touches the real terminal. */
static struct job self;

static int self_setup(void)
{
    int tty = dup(STDOUT_FILENO);
    if (tty < 0)
        return -1;
    fcntl(tty, F_SETFD, FD_CLOEXEC);

    char sname[64];
    int m = pty_master(sname, sizeof sname);
    if (m < 0) {
        close(tty);
        return -1;
    }
    int s = open(sname, O_RDWR | O_NOCTTY);
    if (s < 0 || dup2(s, 1) < 0 || dup2(s, 2) < 0) {
        if (s >= 0)
            close(s);
        close(m);
        close(tty);
        return -1;
    }
    if (s > 2)
        close(s);

    display_set_terminal(tty);
    /* readline's echo and redraw output is just task 0 traffic: it
       flows into the pty, through the filter, and the display layer
       reconstructs the entry line from it like any other stream */
    rl_outstream = stdout;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    memset(&self, 0, sizeof self);
    snprintf(self.name, sizeof self.name, "mtsh");
    self.id = 0; /* reserved; job ids start at 1 */
    self.pid = getpid();
    self.m_in = self.m_err = -1;
    self.m_out = m;
    self.running = 1;
    display_set_default(&self, &self.out_buf);
    return 0;
}

static void on_sigint(int sig)
{
    (void)sig;
    got_sigint = 1;
}

/* ---- draining, shared by both phases --------------------------- */

/* Read whatever one master has; on EOF flush the partial line and
   close. Master reads return EIO (not 0) on Linux once the slave
   side is gone. */
static void drain_master(struct job *j, int *fd, struct linebuf *lb,
                         char mark)
{
    unsigned char buf[4096];
    ssize_t n = read(*fd, buf, sizeof buf);

    if (n > 0) {
        filter_bytes(j, lb, buf, (size_t)n, mark);
        return;
    }
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
        return;
    if (n < 0 && errno != EIO)
        fprintf(stderr, "mtsh: read pty: %s\n", strerror(errno));
    filter_flush(j, lb, mark);
    close(*fd);
    *fd = -1;
}

/* Reap exited children, poll every live master once (zero-timeout
   select), display completed lines, and finalize jobs whose two
   output streams have both hit EOF (so no tail output is lost).
   Called from both phases; never blocks. */
static void drain_all(void)
{
    int st;
    pid_t p;
    while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
        struct job *j = job_find_pid(p);
        if (j != NULL) {
            j->running = 0;
            j->exit_status = st;
        }
    }

    fd_set rf;
    FD_ZERO(&rf);
    int maxfd = -1;
    if (self.m_out >= 0) {
        FD_SET(self.m_out, &rf);
        maxfd = self.m_out;
    }
    for (int i = 0; i < MAX_JOBS; i++) {
        struct job *j = job_get(i);
        if (j->id == 0)
            continue;
        if (j->m_out >= 0) {
            FD_SET(j->m_out, &rf);
            if (j->m_out > maxfd) maxfd = j->m_out;
        }
        if (j->m_err >= 0) {
            FD_SET(j->m_err, &rf);
            if (j->m_err > maxfd) maxfd = j->m_err;
        }
    }
    if (maxfd >= 0) {
        struct timeval tv = { 0, 0 };
        if (select(maxfd + 1, &rf, NULL, NULL, &tv) > 0) {
            if (self.m_out >= 0 && FD_ISSET(self.m_out, &rf))
                drain_master(&self, &self.m_out, &self.out_buf, 0);
            for (int i = 0; i < MAX_JOBS; i++) {
                struct job *j = job_get(i);
                if (j->id == 0)
                    continue;
                if (j->m_out >= 0 && FD_ISSET(j->m_out, &rf))
                    drain_master(j, &j->m_out, &j->out_buf, 0);
                if (j->m_err >= 0 && FD_ISSET(j->m_err, &rf))
                    drain_master(j, &j->m_err, &j->err_buf, '!');
            }
        }
    }

    for (int i = 0; i < MAX_JOBS; i++) {
        struct job *j = job_get(i);
        if (j->id != 0 && !j->running && j->m_out < 0 && j->m_err < 0) {
            if (WIFSIGNALED(j->exit_status))
                display_event(j, "exited, signal %d",
                              WTERMSIG(j->exit_status));
            else
                display_event(j, "exited, status %d",
                              WEXITSTATUS(j->exit_status));
            job_free(j);
        }
    }

    display_sync(); /* put the bottom-line owner's partial back */
}

/* Prompt phase: called ~50x/sec while readline waits for input.
   Must be fast, must never block. */
static int drain_hook(void)
{
    /* Non-tty stdin at EOF: readline's event-hook loop treats
       read()==0 as "no input yet" and would spin forever calling
       this hook. Detect the hangup ourselves and wind readline
       down (rl_done accepts the pending line; main() then exits). */
    struct pollfd pf = { .fd = 0, .events = POLLIN, .revents = 0 };
    if (poll(&pf, 1, 0) > 0 &&
        (pf.revents & POLLHUP) && !(pf.revents & POLLIN)) {
        stdin_gone = 1;
        rl_done = 1;
    }

    if (got_sigint) { /* ^C at the prompt: abort the input line */
        got_sigint = 0;
        rl_replace_line("", 0);
        rl_crlf();
        rl_on_new_line();
        rl_redisplay();
    }
    drain_all();
    return 0;
}

/* ---- foreground phase ------------------------------------------ */

static void foreground(struct job *j)
{
    int fgid = j->id;
    int stdin_open = 1;
    /* The line being typed for the foreground job. It is displayed
       as the job's pending input — bottom line "[name:N<] text" —
       so a captured keyboard never masquerades as a shell prompt. */
    struct linebuf rlb;
    memset(&rlb, 0, sizeof rlb);

    /* Raw mode on the real terminal: mtsh echoes typed characters
       itself, through the task-0 pty, so the entry line is ordinary
       partial-line traffic — erased and restored by the display
       like anyone else's. Kernel echo would paint the screen behind
       the display layer's back. ISIG stays on: ^C still reaches our
       handler, which relays it to the job. */
    /* Serial semantics: the foreground job takes over the console.
       Suspend the task-0 default so no "[mtsh:0]" entry line is
       drawn — the prompt reappears when the shell gets the console
       back. */
    display_set_default(NULL, NULL);

    struct termios tio_save, tio_raw;
    int tty_in = isatty(0) && tcgetattr(0, &tio_save) == 0;
    if (tty_in) {
        tio_raw = tio_save;
        tio_raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        tio_raw.c_cc[VMIN] = 1;
        tio_raw.c_cc[VTIME] = 0;
        if (tcsetattr(0, TCSANOW, &tio_raw) < 0)
            tty_in = 0;
    }
    fflush(stdout);

    got_sigint = 0;

    while (j->id == fgid) {
        fd_set rf;
        FD_ZERO(&rf);
        int maxfd = -1;

        if (stdin_open && j->running) {
            FD_SET(0, &rf);
            maxfd = 0;
        }
        if (self.m_out >= 0) {
            FD_SET(self.m_out, &rf);
            if (self.m_out > maxfd) maxfd = self.m_out;
        }
        for (int i = 0; i < MAX_JOBS; i++) {
            struct job *t = job_get(i);
            if (t->id == 0)
                continue;
            if (t->m_out >= 0) {
                FD_SET(t->m_out, &rf);
                if (t->m_out > maxfd) maxfd = t->m_out;
            }
            if (t->m_err >= 0) {
                FD_SET(t->m_err, &rf);
                if (t->m_err > maxfd) maxfd = t->m_err;
            }
        }

        struct timeval tv = { 0, 200000 };
        int r = select(maxfd + 1, &rf, NULL, NULL, &tv);
        if (r < 0 && errno != EINTR) {
            fprintf(stderr, "mtsh: select: %s\n", strerror(errno));
            break;
        }

        /* ^C: relay 0x03 to the foreground job's input pty; its
           line discipline (ISIG) delivers SIGINT to that job's
           process group — and to no other job. mtsh survives. */
        if (got_sigint) {
            got_sigint = 0;
            if (j->id == fgid && j->running)
                write_all(j->m_in, "\003", 1);
        }

        if (r > 0 && stdin_open && j->running && FD_ISSET(0, &rf)) {
            char buf[512];
            ssize_t n = read(0, buf, sizeof buf);
            if (n > 0 && !tty_in) {
                /* non-tty stdin (scripts): relay verbatim */
                write_all(j->m_in, buf, (size_t)n);
            } else if (n > 0) {
                for (ssize_t k = 0; k < n; k++) {
                    unsigned char c = (unsigned char)buf[k];
                    if (c == '\r' || c == '\n') {
                        display_line(j, '<', rlb.data, rlb.len);
                        rlb.data[rlb.len++] = '\n';
                        write_all(j->m_in, rlb.data, rlb.len);
                        rlb.len = rlb.cur = 0;
                    } else if (c == 0x7f || c == '\b') {
                        if (rlb.len > 0)
                            rlb.cur = --rlb.len;
                    } else if (c == VEOF_CH) {
                        write_all(j->m_in, (char[]){ VEOF_CH }, 1);
                    } else if (c == '\t' || c >= 0x20) {
                        if (rlb.len < LINEBUF_CAP - 2)
                            rlb.data[rlb.len++] = (char)c;
                        rlb.cur = rlb.len;
                    } /* other controls dropped; ISIG covers ^C */
                    display_partial(j, &rlb, '<');
                }
            } else if (n == 0) { /* ^D: job's stdin sees EOF */
                write_all(j->m_in, (char[]){ VEOF_CH }, 1);
                stdin_open = 0;
            }
        }

        drain_all(); /* all jobs print through; finalizes fg job too */
    }

    /* rlb is stack memory: release bottom-line ownership before it
       goes out of scope (a half-typed line went nowhere anyway) */
    rlb.len = rlb.cur = 0;
    display_partial(j, &rlb, '<');
    display_set_default(&self, &self.out_buf); /* console back to us */
    if (tty_in && tcsetattr(0, TCSANOW, &tio_save) < 0)
        fprintf(stderr, "mtsh: tcsetattr: %s\n", strerror(errno));
}

/* ---- command line handling ------------------------------------- */

/* Split line in place on blanks/tabs. Returns argc; argv is
   NULL-terminated. Lines with too many words are rejected. */
static int tokenize(char *line, char *argv[MAX_ARGS + 1])
{
    int argc = 0;
    char *save = NULL;

    for (char *tok = strtok_r(line, " \t", &save); tok != NULL;
         tok = strtok_r(NULL, " \t", &save)) {
        if (argc == MAX_ARGS) {
            fprintf(stderr, "mtsh: too many arguments (max %d)\n", MAX_ARGS);
            return -1;
        }
        argv[argc++] = tok;
    }
    argv[argc] = NULL;
    return argc;
}

/* Handle builtins. Returns 1 if the command was a builtin. */
static int builtin(int argc, char **argv)
{
    if (strcmp(argv[0], "exit") == 0) {
        drain_all(); /* flush task 0's own pending output */
        exit(argc > 1 ? atoi(argv[1]) : 0);
    }
    if (strcmp(argv[0], "cd") == 0) {
        const char *dir = argc > 1 ? argv[1] : getenv("HOME");
        if (dir == NULL)
            fprintf(stderr, "mtsh: cd: HOME not set\n");
        else if (chdir(dir) != 0)
            fprintf(stderr, "mtsh: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    if (strcmp(argv[0], "jobs") == 0) {
        /* exited jobs are finalized as soon as their streams drain,
           so everything still in the table is live */
        int any = 0;
        for (int i = 0; i < MAX_JOBS; i++) {
            struct job *j = job_get(i);
            if (j->id != 0) {
                printf("[%d] %s pid %d running\n",
                       j->id, j->name, (int)j->pid);
                any = 1;
            }
        }
        if (!any)
            printf("no jobs\n");
        fflush(stdout);
        return 1;
    }
    if (strcmp(argv[0], "fg") == 0) {
        struct job *j = argc > 1 ? job_find(atoi(argv[1])) : NULL;
        if (j == NULL || !j->running)
            fprintf(stderr, "mtsh: %s\n",
                    argc > 1 ? "fg: no such job" : "usage: fg N");
        else
            foreground(j);
        return 1;
    }
    if (strcmp(argv[0], "in") == 0) {
        if (argc < 3) {
            fprintf(stderr, "mtsh: usage: in N text...\n");
            return 1;
        }
        struct job *j = job_find(atoi(argv[1]));
        if (j == NULL || !j->running || j->m_in < 0) {
            fprintf(stderr, "mtsh: in: no such job\n");
            return 1;
        }
        for (int k = 2; k < argc; k++) {
            write_all(j->m_in, argv[k], strlen(argv[k]));
            write_all(j->m_in, k + 1 < argc ? " " : "\n", 1);
        }
        return 1;
    }
    return 0;
}

int main(void)
{
    self.m_out = -1;
    if (self_setup() < 0)
        fprintf(stderr,
                "mtsh: warning: no pty for task 0, output untagged\n");

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint; /* no SA_RESTART: selects must wake */
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0)
        fprintf(stderr, "mtsh: sigaction: %s\n", strerror(errno));

    rl_event_hook = drain_hook;
    rl_set_keyboard_input_timeout(20000); /* hook every 20ms: snappy
                                             echo of typed chars */

    for (;;) {
        char *line = readline(PROMPT);
        if (line == NULL || stdin_gone) {
            free(line);
            break;
        }
        drain_all(); /* the accepted line's echo ("[mtsh:0] cmd") is
                        sitting in the task-0 pty; print it before
                        anything the command itself causes */

        if (line[strspn(line, " \t")] != '\0')
            add_history(line); /* before tokenize mutates the line */

        char *argv[MAX_ARGS + 1];
        int argc = tokenize(line, argv);

        int bg = 0;
        if (argc > 0) { /* trailing '&': separate token or attached */
            char *last = argv[argc - 1];
            size_t n = strlen(last);
            if (strcmp(last, "&") == 0) {
                bg = 1;
                argv[--argc] = NULL;
            } else if (n > 0 && last[n - 1] == '&') {
                bg = 1;
                last[n - 1] = '\0';
            }
        }

        if (argc > 0 && !builtin(argc, argv)) {
            struct job *j = job_spawn(argv);
            if (j != NULL) {
                display_event(j, "started (pid %d)", (int)j->pid);
                if (!bg)
                    foreground(j);
            }
        }
        free(line);
        drain_all(); /* show this command's own output (task 0 pty)
                        before the next prompt paints */
    }
    drain_all();
    display_raw("\n"); /* clean exit on ^D */
    return 0;
}
