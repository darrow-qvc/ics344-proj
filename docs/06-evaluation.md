# 6. Quantitative evaluation methodology

Goal: show that MiniSh is a better fit than bash for an embedded Linux
board (Raspberry Pi) using numbers other than binary size, with a
procedure anyone can repeat with `make bench`.

## 6.1 Metrics

| # | metric | why it matters on an embedded board | how measured |
|---|---|---|---|
| M1 | binary size, number of shared libraries, text+data | flash/rootfs footprint, page-cache pressure, start-up I/O | `stat`, `ldd`, `size` |
| M2 | start-up latency: wall time of `sh -c exit` | a shell is spawned for every `system()`, cron job, init script, hotplug helper; latency adds up | `bench/spawn.c`, 300 repetitions, mean/p50/p99 |
| M3 | CPU time per invocation (user+sys of the whole tree) | the direct proxy for energy and thermal budget on a passively cooled SoC; independent of scheduler noise | `wait4` rusage |
| M4 | peak resident memory (RSS) | Pi Zero has 512 MB shared with the GPU; every daemon's shell counts | `ru_maxrss` |
| M5 | per-command overhead: (time of 500 `/bin/true` lines minus empty script) / 500 | cost of parse + fork + exec + wait + job bookkeeping, the loop of every script | `w_exec` vs `w_empty` |
| M6 | per-pipeline overhead: same with 200 three-stage pipelines | pipe setup and descriptor hygiene | `w_pipe3` |
| M7 | per-background-job overhead: 200 `&` jobs | SIGCHLD handling and job table | `w_bg` |
| M8 | system calls per command | each syscall is a kernel entry; fewer means less work and a smaller attack surface | `strace -f -c`, total divided by lines |
| M9 | correctness parity with bash | utility is only real if behaviour matches | `BASH_PARITY=1 tests/run_tests.sh`: number of identical cases |
| M10 | leaks | long-running shell on a board must not grow | valgrind `definitely lost`, `--track-fds`, fd-count test |

Static memory is also reported: `size build/minish` shows bss (the job
table and parse buffers) so the worst-case memory is known at build time.

## 6.2 Protocol

1. Build with the default flags (`-Os`, gc-sections, stripped).
2. Close other work; on the Pi set the governor to `performance`
   (`echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`)
   so clock scaling does not add noise.
3. Run `make bench` three times; keep the run with the lowest p50 for each
   shell (the others confirm it is reproducible to within 5 %).
4. All shells read the same script files; every external command is
   spelled with a full path so bash cannot substitute a built-in.
5. Report the table from `bench/results/results-<host>-<arch>.md`
   together with `uname -a` and `/proc/device-tree/model`.

The intended comparison set is minish, bash (the default Raspberry Pi OS
shell), dash (`/bin/sh` on Raspberry Pi OS) and busybox sh (what a
minimal Buildroot/OpenWrt image ships).

## 6.3 Reading the results

- M2/M3/M4 are where a small shell wins most: bash links readline and
  ncurses, parses `~/.bashrc`-style start-up, and allocates its own memory
  pools.
- M5 to M7 are dominated by the child process itself; compute the
  difference between shells, not the raw value. The honest claim is
  "MiniSh adds X us of overhead per command versus Y us for bash".
- M9 bounds the claim: MiniSh matches bash on every supported feature; the
  unsupported features (expansion, lists, scripting) are listed in the
  proposal and in docs/01.

## 6.4 Sample results (development machine, not the target)

From `bench/results/results-fedora-x86_64.md`, Intel i5-1235U, Linux 6.19,
`RUNS=100 SCRIPT_RUNS=3`. Pi numbers are to be collected with the same
command and will replace this table in the report.

| metric | minish | bash | ratio |
|---|---:|---:|---:|
| M1 binary (bytes) | 27 504 | 1 415 072 | 51x smaller |
| M1 shared libs | 1 (libc) | 2 (libc, libtinfo) | |
| M2 start-up wall p50 (us) | 1 226 | 2 198 | 1.8x faster |
| M3 start-up CPU mean (us) | 1 018 | 1 941 | 1.9x less |
| M4 peak RSS (KB) | 1 696 | 3 776 | 2.2x less |
| M5 per-command wall (us) | 1 213 | 1 291 | 78 us less |
| M6 per-pipeline wall (us) | 1 625 | 1 962 | 337 us less |
| M7 per-background-job wall (us) | 254 | 371 | 117 us less |
| M9 bash-parity cases | 10 / 10 identical | | |

(M5 to M7 derived from the per-script table: `(w_exec - w_empty)/500`,
`(w_pipe3 - w_empty)/200`, `(w_bg - w_empty)/200`.)
