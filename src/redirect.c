/*
 * redirect.c - <  >  >> handling.            [io] Talal Alshammari
 *
 * Called in the child (or around a built-in) after pipe wiring, so an
 * explicit redirection always overrides a pipe on the same descriptor,
 * exactly like bash:   "a | b < file"  -> b reads file, a gets SIGPIPE.
 *
 * Collision rules (docs/03-pipelines-redirection.md):
 *   - Redirections are applied strictly left to right.  "> a > b" opens
 *     (creates/truncates) a, then b; b ends up on fd 1.  Same as bash.
 *   - "< f > f": output is opened (truncated) after input is opened, so
 *     the command reads an empty file.  Same as bash.
 *   - ">> f" never truncates; O_APPEND makes every write go to the end.
 *
 * Every open/dup2 is checked; on failure a bash-style message is printed
 * and -1 is returned.  The caller (child) then _exit(1)s; bash also uses
 * status 1 for a failed redirection.
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "minish.h"

static int redirect_fd(const char *path, int flags, int target)
{
    int fd = open(path, flags, 0666);

    if (fd < 0) {
        ms_err("%s: %s", path, strerror(errno));
        return -1;
    }
    if (fd == target)
        return 0;                  /* already in place; nothing to close */
    if (dup2(fd, target) < 0) {
        ms_perror("dup2");
        close(fd);
        return -1;
    }
    close(fd);                     /* original descriptor no longer needed */
    return 0;
}

int ms_redirects_fd(const struct ms_command *cmd, int fd)
{
    int i;
    for (i = 0; i < cmd->nredirs; i++) {
        int target = cmd->redirs[i].kind == MS_REDIR_IN ? STDIN_FILENO
                                                         : STDOUT_FILENO;
        if (target == fd)
            return 1;
    }
    return 0;
}

int ms_apply_redirections(const struct ms_command *cmd)
{
    int i;

    for (i = 0; i < cmd->nredirs; i++) {
        const struct ms_redir *r = &cmd->redirs[i];
        int flags, target;

        switch (r->kind) {
        case MS_REDIR_IN:
            flags = O_RDONLY;                       target = STDIN_FILENO;  break;
        case MS_REDIR_OUT:
            flags = O_WRONLY | O_CREAT | O_TRUNC;   target = STDOUT_FILENO; break;
        default: /* MS_REDIR_APPEND */
            flags = O_WRONLY | O_CREAT | O_APPEND;  target = STDOUT_FILENO; break;
        }
        if (redirect_fd(r->path, flags, target) < 0)
            return -1;
    }
    return 0;
}
