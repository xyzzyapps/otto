#include "parser.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void parser_error(Parser *p, const char *msg) {
    if (p->has_error) return;
    p->has_error = true;
    p->error_line = p->current.line;
    p->error_col = p->current.col;
    snprintf(p->error_msg, sizeof(p->error_msg), "%s at line %d, col %d (near '%.*s')",
             msg, p->current.line, p->current.col, (int)p->current.lexeme.len, p->current.lexeme.ptr);
}

static Token advance(Parser *p) {
    p->previous = p->current;
    p->current = lexer_next(&p->lexer);
    if (p->current.type == TOK_ERROR) {
        parser_error(p, p->lexer.error_msg ? p->lexer.error_msg : "Lexical error");
    }
    return p->previous;
}

static bool check(Parser *p, OttoTokenType type) {
    return p->current.type == type;
}

static bool match(Parser *p, OttoTokenType type) {
    if (check(p, type)) {
        advance(p);
        return true;
    }
    return false;
}

static Token consume(Parser *p, OttoTokenType type, const char *msg) {
    if (check(p, type)) {
        return advance(p);
    }
    parser_error(p, msg);
    return p->current;
}

// Forward declarations
static ASTNode *parse_pipe(Parser *p);
static ASTNode *parse_expr(Parser *p);
static ASTNode *parse_clause(Parser *p);
static ASTNode *parse_postfix(Parser *p);

static uint16_t parse_adverbs(Parser *p) {
    uint16_t advs = 0;
    while (token_is_adverb(p->current.type)) {
        switch (p->current.type) {
            case TOK_ADV_RECURSIVELY: advs |= ADV_RECURSIVELY; break;
            case TOK_ADV_NUMERICALLY: advs |= ADV_NUMERICALLY; break;
            case TOK_ADV_DESCENDING: advs |= ADV_DESCENDING; break;
            case TOK_ADV_UNIQUELY: advs |= ADV_UNIQUELY; break;
            case TOK_ADV_INPLACE: advs |= ADV_INPLACE; break;
            case TOK_ADV_CONCURRENTLY: advs |= ADV_CONCURRENTLY; break;
            case TOK_ADV_CASEFOLD: advs |= ADV_CASEFOLD; break;
            case TOK_ADV_SIMULATE: advs |= ADV_SIMULATE; break;
            case TOK_ADV_PARALLEL: advs |= ADV_PARALLEL; break;
            case TOK_ADV_BACKGROUND: advs |= ADV_BACKGROUND; break;
            case TOK_ADV_IGNORING:
                advs |= ADV_IGNORING_GIT;
                advance(p);
                if (check(p, TOK_IDENT) && slice_eq_str(p->current.lexeme, "git")) {
                    advance(p);
                }
                continue;
            default: break;
        }
        advance(p);
    }
    return advs;
}

