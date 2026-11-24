/*
 * pipeline.c - multi-stage pipelines.        [io] Talal Alshammari
 *
 * Descriptor discipline (docs/03-pipelines-redirection.md):
 *
 *   Pipes are created one stage at a time, so at most one whole pipe plus
 *   the read end of the previous pipe are open in the parent at any
 *   moment.  After forking stage i the parent closes the write end of
 *   pipe i (otherwise stage i+1 would never see EOF) and the read end of
 *   pipe i-1.  After the last fork the parent holds no pipe descriptors.
 *
 *   Each child dup2()s its two ends onto 0/1 and then closes *every* pipe
 *   descriptor it inherited (ms_child_wire_pipes).  Leaving a stray write
 *   end open in any process is the classic bug that makes "cat f | sort"
 *   hang; leaving a read end open leaks a descriptor into the program.
 *   tests/run_tests.sh "fd-leak" cases verify this by running
 *   `ls /proc/self/fd` in the middle of a 3-stage pipeline.
 *
 * Exit status: the status of the last command in the pipeline (bash
 * default, no `pipefail`).  The job is complete only when all stages have
 * been reaped, so there are never zombies left from earlier stages.
 *
 * Failure recovery: if pipe() or fork() fails after some stages were
 * started, the already-started stages are sent SIGTERM, reaped, and the
 * pipeline returns status 1.  No descriptor is leaked on any path.
 */
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include "minish.h"

void ms_child_wire_pipes(int in_fd, int out_fd, int (*pipes)[2], int npipes)
{
    int i;

    if (in_fd >= 0 && in_fd != STDIN_FILENO) {
        if (dup2(in_fd, STDIN_FILENO) < 0) {
            ms_perror("dup2");
            _exit(1);
        }
    }
    if (out_fd >= 0 && out_fd != STDOUT_FILENO) {
        if (dup2(out_fd, STDOUT_FILENO) < 0) {
            ms_perror("dup2");
            _exit(1);
        }
    }
    /* The originals are no longer needed once dup2'd onto 0/1.  Closing
     * in_fd matters most: it is the read end of the previous pipe, which
     * the parent has already dropped from `pipes`. */
    if (in_fd  > STDERR_FILENO) close(in_fd);
    if (out_fd > STDERR_FILENO && out_fd != in_fd) close(out_fd);
    /* And every other pipe end still recorded in the table. */
    for (i = 0; i < npipes; i++) {
        if (pipes[i][0] > STDERR_FILENO && pipes[i][0] != in_fd && pipes[i][0] != out_fd)
            close(pipes[i][0]);
        if (pipes[i][1] > STDERR_FILENO && pipes[i][1] != in_fd && pipes[i][1] != out_fd)
            close(pipes[i][1]);
    }
}

static void close_pair(int p[2])
{
    if (p[0] >= 0) { close(p[0]); p[0] = -1; }
    if (p[1] >= 0) { close(p[1]); p[1] = -1; }
}

int ms_run_pipeline(struct ms_pipeline *pl)
{
    int pipes[MS_MAX_PIPE > 1 ? MS_MAX_PIPE - 1 : 1][2];
    int n = pl->ncmds;
    int prev_in = -1;          /* read end of the previous stage's pipe */
    int i, status = 0, failed = 0;
    sigset_t saved;
    struct ms_job *job;

    /* SIGCHLD stays blocked from before the first fork until the job and
     * all its pids are in the table (and, for foreground jobs, until the
     * wait loop hands control to sigsuspend). See docs/02-job-control.md. */
    ms_block_sigchld(&saved);

    job = ms_job_add(pl->text, pl->background);
    if (!job) {
        ms_err("too many jobs (limit %d)", MS_MAX_JOBS);
        ms_unblock_sigchld(&saved);
        ms_last_status = 1;
        return 1;
    }

    for (i = 0; i < n; i++) {
        int out_fd = -1;
        pid_t pid;

        pipes[i][0] = pipes[i][1] = -1;
        if (i < n - 1) {
            if (pipe(pipes[i]) < 0) {
                ms_perror("pipe");
                failed = 1;
                break;
            }
            out_fd = pipes[i][1];
        }

        pid = ms_fork();
        if (pid < 0) {
            close_pair(pipes[i]);
            failed = 1;
            break;
        }

        if (pid == 0) {
            /* ---- child ---- */
            ms_child_setup(job->pgid, !pl->background);
            ms_child_wire_pipes(prev_in, out_fd, pipes, i + 1);
            if (ms_apply_redirections(&pl->cmds[i]) < 0)
                _exit(1);
            ms_exec_command(&pl->cmds[i]);   /* does not return */
        }

        /* ---- parent ---- */
        ms_parent_setup(job, pid);

        if (prev_in >= 0)
            close(prev_in);                 /* stage i-1 output: done with it */
        if (out_fd >= 0)
            close(out_fd);                  /* write end belongs to child only */
        prev_in = pipes[i][0];              /* -1 for the last stage */
        pipes[i][0] = pipes[i][1] = -1;
    }

    if (prev_in >= 0)
        close(prev_in);

    if (failed) {
        /* Abort stages already started so nothing is left running or
         * waiting on a pipe that will never be written. */
        if (job->nprocs > 0) {
            ms_job_signal(job, SIGTERM);
            ms_job_foreground(job, 0);     /* reap them; also frees job */
        } else {
            ms_job_free(job);
        }
        ms_unblock_sigchld(&saved);
        ms_last_status = 1;
        return 1;
    }

    if (pl->background) {
        ms_job_background(job, 0);
        ms_job_announce(job);
        status = 0;
    } else {
        status = ms_job_foreground(job, 0);
    }

    ms_unblock_sigchld(&saved);
    ms_last_status = status;
    return status;
}
