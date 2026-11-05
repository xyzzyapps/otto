#include "lexer.h"
#include "ast.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef _WIN32
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

static bool is_ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_' || c == '$' || c == '*' || c == '.' || c == '/';
}

static bool is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '/' || c == '\\' || c == '*';
}

bool token_is_filler(const char *word, size_t len) {
    static const char *fillers[] = {
        "the", "to", "of", "an", "set", "all", NULL
    };
    for (int i = 0; fillers[i]; i++) {
        size_t flen = strlen(fillers[i]);
        if (len == flen && strncasecmp(word, fillers[i], flen) == 0) {
            return true;
        }
    }
    return false;
}

void lexer_init(Lexer *lex, const char *source, size_t length, bool interactive) {
    lex->source = source;
    lex->length = length;
    lex->cursor = 0;
    lex->line = 1;
    lex->col = 1;
    lex->interactive = interactive;
    lex->error_msg = NULL;
}

static char peek_char(Lexer *lex) {
    if (lex->cursor >= lex->length) return '\0';
    return lex->source[lex->cursor];
}

static char peek_next_char(Lexer *lex) {
    if (lex->cursor + 1 >= lex->length) return '\0';
    return lex->source[lex->cursor + 1];
}

static char next_char(Lexer *lex) {
    if (lex->cursor >= lex->length) return '\0';
    char c = lex->source[lex->cursor++];
    if (c == '\n') {
        lex->line++;
        lex->col = 1;
    } else {
        lex->col++;
    }
    return c;
}

static void skip_whitespace_and_comments(Lexer *lex) {
    while (lex->cursor < lex->length) {
        char c = peek_char(lex);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            next_char(lex);
        } else if (c == '#') {
            // Comment until newline
            while (lex->cursor < lex->length && peek_char(lex) != '\n') {
                next_char(lex);
            }
        } else {
            break;
        }
    }
}

// Check ordinal suffix: 1st, 2nd, 3rd, 4th, 10th
static bool is_ordinal_suffix(const char *p, size_t rem, int *suffix_len) {
    if (rem < 2) return false;
    if ((p[0] == 's' && p[1] == 't') ||
        (p[0] == 'n' && p[1] == 'd') ||
        (p[0] == 'r' && p[1] == 'd') ||
        (p[0] == 't' && p[1] == 'h')) {
        *suffix_len = 2;
        return true;
    }
    return false;
}

