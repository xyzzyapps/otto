#include "bc.h"
#include <string.h>
#include <stdio.h>

#define NO_CONST 0xFFFF
#define SLOT_HAS_ORD (1 << 13)

void bc_init(BytecodeEmitter *be, Arena *arena) {
    memset(be, 0, sizeof(BytecodeEmitter));
    be->arena = arena;
    be->capacity = 256;
    be->code = (Instruction *)arena_alloc(arena, be->capacity * sizeof(Instruction));
    be->const_capacity = 64;
    be->constants = (Value *)arena_alloc(arena, be->const_capacity * sizeof(Value));
}

uint16_t bc_add_const(BytecodeEmitter *be, Value val) {
    for (size_t i = 0; i < be->const_count; i++) {
        if (val_equal(be->constants[i], val) && be->constants[i].type == val.type) {
            return (uint16_t)i;
        }
    }
    if (be->const_count >= be->const_capacity) {
        size_t new_cap = be->const_capacity * 2;
        Value *new_c = (Value *)arena_alloc(be->arena, new_cap * sizeof(Value));
        memcpy(new_c, be->constants, be->const_count * sizeof(Value));
        be->constants = new_c;
        be->const_capacity = new_cap;
    }
    uint16_t idx = (uint16_t)be->const_count;
    be->constants[be->const_count++] = val;
    return idx;
}

size_t bc_emit(BytecodeEmitter *be, uint8_t opcode, uint8_t scanner, uint16_t adverbs, uint16_t slot_mask, uint16_t operand) {
    if (be->count >= be->capacity) {
        size_t new_cap = be->capacity * 2;
        Instruction *new_code = (Instruction *)arena_alloc(be->arena, new_cap * sizeof(Instruction));
        memcpy(new_code, be->code, be->count * sizeof(Instruction));
        be->code = new_code;
        be->capacity = new_cap;
    }
    size_t ip = be->count;
    Instruction inst;
    inst.opcode = opcode;
    inst.scanner = scanner;
    inst.adverbs = adverbs;
    inst.slot_mask = slot_mask;
    inst.operand = operand;
    be->code[be->count++] = inst;
    return ip;
}

void bc_patch_operand(BytecodeEmitter *be, size_t ip, uint16_t operand) {
    if (ip < be->count) {
        be->code[ip].operand = operand;
    }
}

// Find local parameter index
static int find_param_idx(BytecodeEmitter *be, Slice name) {
    if (!be->in_function) return -1;
    for (size_t i = 0; i < be->current_fn_param_count; i++) {
        if (slice_eq(be->current_fn_params[i], name)) {
            return (int)i;
        }
    }
    return -1;
}

static void emit_store_target(BytecodeEmitter *be, ASTNode *target) {
    if (!target) return;
    if (target->type == AST_EXPR_VAR) {
        int p_idx = find_param_idx(be, target->as.var.name);
        if (p_idx >= 0) {
            bc_emit(be, OP_STORE_LOCAL, 0, 0, 0, (uint16_t)p_idx);
        } else {
            Value name_val = val_slice(target->as.var.name.ptr, target->as.var.name.len);
            uint16_t c_idx = bc_add_const(be, name_val);
            bc_emit(be, OP_STORE_GLOBAL, 0, 0, 0, c_idx);
        }
    } else if (target->type == AST_EXPR_REGISTER && target->as.reg.reg == TOK_REG_THAT) {
        bc_emit(be, OP_SAVE_THAT, 0, 0, 0, 0);
    }
}

