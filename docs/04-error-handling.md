# 4. Failure recovery

Owner: Abdul Wadood Syed (`src/exec/`), with the io and jobs modules for
their own calls. Answers feedback item 6 (recovery part; the repeatable
test commands are in docs/05).

Policy: **a failed command never takes the shell down and never leaks a
descriptor, a job-table slot or a child.** Errors in the child are
reported and the child `_exit()`s (never `exit()`, so the parent's stdio
buffers are not flushed twice and no atexit handler runs twice). Errors in
the parent abort the current line, clean up, set the status, and the loop
continues.

| call | where | failure handled | recovery | status / message |
|---|---|---|---|---|
| `fork` | `ms_fork` | `EAGAIN` (RLIMIT_NPROC, memory) | retried 3x with 10/20/30 ms back-off; then reported | `minish: fork: Resource temporarily unavailable`, pipeline aborted: already-forked stages get SIGTERM and are reaped, pipes closed, status 1 |
| `pipe` | `ms_run_pipeline` | `EMFILE`, `ENFILE` | same abort path as fork; `prev_in` and the half-created pipe are closed | `minish: pipe: Too many open files`, status 1 |
| `dup2` | `ms_child_wire_pipes`, `redirect_fd`, built-in save/restore | `EBADF`, `EINTR` | child: `_exit(1)`; parent (built-in restore): reported, shell continues with whatever fd is valid | `minish: dup2: ...` |
| `open` (redirection) | `redirect_fd` | `ENOENT`, `EACCES`, `EISDIR`, `ENOSPC`... | stage exits 1 before exec, other stages unaffected | `minish: file: No such file or directory` |
| `execvp` | `ms_exec_command` | `ENOENT` without `/` | `_exit(127)` | `minish: name: command not found` |
| | | `ENOENT` with `/` | `_exit(127)` | `minish: ./x: No such file or directory` |
| | | `EACCES` on a directory | `_exit(126)` | `minish: /: Is a directory` |
| | | `EACCES`, `ENOEXEC`, other | `_exit(126)` | `minish: name: Permission denied` (strerror) |
| `tcsetpgrp` | `ms_job_foreground`, `ms_child_setup`, init | `ENOTTY`, `EPERM`, `EINVAL` | child side: ignored (parent repeats it); parent side: reported, job still runs and is still waited for, terminal modes still restored | `minish: tcsetpgrp: ...` |
| `tcsetattr`/`tcgetattr` | same | `EIO` (hangup) | reported, continue | `minish: tcsetattr: ...` |
| `setpgid` | child and parent | `EACCES` (child already exec'd), `ESRCH` (child already exited), `EPERM` (group gone / session leader) | ignored, these are the expected outcomes of the race described in docs/02 | silent |
| `kill(-pgid, SIGCONT)` | `fg`/`bg` | `ESRCH` | reported; the SIGCHLD handler will have marked the job done | `minish: kill (SIGCONT): No such process` |
| `waitpid` | handler | `ECHILD` | loop ends; `errno` restored | silent |
| `read` (input) | `read_line` | `EINTR` | retried | |
| | | other | treated as EOF: shell exits cleanly | `minish: read: ...` |
| `write` (prompt, messages) | everywhere | `EPIPE`/`EIO` | ignored (terminal gone) | |
| `chdir` | `cd` | any | status 1 | `minish: cd: dir: No such file or directory` |
| `getcwd` | `cd` | `ERANGE` | `PWD` simply not updated | silent |
| `fcntl(F_DUPFD_CLOEXEC)` | built-in redirection | `EMFILE` | built-in not run, status 1 | `minish: fcntl: ...` |
| job table full | `ms_job_add` | 32 live jobs | finished-and-reported jobs are purged first; if still full the command is not started | `minish: too many jobs (limit 32)`, status 1 |
| line too long | `read_line` | > 4095 bytes | rest of the line drained and discarded | `minish: line too long (limit 4095 bytes), ignored`, status 2 |

## How to provoke each failure (repeatable)

```sh
# fork failure: lower the process limit in a subshell first
(ulimit -u 20; build/minish -c 'echo a | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat')

# pipe failure: lower the open-file limit
(ulimit -n 6; build/minish -c 'echo a | cat | cat | cat')

# execvp failures (all four classes)
printf 'nosuchcmd\n./nosuchfile\n/\n/etc/passwd\nexit\n' | build/minish

# open failures
printf 'cat < /no/such/file\necho x > /proc/version\nexit\n' | build/minish

# tcsetpgrp failure: run interactively with stdin a tty but stdout not (-i forced)
script -qc 'build/minish < /dev/tty > /dev/null' /dev/null
```

Expected: a one-line `minish: ...` diagnostic for each, the prompt returns,
`jobs` is empty afterwards and `ls /proc/<shell pid>/fd` shows exactly
the three standard descriptors (plus the script fd when reading a file).

## Memory

The shell performs **no heap allocation** on the command path: the token
array, the parsed pipeline, the job table and the line buffer are static.
`malloc` is only reached inside libc (`setenv`, `strsignal`). Therefore an
out-of-memory condition cannot occur in shell code, and valgrind reports
`definitely lost: 0 bytes` for every workload in `bench/`.