// Parse primary expression
static ASTNode *parse_primary(Parser *p) {
    int line = p->current.line;
    int col = p->current.col;

    // Registers
    if (p->current.type >= TOK_REG_IT && p->current.type <= TOK_REG_PREVIOUS) {
        Token tok = advance(p);
        ASTNode *node = ast_create(p->arena, AST_EXPR_REGISTER, line, col);
        node->as.reg.reg = tok.type;
        return node;
    }

    // Ordinals used as registers (first, second, nth, last, each)
    if (token_is_ordinal(p->current.type)) {
        Token tok = advance(p);
        ASTNode *node = ast_create(p->arena, AST_EXPR_REGISTER, line, col);
        node->as.reg.reg = tok.type;
        node->as.reg.nth_val = (tok.type == TOK_ORD_NTH) ? tok.lit.nth_val : 0;
        return node;
    }

    // Literals
    if (match(p, TOK_NUMBER)) {
        ASTNode *node = ast_create(p->arena, AST_EXPR_LITERAL, line, col);
        node->as.lit.val = p->previous.lit.i64 ? val_int(p->previous.lit.i64) : val_float(p->previous.lit.f64);
        return node;
    }
    if (match(p, TOK_STRING)) {
        ASTNode *node = ast_create(p->arena, AST_EXPR_LITERAL, line, col);
        node->as.lit.val = val_slice(p->previous.lexeme.ptr, p->previous.lexeme.len);
        return node;
    }
    if (match(p, TOK_UNIT_LIT)) {
        ASTNode *node = ast_create(p->arena, AST_EXPR_LITERAL, line, col);
        node->as.lit.val = val_slice(p->previous.lexeme.ptr, p->previous.lexeme.len);
        return node;
    }
    if (match(p, TOK_BOOL)) {
        ASTNode *node = ast_create(p->arena, AST_EXPR_LITERAL, line, col);
        node->as.lit.val = val_bool(p->previous.lit.i64 != 0);
        return node;
    }
    if (match(p, TOK_NIL)) {
        ASTNode *node = ast_create(p->arena, AST_EXPR_LITERAL, line, col);
        node->as.lit.val = val_nil();
        return node;
    }

    // Map literal: { key: expr, ... }
    if (match(p, TOK_LBRACE)) {
        ASTNode *node = ast_create(p->arena, AST_EXPR_MAP, line, col);
        size_t cap = 4;
        ASTExprMapItem *items = (ASTExprMapItem *)arena_alloc(p->arena, cap * sizeof(ASTExprMapItem));
        size_t count = 0;
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            Slice key;
            if (p->current.lexeme.len > 0 && p->current.type != TOK_RBRACE && p->current.type != TOK_COLON) {
                Token ktok = advance(p);
                key = ktok.lexeme;
            } else {
                parser_error(p, "Expected key in map literal");
                break;
            }
            consume(p, TOK_COLON, "Expected ':' after map key");
            ASTNode *val_node = parse_expr(p);
            if (count >= cap) {
                cap *= 2;
                ASTExprMapItem *new_items = (ASTExprMapItem *)arena_alloc(p->arena, cap * sizeof(ASTExprMapItem));
                memcpy(new_items, items, count * sizeof(ASTExprMapItem));
                items = new_items;
            }
            items[count].key = key;
            items[count].val = val_node;
            count++;
            if (!match(p, TOK_COMMA)) break;
        }
        consume(p, TOK_RBRACE, "Expected '}' after map");
        node->as.map.items = items;
        node->as.map.count = count;
        return node;
    }

    // Bracket sub-expression or thunk: [ ... ]
    if (match(p, TOK_LBRACKET)) {
        ASTNode *body = parse_pipe(p);
        consume(p, TOK_RBRACKET, "Expected ']'");
        ASTNode *node = ast_create(p->arena, AST_EXPR_THUNK, line, col);
        node->as.thunk.body = body;
        return node;
    }

    // Parenthesized expression: ( expr )
    if (match(p, TOK_LPAREN)) {
        ASTNode *node = parse_expr(p);
        consume(p, TOK_RPAREN, "Expected ')'");
        return node;
    }

    // Variable or Function call
    if (match(p, TOK_IDENT)) {
        Slice name = p->previous.lexeme;
        int arity = -1;
        for (size_t i = 0; i < p->func_count; i++) {
            if (slice_eq(p->funcs[i].name, name)) {
                arity = p->funcs[i].arity;
                break;
            }
        }

        if (arity > 0) {
            ASTNode **args = (ASTNode **)arena_alloc(p->arena, arity * sizeof(ASTNode *));
            for (int i = 0; i < arity; i++) {
                args[i] = parse_postfix(p);
            }
            ASTNode *node = ast_create(p->arena, AST_EXPR_CALL, line, col);
            node->as.call.func_name = name;
            node->as.call.args = args;
            node->as.call.arg_count = (size_t)arity;
            return node;
        }

        ASTNode *node = ast_create(p->arena, AST_EXPR_VAR, line, col);
        node->as.var.name = name;
        return node;
    }

    parser_error(p, "Unexpected token in expression");
    return NULL;
}

// Parse postfix genitive: expr 's ident
static ASTNode *parse_postfix(Parser *p) {
    ASTNode *expr = parse_primary(p);
    if (!expr) return NULL;

    while (match(p, TOK_GENITIVE)) {
        Token prop_tok = advance(p);
        if (prop_tok.type != TOK_IDENT && !token_is_ordinal(prop_tok.type) &&
            !token_is_scanner(prop_tok.type)) {
            parser_error(p, "Expected property name after 's");
            return expr;
        }
        ASTNode *gen_node = ast_create(p->arena, AST_EXPR_GENITIVE, prop_tok.line, prop_tok.col);
        gen_node->as.genitive.left = expr;
        gen_node->as.genitive.prop = prop_tok.lexeme;
        expr = gen_node;
    }
    return expr;
}

