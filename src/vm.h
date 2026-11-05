#ifndef OTTO_VM_H
#define OTTO_VM_H

#include "value.h"
#include "arena.h"

// Max local variables per frame
#define MAX_LOCALS 32
#define MAX_FRAMES 256

// 64-bit Instruction structure (Phase 3 Task 3.1)
typedef struct Instruction {
    uint8_t opcode;      // Verb or VM control opcode
    uint8_t scanner;     // Optional TOK_SCANNER extractor ID
    uint16_t adverbs;    // 16-bit adverb modifier bitmask
    uint16_t slot_mask;  // Active prepositions bitmask (INTO, FROM, BY, WHERE, etc.)
    uint16_t operand;    // Constant pool index, local index, or jump offset
} Instruction;

typedef struct CallFrame {
    Value acc;        // `it` / `them` - active pipeline accumulator
    Value cur;        // `each` / `line` / `item` - active loop element
    Value fld;        // `first` / `2nd` / `last` - lazy-split slice list of `cur`
    Value err;        // `error` - nil if OK, error Slice if failed
    Value locals[MAX_LOCALS]; // Positional params + `into <var>` bindings
    uint32_t ret_ip;  // Return instruction pointer
    bool fld_split;   // Whether FLD has been lazily computed for cur
} CallFrame;

typedef struct VM {
    Instruction *code;
    size_t code_len;
    CallFrame frames[MAX_FRAMES];
    uint32_t frame_top;
    Arena perm_arena;
    Arena iter_arena; // Reset on every stream step
    bool simulate_mode; // Set by `simulate` adverb
    
    // Constant pool
    Value *constants;
    size_t const_count;

    // Symbol table for globals and functions
    Value global_symbols; // Map of symbol name -> Value or function info
    
    // Ergonomics & History
    Value previous_cmd;
    Value previous_last_arg;
    Value that_val; // Value of previous sentence's result

    // Watch & Hook state
    uint64_t watch_last_mtime;
    bool watch_initialized;
    uint16_t on_failure_ip;
    bool has_on_failure;
    uint16_t on_interrupt_ip;
    bool has_on_interrupt;
} VM;

void vm_init(VM *vm);
void vm_destroy(VM *vm);
void vm_run(VM *vm);
void vm_record_history(VM *vm, Slice cmd_slice);
void vm_split_cur_fields(VM *vm, CallFrame *frame, const char *delim);
Value vm_get_ordinal_field(VM *vm, CallFrame *frame, int index); // index 0 = first, -1 = last

#endif // OTTO_VM_H
