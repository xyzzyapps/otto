#include "bc.h"
#include "tui.h"
#include <string.h>
#include <stdio.h>

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
        if (val_equal(be->constants[i], val)) {
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
                bc_emit(be, OP_PUSH_ACC, 0, 0, 0, 0); // save map
                compile_expr(be, expr->as.map.items[i].val); // evaluate val into ACC
                Value k_val = val_slice(expr->as.map.items[i].key.ptr, expr->as.map.items[i].key.len);
                uint16_t c_idx = bc_add_const(be, k_val);
                bc_emit(be, OP_MAP_SET, 0, 0, 0, c_idx); // sets key on map popped from stack
            }
            return true;
        }
        case AST_EXPR_BINARY: {
            // Evaluate left -> push -> evaluate right -> OP_BINARY_OP
            compile_expr(be, expr->as.binary.left);
            bc_emit(be, OP_PUSH_ACC, 0, 0, 0, 0);
            compile_expr(be, expr->as.binary.right);
            bc_emit(be, OP_BINARY_OP, 0, 0, 0, (uint16_t)expr->as.binary.op);
            return true;
        }
        case AST_EXPR_CALL: {
            // Check Tail Call Optimization
            bool is_tail = be->in_function && slice_eq(expr->as.call.func_name, be->current_fn_name);
            
            // Evaluate arguments
            for (size_t i = 0; i < expr->as.call.arg_count; i++) {
                compile_expr(be, expr->as.call.args[i]);
                bc_emit(be, OP_PUSH_ACC, 0, 0, 0, 0);
            }

            if (is_tail) {
                // OP_TAILCALL takes number of args in adverbs, target IP in operand
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
        
        // True branch
        compile_expr(be, ret->as.ret.expr);
        bc_emit(be, OP_RETURN, 0, 0, 0, 0);
        
        // False branch
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
        case TOK_VERB_REPLY: return OP_VERB_REPLY;
        default: return OP_VERB_RUN;
    }
}