// Parse binary expressions with operator precedence
static ASTNode *parse_multiplicative(Parser *p) {
    ASTNode *left = parse_postfix(p);
    while (check(p, TOK_STAR) || check(p, TOK_SLASH) || check(p, TOK_PERCENT)) {
        Token op = advance(p);
        ASTNode *right = parse_postfix(p);
        ASTNode *bin = ast_create(p->arena, AST_EXPR_BINARY, op.line, op.col);
        bin->as.binary.op = op.type;
        bin->as.binary.left = left;
        bin->as.binary.right = right;
        left = bin;
    }
    return left;
}

static ASTNode *parse_additive(Parser *p) {
    ASTNode *left = parse_multiplicative(p);
    while (check(p, TOK_PLUS) || check(p, TOK_MINUS)) {
        Token op = advance(p);
        ASTNode *right = parse_multiplicative(p);
        ASTNode *bin = ast_create(p->arena, AST_EXPR_BINARY, op.line, op.col);
        bin->as.binary.op = op.type;
        bin->as.binary.left = left;
        bin->as.binary.right = right;
        left = bin;
    }
    return left;
}

static ASTNode *parse_comparison(Parser *p) {
    ASTNode *left = parse_additive(p);
    while (check(p, TOK_EQ) || check(p, TOK_NE) || check(p, TOK_LT) ||
           check(p, TOK_LE) || check(p, TOK_GT) || check(p, TOK_GE)) {
        Token op = advance(p);
        ASTNode *right = parse_additive(p);
        ASTNode *bin = ast_create(p->arena, AST_EXPR_BINARY, op.line, op.col);
        bin->as.binary.op = op.type;
        bin->as.binary.left = left;
        bin->as.binary.right = right;
        left = bin;
    }
    return left;
}

static ASTNode *parse_expr(Parser *p) {
    return parse_comparison(p);
}

// Parse return statement: return Expr ("if" Expr ("else" Expr)?)?
static ASTNode *parse_return_stmt(Parser *p) {
    int line = p->current.line;
    int col = p->current.col;
    consume(p, TOK_VERB_RETURN, "Expected 'return'");

    ASTNode *expr = parse_expr(p);
    ASTNode *cond = NULL;
    ASTNode *else_expr = NULL;

    if (match(p, TOK_IF)) {
        cond = parse_expr(p);
        if (match(p, TOK_ELSE)) {
            else_expr = parse_expr(p);
        }
    }

    ASTNode *node = ast_create(p->arena, AST_CLAUSE_RETURN, line, col);
    node->as.ret.expr = expr;
    node->as.ret.cond = cond;
    node->as.ret.else_expr = else_expr;
    return node;
}

