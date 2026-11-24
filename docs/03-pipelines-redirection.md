# 3. Pipelines and redirection: descriptors and exit status

Owner: Talal Alshammari (`src/io/`). Answers feedback item 4.

## 3.1 Descriptor plumbing for `c1 | c2 | c3`

Pipes are created one stage at a time. `prev_in` is the read end of the
pipe feeding the current stage.

```
stage 0: pipe(p0)        parent holds: p0[0] p0[1]
         fork C1         C1: dup2(p0[1],1); close p0[1]            -> 0=tty 1=p0w
         parent: close p0[1]; prev_in = p0[0]   holds: p0[0]
stage 1: pipe(p1)        parent holds: p0[0] p1[0] p1[1]
         fork C2         C2: dup2(p0[0],0); dup2(p1[1],1);
                             close p0[0] p1[0] p1[1]               -> 0=p0r 1=p1w
         parent: close p0[0]; close p1[1]; prev_in = p1[0]   holds: p1[0]
stage 2: (no pipe)
         fork C3         C3: dup2(p1[0],0); close p1[0]             -> 0=p1r 1=tty
         parent: close p1[0]                                  holds: nothing
```

Rules enforced by `ms_child_wire_pipes` and `ms_run_pipeline`:

1. **Parent closes the write end right after forking the writer.** If it
   kept `p0[1]`, `C2` would never see EOF and `cat f | sort` would hang.
2. **Parent closes the read end right after forking the reader.** Keeping
   it would just leak, and would also stop `C1` from receiving SIGPIPE if
   `C2` died early.
3. **Each child closes every pipe descriptor it inherited** after the
   `dup2`s: its own two ends and any other ends still open in the parent
   at fork time. The first version of this code forgot `prev_in` and the
   test below caught it (stage 2 saw an extra fd 4).
4. At most three pipe descriptors are open in the parent at any moment,
   regardless of pipeline length, so a 16-stage pipeline cannot exhaust
   the descriptor table.

### Descriptor-leak checks

`tests/cases/09_fdleak.sh` runs `ls /proc/self/fd` as the first, middle and
last stage. The expected listing is always `0 1 2 3` (3 is the directory
handle `ls` opens to read `/proc/self/fd` itself):

```
echo | cat | ls /proc/self/fd | cat | cat     ->  0 1 2 3
echo | ls /proc/self/fd > fds.txt; cat fds.txt ->  0 1 2 3
```

`tests/pty_tests.py::shell_leaks_no_descriptors` counts the **shell's own**
descriptors in `/proc/<pid>/fd` before and after 40 pipelines and
redirections, including failing ones (`nosuchcommand`, missing input
file); the count must be identical.

Under valgrind, `--track-fds=yes` should list only 0, 1, 2 at exit
(`make bench` runs it when valgrind is installed).

## 3.2 Redirections

Applied in the child by `ms_apply_redirections`, **after** the pipe wiring
and strictly left to right:

| kind | `open()` flags | mode |
|---|---|---|
| `< f` | `O_RDONLY` | |
| `> f` | `O_WRONLY \| O_CREAT \| O_TRUNC` | 0666 & ~umask |
| `>> f` | `O_WRONLY \| O_CREAT \| O_APPEND` | 0666 & ~umask |

then `dup2(fd, 0 or 1)` and `close(fd)`. If `open` happens to return the
target descriptor itself (possible only if 0/1 were closed), no `dup2` is
done and nothing is closed.

### Collision table

| line | effect | same as bash |
|---|---|---|
| `cmd > a > b` | `a` created empty, output in `b` | yes |
| `cmd < a < b` | reads `b` | yes |
| `cmd < f > f` | `f` truncated before `cmd` reads it: reads empty file | yes |
| `cmd >> f > f` | truncates (last wins) | yes |
| `a \| b < f` | `b` reads `f`; `a` gets SIGPIPE/EPIPE when it writes | yes |
| `a > f \| b` | `b` reads an empty pipe and sees EOF | yes |
| `cd /tmp > f` | built-in runs in the shell with fd 1 temporarily redirected, then restored | yes |

Built-ins executed in the shell save fds 0/1 with
`fcntl(F_DUPFD_CLOEXEC, 10)`, apply the redirections, run, and `dup2` the
saved descriptors back, so a redirection on `cd`/`jobs` never changes the
shell's own stdin/stdout.

## 3.3 Failure and exit-status behaviour

- **Exit status of a pipeline** is the exit status of its **last** command
  (bash default; there is no `pipefail`). `true | false` yields 1,
  `false | true` yields 0. `exit` with no argument returns the last status,
  so this is testable without `$?` (`tests/cases/04`, `05`, `13`).
- **A failing stage does not abort the others.** `nosuchcommand | cat`
  prints `command not found`, `cat` reads EOF and exits 0, status is 0,
  exactly as bash. `echo a | nosuchcommand | cat`: `echo` may get SIGPIPE
  or finish first; either way the job completes and no zombie remains.
- **Redirection failure** (missing input file, unwritable output path)
  is reported as `minish: path: strerror` and that stage exits with
  status 1 before `execvp`. Other stages run normally.
- **Signalled stage**: if the last stage dies of a signal, status is
  `128 + signo`; in interactive mode the signal name is printed
  (`Terminated`, `Killed`), nothing for SIGPIPE, just a newline for
  SIGINT, matching bash.
- **`pipe()` or `fork()` fails mid-way** (EMFILE, EAGAIN): stages already
  forked are sent SIGTERM and reaped; all pipe descriptors created so far
  are closed; status is 1. See docs/04.
- The job is removed from the table only when **every** stage has been
  reaped, so a pipeline where the last command exits first (`yes | head -1`)
  still waits for `yes` to die of SIGPIPE and reaps it.