static OttoTokenType classify_word(const char *word, size_t len) {
    // 1. Hooks
    if (len == 6 && strncasecmp(word, "before", 6) == 0) return TOK_HOOK_BEFORE;
    if (len == 5 && strncasecmp(word, "every", 5) == 0) return TOK_HOOK_EVERY;
    if (len == 5 && strncasecmp(word, "after", 5) == 0) return TOK_HOOK_AFTER;
    if (len == 2 && strncasecmp(word, "on", 2) == 0) return TOK_HOOK_ON;
    if (len == 6 && strncasecmp(word, "extern", 6) == 0) return TOK_HOOK_EXTERN;
    if (len == 5 && strncasecmp(word, "watch", 5) == 0) return TOK_HOOK_WATCH;

    // 2. Verbs
    if (len == 4 && strncasecmp(word, "read", 4) == 0) return TOK_VERB_READ;
    if (len == 5 && strncasecmp(word, "write", 5) == 0) return TOK_VERB_WRITE;
    if (len == 5 && strncasecmp(word, "print", 5) == 0) return TOK_VERB_PRINT;
    if (len == 4 && strncasecmp(word, "take", 4) == 0) return TOK_VERB_TAKE;
    if (len == 4 && strncasecmp(word, "drop", 4) == 0) return TOK_VERB_DROP;
    if (len == 5 && strncasecmp(word, "split", 5) == 0) return TOK_VERB_SPLIT;
    if (len == 4 && strncasecmp(word, "join", 4) == 0) return TOK_VERB_JOIN;
    if (len == 4 && strncasecmp(word, "sort", 4) == 0) return TOK_VERB_SORT;
    if (len == 7 && strncasecmp(word, "replace", 7) == 0) return TOK_VERB_REPLACE;
    if (len == 3 && strncasecmp(word, "add", 3) == 0) return TOK_VERB_ADD;
    if (len == 5 && strncasecmp(word, "count", 5) == 0) return TOK_VERB_COUNT;
    if (len == 6 && strncasecmp(word, "return", 6) == 0) return TOK_VERB_RETURN;
    if (len == 3 && strncasecmp(word, "run", 3) == 0) return TOK_VERB_RUN;
    if (len == 5 && strncasecmp(word, "fetch", 5) == 0) return TOK_VERB_FETCH;
    if (len == 7 && strncasecmp(word, "compile", 7) == 0) return TOK_VERB_COMPILE;
    if (len == 4 && strncasecmp(word, "undo", 4) == 0) return TOK_VERB_UNDO;
    if (len == 4 && strncasecmp(word, "pick", 4) == 0) return TOK_VERB_PICK;
    if (len == 5 && strncasecmp(word, "table", 5) == 0) return TOK_VERB_TABLE;
    if (len == 5 && strncasecmp(word, "chart", 5) == 0) return TOK_VERB_CHART;
    if (len == 6 && strncasecmp(word, "unpack", 6) == 0) return TOK_VERB_UNPACK;
    if (len == 9 && strncasecmp(word, "intersect", 9) == 0) return TOK_VERB_INTERSECT;
    if (len == 7 && strncasecmp(word, "exclude", 7) == 0) return TOK_VERB_EXCLUDE;
    if (len == 3 && strncasecmp(word, "zip", 3) == 0) return TOK_VERB_ZIP;
    if (len == 6 && strncasecmp(word, "remove", 6) == 0) return TOK_VERB_REMOVE;
    if (len == 4 && strncasecmp(word, "move", 4) == 0) return TOK_VERB_MOVE;
    if (len == 4 && strncasecmp(word, "copy", 4) == 0) return TOK_VERB_COPY;
    if (len == 4 && strncasecmp(word, "exit", 4) == 0) return TOK_VERB_EXIT;
    if (len == 4 && strncasecmp(word, "kill", 4) == 0) return TOK_VERB_KILL;
    if (len == 6 && strncasecmp(word, "filter", 6) == 0) return TOK_VERB_FILTER;
    if (len == 5 && strncasecmp(word, "tally", 5) == 0) return TOK_VERB_TALLY;
    if (len == 5 && strncasecmp(word, "group", 5) == 0) return TOK_VERB_GROUP;
    if (len == 4 && strncasecmp(word, "dirs", 4) == 0) return TOK_VERB_DIRS;
    if (len == 4 && strncasecmp(word, "back", 4) == 0) return TOK_VERB_BACK;
    if (len == 2 && strncasecmp(word, "cd", 2) == 0) return TOK_VERB_CD;
    if (len == 4 && strncasecmp(word, "jump", 4) == 0) return TOK_VERB_JUMP;
    if (len == 9 && strncasecmp(word, "benchmark", 9) == 0) return TOK_VERB_BENCHMARK;
    if (len == 4 && strncasecmp(word, "jobs", 4) == 0) return TOK_VERB_JOBS;
    if (len == 6 && strncasecmp(word, "resume", 6) == 0) return TOK_VERB_RESUME;
    if (len == 6 && strncasecmp(word, "repeat", 6) == 0) return TOK_VERB_REPEAT;
    if (len == 6 && strncasecmp(word, "search", 6) == 0) return TOK_VERB_SEARCH;
    if (len == 4 && strncasecmp(word, "stat", 4) == 0) return TOK_VERB_STAT;
    if (len == 4 && strncasecmp(word, "view", 4) == 0) return TOK_VERB_VIEW;
    if (len == 4 && strncasecmp(word, "hash", 4) == 0) return TOK_VERB_HASH;
    if (len == 6 && strncasecmp(word, "encode", 6) == 0) return TOK_VERB_ENCODE;
    if (len == 6 && strncasecmp(word, "decode", 6) == 0) return TOK_VERB_DECODE;
    if (len == 4 && strncasecmp(word, "uuid", 4) == 0) return TOK_VERB_UUID;
    if (len == 4 && strncasecmp(word, "disk", 4) == 0) return TOK_VERB_DISK;
    if (len == 4 && strncasecmp(word, "abbr", 4) == 0) return TOK_VERB_ABBR;
    if (len == 7 && strncasecmp(word, "prepend", 7) == 0) return TOK_VERB_PREPEND;
    if (len == 6 && strncasecmp(word, "append", 6) == 0) return TOK_VERB_APPEND;
    if (len == 4 && strncasecmp(word, "help", 4) == 0) return TOK_VERB_HELP;
    if (len == 8 && strncasecmp(word, "examples", 8) == 0) return TOK_VERB_EXAMPLES;
    if (len == 5 && strncasecmp(word, "tutor", 5) == 0) return TOK_VERB_TUTOR;

    // 3. Scanners
    if (len == 4 && strncasecmp(word, "line", 4) == 0) return TOK_SCAN_LINE;
    if (len == 5 && strncasecmp(word, "lines", 5) == 0) return TOK_SCAN_LINES;
    if (len == 4 && strncasecmp(word, "word", 4) == 0) return TOK_SCAN_WORD;
    if (len == 5 && strncasecmp(word, "words", 5) == 0) return TOK_SCAN_WORDS;
    if (len == 6 && strncasecmp(word, "number", 6) == 0) return TOK_SCAN_NUMBER;
    if (len == 7 && strncasecmp(word, "numbers", 7) == 0) return TOK_SCAN_NUMBERS;
    if (len == 4 && strncasecmp(word, "byte", 4) == 0) return TOK_SCAN_BYTE;
    if (len == 5 && strncasecmp(word, "bytes", 5) == 0) return TOK_SCAN_BYTES;
    if (len == 4 && strncasecmp(word, "file", 4) == 0) return TOK_SCAN_FILE;
    if (len == 5 && strncasecmp(word, "files", 5) == 0) return TOK_SCAN_FILES;
    if (len == 7 && strncasecmp(word, "process", 7) == 0) return TOK_SCAN_PROCESS;
    if (len == 9 && strncasecmp(word, "processes", 9) == 0) return TOK_SCAN_PROCESSES;
    if (len == 3 && strncasecmp(word, "row", 3) == 0) return TOK_SCAN_ROW;
    if (len == 4 && strncasecmp(word, "rows", 4) == 0) return TOK_SCAN_ROWS;
    if (len == 4 && strncasecmp(word, "json", 4) == 0) return TOK_SCAN_JSON;
    if (len == 3 && strncasecmp(word, "csv", 3) == 0) return TOK_SCAN_CSV;
    if (len == 4 && strncasecmp(word, "yaml", 4) == 0) return TOK_SCAN_YAML;
    if (len == 4 && strncasecmp(word, "toml", 4) == 0) return TOK_SCAN_TOML;
    if (len == 5 && strncasecmp(word, "jsonl", 5) == 0) return TOK_SCAN_JSONL;
    if (len == 4 && strncasecmp(word, "urls", 4) == 0) return TOK_SCAN_URLS;
    if (len == 6 && strncasecmp(word, "emails", 6) == 0) return TOK_SCAN_EMAILS;
    if (len == 4 && strncasecmp(word, "host", 4) == 0) return TOK_SCAN_HOST;
    if (len == 5 && strncasecmp(word, "hosts", 5) == 0) return TOK_SCAN_HOSTS;
    if (len == 4 && strncasecmp(word, "tree", 4) == 0) return TOK_SCAN_TREE;
    if (len == 4 && strncasecmp(word, "port", 4) == 0) return TOK_SCAN_PORT;
    if (len == 5 && strncasecmp(word, "ports", 5) == 0) return TOK_SCAN_PORTS;
    if (len == 6 && strncasecmp(word, "status", 6) == 0) return TOK_SCAN_STATUS;
    if ((len == 6 && strncasecmp(word, "commit", 6) == 0) ||
        (len == 7 && strncasecmp(word, "commits", 7) == 0)) return TOK_SCAN_COMMITS;
    if ((len == 6 && strncasecmp(word, "branch", 6) == 0) ||
        (len == 8 && strncasecmp(word, "branches", 8) == 0)) return TOK_SCAN_BRANCHES;
    if (len == 4 && strncasecmp(word, "diff", 4) == 0) return TOK_SCAN_DIFF;

    // 4. Ordinals
    if (len == 5 && strncasecmp(word, "first", 5) == 0) return TOK_ORD_FIRST;
    if (len == 6 && strncasecmp(word, "second", 6) == 0) return TOK_ORD_SECOND;
    if (len == 5 && strncasecmp(word, "third", 5) == 0) return TOK_ORD_THIRD;
    if (len == 6 && strncasecmp(word, "fourth", 6) == 0) return TOK_ORD_FOURTH;
    if (len == 4 && strncasecmp(word, "last", 4) == 0) return TOK_ORD_LAST;
    if (len == 3 && strncasecmp(word, "top", 3) == 0) return TOK_ORD_TOP;
    if (len == 4 && strncasecmp(word, "each", 4) == 0) return TOK_ORD_EACH;

    // 5. Adverbs
    if ((len == 11 && strncasecmp(word, "recursively", 11) == 0) ||
        (len == 9 && strncasecmp(word, "recursive", 9) == 0)) return TOK_ADV_RECURSIVELY;
    if ((len == 11 && strncasecmp(word, "numerically", 11) == 0) ||
        (len == 7 && strncasecmp(word, "numeric", 7) == 0)) return TOK_ADV_NUMERICALLY;
    if ((len == 10 && strncasecmp(word, "descending", 10) == 0) ||
        (len == 7 && strncasecmp(word, "reverse", 7) == 0) ||
        (len == 8 && strncasecmp(word, "reversed", 8) == 0)) return TOK_ADV_DESCENDING;
    if ((len == 8 && strncasecmp(word, "uniquely", 8) == 0) ||
        (len == 6 && strncasecmp(word, "unique", 6) == 0)) return TOK_ADV_UNIQUELY;
    if (len == 7 && strncasecmp(word, "inplace", 7) == 0) return TOK_ADV_INPLACE;
    if (len == 12 && strncasecmp(word, "concurrently", 12) == 0) return TOK_ADV_CONCURRENTLY;
    if ((len == 8 && strncasecmp(word, "casefold", 8) == 0) ||
        (len == 11 && strncasecmp(word, "insensitive", 11) == 0)) return TOK_ADV_CASEFOLD;
    if ((len == 8 && strncasecmp(word, "simulate", 8) == 0) ||
        (len == 6 && strncasecmp(word, "dryrun", 6) == 0)) return TOK_ADV_SIMULATE;
    if (len == 8 && strncasecmp(word, "parallel", 8) == 0) return TOK_ADV_PARALLEL;
    if (len == 10 && strncasecmp(word, "background", 10) == 0) return TOK_ADV_BACKGROUND;
    if ((len == 8 && strncasecmp(word, "ignoring", 8) == 0) ||
        (len == 9 && strncasecmp(word, "gitignore", 9) == 0)) return TOK_ADV_IGNORING;
    if ((len == 9 && strncasecmp(word, "inverting", 9) == 0) ||
        (len == 8 && strncasecmp(word, "inverted", 8) == 0)) return TOK_ADV_INVERT;

    // 6. Prepositions
    if (len == 4 && strncasecmp(word, "into", 4) == 0) return TOK_PREP_INTO;
    if (len == 4 && strncasecmp(word, "onto", 4) == 0) return TOK_PREP_ONTO;
    if (len == 4 && strncasecmp(word, "from", 4) == 0) return TOK_PREP_FROM;
    if (len == 4 && strncasecmp(word, "with", 4) == 0) return TOK_PREP_WITH;
    if (len == 2 && strncasecmp(word, "by", 2) == 0) return TOK_PREP_BY;
    if (len == 5 && strncasecmp(word, "where", 5) == 0) return TOK_PREP_WHERE;
    if (len == 8 && strncasecmp(word, "matching", 8) == 0) return TOK_PREP_MATCHING;
    if (len == 2 && strncasecmp(word, "at", 2) == 0) return TOK_PREP_AT;
    if (len == 2 && strncasecmp(word, "in", 2) == 0) return TOK_PREP_IN;
    if (len == 7 && strncasecmp(word, "discard", 7) == 0) return TOK_PREP_DISCARD;
    if (len == 9 && strncasecmp(word, "replacing", 9) == 0) return TOK_PREP_REPLACING;

    // 7. Conjunction words
    if (len == 3 && strncasecmp(word, "and", 3) == 0) return TOK_AND;
    if (len == 2 && strncasecmp(word, "or", 2) == 0) return TOK_OR;

    // Condition keywords
    if (len == 2 && strncasecmp(word, "if", 2) == 0) return TOK_IF;
    if (len == 4 && strncasecmp(word, "else", 4) == 0) return TOK_ELSE;
    if (len == 6 && strncasecmp(word, "unless", 6) == 0) return TOK_UNLESS;

    // Registers / Pronouns
    if (len == 2 && strncasecmp(word, "it", 2) == 0) return TOK_REG_IT;
    if (len == 4 && strncasecmp(word, "them", 4) == 0) return TOK_REG_THEM;
    if (len == 4 && strncasecmp(word, "that", 4) == 0) return TOK_REG_THAT;
    if (len == 3 && strncasecmp(word, "acc", 3) == 0) return TOK_REG_ACC;
    if (len == 3 && strncasecmp(word, "cur", 3) == 0) return TOK_REG_CUR;
    if (len == 4 && strncasecmp(word, "item", 4) == 0) return TOK_REG_ITEM;
    if (len == 5 && strncasecmp(word, "error", 5) == 0) return TOK_REG_ERROR;
    if (len == 8 && strncasecmp(word, "previous", 8) == 0) return TOK_REG_PREVIOUS;

    // Literals
    if (len == 4 && strncasecmp(word, "true", 4) == 0) return TOK_BOOL;
    if (len == 5 && strncasecmp(word, "false", 5) == 0) return TOK_BOOL;
    if (len == 3 && strncasecmp(word, "nil", 3) == 0) return TOK_NIL;

    return TOK_IDENT;
}