// Parse command clause: Adverb* Verb DirectArg? PrepSlot* Condition?
static ASTNode *parse_command(Parser *p) {
    int line = p->current.line;
    int col = p->current.col;

    uint16_t adverbs = parse_adverbs(p);

    // If starting with a literal (e.g. `0 into s.`), treat as implicit ACC load
    if (check(p, TOK_NUMBER) || check(p, TOK_STRING) || check(p, TOK_LBRACE)) {
        ASTNode *lit_expr = parse_expr(p);
        ASTNode *node = ast_create(p->arena, AST_CLAUSE_CMD, line, col);
        node->as.cmd.verb = TOK_VERB_READ; // or implicit load
        node->as.cmd.adverbs = adverbs;
        node->as.cmd.direct_arg = lit_expr;

        // Parse preposition slots
        while (token_is_prep(p->current.type)) {
            Token prep_tok = advance(p);
            ASTNode *prep_expr = parse_expr(p);
            if (node->as.cmd.prep_count < 8) {
                node->as.cmd.preps[node->as.cmd.prep_count].prep = prep_tok.type;
                node->as.cmd.preps[node->as.cmd.prep_count].expr = prep_expr;
                node->as.cmd.prep_count++;
            }
        }
        return node;
    }

    OttoTokenType verb = TOK_VERB_RUN;
    Slice raw_cmd = {NULL, 0};

    if (token_is_verb(p->current.type)) {
        Token vtok = advance(p);
        verb = vtok.type;
    } else if (check(p, TOK_IDENT)) {
        Token vtok = advance(p);
        bool is_func = false;
        for (size_t f = 0; f < p->func_count; f++) {
            if (slice_eq(p->funcs[f].name, vtok.lexeme)) {
                is_func = true;
                break;
            }
        }

        if (is_func) {
            verb = TOK_VERB_RUN;
            raw_cmd = vtok.lexeme;
        } else {
            verb = TOK_VERB_RUN;
            const char *cmd_start = vtok.lexeme.ptr;
            const char *cmd_end = vtok.lexeme.ptr + vtok.lexeme.len;

            while (!check(p, TOK_COMMA) && !check(p, TOK_AND) && !check(p, TOK_OR) &&
                   !check(p, TOK_DOT) && !check(p, TOK_EOF)) {
                Token arg_tok = advance(p);
                cmd_end = arg_tok.lexeme.ptr + arg_tok.lexeme.len;
            }
            raw_cmd = (Slice){cmd_start, (size_t)(cmd_end - cmd_start)};
        }
    } else {
        parser_error(p, "Expected verb or command");
        return NULL;
    }

    ASTNode *node = ast_create(p->arena, AST_CLAUSE_CMD, line, col);
    node->as.cmd.verb = verb;
    node->as.cmd.raw_cmd = raw_cmd;
    node->as.cmd.adverbs = adverbs;

    if (verb == TOK_VERB_BENCHMARK) {
        if (check(p, TOK_NUMBER)) {
            Token ntok = advance(p);
            node->as.cmd.nth_val = (int)ntok.lit.i64;
        } else {
            node->as.cmd.nth_val = 10;
        }
        if (check(p, TOK_IDENT) && (slice_eq_str(p->current.lexeme, "runs") || slice_eq_str(p->current.lexeme, "times"))) {
            advance(p);
        }
    }

    // DirectArg: Ordinal? Scanner? Expr?
    if (token_is_ordinal(p->current.type) && lexer_peek(&p->lexer).type != TOK_GENITIVE) {
        Token otok = advance(p);
        node->as.cmd.ordinal = otok.type;
        node->as.cmd.nth_val = (otok.type == TOK_ORD_NTH) ? otok.lit.nth_val : 0;
    }

    // Quantifier before scanner (e.g. "8 bytes")
    if (check(p, TOK_NUMBER)) {
        Token ntok = p->current;
        size_t save_cur = p->lexer.cursor;
        int save_l = p->lexer.line, save_c = p->lexer.col;
        advance(p);
        if (token_is_scanner(p->current.type)) {
            Token stok = advance(p);
            node->as.cmd.scanner = stok.type;
            node->as.cmd.nth_val = (int)ntok.lit.i64;
        } else {
            p->lexer.cursor = save_cur;
            p->lexer.line = save_l;
            p->lexer.col = save_c;
            p->current = ntok;
        }
    }

    if (token_is_scanner(p->current.type)) {
        Token stok = advance(p);
        node->as.cmd.scanner = stok.type;
    }

    if (check(p, TOK_VERB_COUNT)) {
        advance(p);
        node->as.cmd.adverbs |= (1 << 13);
        if (token_is_scanner(p->current.type)) {
            Token stok = advance(p);
            node->as.cmd.scanner = stok.type;
        }
    }

    // Check for direct arg expression (unless it's a preposition or conjunction or condition)
    if (!token_is_prep(p->current.type) && !token_is_adverb(p->current.type) &&
        p->current.type != TOK_COMMA && p->current.type != TOK_AND &&
        p->current.type != TOK_OR && p->current.type != TOK_DOT &&
        p->current.type != TOK_COLON && p->current.type != TOK_IF &&
        p->current.type != TOK_UNLESS && p->current.type != TOK_EOF &&
        p->current.type != TOK_RPAREN && p->current.type != TOK_RBRACKET) {
        node->as.cmd.direct_arg = parse_expr(p);
    }

    // Secondary scanner check: e.g. `take first 8 bytes` or `read "a.json" json`
    if (node->as.cmd.scanner == 0 && token_is_scanner(p->current.type)) {
        Token stok = advance(p);
        node->as.cmd.scanner = stok.type;
    }

    // Preposition slots: PrepToken Expr (or multiple variables for `unpack ... into a b c`)
    while (token_is_prep(p->current.type)) {
        Token prep_tok = advance(p);
        ASTNode *prep_expr = NULL;

        // If 'discard' is followed directly by nothing or next prep
        if (prep_tok.type == TOK_PREP_DISCARD) {
            prep_expr = NULL;
        } else if (prep_tok.type == TOK_PREP_INTO && verb == TOK_VERB_UNPACK) {
            // Unpack into multiple identifiers: into magic bits endian
            size_t cap = 4;
            ASTNode **items = (ASTNode **)arena_alloc(p->arena, cap * sizeof(ASTNode *));
            size_t count = 0;
            while (check(p, TOK_IDENT)) {
                Token var_tok = advance(p);
                ASTNode *var_node = ast_create(p->arena, AST_EXPR_VAR, var_tok.line, var_tok.col);
                var_node->as.var.name = var_tok.lexeme;
                if (count >= cap) {
                    cap *= 2;
                    ASTNode **new_items = (ASTNode **)arena_alloc(p->arena, cap * sizeof(ASTNode *));
                    memcpy(new_items, items, count * sizeof(ASTNode *));
                    items = new_items;
                }
                items[count++] = var_node;
            }
            ASTNode *list_node = ast_create(p->arena, AST_EXPR_LIST, prep_tok.line, prep_tok.col);
            list_node->as.list.items = items;
            list_node->as.list.count = count;
            prep_expr = list_node;
        } else {
            prep_expr = parse_expr(p);
            // Check for additional adverbs inside prep slot, e.g. `sort by age descending`
            if (token_is_adverb(p->current.type)) {
                node->as.cmd.adverbs |= parse_adverbs(p);
            }
        }

        if (node->as.cmd.prep_count < 8) {
            node->as.cmd.preps[node->as.cmd.prep_count].prep = prep_tok.type;
            node->as.cmd.preps[node->as.cmd.prep_count].expr = prep_expr;
            node->as.cmd.prep_count++;
        }
    }

    // Zero-preposition syntax support (let user pick positional or prepositional)
    if (node->as.cmd.prep_count == 0 && node->as.cmd.direct_arg) {
        if (verb == TOK_VERB_SORT || verb == TOK_VERB_GROUP) {
            node->as.cmd.preps[0].prep = TOK_PREP_BY;
            node->as.cmd.preps[0].expr = node->as.cmd.direct_arg;
            node->as.cmd.prep_count = 1;
            node->as.cmd.direct_arg = NULL;
            if (token_is_adverb(p->current.type)) {
                node->as.cmd.adverbs |= parse_adverbs(p);
            }
        } else if (verb == TOK_VERB_SPLIT) {
            node->as.cmd.preps[0].prep = TOK_PREP_BY;
            node->as.cmd.preps[0].expr = node->as.cmd.direct_arg;
            node->as.cmd.prep_count = 1;
        }
    }

    // Zero-preposition secondary positional argument check
    if (node->as.cmd.prep_count == 0 && !token_is_prep(p->current.type) && !token_is_adverb(p->current.type) &&
        p->current.type != TOK_COMMA && p->current.type != TOK_AND &&
        p->current.type != TOK_OR && p->current.type != TOK_DOT &&
        p->current.type != TOK_COLON && p->current.type != TOK_IF &&
        p->current.type != TOK_UNLESS && p->current.type != TOK_EOF &&
        p->current.type != TOK_RPAREN && p->current.type != TOK_RBRACKET) {

        OttoTokenType implicit_prep = 0;
        if (verb == TOK_VERB_REPLACE) implicit_prep = TOK_PREP_WITH;
        else if (verb == TOK_VERB_COMPILE) implicit_prep = TOK_PREP_INTO;
        else if (verb == TOK_VERB_WRITE) implicit_prep = TOK_PREP_ONTO;
        else if (verb == TOK_VERB_EXCLUDE) implicit_prep = TOK_PREP_FROM;
        else if (verb == TOK_VERB_ADD) implicit_prep = TOK_PREP_INTO;
        else if (verb == TOK_VERB_COPY || verb == TOK_VERB_MOVE) implicit_prep = TOK_PREP_INTO;

        if (implicit_prep != 0) {
            ASTNode *second_arg = parse_expr(p);
            if (token_is_adverb(p->current.type)) {
                node->as.cmd.adverbs |= parse_adverbs(p);
            }
            node->as.cmd.preps[node->as.cmd.prep_count].prep = implicit_prep;
            node->as.cmd.preps[node->as.cmd.prep_count].expr = second_arg;
            node->as.cmd.prep_count++;
        }
    }

    // Condition: if / unless
    if (match(p, TOK_IF)) {
        node->as.cmd.cond = parse_expr(p);
        node->as.cmd.unless_cond = false;
    } else if (match(p, TOK_UNLESS)) {
        node->as.cmd.cond = parse_expr(p);
        node->as.cmd.unless_cond = true;
    }

    return node;
}

