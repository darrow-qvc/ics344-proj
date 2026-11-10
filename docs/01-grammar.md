# 1. Shell grammar and tokenization

Owner: Karrar Alqallaf (`src/parser/`). Answers feedback item 1.

## 1.1 Tokens

The lexer (`src/parser/lexer.c`) turns one input line into a sequence of
tokens. There are six token types:

| token | spelling | notes |
|---|---|---|
| `WORD` | any run of non-blank, non-operator bytes, possibly quoted | quotes are removed by the lexer |
| `PIPE` | `\|` | |
| `LT` | `<` | |
| `GT` | `>` | |
| `GTGT` | `>>` | longest match: `>>` is one token, never two `>` |
| `AMP` | `&` | |

Blanks (space, tab, CR) separate words and are discarded. An operator ends
a word in progress, so `a|b` and `a | b` tokenize identically.

### Quoting and escaping

| form | rule | example | resulting word |
|---|---|---|---|
| `'...'` | every byte literal until the closing `'`; no escapes possible inside | `'it\'s'` is an error, `'a  "b"'` | `a  "b"` |
| `"..."` | literal except `\"` -> `"` and `\\` -> `\`; any other backslash kept verbatim | `"say \"hi\" \n"` | `say "hi" \n` |
| `\c` | outside quotes, backslash quotes the next byte | `a\ b`, `\|` | `a b`, `\|` as a word |
| adjacency | quoted and unquoted pieces concatenate into one word | `a"b c"d`, `'it''s'` | `ab cd`, `its` |
| `""` or `''` | an empty word (becomes an empty argv entry) | `echo "" \| wc -c` | prints `1` |
| `#` | at the start of a word, comment to end of line | `echo x # note` | `x` |

There is no variable expansion, globbing, tilde expansion or command
substitution (out of scope, see proposal section 4). `$HOME` and `*` are
ordinary characters.

### Lexical errors

| input | message (`minish:` prefix omitted) |
|---|---|
| unterminated `'` | ``unexpected EOF while looking for matching `'` `` |
| unterminated `"` | ``unexpected EOF while looking for matching `"` `` |
| trailing `\` | `syntax error: trailing backslash` |
| `&&`, `\|\|`, `;`, `<<`, `>&` | ``syntax error near unexpected token `&&' (not supported)`` |
| more than 256 tokens | `too many tokens (limit 256)` |

Each error discards the whole line and sets the status to 2 (bash uses 2
for syntax errors). The shell continues with the next line.

## 1.2 Grammar

```
line      := [ pipeline ] [ '&' ] EOL
pipeline  := command { '|' command }
command   := item { item }              -- must contain at least one WORD
item      := WORD | redirect
redirect  := '<' WORD | '>' WORD | '>>' WORD
```

The parser (`src/parser/parser.c`) is a hand-written recursive descent
parser, one function per rule, over the token array. It fills a
`struct ms_pipeline` with fixed-size arrays; nothing is allocated.

### Precedence and associativity

1. A redirection binds to the command whose item list it appears in. It
   may appear before, between or after the words: `< in sort > out` and
   `sort < in > out` are the same command.
2. A command binds tighter than `|`. In `a > f | b`, `a`'s stdout goes to
   `f` and `b` reads an empty pipe (same as bash).
3. Redirections are applied left to right in the child, after the pipe
   has been wired. Therefore a redirection always wins over a pipe for the
   same descriptor, and in `> a > b` both files are created and `b`
   receives the output. See docs/03 for the collision table.
4. `&` applies to the whole pipeline and is only legal as the final token.
   `ls & ls` is a syntax error in MiniSh (bash would run two jobs; lists of
   commands are out of scope).

### Syntax errors

Messages copy bash's wording so that tests can compare the two shells:

| input | message |
|---|---|
| `ls >` | ``syntax error near unexpected token `newline'`` |
| `> out` | ``syntax error near unexpected token `newline'`` (a command needs a word) |
| `\| ls` | ``syntax error near unexpected token `\|'`` |
| `ls \| \| wc` | ``syntax error near unexpected token `\|'`` |
| `ls \|` | ``syntax error near unexpected token `newline'`` (no line continuation) |
| `ls & ls` | ``syntax error near unexpected token `ls'`` |
| 17-stage pipeline | `pipeline too long (limit 16 commands)` |
| 9 redirections on one command | `too many redirections (limit 8)` |

## 1.3 Maximum sizes

All limits are compile-time constants in `include/minish.h`. They were
chosen so that the whole per-line state is about 25 KB of static memory,
which is comfortable on a Raspberry Pi Zero and still far above anything a
person types interactively.

| constant | value | what happens when exceeded |
|---|---:|---|
| `MS_MAX_LINE` | 4096 bytes | `line too long (limit 4095 bytes), ignored`; the rest of the line is drained and discarded, next line is processed normally |
| `MS_MAX_TOKENS` | 256 | `too many tokens` |
| `MS_MAX_ARGS` | 128 per command | `too many arguments` |
| `MS_MAX_PIPE` | 16 commands | `pipeline too long` |
| `MS_MAX_REDIRS` | 8 per command | `too many redirections` |
| `MS_MAX_JOBS` | 32 | `too many jobs`; the command is not started |
| `MS_JOB_TEXT` | 256 bytes | command text shown by `jobs` is truncated (execution unaffected) |

The line reader (`src/parser/main.c`) uses `read(2)` into a fixed buffer, so
a hostile 1 MB line costs no memory: bytes beyond the limit are counted and
dropped as they stream in.

## 1.4 Worked example

Input: `  cat < 'in file' | sort -r >> "out.txt" &`

Tokens: `WORD(cat) LT WORD(in file) PIPE WORD(sort) WORD(-r) GTGT WORD(out.txt) AMP EOL`

Parse tree:

```
line (background = 1)
└── pipeline
    ├── command argv=[cat]      redirs=[< "in file"]
    └── command argv=[sort,-r]  redirs=[>> "out.txt"]
```
