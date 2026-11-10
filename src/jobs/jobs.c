/*
 * jobs.c - job table, fg/bg/jobs, foreground wait.  [jobs] Husain Althagafi
 *
 * Concurrency model (docs/02-job-control.md, section "Races"):
 *
 *   The job table is touched from two contexts: the main program and the
 *   SIGCHLD handler.  The rule is simple:  the main program only touches
 *   the table while SIGCHLD is blocked.  The handler therefore never
 *   interrupts a half-finished table update, and it never sees a child
 *   that has not been registered yet, because fork() itself happens with
 *   SIGCHLD blocked.  Waiting for a foreground job is done with
 *   sigsuspend(), which atomically unblocks SIGCHLD and sleeps: a SIGCHLD
 *   that arrived while blocked (quick-exiting child) is delivered at that
 *   instant, so the "child exited before we started waiting" case cannot
 *   be lost.
 *
 *   The handler only reads/writes plain ints and pid_t fields of the
 *   table and calls waitpid(); all of that is async-signal-safe.
 *   Nothing is printed from the handler: completion messages are printed
 *   by ms_jobs_notify() right before the prompt.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include "minish.h"

struct ms_job   ms_jobs[MS_MAX_JOBS];
int             ms_interactive = 0;
int             ms_terminal    = STDIN_FILENO;
pid_t           ms_shell_pgid  = 0;
struct termios  ms_shell_tmodes;
int             ms_last_status = 0;

/* ---------------------------------------------------------------- */
/* Output helpers: a single write() per message, no stdio buffering  */
/* ---------------------------------------------------------------- */
static void out(const char *s, size_t n)
{
    if (write(STDOUT_FILENO, s, n) < 0) { /* terminal gone */ }
}

/* ---------------------------------------------------------------- */
/* Table management -- caller holds SIGCHLD blocked                  */
/* ---------------------------------------------------------------- */
static void purge_done(int all)
{
    int i;
    for (i = 0; i < MS_MAX_JOBS; i++)
        if (ms_jobs[i].id && ms_jobs[i].state == MS_JOB_DONE &&
            (all || ms_jobs[i].notified))
            ms_job_free(&ms_jobs[i]);
}

static int next_id(void)
{
    int i, max = 0;
    for (i = 0; i < MS_MAX_JOBS; i++)
        if (ms_jobs[i].id > max)
            max = ms_jobs[i].id;
    return max + 1;
}

static struct ms_job *free_slot(void)
{
    int i;
    for (i = 0; i < MS_MAX_JOBS; i++)
        if (ms_jobs[i].id == 0)
            return &ms_jobs[i];
    return NULL;
}

struct ms_job *ms_job_add(const char *text, int background)
{
    struct ms_job *j = free_slot();
    size_t n;

    if (!j) {
        purge_done(!ms_interactive);
        j = free_slot();
        if (!j)
            return NULL;
    }
    memset(j, 0, sizeof *j);
    j->id = next_id();
    j->state = MS_JOB_RUNNING;
    j->background = background;

    /* keep the text without the trailing '&' (bash shows it separately) */
    n = strlen(text);
    while (n > 0 && (text[n-1] == ' ' || text[n-1] == '\t' || text[n-1] == '&'))
        n--;
    if (n >= sizeof j->text)
        n = sizeof j->text - 1;
    memcpy(j->text, text, n);
    j->text[n] = '\0';
    return j;
}

void ms_job_free(struct ms_job *j)
{
    j->id = 0;
    j->state = MS_JOB_FREE;
    j->nprocs = 0;
}

void ms_job_add_proc(struct ms_job *j, pid_t pid)
{
    if (j->nprocs < MS_MAX_PIPE) {
        struct ms_proc *p = &j->procs[j->nprocs++];
        p->pid = pid;
        p->status = 0;
        p->done = 0;
        p->stopped = 0;
    }
}

int ms_job_signal(struct ms_job *j, int sig)
{
    int i, rc = 0;

    if (ms_interactive && j->pgid > 0)
        return kill(-j->pgid, sig);

    for (i = 0; i < j->nprocs; i++)
        if (!j->procs[i].done && kill(j->procs[i].pid, sig) < 0 &&
            errno != ESRCH)
            rc = -1;
    return rc;
}

