#!/usr/bin/env bash
# run_bench.sh - quantitative comparison of MiniSh against bash/dash/busybox.
#
#   bench/run_bench.sh build/minish.stripped [more shells...]
#   RUNS=50 SCRIPT_RUNS=3 bench/run_bench.sh build/minish.stripped   (quick)
#
# Methodology: docs/06-evaluation.md.  Every shell runs the *same* script
# files (generated below) with /bin/true etc. spelled with a path, so no
# shell can cheat with a built-in; the cost measured is parse + fork +
# exec + wait + job bookkeeping.  Results are appended as a markdown table
# to bench/results/results-<host>-<arch>.md.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
MINISH=$(readlink -f "${1:?usage: run_bench.sh path/to/minish}")
shift
RUNS=${RUNS:-300}            # repetitions for the startup micro-benchmark
SCRIPT_RUNS=${SCRIPT_RUNS:-10}
OUT=$HERE/results
WORK=$OUT/work
mkdir -p "$WORK"
cc -O2 -o "$OUT/spawn" "$HERE/spawn.c" || exit 1

# ----- candidate shells ---------------------------------------------------
shells=("minish=$MINISH")
for s in bash dash "busybox sh" "$@"; do
    bin=${s%% *}
    if command -v "$bin" >/dev/null 2>&1; then
        shells+=("${s// /_}=$(command -v "$bin")${s#"$bin"}")
    fi
done

# ----- workloads ----------------------------------------------------------
gen() { local f=$1 n=$2 line=$3; : > "$f"; for ((i=0;i<n;i++)); do echo "$line" >> "$f"; done; echo "exit" >> "$f"; }
gen "$WORK/w_exec.sh"  500 "/bin/true"
gen "$WORK/w_pipe3.sh" 200 "/bin/true | /bin/true | /bin/true"
gen "$WORK/w_redir.sh" 200 "/bin/echo x > $WORK/out.txt"
gen "$WORK/w_bg.sh"    200 "/bin/true &"
gen "$WORK/w_mixed.sh" 100 "/bin/ls /usr/bin | /bin/grep a | /usr/bin/sort | /usr/bin/head -1 > /dev/null"
: > "$WORK/w_empty.sh"; echo exit >> "$WORK/w_empty.sh"

host=$(hostname -s 2>/dev/null || echo host); arch=$(uname -m)
model=$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')
report=$OUT/results-$host-$arch.md

{
echo "# MiniSh benchmark: $host ($arch)"
echo
echo "- Date: $(date -u +%Y-%m-%dT%H:%MZ)"
echo "- Machine: ${model:-unknown}, $(nproc) CPUs, kernel $(uname -r)"
echo "- Settings: RUNS=$RUNS startup repetitions, SCRIPT_RUNS=$SCRIPT_RUNS script repetitions"
echo
echo "## 1. Static footprint"
echo
echo "| shell | binary (bytes) | shared libs | text+data (bytes) |"
echo "|---|---:|---:|---:|"
for e in "${shells[@]}"; do
    name=${e%%=*}; path=${e#*=}; bin=${path%% *}
    real=$(readlink -f "$bin")
    libs=$(ldd "$real" 2>/dev/null | grep -c '=>' || echo 0)
    td=$(size "$real" 2>/dev/null | awk 'NR==2{print $1+$2}')
    printf '| %s | %s | %s | %s |\n' "$name" "$(stat -c %s "$real")" "$libs" "${td:-?}"
done
echo
echo "## 2. Per-invocation cost (spawn N times, mean)"
echo
echo "Wall and CPU in microseconds per run; RSS = peak resident set in KB."
echo
echo "| workload | shell | wall mean | wall p50 | wall p99 | CPU mean | peak RSS KB | fails |"
echo "|---|---|---:|---:|---:|---:|---:|---:|"
run_row() {   # $1 label $2 name $3 runs $4... command
    local label=$1 name=$2 runs=$3; shift 3
    local r; r=$("$OUT/spawn" -n "$runs" -- "$@") || r="fails=?"
    eval "${r// /;}" 2>/dev/null
    printf '| %s | %s | %s | %s | %s | %s | %s | %s |\n' "$label" "$name" \
        "${wall_us_mean:-?}" "${wall_us_p50:-?}" "${wall_us_p99:-?}" "${cpu_us_mean:-?}" "${maxrss_kb:-?}" "${fails:-?}"
}
for e in "${shells[@]}"; do
    name=${e%%=*}; path=${e#*=}
    # shellcheck disable=SC2086
    run_row "startup (-c exit)" "$name" "$RUNS" $path -c exit
done
for w in w_empty w_exec w_pipe3 w_redir w_bg w_mixed; do
    for e in "${shells[@]}"; do
        name=${e%%=*}; path=${e#*=}
        # shellcheck disable=SC2086
        run_row "$w" "$name" "$SCRIPT_RUNS" $path "$WORK/$w.sh"
    done
done
echo
echo "Per-command overhead = (w_exec - w_empty) / 500;  per-pipeline = (w_pipe3 - w_empty) / 200."
echo

if command -v strace >/dev/null 2>&1; then
    echo "## 3. System calls per script (strace -c -f, whole process tree)"
    echo
    echo "| workload | shell | total syscalls | syscalls per line |"
    echo "|---|---|---:|---:|"
    for w in w_exec w_pipe3; do
        lines=$(( $(wc -l < "$WORK/$w.sh") - 1 ))
        for e in "${shells[@]}"; do
            name=${e%%=*}; path=${e#*=}
            # shellcheck disable=SC2086
            total=$(strace -f -c -o "$WORK/strace.txt" $path "$WORK/$w.sh" >/dev/null 2>&1; awk '/^total/{print $4}' "$WORK/strace.txt")
            printf '| %s | %s | %s | %.1f |\n' "$w" "$name" "${total:-?}" "$(echo "${total:-0} / $lines" | bc -l 2>/dev/null || echo 0)"
        done
    done
    echo
else
    echo "## 3. System calls: strace not installed, skipped"; echo
fi

if command -v valgrind >/dev/null 2>&1; then
    echo "## 4. Leak check (valgrind, minish only)"
    echo
    echo '```'
    valgrind --leak-check=full --track-fds=yes --error-exitcode=9 "$MINISH" "$WORK/w_mixed.sh" 2>&1 >/dev/null | grep -E 'definitely lost|indirectly lost|FILE DESCRIPTORS|ERROR SUMMARY|Open file descriptor' 
    echo '```'
    echo
else
    echo "## 4. Leak check: valgrind not installed, skipped (see tests/pty_tests.py shell_leaks_no_descriptors)"; echo
fi
} | tee "$report"
echo "written: $report"