static ASTNode *parse_clause(Parser *p) {
    if (check(p, TOK_VERB_RETURN)) {
        return parse_return_stmt(p);
    }
    return parse_command(p);
}

// Pipe <- Clause ( ("," / "and" / "or") Clause )* "."?
static ASTNode *parse_pipe(Parser *p) {
    int line = p->current.line;
    int col = p->current.col;

    ASTNode *first_clause = parse_clause(p);
    if (!first_clause) return NULL;

    ASTNode *pipe = ast_create(p->arena, AST_PIPE, line, col);
    ast_pipe_add_clause(pipe, first_clause, CONJ_NONE, p->arena);

    while (check(p, TOK_COMMA) || check(p, TOK_AND) || check(p, TOK_OR)) {
        ConjKind conj = CONJ_PIPE;
        if (match(p, TOK_COMMA)) conj = CONJ_PIPE;
        else if (match(p, TOK_AND)) conj = CONJ_AND;
        else if (match(p, TOK_OR)) conj = CONJ_OR;

        // Skip optional dot or whitespace
        if (check(p, TOK_DOT) || check(p, TOK_EOF)) break;

        ASTNode *next_clause = parse_clause(p);
        if (!next_clause) break;
        ast_pipe_add_clause(pipe, next_clause, conj, p->arena);
    }

    // Optional terminating dot
    match(p, TOK_DOT);

    return pipe;
}

