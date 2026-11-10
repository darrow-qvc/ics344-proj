# MiniSh: a small Unix shell with job control

ICS 433 (Operating Systems Lab) project, idea #7. C11, POSIX, no
dependencies beyond libc. Target: Raspberry Pi (any model) and desktop
Linux. Stripped binary is about 27 KB; the shell performs no heap
allocation while running commands.

```sh
make                 # build/minish, build/minish.stripped, prints size
make test            # regression suite + interactive (pty) job-control suite
make bench           # compare against bash/dash/busybox, writes bench/results/*.md
make debug           # ASan/UBSan build in build-debug/
make STATIC=1        # fully static binary for a minimal rootfs
./build/minish       # interactive;  ./build/minish script.sh;  ./build/minish -c 'cmd'
```

Cross-compiling for a Pi from a PC: `make CC=aarch64-linux-gnu-gcc STATIC=1`
(or `arm-linux-gnueabihf-gcc` for 32-bit OS images), then copy
`build/minish.stripped` over.

## Features

- External commands via PATH with arguments; quoting `'...'`, `"..."`, `\`
- Built-ins: `cd`, `exit`, `jobs`, `fg`, `bg`
- Redirection `<`, `>`, `>>` and pipelines up to 16 stages
- Background jobs with `&`, completion/stop notifications, `%n` job specs
- Full terminal job control: Ctrl+C / Ctrl+Z affect only the foreground
  job, `fg` restores a stopped job's terminal modes, no zombies
- Error messages and exit statuses (1, 2, 126, 127, 128+n) match bash

Not supported, by design: variables and expansion, globbing, `;`, `&&`,
`||`, `<<`, scripting constructs.

## Layout and ownership

| path | owner | contents |
|---|---|---|
| `src/parser/` | Karrar Alqallaf | `lexer.c` tokenizer, `parser.c` grammar, `main.c` prompt loop and line reader |
| `src/exec/` | Abdul Wadood Syed | `exec.c` fork/execvp/dispatch, `builtins.c` cd/exit/table |
| `src/io/` | Talal Alshammari | `redirect.c`, `pipeline.c` |
| `src/jobs/` | Husain Althagafi | `jobs.c` job table/fg/bg/jobs, `signals.c` SIGCHLD, process groups, terminal |
| `include/minish.h` | shared | all data structures and the four module interfaces |
| `src/common/util.c` | shared | diagnostics helpers |
| `docs/` | see each file | design docs, one per feedback item |
| `tests/` | shared | `run_tests.sh` + `cases/`, `pty_tests.py` |
| `bench/` | shared | `spawn.c`, `run_bench.sh`, results |
| `scripts/export-member.sh` | | packs one member's portion for sending |

`make export-karrar` (or `abdulwadood`, `talal`, `husain`) writes
`dist/minish-<member>.tar.gz` containing the whole tree plus a
`PORTION.md` listing that member's files, and `dist/minish-<member>.patch`
with only their changes, so each person can review and present their own
part. Git history is also grouped by member (`git log --grep='\[jobs\]'`).

## Where each piece of reviewer feedback is answered

| feedback | document | code | tests |
|---|---|---|---|
| 1. grammar, tokenization, precedence, errors, max sizes | docs/01-grammar.md | `src/parser/` | cases 01, 07, 11, 12, 16 |
| 2. process-group and terminal handoff sequence, SIGINT/SIGTSTP | docs/02-job-control.md 2.2 to 2.4 | `src/jobs/signals.c`, `jobs.c` | pty_tests.py |
| 3. SIGCHLD/job-table races, masking, waitpid flags | docs/02-job-control.md 2.5 | `ms_block_sigchld`, `wait_for_job`, handler | case 10, pty quick-exit test |
| 4. pipeline exit status, closing descriptors, leak checks | docs/03-pipelines-redirection.md | `src/io/pipeline.c` | cases 04, 05, 09, pty fd test |
| 5. tests: pipelines, redirection collisions, missing commands, stopped/resumed jobs, quick exits | docs/05-testing.md | | whole suite |
| 6. failure recovery for fork/pipe/dup2/execvp/open/tcsetpgrp, repeatable commands | docs/04-error-handling.md, docs/05 | `ms_fork`, `ms_run_pipeline` abort path, `redirect_fd` | cases 03, 06, 13 to 15 |
| embedded goal and evaluation vs bash | docs/06-evaluation.md | `-Os` build, static buffers | `make bench` |