// Compile a Command Clause
static bool compile_command(BytecodeEmitter *be, ASTNode *cmd) {
    uint8_t op = verb_to_opcode(cmd->as.cmd.verb);
    uint8_t scanner = (uint8_t)cmd->as.cmd.scanner;
    uint16_t adverbs = cmd->as.cmd.adverbs;
    uint16_t slot_mask = 0;
    uint16_t operand = 0;

    // Direct arg (like string or number or expression)
    if (cmd->as.cmd.verb == TOK_VERB_BENCHMARK) {
        adverbs = cmd->as.cmd.nth_val > 0 ? (uint16_t)cmd->as.cmd.nth_val : 10;
    }

    if (cmd->as.cmd.verb == TOK_VERB_UNPACK) {
        if (cmd->as.cmd.direct_arg && cmd->as.cmd.direct_arg->type == AST_EXPR_LITERAL) {
            operand = bc_add_const(be, cmd->as.cmd.direct_arg->as.lit.val);
        }
    } else if (cmd->as.cmd.verb == TOK_VERB_REPLACE) {
        uint16_t old_idx = 0;
        uint16_t new_idx = 0;
        if (cmd->as.cmd.direct_arg && cmd->as.cmd.direct_arg->type == AST_EXPR_LITERAL) {
            old_idx = bc_add_const(be, cmd->as.cmd.direct_arg->as.lit.val);
        }
        if (cmd->as.cmd.prep_count > 0 && cmd->as.cmd.preps[0].expr && cmd->as.cmd.preps[0].expr->type == AST_EXPR_LITERAL) {
            new_idx = bc_add_const(be, cmd->as.cmd.preps[0].expr->as.lit.val);
        }
        slot_mask = old_idx;
        operand = new_idx;
    } else if (cmd->as.cmd.verb == TOK_VERB_SPLIT) {
        if (cmd->as.cmd.prep_count > 0 && cmd->as.cmd.preps[0].expr && cmd->as.cmd.preps[0].expr->type == AST_EXPR_LITERAL) {
            operand = bc_add_const(be, cmd->as.cmd.preps[0].expr->as.lit.val);
        } else if (cmd->as.cmd.direct_arg && cmd->as.cmd.direct_arg->type == AST_EXPR_LITERAL) {
            operand = bc_add_const(be, cmd->as.cmd.direct_arg->as.lit.val);
        }
    } else if (cmd->as.cmd.direct_arg) {
        if (cmd->as.cmd.direct_arg->type == AST_EXPR_GENITIVE) {
            Slice prop = cmd->as.cmd.direct_arg->as.genitive.prop;
            Value prop_val = val_slice(prop.ptr, prop.len);
            operand = bc_add_const(be, prop_val);
            slot_mask |= (1 << 12);
            ASTNode *left = cmd->as.cmd.direct_arg->as.genitive.left;
            if (left && left->type == AST_EXPR_REGISTER) {
                uint16_t ord_val = 0;
                if (left->as.reg.reg == TOK_ORD_FIRST) ord_val = 1;
                else if (left->as.reg.reg == TOK_ORD_SECOND) ord_val = 2;
                else if (left->as.reg.reg == TOK_ORD_THIRD) ord_val = 3;
                else if (left->as.reg.reg == TOK_ORD_FOURTH) ord_val = 4;
                else if (left->as.reg.reg == TOK_ORD_LAST) ord_val = 999;
                else if (left->as.reg.reg == TOK_ORD_NTH) ord_val = (uint16_t)left->as.reg.nth_val;
                adverbs |= (ord_val & 0xFFF);
            }
        } else {
            compile_expr(be, cmd->as.cmd.direct_arg);
        }
    }

    if (!cmd->as.cmd.direct_arg && cmd->as.cmd.raw_cmd.len > 0) {
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
        
        adverbs |= (ord_val & 0xFFF);
        operand = ord_val;
    }

    if (cmd->as.cmd.scanner == TOK_SCAN_BYTE || cmd->as.cmd.scanner == TOK_SCAN_BYTES) {
        if (cmd->as.cmd.nth_val > 0) operand = (uint16_t)cmd->as.cmd.nth_val;
    }

    // Handle Preposition Slots: into, from, with, by, where, matching, at, etc.
    for (size_t i = 0; i < cmd->as.cmd.prep_count; i++) {
        OttoTokenType prep = cmd->as.cmd.preps[i].prep;
        ASTNode *pexpr = cmd->as.cmd.preps[i].expr;

        // Handle Preposition Slots: into, from, with, by, where, matching, at, etc.
        if (prep == TOK_PREP_WHERE) {
            slot_mask |= PREP_WHERE;
            if (pexpr && pexpr->type == AST_EXPR_BINARY) {
                if (pexpr->as.binary.left->type == AST_EXPR_GENITIVE) {
                    Slice prop = pexpr->as.binary.left->as.genitive.prop;
                    operand = bc_add_const(be, val_slice(prop.ptr, prop.len));
                }
            }
            continue;
        }

        if (prep == TOK_PREP_BY) {
            slot_mask |= PREP_BY;
            if (pexpr && pexpr->type == AST_EXPR_VAR) {
                operand = bc_add_const(be, val_slice(pexpr->as.var.name.ptr, pexpr->as.var.name.len));
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

        switch (prep) {
            case TOK_PREP_INTO: slot_mask |= PREP_INTO; break;
            case TOK_PREP_ONTO: slot_mask |= PREP_ONTO; break;
            case TOK_PREP_FROM: slot_mask |= PREP_FROM; break;
            case TOK_PREP_WITH: slot_mask |= PREP_WITH; break;
            case TOK_PREP_BY: slot_mask |= PREP_BY; break;
            case TOK_PREP_WHERE: slot_mask |= PREP_WHERE; break;
            case TOK_PREP_MATCHING: slot_mask |= PREP_MATCHING; break;
            case TOK_PREP_AT: slot_mask |= PREP_AT; break;
            case TOK_PREP_ERRORS: slot_mask |= PREP_ERRORS; break;
            case TOK_PREP_DISCARD: slot_mask |= PREP_DISCARD; break;
            case TOK_PREP_TARGET: slot_mask |= PREP_TARGET; break;
            case TOK_PREP_REPLACING: slot_mask |= PREP_REPLACING; break;
            default: break;
        }

        // Emit prep slot expr onto stack or const pool
        if (pexpr) {
            if (pexpr->type == AST_EXPR_VAR) {
                Value name = val_slice(pexpr->as.var.name.ptr, pexpr->as.var.name.len);
                uint16_t c_idx = bc_add_const(be, name);
                operand = c_idx;
            } else if (pexpr->type == AST_EXPR_LITERAL) {
                uint16_t c_idx = bc_add_const(be, pexpr->as.lit.val);
                operand = c_idx;
            } else {
                compile_expr(be, pexpr);
            }
        }
    }

    // If external command or raw command
    if (cmd->as.cmd.verb == TOK_VERB_RUN && cmd->as.cmd.raw_cmd.len > 0) {
        Value raw_val = val_slice(cmd->as.cmd.raw_cmd.ptr, cmd->as.cmd.raw_cmd.len);
        operand = bc_add_const(be, raw_val);
    }

    // Condition check: if / unless
    size_t jump_cond = 0;
    if (cmd->as.cmd.cond) {
        compile_expr(be, cmd->as.cmd.cond);
        if (cmd->as.cmd.unless_cond) {
            jump_cond = bc_emit(be, OP_JUMP_IF_TRUE, 0, 0, 0, 0);
        } else {
            jump_cond = bc_emit(be, OP_JUMP_IF_FALSE, 0, 0, 0, 0);
        }
    }

    bc_emit(be, op, scanner, adverbs, slot_mask, operand);

    if (cmd->as.cmd.cond) {
        bc_patch_operand(be, jump_cond, (uint16_t)be->count);
    }

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
    if (!expr) return NULL;
    if (expr->type == AST_EXPR_BINARY) {
        const char *op_str = ">=";
        if (expr->as.binary.op == TOK_GE) op_str = ">=";
        else if (expr->as.binary.op == TOK_GT) op_str = ">";
        else if (expr->as.binary.op == TOK_LE) op_str = "<=";
        else if (expr->as.binary.op == TOK_LT) op_str = "<";
        else if (expr->as.binary.op == TOK_EQ) op_str = "=";
        else if (expr->as.binary.op == TOK_NE) op_str = "!=";

        char left[64] = "cpu";
        if (expr->as.binary.left->type == AST_EXPR_VAR) {
            Slice s = expr->as.binary.left->as.var.name;
            snprintf(left, sizeof(left), "%.*s", (int)s.len, s.ptr);
        }
        int right = 80;
        if (expr->as.binary.right->type == AST_EXPR_LITERAL) {
            right = (int)expr->as.binary.right->as.lit.val.as.i64;
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "%s %s %d", left, op_str, right);
        return arena_strdup(arena, buf);
    }
    return NULL;
}

// Compile Entire Program in Lifecycle Execution Order (Phase 4 Task 4.1)
bool bc_compile_program(BytecodeEmitter *be, ASTNode *program) {
    if (!program || program->type != AST_PROGRAM) return false;

    size_t total = program->as.program.count;

    // Check for Implicit Stdin Pipeline Loop:
    // When explicitly enabled via -n / --stdin flag:
    if (be->stdin_loop && total == 1 && program->as.program.items[0]->type == AST_PIPE) {
        ASTNode *pipe = program->as.program.items[0];
        if (pipe->as.pipe.count > 0) {
            ASTNode *c0 = pipe->as.pipe.clauses[0];
            if (c0->type == AST_CLAUSE_CMD) {
                OttoTokenType v = c0->as.cmd.verb;
                bool is_producer = (v == TOK_VERB_READ || v == TOK_VERB_FETCH ||
                                    v == TOK_VERB_BENCHMARK || v == TOK_VERB_COMPILE ||
                                    v == TOK_VERB_UNDO || v == TOK_VERB_DIRS ||
                                    v == TOK_VERB_BACK || v == TOK_VERB_CD ||
                                    v == TOK_VERB_JOBS || v == TOK_VERB_RESUME ||
                                    v == TOK_VERB_REPEAT || v == TOK_VERB_JUMP ||
                                    v == TOK_VERB_EXCLUDE || v == TOK_VERB_INTERSECT ||
                                    v == TOK_VERB_ZIP);
                bool has_literal_arg = (c0->as.cmd.direct_arg && 
                    (c0->as.cmd.direct_arg->type == AST_EXPR_LITERAL || c0->as.cmd.direct_arg->type == AST_EXPR_MAP) &&
                    v != TOK_VERB_SPLIT && v != TOK_VERB_REPLACE);
                
                if (be->stdin_loop || (!is_producer && !has_literal_arg)) {
                    ASTNode *block = ast_create(be->arena, AST_BLOCK, pipe->line, pipe->col);
                    block->as.block.kind = HOOK_EVERY;
                    block->as.block.scanner = TOK_SCAN_LINE;
                    block->as.block.src_name = (Slice){"stdin", 5};
                    block->as.block.body = pipe;
                    program->as.program.items[0] = block;
                }
            }
        }
    }

    // Pass 0: Compile Function Definitions
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

            // Record function in global symbols table / constants
            Value fn_name = val_slice(item->as.block.hook_name.ptr, item->as.block.hook_name.len);
            uint16_t c_idx = bc_add_const(be, fn_name);

            // Store entry IP and arity in constant pool / func info
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

            // Store function object in global symbol
            bc_emit(be, OP_LOAD_CONST, 0, 0, 0, fn_cidx);
            bc_emit(be, OP_STORE_GLOBAL, 0, 0, 0, c_idx);

            be->in_function = false;
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
            // Run build rule if target is older than src or missing
            // Compile check -> jump over if up to date -> body -> patch
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

    // Queue 3 & 4: Main execution items in program order (every stream loops & top-level pipes)
    for (size_t i = 0; i < total; i++) {
        ASTNode *item = program->as.program.items[i];
        if (item->type == AST_BLOCK && item->as.block.kind == HOOK_EVERY) {
            Value src = val_slice(item->as.block.src_name.ptr, item->as.block.src_name.len);
            uint16_t src_idx = bc_add_const(be, src);
            uint8_t sc = (uint8_t)item->as.block.scanner;

            const char *where_sql = NULL;
            if (item->as.block.prep_count > 0 && item->as.block.preps[0].prep == TOK_PREP_WHERE) {
                where_sql = expr_to_sql_where(item->as.block.preps[0].expr, be->arena);
            }
            uint16_t where_idx = 0;
            if (where_sql) {
                where_idx = bc_add_const(be, val_cstr(where_sql));
            }

            // OP_STREAM_START sets up iterator stream
            bc_emit(be, OP_STREAM_START, sc, item->as.block.adverbs, where_idx, src_idx);
            
            size_t loop_start = be->count;
            // OP_STREAM_NEXT fetches next CUR; if EOF jumps to loop_end
            size_t loop_next = bc_emit(be, OP_STREAM_NEXT, sc, 0, 0, 0);

            // Body pipe
            bc_compile_pipe(be, item->as.block.body);

            // Reset iter_arena per stream iteration
            bc_emit(be, OP_RESET_ITER, 0, 0, 0, 0);

            // Jump back to loop_start
            bc_emit(be, OP_JUMP, 0, 0, 0, (uint16_t)loop_start);

            // Patch loop_next EOF exit jump
            bc_patch_operand(be, loop_next, (uint16_t)be->count);
            bc_emit(be, OP_STREAM_END, 0, 0, 0, 0);
        } else if (item->type == AST_PIPE) {
            bc_compile_pipe(be, item);
            bc_emit(be, OP_SAVE_THAT, 0, 0, 0, 0);
        }
    }

    // Queue 5: after: blocks
    for (size_t i = 0; i < total; i++) {
        ASTNode *item = program->as.program.items[i];
        if (item->type == AST_BLOCK && item->as.block.kind == HOOK_AFTER) {
            bc_compile_pipe(be, item->as.block.body);
        }
    }

    bc_emit(be, OP_HALT, 0, 0, 0, 0);
    return true;
}
