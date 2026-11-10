/*
 * parser.c - recursive-descent parser.       [parser] Karrar Alqallaf
 *
 * Grammar (docs/01-grammar.md):
 *
 *   line      := [ pipeline ] [ '&' ] EOL
 *   pipeline  := command { '|' command }
 *   command   := item { item }            -- at least one WORD overall
 *   item      := WORD | redirect
 *   redirect  := '<' WORD | '>' WORD | '>>' WORD
 *
 * Precedence: a redirection binds to the command it appears in, which
 * binds tighter than '|'.  "a > f | b" redirects a's stdout to f and b
 * reads an empty pipe (same as bash).  Redirections are kept in source
 * order and applied in that order, so with "> a > b" both files are
 * created and the last one receives the output (bash behaviour).  '&'
 * applies to the whole pipeline and may only be the final token.
 *
 * Error reporting mimics bash:
 *   minish: syntax error near unexpected token `|'
 *   minish: syntax error near unexpected token `newline'
 */
#include <string.h>
#include "lexer.h"

struct parser {
    struct ms_lexer *lx;
    int              pos;
};

static const struct ms_token *peek(struct parser *ps)
{
    return &ps->lx->toks[ps->pos];
}

static const struct ms_token *next(struct parser *ps)
{
    return &ps->lx->toks[ps->pos++];
}

static int syntax_error(const struct ms_token *t)
{
    ms_err("syntax error near unexpected token `%s'", ms_tok_name(t));
    return -1;
}

static int parse_command(struct parser *ps, struct ms_command *cmd)
{
    int nwords = 0;

    memset(cmd, 0, sizeof *cmd);

    for (;;) {
        const struct ms_token *t = peek(ps);

        if (t->type == T_WORD) {
            next(ps);
            if (cmd->argc >= MS_MAX_ARGS) {
                ms_err("too many arguments (limit %d)", MS_MAX_ARGS);
                return -1;
            }
            cmd->argv[cmd->argc++] = t->text;
            nwords++;
        } else if (t->type == T_LT || t->type == T_GT || t->type == T_GTGT) {
            const struct ms_token *f;
            next(ps);
            f = next(ps);
            if (f->type != T_WORD)
                return syntax_error(f);
            if (cmd->nredirs >= MS_MAX_REDIRS) {
                ms_err("too many redirections (limit %d)", MS_MAX_REDIRS);
                return -1;
            }
            cmd->redirs[cmd->nredirs].kind =
                t->type == T_LT ? MS_REDIR_IN :
                t->type == T_GT ? MS_REDIR_OUT : MS_REDIR_APPEND;
            cmd->redirs[cmd->nredirs].path = f->text;
            cmd->nredirs++;
        } else {
            break;
        }
    }

    if (nwords == 0)
        /* e.g. "| cmd", "cmd | | cmd", "> f" alone, "cmd |" at EOL */
        return syntax_error(peek(ps));

    cmd->argv[cmd->argc] = NULL;
    return 0;
}

enum ms_parse_status ms_parse_line(const char *line, struct ms_pipeline *pl)
{
    static struct ms_lexer lx;   /* 4 KB; static keeps it off the stack */
    struct parser ps = { &lx, 0 };

    pl->ncmds = 0;
    pl->background = 0;

    /* keep the original text for `jobs`, minus trailing newline */
    {
        size_t n = strcspn(line, "\n");
        if (n >= sizeof pl->text)
            n = sizeof pl->text - 1;
        memcpy(pl->text, line, n);
        pl->text[n] = '\0';
    }

    if (ms_lex(line, &lx, pl->words) < 0)
        return MS_PARSE_ERROR;
    if (lx.ntoks == 0)
        return MS_PARSE_EMPTY;

    for (;;) {
        if (pl->ncmds >= MS_MAX_PIPE) {
            ms_err("pipeline too long (limit %d commands)", MS_MAX_PIPE);
            return MS_PARSE_ERROR;
        }
        if (parse_command(&ps, &pl->cmds[pl->ncmds]) < 0)
            return MS_PARSE_ERROR;
        pl->ncmds++;
        if (peek(&ps)->type == T_PIPE) {
            next(&ps);
            continue;
        }
        break;
    }

    if (peek(&ps)->type == T_AMP) {
        next(&ps);
        pl->background = 1;
    }
    if (peek(&ps)->type != T_EOL) {
        syntax_error(peek(&ps));
        return MS_PARSE_ERROR;
    }
    return MS_PARSE_OK;
}