Token lexer_next(Lexer *lex) {
    skip_whitespace_and_comments(lex);
    
    Token tok;
    memset(&tok, 0, sizeof(tok));
    tok.line = lex->line;
    tok.col = lex->col;

    if (lex->cursor >= lex->length) {
        tok.type = TOK_EOF;
        tok.lexeme = (Slice){"", 0};
        return tok;
    }

    const char *start = lex->source + lex->cursor;
    char c = peek_char(lex);

    // Genitive check: 's
    if (c == '\'' && peek_next_char(lex) == 's') {
        next_char(lex); // '
        next_char(lex); // s
        tok.type = TOK_GENITIVE;
        tok.lexeme = (Slice){start, 2};
        return tok;
    }

    // Punctuation and single-character operators
    if (c == ',') {
        next_char(lex);
        tok.type = TOK_COMMA;
        tok.lexeme = (Slice){start, 1};
        return tok;
    }
    if (c == '.') {
        if (isalnum((unsigned char)peek_next_char(lex)) || peek_next_char(lex) == '/' ||
            peek_next_char(lex) == '\\' || peek_next_char(lex) == '.') {
            // Fallthrough to identifier scanner
        } else {
            next_char(lex);
            tok.type = TOK_DOT;
            tok.lexeme = (Slice){start, 1};
            return tok;
        }
    }
    if (c == ':') {
        // Could be port literal like :8080 if followed by digits
        if (isdigit((unsigned char)peek_next_char(lex))) {
            next_char(lex); // :
            while (isdigit((unsigned char)peek_char(lex))) next_char(lex);
            tok.type = TOK_UNIT_LIT;
            tok.lexeme = (Slice){start, (size_t)(lex->source + lex->cursor - start)};
            return tok;
        }
        next_char(lex);
        tok.type = TOK_COLON;
        tok.lexeme = (Slice){start, 1};
        return tok;
    }
    if (c == '(') { next_char(lex); tok.type = TOK_LPAREN; tok.lexeme = (Slice){start, 1}; return tok; }
    if (c == ')') { next_char(lex); tok.type = TOK_RPAREN; tok.lexeme = (Slice){start, 1}; return tok; }
    if (c == '[') { next_char(lex); tok.type = TOK_LBRACKET; tok.lexeme = (Slice){start, 1}; return tok; }
    if (c == ']') { next_char(lex); tok.type = TOK_RBRACKET; tok.lexeme = (Slice){start, 1}; return tok; }
    if (c == '{') { next_char(lex); tok.type = TOK_LBRACE; tok.lexeme = (Slice){start, 1}; return tok; }
    if (c == '}') { next_char(lex); tok.type = TOK_RBRACE; tok.lexeme = (Slice){start, 1}; return tok; }
    if (c == '&') { next_char(lex); tok.type = TOK_AMP; tok.lexeme = (Slice){start, 1}; return tok; }

    // Multi-char operators: ==, !=, <=, >=, etc.
    if (c == '=') {
        next_char(lex);
        if (peek_char(lex) == '=') {
            next_char(lex);
            tok.type = TOK_EQ;
            tok.lexeme = (Slice){start, 2};
        } else {
            tok.type = TOK_ERROR;
            tok.lexeme = (Slice){start, 1};
            lex->error_msg = "Unexpected '=' operator (use '==' for equality or 'into' for assignment)";
        }
        return tok;
    }
    if (c == '!') {
        next_char(lex);
        if (peek_char(lex) == '=') {
            next_char(lex);
            tok.type = TOK_NE;
            tok.lexeme = (Slice){start, 2};
            return tok;
        }
        tok.type = TOK_ERROR;
        lex->error_msg = "Unexpected '!' operator (use '!=' or 'unless')";
        return tok;
    }
    if (c == '<') {
        next_char(lex);
        if (peek_char(lex) == '=') {
            next_char(lex);
            tok.type = TOK_LE;
            tok.lexeme = (Slice){start, 2};
        } else {
            tok.type = TOK_LT;
            tok.lexeme = (Slice){start, 1};
        }
        return tok;
    }
    if (c == '>') {
        next_char(lex);
        if (peek_char(lex) == '=') {
            next_char(lex);
            tok.type = TOK_GE;
            tok.lexeme = (Slice){start, 2};
        } else {
            tok.type = TOK_GT;
            tok.lexeme = (Slice){start, 1};
        }
        return tok;
    }
    if (c == '+') { next_char(lex); tok.type = TOK_PLUS; tok.lexeme = (Slice){start, 1}; return tok; }
    if (c == '-') {
        char n1 = peek_next_char(lex);
        if (isdigit((unsigned char)n1)) {
            // fallthrough to number lexer
        } else if (n1 == '-' && lex->cursor + 2 < lex->length && isalpha((unsigned char)lex->source[lex->cursor + 2])) {
            // Long CLI flags: --recursive, --ignore-case, --numeric, --reverse, --unique, --inplace, --dry-run, etc.
            size_t flen = 0;
            const char *fstart = lex->source + lex->cursor + 2;
            while (lex->cursor + 2 + flen < lex->length) {
                char fc = fstart[flen];
                if (isalnum((unsigned char)fc) || fc == '-') flen++;
                else break;
            }
            char after = (lex->cursor + 2 + flen < lex->length) ? fstart[flen] : '\0';
            if (flen > 0 && !is_ident_char(after) && after != '.') {
                OttoTokenType ftype = TOK_ERROR;
                uint16_t fmask = 0;
                bool strip_prefix = false;

                if ((flen == 9 && strncasecmp(fstart, "recursive", 9) == 0)) { ftype = TOK_ADV_RECURSIVELY; fmask = ADV_RECURSIVELY; }
                else if ((flen == 11 && strncasecmp(fstart, "ignore-case", 11) == 0) ||
                         (flen == 8 && strncasecmp(fstart, "casefold", 8) == 0) ||
                         (flen == 11 && strncasecmp(fstart, "insensitive", 11) == 0)) { ftype = TOK_ADV_CASEFOLD; fmask = ADV_CASEFOLD; }
                else if ((flen == 7 && strncasecmp(fstart, "numeric", 7) == 0) ||
                         (flen == 12 && strncasecmp(fstart, "numeric-sort", 12) == 0)) { ftype = TOK_ADV_NUMERICALLY; fmask = ADV_NUMERICALLY; }
                else if ((flen == 7 && strncasecmp(fstart, "reverse", 7) == 0) ||
                         (flen == 4 && strncasecmp(fstart, "desc", 4) == 0) ||
                         (flen == 10 && strncasecmp(fstart, "descending", 10) == 0)) { ftype = TOK_ADV_DESCENDING; fmask = ADV_DESCENDING; }
                else if (flen == 6 && strncasecmp(fstart, "unique", 6) == 0) { ftype = TOK_ADV_UNIQUELY; fmask = ADV_UNIQUELY; }
                else if ((flen == 7 && strncasecmp(fstart, "inplace", 7) == 0) ||
                         (flen == 8 && strncasecmp(fstart, "in-place", 8) == 0)) { ftype = TOK_ADV_INPLACE; fmask = ADV_INPLACE; }
                else if ((flen == 8 && strncasecmp(fstart, "simulate", 8) == 0) ||
                         (flen == 7 && strncasecmp(fstart, "dry-run", 7) == 0) ||
                         (flen == 6 && strncasecmp(fstart, "dryrun", 6) == 0)) { ftype = TOK_ADV_SIMULATE; fmask = ADV_SIMULATE; }
                else if ((flen == 8 && strncasecmp(fstart, "parallel", 8) == 0) ||
                         (flen == 10 && strncasecmp(fstart, "concurrent", 10) == 0) ||
                         (flen == 12 && strncasecmp(fstart, "concurrently", 12) == 0)) { ftype = TOK_ADV_FLAGS; fmask = ADV_PARALLEL | ADV_CONCURRENTLY; }
                else if ((flen == 10 && strncasecmp(fstart, "background", 10) == 0) ||
                         (flen == 2 && strncasecmp(fstart, "bg", 2) == 0)) { ftype = TOK_ADV_BACKGROUND; fmask = ADV_BACKGROUND; }
                else if ((flen == 10 && strncasecmp(fstart, "ignore-git", 10) == 0) ||
                         (flen == 6 && strncasecmp(fstart, "no-git", 6) == 0) ||
                         (flen == 9 && strncasecmp(fstart, "gitignore", 9) == 0)) { ftype = TOK_ADV_FLAGS; fmask = ADV_IGNORING_GIT; }
                else if (flen == 5 && strncasecmp(fstart, "count", 5) == 0) { ftype = TOK_ADV_FLAGS; fmask = ADV_COUNT; }
                else if ((flen == 6 && strncasecmp(fstart, "invert", 6) == 0) ||
                         (flen == 12 && strncasecmp(fstart, "invert-match", 12) == 0)) { ftype = TOK_ADV_INVERT; fmask = ADV_INVERT; }
                else if (flen == 6 && strncasecmp(fstart, "append", 6) == 0) { ftype = TOK_ADV_FLAGS; fmask = ADV_APPEND; }
                // Scanner long flags
                else if (flen == 4 && strncasecmp(fstart, "json", 4) == 0) ftype = TOK_SCAN_JSON;
                else if (flen == 4 && strncasecmp(fstart, "yaml", 4) == 0) ftype = TOK_SCAN_YAML;
                else if (flen == 4 && strncasecmp(fstart, "toml", 4) == 0) ftype = TOK_SCAN_TOML;
                else if (flen == 3 && strncasecmp(fstart, "csv", 3) == 0) ftype = TOK_SCAN_CSV;
                else if (flen == 5 && strncasecmp(fstart, "jsonl", 5) == 0) ftype = TOK_SCAN_JSONL;
                else if (flen == 4 && strncasecmp(fstart, "tree", 4) == 0) ftype = TOK_SCAN_TREE;
                else if ((flen == 4 && strncasecmp(fstart, "file", 4) == 0) ||
                         (flen == 5 && strncasecmp(fstart, "files", 5) == 0)) ftype = TOK_SCAN_FILES;
                else if ((flen == 4 && strncasecmp(fstart, "line", 4) == 0) ||
                         (flen == 5 && strncasecmp(fstart, "lines", 5) == 0)) ftype = TOK_SCAN_LINES;
                else if ((flen == 4 && strncasecmp(fstart, "word", 4) == 0) ||
                         (flen == 5 && strncasecmp(fstart, "words", 5) == 0)) ftype = TOK_SCAN_WORDS;
                else if ((flen == 4 && strncasecmp(fstart, "byte", 4) == 0) ||
                         (flen == 5 && strncasecmp(fstart, "bytes", 5) == 0)) ftype = TOK_SCAN_BYTES;
                else if ((flen == 6 && strncasecmp(fstart, "number", 6) == 0) ||
                         (flen == 7 && strncasecmp(fstart, "numbers", 7) == 0)) ftype = TOK_SCAN_NUMBERS;
                else if (flen == 4 && strncasecmp(fstart, "urls", 4) == 0) ftype = TOK_SCAN_URLS;
                else if (flen == 6 && strncasecmp(fstart, "emails", 6) == 0) ftype = TOK_SCAN_EMAILS;
                else if ((flen == 4 && strncasecmp(fstart, "port", 4) == 0) ||
                         (flen == 5 && strncasecmp(fstart, "ports", 5) == 0)) ftype = TOK_SCAN_PORTS;
                else if ((flen == 7 && strncasecmp(fstart, "process", 7) == 0) ||
                         (flen == 9 && strncasecmp(fstart, "processes", 9) == 0)) ftype = TOK_SCAN_PROCESSES;
                else if (flen == 6 && strncasecmp(fstart, "status", 6) == 0) ftype = TOK_SCAN_STATUS;
                else if ((flen == 6 && strncasecmp(fstart, "commit", 6) == 0) ||
                         (flen == 7 && strncasecmp(fstart, "commits", 7) == 0)) ftype = TOK_SCAN_COMMITS;
                else if ((flen == 6 && strncasecmp(fstart, "branch", 6) == 0) ||
                         (flen == 8 && strncasecmp(fstart, "branches", 8) == 0)) ftype = TOK_SCAN_BRANCHES;
                else if (flen == 4 && strncasecmp(fstart, "diff", 4) == 0) ftype = TOK_SCAN_DIFF;
                // Preposition long flags
                else if ((flen == 5 && strncasecmp(fstart, "match", 5) == 0) ||
                         (flen == 8 && strncasecmp(fstart, "matching", 8) == 0)) ftype = TOK_PREP_MATCHING;
                else if ((flen == 3 && strncasecmp(fstart, "key", 3) == 0) ||
                         (flen == 2 && strncasecmp(fstart, "by", 2) == 0)) ftype = TOK_PREP_BY;
                else if ((flen == 6 && strncasecmp(fstart, "output", 6) == 0) ||
                         (flen == 4 && strncasecmp(fstart, "into", 4) == 0)) ftype = TOK_PREP_INTO;
                else if (flen == 4 && strncasecmp(fstart, "onto", 4) == 0) ftype = TOK_PREP_ONTO;
                else if (flen == 4 && strncasecmp(fstart, "from", 4) == 0) ftype = TOK_PREP_FROM;
                else if (flen == 4 && strncasecmp(fstart, "with", 4) == 0) ftype = TOK_PREP_WITH;
                else if (flen == 5 && strncasecmp(fstart, "where", 5) == 0) ftype = TOK_PREP_WHERE;
                else if (flen == 2 && strncasecmp(fstart, "in", 2) == 0) ftype = TOK_PREP_IN;
                else if (flen == 2 && strncasecmp(fstart, "at", 2) == 0) ftype = TOK_PREP_AT;
                else if ((flen == 5 && strncasecmp(fstart, "quiet", 5) == 0) ||
                         (flen == 6 && strncasecmp(fstart, "silent", 6) == 0) ||
                         (flen == 7 && strncasecmp(fstart, "discard", 7) == 0)) ftype = TOK_PREP_DISCARD;
                // Algorithm long flags (--sha256, --crc32, --base64, --hex, --url)
                else if ((flen == 6 && strncasecmp(fstart, "sha256", 6) == 0) ||
                         (flen == 5 && strncasecmp(fstart, "crc32", 5) == 0) ||
                         (flen == 6 && strncasecmp(fstart, "base64", 6) == 0) ||
                         (flen == 3 && strncasecmp(fstart, "hex", 3) == 0) ||
                         (flen == 3 && strncasecmp(fstart, "url", 3) == 0)) {
                    ftype = TOK_IDENT;
                    strip_prefix = true;
                }

                if (ftype != TOK_ERROR) {
                    for (size_t k = 0; k < 2 + flen; k++) next_char(lex);
                    tok.type = ftype;
                    tok.lit.i64 = fmask;
                    tok.lexeme = strip_prefix ? (Slice){fstart, flen} : (Slice){start, 2 + flen};
                    return tok;
                }
            }
            next_char(lex);
            tok.type = TOK_MINUS;
            tok.lexeme = (Slice){start, 1};
            return tok;
        } else if (isalpha((unsigned char)n1)) {
            // Short CLI flags (-i, -r, -n, -u, -d, -s, -p, -b, -c, -v, -a, -I, -l, -w, -e, -m, -k, -o, -f, -q, or combined -ir, -nr, -rn, -nu, -iu, -ic, etc.)
            size_t flen = 0;
            const char *fstart = lex->source + lex->cursor + 1;
            while (lex->cursor + 1 + flen < lex->length && isalnum((unsigned char)fstart[flen])) {
                flen++;
            }
            char after = (lex->cursor + 1 + flen < lex->length) ? fstart[flen] : '\0';
            if (flen >= 1 && flen <= 4 && !is_ident_char(after) && after != '.') {
                if (flen == 1) {
                    OttoTokenType stype = TOK_ERROR;
                    if (n1 == 'l') stype = TOK_SCAN_LINES;
                    else if (n1 == 'w') stype = TOK_SCAN_WORDS;
                    else if (n1 == 'e' || n1 == 'm') stype = TOK_PREP_MATCHING;
                    else if (n1 == 'k') stype = TOK_PREP_BY;
                    else if (n1 == 'o') stype = TOK_PREP_INTO;
                    else if (n1 == 'f') stype = TOK_PREP_FROM;
                    else if (n1 == 'q') stype = TOK_PREP_DISCARD;
                    if (stype != TOK_ERROR) {
                        next_char(lex); // '-'
                        next_char(lex); // flag char
                        tok.type = stype;
                        tok.lexeme = (Slice){start, 2};
                        return tok;
                    }
                }
                uint16_t mask = 0;
                bool all_adv = true;
                for (size_t k = 0; k < flen; k++) {
                    char fc = fstart[k];
                    if (fc == 'i') mask |= ADV_CASEFOLD;
                    else if (fc == 'I') mask |= ADV_INPLACE;
                    else if (fc == 'r' || fc == 'R') mask |= ADV_RECURSIVELY;
                    else if (fc == 'n') mask |= ADV_NUMERICALLY;
                    else if (fc == 'd') mask |= ADV_DESCENDING;
                    else if (fc == 'u') mask |= ADV_UNIQUELY;
                    else if (fc == 's') mask |= ADV_SIMULATE;
                    else if (fc == 'p' || fc == 'P') mask |= (ADV_PARALLEL | ADV_CONCURRENTLY);
                    else if (fc == 'b') mask |= ADV_BACKGROUND;
                    else if (fc == 'c') mask |= ADV_COUNT;
                    else if (fc == 'v') mask |= ADV_INVERT;
                    else if (fc == 'a') mask |= ADV_APPEND;
                    else { all_adv = false; break; }
                }
                if (all_adv && mask != 0) {
                    for (size_t k = 0; k < 1 + flen; k++) next_char(lex);
                    if (mask == ADV_CASEFOLD) tok.type = TOK_ADV_CASEFOLD;
                    else if (mask == ADV_RECURSIVELY) tok.type = TOK_ADV_RECURSIVELY;
                    else if (mask == (ADV_CASEFOLD | ADV_RECURSIVELY)) tok.type = TOK_ADV_CASEFOLD_REC;
                    else tok.type = TOK_ADV_FLAGS;
                    tok.lit.i64 = mask;
                    tok.lexeme = (Slice){start, 1 + flen};
                    return tok;
                }
            }
            next_char(lex);
            tok.type = TOK_MINUS;
            tok.lexeme = (Slice){start, 1};
            return tok;
        } else {
            next_char(lex);
            tok.type = TOK_MINUS;
            tok.lexeme = (Slice){start, 1};
            return tok;
        }
    }
    if (c == '*') {
        if (peek_next_char(lex) == '.' || isalnum((unsigned char)peek_next_char(lex))) {
            // Fallthrough to identifier scanner for globs like *.txt
        } else {
            next_char(lex); tok.type = TOK_STAR; tok.lexeme = (Slice){start, 1}; return tok;
        }
    }
    if (c == '/') { next_char(lex); tok.type = TOK_SLASH; tok.lexeme = (Slice){start, 1}; return tok; }
    if (c == '%') { next_char(lex); tok.type = TOK_PERCENT; tok.lexeme = (Slice){start, 1}; return tok; }

    // Numbers & Unit Literals & Ordinals (e.g. 1st, 2nd, 3rd, 4th, 10mb, 2s, 500ms)
    if (isdigit((unsigned char)c) || (c == '-' && isdigit((unsigned char)peek_next_char(lex)))) {
        bool is_float = false;
        next_char(lex); // consume first digit or minus
        while (isdigit((unsigned char)peek_char(lex))) next_char(lex);
        if (peek_char(lex) == '.' && isdigit((unsigned char)peek_next_char(lex))) {
            is_float = true;
            next_char(lex); // .
            while (isdigit((unsigned char)peek_char(lex))) next_char(lex);
        }

        // Check if immediately followed by ordinal suffix (st, nd, rd, th)
        int suf_len = 0;
        if (!is_float && is_ordinal_suffix(lex->source + lex->cursor, lex->length - lex->cursor, &suf_len)) {
            for (int k = 0; k < suf_len; k++) next_char(lex);
            tok.type = TOK_ORD_NTH;
            tok.lexeme = (Slice){start, (size_t)(lex->source + lex->cursor - start)};
            // parse integer value
            bool ok;
            Slice num_slice = {start, tok.lexeme.len - suf_len};
            tok.lit.nth_val = (int)slice_to_i64(num_slice, &ok);
            return tok;
        }

        // Check unit suffix: ms, s, m, h, d, b, kb, mb, gb
        char u = tolower((unsigned char)peek_char(lex));
        if (isalpha((unsigned char)u)) {
            while (isalpha((unsigned char)peek_char(lex))) next_char(lex);
            tok.type = TOK_UNIT_LIT;
            tok.lexeme = (Slice){start, (size_t)(lex->source + lex->cursor - start)};
            return tok;
        }

        tok.type = TOK_NUMBER;
        tok.lexeme = (Slice){start, (size_t)(lex->source + lex->cursor - start)};
        bool ok;
        if (is_float) {
            tok.lit.f64 = slice_to_f64(tok.lexeme, &ok);
        } else {
            tok.lit.i64 = slice_to_i64(tok.lexeme, &ok);
        }
        return tok;
    }

    // Strings: "..."
    if (c == '"') {
        next_char(lex); // opening quote
        const char *str_content = lex->source + lex->cursor;
        while (lex->cursor < lex->length && peek_char(lex) != '"') {
            if (peek_char(lex) == '\\' && lex->cursor + 1 < lex->length) {
                next_char(lex); // skip backslash
            }
            next_char(lex);
        }
        size_t content_len = (size_t)(lex->source + lex->cursor - str_content);
        if (peek_char(lex) == '"') {
            next_char(lex); // closing quote
        } else {
            tok.type = TOK_ERROR;
            lex->error_msg = "Unterminated string literal";
            return tok;
        }
        tok.type = TOK_STRING;
        tok.lexeme = (Slice){str_content, content_len};
        return tok;
    }

    // Identifiers, keywords, registers, filenames
    if (is_ident_start(c)) {
        next_char(lex); // always consume the leading ident character (e.g. '$', letter, '_', '*')
        while (lex->cursor < lex->length) {
            char ch = peek_char(lex);
            if (ch == '\'' && peek_next_char(lex) == 's') {
                break;
            }
            if (ch == '.' && (isalnum((unsigned char)peek_next_char(lex)) || peek_next_char(lex) == '/' || peek_next_char(lex) == '\\' || peek_next_char(lex) == '.')) {
                next_char(lex);
                continue;
            }
            if (ch == ':' && isalnum((unsigned char)peek_next_char(lex))) {
                const char *s = start;
                bool has_dot_or_slash = false;
                while (s < lex->source + lex->cursor) {
                    if (*s == '.' || *s == '/') { has_dot_or_slash = true; break; }
                    s++;
                }
                if (has_dot_or_slash) {
                    next_char(lex);
                    continue;
                }
            }
            if (is_ident_char(ch)) {
                next_char(lex);
            } else {
                break;
            }
        }
        size_t wlen = (size_t)(lex->source + lex->cursor - start);
        
        // Strict Zero-Filler Check
        if (token_is_filler(start, wlen)) {
            tok.type = TOK_ERROR;
            tok.lexeme = (Slice){start, wlen};
            lex->error_msg = "Decorative filler word rejected by zero-filler rule";
            return tok;
        }

        tok.type = classify_word(start, wlen);
        tok.lexeme = (Slice){start, wlen};
        if (tok.type == TOK_BOOL) {
            tok.lit.i64 = (wlen == 4 && strncasecmp(start, "true", 4) == 0) ? 1 : 0;
        }
        return tok;
    }

    // Unknown character
    next_char(lex);
    tok.type = TOK_ERROR;
    tok.lexeme = (Slice){start, 1};
    lex->error_msg = "Unexpected character";
    return tok;
}

