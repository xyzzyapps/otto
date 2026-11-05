#ifndef OTTO_PARSER_H
#define OTTO_PARSER_H

#include "lexer.h"
#include "ast.h"

typedef struct FuncSignature {
    Slice name;
    int arity;
} FuncSignature;

typedef struct Parser {
    Lexer lexer;
    Token current;
    Token previous;
    Arena *arena;
    bool has_error;
    char error_msg[256];
    int error_line;
    int error_col;
    FuncSignature funcs[64];
    size_t func_count;
} Parser;

void parser_init(Parser *parser, const char *source, size_t length, bool interactive, Arena *arena);
ASTNode *parser_parse_program(Parser *parser);
ASTNode *parser_parse_pipe(Parser *parser);
ASTNode *parser_parse_expr(Parser *parser);

#endif // OTTO_PARSER_H
