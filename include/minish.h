/*
 * minish.h - shared data structures and module interfaces for MiniSh.
 *
 * Every module includes only this header.  The ownership of each
 * declaration block is marked so members can work independently:
 *
 *   [parser] Karrar Alqallaf      src/parser/
 *   [exec]   Abdul Wadood Syed    src/exec/
 *   [io]     Talal Alshammari     src/io/
 *   [jobs]   Husain Althagafi     src/jobs/
 *
 * Design rule for the embedded target (Raspberry Pi): no heap allocation
 * on the per-command path.  All buffers below are fixed-size and either
 * static or stack-allocated, so memory use is bounded and predictable and
 * there is nothing to leak.
 */
#ifndef MINISH_H
#define MINISH_H

#include <sys/types.h>
#include <termios.h>
#include <signal.h>

/* ------------------------------------------------------------------ */
/* Compile-time limits (see docs/01-grammar.md, "Maximum sizes")       */
/* ------------------------------------------------------------------ */
#define MS_MAX_LINE    4096   /* bytes per input line, including '\n'   */
#define MS_MAX_TOKENS   256   /* tokens per line                        */
#define MS_MAX_ARGS     128   /* argv entries per command (excl. NULL)  */
#define MS_MAX_PIPE      16   /* commands per pipeline                  */
#define MS_MAX_REDIRS     8   /* redirections per command               */
#define MS_MAX_JOBS      32   /* entries in the job table               */
#define MS_JOB_TEXT     256   /* bytes of command text kept per job     */
#define MS_PROMPT      "minish> "

/* Exit statuses used by the shell itself (match bash/POSIX). */
#define MS_EXIT_SYNTAX    2
#define MS_EXIT_CANTEXEC  126
#define MS_EXIT_NOTFOUND  127

/* ------------------------------------------------------------------ */
/* [parser] Parsed representation of one input line                   */
/* ------------------------------------------------------------------ */
enum ms_redir_kind { MS_REDIR_IN, MS_REDIR_OUT, MS_REDIR_APPEND };

struct ms_redir {
    enum ms_redir_kind kind;       /* <  >  >>                            */
    char *path;
};

struct ms_command {
    char *argv[MS_MAX_ARGS + 1];   /* NULL-terminated                     */
    int   argc;
    struct ms_redir redirs[MS_MAX_REDIRS];  /* applied in source order    */
    int   nredirs;
};

struct ms_pipeline {
    struct ms_command cmds[MS_MAX_PIPE];
    int   ncmds;
    int   background;              /* trailing '&'                        */
    char  text[MS_MAX_LINE];       /* original line, for `jobs` display   */
    char  words[MS_MAX_LINE];      /* backing store for argv/file strings */
};

/* Parse result codes */
enum ms_parse_status {
    MS_PARSE_OK = 0,
    MS_PARSE_EMPTY,     /* blank line / only whitespace: nothing to run */
    MS_PARSE_ERROR      /* syntax error already reported on stderr      */
};

/* Tokenize + parse `line` into `pl`.  Never allocates. */
enum ms_parse_status ms_parse_line(const char *line, struct ms_pipeline *pl);

/* ------------------------------------------------------------------ */
/* [jobs] Job table and job control                                   */
/* ------------------------------------------------------------------ */
enum ms_job_state {
    MS_JOB_FREE = 0,
    MS_JOB_RUNNING,
    MS_JOB_STOPPED,
    MS_JOB_DONE
};

struct ms_proc {
    pid_t pid;
    int   status;      /* raw wait status, valid when done/stopped */
    int   done;        /* 1 when waitpid reported exit/termination  */
    int   stopped;     /* 1 when currently stopped                  */
};

struct ms_job {
    int               id;                 /* 1-based, 0 => slot free     */
    pid_t             pgid;
    enum ms_job_state state;
    int               notified;           /* done msg already printed    */
    int               background;         /* launched with '&'           */
    int               nprocs;
    struct ms_proc    procs[MS_MAX_PIPE];
    struct termios    tmodes;             /* terminal modes when stopped */
    char              text[MS_JOB_TEXT];
};