// Compile Expression into instructions leaving result in ACC
static bool compile_expr(BytecodeEmitter *be, ASTNode *expr) {
    if (!expr) return true;

    switch (expr->type) {
        case AST_EXPR_LITERAL: {
            uint16_t c_idx = bc_add_const(be, expr->as.lit.val);
            bc_emit(be, OP_LOAD_CONST, 0, 0, 0, c_idx);
            return true;
        }
        case AST_EXPR_REGISTER: {
            OttoTokenType reg = expr->as.reg.reg;
            uint16_t op = 0;
            if (reg == TOK_REG_IT || reg == TOK_REG_ACC) op = 0; // ACC
            else if (reg == TOK_REG_CUR || reg == TOK_REG_ITEM || reg == TOK_SCAN_LINE || reg == TOK_ORD_EACH) op = 1; // CUR
            else if (reg == TOK_ORD_FIRST) op = 2; // FLD 0
            else if (reg == TOK_ORD_SECOND) op = 3; // FLD 1
            else if (reg == TOK_ORD_THIRD) op = 4; // FLD 2
            else if (reg == TOK_ORD_FOURTH) op = 5; // FLD 3
            else if (reg == TOK_ORD_LAST) op = 6; // FLD last
            else if (reg == TOK_ORD_NTH) op = 100 + (uint16_t)expr->as.reg.nth_val; // FLD nth
            else if (reg == TOK_REG_ERROR) op = 7; // ERR
            else if (reg == TOK_REG_PREVIOUS) op = 8; // PREVIOUS
            else if (reg == TOK_REG_THAT) op = 9; // THAT (previous sentence result)
            else if (reg == TOK_REG_THEM) op = 10; // THEM (active list / collection)
            bc_emit(be, OP_LOAD_REG, 0, 0, 0, op);
            return true;
        }
        case AST_EXPR_VAR: {
            int p_idx = find_param_idx(be, expr->as.var.name);
            if (p_idx >= 0) {
                bc_emit(be, OP_LOAD_LOCAL, 0, 0, 0, (uint16_t)p_idx);
            } else {
                Value name_val = val_slice(expr->as.var.name.ptr, expr->as.var.name.len);
                uint16_t c_idx = bc_add_const(be, name_val);
                bc_emit(be, OP_LOAD_GLOBAL, 0, 0, 0, c_idx);
            }
            return true;
        }
        case AST_EXPR_GENITIVE: {
            compile_expr(be, expr->as.genitive.left);
            Value prop_val = val_slice(expr->as.genitive.prop.ptr, expr->as.genitive.prop.len);
            uint16_t c_idx = bc_add_const(be, prop_val);
            bc_emit(be, OP_GENITIVE_GET, 0, 0, 0, c_idx);
            return true;
        }
        case AST_EXPR_MAP: {
            bc_emit(be, OP_MAP_CREATE, 0, 0, 0, (uint16_t)expr->as.map.count);
            for (size_t i = 0; i < expr->as.map.count; i++) {
                bc_emit(be, OP_PUSH_ACC, 0, 0, 0, 0);
                compile_expr(be, expr->as.map.items[i].val);
                Value k_val = val_slice(expr->as.map.items[i].key.ptr, expr->as.map.items[i].key.len);
                uint16_t c_idx = bc_add_const(be, k_val);
                bc_emit(be, OP_MAP_SET, 0, 0, 0, c_idx);
            }
            return true;
        }
        case AST_EXPR_LIST: {
            bc_emit(be, OP_LIST_CREATE, 0, 0, 0, (uint16_t)expr->as.list.count);
            for (size_t i = 0; i < expr->as.list.count; i++) {
                bc_emit(be, OP_PUSH_ACC, 0, 0, 0, 0);
                compile_expr(be, expr->as.list.items[i]);
                bc_emit(be, OP_LIST_APPEND, 0, 0, 0, 0);
            }
            return true;
        }
        case AST_EXPR_UNARY: {
            compile_expr(be, expr->as.unary.operand);
            bc_emit(be, OP_UNARY_OP, 0, 0, 0, (uint16_t)expr->as.unary.op);
            return true;
        }
        case AST_EXPR_BINARY: {
            compile_expr(be, expr->as.binary.left);
            bc_emit(be, OP_PUSH_ACC, 0, 0, 0, 0);
            compile_expr(be, expr->as.binary.right);
            bc_emit(be, OP_BINARY_OP, 0, 0, 0, (uint16_t)expr->as.binary.op);
            return true;
        }
        case AST_EXPR_CALL: {
            bool is_tail = be->in_function && slice_eq(expr->as.call.func_name, be->current_fn_name);
            for (size_t i = 0; i < expr->as.call.arg_count; i++) {
                compile_expr(be, expr->as.call.args[i]);
                bc_emit(be, OP_PUSH_ACC, 0, 0, 0, 0);
            }
            if (is_tail) {
                bc_emit(be, OP_TAILCALL, 0, (uint16_t)expr->as.call.arg_count, 0, (uint16_t)be->current_fn_ip);
            } else {
                Value fn_name = val_slice(expr->as.call.func_name.ptr, expr->as.call.func_name.len);
                uint16_t c_idx = bc_add_const(be, fn_name);
                bc_emit(be, OP_CALL, 0, (uint16_t)expr->as.call.arg_count, 0, c_idx);
            }
            return true;
        }
        default:
            return false;
    }
}

// Compile Return Statement with TCO support
static bool compile_return(BytecodeEmitter *be, ASTNode *ret) {
    if (ret->as.ret.cond) {
        compile_expr(be, ret->as.ret.cond);
        size_t jump_false = bc_emit(be, OP_JUMP_IF_FALSE, 0, 0, 0, 0);

        compile_expr(be, ret->as.ret.expr);
        bc_emit(be, OP_RETURN, 0, 0, 0, 0);

        bc_patch_operand(be, jump_false, (uint16_t)be->count);
        if (ret->as.ret.else_expr) {
            compile_expr(be, ret->as.ret.else_expr);
            bc_emit(be, OP_RETURN, 0, 0, 0, 0);
        }
    } else {
        compile_expr(be, ret->as.ret.expr);
        bc_emit(be, OP_RETURN, 0, 0, 0, 0);
    }
    return true;
}

static uint8_t verb_to_opcode(OttoTokenType verb) {
    switch (verb) {
        case TOK_VERB_READ: return OP_VERB_READ;
        case TOK_VERB_WRITE: return OP_VERB_WRITE;
        case TOK_VERB_PRINT: return OP_VERB_PRINT;
        case TOK_VERB_TAKE: return OP_VERB_TAKE;
        case TOK_VERB_DROP: return OP_VERB_DROP;
        case TOK_VERB_SPLIT: return OP_VERB_SPLIT;
        case TOK_VERB_JOIN: return OP_VERB_JOIN;
        case TOK_VERB_SORT: return OP_VERB_SORT;
        case TOK_VERB_REPLACE: return OP_VERB_REPLACE;
        case TOK_VERB_ADD: return OP_VERB_ADD;
        case TOK_VERB_COUNT: return OP_VERB_COUNT;
        case TOK_VERB_RUN: return OP_VERB_RUN;
        case TOK_VERB_FETCH: return OP_VERB_FETCH;
        case TOK_VERB_COMPILE: return OP_VERB_COMPILE;
        case TOK_VERB_UNDO: return OP_VERB_UNDO;
        case TOK_VERB_PICK: return OP_VERB_PICK;
        case TOK_VERB_TABLE: return OP_VERB_TABLE;
        case TOK_VERB_CHART: return OP_VERB_CHART;
        case TOK_VERB_UNPACK: return OP_VERB_UNPACK;
        case TOK_VERB_INTERSECT: return OP_VERB_INTERSECT;
        case TOK_VERB_EXCLUDE: return OP_VERB_EXCLUDE;
        case TOK_VERB_ZIP: return OP_VERB_ZIP;
        case TOK_VERB_REMOVE: return OP_VERB_REMOVE;
        case TOK_VERB_MOVE: return OP_VERB_MOVE;
        case TOK_VERB_COPY: return OP_VERB_COPY;
        case TOK_VERB_EXIT: return OP_VERB_EXIT;
        case TOK_VERB_KILL: return OP_VERB_KILL;
        case TOK_VERB_FILTER: return OP_VERB_FILTER;
        case TOK_VERB_TALLY: return OP_VERB_TALLY;
        case TOK_VERB_GROUP: return OP_VERB_GROUP;
        case TOK_VERB_DIRS: return OP_VERB_DIRS;
        case TOK_VERB_BACK: return OP_VERB_BACK;
        case TOK_VERB_CD: return OP_VERB_CD;
        case TOK_VERB_JUMP: return OP_VERB_JUMP;
        case TOK_VERB_BENCHMARK: return OP_VERB_BENCHMARK;
        case TOK_VERB_JOBS: return OP_VERB_JOBS;
        case TOK_VERB_RESUME: return OP_VERB_RESUME;
        case TOK_VERB_REPEAT: return OP_VERB_REPEAT;
        default: return OP_VERB_RUN;
    }
}

