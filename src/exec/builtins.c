/*
 * builtins.c - cd, exit and built-in dispatch.  [exec] Abdul Wadood Syed
 *
 * jobs / fg / bg are implemented in src/jobs (job-control owner) and only
 * dispatched from here so that the executor has a single table of
 * built-ins.
 *
 * Exit statuses follow bash:
 *   cd: 0 ok, 1 on error (message "minish: cd: dir: strerror")
 *   exit [n]: terminates shell with n & 0xff; a non-numeric argument
 *             prints "numeric argument required" and exits with 2;
 *             too many arguments prints an error and does NOT exit (1).
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "minish.h"

static int builtin_cd(char **argv)
{
    const char *dir;
    static char cwd[PATH_MAX];

    if (argv[1] && argv[2]) {
        ms_err("cd: too many arguments");
        return 1;
    }
    if (!argv[1] || strcmp(argv[1], "~") == 0) {
        dir = getenv("HOME");
        if (!dir) {
            ms_err("cd: HOME not set");
            return 1;
        }
    } else if (strcmp(argv[1], "-") == 0) {
        dir = getenv("OLDPWD");
        if (!dir) {
            ms_err("cd: OLDPWD not set");
            return 1;
        }
        printf("%s\n", dir);
        fflush(stdout);
    } else {
        dir = argv[1];
    }

    if (getcwd(cwd, sizeof cwd))
        setenv("OLDPWD", cwd, 1);
    if (chdir(dir) < 0) {
        ms_err("cd: %s: %s", dir, strerror(errno));
        return 1;
    }
    if (getcwd(cwd, sizeof cwd))
        setenv("PWD", cwd, 1);
    return 0;
}

static int builtin_exit(char **argv)
{
    long n = ms_last_status;

    if (argv[1]) {
        char *end;
        errno = 0;
        n = strtol(argv[1], &end, 10);
        if (*argv[1] == '\0' || *end != '\0' || errno != 0) {
            ms_err("exit: %s: numeric argument required", argv[1]);
            n = 2;
            ms_exit_requested = 1;
            ms_exit_code = 2;
            return 2;
        }
        if (argv[2]) {
            ms_err("exit: too many arguments");
            return 1;
        }
    }
    ms_exit_requested = 1;
    ms_exit_code = (int)(n & 0xff);
    return ms_exit_code;
}

struct builtin {
    const char *name;
    int (*fn)(char **argv);
};

static const struct builtin builtins[] = {
    { "cd",   builtin_cd },
    { "exit", builtin_exit },
    { "jobs", ms_builtin_jobs },
    { "fg",   ms_builtin_fg },
    { "bg",   ms_builtin_bg },
};

static const struct builtin *lookup(const char *name)
{
    size_t i;
    if (!name)
        return NULL;
    for (i = 0; i < sizeof builtins / sizeof builtins[0]; i++)
        if (strcmp(builtins[i].name, name) == 0)
            return &builtins[i];
    return NULL;
}

int ms_is_builtin(const char *name)
{
    return lookup(name) != NULL;
}

int ms_run_builtin(char **argv)
{
    const struct builtin *b = lookup(argv[0]);
    int status;

    if (!b)
        return MS_EXIT_NOTFOUND;
    status = b->fn(argv);
    fflush(stdout);
    return status;
}
