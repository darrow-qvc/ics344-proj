/*
 * signals.c - signal setup, SIGCHLD handler, process-group plumbing.
 *                                            [jobs] Husain Althagafi
 *
 * Terminal / process-group protocol (docs/02-job-control.md):
 *
 *  shell start (interactive only)
 *    1. loop: while tcgetpgrp(tty) != our pgrp -> kill(-pgrp, SIGTTIN)
 *       (we were started in the background: stop until we are foreground)
 *    2. ignore SIGINT SIGQUIT SIGTSTP SIGTTIN SIGTTOU  (the shell itself
 *       must never be interrupted or stopped by the user's keys)
 *    3. install SIGCHLD handler (SA_RESTART, *without* SA_NOCLDSTOP so we
 *       are told about stops and continues)
 *    4. setpgid(0,0): become our own process group leader
 *    5. tcsetpgrp(tty, our pgid); tcgetattr -> ms_shell_tmodes
 *
 *  launching a job (ms_run_pipeline)
 *    parent: block SIGCHLD; register job; for each stage: fork
 *      child : setpgid(0, pgid)        -- pgid = first child's pid
 *              if foreground: tcsetpgrp(tty, pgid)   (SIGTTOU still ignored)
 *              reset the 5 signals + SIGCHLD to SIG_DFL
 *              unblock everything; wire pipes; redirections; execvp
 *      parent: setpgid(pid, pgid)       -- done on BOTH sides so that
 *              whichever runs first wins; EACCES/ESRCH are expected
 *    parent: foreground -> tcsetpgrp(tty, pgid); sigsuspend-loop;
 *            tcsetpgrp(tty, shell); restore termios.   Unblock SIGCHLD.
 *
 *  Ctrl+C / Ctrl+Z are turned into SIGINT / SIGTSTP by the tty driver
 *  and delivered to the *foreground process group* only.  Because the
 *  job owns the terminal while it runs, the shell and background jobs
 *  never see those signals.  Background jobs that try to read the
 *  terminal get SIGTTIN and stop, which the handler records as Stopped.
 */
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "minish.h"

static void sigchld_handler(int sig)
{
    int saved_errno = errno;
    int status;
    pid_t pid;

    (void)sig;
    /* Reap everything that has changed state.  WNOHANG: never block in a
     * handler.  WUNTRACED/WCONTINUED: report stops and continues so the
     * job table can track Ctrl+Z, bg and fg.  One signal may stand for
     * several children, hence the loop. */
    while ((pid = waitpid(-1, &status,
                          WNOHANG | WUNTRACED | WCONTINUED)) > 0)
        ms_job_mark(pid, status);
    errno = saved_errno;
}

void ms_block_sigchld(sigset_t *saved)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigprocmask(SIG_BLOCK, &set, saved);
}

void ms_unblock_sigchld(const sigset_t *saved)
{
    sigprocmask(SIG_SETMASK, saved, NULL);
}

static void set_disposition(int sig, void (*fn)(int))
{
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = fn;
    sigaction(sig, &sa, NULL);
}

static const int job_signals[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU };

void ms_jobs_init(void)
{
    struct sigaction sa;

    ms_terminal = STDIN_FILENO;

    if (ms_interactive) {
        /* 1. wait until we are in the foreground */
        for (;;) {
            pid_t fg = tcgetpgrp(ms_terminal);
            ms_shell_pgid = getpgrp();
            if (fg < 0) {              /* not a controlling tty after all */
                ms_interactive = 0;
                break;
            }
            if (fg == ms_shell_pgid)
                break;
            kill(-ms_shell_pgid, SIGTTIN);
        }
    }

    if (ms_interactive) {
        size_t i;
        /* 2. ignore interactive and job-control signals */
        for (i = 0; i < sizeof job_signals / sizeof job_signals[0]; i++)
            set_disposition(job_signals[i], SIG_IGN);

        /* 4. own process group */
        ms_shell_pgid = getpid();
        if (setpgid(ms_shell_pgid, ms_shell_pgid) < 0 && errno != EPERM) {
            /* EPERM: already a session leader (e.g. started by a getty);
             * that is fine, we are a group leader already. */
            ms_perror("setpgid");
        }
        ms_shell_pgid = getpgrp();

        /* 5. grab the terminal and remember its modes */
        if (tcsetpgrp(ms_terminal, ms_shell_pgid) < 0)
            ms_perror("tcsetpgrp");
        if (tcgetattr(ms_terminal, &ms_shell_tmodes) < 0)
            ms_perror("tcgetattr");
    } else {
        ms_shell_pgid = getpgrp();
    }

    /* 3. SIGCHLD handler: the same in both modes */
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;          /* NOT SA_NOCLDSTOP: we need stops */
    sa.sa_handler = sigchld_handler;
    if (sigaction(SIGCHLD, &sa, NULL) < 0)
        ms_perror("sigaction");
}

void ms_child_setup(pid_t pgid, int foreground)
{
    sigset_t none;
    size_t i;

    if (ms_interactive) {
        pid_t pid = getpid();
        if (pgid == 0)
            pgid = pid;                /* first stage leads the group */
        if (setpgid(pid, pgid) < 0 && errno != EPERM && errno != EACCES)
            ms_perror("setpgid (child)");
        /* The group may be empty if the leader already exited (EPERM);
         * we then stay in the shell's group, which is also what bash does. */

        if (foreground)
            (void)tcsetpgrp(ms_terminal, pgid);   /* parent does it too */

        for (i = 0; i < sizeof job_signals / sizeof job_signals[0]; i++)
            set_disposition(job_signals[i], SIG_DFL);
    }
    set_disposition(SIGCHLD, SIG_DFL);

    /* We inherited a mask with SIGCHLD blocked: clear it before exec. */
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
}

void ms_parent_setup(struct ms_job *j, pid_t pid)
{
    if (j->pgid == 0)
        j->pgid = pid;
    if (ms_interactive) {
        /* Mirror the child's setpgid so the group exists before the parent
         * calls tcsetpgrp.  EACCES = child already exec'd (it did it
         * itself); ESRCH = child already exited (nothing to do). */
        if (setpgid(pid, j->pgid) < 0 && errno != EACCES && errno != ESRCH &&
            errno != EPERM)
            ms_perror("setpgid (parent)");
    }
    ms_job_add_proc(j, pid);
}