static size_t begin_cond_guard(BytecodeEmitter *be, ASTNode *cond, bool unless_cond) {
    if (!cond) return 0;
    bc_emit(be, OP_PUSH_ACC, 0, 0, 0, 0);
    compile_expr(be, cond);
    size_t j = bc_emit(be, unless_cond ? OP_JUMP_IF_TRUE : OP_JUMP_IF_FALSE, 0, 0, 0, 0);
    bc_emit(be, OP_POP_ACC, 0, 0, 0, 0);
    return j;
}

static void end_cond_guard(BytecodeEmitter *be, ASTNode *cond, size_t jump_cond) {
    if (!cond) return;
    size_t skip_pop = bc_emit(be, OP_JUMP, 0, 0, 0, 0);
    bc_patch_operand(be, jump_cond, (uint16_t)be->count);
    bc_emit(be, OP_POP_ACC, 0, 0, 0, 0);
    bc_patch_operand(be, skip_pop, (uint16_t)be->count);
}

// Compile a Command Clause
static bool compile_command(BytecodeEmitter *be, ASTNode *cmd) {
    OttoTokenType verb = cmd->as.cmd.verb;

    // 0. Pure Expression / Assignment Clause (e.g. `0 into s.`, `"a,b", split ","`, `[1,2], chart.`)
    if (verb == TOK_EOF) {
        size_t jcond = begin_cond_guard(be, cmd->as.cmd.cond, cmd->as.cmd.unless_cond);
        compile_expr(be, cmd->as.cmd.direct_arg);
        for (size_t i = 0; i < cmd->as.cmd.prep_count; i++) {
            OttoTokenType prep = cmd->as.cmd.preps[i].prep;
            ASTNode *pexpr = cmd->as.cmd.preps[i].expr;
            if (prep == TOK_PREP_INTO) {
                emit_store_target(be, pexpr);
            } else if (prep == TOK_PREP_ONTO && pexpr) {
                uint16_t c_idx = NO_CONST;
                if (pexpr->type == AST_EXPR_LITERAL) c_idx = bc_add_const(be, pexpr->as.lit.val);
                else if (pexpr->type == AST_EXPR_VAR) c_idx = bc_add_const(be, val_slice(pexpr->as.var.name.ptr, pexpr->as.var.name.len));
                bc_emit(be, OP_VERB_WRITE, 0, cmd->as.cmd.adverbs, PREP_ONTO, c_idx);
            } else if (prep == TOK_PREP_DISCARD) {
                bc_emit(be, OP_DISCARD, 0, 0, 0, 0);
            }
        }
        end_cond_guard(be, cmd->as.cmd.cond, jcond);
        return true;
    }

    // 1. General Predicate Filter Loop (`take where <expr>`, `filter where <expr>`, `drop where <expr>`)
    if (verb == TOK_VERB_TAKE || verb == TOK_VERB_FILTER || verb == TOK_VERB_DROP) {
        ASTNode *where_expr = NULL;
        ASTNode *into_expr = NULL;
        bool has_discard = false;
        for (size_t i = 0; i < cmd->as.cmd.prep_count; i++) {
            if (cmd->as.cmd.preps[i].prep == TOK_PREP_WHERE) where_expr = cmd->as.cmd.preps[i].expr;
            else if (cmd->as.cmd.preps[i].prep == TOK_PREP_INTO) into_expr = cmd->as.cmd.preps[i].expr;
            else if (cmd->as.cmd.preps[i].prep == TOK_PREP_DISCARD) has_discard = true;
        }
        if (where_expr) {
            size_t jcond = begin_cond_guard(be, cmd->as.cmd.cond, cmd->as.cmd.unless_cond);
            if (cmd->as.cmd.direct_arg) {
                compile_expr(be, cmd->as.cmd.direct_arg);
            }
            size_t start_ip = bc_emit(be, OP_FILTER_START, 0, 0, 0, 0);
            size_t body_ip = be->count;
            compile_expr(be, where_expr);
            uint16_t invert = (verb == TOK_VERB_DROP) ? 1 : 0;
            bc_emit(be, OP_FILTER_NEXT, 0, invert, 0, (uint16_t)body_ip);
            bc_patch_operand(be, start_ip, (uint16_t)be->count);
            if (into_expr) emit_store_target(be, into_expr);
            if (has_discard) bc_emit(be, OP_DISCARD, 0, 0, 0, 0);
            end_cond_guard(be, cmd->as.cmd.cond, jcond);
            return true;
        }
    }

    uint8_t op = verb_to_opcode(verb);
    uint8_t scanner = (uint8_t)cmd->as.cmd.scanner;
    uint16_t adverbs = cmd->as.cmd.adverbs;
    uint16_t slot_mask = 0;
    uint16_t operand = NO_CONST;

    ASTNode *post_into_expr = NULL;
    bool post_discard = false;

    size_t jcond = begin_cond_guard(be, cmd->as.cmd.cond, cmd->as.cmd.unless_cond);

    // Pre-push `at <host>` or `repeat` modifiers onto stack before `direct_arg` modifies ACC
    for (size_t i = 0; i < cmd->as.cmd.prep_count; i++) {
        OttoTokenType prep = cmd->as.cmd.preps[i].prep;
        ASTNode *pexpr = cmd->as.cmd.preps[i].expr;
        if (prep == TOK_PREP_AT && pexpr) {
            slot_mask |= PREP_AT;
            compile_expr(be, pexpr);
            bc_emit(be, OP_PUSH_ACC, 0, 0, 0, 0);
        } else if (verb == TOK_VERB_REPEAT && prep == TOK_PREP_REPLACING && pexpr) {
            slot_mask |= PREP_REPLACING;
            compile_expr(be, pexpr);
            bc_emit(be, OP_PUSH_ACC, 0, 0, 0, 0);
        } else if (verb == TOK_VERB_REPEAT && prep == TOK_PREP_WITH && pexpr) {
            slot_mask |= PREP_WITH;
            compile_expr(be, pexpr);
            bc_emit(be, OP_PUSH_ACC, 0, 0, 0, 0);
        }
    }

    if (verb == TOK_VERB_BENCHMARK) {
        adverbs = cmd->as.cmd.nth_val > 0 ? (uint16_t)cmd->as.cmd.nth_val : 10;
    }

    if (verb == TOK_VERB_UNPACK) {
        slot_mask = NO_CONST;
        if (cmd->as.cmd.direct_arg && cmd->as.cmd.direct_arg->type == AST_EXPR_LITERAL) {
            operand = bc_add_const(be, cmd->as.cmd.direct_arg->as.lit.val);
        }
        for (size_t i = 0; i < cmd->as.cmd.prep_count; i++) {
            if (cmd->as.cmd.preps[i].prep == TOK_PREP_INTO && cmd->as.cmd.preps[i].expr) {
                ASTNode *lexpr = cmd->as.cmd.preps[i].expr;
                if (lexpr->type == AST_EXPR_LIST) {
                    // Join var names with space into a constant Slice so it serializes cleanly in bundles too
                    char names_buf[512] = "";
                    size_t pos = 0;
                    for (size_t k = 0; k < lexpr->as.list.count; k++) {
                        ASTNode *vn = lexpr->as.list.items[k];
                        if (vn->type == AST_EXPR_VAR) {
                            if (k > 0 && pos + 1 < sizeof(names_buf)) names_buf[pos++] = ' ';
                            size_t nlen = vn->as.var.name.len;
                            if (pos + nlen < sizeof(names_buf)) {
                                memcpy(names_buf + pos, vn->as.var.name.ptr, nlen);
                                pos += nlen;
                            }
                        }
                    }
                    names_buf[pos] = '\0';
                    char *dup = arena_strndup(be->arena, names_buf, pos);
                    slot_mask = bc_add_const(be, val_slice(dup, pos));
                }
            }
        }
        bc_emit(be, op, scanner, adverbs, slot_mask, operand);
        end_cond_guard(be, cmd->as.cmd.cond, jcond);
        return true;
    }

    if (verb == TOK_VERB_REPLACE) {
        uint16_t old_idx = NO_CONST;
        uint16_t new_idx = NO_CONST;
        if (cmd->as.cmd.direct_arg && cmd->as.cmd.direct_arg->type == AST_EXPR_LITERAL) {
            old_idx = bc_add_const(be, cmd->as.cmd.direct_arg->as.lit.val);
        }
        for (size_t i = 0; i < cmd->as.cmd.prep_count; i++) {
            OttoTokenType prep = cmd->as.cmd.preps[i].prep;
            ASTNode *pexpr = cmd->as.cmd.preps[i].expr;
            if (prep == TOK_PREP_WITH && pexpr && pexpr->type == AST_EXPR_LITERAL) {
                new_idx = bc_add_const(be, pexpr->as.lit.val);
            } else if (prep == TOK_PREP_INTO) {
                post_into_expr = pexpr;
            } else if (prep == TOK_PREP_DISCARD) {
                post_discard = true;
            }
        }
        bc_emit(be, op, scanner, adverbs, old_idx, new_idx);
        if (post_into_expr) emit_store_target(be, post_into_expr);
        if (post_discard) bc_emit(be, OP_DISCARD, 0, 0, 0, 0);
        end_cond_guard(be, cmd->as.cmd.cond, jcond);
        return true;
    }

    if (verb == TOK_VERB_SPLIT || verb == TOK_VERB_JOIN) {
        for (size_t i = 0; i < cmd->as.cmd.prep_count; i++) {
            OttoTokenType prep = cmd->as.cmd.preps[i].prep;
            ASTNode *pexpr = cmd->as.cmd.preps[i].expr;
            if ((prep == TOK_PREP_BY || prep == TOK_PREP_WITH) && pexpr && pexpr->type == AST_EXPR_LITERAL) {
                operand = bc_add_const(be, pexpr->as.lit.val);
            } else if (prep == TOK_PREP_INTO) {
                post_into_expr = pexpr;
            } else if (prep == TOK_PREP_DISCARD) {
                post_discard = true;
            }
        }
        if (operand == NO_CONST && cmd->as.cmd.direct_arg && cmd->as.cmd.direct_arg->type == AST_EXPR_LITERAL) {
            operand = bc_add_const(be, cmd->as.cmd.direct_arg->as.lit.val);
        } else if (cmd->as.cmd.direct_arg && cmd->as.cmd.prep_count > 0 &&
                   cmd->as.cmd.direct_arg != cmd->as.cmd.preps[0].expr) {
            compile_expr(be, cmd->as.cmd.direct_arg);
        }
        bc_emit(be, op, scanner, adverbs, slot_mask, operand);
        if (post_into_expr) emit_store_target(be, post_into_expr);
        if (post_discard) bc_emit(be, OP_DISCARD, 0, 0, 0, 0);
        end_cond_guard(be, cmd->as.cmd.cond, jcond);
        return true;
    }

    // For `add <expr>` without `into` (e.g. `100, add 50`), preserve left ACC on stack
    if (verb == TOK_VERB_ADD && cmd->as.cmd.ordinal == 0 && cmd->as.cmd.prep_count == 0 && cmd->as.cmd.direct_arg) {
        bc_emit(be, OP_PUSH_ACC, 0, 0, 0, 0);
        slot_mask |= PREP_WITH; // signals binary accumulator addition
    }

    if (cmd->as.cmd.direct_arg && verb != TOK_VERB_REPEAT) {
        if (verb == TOK_VERB_PRINT && cmd->as.cmd.direct_arg->type == AST_EXPR_GENITIVE) {
            ASTNode *left = cmd->as.cmd.direct_arg->as.genitive.left;
            bool is_ord_or_each = (left && left->type == AST_EXPR_REGISTER &&
                (left->as.reg.reg == TOK_ORD_EACH || left->as.reg.reg == TOK_REG_CUR ||
                 left->as.reg.reg == TOK_REG_ITEM || token_is_ordinal(left->as.reg.reg)));
            if (is_ord_or_each) {
                Slice prop = cmd->as.cmd.direct_arg->as.genitive.prop;
                Value prop_val = val_slice(prop.ptr, prop.len);
                operand = bc_add_const(be, prop_val);
                slot_mask |= (1 << 12);
                uint16_t ord_val = 0;
                if (left->as.reg.reg == TOK_ORD_FIRST) ord_val = 1;
                else if (left->as.reg.reg == TOK_ORD_SECOND) ord_val = 2;
                else if (left->as.reg.reg == TOK_ORD_THIRD) ord_val = 3;
                else if (left->as.reg.reg == TOK_ORD_FOURTH) ord_val = 4;
                else if (left->as.reg.reg == TOK_ORD_LAST) ord_val = 999;
                else if (left->as.reg.reg == TOK_ORD_NTH) ord_val = (uint16_t)left->as.reg.nth_val;
                if (ord_val > 0) {
                    slot_mask |= SLOT_HAS_ORD;
                    adverbs |= (ord_val & 0xFFF);
                }
            } else {
                compile_expr(be, cmd->as.cmd.direct_arg);
            }
        } else {
            compile_expr(be, cmd->as.cmd.direct_arg);
        }
    }

    if (!cmd->as.cmd.direct_arg && cmd->as.cmd.raw_cmd.len > 0 && verb != TOK_VERB_RUN) {
        if (cmd->as.cmd.scanner != TOK_SCAN_LINE && cmd->as.cmd.scanner != TOK_SCAN_LINES) {
            Value sname = val_slice(cmd->as.cmd.raw_cmd.ptr, cmd->as.cmd.raw_cmd.len);
            operand = bc_add_const(be, sname);
            slot_mask |= (1 << 12);
        }
    }

    if (cmd->as.cmd.ordinal) {
        uint16_t ord_val = 0;
        if (cmd->as.cmd.ordinal == TOK_ORD_FIRST) ord_val = 1;
        else if (cmd->as.cmd.ordinal == TOK_ORD_SECOND) ord_val = 2;
        else if (cmd->as.cmd.ordinal == TOK_ORD_THIRD) ord_val = 3;
        else if (cmd->as.cmd.ordinal == TOK_ORD_FOURTH) ord_val = 4;
        else if (cmd->as.cmd.ordinal == TOK_ORD_LAST) ord_val = 999;
        else if (cmd->as.cmd.ordinal == TOK_ORD_NTH) ord_val = (uint16_t)cmd->as.cmd.nth_val;
        else if (cmd->as.cmd.ordinal == TOK_ORD_TOP) ord_val = 500;

        slot_mask |= SLOT_HAS_ORD;
        adverbs |= (ord_val & 0xFFF);
        if (operand == NO_CONST) operand = ord_val;
    }

    if (cmd->as.cmd.nth_val > 0 && verb != TOK_VERB_BENCHMARK) {
        if (operand == NO_CONST || cmd->as.cmd.scanner == TOK_SCAN_BYTE || cmd->as.cmd.scanner == TOK_SCAN_BYTES ||
            cmd->as.cmd.scanner == TOK_SCAN_LINE || cmd->as.cmd.scanner == TOK_SCAN_LINES) {
            operand = (uint16_t)cmd->as.cmd.nth_val;
        }
    }

    // Handle Preposition Slots
    for (size_t i = 0; i < cmd->as.cmd.prep_count; i++) {
        OttoTokenType prep = cmd->as.cmd.preps[i].prep;
        ASTNode *pexpr = cmd->as.cmd.preps[i].expr;

        if (prep == TOK_PREP_AT || (verb == TOK_VERB_REPEAT && (prep == TOK_PREP_REPLACING || prep == TOK_PREP_WITH))) {
            continue; // already pushed before direct_arg
        }

        if (prep == TOK_PREP_DISCARD) {
            post_discard = true;
            continue;
        }

        if (prep == TOK_PREP_BY) {
            slot_mask |= PREP_BY;
            if (pexpr && pexpr->type == AST_EXPR_VAR) {
                operand = bc_add_const(be, val_slice(pexpr->as.var.name.ptr, pexpr->as.var.name.len));
            } else if (pexpr && pexpr->type == AST_EXPR_LITERAL) {
                operand = bc_add_const(be, pexpr->as.lit.val);
            }
            continue;
        }

        if (prep == TOK_PREP_MATCHING) {
            slot_mask |= PREP_MATCHING;
            if (pexpr && pexpr->type == AST_EXPR_LITERAL) {
                operand = bc_add_const(be, pexpr->as.lit.val);
            }
            continue;
        }

        if (prep == TOK_PREP_INTO) {
            if (verb == TOK_VERB_WRITE && pexpr && pexpr->type == AST_EXPR_VAR) {
                // `write {a: 1} into user` -> bind variable directly without touching disk
                emit_store_target(be, pexpr);
                end_cond_guard(be, cmd->as.cmd.cond, jcond);
                return true;
            }
            if (verb == TOK_VERB_ADD) {
                slot_mask |= PREP_INTO;
                if (pexpr && pexpr->type == AST_EXPR_REGISTER && pexpr->as.reg.reg == TOK_REG_THAT) {
                    operand = 0xFFFE;
                } else if (pexpr && pexpr->type == AST_EXPR_VAR) {
                    operand = bc_add_const(be, val_slice(pexpr->as.var.name.ptr, pexpr->as.var.name.len));
                }
                continue;
            }
            if (verb != TOK_VERB_COMPILE && verb != TOK_VERB_COPY && verb != TOK_VERB_MOVE && verb != TOK_VERB_WRITE) {
                post_into_expr = pexpr;
                continue;
            }
            slot_mask |= PREP_INTO;
        } else if (prep == TOK_PREP_ONTO) {
            slot_mask |= PREP_ONTO;
        } else if (prep == TOK_PREP_FROM) {
            slot_mask |= PREP_FROM;
        } else if (prep == TOK_PREP_WITH) {
            slot_mask |= PREP_WITH;
        } else if (prep == TOK_PREP_REPLACING) {
            slot_mask |= PREP_REPLACING;
        }

        if (pexpr) {
            if (pexpr->type == AST_EXPR_VAR) {
                Value name = val_slice(pexpr->as.var.name.ptr, pexpr->as.var.name.len);
                operand = bc_add_const(be, name);
            } else if (pexpr->type == AST_EXPR_LITERAL) {
                operand = bc_add_const(be, pexpr->as.lit.val);
            } else {
                compile_expr(be, pexpr);
            }
        }
    }

    // If external command or raw command
    if (verb == TOK_VERB_RUN && cmd->as.cmd.raw_cmd.len > 0) {
        Value raw_val = val_slice(cmd->as.cmd.raw_cmd.ptr, cmd->as.cmd.raw_cmd.len);
        operand = bc_add_const(be, raw_val);
    }

    bc_emit(be, op, scanner, adverbs, slot_mask, operand);

    if (post_into_expr) {
        emit_store_target(be, post_into_expr);
    }
    if (post_discard) {
        bc_emit(be, OP_DISCARD, 0, 0, 0, 0);
    }

    end_cond_guard(be, cmd->as.cmd.cond, jcond);
    return true;
}