Token lexer_peek(Lexer *lex) {
    size_t save_cursor = lex->cursor;
    int save_line = lex->line;
    int save_col = lex->col;
    Token tok = lexer_next(lex);
    lex->cursor = save_cursor;
    lex->line = save_line;
    lex->col = save_col;
    return tok;
}

bool token_is_hook(OttoTokenType type) {
    return type >= TOK_HOOK_BEFORE && type <= TOK_HOOK_WATCH;
}

bool token_is_verb(OttoTokenType type) {
    return type >= TOK_VERB_READ && type <= TOK_VERB_TUTOR;
}

bool token_is_scanner(OttoTokenType type) {
    return type >= TOK_SCAN_LINE && type <= TOK_SCAN_DIFF;
}

bool token_is_ordinal(OttoTokenType type) {
    return type >= TOK_ORD_FIRST && type <= TOK_ORD_EACH;
}

bool token_is_adverb(OttoTokenType type) {
    return type >= TOK_ADV_RECURSIVELY && type <= TOK_ADV_IGNORING;
}

bool token_is_prep(OttoTokenType type) {
    return type >= TOK_PREP_INTO && type <= TOK_PREP_REPLACING;
}

const char *token_type_name(OttoTokenType type) {
    switch (type) {
        case TOK_EOF: return "EOF";
        case TOK_ERROR: return "ERROR";
        case TOK_HOOK_BEFORE: return "before";
        case TOK_HOOK_EVERY: return "every";
        case TOK_HOOK_AFTER: return "after";
        case TOK_HOOK_ON: return "on";
        case TOK_HOOK_EXTERN: return "extern";
        case TOK_VERB_READ: return "read";
        case TOK_VERB_WRITE: return "write";
        case TOK_VERB_PRINT: return "print";
        case TOK_VERB_TAKE: return "take";
        case TOK_VERB_DROP: return "drop";
        case TOK_VERB_SPLIT: return "split";
        case TOK_VERB_JOIN: return "join";
        case TOK_VERB_SORT: return "sort";
        case TOK_VERB_REPLACE: return "replace";
        case TOK_VERB_ADD: return "add";
        case TOK_VERB_COUNT: return "count";
        case TOK_VERB_RETURN: return "return";
        case TOK_VERB_RUN: return "run";
        case TOK_VERB_FETCH: return "fetch";
        case TOK_VERB_COMPILE: return "compile";
        case TOK_VERB_UNDO: return "undo";
        case TOK_VERB_PICK: return "pick";
        case TOK_VERB_TABLE: return "table";
        case TOK_VERB_CHART: return "chart";
        case TOK_VERB_UNPACK: return "unpack";
        case TOK_VERB_INTERSECT: return "intersect";
        case TOK_VERB_EXCLUDE: return "exclude";
        case TOK_VERB_ZIP: return "zip";
        case TOK_VERB_REMOVE: return "remove";
        case TOK_VERB_MOVE: return "move";
        case TOK_VERB_COPY: return "copy";
        case TOK_VERB_EXIT: return "exit";
        case TOK_VERB_KILL: return "kill";
        case TOK_VERB_FILTER: return "filter";
        case TOK_VERB_TALLY: return "tally";
        case TOK_VERB_GROUP: return "group";
        case TOK_VERB_DIRS: return "dirs";
        case TOK_VERB_BACK: return "back";
        case TOK_VERB_CD: return "cd";
        case TOK_VERB_JUMP: return "jump";
        case TOK_VERB_BENCHMARK: return "benchmark";
        case TOK_VERB_JOBS: return "jobs";
        case TOK_VERB_RESUME: return "resume";
        case TOK_VERB_REPEAT: return "repeat";
        case TOK_VERB_SEARCH: return "search";
        case TOK_VERB_STAT: return "stat";
        case TOK_VERB_VIEW: return "view";
        case TOK_VERB_HASH: return "hash";
        case TOK_VERB_ENCODE: return "encode";
        case TOK_VERB_DECODE: return "decode";
        case TOK_VERB_UUID: return "uuid";
        case TOK_VERB_DISK: return "disk";
        case TOK_VERB_ABBR: return "abbr";
        case TOK_VERB_PREPEND: return "prepend";
        case TOK_VERB_APPEND: return "append";
        case TOK_VERB_HELP: return "help";
        case TOK_VERB_EXAMPLES: return "examples";
        case TOK_VERB_TUTOR: return "tutor";
        case TOK_COMMA: return ",";
        case TOK_AND: return "and";
        case TOK_OR: return "or";
        case TOK_DOT: return ".";
        case TOK_COLON: return ":";
        case TOK_GENITIVE: return "'s";
        case TOK_REG_THAT: return "that";
        case TOK_REG_THEM: return "them";
        case TOK_REG_IT: return "it";
        case TOK_IDENT: return "IDENT";
        case TOK_STRING: return "STRING";
        case TOK_NUMBER: return "NUMBER";
        default: return "TOKEN";
    }
}

