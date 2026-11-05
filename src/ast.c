#include "ast.h"
#include <string.h>

ASTNode *ast_create(Arena *arena, ASTNodeType type, int line, int col) {
    ASTNode *node = (ASTNode *)arena_alloc_zero(arena, sizeof(ASTNode));
    node->type = type;
    node->line = line;
    node->col = col;
    return node;
}

void ast_pipe_add_clause(ASTNode *pipe, ASTNode *clause, ConjKind conj, Arena *arena) {
    if (!pipe || pipe->type != AST_PIPE || !clause) return;
    if (pipe->as.pipe.count >= pipe->as.pipe.capacity) {
        size_t new_cap = pipe->as.pipe.capacity == 0 ? 4 : pipe->as.pipe.capacity * 2;
        ASTNode **new_clauses = (ASTNode **)arena_alloc(arena, new_cap * sizeof(ASTNode *));
        ConjKind *new_conjs = (ConjKind *)arena_alloc(arena, new_cap * sizeof(ConjKind));
        if (pipe->as.pipe.count > 0) {
            memcpy(new_clauses, pipe->as.pipe.clauses, pipe->as.pipe.count * sizeof(ASTNode *));
            memcpy(new_conjs, pipe->as.pipe.conjs, pipe->as.pipe.count * sizeof(ConjKind));
        }
        pipe->as.pipe.clauses = new_clauses;
        pipe->as.pipe.conjs = new_conjs;
        pipe->as.pipe.capacity = new_cap;
    }
    pipe->as.pipe.clauses[pipe->as.pipe.count] = clause;
    pipe->as.pipe.conjs[pipe->as.pipe.count] = conj;
    pipe->as.pipe.count++;
}

void ast_program_add_item(ASTNode *prog, ASTNode *item, Arena *arena) {
    if (!prog || prog->type != AST_PROGRAM || !item) return;
    if (prog->as.program.count >= prog->as.program.capacity) {
        size_t new_cap = prog->as.program.capacity == 0 ? 8 : prog->as.program.capacity * 2;
        ASTNode **new_items = (ASTNode **)arena_alloc(arena, new_cap * sizeof(ASTNode *));
        if (prog->as.program.count > 0) {
            memcpy(new_items, prog->as.program.items, prog->as.program.count * sizeof(ASTNode *));
        }
        prog->as.program.items = new_items;
        prog->as.program.capacity = new_cap;
    }
    prog->as.program.items[prog->as.program.count++] = item;
}