// Compile a Pipe: Clause ((, / and / or) Clause)*
bool bc_compile_pipe(BytecodeEmitter *be, ASTNode *pipe) {
    if (!pipe || pipe->type != AST_PIPE) return false;

    size_t count = pipe->as.pipe.count;
    size_t or_jump = 0;

    for (size_t i = 0; i < count; i++) {
        ASTNode *clause = pipe->as.pipe.clauses[i];
        ConjKind conj = pipe->as.pipe.conjs[i];

        bool next_is_and = (i + 1 < count && pipe->as.pipe.conjs[i + 1] == CONJ_AND);
        bool next_is_or = (i + 1 < count && pipe->as.pipe.conjs[i + 1] == CONJ_OR);

        if (next_is_and) {
            bc_emit(be, OP_PUSH_ACC, 0, 0, 0, 0);
        }

        if (conj == CONJ_AND) {
            bc_emit(be, OP_POP_ACC, 0, 0, 0, 0);
        }

        if (clause->type == AST_CLAUSE_RETURN) {
            compile_return(be, clause);
        } else {
            compile_command(be, clause);
        }

        if (next_is_or) {
            or_jump = bc_emit(be, OP_JUMP_IF_OK, 0, 0, 0, 0);
        }

        if (conj == CONJ_OR && or_jump > 0) {
            bc_patch_operand(be, or_jump, (uint16_t)be->count);
            or_jump = 0;
        }
    }
    return true;
}

