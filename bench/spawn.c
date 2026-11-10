/*
 * spawn.c - run a command N times and report precise cost per run.
 *
 *   spawn -n N [-q] -- command [args...]
 *
 * For each run: fork, exec, wait4.  Reports (one key=value line):
 *   wall_us_mean/min/p50/p99  wall-clock per run (CLOCK_MONOTONIC)
 *   cpu_us_mean               user+sys CPU time of the child per run
 *   maxrss_kb                 peak resident set size (max over runs)
 *   fails                     runs with non-zero exit status
 *
 * CPU time is the number that matters most on a battery/thermal-limited
 * board: it is what the shell actually makes the CPU do, independent of
 * scheduler noise.  stdin is /dev/null, stdout+stderr /dev/null unless -q
 * is omitted (-q = quiet is the default for fairness).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>

static long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

static int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    int n = 100, i, opt, quiet = 1, fails = 0;
    long *wall, cpu_total = 0, maxrss = 0;
    double wall_total = 0;

    while ((opt = getopt(argc, argv, "n:v")) != -1) {
        if (opt == 'n') n = atoi(optarg);
        else if (opt == 'v') quiet = 0;
    }
    if (optind >= argc || n <= 0) {
        fprintf(stderr, "usage: spawn -n N [-v] -- command [args]\n");
        return 2;
    }
    wall = calloc((size_t)n, sizeof *wall);
    if (!wall) return 2;

    for (i = 0; i < n; i++) {
        long t0 = now_us(), t1;
        int status;
        struct rusage ru;
        pid_t pid = fork();

        if (pid < 0) { perror("fork"); return 2; }
        if (pid == 0) {
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, 0);
                if (quiet) { dup2(devnull, 1); dup2(devnull, 2); }
                close(devnull);
            }
            execvp(argv[optind], &argv[optind]);
            _exit(127);
        }
        if (wait4(pid, &status, 0, &ru) < 0) { perror("wait4"); return 2; }
        t1 = now_us();
        wall[i] = t1 - t0;
        wall_total += (double)wall[i];
        cpu_total += ru.ru_utime.tv_sec * 1000000L + ru.ru_utime.tv_usec
                   + ru.ru_stime.tv_sec * 1000000L + ru.ru_stime.tv_usec;
        if (ru.ru_maxrss > maxrss) maxrss = ru.ru_maxrss;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) fails++;
    }
    qsort(wall, (size_t)n, sizeof *wall, cmp_long);
    printf("runs=%d wall_us_mean=%.1f wall_us_min=%ld wall_us_p50=%ld wall_us_p99=%ld "
           "cpu_us_mean=%.1f maxrss_kb=%ld fails=%d\n",
           n, wall_total / n, wall[0], wall[n / 2], wall[(n * 99) / 100],
           (double)cpu_total / n, maxrss, fails);
    free(wall);
    return 0;
}