int ms_jobs_any_stopped(void)
{
    sigset_t saved;
    int i, any = 0;

    ms_block_sigchld(&saved);
    for (i = 0; i < MS_MAX_JOBS; i++)
        if (ms_jobs[i].id && ms_jobs[i].state == MS_JOB_STOPPED)
            any = 1;
    ms_unblock_sigchld(&saved);
    return any;
}

/* ---------------------------------------------------------------- */
/* Status bookkeeping -- called from the SIGCHLD handler              */
/* (and therefore async-signal-safe: loops and assignments only)      */
/* ---------------------------------------------------------------- */
void ms_job_mark(pid_t pid, int status)
{
    int i, k;

    for (i = 0; i < MS_MAX_JOBS; i++) {
        struct ms_job *j = &ms_jobs[i];
        if (!j->id)
            continue;
        for (k = 0; k < j->nprocs; k++) {
            struct ms_proc *p = &j->procs[k];
            int alive = 0, stopped = 1;
            if (p->pid != pid)
                continue;

            if (WIFSTOPPED(status)) {
                p->stopped = 1;
                p->status = status;
            } else if (WIFCONTINUED(status)) {
                p->stopped = 0;
            } else {
                p->done = 1;
                p->stopped = 0;
                p->status = status;
            }

            /* Derive job state from its processes. */
            for (k = 0; k < j->nprocs; k++) {
                if (!j->procs[k].done) {
                    alive = 1;
                    if (!j->procs[k].stopped)
                        stopped = 0;
                }
            }
            if (!alive)
                j->state = MS_JOB_DONE;
            else if (stopped)
                j->state = MS_JOB_STOPPED;
            else
                j->state = MS_JOB_RUNNING;
            return;
        }
    }
    /* Unknown pid: a child that was never registered.  Cannot happen with
     * SIGCHLD blocked across fork+register, so nothing to do. */
}

/* Exit status of the job = status of its last process (bash default). */
static int job_exit_status(const struct ms_job *j)
{
    const struct ms_proc *p;

    if (j->nprocs == 0)
        return 1;
    p = &j->procs[j->nprocs - 1];
    if (p->stopped && !p->done)
        return 128 + WSTOPSIG(p->status);
    if (WIFEXITED(p->status))
        return WEXITSTATUS(p->status);
    if (WIFSIGNALED(p->status))
        return 128 + WTERMSIG(p->status);
    return 1;
}

/* ---------------------------------------------------------------- */
/* Reporting                                                          */
/* ---------------------------------------------------------------- */
static char job_mark(const struct ms_job *j)
{
    /* '+' = highest job id (current), '-' = second highest, ' ' else.
     * bash tracks "most recently stopped/started"; this is the common
     * simplification and is enough for fg/bg defaults. */
    int i, first = 0, second = 0;
    for (i = 0; i < MS_MAX_JOBS; i++) {
        int id = ms_jobs[i].id;
        if (id > first) { second = first; first = id; }
        else if (id > second) second = id;
    }
    return j->id == first ? '+' : j->id == second ? '-' : ' ';
}

static const char *state_string(const struct ms_job *j, char *buf, size_t n)
{
    const struct ms_proc *p = &j->procs[j->nprocs - 1];

    switch (j->state) {
    case MS_JOB_RUNNING: return "Running";
    case MS_JOB_STOPPED: return "Stopped";
    case MS_JOB_DONE:
        if (WIFSIGNALED(p->status)) {
            const char *s = strsignal(WTERMSIG(p->status));
            snprintf(buf, n, "%s", s ? s : "Signaled");
            return buf;
        }
        if (WIFEXITED(p->status) && WEXITSTATUS(p->status) != 0) {
            snprintf(buf, n, "Exit %d", WEXITSTATUS(p->status));
            return buf;
        }
        return "Done";
    default:
        return "?";
    }
}