static const char *expr_to_sql_where(ASTNode *expr, Arena *arena) {
    if (!expr || expr->type != AST_EXPR_BINARY) return NULL;

    const char *op_str = NULL;
    if (expr->as.binary.op == TOK_GE) op_str = ">=";
    else if (expr->as.binary.op == TOK_GT) op_str = ">";
    else if (expr->as.binary.op == TOK_LE) op_str = "<=";
    else if (expr->as.binary.op == TOK_LT) op_str = "<";
    else if (expr->as.binary.op == TOK_EQ) op_str = "=";
    else if (expr->as.binary.op == TOK_NE) op_str = "!=";
    if (!op_str) return NULL;

    Slice col_slice = {NULL, 0};
    if (expr->as.binary.left->type == AST_EXPR_VAR) {
        col_slice = expr->as.binary.left->as.var.name;
    } else if (expr->as.binary.left->type == AST_EXPR_GENITIVE) {
        col_slice = expr->as.binary.left->as.genitive.prop;
    } else {
        return NULL;
    }

    if (expr->as.binary.right->type != AST_EXPR_LITERAL) return NULL;
    Value rval = expr->as.binary.right->as.lit.val;

    char col[64];
    snprintf(col, sizeof(col), "%.*s", (int)(col_slice.len < 63 ? col_slice.len : 63), col_slice.ptr);

    char buf[256];
    switch (rval.type) {
        case VAL_INT:
            snprintf(buf, sizeof(buf), "%s %s %lld", col, op_str, (long long)rval.as.i64);
            return arena_strdup(arena, buf);
        case VAL_FLOAT:
            snprintf(buf, sizeof(buf), "%s %s %g", col, op_str, rval.as.f64);
            return arena_strdup(arena, buf);
        case VAL_BOOL:
            snprintf(buf, sizeof(buf), "%s %s %d", col, op_str, rval.as.i64 ? 1 : 0);
            return arena_strdup(arena, buf);
        case VAL_SLICE: {
            char esc[160];
            size_t ei = 0;
            for (size_t i = 0; i < rval.as.slice.len && ei + 3 < sizeof(esc); i++) {
                if (rval.as.slice.ptr[i] == '\'') esc[ei++] = '\'';
                esc[ei++] = rval.as.slice.ptr[i];
            }
            esc[ei] = '\0';
            snprintf(buf, sizeof(buf), "%s %s '%s'", col, op_str, esc);
            return arena_strdup(arena, buf);
        }
        case VAL_NIL:
            snprintf(buf, sizeof(buf), "%s %s NULL", col, (expr->as.binary.op == TOK_NE) ? "IS NOT" : "IS");
            return arena_strdup(arena, buf);
        default:
            return NULL;
    }
}

