# basht — adapting bash to the line-multiplexed display model

Analysis of what it takes to insert the mtsh mechanism (tsh_spec.md,
proven by the mtsh prototype) into GNU bash. Source references are to
the bash devel tree (savannah), which matches the 5.x layout.

## Verdict up front

Feasible as a moderate, well-contained patch. Bash's architecture
cooperates in three lucky ways:

1. **One fork point.** Every job child is created by
   `make_child(char *command, int flags)` (jobs.c:2265) — disk
   commands, subshells, coprocs alike. The pty-attach code goes in
   exactly one place.
2. **Child fd order is already right.** In the child,
   `execute_disk_command` runs: piping (`do_piping`) → redirections
   (`do_redirections`) → `shell_execve` (execute_cmd.c:5834ff). If
   the pty trio is attached as the *default* fds 0/1/2 immediately
   after fork, pipes and explicit redirections override it
   naturally. Only streams that would have reached the terminal get
   captured — command substitution pipes, `2>file`, `a | b`
   internals all keep working untouched.
3. **`rl_event_hook` is free.** Bash uses `rl_signal_event_hook`
   (bashline.c:658) but not `rl_event_hook`, so the mtsh prompt-phase
   drain hook drops straight in. Bash even already routes readline
   output through a settable stream (`rl_outstream = stderr`,
   bashline.c:461) — rerouting it into the task-0 pty is one line.

The mtsh prototype de-risked the two parts that were genuinely
uncertain: reconstructing readline's echo through a pty with the
one-line terminal emulator (works, including history/editing via
CSI K/C/D/G/P/@/X), and the bottom-line ownership display (works,
including serial foreground takeover).

## What lifts verbatim

Per spec §11 these were written to be liftable, and they are:

| mtsh module | into bash as | changes needed |
|---|---|---|
| `filter.c` (escape strip + one-line emulator) | new file `tag_filter.c` | none |
| `display.c` (bottom-line ownership, tags, erase/redraw) | new file `tag_display.c` | tag format only (see naming) |
| `linebuf` struct | `tag_filter.h` | none |
| task-0 self-pty setup (`self_setup`) | into `shell.c` startup | wire to `rl_outstream` |
| pty trio allocation (`pty_master` + attach) | into `jobs.c` | child side simplifies (see signals) |

## Where bash must change

### jobs.c — the heart of the patch

- **`struct job` (jobs.h)** gains the mtsh fields: `m_in, m_out,
  m_err` master fds and two `linebuf`s. Attribution is per JOB
  (= pipeline), which matches the spec: the tag names the job, not
  each process. Suggested tag: first word of the job's `command`
  string plus job number — `[make:2]`, `[cc:3!]` — bash already
  stores the command text per process.
- **`make_child`**: parent side allocates the three ptys when (and
  only when) the fork is creating a jobs-table child in an
  interactive shell (`interactive_shell && subshell_environment == 0`
  gate — this excludes command substitution, process substitution
  and script execution automatically). Child side: attach slaves to
  fds 0/1/2.
- **`wait_for` / `waitchld`**: the foreground blocking wait
  (blocking `waitpid` at jobs.c:4064ff) becomes the mtsh foreground
  phase: a `select()` loop over the real stdin + all live masters,
  draining/displaying, relaying typed input to the fg job's `m_in`,
  reaping with WNOHANG per pass. This is the largest single piece of
  surgery, but mtsh's `foreground()` is the working reference,
  and `waitchld` already supports WNOHANG operation.
- **`give_terminal_to` / `maybe_give_terminal_to`** (jobs.c:733,
  2400, 3253...): neutralized. Nobody but the shell ever owns the
  real terminal again — that is the whole point of the design. These
  become no-ops when the multiplexer is active.
- **Job cleanup**: drain masters to EIO before closing (mtsh
  `drain_all` finalization), then the normal `delete_job`.

### Signals — a real design choice

mtsh follows spec §3: each child does `setsid()` + `TIOCSCTTY` on
its in-pty, and `^C` is delivered by writing 0x03 to the master so
the pty's ISIG does the killing. That works because each mtsh job is
a single child. Bash pipelines are *sibling* forks, and siblings
cannot share a session created by `setsid()` in one of them — the
ISIG trick would signal only the pipeline leader.

**Option A (spec-faithful):** keep setsid+ctty, accept that only the
job leader's process group gets pty-delivered signals. Broken for
`a | b` where `b` ignores nothing.