static void print_job(const struct ms_job *j, int newline_first)
{
    char line[MS_JOB_TEXT + 64], st[64];
    int n;

    n = snprintf(line, sizeof line, "%s[%d]%c  %-23s %s%s\n",
                 newline_first ? "\n" : "",
                 j->id, job_mark(j), state_string(j, st, sizeof st),
                 j->text,
                 j->state == MS_JOB_RUNNING && j->background ? " &" : "");
    if (n > 0)
        out(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

void ms_job_announce(struct ms_job *j)
{
    char buf[64];
    int n;

    if (!ms_interactive)
        return;
    n = snprintf(buf, sizeof buf, "[%d] %ld\n", j->id,
                 (long)(j->nprocs ? j->procs[j->nprocs-1].pid : j->pgid));
    if (n > 0)
        out(buf, (size_t)n);
}

void ms_jobs_notify(void)
{
    sigset_t saved;
    int i;

    ms_block_sigchld(&saved);
    for (i = 0; i < MS_MAX_JOBS; i++) {
        struct ms_job *j = &ms_jobs[i];
        if (!j->id || j->notified)
            continue;
        if (j->state == MS_JOB_DONE) {
            if (ms_interactive && j->background)
                print_job(j, 0);
            ms_job_free(j);
        } else if (j->state == MS_JOB_STOPPED) {
            if (ms_interactive)
                print_job(j, 0);
            j->notified = 1;
        }
    }
    ms_unblock_sigchld(&saved);
}

/* ---------------------------------------------------------------- */
/* Foreground / background                                            */
/* ---------------------------------------------------------------- */
static void mark_running(struct ms_job *j)
{
    int i;
    for (i = 0; i < j->nprocs; i++)
        j->procs[i].stopped = 0;
    j->state = MS_JOB_RUNNING;
    j->notified = 0;
}

/* Called with SIGCHLD blocked. */
static void wait_for_job(struct ms_job *j)
{
    sigset_t waitmask;

    sigprocmask(SIG_BLOCK, NULL, &waitmask);   /* current mask (SIGCHLD in) */
    sigdelset(&waitmask, SIGCHLD);
    /* sigsuspend() installs waitmask (SIGCHLD unblocked), sleeps until a
     * signal is handled, then restores the mask (SIGCHLD blocked again)
     * before returning.  Re-check the state each time: one SIGCHLD may
     * report several children, and unrelated signals may also wake us. */
    while (j->state == MS_JOB_RUNNING)
        sigsuspend(&waitmask);
}

int ms_job_foreground(struct ms_job *j, int cont)
{
    int status;

    if (ms_interactive) {
        /* 1. hand the terminal to the job's process group */
        if (tcsetpgrp(ms_terminal, j->pgid) < 0)
            ms_perror("tcsetpgrp");
        /* 2. when resuming, restore the terminal modes the job had */
        if (cont && tcsetattr(ms_terminal, TCSADRAIN, &j->tmodes) < 0)
            ms_perror("tcsetattr");
    }
    if (cont) {
        mark_running(j);
        if (ms_job_signal(j, SIGCONT) < 0)
            ms_perror("kill (SIGCONT)");
    }
    j->background = 0;

    /* 3. wait until every process has exited or stopped */
    wait_for_job(j);

    if (ms_interactive) {
        /* 4. take the terminal back and restore the shell's modes */
        if (tcsetpgrp(ms_terminal, ms_shell_pgid) < 0)
            ms_perror("tcsetpgrp");
        if (j->state == MS_JOB_STOPPED)
            tcgetattr(ms_terminal, &j->tmodes);   /* remember job's modes */
        if (tcsetattr(ms_terminal, TCSADRAIN, &ms_shell_tmodes) < 0)
            ms_perror("tcsetattr");
    }

    status = job_exit_status(j);

    if (j->state == MS_JOB_STOPPED) {
        /* Ctrl+Z: it is now a background job; report immediately. */
        j->background = 1;
        j->notified = 1;
        if (ms_interactive)
            print_job(j, 1);
    } else {
        if (ms_interactive) {
            const struct ms_proc *p = &j->procs[j->nprocs - 1];
            if (WIFSIGNALED(p->status)) {
                int sig = WTERMSIG(p->status);
                if (sig == SIGINT) {
                    out("\n", 1);
                } else if (sig != SIGPIPE) {
                    const char *s = strsignal(sig);
                    char buf[80];
                    int n = snprintf(buf, sizeof buf, "%s\n", s ? s : "Killed");
                    if (n > 0) out(buf, (size_t)n);
                }
            }
        }
        ms_job_free(j);
    }
    return status;
}

void ms_job_background(struct ms_job *j, int cont)
{
    j->background = 1;
    if (cont) {
        mark_running(j);
        if (ms_job_signal(j, SIGCONT) < 0)
            ms_perror("kill (SIGCONT)");
    }
}

/* ---------------------------------------------------------------- */
/* Built-ins: jobs, fg, bg                                            */
/* ---------------------------------------------------------------- */
int ms_builtin_jobs(char **argv)
{
    sigset_t saved;
    int i;

    (void)argv;
    ms_block_sigchld(&saved);
    for (i = 0; i < MS_MAX_JOBS; i++) {
        struct ms_job *j = &ms_jobs[i];
        if (!j->id)
            continue;
        print_job(j, 0);
        if (j->state == MS_JOB_DONE)
            ms_job_free(j);          /* reported once, like bash */
        else if (j->state == MS_JOB_STOPPED)
            j->notified = 1;
    }
    ms_unblock_sigchld(&saved);
    return 0;
}

/* Resolve "%n", "n", "%+", "%%", "%-" or no argument to a job.
 * Must be called with SIGCHLD blocked.  Returns NULL after a message. */
static struct ms_job *find_job(const char *who, const char *spec)
{
    int i, id = 0, want_current = 0, want_prev = 0;

    if (!spec || strcmp(spec, "%+") == 0 || strcmp(spec, "%%") == 0 ||
        strcmp(spec, "%") == 0) {
        want_current = 1;
    } else if (strcmp(spec, "%-") == 0) {
        want_prev = 1;
    } else {
        const char *s = spec[0] == '%' ? spec + 1 : spec;
        char *end;
        id = (int)strtol(s, &end, 10);
        if (*s == '\0' || *end != '\0' || id <= 0) {
            ms_err("%s: %s: no such job", who, spec);
            return NULL;
        }
    }

    if (want_current || want_prev) {
        char target = want_current ? '+' : '-';
        for (i = 0; i < MS_MAX_JOBS; i++)
            if (ms_jobs[i].id && ms_jobs[i].state != MS_JOB_DONE &&
                job_mark(&ms_jobs[i]) == target)
                return &ms_jobs[i];
        ms_err("%s: %s: no such job", who, spec ? spec : "current");
        return NULL;
    }
    for (i = 0; i < MS_MAX_JOBS; i++)
        if (ms_jobs[i].id == id) {
            if (ms_jobs[i].state == MS_JOB_DONE) {
                ms_err("%s: job has terminated", who);
                return NULL;
            }
            return &ms_jobs[i];
        }
    ms_err("%s: %s: no such job", who, spec);
    return NULL;
}

int ms_builtin_fg(char **argv)
{
    sigset_t saved;
    struct ms_job *j;
    int status, cont;

    if (argv[1] && argv[2]) {
        ms_err("fg: too many arguments");
        return 1;
    }
    ms_block_sigchld(&saved);
    j = find_job("fg", argv[1]);
    if (!j) {
        ms_unblock_sigchld(&saved);
        return 1;
    }
    {
        char buf[MS_JOB_TEXT + 2];
        int n = snprintf(buf, sizeof buf, "%s\n", j->text);
        if (n > 0) out(buf, (size_t)n);
    }
    cont = (j->state == MS_JOB_STOPPED);
    status = ms_job_foreground(j, cont);
    ms_unblock_sigchld(&saved);
    ms_last_status = status;
    return status;
}

int ms_builtin_bg(char **argv)
{
    sigset_t saved;
    struct ms_job *j;

    if (argv[1] && argv[2]) {
        ms_err("bg: too many arguments");
        return 1;
    }
    ms_block_sigchld(&saved);
    j = find_job("bg", argv[1]);
    if (!j) {
        ms_unblock_sigchld(&saved);
        return 1;
    }
    if (j->state == MS_JOB_RUNNING) {
        ms_err("bg: job %d already in background", j->id);
        ms_unblock_sigchld(&saved);
        return 0;
    }
    ms_job_background(j, 1);
    {
        char buf[MS_JOB_TEXT + 16];
        int n = snprintf(buf, sizeof buf, "[%d]%c %s &\n", j->id, job_mark(j), j->text);
        if (n > 0) out(buf, (size_t)n);
    }
    ms_unblock_sigchld(&saved);
    return 0;
}