/* Shell-wide state owned by the jobs module. */
extern int            ms_interactive;     /* stdin is a terminal         */
extern int            ms_terminal;        /* fd of controlling terminal  */
extern pid_t          ms_shell_pgid;
extern struct termios ms_shell_tmodes;
extern int            ms_last_status;     /* exit status of last fg job  */

/* One-time initialisation: process group, terminal, signal dispositions. */
void ms_jobs_init(void);

/* Block / unblock SIGCHLD around fork + job registration (saves old mask). */
void ms_block_sigchld(sigset_t *saved);
void ms_unblock_sigchld(const sigset_t *saved);

/* Record a waitpid() result for pid.  Async-signal-safe; called from the
 * SIGCHLD handler. */
void ms_job_mark(pid_t pid, int status);

/* Allocate a job slot.  Must be called with SIGCHLD blocked. Returns NULL
 * if the table is full. */
struct ms_job *ms_job_add(const char *text, int background);
void           ms_job_free(struct ms_job *j);
void           ms_job_add_proc(struct ms_job *j, pid_t pid);

/* Called in the child right after fork: join process group, take the
 * terminal if foreground, restore default signal dispositions. */
void ms_child_setup(pid_t pgid, int foreground);

/* Called in the parent right after fork for every child. */
void ms_parent_setup(struct ms_job *j, pid_t pid);

/* Hand the terminal to job j, wait until it stops or finishes, then take
 * the terminal back.  Returns job exit status (status of last command). */
int  ms_job_foreground(struct ms_job *j, int cont);
void ms_job_background(struct ms_job *j, int cont);

/* Send sig to every live process of the job (pgid when interactive). */
int  ms_job_signal(struct ms_job *j, int sig);

/* 1 if any job is currently stopped (used before exiting the shell). */
int  ms_jobs_any_stopped(void);

/* Print "[n] pid" for a new background job (interactive only). */
void ms_job_announce(struct ms_job *j);

/* Report finished/stopped background jobs; called before each prompt. */
void ms_jobs_notify(void);

/* Built-ins implemented by the jobs module. */
int  ms_builtin_jobs(char **argv);
int  ms_builtin_fg(char **argv);
int  ms_builtin_bg(char **argv);

/* ------------------------------------------------------------------ */
/* [io] Redirections and pipelines                                    */
/* ------------------------------------------------------------------ */

/* Apply the redirections of cmd in order.  Returns 0 or -1 (error printed). */
int ms_apply_redirections(const struct ms_command *cmd);

/* 1 if any redirection of cmd targets descriptor fd (0 or 1). */
int ms_redirects_fd(const struct ms_command *cmd, int fd);

/* Run pipeline `pl` (ncmds >= 1).  Creates pipes, forks each stage,
 * registers the job and waits (foreground) or announces (background).
 * Returns exit status of the pipeline. */
int ms_run_pipeline(struct ms_pipeline *pl);

/* Child-side plumbing: wire stdin/stdout to the given pipe ends and close
 * every pipe descriptor.  in_fd/out_fd may be -1. */
void ms_child_wire_pipes(int in_fd, int out_fd, int (*pipes)[2], int npipes);

/* ------------------------------------------------------------------ */
/* [exec] Command execution and simple built-ins                      */
/* ------------------------------------------------------------------ */

/* Returns 1 if argv[0] is a built-in that must run in the shell process */
int ms_is_builtin(const char *name);

/* Run a built-in in the current process, return its exit status. */
int ms_run_builtin(char **argv);

/* execvp with bash-compatible diagnostics.  Never returns; exits 126/127. */
void ms_exec_command(struct ms_command *cmd) __attribute__((noreturn));

/* Top-level entry: run one parsed line (built-in or pipeline). */
int ms_execute(struct ms_pipeline *pl);

/* fork() with EAGAIN retry; returns -1 after diagnostics. */
pid_t ms_fork(void);

/* Set by `exit` built-in: shell loop should terminate. */
extern int ms_exit_requested;
extern int ms_exit_code;

/* ------------------------------------------------------------------ */
/* Diagnostics helper (async-signal-safe variant for handlers)         */
/* ------------------------------------------------------------------ */
void ms_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ms_perror(const char *what);              /* "minish: what: strerror" */

#endif /* MINISH_H */
