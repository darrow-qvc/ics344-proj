/*
 * main.c - prompt loop and line reader.      [parser] Karrar Alqallaf
 *
 * Usage:  minish                 interactive (if stdin is a tty) or
 *                                reads commands from stdin
 *         minish script          read commands from file
 *         minish -c 'command'    run one command line and exit
 *
 * Input is read with read(2) into a fixed buffer; no stdio, no readline.
 * A line longer than MS_MAX_LINE-1 bytes is reported, the rest of that
 * line is discarded, and the shell continues with the next line.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "minish.h"

struct reader {
    int    fd;
    char   buf[MS_MAX_LINE];
    size_t len;      /* bytes buffered           */
    size_t pos;      /* consumed bytes           */
    int    eof;
};

/* Read one line into `line` (NUL-terminated, newline stripped).
 * Returns 1 line read, 0 EOF, -1 line too long (already reported). */
static int read_line(struct reader *r, char *line)
{
    size_t n = 0;
    int too_long = 0;

    for (;;) {
        if (r->pos >= r->len) {
            ssize_t k;
            if (r->eof)
                break;
            do {
                k = read(r->fd, r->buf, sizeof r->buf);
            } while (k < 0 && errno == EINTR);
            if (k < 0) {
                ms_perror("read");
                r->eof = 1;
                break;
            }
            if (k == 0) {
                r->eof = 1;
                break;
            }
            r->len = (size_t)k;
            r->pos = 0;
        }
        {
            char c = r->buf[r->pos++];
            if (c == '\n') {
                line[n] = '\0';
                return too_long ? -1 : 1;
            }
            if (n < MS_MAX_LINE - 1)
                line[n++] = c;
            else
                too_long = 1;
        }
    }
    if (n == 0 && !too_long)
        return 0;
    line[n] = '\0';
    return too_long ? -1 : 1;
}

static void prompt(void)
{
    if (write(STDOUT_FILENO, MS_PROMPT, sizeof MS_PROMPT - 1) < 0) {
        /* terminal gone; nothing to do */
    }
}

static int run_one(const char *line, struct ms_pipeline *pl)
{
    switch (ms_parse_line(line, pl)) {
    case MS_PARSE_OK:
        return ms_execute(pl);
    case MS_PARSE_ERROR:
        ms_last_status = MS_EXIT_SYNTAX;
        return MS_EXIT_SYNTAX;
    case MS_PARSE_EMPTY:
    default:
        return ms_last_status;
    }
}

int main(int argc, char **argv)
{
    static struct ms_pipeline pl;         /* ~20 KB, keep off the stack */
    static char line[MS_MAX_LINE];
    struct reader rd = { STDIN_FILENO, {0}, 0, 0, 0 };
    int warned_stopped = 0;

    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        ms_interactive = 0;
        ms_jobs_init();
        run_one(argv[2], &pl);
        return ms_exit_requested ? ms_exit_code : ms_last_status;
    }
    if (argc >= 2) {
        rd.fd = open(argv[1], O_RDONLY | O_CLOEXEC);
        if (rd.fd < 0) {
            ms_perror(argv[1]);
            return 127;
        }
        ms_interactive = 0;
    } else {
        ms_interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    }

    ms_jobs_init();

    for (;;) {
        int rc;

        if (ms_interactive) {
            ms_jobs_notify();
            prompt();
        }
        rc = read_line(&rd, line);
        if (rc == 0) {
            /* EOF */
            if (ms_interactive && !warned_stopped && ms_jobs_any_stopped()) {
                ms_err("There are stopped jobs.");
                warned_stopped = 1;
                rd.eof = 0;             /* let the user try again */
                continue;
            }
            if (ms_interactive && write(STDOUT_FILENO, "exit\n", 5) < 0) {
                /* ignore */
            }
            break;
        }
        if (rc < 0) {
            ms_err("line too long (limit %d bytes), ignored", MS_MAX_LINE - 1);
            ms_last_status = MS_EXIT_SYNTAX;
            continue;
        }
        run_one(line, &pl);
        if (ms_exit_requested) {
            if (ms_interactive && !warned_stopped && ms_jobs_any_stopped()) {
                ms_err("There are stopped jobs.");
                warned_stopped = 1;
                ms_exit_requested = 0;
                continue;
            }
            break;
        }
    }
    if (!ms_interactive)
        ms_jobs_notify();   /* flush nothing, but keeps table consistent */
    return ms_exit_requested ? ms_exit_code : ms_last_status;
}