static Slice reconstruct_pipe_text(ASTNode *pipe, Arena *arena) {
    if (!pipe || pipe->type != AST_PIPE || pipe->as.pipe.count == 0) return (Slice){"", 0};
    ASTNode *c0 = pipe->as.pipe.clauses[0];
    if (c0->type == AST_CLAUSE_CMD && c0->as.cmd.verb == TOK_VERB_REPEAT) return (Slice){"", 0};

    // If raw_cmd is present on single-clause passthrough (e.g. `mkdir "tmp"`), record it directly
    if (pipe->as.pipe.count == 1 && c0->type == AST_CLAUSE_CMD && c0->as.cmd.raw_cmd.len > 0) {
        char *dup = arena_strndup(arena, c0->as.cmd.raw_cmd.ptr, c0->as.cmd.raw_cmd.len);
        return (Slice){dup, c0->as.cmd.raw_cmd.len};
    }

    // Reconstruct simple verb + literal/var direct arg for multi-sentence history (`print "hi"`, `cd "dir"`, etc.)
    if (pipe->as.pipe.count == 1 && c0->type == AST_CLAUSE_CMD && c0->as.cmd.verb != TOK_EOF) {
        const char *vname = token_type_name(c0->as.cmd.verb);
        if (c0->as.cmd.direct_arg && c0->as.cmd.direct_arg->type == AST_EXPR_LITERAL &&
            c0->as.cmd.direct_arg->as.lit.val.type == VAL_SLICE) {
            Slice s = c0->as.cmd.direct_arg->as.lit.val.as.slice;
            char buf[512];
            snprintf(buf, sizeof(buf), "%s \"%.*s\"", vname, (int)s.len, s.ptr);
            char *dup = arena_strdup(arena, buf);
            return (Slice){dup, strlen(dup)};
        }
    }
    return (Slice){"", 0};
}

