# tsh — Tagged Shell Prototype

## Implementation specification, v1

**Target:** Linux only. C (C99 or later). Single binary. GNU readline as the only
external dependency beyond libc.

**License/output:** single repo containing `tsh.c` (may be split into a few .c/.h
files if cleaner), `Makefile`, `README.md`, and a `test/` directory as described
in §9.

---

## 1. Purpose and model

tsh is a proof-of-concept command shell that fixes the multitasking display
model of the conventional Unix CLI. In a normal terminal, output from
concurrent processes interleaves at the character level into one merged,
unattributed stream, and background output corrupts the line the user is
typing.

tsh implements the line-multiplexed model (per the Univac / OS/Z design):

1. **Every spawned command gets its own pseudoterminals** — it never writes to
   the real terminal. The kernel therefore enforces output attribution: tsh
   always knows which process produced which bytes, because they arrive on
   that process's pty.
2. **Output is displayed in whole lines, each prefixed with an identity tag**
   identifying the originating job.
3. **A per-stream line buffer** accumulates each stream's partial line.
   Nothing is displayed until a stream completes a line (`\n`).
4. **Completed lines "print through" the user's entry line**: the entry line
   is cleared, the tagged completed line is written (scrolling the terminal),
   and the entry line (prompt + whatever the user had typed) is redrawn
   beneath it, undisturbed.

The result: any number of background jobs can produce output concurrently
while the user types at a stable prompt, and every line on screen says which
job it came from.

**Explicit non-goals for v1:** no escape-sequence interpretation, no
full-screen program support (vim, top — see §7), no pipelines between jobs, no
shell scripting language, no thread-level attribution (process-level only), no
job control signals beyond those listed in §6.

---

## 2. Definitions

- **Job**: one spawned command. Identified by a small integer job id `N`
  (monotonically increasing from 1) and a name (basename of the command's
  first word).
- **Tag**: the prefix on every displayed line. Format:
  - stdout line: `[name:N] `
  - stderr line: `[name:N!] `
  - job lifecycle events (start/exit): `[name:N*] `
  Field widths are not fixed; the tag is followed by exactly one space.
- **Entry line**: the bottom line of the terminal, owned by readline: the tsh
  prompt plus the user's in-progress input.
- **Stream**: one of a job's two output channels (stdout pty, stderr pty).
  Each stream has its own line buffer.

---

## 3. Process plumbing

For each job, tsh allocates **three pseudoterminal pairs** before forking:

| pty | child fd | controlling? | purpose |
|---|---|---|---|
| in  | 0 | **yes** | keyboard input relayed by tsh; signal generation |
| out | 1 | no (`O_NOCTTY`) | stdout capture |
| err | 2 | no (`O_NOCTTY`) | stderr capture |

Child-side sequence after `fork()`:

```c
setsid();                                  /* new session, no ctty */
int s_in  = open(ptsname_in,  O_RDWR);     /* becomes candidate ctty */
ioctl(s_in, TIOCSCTTY, 0);                 /* input pty = controlling tty */
int s_out = open(ptsname_out, O_RDWR | O_NOCTTY);
int s_err = open(ptsname_err, O_RDWR | O_NOCTTY);
dup2(s_in, 0); dup2(s_out, 1); dup2(s_err, 2);
/* close all other fds above 2 */
execvp() or execl("/bin/sh", "sh", "-c", cmdline, ...);
```

Requirements:

- Use `posix_openpt(O_RDWR | O_NOCTTY)`, `grantpt`, `unlockpt`, `ptsname`.
- Command parsing is **delegated to `/bin/sh -c <cmdline>`**. tsh does no word
  splitting, globbing, or quoting of its own, with the single exception of
  detecting a trailing `&` (background) and stripping it before handing the
  rest to `sh -c`. Note the attribution consequence: the job's processes are
  whatever `sh -c` spawns; the tag names the job, not each descendant.
- The out/err ptys exist (rather than pipes) so that `isatty(1)` is true in
  the child and stdio remains **line-buffered** — this property is
  load-bearing; do not substitute pipes.
- Set each pty slave to sane cooked-mode termios defaults. Disable `ECHO` on
  the **out** and **err** ptys (nothing reads them back). Leave the **in**
  pty's line discipline in normal cooked mode with `ISIG` enabled — this is
  what makes `^C` delivery work (§6).
- On the **real** terminal, tsh runs readline normally (readline manages its
  own termios).

Master fds are registered in the job table:

