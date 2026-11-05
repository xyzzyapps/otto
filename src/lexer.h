#ifndef OTTO_LEXER_H
#define OTTO_LEXER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "value.h"

typedef enum {
    TOK_EOF = 0,
    TOK_ERROR,

    // 8 Grammatical Classes
    // 1. Hooks
    TOK_HOOK_BEFORE,
    TOK_HOOK_EVERY,
    TOK_HOOK_AFTER,
    TOK_HOOK_ON,
    TOK_HOOK_EXTERN,
    TOK_HOOK_WATCH,

    // 2. Verbs
    TOK_VERB_READ,
    TOK_VERB_WRITE,
    TOK_VERB_PRINT,
    TOK_VERB_TAKE,
    TOK_VERB_DROP,
    TOK_VERB_SPLIT,
    TOK_VERB_JOIN,
    TOK_VERB_SORT,
    TOK_VERB_REPLACE,
    TOK_VERB_ADD,
    TOK_VERB_COUNT,
    TOK_VERB_RETURN,
    TOK_VERB_RUN,
    TOK_VERB_FETCH,
    TOK_VERB_COMPILE,
    TOK_VERB_UNDO,
    TOK_VERB_PICK,
    TOK_VERB_TABLE,
    TOK_VERB_CHART,
    TOK_VERB_UNPACK,
    TOK_VERB_INTERSECT,
    TOK_VERB_EXCLUDE,
    TOK_VERB_ZIP,
    TOK_VERB_REMOVE,
    TOK_VERB_MOVE,
    TOK_VERB_COPY,
    TOK_VERB_EXIT,
    TOK_VERB_KILL,
    TOK_VERB_FILTER,
    TOK_VERB_TALLY,
    TOK_VERB_GROUP,
    TOK_VERB_DIRS,
    TOK_VERB_BACK,
    TOK_VERB_CD,
    TOK_VERB_JUMP,
    TOK_VERB_BENCHMARK,
    TOK_VERB_JOBS,
    TOK_VERB_RESUME,
    TOK_VERB_REPEAT,

    // 3. Scanners
    TOK_SCAN_LINE,
    TOK_SCAN_LINES,
    TOK_SCAN_WORD,
    TOK_SCAN_WORDS,
    TOK_SCAN_NUMBER,
    TOK_SCAN_NUMBERS,
    TOK_SCAN_BYTE,
    TOK_SCAN_BYTES,
    TOK_SCAN_FILE,
    TOK_SCAN_FILES,
    TOK_SCAN_PROCESS,
    TOK_SCAN_PROCESSES,
    TOK_SCAN_ROW,
    TOK_SCAN_ROWS,
    TOK_SCAN_JSON,
    TOK_SCAN_CSV,
    TOK_SCAN_YAML,
    TOK_SCAN_URLS,
    TOK_SCAN_EMAILS,
    TOK_SCAN_HOST,
    TOK_SCAN_HOSTS,
    TOK_SCAN_TREE,

    // 4. Ordinals
    TOK_ORD_FIRST,
    TOK_ORD_SECOND,
    TOK_ORD_THIRD,
    TOK_ORD_FOURTH,
    TOK_ORD_NTH, // 1st, 2nd, 3rd, 4th, 5th, etc.
    TOK_ORD_LAST,
    TOK_ORD_TOP,
    TOK_ORD_EACH,

    // 5. Adverbs
    TOK_ADV_RECURSIVELY,
    TOK_ADV_NUMERICALLY,
    TOK_ADV_DESCENDING,
    TOK_ADV_UNIQUELY,
    TOK_ADV_INPLACE,
    TOK_ADV_CONCURRENTLY,
    TOK_ADV_CASEFOLD,
    TOK_ADV_SIMULATE,
    TOK_ADV_PARALLEL,
    TOK_ADV_BACKGROUND,
    TOK_ADV_IGNORING,

    // 6. Prepositions
    TOK_PREP_INTO,
    TOK_PREP_ONTO,
    TOK_PREP_FROM,
    TOK_PREP_WITH,
    TOK_PREP_BY,
    TOK_PREP_WHERE,
    TOK_PREP_MATCHING,
    TOK_PREP_AT,
    TOK_PREP_DISCARD,
    TOK_PREP_REPLACING,

    // 7. Conjunctions & Punctuation
    TOK_COMMA,     // , (pipe)
    TOK_AND,       // and (tee)
    TOK_OR,        // or (fallback)
    TOK_DOT,       // . (terminator)
    TOK_COLON,     // : (block header)

    // 8. Genitive
    TOK_GENITIVE,  // 's

    // Literals & Identifiers
    TOK_UNIT_LIT,  // 10mb, 4kb, 2s, 500ms, :8080
    TOK_NUMBER,    // 123, 3.14
    TOK_STRING,    // "hello"
    TOK_IDENT,     // variable or func name
    TOK_BOOL,      // true, false
    TOK_NIL,       // nil

    // Registers / Pronouns
    TOK_REG_IT,       // it -> ACC
    TOK_REG_THEM,     // them -> ACC / FLD collection
    TOK_REG_THAT,     // that -> previous sentence result
    TOK_REG_ACC,      // acc
    TOK_REG_CUR,      // cur / line / item / each
    TOK_REG_ITEM,     // item
    TOK_REG_ERROR,    // error -> ERR
    TOK_REG_PREVIOUS, // previous command ring

    // Control keywords
    TOK_IF,
    TOK_ELSE,
    TOK_UNLESS,

    // Operators & Brackets
    TOK_LPAREN,   // (
    TOK_RPAREN,   // )
    TOK_LBRACKET, // [
    TOK_RBRACKET, // ]
    TOK_LBRACE,   // {
    TOK_RBRACE,   // }
    TOK_EQ,       // ==
    TOK_NE,       // !=
    TOK_LT,       // <
    TOK_LE,       // <=
    TOK_GT,       // >
    TOK_GE,       // >=
    TOK_PLUS,     // +
    TOK_MINUS,    // -
    TOK_STAR,     // *
    TOK_SLASH,    // /
    TOK_PERCENT,  // %
    TOK_AMP       // & (for background job)
} OttoTokenType;

typedef struct {
    OttoTokenType type;
    Slice lexeme;
    int line;
    int col;
    union {
        int64_t i64;
        double f64;
        int nth_val; // for TOK_ORD_NTH: 1 for 1st, 2 for 2nd, etc.
    } lit;
} Token;

typedef struct Lexer {
    const char *source;
    size_t length;
    size_t cursor;
    int line;
    int col;
    bool interactive;
    const char *error_msg;
} Lexer;

void lexer_init(Lexer *lex, const char *source, size_t length, bool interactive);
Token lexer_next(Lexer *lex);
Token lexer_peek(Lexer *lex);
const char *token_type_name(OttoTokenType type);

// Classification checks
bool token_is_hook(OttoTokenType type);
bool token_is_verb(OttoTokenType type);
bool token_is_scanner(OttoTokenType type);
bool token_is_ordinal(OttoTokenType type);
bool token_is_adverb(OttoTokenType type);
bool token_is_prep(OttoTokenType type);
bool token_is_filler(const char *word, size_t len);

#endif // OTTO_LEXER_H