// Compile Entire Program in Lifecycle Execution Order
bool bc_compile_program(BytecodeEmitter *be, ASTNode *program) {
    if (!program || program->type != AST_PROGRAM) return false;

    size_t total = program->as.program.count;

    // Implicit Stdin Pipeline Loop when explicitly enabled via -n / --stdin flag
    if (be->stdin_loop && total == 1 && program->as.program.items[0]->type == AST_PIPE) {
        ASTNode *pipe = program->as.program.items[0];
        if (pipe->as.pipe.count > 0) {
            ASTNode *c0 = pipe->as.pipe.clauses[0];
            if (c0->type == AST_CLAUSE_CMD) {
                ASTNode *block = ast_create(be->arena, AST_BLOCK, pipe->line, pipe->col);
                block->as.block.kind = HOOK_EVERY;
                block->as.block.scanner = TOK_SCAN_LINE;
                block->as.block.src_name = (Slice){"stdin", 5};
                block->as.block.body = pipe;
                program->as.program.items[0] = block;
            }
        }
    }

    // Pass 0a: Compile Extern C ABI Bindings (`extern "lib" sym(...) into fn.`)
    for (size_t i = 0; i < total; i++) {
        ASTNode *item = program->as.program.items[i];
        if (item->type == AST_BLOCK && item->as.block.kind == HOOK_EXTERN) {
            uint16_t lib_idx = bc_add_const(be, val_slice(item->as.block.extern_lib.ptr, item->as.block.extern_lib.len));
            uint16_t sym_idx = bc_add_const(be, val_slice(item->as.block.extern_symbol.ptr, item->as.block.extern_symbol.len));
            uint16_t verb_idx = bc_add_const(be, val_slice(item->as.block.hook_name.ptr, item->as.block.hook_name.len));
            bc_emit(be, OP_EXTERN_REG, 0, lib_idx, sym_idx, verb_idx);
        }
    }

    // Pass 0b: Compile Function Definitions
    for (size_t i = 0; i < total; i++) {
        ASTNode *item = program->as.program.items[i];
        if (item->type == AST_BLOCK && item->as.block.kind == HOOK_FUNC_DEF) {
            size_t jump_over = bc_emit(be, OP_JUMP, 0, 0, 0, 0);

            be->in_function = true;
            be->current_fn_name = item->as.block.hook_name;
            be->current_fn_ip = (uint32_t)be->count;
            be->current_fn_param_count = item->as.block.param_count;
            for (size_t p = 0; p < item->as.block.param_count; p++) {
                be->current_fn_params[p] = item->as.block.params[p];
            }

            Value fn_name = val_slice(item->as.block.hook_name.ptr, item->as.block.hook_name.len);
            uint16_t c_idx = bc_add_const(be, fn_name);

            Value fn_obj;
            fn_obj.type = VAL_FUNC;
            fn_obj.flags = 0;
            fn_obj.as.func.ip_offset = be->current_fn_ip;
            fn_obj.as.func.arity = (uint8_t)item->as.block.param_count;
            fn_obj.as.func.slot_mask = 0;
            uint16_t fn_cidx = bc_add_const(be, fn_obj);

            bc_compile_pipe(be, item->as.block.body);
            bc_emit(be, OP_RETURN, 0, 0, 0, 0);

            bc_patch_operand(be, jump_over, (uint16_t)be->count);

            bc_emit(be, OP_LOAD_CONST, 0, 0, 0, fn_cidx);
            bc_emit(be, OP_STORE_GLOBAL, 0, 0, 0, c_idx);

            be->in_function = false;
        }
    }

    // Pass 0c: Compile Event Handlers (`on failure:`, `on interrupt:`)
    for (size_t i = 0; i < total; i++) {
        ASTNode *item = program->as.program.items[i];
        if (item->type == AST_BLOCK && item->as.block.kind == HOOK_ON) {
            Slice ev = item->as.block.hook_name;
            uint8_t kind = 0;
            if (slice_eq_str(ev, "failure") || slice_eq_str(ev, "error")) kind = 1;
            else if (slice_eq_str(ev, "interrupt")) kind = 2;
            if (kind > 0) {
                size_t jump_over = bc_emit(be, OP_JUMP, 0, 0, 0, 0);
                uint16_t handler_ip = (uint16_t)be->count;
                bc_compile_pipe(be, item->as.block.body);
                bc_emit(be, (kind == 2) ? OP_HALT : OP_RETURN, 0, 0, 0, 0);
                bc_patch_operand(be, jump_over, (uint16_t)be->count);
                bc_emit(be, OP_ON_HANDLER, kind, 0, 0, handler_ip);
            }
        }
    }

    // Queue 1: before: blocks
    for (size_t i = 0; i < total; i++) {
        ASTNode *item = program->as.program.items[i];
        if (item->type == AST_BLOCK && item->as.block.kind == HOOK_BEFORE) {
            bc_compile_pipe(be, item->as.block.body);
        }
    }

    // Queue 2: Dependency "target" from "src": blocks
    for (size_t i = 0; i < total; i++) {
        ASTNode *item = program->as.program.items[i];
        if (item->type == AST_BLOCK && item->as.block.kind == HOOK_FROM_BUILD) {
            Value tgt = val_slice(item->as.block.hook_name.ptr, item->as.block.hook_name.len);
            Value src = val_slice(item->as.block.src_name.ptr, item->as.block.src_name.len);
            uint16_t tgt_idx = bc_add_const(be, tgt);
            uint16_t src_idx = bc_add_const(be, src);
            size_t jump_over = bc_emit(be, OP_BUILD_CHECK, 0, tgt_idx, src_idx, 0);
            bc_compile_pipe(be, item->as.block.body);
            bc_patch_operand(be, jump_over, (uint16_t)be->count);
        }
    }

    // Queue 2b: watch blocks
    for (size_t i = 0; i < total; i++) {
        ASTNode *item = program->as.program.items[i];
        if (item->type == AST_BLOCK && item->as.block.kind == HOOK_WATCH) {
            Value src = val_slice(item->as.block.src_name.ptr, item->as.block.src_name.len);
            uint16_t src_idx = bc_add_const(be, src);
            size_t watch_loop = be->count;
            size_t exit_jump = bc_emit(be, OP_WATCH_LOOP, (uint8_t)item->as.block.scanner, item->as.block.adverbs, src_idx, 0);
            bc_compile_pipe(be, item->as.block.body);
            bc_emit(be, OP_RESET_ITER, 0, 0, 0, 0);
            bc_emit(be, OP_JUMP, 0, 0, 0, (uint16_t)watch_loop);
            bc_patch_operand(be, exit_jump, (uint16_t)be->count);
        }
    }

    // Queue 3 & 4: Main execution items in program order (`every` loops & top-level pipes)
    for (size_t i = 0; i < total; i++) {
        ASTNode *item = program->as.program.items[i];
        if (item->type == AST_BLOCK && item->as.block.kind == HOOK_EVERY) {
            // Timer interval loop: `every 5s: ...`
            if (item->as.block.scanner == 0 && item->as.block.hook_name.len > 0) {
                Value iv = val_slice(item->as.block.hook_name.ptr, item->as.block.hook_name.len);
                uint16_t iv_idx = bc_add_const(be, iv);
                size_t timer_start = be->count;
                bc_compile_pipe(be, item->as.block.body);
                bc_emit(be, OP_RESET_ITER, 0, 0, 0, 0);
                size_t exit_jump = bc_emit(be, OP_INTERVAL_WAIT, 0, item->as.block.adverbs, iv_idx, 0);
                bc_emit(be, OP_JUMP, 0, 0, 0, (uint16_t)timer_start);
                bc_patch_operand(be, exit_jump, (uint16_t)be->count);
                continue;
            }

            Value src = val_slice(item->as.block.src_name.ptr, item->as.block.src_name.len);
            uint16_t src_idx = bc_add_const(be, src);
            uint8_t sc = (uint8_t)item->as.block.scanner;

            ASTNode *where_expr = NULL;
            const char *where_sql = NULL;
            for (size_t p = 0; p < item->as.block.prep_count; p++) {
                if (item->as.block.preps[p].prep == TOK_PREP_WHERE) {
                    where_expr = item->as.block.preps[p].expr;
                    where_sql = expr_to_sql_where(where_expr, be->arena);
                }
            }
            uint16_t where_idx = NO_CONST;
            if (where_sql) {
                where_idx = bc_add_const(be, val_cstr(where_sql));
            }

            bc_emit(be, OP_STREAM_START, sc, item->as.block.adverbs, where_idx, src_idx);

            size_t loop_start = be->count;
            size_t loop_next = bc_emit(be, OP_STREAM_NEXT, sc, 0, 0, 0);

            size_t skip_row = 0;
            if (where_expr) {
                compile_expr(be, where_expr);
                skip_row = bc_emit(be, OP_JUMP_IF_FALSE, 0, 0, 0, 0);
            }

            bc_compile_pipe(be, item->as.block.body);

            if (where_expr) {
                bc_patch_operand(be, skip_row, (uint16_t)be->count);
            }

            bc_emit(be, OP_RESET_ITER, 0, 0, 0, 0);
            bc_emit(be, OP_JUMP, 0, 0, 0, (uint16_t)loop_start);

            bc_patch_operand(be, loop_next, (uint16_t)be->count);
            bc_emit(be, OP_STREAM_END, 0, 0, 0, 0);
        } else if (item->type == AST_PIPE) {
            bc_compile_pipe(be, item);
            bc_emit(be, OP_SAVE_THAT, 0, 0, 0, 0);
            Slice hist_s = reconstruct_pipe_text(item, be->arena);
            if (hist_s.len > 0) {
                uint16_t h_idx = bc_add_const(be, val_slice(hist_s.ptr, hist_s.len));
                bc_emit(be, OP_RECORD_HIST, 0, 0, 0, h_idx);
            }
        }
    }

    // Queue 5: after: blocks
    for (size_t i = 0; i < total; i++) {
        ASTNode *item = program->as.program.items[i];
        if (item->type == AST_BLOCK && item->as.block.kind == HOOK_AFTER) {
            bc_compile_pipe(be, item->as.block.body);
        }
    }

    // Queue 6: `on exit:` blocks
    for (size_t i = 0; i < total; i++) {
        ASTNode *item = program->as.program.items[i];
        if (item->type == AST_BLOCK && item->as.block.kind == HOOK_ON &&
            slice_eq_str(item->as.block.hook_name, "exit")) {
            bc_compile_pipe(be, item->as.block.body);
        }
    }

    bc_emit(be, OP_HALT, 0, 0, 0, 0);
    return true;
}
