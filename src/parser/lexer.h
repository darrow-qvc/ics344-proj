/* lexer.h - token stream for the MiniSh parser.  [parser] Karrar Alqallaf */
#ifndef MS_LEXER_H
#define MS_LEXER_H

#include "minish.h"

enum ms_tok_type {
    T_EOL = 0,   /* end of line                          */
    T_WORD,      /* argument or file name (quotes removed) */
    T_PIPE,      /* |   */
    T_LT,        /* <   */
    T_GT,        /* >   */
    T_GTGT,      /* >>  */
    T_AMP        /* &   */
};

struct ms_token {
    enum ms_tok_type type;
    char            *text;   /* points into ms_pipeline.words            */
};

struct ms_lexer {
    struct ms_token toks[MS_MAX_TOKENS + 1];   /* +1 for T_EOL */
    int             ntoks;
};

/* Tokenize `line` into `lx`; word bytes are written into `words`
 * (size MS_MAX_LINE, always enough because quoting only shrinks text).
 * Returns 0 on success, -1 after printing a syntax error. */
int ms_lex(const char *line, struct ms_lexer *lx, char *words);

/* Human-readable spelling of a token for error messages. */
const char *ms_tok_name(const struct ms_token *t);

#endif
