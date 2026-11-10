/*
 * exec.c - process execution.                [exec] Abdul Wadood Syed
 *
 * Responsibilities:
 *   - ms_fork(): fork with bounded retry on EAGAIN (process limit) and a
 *     clear diagnostic when it ultimately fails.
 *   - ms_exec_command(): execvp with bash-compatible messages and exit
 *     codes:  127 "command not found" / "No such file or directory",
 *             126 "Permission denied" / "Is a directory" / other exec errors.
 *   - ms_execute(): top-level dispatch between built-ins executed in the
 *     shell process and pipelines executed by the io module.
 *
 * Failure recovery policy (docs/04-error-handling.md):
 *   Every system call that can fail is checked.  Failures in the child are
 *   reported and the child calls _exit() (never exit()) so that the
 *   parent's stdio buffers are not flushed twice and atexit handlers do not
 *   run in the child.  Failures in the parent abort the current command
 *   line only; the shell never exits because of a failed command.
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "minish.h"

int ms_exit_requested = 0;
int ms_exit_code      = 0;

pid_t ms_fork(void)
{
    int attempt;

    for (attempt = 0; attempt < 3; attempt++) {
        pid_t pid = fork();
        if (pid >= 0)
            return pid;
        if (errno != EAGAIN)
            break;
        /* Hit RLIMIT_NPROC or a transient shortage: back off 10/20/30 ms. */
        {
            struct timespec ts = { 0, (attempt + 1) * 10L * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    ms_perror("fork");
    return -1;
}

void ms_exec_command(struct ms_command *cmd)
{
    char *const *argv = cmd->argv;
    int e;

    if (ms_is_builtin(argv[0]))
        _exit(ms_run_builtin(cmd->argv));   /* built-in inside a pipeline */

    execvp(argv[0], argv);
    e = errno;

    if (e == ENOENT) {
        if (strchr(argv[0], '/'))
            ms_err("%s: No such file or directory", argv[0]);
        else
            ms_err("%s: command not found", argv[0]);
        _exit(MS_EXIT_NOTFOUND);
    }
    if (e == EACCES) {
        struct stat st;
        if (stat(argv[0], &st) == 0 && S_ISDIR(st.st_mode)) {
            ms_err("%s: Is a directory", argv[0]);
            _exit(MS_EXIT_CANTEXEC);
        }
    }
    ms_err("%s: %s", argv[0], strerror(e));
    _exit(MS_EXIT_CANTEXEC);
}

/* Run a built-in in the shell process with its redirections applied and
 * then undone, so e.g. "jobs > out.txt" works without forking. */
static int run_builtin_with_redirs(struct ms_command *cmd)
{
    int saved_in = -1, saved_out = -1, status;

    if (ms_redirects_fd(cmd, STDIN_FILENO)) {
        saved_in = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 10);
        if (saved_in < 0) { ms_perror("fcntl"); return 1; }
    }
    if (ms_redirects_fd(cmd, STDOUT_FILENO)) {
        saved_out = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 10);
        if (saved_out < 0) {
            ms_perror("fcntl");
            if (saved_in >= 0) close(saved_in);
            return 1;
        }
    }
    if (ms_apply_redirections(cmd) < 0) {
        status = 1;
    } else {
        status = ms_run_builtin(cmd->argv);
    }
    if (saved_in >= 0) {
        if (dup2(saved_in, STDIN_FILENO) < 0) ms_perror("dup2");
        close(saved_in);
    }
    if (saved_out >= 0) {
        if (dup2(saved_out, STDOUT_FILENO) < 0) ms_perror("dup2");
        close(saved_out);
    }
    return status;
}

int ms_execute(struct ms_pipeline *pl)
{
    int status;

    if (pl->ncmds == 1 && !pl->background && ms_is_builtin(pl->cmds[0].argv[0])) {
        status = run_builtin_with_redirs(&pl->cmds[0]);
        ms_last_status = status;
        return status;
    }
    status = ms_run_pipeline(pl);
    return status;
}
