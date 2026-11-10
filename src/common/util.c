/* util.c - shared diagnostics.  Not member-specific. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "minish.h"

/* Diagnostics go through a single buffer + write(2) so that a message is
 * emitted atomically (no interleaving with child output) and so stdio
 * buffering never holds an error message back. */
void ms_err(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;

    memcpy(buf, "minish: ", 8);
    va_start(ap, fmt);
    n = vsnprintf(buf + 8, sizeof buf - 9, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    if ((size_t)n > sizeof buf - 9)
        n = (int)sizeof buf - 9;
    buf[8 + n] = '\n';
    if (write(STDERR_FILENO, buf, (size_t)n + 9) < 0) {
        /* nothing sensible to do */
    }
}

void ms_perror(const char *what)
{
    int e = errno;
    ms_err("%s: %s", what, strerror(e));
    errno = e;
}
