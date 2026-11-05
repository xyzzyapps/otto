#include "lexer.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

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
    if (len == 4 && strncasecmp(word, "urls", 4) == 0) return TOK_SCAN_URLS;
    if (len == 6 && strncasecmp(word, "emails", 6) == 0) return TOK_SCAN_EMAILS;
    if (len == 4 && strncasecmp(word, "host", 4) == 0) return TOK_SCAN_HOST;
    if (len == 5 && strncasecmp(word, "hosts", 5) == 0) return TOK_SCAN_HOSTS;
    if (len == 4 && strncasecmp(word, "tree", 4) == 0) return TOK_SCAN_TREE;

    // 4. Ordinals
    if (len == 5 && strncasecmp(word, "first", 5) == 0) return TOK_ORD_FIRST;
    if (len == 6 && strncasecmp(word, "second", 6) == 0) return TOK_ORD_SECOND;
    if (len == 5 && strncasecmp(word, "third", 5) == 0) return TOK_ORD_THIRD;
    if (len == 6 && strncasecmp(word, "fourth", 6) == 0) return TOK_ORD_FOURTH;
    if (len == 4 && strncasecmp(word, "last", 4) == 0) return TOK_ORD_LAST;
    if (len == 3 && strncasecmp(word, "top", 3) == 0) return TOK_ORD_TOP;
    if (len == 4 && strncasecmp(word, "each", 4) == 0) return TOK_ORD_EACH;

    // 5. Adverbs
    if (len == 11 && strncasecmp(word, "recursively", 11) == 0) return TOK_ADV_RECURSIVELY;
    if (len == 11 && strncasecmp(word, "numerically", 11) == 0) return TOK_ADV_NUMERICALLY;
    if (len == 10 && strncasecmp(word, "descending", 10) == 0) return TOK_ADV_DESCENDING;
    if (len == 8 && strncasecmp(word, "uniquely", 8) == 0) return TOK_ADV_UNIQUELY;
    if (len == 7 && strncasecmp(word, "inplace", 7) == 0) return TOK_ADV_INPLACE;
    if (len == 12 && strncasecmp(word, "concurrently", 12) == 0) return TOK_ADV_CONCURRENTLY;
    if (len == 8 && strncasecmp(word, "casefold", 8) == 0) return TOK_ADV_CASEFOLD;
    if (len == 8 && strncasecmp(word, "simulate", 8) == 0) return TOK_ADV_SIMULATE;
    if (len == 8 && strncasecmp(word, "parallel", 8) == 0) return TOK_ADV_PARALLEL;
    if (len == 10 && strncasecmp(word, "background", 10) == 0) return TOK_ADV_BACKGROUND;
    if (len == 8 && strncasecmp(word, "ignoring", 8) == 0) return TOK_ADV_IGNORING;

    // 6. Prepositions
    if (len == 4 && strncasecmp(word, "into", 4) == 0) return TOK_PREP_INTO;
    if (len == 4 && strncasecmp(word, "onto", 4) == 0) return TOK_PREP_ONTO;
    if (len == 4 && strncasecmp(word, "from", 4) == 0) return TOK_PREP_FROM;
    if (len == 4 && strncasecmp(word, "with", 4) == 0) return TOK_PREP_WITH;
    if (len == 2 && strncasecmp(word, "by", 2) == 0) return TOK_PREP_BY;
    if (len == 5 && strncasecmp(word, "where", 5) == 0) return TOK_PREP_WHERE;
    if (len == 8 && strncasecmp(word, "matching", 8) == 0) return TOK_PREP_MATCHING;
    if (len == 2 && strncasecmp(word, "at", 2) == 0) return TOK_PREP_AT;
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
        // Could be negative number if followed by digit
        if (isdigit((unsigned char)peek_next_char(lex))) {
            // fallthrough to number lexer
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
    return type >= TOK_VERB_READ && type <= TOK_VERB_REPEAT;
}

bool token_is_scanner(OttoTokenType type) {
    return type >= TOK_SCAN_LINE && type <= TOK_SCAN_TREE;
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