// Block parsing
static ASTNode *parse_block_or_pipe(Parser *p) {
    int line = p->current.line;
    int col = p->current.col;

    // Check if Hook block
    // 1. "before:"
    if (match(p, TOK_HOOK_BEFORE)) {
        consume(p, TOK_COLON, "Expected ':' after 'before'");
        ASTNode *body = parse_pipe(p);
        ASTNode *node = ast_create(p->arena, AST_BLOCK, line, col);
        node->as.block.kind = HOOK_BEFORE;
        node->as.block.body = body;
        return node;
    }

    // 2. "after:"
    if (match(p, TOK_HOOK_AFTER)) {
        consume(p, TOK_COLON, "Expected ':' after 'after'");
        ASTNode *body = parse_pipe(p);
        ASTNode *node = ast_create(p->arena, AST_BLOCK, line, col);
        node->as.block.kind = HOOK_AFTER;
        node->as.block.body = body;
        return node;
    }

    // 3. "on interrupt:" / "on failure:" / "on exit:"
    if (match(p, TOK_HOOK_ON)) {
        Token event_tok = advance(p);
        consume(p, TOK_COLON, "Expected ':' after 'on <event>'");
        ASTNode *body = parse_pipe(p);
        ASTNode *node = ast_create(p->arena, AST_BLOCK, line, col);
        node->as.block.kind = HOOK_ON;
        node->as.block.hook_name = event_tok.lexeme;
        node->as.block.body = body;
        return node;
    }

    // 3b. "watch" [Scanner?] [Pattern?]: Pipe .
    if (match(p, TOK_HOOK_WATCH)) {
        OttoTokenType scanner = TOK_SCAN_FILES;
        if (token_is_scanner(p->current.type)) {
            Token stok = advance(p);
            scanner = stok.type;
        }
        Slice pat = (Slice){"*", 1};
        if (!check(p, TOK_COLON) && !check(p, TOK_EOF)) {
            if (check(p, TOK_STRING) || check(p, TOK_IDENT)) {
                Token ptok = advance(p);
                pat = ptok.lexeme;
            }
        }
        consume(p, TOK_COLON, "Expected ':' after 'watch' header");
        ASTNode *body = parse_pipe(p);
        ASTNode *node = ast_create(p->arena, AST_BLOCK, line, col);
        node->as.block.kind = HOOK_WATCH;
        node->as.block.scanner = scanner;
        node->as.block.src_name = pat;
        node->as.block.body = body;
        return node;
    }

    // 4. "every" [Quantifier?] Adverb* Scanner [Expr?] [PrepSlots*]: Pipe .
    if (match(p, TOK_HOOK_EVERY)) {
        int quantifier = 0;
        if (check(p, TOK_NUMBER)) {
            Token qtok = advance(p);
            quantifier = (int)qtok.lit.i64;
        } else if (check(p, TOK_UNIT_LIT)) {
            // e.g. every 10s: or every 500ms:
            Token utok = advance(p);
            consume(p, TOK_COLON, "Expected ':' after interval");
            ASTNode *body = parse_pipe(p);
            ASTNode *node = ast_create(p->arena, AST_BLOCK, line, col);
            node->as.block.kind = HOOK_EVERY;
            node->as.block.hook_name = utok.lexeme; // interval
            node->as.block.body = body;
            return node;
        }

        uint16_t advs = parse_adverbs(p);
        OttoTokenType scanner = 0;
        if (token_is_scanner(p->current.type)) {
            Token stok = advance(p);
            scanner = stok.type;
        }

        ASTNode *src_expr = NULL;
        // Direct arg (like "a.log" or "*.txt" or :8080)
        if (!token_is_prep(p->current.type) && !check(p, TOK_COLON) && !check(p, TOK_EOF)) {
            src_expr = parse_expr(p);
        }

        ASTNode *node = ast_create(p->arena, AST_BLOCK, line, col);
        node->as.block.kind = HOOK_EVERY;
        node->as.block.quantifier = quantifier;
        node->as.block.adverbs = advs;
        node->as.block.scanner = scanner;
        if (src_expr) {
            if (src_expr->type == AST_EXPR_LITERAL && src_expr->as.lit.val.type == VAL_SLICE) {
                node->as.block.src_name = src_expr->as.lit.val.as.slice;
            } else if (src_expr->type == AST_EXPR_VAR) {
                node->as.block.src_name = src_expr->as.var.name;
            }
        }

        // Prep slots before colon (e.g. matching "500", where cpu >= 80)
        while (token_is_prep(p->current.type)) {
            Token prep_tok = advance(p);
            ASTNode *pexpr = parse_expr(p);
            if (node->as.block.prep_count < 8) {
                node->as.block.preps[node->as.block.prep_count].prep = prep_tok.type;
                node->as.block.preps[node->as.block.prep_count].expr = pexpr;
                node->as.block.prep_count++;
            }
        }

        consume(p, TOK_COLON, "Expected ':' after 'every' clause header");
        ASTNode *body = parse_pipe(p);
        node->as.block.body = body;
        return node;
    }

    // 5. Build rule: (String / Ident) "from" (String / Ident) ":" Pipe .
    if (check(p, TOK_STRING) || check(p, TOK_IDENT)) {
        size_t save_cursor = p->lexer.cursor;
        int save_line = p->lexer.line;
        int save_col = p->lexer.col;
        Token save_curr = p->current;
        Token save_prev = p->previous;

        Token s1 = advance(p);
        if (match(p, TOK_PREP_FROM)) {
            if (check(p, TOK_STRING) || check(p, TOK_IDENT)) {
                Token s2 = advance(p);
                if (match(p, TOK_COLON)) {
                    ASTNode *body = parse_pipe(p);
                    ASTNode *node = ast_create(p->arena, AST_BLOCK, line, col);
                    node->as.block.kind = HOOK_FROM_BUILD;
                    node->as.block.hook_name = s1.lexeme; // target
                    node->as.block.src_name = s2.lexeme;  // source
                    node->as.block.body = body;
                    return node;
                }
            }
        }
        // Not a build hook; rewind
        p->lexer.cursor = save_cursor;
        p->lexer.line = save_line;
        p->lexer.col = save_col;
        p->current = save_curr;
        p->previous = save_prev;
    }

    // 6. "extern" String Ident "(" TypeList ")" "into" Ident
    if (match(p, TOK_HOOK_EXTERN)) {
        Token lib_tok = consume(p, TOK_STRING, "Expected library path for extern");
        Token sym_tok = consume(p, TOK_IDENT, "Expected symbol name for extern");
        consume(p, TOK_LPAREN, "Expected '('");
        while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
            advance(p); // skip type names for now
            match(p, TOK_COMMA);
        }
        consume(p, TOK_RPAREN, "Expected ')'");
        consume(p, TOK_PREP_INTO, "Expected 'into' after extern declaration");
        Token fn_name = consume(p, TOK_IDENT, "Expected verb name for extern");
        match(p, TOK_DOT);

        ASTNode *node = ast_create(p->arena, AST_BLOCK, line, col);
        node->as.block.kind = HOOK_EXTERN;
        node->as.block.extern_lib = lib_tok.lexeme;
        node->as.block.extern_symbol = sym_tok.lexeme;
        node->as.block.hook_name = fn_name.lexeme;
        return node;
    }

    // 7. Function Definition: Ident Ident* ":" Pipe .
    // e.g. "gcd a b: return a if b == 0 else gcd b (a % b)."
    if (check(p, TOK_IDENT)) {
        // Check if followed by parameters and then a colon ':'
        // We can inspect lexer tokens
        size_t save_cursor = p->lexer.cursor;
        int save_line = p->lexer.line;
        int save_col = p->lexer.col;
        Token save_curr = p->current;
        Token save_prev = p->previous;

        Token fn_name = advance(p);
        bool is_func_def = false;
        size_t pcount = 0;
        Slice params[16];

        while (check(p, TOK_IDENT)) {
            Token ptok = advance(p);
            if (pcount < 16) params[pcount++] = ptok.lexeme;
        }

        if (check(p, TOK_COLON)) {
            is_func_def = true;
            advance(p); // consume ':'
        }

        if (is_func_def) {
            ASTNode *body = parse_pipe(p);
            ASTNode *node = ast_create(p->arena, AST_BLOCK, line, col);
            node->as.block.kind = HOOK_FUNC_DEF;
            node->as.block.hook_name = fn_name.lexeme;
            node->as.block.param_count = pcount;
            if (pcount > 0) {
                node->as.block.params = (Slice *)arena_alloc(p->arena, pcount * sizeof(Slice));
                memcpy(node->as.block.params, params, pcount * sizeof(Slice));
            }
            node->as.block.body = body;
            return node;
        }

        // Restore parser state if not a function definition
        p->lexer.cursor = save_cursor;
        p->lexer.line = save_line;
        p->lexer.col = save_col;
        p->current = save_curr;
        p->previous = save_prev;
    }

    // Fallback: Standalone top-level pipe
    return parse_pipe(p);
}

