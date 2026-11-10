/*
 * lexer.c - tokenizer.                       [parser] Karrar Alqallaf
 *
 * Tokenization rules (normative description in docs/01-grammar.md):
 *
 *   - Blanks (space, tab) separate words and are otherwise discarded.
 *   - Operators: |  <  >  >>  &.  They end any word in progress.
 *   - '...'  : every byte literal until the next '.
 *   - "..."  : literal except that \" and \\ produce " and \.
 *              Any other backslash is kept verbatim (as in bash).
 *   - \c      : outside quotes, backslash quotes the next byte c.
 *   - #       : at the start of a word begins a comment to end of line.
 *   - Quotes never nest; adjacent quoted/unquoted pieces concatenate
 *     into one word ( a"b c"d  ->  ab cd ).
 *   - An empty quoted string "" or '' is a (zero-length) word.
 *
 * Errors (all return -1 after a message, line is discarded):
 *   - unterminated ' or "            (bash: unexpected EOF ...)
 *   - more than MS_MAX_TOKENS tokens
 *   - unsupported operators ; && || <<  >& (reported as syntax error)
 *
 * No heap allocation: word bytes are copied into the caller's `words`
 * buffer, which is at least as long as the line, so it cannot overflow:
 * quote removal and escape processing only ever shorten the text.
 */
#include <string.h>
#include "lexer.h"

static int push(struct ms_lexer *lx, enum ms_tok_type t, char *text)
{
    if (lx->ntoks >= MS_MAX_TOKENS) {
        ms_err("too many tokens (limit %d)", MS_MAX_TOKENS);
        return -1;
    }
    lx->toks[lx->ntoks].type = t;
    lx->toks[lx->ntoks].text = text;
    lx->ntoks++;
    return 0;
}

const char *ms_tok_name(const struct ms_token *t)
{
    switch (t->type) {
    case T_EOL:  return "newline";
    case T_PIPE: return "|";
    case T_LT:   return "<";
    case T_GT:   return ">";
    case T_GTGT: return ">>";
    case T_AMP:  return "&";
    case T_WORD: return t->text;
    }
    return "?";
}

int ms_lex(const char *line, struct ms_lexer *lx, char *words)
{
    const char *p = line;
    char *w = words;          /* write cursor in words buffer           */
    char *wstart = NULL;      /* start of word in progress, or NULL     */

    lx->ntoks = 0;

#define END_WORD()                                              \
    do {                                                        \
        if (wstart) {                                           \
            *w++ = '\0';                                        \
            if (push(lx, T_WORD, wstart) < 0) return -1;        \
            wstart = NULL;                                      \
        }                                                       \
    } while (0)
#define BEGIN_WORD() do { if (!wstart) wstart = w; } while (0)

    for (;;) {
        char c = *p;

        if (c == '\0' || c == '\n') {
            END_WORD();
            break;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            END_WORD();
            p++;
            continue;
        }
        if (c == '#' && !wstart) {          /* comment to end of line */
            break;
        }
        if (c == '|' || c == '<' || c == '>' || c == '&' || c == ';') {
            END_WORD();
            if (c == '|' && p[1] == '|') {
                ms_err("syntax error near unexpected token `||' (not supported)");
                return -1;
            }
            if (c == '&' && p[1] == '&') {
                ms_err("syntax error near unexpected token `&&' (not supported)");
                return -1;
            }
            if (c == '<' && p[1] == '<') {
                ms_err("syntax error near unexpected token `<<' (not supported)");
                return -1;
            }
            if (c == '>' && p[1] == '&') {
                ms_err("syntax error near unexpected token `>&' (not supported)");
                return -1;
            }
            if (c == ';') {
                ms_err("syntax error near unexpected token `;' (not supported)");
                return -1;
            }
            if (c == '>' && p[1] == '>') {
                if (push(lx, T_GTGT, NULL) < 0) return -1;
                p += 2;
            } else {
                enum ms_tok_type t = c == '|' ? T_PIPE :
                                     c == '<' ? T_LT   :
                                     c == '>' ? T_GT   : T_AMP;
                if (push(lx, t, NULL) < 0) return -1;
                p++;
            }
            continue;
        }

        /* Part of a word. */
        BEGIN_WORD();
        if (c == '\'') {
            p++;
            while (*p && *p != '\'' && *p != '\n')
                *w++ = *p++;
            if (*p != '\'') {
                ms_err("unexpected EOF while looking for matching `''");
                return -1;
            }
            p++;
        } else if (c == '"') {
            p++;
            while (*p && *p != '"' && *p != '\n') {
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) {
                    *w++ = p[1];
                    p += 2;
                } else {
                    *w++ = *p++;
                }
            }
            if (*p != '"') {
                ms_err("unexpected EOF while looking for matching `\"'");
                return -1;
            }
            p++;
        } else if (c == '\\') {
            if (p[1] == '\0' || p[1] == '\n') {
                ms_err("syntax error: trailing backslash");
                return -1;
            }
            *w++ = p[1];
            p += 2;
        } else {
            *w++ = c;
            p++;
        }
    }

    lx->toks[lx->ntoks].type = T_EOL;   /* sentinel, slot reserved */
    lx->toks[lx->ntoks].text = NULL;
    return 0;
#undef END_WORD
#undef BEGIN_WORD
}
