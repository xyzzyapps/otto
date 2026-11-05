#include "vm.h"
#include "bc.h"
#include "lexer.h"
#include "scanner.h"
#include "json_yaml.h"
#include "sqlite_stream.h"
#include "journal.h"
#include "tui.h"
#include "posix_io.h"
#include "ffi.h"
#include "bundle.h"
#include "shell_env.h"
#include "ssh_remote.h"
#include "http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#endif

static uint64_t get_watch_mtime(const char *path) {
    if (!path || !path[0]) path = ".";
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    char pat[512];
    snprintf(pat, sizeof(pat), "%s/*", path);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        h = FindFirstFileA(path, &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        uint64_t t = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        FindClose(h);
        return t;
    }
    uint64_t max_t = 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        uint64_t t = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (t > max_t) max_t = t;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return max_t;
#else
    struct stat st;
    if (stat(path, &st) == 0) {
        return (uint64_t)st.st_mtime;
    }
    return 0;
#endif
}

void vm_init(VM *vm) {
    memset(vm, 0, sizeof(VM));
    arena_init(&vm->perm_arena, 256 * 1024);
    arena_init(&vm->iter_arena, 64 * 1024);
    vm->frame_top = 0;
    vm->frames[0].acc = val_nil();
    vm->frames[0].cur = val_nil();
    vm->frames[0].fld = val_nil();
    vm->frames[0].err = val_nil();
    vm->frames[0].fld_split = false;
    vm->global_symbols = val_map_create(&vm->perm_arena, 64);
    vm->dir_stack = val_list_create(&vm->perm_arena, 16);
    vm->active_jobs = val_list_create(&vm->perm_arena, 16);
    vm->that_val = val_nil();
    vm->simulate_mode = false;
    journal_init();
}

void vm_destroy(VM *vm) {
    arena_free(&vm->iter_arena);
    arena_free(&vm->perm_arena);
}

void vm_split_cur_fields(VM *vm, CallFrame *frame, const char *delim) {
    if (frame->fld_split) return;
    frame->fld_split = true;

    Value cur = frame->cur;
    if (cur.type != VAL_SLICE) {
        frame->fld = val_list_create(&vm->iter_arena, 1);
        val_list_append(&frame->fld, cur, &vm->iter_arena);
        return;
    }

    Slice s = cur.as.slice;
    size_t cap = 16;
    frame->fld = val_list_create(&vm->iter_arena, cap);

    size_t i = 0;
    while (i < s.len) {
        if (!delim) {
            while (i < s.len && isspace((unsigned char)s.ptr[i])) i++;
        } else {
            size_t dlen = strlen(delim);
            while (i + dlen <= s.len && memcmp(s.ptr + i, delim, dlen) == 0) i += dlen;
        }
        if (i >= s.len) break;

        size_t start = i;
        if (!delim) {
            while (i < s.len && !isspace((unsigned char)s.ptr[i])) i++;
        } else {
            size_t dlen = strlen(delim);
            while (i < s.len && (i + dlen > s.len || memcmp(s.ptr + i, delim, dlen) != 0)) i++;
        }
        size_t field_len = i - start;

        Value fld_val = val_slice(s.ptr + start, field_len);
        val_list_append(&frame->fld, fld_val, &vm->iter_arena);
    }
}

Value vm_get_ordinal_field(VM *vm, CallFrame *frame, int index) {
    if (!frame->fld_split) {
        vm_split_cur_fields(vm, frame, NULL);
    }
    size_t len = val_list_len(frame->fld);
    if (len == 0) return val_nil();

    if (index == -1) {
        return val_list_get(frame->fld, len - 1);
    }
    if (index >= 0 && (size_t)index < len) {
        return val_list_get(frame->fld, (size_t)index);
    }
    return val_nil();
}

// Stack for conjunctions / expressions
#define VM_STACK_MAX 1024
static Value vm_stack[VM_STACK_MAX];
static size_t vm_sp = 0;

static inline void push(Value v) {
    if (vm_sp < VM_STACK_MAX) vm_stack[vm_sp++] = v;
}
static inline Value pop(void) {
    return (vm_sp > 0) ? vm_stack[--vm_sp] : val_nil();
}
static inline Value peek_stack(void) {
    return (vm_sp > 0) ? vm_stack[vm_sp - 1] : val_nil();
}

// Stream stack for loops
static StreamDesc *stream_stack[16];
static size_t stream_sp = 0;