static void pre_scan_functions(Parser *p, const char *source, size_t length) {
    Lexer lex;
    lexer_init(&lex, source, length, false);
    Token tok = lexer_next(&lex);
    while (tok.type != TOK_EOF) {
        if (tok.type == TOK_IDENT) {
            Token fn_tok = tok;
            int arity = 0;
            Token next = lexer_next(&lex);
            while (next.type == TOK_IDENT) {
                arity++;
                next = lexer_next(&lex);
            }
            if (next.type == TOK_COLON) {
                if (p->func_count < 64) {
                    p->funcs[p->func_count].name = fn_tok.lexeme;
                    p->funcs[p->func_count].arity = arity;
                    p->func_count++;
                }
            }
            tok = next;
        } else {
            tok = lexer_next(&lex);
        }
    }
}

void parser_init(Parser *parser, const char *source, size_t length, bool interactive, Arena *arena) {
    parser->arena = arena;
    parser->has_error = false;
    parser->error_msg[0] = '\0';
    parser->error_line = 1;
    parser->error_col = 1;
    parser->func_count = 0;
    pre_scan_functions(parser, source, length);
    lexer_init(&parser->lexer, source, length, interactive);
    advance(parser); // load first token
}

ASTNode *parser_parse_program(Parser *parser) {
    ASTNode *prog = ast_create(parser->arena, AST_PROGRAM, 1, 1);
    while (!check(parser, TOK_EOF) && !parser->has_error) {
        ASTNode *item = parse_block_or_pipe(parser);
        if (item) {
            ast_program_add_item(prog, item, parser->arena);
        } else {
            break;
        }
    }
    return prog;
}

ASTNode *parser_parse_pipe(Parser *parser) {
    return parse_pipe(parser);
}

ASTNode *parser_parse_expr(Parser *parser) {
    return parse_expr(parser);
}