**Option B (bash-native, recommended for v1):** drop setsid/ctty
entirely. Bash already places every pipeline in its own process
group (`pipeline_pgrp` via `setpgid`) — keep that, keep the children
in the shell's session, open all three pty slaves `O_NOCTTY`, and
deliver `^C`/`^Z` as `killpg(job->pgrp, SIGINT/SIGTSTP)` from
basht's own SIGINT path. The pty is then purely a capture/identity
device; signal routing uses machinery bash has had for 30 years.
`isatty()` stays true (line buffering preserved — the load-bearing
property). Cost: a job's `/dev/tty` still names the real terminal
(rare programs that read passwords from `/dev/tty` would fight the
shell for the keyboard; documented limitation, same class as the
spec's full-screen exclusions).

### bashline.c / shell.c — display integration

- Startup (interactive only): create the task-0 pty, dup the real
  terminal to a private fd owned by `tag_display.c`, route the
  shell's own stdout/stderr through the task-0 slave, set
  `rl_outstream = stdout` (replacing the `stderr` assignment at
  bashline.c:461).
- Install the drain hook: `rl_event_hook = tag_drain_hook;` with a
  short keyboard timeout, exactly as mtsh.
- The prompt: bash renders `$PS1` (via `decode_prompt_string`) and
  readline echoes it into the task-0 stream, where it becomes the
  shell's partial line — displayed as `[bash:0] <PS1><input>` at the
  bottom. PS1 color escapes get stripped by the filter (documented:
  no color in v1). No readline redisplay-function replacement is
  needed — mtsh proved reconstruction-from-the-byte-stream works.
- **Serial foreground rule** (established in mtsh): while a
  foreground job runs, suspend the task-0 default owner; the console
  scrolls plainly and typed input shows as `[job:N<]` pending input.

### sig.c — ^C routing

Interactive SIGINT currently reaches bash and its terminal pgrp
children together. Under basht: at the prompt, keep bash's behavior
(abort the input line). During a foreground job, the handler does
`killpg(fg_job->pgrp, SIGINT)` (option B) instead of relying on
shared terminal ownership. SIGCHLD handling is untouched — bash's
reaper already coexists with WNOHANG sweeps.

### builtins

- `jobs` — add the tag/id column; otherwise unchanged (bash's table
  is richer than mtsh's).
- `fg` — becomes "route keyboard + signals" (no `give_terminal_to`,
  no process-group terminal transfer). `bg` already just SIGCONTs.
- new `in N text...` — trivial: write to job N's `m_in`.

## What is explicitly not captured

- Non-interactive shells and scripts: the entire mechanism gates on
  `interactive_shell`; `bash -c` and script execution are untouched.
- Command/process substitution children: excluded by the
  `subshell_environment` gate; their pipes are wired after the pty
  defaults anyway.
- Explicitly redirected streams: `do_redirections` runs after pty
  attach, so `>file` wins, as it should.

## Phasing (each phase independently demonstrable)

1. **Task 0 only.** Shell's own output through its pty, tagged
   `[bash:0]`, display + filter modules in, drain hook installed.
   Children untouched (still plain serial). Low risk, proves the
   modules compile/live inside bash.
2. **Background jobs captured.** Pty trios for `&` jobs only;
   foreground jobs keep the real terminal exactly as stock bash.
   This already delivers the headline demo (background `make -j8`
   printing through the prompt, tagged) with foreground semantics
   bit-for-bit identical to stock bash.
3. **Foreground jobs captured.** Replace the blocking `wait_for`
   with the select loop; raw-mode input relay; `[job:N<]` input
   display; serial takeover display rule; `killpg` signal routing.
4. **Polish.** `in` builtin, `jobs` tags, drain-to-EIO on cleanup,
   valgrind, docs.

## Risks and open questions

- `wait_for` is called from several contexts (traps, `wait`
  builtin, subshells); the select-loop replacement must apply only
  to the interactive foreground path (`wait_for_job` from
  `execute_command_internal`), leaving the others blocking.
- Bash's SIGCHLD-driven `waitchld` can reap from inside the handler;
  mtsh's poll-only model avoided reentrancy — basht should keep
  bash's queueing (`queue_sigchld`) discipline and only add the
  drain pass outside it.
- Stopped jobs (`^Z`): option B's `killpg(SIGTSTP)` should work with
  bash's existing stopped-job bookkeeping, but the notify path
  (`notify_of_job_status`) needs routing through the display layer.
- Readline redisplay against very long lines / resized terminals:
  same v1 limitations as mtsh (single-row entry line, no SIGWINCH
  forwarding — spec §7/§11).
- Every other place bash writes directly to stderr (error messages,
  `xtrace`) lands in the task-0 stream and gets tagged `[bash:0]` —
  almost certainly desirable, but xtrace under load will be chatty.

## Scope estimate

- New files: `tag_filter.c/h`, `tag_display.c/h` (~500 lines, lifted).
- jobs.c/jobs.h: ~300-400 lines touched (pty alloc, make_child child
  path, wait_for select loop, cleanup).
- bashline.c, shell.c, sig.c: ~100 lines combined.
- builtins: ~50 lines.

The mtsh prototype remains the living spec for all display/filter
behavior; when in doubt, basht should reproduce mtsh's observable
output byte-for-byte.