const char * const *lexer_all_keywords(void) {
    static const char * const kws[] = {
        // Hooks
        "before", "every", "after", "on", "extern", "watch",
        // Verbs
        "read", "write", "print", "take", "drop", "split", "join", "sort",
        "replace", "add", "count", "return", "run", "fetch", "compile", "undo",
        "pick", "table", "chart", "unpack", "intersect", "exclude", "zip",
        "remove", "move", "copy", "exit", "kill", "filter", "tally", "group",
        "dirs", "back", "cd", "jump", "benchmark", "jobs", "resume", "repeat",
        "search", "stat", "view", "hash", "encode", "decode", "uuid", "disk",
        "abbr", "prepend", "append", "help", "examples", "tutor",
        // Scanners
        "line", "lines", "word", "words", "number", "numbers", "byte", "bytes",
        "file", "files", "process", "processes", "row", "rows", "json", "csv",
        "yaml", "toml", "jsonl", "urls", "emails", "host", "hosts", "tree",
        "port", "ports", "status", "commit", "commits", "branch", "branches", "diff",
        // Ordinals & Pronouns
        "first", "second", "third", "fourth", "last", "top", "each",
        "it", "them", "that", "error", "previous",
        // Adverbs
        "recursively", "recursive", "numerically", "numeric", "descending", "reverse", "reversed",
        "uniquely", "unique", "inplace", "concurrently", "casefold", "insensitive", "simulate",
        "dryrun", "parallel", "background", "ignoring", "gitignore", "inverting", "inverted",
        // Prepositions & Control
        "into", "onto", "from", "with", "by", "where", "matching", "at", "in",
        "discard", "replacing", "and", "or", "if", "else", "unless",
        NULL
    };
    return kws;
}
