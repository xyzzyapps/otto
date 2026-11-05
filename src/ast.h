#ifndef OTTO_AST_H
#define OTTO_AST_H

#include "lexer.h"
#include "value.h"

// AST Node Types
typedef enum {
    AST_PROGRAM,
    AST_BLOCK,
    AST_PIPE,
    AST_CLAUSE_CMD,
    AST_CLAUSE_RETURN,

    // Expressions
    AST_EXPR_LITERAL,
    AST_EXPR_REGISTER,
    AST_EXPR_VAR,
    AST_EXPR_CALL,
    AST_EXPR_UNARY,
    AST_EXPR_BINARY,
    AST_EXPR_GENITIVE, // a's b
    AST_EXPR_MAP,      // { k: v, ... }
    AST_EXPR_LIST      // [ a, b, ... ]
} ASTNodeType;

// Preposition types
typedef enum {
    PREP_NONE = 0,
    PREP_INTO = (1 << 0),
    PREP_ONTO = (1 << 1),
    PREP_FROM = (1 << 2),
    PREP_WITH = (1 << 3),
    PREP_BY = (1 << 4),
    PREP_WHERE = (1 << 5),
    PREP_MATCHING = (1 << 6),
    PREP_AT = (1 << 7),
    PREP_DISCARD = (1 << 9),
    PREP_REPLACING = (1 << 11)
} PrepBit;

// Adverb flags
typedef enum {
    ADV_NONE = 0,
    ADV_RECURSIVELY = (1 << 0),
    ADV_NUMERICALLY = (1 << 1),
    ADV_DESCENDING = (1 << 2),
    ADV_UNIQUELY = (1 << 3),
    ADV_INPLACE = (1 << 4),
    ADV_CONCURRENTLY = (1 << 5),
    ADV_CASEFOLD = (1 << 6),
    ADV_SIMULATE = (1 << 7),
    ADV_PARALLEL = (1 << 8),
    ADV_BACKGROUND = (1 << 9),
    ADV_IGNORING_GIT = (1 << 10)
} AdverbBit;

// Hook kinds
typedef enum {
    HOOK_NONE,
    HOOK_BEFORE,
    HOOK_AFTER,
    HOOK_ON,
    HOOK_EVERY,
    HOOK_FROM_BUILD,  // "target" from "src"
    HOOK_EXTERN,
    HOOK_FUNC_DEF,
    HOOK_WATCH
} HookKind;

// Pipe conjunction between clauses
typedef enum {
    CONJ_NONE,
    CONJ_PIPE, // ,
    CONJ_AND,  // and
    CONJ_OR    // or
} ConjKind;

struct ASTNode;

typedef struct ASTExprMapItem {
    Slice key;
    struct ASTNode *val;
} ASTExprMapItem;

typedef struct ASTPrepSlot {
    OttoTokenType prep;
    struct ASTNode *expr;
} ASTPrepSlot;

typedef struct ASTNode {
    ASTNodeType type;
    int line;
    int col;

    union {
        // AST_PROGRAM
        struct {
            struct ASTNode **items;
            size_t count;
            size_t capacity;
        } program;

        // AST_BLOCK
        struct {
            HookKind kind;
            Slice hook_name;      // func name, or on event
            Slice src_name;       // for "target" from "src"
            Slice extern_lib;     // for extern
            Slice extern_symbol;
            OttoTokenType scanner;    // for every <scanner>
            int quantifier;       // for every N scanner
            uint16_t adverbs;
            ASTPrepSlot preps[8];
            size_t prep_count;
            struct ASTNode *body; // AST_PIPE
            Slice *params;        // for function def params
            size_t param_count;
        } block;

        // AST_PIPE
        struct {
            struct ASTNode **clauses;
            ConjKind *conjs;
            size_t count;
            size_t capacity;
        } pipe;

        // AST_CLAUSE_CMD
        struct {
            OttoTokenType verb;
            Slice raw_cmd;       // for external or passthrough command
            uint16_t adverbs;
            OttoTokenType ordinal;   // first, second, nth, last, etc.
            int nth_val;
            OttoTokenType scanner;   // line, word, number, byte, etc.
            struct ASTNode *direct_arg;
            ASTPrepSlot preps[8];
            size_t prep_count;
            struct ASTNode *cond; // if / unless
            bool unless_cond;
        } cmd;

        // AST_CLAUSE_RETURN
        struct {
            struct ASTNode *expr;
            struct ASTNode *cond;
            struct ASTNode *else_expr;
        } ret;

        // AST_EXPR_LITERAL
        struct {
            Value val;
        } lit;

        // AST_EXPR_REGISTER
        struct {
            OttoTokenType reg; // it, each, first, last, previous, error, etc.
            int nth_val;
        } reg;

        // AST_EXPR_VAR
        struct {
            Slice name;
        } var;

        // AST_EXPR_CALL
        struct {
            Slice func_name;
            struct ASTNode **args;
            size_t arg_count;
        } call;

        // AST_EXPR_UNARY
        struct {
            OttoTokenType op;
            struct ASTNode *operand;
        } unary;

        // AST_EXPR_BINARY
        struct {
            OttoTokenType op;
            struct ASTNode *left;
            struct ASTNode *right;
        } binary;

        // AST_EXPR_GENITIVE (a's b)
        struct {
            struct ASTNode *left;
            Slice prop;
        } genitive;

        // AST_EXPR_MAP
        struct {
            ASTExprMapItem *items;
            size_t count;
        } map;

        // AST_EXPR_LIST
        struct {
            struct ASTNode **items;
            size_t count;
        } list;
    } as;
} ASTNode;

ASTNode *ast_create(Arena *arena, ASTNodeType type, int line, int col);
void ast_pipe_add_clause(ASTNode *pipe, ASTNode *clause, ConjKind conj, Arena *arena);
void ast_program_add_item(ASTNode *prog, ASTNode *item, Arena *arena);

#endif // OTTO_AST_H
