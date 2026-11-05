#ifndef OTTO_BC_H
#define OTTO_BC_H

#include <stdint.h>
#include <stddef.h>
#include "vm.h"
#include "ast.h"

// VM Opcodes
typedef enum {
    OP_NOP = 0,
    OP_HALT,

    // Register / Local Moves
    OP_LOAD_CONST,    // operand: const pool idx -> ACC
    OP_LOAD_LOCAL,    // operand: local slot idx -> ACC
    OP_STORE_LOCAL,   // operand: local slot idx <- ACC
    OP_LOAD_GLOBAL,   // operand: const pool idx (name) -> ACC
    OP_STORE_GLOBAL,  // operand: const pool idx (name) <- ACC
    OP_LOAD_REG,      // operand: reg type -> ACC
    OP_STORE_REG,     // operand: reg type <- ACC
    OP_RESET_ITER,    // resets iter_arena
    OP_SAVE_THAT,     // saves frame->acc into vm->that_val

    // Stack / ACC manipulation for conjunctions
    OP_PUSH_ACC,      // Push ACC to internal stack
    OP_POP_ACC,       // Pop from stack to ACC
    OP_SWAP_ACC,      // Swap ACC with top of stack
    OP_MERGE_ACC,     // Merge ACC with top of stack (for tee `and`)

    // Flow Control
    OP_JUMP,          // operand: target IP
    OP_JUMP_IF_FALSE, // operand: target IP if ACC is falsey
    OP_JUMP_IF_TRUE,  // operand: target IP if ACC is truthy
    OP_JUMP_IF_OK,    // operand: target IP if ERR is nil and ACC truthy (for `or`)

    // Function Calls & TCO
    OP_CALL,          // operand: symbol or func idx
    OP_TAILCALL,      // operand: entry IP (overwrites locals, jumps without growing stack)
    OP_RETURN,        // return from current frame
    OP_BUILD_CHECK,   // operand: jump IP if up to date, slot_mask: src_idx, adverbs: tgt_idx

    // Expressions & Math
    OP_BINARY_OP,     // operand: operator type (==, !=, <, +, -, *, /, %)
    OP_GENITIVE_GET,  // operand: const pool idx of property name
    OP_MAP_CREATE,    // creates map
    OP_MAP_SET,       // operand: const pool idx of key
    OP_LIST_CREATE,
    OP_LIST_APPEND,

    // Verbs (Built-in Shell Operations)
    OP_VERB_READ,
    OP_VERB_WRITE,
    OP_VERB_PRINT,
    OP_VERB_TAKE,
    OP_VERB_DROP,
    OP_VERB_SPLIT,
    OP_VERB_JOIN,
    OP_VERB_SORT,
    OP_VERB_REPLACE,
    OP_VERB_ADD,
    OP_VERB_COUNT,
    OP_VERB_RUN,
    OP_VERB_FETCH,
    OP_VERB_COMPILE,
    OP_VERB_UNDO,
    OP_VERB_PICK,
    OP_VERB_TABLE,
    OP_VERB_CHART,
    OP_VERB_UNPACK,
    OP_VERB_INTERSECT,
    OP_VERB_EXCLUDE,
    OP_VERB_ZIP,
    OP_VERB_REMOVE,
    OP_VERB_MOVE,
    OP_VERB_COPY,
    OP_VERB_EXIT,
    OP_VERB_KILL,
    OP_VERB_FILTER,
    OP_VERB_TALLY,
    OP_VERB_GROUP,
    OP_VERB_DIRS,
    OP_VERB_BACK,
    OP_VERB_CD,
    OP_VERB_JUMP,
    OP_VERB_BENCHMARK,
    OP_VERB_JOBS,
    OP_VERB_RESUME,
    OP_VERB_REPEAT,
    OP_VERB_REPLY,

    // Loop header / stream step
    OP_STREAM_START,  // starts stream
    OP_STREAM_NEXT,   // fetches next CUR; if EOF jumps to operand
    OP_STREAM_END,
    OP_WATCH_LOOP
} OpCode;

typedef struct BytecodeEmitter {
    Instruction *code;
    size_t count;
    size_t capacity;
    
    // Constant pool
    Value *constants;
    size_t const_count;
    size_t const_capacity;

    Arena *arena;

    // Current function context for TCO
    Slice current_fn_name;
    uint32_t current_fn_ip;
    Slice current_fn_params[MAX_LOCALS];
    size_t current_fn_param_count;
    bool in_function;
    bool is_one_liner;
    bool stdin_loop;
} BytecodeEmitter;

void bc_init(BytecodeEmitter *be, Arena *arena);
uint16_t bc_add_const(BytecodeEmitter *be, Value val);
size_t bc_emit(BytecodeEmitter *be, uint8_t opcode, uint8_t scanner, uint16_t adverbs, uint16_t slot_mask, uint16_t operand);
void bc_patch_operand(BytecodeEmitter *be, size_t ip, uint16_t operand);

// Compiler from AST to Bytecode
bool bc_compile_program(BytecodeEmitter *be, ASTNode *program);
bool bc_compile_pipe(BytecodeEmitter *be, ASTNode *pipe);

#endif // OTTO_BC_H