// Execute compiled bytecode
void vm_run(VM *vm) {
    if (!vm || !vm->code || vm->code_len == 0) return;

    size_t ip = 0;
    CallFrame *frame = &vm->frames[vm->frame_top];

    while (ip < vm->code_len) {
        Instruction inst = vm->code[ip++];
        uint8_t op = inst.opcode;

        switch (op) {
            case OP_NOP:
                break;

            case OP_HALT:
                return;

            case OP_LOAD_CONST:
                frame->acc = vm->constants[inst.operand];
                break;

            case OP_LOAD_LOCAL:
                if (inst.operand < MAX_LOCALS) {
                    frame->acc = frame->locals[inst.operand];
                }
                break;

            case OP_STORE_LOCAL:
                if (inst.operand < MAX_LOCALS) {
                    frame->locals[inst.operand] = frame->acc;
                }
                break;

            case OP_LOAD_GLOBAL: {
                Value name = vm->constants[inst.operand];
                Value val = val_nil();
                if (name.type == VAL_SLICE && name.as.slice.len > 1 && name.as.slice.ptr[0] == '$') {
                    char *env_key = arena_strndup(&vm->iter_arena, name.as.slice.ptr + 1, name.as.slice.len - 1);
                    const char *env_val = getenv(env_key);
                    frame->acc = env_val ? val_cstr(arena_strdup(&vm->iter_arena, env_val)) : val_nil();
                    break;
                }
                if (frame->cur.type == VAL_MAP && val_map_get(frame->cur, name.as.slice, &val)) {
                    frame->acc = val;
                } else if (val_map_get(vm->global_symbols, name.as.slice, &val)) {
                    frame->acc = val;
                } else {
                    frame->acc = val_nil();
                }
                break;
            }

            case OP_STORE_GLOBAL: {
                Value name = vm->constants[inst.operand];
                if (name.type == VAL_SLICE && name.as.slice.len > 1 && name.as.slice.ptr[0] == '$') {
                    char *env_key = arena_strndup(&vm->iter_arena, name.as.slice.ptr + 1, name.as.slice.len - 1);
                    char *env_val = val_to_string(frame->acc, &vm->iter_arena);
#ifdef _WIN32
                    _putenv_s(env_key, env_val);
#else
                    setenv(env_key, env_val, 1);
#endif
                }
                val_map_set(&vm->global_symbols, name.as.slice, frame->acc, &vm->perm_arena);
                break;
            }

            case OP_LOAD_REG: {
                uint16_t reg = inst.operand;
                if (reg == 0) {
                    // ACC
                } else if (reg == 1) {
                    frame->acc = frame->cur;
                } else if (reg == 2) {
                    frame->acc = vm_get_ordinal_field(vm, frame, 0); // first
                } else if (reg == 3) {
                    frame->acc = vm_get_ordinal_field(vm, frame, 1); // second
                } else if (reg == 4) {
                    frame->acc = vm_get_ordinal_field(vm, frame, 2); // third
                } else if (reg == 5) {
                    frame->acc = vm_get_ordinal_field(vm, frame, 3); // fourth
                } else if (reg == 6) {
                    frame->acc = vm_get_ordinal_field(vm, frame, -1); // last
                } else if (reg >= 100) {
                    int nth = (int)(reg - 100);
                    frame->acc = vm_get_ordinal_field(vm, frame, nth - 1);
                } else if (reg == 7) {
                    frame->acc = frame->err;
                } else if (reg == 8) {
                    frame->acc = vm->previous_cmd;
                } else if (reg == 9) {
                    // that -> previous sentence result
                    frame->acc = vm->that_val;
                } else if (reg == 10) {
                    // them -> active collection / list
                    if (frame->acc.type == VAL_LIST) {
                        // keep list in frame->acc
                    } else if (frame->fld.type == VAL_LIST) {
                        frame->acc = frame->fld;
                    } else if (frame->cur.type != VAL_NIL) {
                        vm_split_cur_fields(vm, frame, NULL);
                        frame->acc = frame->fld;
                    }
                }
                break;
            }

            case OP_SAVE_THAT:
                vm->that_val = frame->acc;
                break;

            case OP_RESET_ITER:
                arena_reset(&vm->iter_arena);
                frame->fld_split = false;
                break;

            case OP_PUSH_ACC:
                push(frame->acc);
                break;

            case OP_POP_ACC:
                frame->acc = pop();
                break;

            case OP_SWAP_ACC: {
                Value top = pop();
                push(frame->acc);
                frame->acc = top;
                break;
            }

            case OP_MERGE_ACC: {
                Value left = pop();
                // If both are lists or outputs, pair/merge them
                Value merged = val_list_create(&vm->iter_arena, 2);
                val_list_append(&merged, left, &vm->iter_arena);
                val_list_append(&merged, frame->acc, &vm->iter_arena);
                frame->acc = merged;
                break;
            }

            case OP_JUMP:
                ip = inst.operand;
                break;

            case OP_JUMP_IF_FALSE:
                if (!val_is_truthy(frame->acc)) {
                    ip = inst.operand;
                }
                break;

            case OP_JUMP_IF_TRUE:
                if (val_is_truthy(frame->acc)) {
                    ip = inst.operand;
                }
                break;

            case OP_JUMP_IF_OK:
                if (frame->err.type == VAL_NIL && val_is_truthy(frame->acc)) {
                    ip = inst.operand;
                } else {
                    frame->err = val_nil(); // Clear ERR for 'or' branch
                }
                break;

            // Tail Call Optimization (Task 3.3)
            case OP_TAILCALL: {
                uint8_t arity = (uint8_t)inst.adverbs;
                uint16_t target_ip = inst.operand;
                // Pop arguments from stack in reverse order directly into frame->locals
                for (int a = (int)arity - 1; a >= 0; a--) {
                    frame->locals[a] = pop();
                }
                ip = target_ip;
                // Zero new frames pushed! frame_top remains unchanged!
                break;
            }

            case OP_CALL: {
                uint8_t arity = (uint8_t)inst.adverbs;
                Value fn_name = vm->constants[inst.operand];
                Value fn_obj = val_nil();
                val_map_get(vm->global_symbols, fn_name.as.slice, &fn_obj);

                if (fn_obj.type == VAL_FUNC && vm->frame_top + 1 < MAX_FRAMES) {
                    CallFrame *new_frame = &vm->frames[++vm->frame_top];
                    memset(new_frame, 0, sizeof(CallFrame));
                    new_frame->ret_ip = (uint32_t)ip;
                    for (int a = (int)arity - 1; a >= 0; a--) {
                        new_frame->locals[a] = pop();
                    }
                    frame = new_frame;
                    ip = fn_obj.as.func.ip_offset;
                }
                break;
            }

            case OP_RETURN: {
                if (vm->frame_top > 0) {
                    Value ret_val = frame->acc;
                    uint32_t ret_ip = frame->ret_ip;
                    vm->frame_top--;
                    frame = &vm->frames[vm->frame_top];
                    frame->acc = ret_val;
                    ip = ret_ip;
                } else {
                    return;
                }
                break;
            }

            case OP_BUILD_CHECK: {
                uint16_t tgt_idx = inst.adverbs;
                uint16_t src_idx = inst.slot_mask;
                Value tgt_v = vm->constants[tgt_idx];
                Value src_v = vm->constants[src_idx];
                char *tgt = (tgt_v.type == VAL_SLICE) ? slice_to_cstr(tgt_v.as.slice, &vm->iter_arena) : "";
                char *src = (src_v.type == VAL_SLICE) ? slice_to_cstr(src_v.as.slice, &vm->iter_arena) : "";
                if (!posix_build_needs_rebuild(tgt, src)) {
                    ip = inst.operand;
                }
                break;
            }

            case OP_BINARY_OP: {
                Value right = frame->acc;
                Value left = pop();
                OttoTokenType bop = (OttoTokenType)inst.operand;
                switch (bop) {
                    case TOK_EQ: frame->acc = val_bool(val_equal(left, right)); break;
                    case TOK_NE: frame->acc = val_bool(!val_equal(left, right)); break;
                    case TOK_LT: frame->acc = val_bool(val_compare(left, right, true) < 0); break;
                    case TOK_LE: frame->acc = val_bool(val_compare(left, right, true) <= 0); break;
                    case TOK_GT: frame->acc = val_bool(val_compare(left, right, true) > 0); break;
                    case TOK_GE: frame->acc = val_bool(val_compare(left, right, true) >= 0); break;
                    case TOK_PLUS: {
                        if (left.type == VAL_INT && right.type == VAL_INT) frame->acc = val_int(left.as.i64 + right.as.i64);
                        else frame->acc = val_float(((left.type == VAL_INT) ? (double)left.as.i64 : left.as.f64) +
                                                    ((right.type == VAL_INT) ? (double)right.as.i64 : right.as.f64));
                        break;
                    }
                    case TOK_MINUS: {
                        if (left.type == VAL_INT && right.type == VAL_INT) frame->acc = val_int(left.as.i64 - right.as.i64);
                        else frame->acc = val_float(((left.type == VAL_INT) ? (double)left.as.i64 : left.as.f64) -
                                                    ((right.type == VAL_INT) ? (double)right.as.i64 : right.as.f64));
                        break;
                    }
                    case TOK_STAR: {
                        if (left.type == VAL_INT && right.type == VAL_INT) frame->acc = val_int(left.as.i64 * right.as.i64);
                        else frame->acc = val_float(((left.type == VAL_INT) ? (double)left.as.i64 : left.as.f64) *
                                                    ((right.type == VAL_INT) ? (double)right.as.i64 : right.as.f64));
                        break;
                    }
                    case TOK_SLASH: {
                        double dl = (left.type == VAL_INT) ? (double)left.as.i64 : left.as.f64;
                        double dr = (right.type == VAL_INT) ? (double)right.as.i64 : right.as.f64;
                        frame->acc = val_float(dr != 0 ? dl / dr : 0);
                        break;
                    }
                    case TOK_PERCENT: {
                        int64_t il = left.as.i64;
                        int64_t ir = right.as.i64;
                        frame->acc = val_int(ir != 0 ? il % ir : 0);
                        break;
                    }
                    default:
                        break;
                }
                break;
            }

            case OP_GENITIVE_GET: {
                Value prop = vm->constants[inst.operand];
                Value obj = frame->acc;
                if (obj.type == VAL_MAP) {
                    Value out_val = val_nil();
                    if (val_map_get(obj, prop.as.slice, &out_val)) {
                        frame->acc = out_val;
                    } else {
                        frame->acc = val_nil();
                    }
                } else if (slice_eq_str(prop.as.slice, "last") && slice_eq_str(vm->constants[inst.operand].as.slice, "last")) {
                    // previous's last argument
                    frame->acc = vm->previous_last_arg;
                }
                break;
            }

            case OP_MAP_CREATE:
                frame->acc = val_map_create(&vm->iter_arena, inst.operand > 0 ? inst.operand : 4);
                break;

            case OP_MAP_SET: {
                Value key = vm->constants[inst.operand];
                Value val = frame->acc;
                Value map = pop();
                val_map_set(&map, key.as.slice, val, &vm->iter_arena);
                frame->acc = map;
                break;
            }

            // Stream Loop Execution (Phase 4 Task 4.1 & 4.2)
            case OP_STREAM_START: {
                Value src = vm->constants[inst.operand];
                char *source_str = (src.type == VAL_SLICE) ? slice_to_cstr(src.as.slice, &vm->perm_arena) : NULL;
                uint8_t sc = inst.scanner;
                const char *where_str = NULL;
                if (inst.slot_mask > 0 && inst.slot_mask < vm->const_count) {
                    Value wval = vm->constants[inst.slot_mask];
                    if (wval.type == VAL_SLICE) where_str = slice_to_cstr(wval.as.slice, &vm->perm_arena);
                }

                StreamDesc *sd = NULL;
                char db_p[256], tbl[128];
                if (source_str && sqlite_is_db_target(source_str, db_p, sizeof(db_p), tbl, sizeof(tbl))) {
                    sd = sqlite_open_stream(source_str, where_str, &vm->perm_arena);
                } else {
                    sd = scanner_open_stream((OttoTokenType)sc, source_str, inst.adverbs, &vm->perm_arena);
                }

                if (sd && stream_sp < 16) {
                    stream_stack[stream_sp++] = sd;
                }
                break;
            }

            case OP_STREAM_NEXT: {
                if (stream_sp == 0) {
                    ip = inst.operand;
                    break;
                }
                StreamDesc *sd = stream_stack[stream_sp - 1];
                Value item = val_nil();
                if (sd && sd->next && sd->next(sd, &item, &vm->iter_arena)) {
                    frame->cur = item;
                    frame->fld_split = false;
                } else {
                    // EOF: pop stream and jump to end
                    if (sd && sd->close) sd->close(sd);
                    stream_sp--;
                    ip = inst.operand;
                }
                break;
            }

            case OP_STREAM_END:
                break;

            // --- Verbs ---
            case OP_VERB_READ: {
                char *path = (frame->acc.type == VAL_SLICE) ? slice_to_cstr(frame->acc.as.slice, &vm->iter_arena) : NULL;
                if (!path) break;

                if (inst.scanner == TOK_SCAN_TREE) {
                    StreamDesc *sd = scanner_open_stream(TOK_SCAN_TREE, path, inst.adverbs, &vm->iter_arena);
                    Value list = val_list_create(&vm->iter_arena, 16);
                    Value item;
                    while (sd->next(sd, &item, &vm->iter_arena)) {
                        val_list_append(&list, item, &vm->iter_arena);
                    }
                    if (sd->close) sd->close(sd);
                    frame->acc = list;
                    break;
                }

                char db_p[256], tbl[128];
                if (sqlite_is_db_target(path, db_p, sizeof(db_p), tbl, sizeof(tbl))) {
                    frame->acc = sqlite_query_rows(path, NULL, &vm->iter_arena);
                } else {
                    FILE *fp = fopen(path, "rb");
                    if (!fp) {
                        // Try trimming quotes or whitespace if any
                        Slice trimmed = slice_trim(frame->acc.as.slice);
                        char *tpath = slice_to_cstr(trimmed, &vm->iter_arena);
                        fp = fopen(tpath, "rb");
                    }
                    if (fp) {
                        fseek(fp, 0, SEEK_END);
                        long fsize = ftell(fp);
                        fseek(fp, 0, SEEK_SET);
                        char *content = (char *)arena_alloc(&vm->iter_arena, fsize + 1);
                        fread(content, 1, fsize, fp);
                        content[fsize] = '\0';
                        fclose(fp);

                        if (inst.scanner == TOK_SCAN_JSON) {
                            bool ok;
                            frame->acc = json_parse(content, fsize, &vm->iter_arena, &ok);
                        } else if (inst.scanner == TOK_SCAN_YAML) {
                            bool ok;
                            frame->acc = yaml_parse(content, fsize, &vm->iter_arena, &ok);
                        } else {
                            frame->acc = val_slice(content, fsize);
                        }
                    } else {
                        frame->err = val_cstr("File not found");
                    }
                }

                // If preposition `into <var>`
                if (inst.slot_mask & PREP_INTO) {
                    Value var_name = vm->constants[inst.operand];
                    if (var_name.type == VAL_SLICE && var_name.as.slice.len > 1 && var_name.as.slice.ptr[0] == '$') {
                        char *env_key = arena_strndup(&vm->iter_arena, var_name.as.slice.ptr + 1, var_name.as.slice.len - 1);
                        char *env_val = val_to_string(frame->acc, &vm->iter_arena);
#ifdef _WIN32
                        _putenv_s(env_key, env_val);
#else
                        setenv(env_key, env_val, 1);
#endif
                    }
                    val_map_set(&vm->global_symbols, var_name.as.slice, frame->acc, &vm->perm_arena);
                }
                break;
            }

            case OP_VERB_WRITE: {
                char *target = NULL;
                bool append = (inst.slot_mask & PREP_ONTO) != 0;
                if (inst.operand < vm->const_count) {
                    Value tval = vm->constants[inst.operand];
                    target = val_to_string(tval, &vm->iter_arena);
                }

                if (target) {
                    if (http_is_url(target)) {
                        Value body_val = frame->acc;
                        if (body_val.type == VAL_MAP || body_val.type == VAL_LIST) {
                            char *jstr = json_stringify(body_val, &vm->iter_arena, false);
                            body_val = val_slice(jstr, strlen(jstr));
                        }
                        Value err = val_nil();
                        Value resp = http_post(target, body_val, &vm->iter_arena, &err);
                        if (err.type != VAL_NIL) frame->err = err;
                        else frame->acc = resp;
                    } else {
                        char db_p[256], tbl[128];
                        if (sqlite_is_db_target(target, db_p, sizeof(db_p), tbl, sizeof(tbl))) {
                            sqlite_write_row(target, frame->acc, append);
                        } else {
                            if (inst.adverbs & ADV_SIMULATE) {
                                char *new_s = val_to_string(frame->acc, &vm->iter_arena);
                                journal_simulate_diff(target, "", 0, new_s, strlen(new_s));
                            } else {
                                journal_backup_file(target);
                                FILE *fp = fopen(target, append ? "ab" : "wb");
                                if (fp) {
                                    val_print(frame->acc, fp);
                                    fclose(fp);
                                }
                            }
                        }
                    }
                }
                break;
            }

            case OP_VERB_PRINT: {
                Value to_print = (frame->acc.type != VAL_NIL) ? frame->acc : frame->cur;
                if (inst.adverbs & (1 << 13)) {
                    // print count lines
                    size_t lines = 0;
                    if (to_print.type == VAL_SLICE) {
                        Slice s = to_print.as.slice;
                        for (size_t i = 0; i < s.len; i++) if (s.ptr[i] == '\n') lines++;
                        if (s.len > 0 && s.ptr[s.len - 1] != '\n') lines++;
                    } else if (to_print.type == VAL_LIST) {
                        lines = val_list_len(to_print);
                    }
                    val_println(val_int((int64_t)lines), stdout);
                    break;
                }

                if ((inst.slot_mask & (1 << 12)) && inst.operand < vm->const_count) {
                    Value prop_name = vm->constants[inst.operand];
                    if (prop_name.type == VAL_SLICE) {
                        if (frame->cur.type == VAL_MAP) {
                            Value pval = val_nil();
                            val_map_get(frame->cur, prop_name.as.slice, &pval);
                            val_println(pval, stdout);
                            break;
                        } else if (frame->acc.type == VAL_MAP) {
                            Value pval = val_nil();
                            val_map_get(frame->acc, prop_name.as.slice, &pval);
                            val_println(pval, stdout);
                            break;
                        } else if (frame->acc.type == VAL_LIST) {
                            int ord = (int)(inst.adverbs & 0xFFF);
                            if (ord > 0) {
                                size_t len = val_list_len(frame->acc);
                                size_t target_idx = (ord == 999) ? (len > 0 ? len - 1 : 0) : (size_t)(ord - 1);
                                if (target_idx < len) {
                                    Value item = val_list_get(frame->acc, target_idx);
                                    if (item.type == VAL_MAP) {
                                        Value pval = val_nil();
                                        val_map_get(item, prop_name.as.slice, &pval);
                                        val_println(pval, stdout);
                                    }
                                }
                            } else {
                                size_t len = val_list_len(frame->acc);
                                for (size_t i = 0; i < len; i++) {
                                    Value item = val_list_get(frame->acc, i);
                                    if (item.type == VAL_MAP) {
                                        Value pval = val_nil();
                                        val_map_get(item, prop_name.as.slice, &pval);
                                        val_println(pval, stdout);
                                    }
                                }
                            }
                            break;
                        }
                    }
                }

                if (inst.scanner == TOK_SCAN_HOST || inst.scanner == TOK_SCAN_HOSTS) {
                    if (frame->cur.type == VAL_MAP) {
                        Value pval = val_nil();
                        if (val_map_get_str(frame->cur, "host", &pval)) {
                            val_println(pval, stdout);
                            break;
                        }
                    }
                }

                if (inst.scanner == TOK_SCAN_LINE || inst.scanner == TOK_SCAN_LINES) {
                    if (inst.operand == 1) { // first line
                        if (to_print.type == VAL_SLICE) {
                            Slice s = to_print.as.slice;
                            const char *nl = (const char *)memchr(s.ptr, '\n', s.len);
                            size_t l_len = nl ? (size_t)(nl - s.ptr) : s.len;
                            if (l_len > 0 && s.ptr[l_len - 1] == '\r') l_len--;
                            to_print = val_slice(s.ptr, l_len);
                        }
                    }
                }

                int ord = (inst.adverbs > 0) ? (int)(inst.adverbs & 0xFFF) : (int)inst.operand;
                if (to_print.type == VAL_LIST && ord > 0) {
                    size_t len = val_list_len(to_print);
                    size_t target_idx = (ord == 999) ? (len > 0 ? len - 1 : 0) : (size_t)(ord - 1);
                    if (target_idx < len) {
                        to_print = val_list_get(to_print, target_idx);
                    }
                } else if (inst.scanner == TOK_SCAN_WORD || inst.scanner == TOK_SCAN_WORDS ||
                    inst.scanner == TOK_SCAN_NUMBER || inst.scanner == TOK_SCAN_NUMBERS) {
                    if (ord > 0) {
                        to_print = (ord == 999) ? vm_get_ordinal_field(vm, frame, -1) : vm_get_ordinal_field(vm, frame, ord - 1);
                    }
                }

                val_println(to_print, stdout);
                fflush(stdout);
                break;
            }

            case OP_VERB_TAKE: {
                // take first word / take first 8 bytes / take where each's active == true
                if (inst.scanner == TOK_SCAN_BYTE || inst.scanner == TOK_SCAN_BYTES) {
                    size_t bytes_to_take = inst.operand > 0 ? inst.operand : 8;
                    if (frame->acc.type == VAL_SLICE && frame->acc.as.slice.len >= bytes_to_take) {
                        frame->acc = val_slice(frame->acc.as.slice.ptr, bytes_to_take);
                    }
                } else if (inst.operand >= 1 && inst.operand <= 4) {
                    if (inst.scanner == TOK_SCAN_WORD || inst.scanner == TOK_SCAN_WORDS ||
                    inst.scanner == TOK_SCAN_NUMBER || inst.scanner == TOK_SCAN_NUMBERS) {
                        frame->acc = vm_get_ordinal_field(vm, frame, inst.operand - 1);
                    }
                } else if (inst.operand == (uint16_t)TOK_ORD_NTH || (inst.operand > 0 && !(inst.slot_mask & PREP_WHERE))) {
                    if (inst.scanner == TOK_SCAN_WORD || inst.scanner == TOK_SCAN_WORDS ||
                    inst.scanner == TOK_SCAN_NUMBER || inst.scanner == TOK_SCAN_NUMBERS) {
                        frame->acc = vm_get_ordinal_field(vm, frame, inst.operand - 1);
                    }
                }

                // If filter list where condition holds
                if ((inst.slot_mask & PREP_WHERE) && frame->acc.type == VAL_LIST) {
                    Value list = frame->acc;
                    size_t len = val_list_len(list);
                    Value filtered = val_list_create(&vm->iter_arena, len);
                    Value prop_name = (inst.operand < vm->const_count) ? vm->constants[inst.operand] : val_cstr("active");

                    for (size_t i = 0; i < len; i++) {
                        Value item = val_list_get(list, i);
                        if (item.type == VAL_MAP) {
                            Value act = val_nil();
                            if (val_map_get(item, prop_name.as.slice, &act) && val_is_truthy(act)) {
                                val_list_append(&filtered, item, &vm->iter_arena);
                            }
                        }
                    }
                    frame->acc = filtered;
                }

                if ((inst.slot_mask & PREP_MATCHING) && frame->acc.type == VAL_SLICE) {
                    Value pat_val = (inst.operand < vm->const_count) ? vm->constants[inst.operand] : val_nil();
                    if (pat_val.type == VAL_SLICE) {
                        char *pat = slice_to_cstr(pat_val.as.slice, &vm->iter_arena);
                        Slice s = frame->acc.as.slice;
                        size_t out_cap = s.len + 2;
                        char *out_buf = (char *)arena_alloc(&vm->iter_arena, out_cap);
                        size_t out_len = 0;
                        size_t start = 0;
                        for (size_t i = 0; i <= s.len; i++) {
                            if (i == s.len || s.ptr[i] == '\n') {
                                size_t line_len = i - start;
                                if (line_len > 0 && s.ptr[start + line_len - 1] == '\r') line_len--;
                                char *line_cstr = arena_strndup(&vm->iter_arena, s.ptr + start, line_len);
                                if (strstr(line_cstr, pat) != NULL) {
                                    memcpy(out_buf + out_len, line_cstr, line_len);
                                    out_len += line_len;
                                    out_buf[out_len++] = '\n';
                                }
                                start = i + 1;
                            }
                        }
                        if (out_len > 0 && out_buf[out_len - 1] == '\n') out_len--;
                        frame->acc = val_slice(out_buf, out_len);
                    }
                }
                break;
            }

            case OP_VERB_DROP:
                break;

            case OP_VERB_SPLIT: {
                // split "," or split by ","
                const char *delim = " ";
                if (inst.operand < vm->const_count) {
                    Value cval = vm->constants[inst.operand];
                    if (cval.type == VAL_SLICE) delim = slice_to_cstr(cval.as.slice, &vm->iter_arena);
                }
                Value to_split = (frame->acc.type == VAL_SLICE) ? frame->acc : frame->cur;
                if (to_split.type == VAL_SLICE) {
                    Slice s = to_split.as.slice;
                    Value split_list = val_list_create(&vm->iter_arena, 16);
                    size_t dlen = strlen(delim);
                    size_t start = 0;
                    for (size_t i = 0; i <= s.len; i++) {
                        bool is_match = false;
                        if (dlen == 1 && delim[0] == ' ') {
                            if (i == s.len || s.ptr[i] == ' ' || s.ptr[i] == '\t' || s.ptr[i] == '\r' || s.ptr[i] == '\n') {
                                is_match = true;
                            }
                        } else if (i + dlen <= s.len && memcmp(s.ptr + i, delim, dlen) == 0) {
                            is_match = true;
                        } else if (i == s.len) {
                            is_match = true;
                        }
                        if (is_match) {
                            size_t token_len = i - start;
                            if (token_len > 0 || (dlen > 0 && delim[0] != ' ')) {
                                val_list_append(&split_list, val_slice(s.ptr + start, token_len), &vm->iter_arena);
                            }
                            start = i + (dlen > 0 ? dlen : 1);
                        }
                    }
                    frame->acc = split_list;
                    frame->fld = split_list;
                    frame->fld_split = true;
                }
                break;
            }

            case OP_VERB_SORT: {
                // sort by age descending
                if (frame->acc.type == VAL_LIST) {
                    Value list = frame->acc;
                    size_t count = val_list_len(list);
                    bool desc = (inst.adverbs & ADV_DESCENDING) != 0;
                    Value sort_key = (inst.operand < vm->const_count) ? vm->constants[inst.operand] : val_cstr("age");
                    // Sort items in place
                    for (size_t i = 0; i < count; i++) {
                        for (size_t j = i + 1; j < count; j++) {
                            Value a = val_list_get(list, i);
                            Value b = val_list_get(list, j);
                            Value va = a, vb = b;
                            if (a.type == VAL_MAP) val_map_get(a, sort_key.as.slice, &va);
                            if (b.type == VAL_MAP) val_map_get(b, sort_key.as.slice, &vb);
                            int cmp = val_compare(va, vb, true);
                            if ((desc && cmp < 0) || (!desc && cmp > 0)) {
                                val_list_set(&list, i, b);
                                val_list_set(&list, j, a);
                            }
                        }
                    }
                }
                break;
            }

            case OP_VERB_REPLACE: {
                // replace "old" with "new" OR replace "old" "new" [inplace] [simulate]
                Value target_file = (frame->cur.type != VAL_NIL) ? frame->cur : frame->acc;
                char *target_str = (target_file.type == VAL_SLICE) ? slice_to_cstr(target_file.as.slice, &vm->iter_arena) : NULL;
                if (!target_str) break;

                const char *old_str = "foo";
                const char *new_str = "bar";
                if (inst.slot_mask < vm->const_count) {
                    Value oval = vm->constants[inst.slot_mask];
                    if (oval.type == VAL_SLICE) old_str = slice_to_cstr(oval.as.slice, &vm->iter_arena);
                }
                if (inst.operand < vm->const_count) {
                    Value nval = vm->constants[inst.operand];
                    if (nval.type == VAL_SLICE) new_str = slice_to_cstr(nval.as.slice, &vm->iter_arena);
                }

                // Check if target is an on-disk file
                FILE *fp = fopen(target_str, "rb");
                if (fp) {
                    fseek(fp, 0, SEEK_END);
                    long fsize = ftell(fp);
                    fseek(fp, 0, SEEK_SET);
                    char *old_buf = (char *)malloc(fsize + 1);
                    fread(old_buf, 1, fsize, fp);
                    old_buf[fsize] = '\0';
                    fclose(fp);

                    char *pos = strstr(old_buf, old_str);
                    char *new_buf = NULL;
                    size_t new_len = 0;

                    if (pos) {
                        size_t pre_len = pos - old_buf;
                        size_t old_slen = strlen(old_str);
                        size_t new_slen = strlen(new_str);
                        size_t post_len = fsize - pre_len - old_slen;
                        new_len = pre_len + new_slen + post_len;
                        new_buf = (char *)malloc(new_len + 1);
                        memcpy(new_buf, old_buf, pre_len);
                        memcpy(new_buf + pre_len, new_str, new_slen);
                        memcpy(new_buf + pre_len + new_slen, pos + old_slen, post_len);
                        new_buf[new_len] = '\0';
                    } else {
                        new_buf = strdup(old_buf);
                        new_len = fsize;
                    }

                    if (inst.adverbs & ADV_SIMULATE) {
                        journal_simulate_diff(target_str, old_buf, fsize, new_buf, new_len);
                    } else {
                        journal_backup_file(target_str);
                        FILE *out = fopen(target_str, "wb");
                        if (out) {
                            fwrite(new_buf, 1, new_len, out);
                            fclose(out);
                        }
                    }
                    free(old_buf);
                    free(new_buf);
                } else {
                    // In-memory string replacement on target_str
                    char *pos = strstr(target_str, old_str);
                    if (pos) {
                        size_t pre_len = pos - target_str;
                        size_t old_slen = strlen(old_str);
                        size_t new_slen = strlen(new_str);
                        size_t post_len = strlen(pos + old_slen);
                        size_t new_len = pre_len + new_slen + post_len;
                        char *new_buf = (char *)arena_alloc(&vm->iter_arena, new_len + 1);
                        memcpy(new_buf, target_str, pre_len);
                        memcpy(new_buf + pre_len, new_str, new_slen);
                        memcpy(new_buf + pre_len + new_slen, pos + old_slen, post_len);
                        new_buf[new_len] = '\0';
                        frame->acc = val_slice(new_buf, new_len);
                    } else {
                        frame->acc = target_file;
                    }
                }
                break;
            }

            case OP_VERB_ADD: {
                // add 2nd number into s
                Value to_add = val_int(0);
                int ordinal = (inst.adverbs > 0) ? (int)(inst.adverbs & 0xFFF) : (int)inst.operand;
                if (ordinal > 0) {
                    Value num_fld = (ordinal == 999) ? vm_get_ordinal_field(vm, frame, -1) : vm_get_ordinal_field(vm, frame, ordinal - 1);
                    if (num_fld.type == VAL_SLICE) {
                        bool ok;
                        to_add = val_int(slice_to_i64(num_fld.as.slice, &ok));
                    } else if (num_fld.type == VAL_INT || num_fld.type == VAL_FLOAT) {
                        to_add = num_fld;
                    }
                }

                // Add to variable in symbols table
                if (inst.slot_mask & PREP_INTO) {
                    Value var_name = vm->constants[inst.operand];
                    Value curr = val_int(0);
                    val_map_get(vm->global_symbols, var_name.as.slice, &curr);
                    int64_t sum = (curr.type == VAL_INT ? curr.as.i64 : 0) + (to_add.type == VAL_INT ? to_add.as.i64 : 0);
                    val_map_set(&vm->global_symbols, var_name.as.slice, val_int(sum), &vm->perm_arena);
                }
                break;
            }

            case OP_VERB_COUNT: {
                if (frame->acc.type == VAL_LIST) {
                    frame->acc = val_int((int64_t)val_list_len(frame->acc));
                } else if (frame->acc.type == VAL_SLICE) {
                    // Count lines
                    size_t lines = 0;
                    Slice s = frame->acc.as.slice;
                    for (size_t i = 0; i < s.len; i++) if (s.ptr[i] == '\n') lines++;
                    frame->acc = val_int((int64_t)lines);
                }
                break;
            }

            case OP_VERB_RUN: {
                char *cmd = (frame->acc.type == VAL_SLICE) ? slice_to_cstr(frame->acc.as.slice, &vm->iter_arena) : NULL;
                if (!cmd && inst.operand < vm->const_count) {
                    Value cval = vm->constants[inst.operand];
                    if (cval.type == VAL_SLICE) cmd = slice_to_cstr(cval.as.slice, &vm->iter_arena);
                }
                if (cmd) {
                    struct stat st;
                    if (stat(cmd, &st) == 0 && (st.st_mode & S_IFDIR)) {
                        shell_cd(cmd, &vm->iter_arena);
                        frame->acc = val_cstr(cmd);
                        break;
                    }
                    Value out_val = val_nil();
                    Value out_err = val_nil();
                    posix_run_command(cmd, val_nil(), &out_val, &out_err, &vm->iter_arena);
                    frame->acc = out_val;
                    frame->err = out_err;
                }
                break;
            }

            case OP_VERB_COMPILE: {
                // compile "test.otto" into "./test-bin"
                char *script_path = (frame->acc.type == VAL_SLICE) ? slice_to_cstr(frame->acc.as.slice, &vm->iter_arena) : NULL;
                char *out_bin = "./test-bin";
                if (inst.operand < vm->const_count) {
                    Value oval = vm->constants[inst.operand];
                    if (oval.type == VAL_SLICE) out_bin = slice_to_cstr(oval.as.slice, &vm->iter_arena);
                }

                char self_exe[512] = "";
#ifdef _WIN32
                GetModuleFileNameA(NULL, self_exe, sizeof(self_exe));
#else
                ssize_t r = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
                if (r > 0) self_exe[r] = '\0';
#endif
                bundle_compile_script(script_path, out_bin, NULL, self_exe);
                break;
            }

            case OP_VERB_UNDO: {
                journal_undo_last();
                break;
            }

            case OP_VERB_PICK: {
                frame->acc = tui_pick(frame->acc, &vm->iter_arena);
                break;
            }

            case OP_VERB_TABLE: {
                tui_table(frame->acc, stdout);
                break;
            }

            case OP_VERB_CHART: {
                tui_chart(frame->acc, "bar", stdout);
                break;
            }

            case OP_VERB_UNPACK: {
                // unpack "u32 u8 u8" into magic bits endian
                Value byte_slice = frame->acc;
                const char *fmt = "u32 u8 u8";
                if (inst.operand < vm->const_count) {
                    Value fval = vm->constants[inst.operand];
                    if (fval.type == VAL_SLICE) fmt = slice_to_cstr(fval.as.slice, &vm->iter_arena);
                }
                Value out_vals[8];
                size_t out_count = 0;
                posix_unpack_bytes(byte_slice, fmt, out_vals, 8, &out_count);

                // Store in magic, bits, endian
                if (out_count >= 1) val_map_set_str(&vm->global_symbols, "magic", out_vals[0], &vm->perm_arena);
                if (out_count >= 2) val_map_set_str(&vm->global_symbols, "bits", out_vals[1], &vm->perm_arena);
                if (out_count >= 3) val_map_set_str(&vm->global_symbols, "endian", out_vals[2], &vm->perm_arena);
                break;
            }

            case OP_VERB_INTERSECT: {
                Value with_val = frame->acc;
                if (inst.operand < vm->const_count) with_val = vm->constants[inst.operand];
                frame->acc = posix_set_intersect(frame->acc, with_val, &vm->iter_arena);
                break;
            }

            case OP_VERB_EXCLUDE: {
                Value from_val = val_nil();
                if (inst.operand < vm->const_count) from_val = vm->constants[inst.operand];
                frame->acc = posix_set_exclude(frame->acc, from_val, &vm->iter_arena);
                break;
            }

            case OP_VERB_ZIP: {
                Value with_val = val_nil();
                if (inst.operand < vm->const_count) with_val = vm->constants[inst.operand];
                frame->acc = posix_set_zip(frame->acc, with_val, &vm->iter_arena);
                break;
            }

            case OP_VERB_DIRS: {
                frame->acc = shell_get_dirs(&vm->iter_arena);
                break;
            }

            case OP_VERB_BACK: {
                shell_back();
                break;
            }

            case OP_VERB_CD: {
                char *path = (frame->acc.type == VAL_SLICE) ? slice_to_cstr(frame->acc.as.slice, &vm->iter_arena) : ".";
                shell_cd(path, &vm->iter_arena);
                break;
            }

            case OP_VERB_JOBS: {
                frame->acc = shell_list_jobs(&vm->iter_arena);
                break;
            }

            case OP_VERB_RESUME: {
                bool bg = (inst.adverbs & ADV_BACKGROUND) != 0;
                shell_resume_job(inst.operand, bg);
                break;
            }

            case OP_VERB_REPEAT: {
                frame->acc = shell_repeat_history(vm->previous_cmd, NULL, NULL, NULL, &vm->iter_arena);
                break;
            }

            case OP_VERB_FETCH: {
                char *url_str = (frame->acc.type == VAL_SLICE) ? slice_to_cstr(frame->acc.as.slice, &vm->iter_arena) : NULL;
                if (!url_str && inst.operand < vm->const_count) {
                    url_str = val_to_string(vm->constants[inst.operand], &vm->iter_arena);
                }
                if (!url_str) {
                    frame->err = val_cstr("fetch: missing URL");
                    break;
                }
                Value err = val_nil();
                Value fetched = http_fetch(url_str, &vm->iter_arena, &err);
                if (err.type != VAL_NIL) {
                    frame->err = err;
                } else {
                    if (inst.scanner == TOK_SCAN_JSON) {
                        Slice s = (fetched.type == VAL_SLICE) ? fetched.as.slice : (Slice){val_to_string(fetched, &vm->iter_arena), strlen(val_to_string(fetched, &vm->iter_arena))};
                        bool ok = false;
                        frame->acc = json_parse((char *)s.ptr, s.len, &vm->iter_arena, &ok);
                    } else if (inst.scanner == TOK_SCAN_YAML) {
                        Slice s = (fetched.type == VAL_SLICE) ? fetched.as.slice : (Slice){val_to_string(fetched, &vm->iter_arena), strlen(val_to_string(fetched, &vm->iter_arena))};
                        bool ok = false;
                        frame->acc = yaml_parse((char *)s.ptr, s.len, &vm->iter_arena, &ok);
                    } else {
                        frame->acc = fetched;
                    }
                }
                break;
            }

            case OP_VERB_JUMP: {
                char *kw = (frame->acc.type == VAL_SLICE) ? slice_to_cstr(frame->acc.as.slice, &vm->iter_arena) : NULL;
                if (!kw && inst.operand < vm->const_count) {
                    kw = val_to_string(vm->constants[inst.operand], &vm->iter_arena);
                }
                if (kw) {
                    Value res = shell_jump(kw, &vm->iter_arena);
                    if (res.type != VAL_NIL) {
                        frame->acc = res;
                    } else {
                        frame->err = val_cstr("jump: no matching directory");
                    }
                }
                break;
            }

            case OP_VERB_BENCHMARK: {
                int runs = inst.adverbs > 0 ? (int)inst.adverbs : 10;
                char *cmd = (frame->acc.type == VAL_SLICE) ? slice_to_cstr(frame->acc.as.slice, &vm->iter_arena) : NULL;
                if (!cmd && inst.operand < vm->const_count) {
                    cmd = val_to_string(vm->constants[inst.operand], &vm->iter_arena);
                }
                if (!cmd) {
                    frame->err = val_cstr("benchmark: missing command");
                    break;
                }

                double *times = (double *)malloc(runs * sizeof(double));
                double sum = 0.0;
                double min_t = 1e9, max_t = 0.0;

#ifdef _WIN32
                LARGE_INTEGER freq;
                QueryPerformanceFrequency(&freq);
#endif

                for (int r = 0; r < runs; r++) {
                    Value out_val = val_nil();
                    Value out_err = val_nil();
#ifdef _WIN32
                    LARGE_INTEGER t0, t1;
                    QueryPerformanceCounter(&t0);
                    posix_run_command(cmd, val_nil(), &out_val, &out_err, &vm->iter_arena);
                    QueryPerformanceCounter(&t1);
                    double ms = ((double)(t1.QuadPart - t0.QuadPart) * 1000.0) / (double)freq.QuadPart;
#else
                    struct timespec t0, t1;
                    clock_gettime(CLOCK_MONOTONIC, &t0);
                    posix_run_command(cmd, val_nil(), &out_val, &out_err, &vm->iter_arena);
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
#endif
                    times[r] = ms;
                    sum += ms;
                    if (ms < min_t) min_t = ms;
                    if (ms > max_t) max_t = ms;
                }

                double mean = sum / runs;
                double var_sum = 0.0;
                for (int r = 0; r < runs; r++) {
                    double diff = times[r] - mean;
                    var_sum += diff * diff;
                }
                double stddev = sqrt(var_sum / runs);
                free(times);

                Value map = val_map_create(&vm->iter_arena, 8);
                val_map_set(&map, (Slice){"runs", 4}, val_int(runs), &vm->iter_arena);
                val_map_set(&map, (Slice){"mean_ms", 7}, val_float(mean), &vm->iter_arena);
                val_map_set(&map, (Slice){"min_ms", 6}, val_float(min_t), &vm->iter_arena);
                val_map_set(&map, (Slice){"max_ms", 6}, val_float(max_t), &vm->iter_arena);
                val_map_set(&map, (Slice){"stddev_ms", 9}, val_float(stddev), &vm->iter_arena);
                frame->acc = map;
                break;
            }

            case OP_VERB_TALLY: {
                Value src = frame->acc;
                ValueList *items = NULL;
                if (src.type == VAL_LIST) {
                    items = src.as.list;
                } else if (src.type == VAL_SLICE) {
                    Value split_list = val_list_create(&vm->iter_arena, 16);
                    Slice s = src.as.slice;
                    size_t start = 0;
                    for (size_t i = 0; i <= s.len; i++) {
                        if (i == s.len || s.ptr[i] == '\n') {
                            size_t elen = i - start;
                            if (elen > 0 && s.ptr[start + elen - 1] == '\r') elen--;
                            if (elen > 0) {
                                val_list_append(&split_list, val_slice(s.ptr + start, elen), &vm->iter_arena);
                            }
                            start = i + 1;
                        }
                    }
                    items = split_list.as.list;
                }

                if (!items || items->count == 0) {
                    frame->acc = val_list_create(&vm->iter_arena, 0);
                    break;
                }

                typedef struct TallyEntry {
                    char key[256];
                    int count;
                } TallyEntry;
                TallyEntry *tallies = (TallyEntry *)arena_alloc_zero(&vm->iter_arena, items->count * sizeof(TallyEntry));
                size_t tally_count = 0;

                for (size_t i = 0; i < items->count; i++) {
                    char *k = val_to_string(items->items[i], &vm->iter_arena);
                    bool found = false;
                    for (size_t j = 0; j < tally_count; j++) {
                        if (strcmp(tallies[j].key, k) == 0) {
                            tallies[j].count++;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        strncpy(tallies[tally_count].key, k, 255);
                        tallies[tally_count].count = 1;
                        tally_count++;
                    }
                }

                for (size_t i = 0; i < tally_count; i++) {
                    for (size_t j = i + 1; j < tally_count; j++) {
                        if (tallies[j].count > tallies[i].count) {
                            TallyEntry tmp = tallies[i];
                            tallies[i] = tallies[j];
                            tallies[j] = tmp;
                        }
                    }
                }

                Value res_list = val_list_create(&vm->iter_arena, tally_count);
                for (size_t i = 0; i < tally_count; i++) {
                    Value m = val_map_create(&vm->iter_arena, 4);
                    val_map_set(&m, (Slice){"key", 3}, val_cstr(tallies[i].key), &vm->iter_arena);
                    val_map_set(&m, (Slice){"count", 5}, val_int(tallies[i].count), &vm->iter_arena);
                    val_list_append(&res_list, m, &vm->iter_arena);
                }
                frame->acc = res_list;
                break;
            }

            case OP_VERB_GROUP: {
                Value src = frame->acc;
                if (src.type != VAL_LIST || !src.as.list || src.as.list->count == 0) {
                    break;
                }
                Slice prop_name = (Slice){"", 0};
                if (inst.operand < vm->const_count) {
                    Value cval = vm->constants[inst.operand];
                    if (cval.type == VAL_SLICE) prop_name = cval.as.slice;
                }

                Value res_map = val_map_create(&vm->iter_arena, 16);
                for (size_t i = 0; i < src.as.list->count; i++) {
                    Value item = src.as.list->items[i];
                    char key_buf[128] = "default";
                    if (item.type == VAL_MAP && prop_name.len > 0) {
                        Value pval = val_nil();
                        if (val_map_get(item, prop_name, &pval) && pval.type != VAL_NIL) {
                            char *s = val_to_string(pval, &vm->iter_arena);
                            strncpy(key_buf, s, sizeof(key_buf) - 1);
                        }
                    } else {
                        char *s = val_to_string(item, &vm->iter_arena);
                        strncpy(key_buf, s, sizeof(key_buf) - 1);
                    }

                    Slice kslice = (Slice){arena_strdup(&vm->iter_arena, key_buf), strlen(key_buf)};
                    Value existing = val_nil();
                    if (!val_map_get(res_map, kslice, &existing) || existing.type != VAL_LIST) {
                        existing = val_list_create(&vm->iter_arena, 8);
                    }
                    val_list_append(&existing, item, &vm->iter_arena);
                    val_map_set(&res_map, kslice, existing, &vm->iter_arena);
                }
                frame->acc = res_map;
                break;
            }

            case OP_WATCH_LOOP: {
                static uint64_t s_last_mtime = 0;
                static bool s_first_run = true;
                const char *path = ".";
                if (inst.slot_mask < vm->const_count) {
                    Value pval = vm->constants[inst.slot_mask];
                    path = val_to_string(pval, &vm->iter_arena);
                }

                if (s_first_run) {
                    s_first_run = false;
                    s_last_mtime = get_watch_mtime(path);
                    break;
                }

                if (vm->simulate_mode || (inst.adverbs & ADV_SIMULATE) || getenv("OTTO_WATCH_ONCE") || getenv("DUD_WATCH_ONCE")) {
                    ip = inst.operand;
                    break;
                }

                while (1) {
#ifdef _WIN32
                    Sleep(150);
#else
                    usleep(150000);
#endif
                    uint64_t cur_t = get_watch_mtime(path);
                    if (cur_t > s_last_mtime) {
                        s_last_mtime = cur_t;
                        break;
                    }
                }
                break;
            }

            default:
                break;
        }
    }

    vm->that_val = vm->frames[0].acc;
}