```c
struct job {
    int   id;                 /* 1, 2, 3, ... */
    char  name[NAME_MAX];     /* basename of first command word */
    pid_t pid;                /* pid of the sh -c child */
    int   m_in, m_out, m_err; /* pty master fds */
    struct linebuf out_buf, err_buf;
    int   running;            /* cleared by SIGCHLD reaper */
    int   exit_status;
};
```

A fixed table of 64 jobs is sufficient for v1.

---

## 4. Line buffers and the escape filter

Each stream has a line buffer (suggested capacity 8 KiB; on overflow, force a
line break and continue — never drop bytes silently, and append a literal
`" [wrapped]"` marker to the forced line).

All bytes read from an out/err master pass through two stages **in order**:

**Stage 1 — escape stripping.** tsh is CLI-only: escape sequences are
*removed*, never interpreted or displayed. Implement a small state machine
that deletes:

- CSI sequences: `ESC [` … through final byte `0x40–0x7E` (parameters and
  intermediates `0x20–0x3F` consumed)
- OSC sequences: `ESC ]` … through `BEL` or `ESC \`
- Two-character escapes: `ESC` followed by any byte in `0x40–0x5F` not covered
  above (consume both)
- Bare `ESC` at end of read: **held in the stream's filter state** until the
  next read — sequences split across read boundaries must not leak partial
  fragments into the display.
- All other C0 control bytes except `\n`, `\t` are deleted. `\r` is deleted
  (see below). `\t` passes through.

**Stage 2 — line assembly.** Filtered printable bytes append to the stream's
buffer. On `\n`, the buffer contents constitute a **completed line** and are
handed to the display layer (§5), and the buffer resets. `\r\n` therefore
reduces to `\n` (the `\r` was deleted in stage 1); a bare `\r` (progress-bar
style rewriting) simply vanishes, and such programs' successive rewrites will
each arrive as they complete with `\n` or on EOF flush — acceptable for v1.

On stream EOF (master read returns 0 or `EIO`): if the buffer is non-empty,
emit it as a completed line (a final unterminated line must not be lost).

---

## 5. Display layer

The single point that writes to the real terminal. Two states:

**At the prompt (readline active).** Completed lines arrive via the
`rl_event_hook` path (§8). To display a batch of completed lines:

```c
rl_clear_visible_line();        /* erase prompt + partial input from screen */
/* write each pending line:  tag, space, line text, "\n"  */
rl_on_new_line();
rl_forced_update_display();     /* redraw prompt + user's typed input */
```

The user's in-progress input is preserved exactly; only its screen position
moves down as output scrolls above it.

**During a foreground job (readline not active).** Completed lines are written
directly with `write(1, ...)`. The foreground job's own stdout lines are
still tagged like any other (uniformity beats cleverness in v1).

Job lifecycle events use the `*` tag form and flow through the same path:

```
[cc:3*] started (pid 41172)
[cc:3*] exited, status 0
```

All display writes go through one function; nothing else in tsh writes to
fd 1.

---

## 6. Input routing, foreground/background, signals

- A command line ending in `&` starts a **background** job: tsh returns to the
  prompt immediately.
- Otherwise the job is **foreground**: tsh leaves readline, and until the job
  exits, lines read from the real stdin are written verbatim (with `\n`) to
  the foreground job's `m_in`. Background jobs continue printing through
  during this time (§8 loop). For v1, foreground input is line-oriented —
  read with a plain `fgets`/`read` loop on the real stdin, not raw-mode
  character relay.
- **`^C` at the prompt**: readline's default (abort current input line);
  never signals any job.
- **`^C` while a foreground job runs**: tsh catches SIGINT and writes byte
  `0x03` to the foreground job's `m_in`. The input pty's line discipline
  (ISIG) then delivers SIGINT to the job's process group — and to no other
  job. tsh itself must not die: install a SIGINT handler for the duration of
  foreground execution.
- **EOF (`^D`) while a foreground job runs**: close nothing; write `VEOF`
  (0x04) to `m_in` so the job's stdin sees EOF.
- **SIGCHLD**: reap with `waitpid(-1, &st, WNOHANG)` in a loop; mark jobs
  exited, record status. Do not display from the handler — set a flag; the
  main loop emits the `exited` event line (self-pipe or `pselect` pattern to
  avoid races).
- After a job exits, continue draining its out/err masters until EOF so no
  tail output is lost, then close all three masters.

Builtins (interpreted by tsh before `sh -c` delegation), minimal set:

| builtin | behavior |
|---|---|
| `exit` | exit tsh (SIGHUP will reach children via their ptys naturally) |
| `cd [dir]` | chdir of tsh itself, so subsequent jobs inherit it |
| `jobs` | list job table: id, name, pid, running/exited+status |
| `fg N` | make job N the foreground job (input relay + `^C` target) |
| `in N text...` | write one line of text to job N's stdin without foregrounding it |

`fg` on tsh means only "route keyboard and signals there" — there is no
terminal ownership to transfer, which is the point of the design.

---

## 7. Out of scope, enforced

- If a job emits alternate-screen or cursor-addressing sequences, they are
  simply stripped (§4); the program will misbehave on its own pty and that is
  acceptable. README should state plainly: tsh is for line-oriented commands.
- No `TIOCSWINSZ` forwarding in v1 (children see default pty size; harmless
  for line output).
- No color pass-through: SGR is stripped like everything else.

---

## 8. Event loop

Single-threaded. Two phases, both driven by `select()` over all live
`m_out`/`m_err` fds (and `m_in` fds never — tsh only writes those):

**Prompt phase.** readline is in control; asynchronous draining happens via:

```c
rl_event_hook = drain_ptys_hook;   /* called ~10x/sec while readline waits */
```

The hook performs a zero-timeout `select()` over all masters, runs stages
1–2 on whatever is readable, and if any completed lines resulted, executes the
clear/print/restore sequence of §5. It also checks the SIGCHLD flag and emits
exit events. The hook must be fast and must never block.

**Foreground phase.** A conventional loop:

```c
while (fg job running) {
    select(real_stdin + all masters, timeout 200ms);
    /* readable master  → stages 1-2 → display directly */
    /* readable stdin   → read line → write to fg m_in  */
    /* each pass        → waitpid(-1, WNOHANG) sweep    */
}
```

No threads anywhere. readline state is only touched from the prompt phase.

---

## 9. Tests (deliver as `test/` with a driver script)

Provide `test/writer.c` — a small program taking `name interval_ms count`
that writes numbered lines to stdout (and every third line to stderr), used
by the scripted tests:

1. **Attribution**: start three background writers with different intervals;
   verify (by capturing tsh's output with `script(1)` or a pty harness) that
   every emitted line carries the correct `[name:N]`/`[name:N!]` tag and that
   no line is ever interleaved mid-line with another.
2. **Print-through**: programmatic pty harness types a partial command at the
   prompt while a background writer runs; verify the typed characters are
   intact in readline's buffer afterward (send `\n` and check the executed
   command), and that writer lines appeared meanwhile.
3. **Escape stripping**: writer variant emits SGR color, a split-across-write
   CSI sequence, and OSC titles; verify none of `ESC` bytes appear in output
   and text content is preserved.
4. **Signal isolation**: foreground `sleep 100`; send `^C`; verify the sleep
   dies, tsh survives, and a concurrently running background writer is
   untouched and keeps printing.
5. **EOF flush**: writer that exits with an unterminated final line; verify
   the partial line is displayed before the exit event.
6. **Exit events**: verify `started`/`exited, status` lines with correct
   statuses (test one job exiting 0 and one exiting nonzero).

A plain `make test` should build and run all of it. Manual smoke test to
document in the README: `./tsh`, then `make -C /some/project -j8 &`, keep
typing at the prompt.

---

## 10. Structure and quality expectations

- Suggested layout: `main.c` (loop, readline), `job.c` (table, spawn, reap),
  `filter.c` (escape strip + line assembly), `display.c` (the single output
  point). Header per module. No global state outside these modules'
  translation units except the job table.
- Every syscall checked; on pty allocation failure, report and refuse the
  job, don't exit tsh.
- `valgrind --leak-check=full` clean on a session that starts and reaps
  several jobs.
- Compile clean with `-Wall -Wextra` on gcc and clang.
- README: what it is (three paragraphs, the model above), build, usage,
  builtins table, limitations (§7), and the make -j smoke test.

## 11. Explicitly deferred (v2+ notes, do not implement)

- Thread-level tags for cooperating runtimes (requires a client library).
- Partial-line live display region above the prompt.
- Insertion of the mechanism into bash source (jobs.c / execute_cmd.c /
  rl_event_hook path — this prototype's display and filter modules should be
  written to be liftable for that).
- `TIOCSWINSZ` forwarding and terminal resize handling.
- Configurable tag format.
