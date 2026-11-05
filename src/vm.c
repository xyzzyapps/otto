#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif

#include "vm.h"
#include "bc.h"
#include "lexer.h"
#include "parser.h"
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
#include "dev_tools.h"
#include "help_tutor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <signal.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#else
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

#define NO_CONST 0xFFFF
#define SLOT_HAS_ORD (1 << 13)

static volatile sig_atomic_t g_sigint_received = 0;

static void otto_sigint_handler(int sig) {
    (void)sig;
    g_sigint_received = 1;
}

static void sleep_millis(uint32_t ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

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

static const char *find_substr(const char *hay, size_t hlen, const char *needle, size_t nlen, bool casefold) {
    if (nlen == 0) return hay;
    if (nlen > hlen) return NULL;
    size_t limit = hlen - nlen;
    for (size_t i = 0; i <= limit; i++) {
        bool match = true;
        if (casefold) {
            for (size_t j = 0; j < nlen; j++) {
                if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j])) {
                    match = false;
                    break;
                }
            }
        } else {
            if (memcmp(hay + i, needle, nlen) != 0) match = false;
        }
        if (match) return hay + i;
    }
    return NULL;
}

static Value replace_all_slice(Slice src, const char *old_s, const char *new_s, bool casefold, Arena *arena) {
    size_t old_len = strlen(old_s);
    size_t new_len = strlen(new_s);
    if (old_len == 0 || src.len == 0) return val_slice(src.ptr, src.len);

    // Count matches first
    size_t matches = 0;
    const char *p = src.ptr;
    size_t rem = src.len;
    while (rem >= old_len) {
        const char *hit = find_substr(p, rem, old_s, old_len, casefold);
        if (!hit) break;
        matches++;
        size_t adv = (size_t)(hit - p) + old_len;
        p += adv;
        rem -= adv;
    }
    if (matches == 0) return val_slice(src.ptr, src.len);

    size_t out_len = src.len + matches * new_len - matches * old_len;
    char *out = (char *)arena_alloc(arena, out_len + 1);
    size_t wp = 0;
    p = src.ptr;
    rem = src.len;
    while (rem >= old_len) {
        const char *hit = find_substr(p, rem, old_s, old_len, casefold);
        if (!hit) break;
        size_t pre = (size_t)(hit - p);
        if (pre > 0) {
            memcpy(out + wp, p, pre);
            wp += pre;
        }
        if (new_len > 0) {
            memcpy(out + wp, new_s, new_len);
            wp += new_len;
        }
        size_t adv = pre + old_len;
        p += adv;
        rem -= adv;
    }
    if (rem > 0) {
        memcpy(out + wp, p, rem);
        wp += rem;
    }
    out[wp] = '\0';
    return val_slice(out, wp);
}

static Value split_lines_to_list(Slice s, Arena *arena) {
    Value list = val_list_create(arena, 16);
    if (s.len == 0) return list;
    size_t start = 0;
    for (size_t i = 0; i <= s.len; i++) {
        if (i == s.len || s.ptr[i] == '\n') {
            if (i == s.len && start == s.len) break;
            size_t llen = i - start;
            if (llen > 0 && s.ptr[start + llen - 1] == '\r') llen--;
            val_list_append(&list, val_slice(s.ptr + start, llen), arena);
            start = i + 1;
        }
    }
    return list;
}

static Value join_list_with_delim(Value list, const char *delim, Arena *arena) {
    size_t count = val_list_len(list);
    if (count == 0) return val_slice("", 0);
    size_t dlen = strlen(delim);
    size_t total = 0;
    char **parts = (char **)arena_alloc(arena, count * sizeof(char *));
    size_t *lens = (size_t *)arena_alloc(arena, count * sizeof(size_t));
    for (size_t i = 0; i < count; i++) {
        Value v = val_list_get(list, i);
        if (v.type == VAL_SLICE) {
            parts[i] = (char *)v.as.slice.ptr;
            lens[i] = v.as.slice.len;
        } else {
            parts[i] = val_to_string(v, arena);
            lens[i] = strlen(parts[i]);
        }
        total += lens[i];
        if (i + 1 < count) total += dlen;
    }
    char *buf = (char *)arena_alloc(arena, total + 1);
    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        if (lens[i] > 0) {
            memcpy(buf + pos, parts[i], lens[i]);
            pos += lens[i];
        }
        if (i + 1 < count && dlen > 0) {
            memcpy(buf + pos, delim, dlen);
            pos += dlen;
        }
    }
    buf[pos] = '\0';
    return val_slice(buf, pos);
}

static bool copy_file_bytes(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);
    return true;
}

static uint32_t parse_interval_ms(Slice s) {
    Slice t = slice_trim(s);
    if (t.len == 0) return 1000;
    size_t i = 0;
    uint32_t val = 0;
    while (i < t.len && isdigit((unsigned char)t.ptr[i])) {
        val = val * 10 + (uint32_t)(t.ptr[i] - '0');
        i++;
    }
    if (val == 0) val = 1;
    Slice unit = { t.ptr + i, t.len - i };
    if (slice_eq_str(unit, "ms")) return val;
    if (slice_eq_str(unit, "m") || slice_eq_str(unit, "min")) return val * 60000U;
    if (slice_eq_str(unit, "h") || slice_eq_str(unit, "hr")) return val * 3600000U;
    return val * 1000U;
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
    vm->previous_cmd = val_nil();
    vm->previous_last_arg = val_nil();
    vm->that_val = val_nil();
    vm->simulate_mode = false;
    journal_init();
}

void vm_destroy(VM *vm) {
    arena_free(&vm->iter_arena);
    arena_free(&vm->perm_arena);
}

void vm_record_history(VM *vm, Slice cmd_slice) {
    Slice trimmed = slice_trim(cmd_slice);
    if (trimmed.len == 0) return;
    if (trimmed.len > 0 && trimmed.ptr[trimmed.len - 1] == '.') {
        trimmed.len--;
        trimmed = slice_trim(trimmed);
    }
    if (trimmed.len == 0) return;
    if (trimmed.len >= 6 && strncmp(trimmed.ptr, "repeat", 6) == 0 &&
        (trimmed.len == 6 || isspace((unsigned char)trimmed.ptr[6]))) {
        return;
    }

    char *dup = arena_strndup(&vm->perm_arena, trimmed.ptr, trimmed.len);
    vm->previous_cmd = val_slice(dup, trimmed.len);

    Lexer lex;
    lexer_init(&lex, dup, trimmed.len, false);
    Slice last_arg = { NULL, 0 };
    while (1) {
        Token tok = lexer_next(&lex);
        if (tok.type == TOK_EOF || tok.type == TOK_DOT || tok.type == TOK_AMP) break;
        last_arg = tok.lexeme;
    }
    if (last_arg.ptr && last_arg.len > 0) {
        char *adup = arena_strndup(&vm->perm_arena, last_arg.ptr, last_arg.len);
        vm->previous_last_arg = val_slice(adup, last_arg.len);
    }
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

static Value add_numeric_values(Value left, Value right) {
    if (left.type == VAL_SLICE) {
        bool ok = false;
        int64_t iv = slice_to_i64(left.as.slice, &ok);
        if (ok) left = val_int(iv);
        else {
            double dv = slice_to_f64(left.as.slice, &ok);
            left = ok ? val_float(dv) : val_int(0);
        }
    }
    if (right.type == VAL_SLICE) {
        bool ok = false;
        int64_t iv = slice_to_i64(right.as.slice, &ok);
        if (ok) right = val_int(iv);
        else {
            double dv = slice_to_f64(right.as.slice, &ok);
            right = ok ? val_float(dv) : val_int(0);
        }
    }
    if (left.type == VAL_FLOAT || right.type == VAL_FLOAT) {
        double dl = (left.type == VAL_FLOAT) ? left.as.f64 : (left.type == VAL_INT ? (double)left.as.i64 : 0.0);
        double dr = (right.type == VAL_FLOAT) ? right.as.f64 : (right.type == VAL_INT ? (double)right.as.i64 : 0.0);
        return val_float(dl + dr);
    }
    int64_t il = (left.type == VAL_INT) ? left.as.i64 : 0;
    int64_t ir = (right.type == VAL_INT) ? right.as.i64 : 0;
    return val_int(il + ir);
}

static Value add_values_or_concat(Value left, Value right, Arena *arena) {
    bool left_is_str = (left.type == VAL_SLICE);
    bool right_is_str = (right.type == VAL_SLICE);

    if (left_is_str || right_is_str) {
        bool lok = !left_is_str;
        bool rok = !right_is_str;
        if (left_is_str) {
            (void)slice_to_i64(left.as.slice, &lok);
            if (!lok) (void)slice_to_f64(left.as.slice, &lok);
        }
        if (right_is_str) {
            (void)slice_to_i64(right.as.slice, &rok);
            if (!rok) (void)slice_to_f64(right.as.slice, &rok);
        }
        if (!lok || !rok) {
            char *ls = val_to_string(left, arena);
            char *rs = val_to_string(right, arena);
            size_t ll = strlen(ls);
            size_t rl = strlen(rs);
            char *cat = (char *)arena_alloc(arena, ll + rl + 1);
            memcpy(cat, ls, ll);
            memcpy(cat + ll, rs, rl);
            cat[ll + rl] = '\0';
            return val_slice(cat, ll + rl);
        }
    }
    return add_numeric_values(left, right);
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

// Stream stack for `every` loops
static StreamDesc *stream_stack[16];
static size_t stream_sp = 0;

// Filter stack for `where` predicate loops
typedef struct FilterState {
    Value in_list;
    Value out_list;
    size_t idx;
    Value saved_cur;
    Value saved_fld;
    bool saved_fld_split;
    bool from_slice;
} FilterState;

static FilterState filter_stack[16];
static size_t filter_sp = 0;

// Execute compiled bytecode
void vm_run(VM *vm) {
    if (!vm || !vm->code || vm->code_len == 0) return;

    vm->watch_initialized = false;
    vm->watch_last_mtime = 0;
    vm->has_on_failure = false;
    vm->has_on_interrupt = false;

    size_t ip = 0;
    CallFrame *frame = &vm->frames[vm->frame_top];

    while (ip < vm->code_len) {
        if (g_sigint_received && vm->has_on_interrupt) {
            g_sigint_received = 0;
            vm->has_on_interrupt = false;
            ip = vm->on_interrupt_ip;
        }

        Instruction inst = vm->code[ip++];
        uint8_t op = inst.opcode;

        switch (op) {
            case OP_HALT:
                vm->that_val = val_clone_to_arena(vm->frames[0].acc, &vm->perm_arena);
                journal_commit_tx();
                return;

            case OP_LOAD_CONST:
                if (inst.operand < vm->const_count) {
                    frame->acc = vm->constants[inst.operand];
                }
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
                Value cloned = val_clone_to_arena(frame->acc, &vm->perm_arena);
                val_map_set(&vm->global_symbols, name.as.slice, cloned, &vm->perm_arena);
                break;
            }

            case OP_LOAD_REG: {
                uint16_t reg = inst.operand;
                if (reg == 0) {
                    // ACC
                } else if (reg == 1) {
                    frame->acc = frame->cur;
                } else if (reg == 2) {
                    frame->acc = vm_get_ordinal_field(vm, frame, 0);
                } else if (reg == 3) {
                    frame->acc = vm_get_ordinal_field(vm, frame, 1);
                } else if (reg == 4) {
                    frame->acc = vm_get_ordinal_field(vm, frame, 2);
                } else if (reg == 5) {
                    frame->acc = vm_get_ordinal_field(vm, frame, 3);
                } else if (reg == 6) {
                    frame->acc = vm_get_ordinal_field(vm, frame, -1);
                } else if (reg >= 100) {
                    int nth = (int)(reg - 100);
                    frame->acc = vm_get_ordinal_field(vm, frame, nth - 1);
                } else if (reg == 7) {
                    frame->acc = frame->err;
                } else if (reg == 8) {
                    frame->acc = vm->previous_cmd;
                } else if (reg == 9) {
                    frame->acc = vm->that_val;
                } else if (reg == 10) {
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
                vm->that_val = val_clone_to_arena(frame->acc, &vm->perm_arena);
                break;

            case OP_RECORD_HIST:
                if (inst.operand < vm->const_count && vm->constants[inst.operand].type == VAL_SLICE) {
                    vm_record_history(vm, vm->constants[inst.operand].as.slice);
                }
                break;

            case OP_DISCARD:
                frame->acc = val_nil();
                frame->err = val_nil();
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
                    frame->err = val_nil();
                }
                break;

            case OP_TAILCALL: {
                uint8_t arity = (uint8_t)inst.adverbs;
                uint16_t target_ip = inst.operand;
                for (int a = (int)arity - 1; a >= 0; a--) {
                    frame->locals[a] = pop();
                }
                ip = target_ip;
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
                    vm->that_val = val_clone_to_arena(vm->frames[0].acc, &vm->perm_arena);
                    journal_commit_tx();
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

            case OP_EXTERN_REG: {
                const char *lib_s = (inst.adverbs < vm->const_count) ? val_to_string(vm->constants[inst.adverbs], &vm->iter_arena) : "";
                const char *sym_s = (inst.slot_mask < vm->const_count) ? val_to_string(vm->constants[inst.slot_mask], &vm->iter_arena) : "";
                const char *vrb_s = (inst.operand < vm->const_count) ? val_to_string(vm->constants[inst.operand], &vm->iter_arena) : "";
                if (!ffi_register_extern(lib_s, sym_s, vrb_s)) {
                    frame->err = val_cstr("extern: failed to load symbol");
                }
                break;
            }

            case OP_ON_HANDLER: {
                if (inst.scanner == 1) {
                    vm->has_on_failure = true;
                    vm->on_failure_ip = inst.operand;
                } else if (inst.scanner == 2) {
                    vm->has_on_interrupt = true;
                    vm->on_interrupt_ip = inst.operand;
                    signal(SIGINT, otto_sigint_handler);
                }
                break;
            }

            case OP_UNARY_OP: {
                OttoTokenType uop = (OttoTokenType)inst.operand;
                if (uop == TOK_MINUS) {
                    if (frame->acc.type == VAL_INT) frame->acc = val_int(-frame->acc.as.i64);
                    else if (frame->acc.type == VAL_FLOAT) frame->acc = val_float(-frame->acc.as.f64);
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
                    case TOK_PLUS:
                        frame->acc = add_values_or_concat(left, right, &vm->iter_arena);
                        break;
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
                } else if (prop.type == VAL_SLICE && slice_eq_str(prop.as.slice, "last")) {
                    frame->acc = vm->previous_last_arg;
                } else if (obj.type == VAL_SLICE && prop.type == VAL_SLICE) {
                    bool handled = false;
                    Value fprop = dev_stat_prop(obj.as.slice, prop.as.slice, &vm->iter_arena, &handled);
                    frame->acc = handled ? fprop : val_nil();
                } else {
                    frame->acc = val_nil();
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

            case OP_LIST_CREATE:
                frame->acc = val_list_create(&vm->iter_arena, inst.operand > 0 ? inst.operand : 4);
                break;

            case OP_LIST_APPEND: {
                Value item = frame->acc;
                Value list = pop();
                val_list_append(&list, item, &vm->iter_arena);
                frame->acc = list;
                break;
            }

            case OP_FILTER_START: {
                bool from_slice = false;
                Value in_list = val_nil();
                if (frame->acc.type == VAL_LIST) {
                    in_list = frame->acc;
                } else if (frame->acc.type == VAL_SLICE) {
                    from_slice = true;
                    in_list = split_lines_to_list(frame->acc.as.slice, &vm->iter_arena);
                } else {
                    in_list = val_list_create(&vm->iter_arena, 0);
                }

                size_t len = val_list_len(in_list);
                if (len == 0 || filter_sp >= 16) {
                    frame->acc = from_slice ? val_slice("", 0) : val_list_create(&vm->iter_arena, 0);
                    ip = inst.operand;
                    break;
                }

                FilterState *fs = &filter_stack[filter_sp++];
                fs->in_list = in_list;
                fs->out_list = val_list_create(&vm->iter_arena, len);
                fs->idx = 0;
                fs->saved_cur = frame->cur;
                fs->saved_fld = frame->fld;
                fs->saved_fld_split = frame->fld_split;
                fs->from_slice = from_slice;

                frame->cur = val_list_get(in_list, 0);
                frame->acc = frame->cur;
                frame->fld_split = false;
                break;
            }

            case OP_FILTER_NEXT: {
                if (filter_sp == 0) break;
                FilterState *fs = &filter_stack[filter_sp - 1];
                bool keep = val_is_truthy(frame->acc);
                if (inst.adverbs == 1) keep = !keep;
                if (keep) {
                    val_list_append(&fs->out_list, val_list_get(fs->in_list, fs->idx), &vm->iter_arena);
                }
                fs->idx++;
                if (fs->idx < val_list_len(fs->in_list)) {
                    frame->cur = val_list_get(fs->in_list, fs->idx);
                    frame->acc = frame->cur;
                    frame->fld_split = false;
                    ip = inst.operand;
                } else {
                    frame->cur = fs->saved_cur;
                    frame->fld = fs->saved_fld;
                    frame->fld_split = fs->saved_fld_split;
                    frame->acc = fs->from_slice ? join_list_with_delim(fs->out_list, "\n", &vm->iter_arena) : fs->out_list;
                    filter_sp--;
                }
                break;
            }

            case OP_STREAM_START: {
                Value src = vm->constants[inst.operand];
                char *source_str = (src.type == VAL_SLICE) ? slice_to_cstr(src.as.slice, &vm->perm_arena) : NULL;
                uint8_t sc = inst.scanner;
                const char *where_str = NULL;
                if (inst.slot_mask != NO_CONST && inst.slot_mask < vm->const_count) {
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
                if (frame->acc.type == VAL_NIL) {
                    if (inst.scanner == TOK_SCAN_PROCESS || inst.scanner == TOK_SCAN_PROCESSES) {
                        StreamDesc *sd = scanner_open_stream(TOK_SCAN_PROCESS, NULL, inst.adverbs, &vm->iter_arena);
                        Value list = val_list_create(&vm->iter_arena, 32);
                        Value item;
                        while (sd && sd->next && sd->next(sd, &item, &vm->iter_arena)) {
                            val_list_append(&list, item, &vm->iter_arena);
                        }
                        if (sd && sd->close) sd->close(sd);
                        frame->acc = list;
                        break;
                    }
                    if (inst.scanner == TOK_SCAN_PORT || inst.scanner == TOK_SCAN_PORTS) {
                        frame->acc = dev_scan_ports(&vm->iter_arena);
                        break;
                    }
                    if (inst.scanner == TOK_SCAN_STATUS || inst.scanner == TOK_SCAN_COMMITS ||
                        inst.scanner == TOK_SCAN_BRANCHES || inst.scanner == TOK_SCAN_DIFF) {
                        frame->acc = dev_git_scan((OttoTokenType)inst.scanner, NULL, &vm->iter_arena);
                        break;
                    }
                }
                char *path = (frame->acc.type == VAL_SLICE) ? slice_to_cstr(frame->acc.as.slice, &vm->iter_arena) : NULL;
                if (!path) break;

                if (inst.scanner == TOK_SCAN_DIFF) {
                    frame->acc = dev_git_scan(TOK_SCAN_DIFF, path, &vm->iter_arena);
                    break;
                }

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
                        Slice trimmed = slice_trim(frame->acc.as.slice);
                        char *tpath = slice_to_cstr(trimmed, &vm->iter_arena);
                        fp = fopen(tpath, "rb");
                    }
                    if (fp) {
                        fseek(fp, 0, SEEK_END);
                        long fsize = ftell(fp);
                        if (fsize < 0) fsize = 0;
                        fseek(fp, 0, SEEK_SET);
                        char *content = (char *)arena_alloc(&vm->iter_arena, (size_t)fsize + 1);
                        size_t nread = fread(content, 1, (size_t)fsize, fp);
                        content[nread] = '\0';
                        fclose(fp);

                        if (inst.scanner == TOK_SCAN_JSON) {
                            bool ok;
                            frame->acc = json_parse(content, nread, &vm->iter_arena, &ok);
                        } else if (inst.scanner == TOK_SCAN_YAML) {
                            bool ok;
                            frame->acc = yaml_parse(content, nread, &vm->iter_arena, &ok);
                        } else if (inst.scanner == TOK_SCAN_TOML) {
                            bool ok;
                            frame->acc = toml_parse(content, nread, &vm->iter_arena, &ok);
                        } else if (inst.scanner == TOK_SCAN_CSV) {
                            frame->acc = csv_parse(content, nread, &vm->iter_arena);
                        } else if (inst.scanner == TOK_SCAN_JSONL) {
                            frame->acc = jsonl_parse(content, nread, &vm->iter_arena);
                        } else {
                            frame->acc = val_slice(content, nread);
                        }
                    } else {
                        frame->err = val_cstr("File not found");
                    }
                }
                break;
            }

            case OP_VERB_WRITE: {
                char *target = NULL;
                bool append = (inst.slot_mask & PREP_ONTO) != 0;
                if (inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    Value tval = vm->constants[inst.operand];
                    target = val_to_string(tval, &vm->iter_arena);
                }

                if (target) {
                    if (http_is_url(target)) {
                        Value body_val = frame->acc;
                        if (body_val.type == VAL_MAP || body_val.type == VAL_LIST) {
                            char *jstr = json_stringify(body_val, &vm->iter_arena);
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
                            if ((inst.adverbs & ADV_SIMULATE) || vm->simulate_mode) {
                                char *new_s = val_to_string(frame->acc, &vm->iter_arena);
                                char *old_s = "";
                                size_t old_len = 0;
                                FILE *efp = fopen(target, "rb");
                                if (efp) {
                                    fseek(efp, 0, SEEK_END);
                                    long sz = ftell(efp);
                                    if (sz > 0) {
                                        fseek(efp, 0, SEEK_SET);
                                        old_s = (char *)arena_alloc(&vm->iter_arena, (size_t)sz + 1);
                                        old_len = fread(old_s, 1, (size_t)sz, efp);
                                        old_s[old_len] = '\0';
                                    }
                                    fclose(efp);
                                }
                                journal_simulate_diff(target, old_s, old_len, new_s, strlen(new_s));
                            } else {
                                journal_backup_file(target);
                                FILE *fp = fopen(target, append ? "ab" : "wb");
                                if (fp) {
                                    val_print(frame->acc, fp);
                                    fclose(fp);
                                } else {
                                    frame->err = val_cstr("write: cannot open file");
                                }
                            }
                        }
                    }
                }
                break;
            }

            case OP_VERB_PRINT: {
                Value to_print = (frame->acc.type != VAL_NIL) ? frame->acc : frame->cur;
                bool has_ord = (inst.slot_mask & SLOT_HAS_ORD) != 0;
                int ord = has_ord ? (int)(inst.adverbs & 0xFFF) : 0;

                if (inst.adverbs & (1 << 13)) {
                    size_t cnt = 0;
                    if (to_print.type == VAL_SLICE) {
                        Slice s = to_print.as.slice;
                        for (size_t i = 0; i < s.len; i++) if (s.ptr[i] == '\n') cnt++;
                        if (s.len > 0 && s.ptr[s.len - 1] != '\n') cnt++;
                    } else if (to_print.type == VAL_LIST) {
                        cnt = val_list_len(to_print);
                    } else if (to_print.type == VAL_MAP && to_print.as.map) {
                        cnt = to_print.as.map->count;
                    }
                    val_println(val_int((int64_t)cnt), stdout);
                    fflush(stdout);
                    break;
                }

                if ((inst.slot_mask & (1 << 12)) && inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    Value prop_name = vm->constants[inst.operand];
                    if (prop_name.type == VAL_SLICE) {
                        if (frame->cur.type == VAL_MAP) {
                            Value pval = val_nil();
                            val_map_get(frame->cur, prop_name.as.slice, &pval);
                            val_println(pval, stdout);
                            fflush(stdout);
                            break;
                        } else if (frame->acc.type == VAL_MAP) {
                            Value pval = val_nil();
                            val_map_get(frame->acc, prop_name.as.slice, &pval);
                            val_println(pval, stdout);
                            fflush(stdout);
                            break;
                        } else if (frame->acc.type == VAL_LIST) {
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
                            fflush(stdout);
                            break;
                        }
                    }
                }

                if (frame->cur.type == VAL_MAP && inst.scanner != 0 &&
                    inst.scanner != TOK_SCAN_LINE && inst.scanner != TOK_SCAN_LINES &&
                    inst.scanner != TOK_SCAN_WORD && inst.scanner != TOK_SCAN_WORDS &&
                    inst.scanner != TOK_SCAN_NUMBER && inst.scanner != TOK_SCAN_NUMBERS) {
                    const char *mkey = NULL;
                    if (inst.scanner == TOK_SCAN_HOST || inst.scanner == TOK_SCAN_HOSTS) mkey = "host";
                    else if (inst.scanner == TOK_SCAN_PORT || inst.scanner == TOK_SCAN_PORTS) mkey = "port";
                    else if (inst.scanner == TOK_SCAN_FILE || inst.scanner == TOK_SCAN_FILES) mkey = "file";
                    else if (inst.scanner == TOK_SCAN_STATUS) mkey = "state";
                    else if (inst.scanner == TOK_SCAN_BRANCHES) mkey = "name";
                    else if (inst.scanner == TOK_SCAN_COMMITS) mkey = "message";
                    if (mkey) {
                        Value pval = val_nil();
                        if (val_map_get_str(frame->cur, mkey, &pval)) {
                            val_println(pval, stdout);
                            fflush(stdout);
                            break;
                        }
                    }
                }

                if (inst.scanner == TOK_SCAN_LINE || inst.scanner == TOK_SCAN_LINES) {
                    if (to_print.type == VAL_SLICE) {
                        Value lines = split_lines_to_list(to_print.as.slice, &vm->iter_arena);
                        size_t len = val_list_len(lines);
                        if (ord == 999 && len > 0) {
                            to_print = val_list_get(lines, len - 1);
                        } else if (ord > 0 && (size_t)(ord - 1) < len) {
                            to_print = val_list_get(lines, (size_t)(ord - 1));
                        } else if (!has_ord && inst.operand != NO_CONST && inst.operand > 0) {
                            Value sub = val_list_create(&vm->iter_arena, inst.operand);
                            for (size_t i = 0; i < len && i < inst.operand; i++) {
                                val_list_append(&sub, val_list_get(lines, i), &vm->iter_arena);
                            }
                            to_print = join_list_with_delim(sub, "\n", &vm->iter_arena);
                        }
                        val_println(to_print, stdout);
                        fflush(stdout);
                        break;
                    }
                }

                if (to_print.type == VAL_LIST && ord > 0) {
                    size_t len = val_list_len(to_print);
                    size_t target_idx = (ord == 999) ? (len > 0 ? len - 1 : 0) : (size_t)(ord - 1);
                    if (target_idx < len) {
                        to_print = val_list_get(to_print, target_idx);
                    }
                } else if (ord > 0 && (inst.scanner == TOK_SCAN_WORD || inst.scanner == TOK_SCAN_WORDS ||
                           inst.scanner == TOK_SCAN_NUMBER || inst.scanner == TOK_SCAN_NUMBERS || inst.scanner == 0)) {
                    to_print = (ord == 999) ? vm_get_ordinal_field(vm, frame, -1) : vm_get_ordinal_field(vm, frame, ord - 1);
                }

                val_println(to_print, stdout);
                fflush(stdout);
                break;
            }

            case OP_VERB_TAKE:
            case OP_VERB_FILTER: {
                bool has_ord = (inst.slot_mask & SLOT_HAS_ORD) != 0;
                int ord = has_ord ? (int)(inst.adverbs & 0xFFF) : 0;
                bool casefold = (inst.adverbs & ADV_CASEFOLD) != 0;

                if (inst.scanner == TOK_SCAN_BYTE || inst.scanner == TOK_SCAN_BYTES) {
                    size_t n = (inst.operand != NO_CONST && inst.operand > 0) ? inst.operand : 8;
                    if (frame->acc.type == VAL_SLICE) {
                        size_t take_len = frame->acc.as.slice.len < n ? frame->acc.as.slice.len : n;
                        frame->acc = val_slice(frame->acc.as.slice.ptr, take_len);
                    }
                    break;
                }

                if (inst.scanner == TOK_SCAN_LINE || inst.scanner == TOK_SCAN_LINES) {
                    Value src = (frame->acc.type != VAL_NIL) ? frame->acc : frame->cur;
                    bool from_slice = (src.type == VAL_SLICE);
                    Value lines = from_slice ? split_lines_to_list(src.as.slice, &vm->iter_arena) : src;
                    if (lines.type == VAL_LIST) {
                        size_t len = val_list_len(lines);
                        if (has_ord && ord != 500) {
                            size_t idx = (ord == 999) ? (len > 0 ? len - 1 : 0) : (size_t)(ord - 1);
                            frame->acc = (idx < len) ? val_list_get(lines, idx) : val_slice("", 0);
                        } else {
                            size_t n = (inst.operand != NO_CONST && inst.operand > 0) ? inst.operand : 1;
                            Value out = val_list_create(&vm->iter_arena, n);
                            for (size_t i = 0; i < len && i < n; i++) {
                                val_list_append(&out, val_list_get(lines, i), &vm->iter_arena);
                            }
                            frame->acc = from_slice ? join_list_with_delim(out, "\n", &vm->iter_arena) : out;
                        }
                    }
                    break;
                }

                if (has_ord) {
                    if (frame->acc.type == VAL_LIST && inst.scanner == 0) {
                        size_t len = val_list_len(frame->acc);
                        size_t idx = (ord == 999) ? (len > 0 ? len - 1 : 0) : (size_t)(ord - 1);
                        frame->acc = (idx < len) ? val_list_get(frame->acc, idx) : val_nil();
                    } else {
                        if (frame->acc.type == VAL_SLICE && frame->cur.type == VAL_NIL) {
                            frame->cur = frame->acc;
                            frame->fld_split = false;
                        }
                        frame->acc = (ord == 999) ? vm_get_ordinal_field(vm, frame, -1) : vm_get_ordinal_field(vm, frame, ord - 1);
                    }
                    break;
                }

                if ((inst.slot_mask & PREP_MATCHING) && inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    Value pat_val = vm->constants[inst.operand];
                    if (pat_val.type == VAL_SLICE) {
                        Slice pat = pat_val.as.slice;
                        Value src = (frame->acc.type != VAL_NIL) ? frame->acc : frame->cur;
                        if (src.type == VAL_SLICE) {
                            Value lines = split_lines_to_list(src.as.slice, &vm->iter_arena);
                            size_t len = val_list_len(lines);
                            Value out = val_list_create(&vm->iter_arena, len);
                            for (size_t i = 0; i < len; i++) {
                                Value lv = val_list_get(lines, i);
                                if (find_substr(lv.as.slice.ptr, lv.as.slice.len, pat.ptr, pat.len, casefold)) {
                                    val_list_append(&out, lv, &vm->iter_arena);
                                }
                            }
                            frame->acc = join_list_with_delim(out, "\n", &vm->iter_arena);
                        } else if (src.type == VAL_LIST) {
                            size_t len = val_list_len(src);
                            Value out = val_list_create(&vm->iter_arena, len);
                            for (size_t i = 0; i < len; i++) {
                                Value item = val_list_get(src, i);
                                char *s = val_to_string(item, &vm->iter_arena);
                                if (find_substr(s, strlen(s), pat.ptr, pat.len, casefold)) {
                                    val_list_append(&out, item, &vm->iter_arena);
                                }
                            }
                            frame->acc = out;
                        }
                    }
                    break;
                }

                if (inst.scanner == TOK_SCAN_NUMBERS || inst.scanner == TOK_SCAN_NUMBER ||
                    inst.scanner == TOK_SCAN_WORDS || inst.scanner == TOK_SCAN_WORD ||
                    inst.scanner == TOK_SCAN_URLS || inst.scanner == TOK_SCAN_EMAILS) {
                    Value src = (frame->acc.type != VAL_NIL) ? frame->acc : frame->cur;
                    frame->acc = scanner_extract_all((OttoTokenType)inst.scanner, src, &vm->iter_arena);
                }
                break;
            }

            case OP_VERB_DROP: {
                bool has_ord = (inst.slot_mask & SLOT_HAS_ORD) != 0;
                int ord = has_ord ? (int)(inst.adverbs & 0xFFF) : 0;
                bool casefold = (inst.adverbs & ADV_CASEFOLD) != 0;

                if (inst.scanner == TOK_SCAN_BYTE || inst.scanner == TOK_SCAN_BYTES) {
                    size_t n = (inst.operand != NO_CONST && inst.operand > 0) ? inst.operand : 8;
                    if (frame->acc.type == VAL_SLICE) {
                        if (n >= frame->acc.as.slice.len) frame->acc = val_slice("", 0);
                        else frame->acc = val_slice(frame->acc.as.slice.ptr + n, frame->acc.as.slice.len - n);
                    }
                    break;
                }

                if (inst.scanner == TOK_SCAN_LINE || inst.scanner == TOK_SCAN_LINES) {
                    Value src = (frame->acc.type != VAL_NIL) ? frame->acc : frame->cur;
                    bool from_slice = (src.type == VAL_SLICE);
                    Value lines = from_slice ? split_lines_to_list(src.as.slice, &vm->iter_arena) : src;
                    if (lines.type == VAL_LIST) {
                        size_t len = val_list_len(lines);
                        Value out = val_list_create(&vm->iter_arena, len);
                        if (has_ord && ord == 999) {
                            for (size_t i = 0; i + 1 < len; i++) {
                                val_list_append(&out, val_list_get(lines, i), &vm->iter_arena);
                            }
                        } else {
                            size_t n = has_ord ? (size_t)ord : ((inst.operand != NO_CONST && inst.operand > 0) ? inst.operand : 1);
                            for (size_t i = n; i < len; i++) {
                                val_list_append(&out, val_list_get(lines, i), &vm->iter_arena);
                            }
                        }
                        frame->acc = from_slice ? join_list_with_delim(out, "\n", &vm->iter_arena) : out;
                    }
                    break;
                }

                if (has_ord && frame->acc.type == VAL_LIST) {
                    size_t len = val_list_len(frame->acc);
                    Value out = val_list_create(&vm->iter_arena, len);
                    size_t skip_idx = (ord == 999) ? (len > 0 ? len - 1 : 0) : (size_t)(ord - 1);
                    for (size_t i = 0; i < len; i++) {
                        if (i != skip_idx) val_list_append(&out, val_list_get(frame->acc, i), &vm->iter_arena);
                    }
                    frame->acc = out;
                    break;
                }

                if ((inst.slot_mask & PREP_MATCHING) && inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    Value pat_val = vm->constants[inst.operand];
                    if (pat_val.type == VAL_SLICE) {
                        Slice pat = pat_val.as.slice;
                        Value src = (frame->acc.type != VAL_NIL) ? frame->acc : frame->cur;
                        if (src.type == VAL_SLICE) {
                            Value lines = split_lines_to_list(src.as.slice, &vm->iter_arena);
                            size_t len = val_list_len(lines);
                            Value out = val_list_create(&vm->iter_arena, len);
                            for (size_t i = 0; i < len; i++) {
                                Value lv = val_list_get(lines, i);
                                if (!find_substr(lv.as.slice.ptr, lv.as.slice.len, pat.ptr, pat.len, casefold)) {
                                    val_list_append(&out, lv, &vm->iter_arena);
                                }
                            }
                            frame->acc = join_list_with_delim(out, "\n", &vm->iter_arena);
                        } else if (src.type == VAL_LIST) {
                            size_t len = val_list_len(src);
                            Value out = val_list_create(&vm->iter_arena, len);
                            for (size_t i = 0; i < len; i++) {
                                Value item = val_list_get(src, i);
                                char *s = val_to_string(item, &vm->iter_arena);
                                if (!find_substr(s, strlen(s), pat.ptr, pat.len, casefold)) {
                                    val_list_append(&out, item, &vm->iter_arena);
                                }
                            }
                            frame->acc = out;
                        }
                    }
                }
                break;
            }

            case OP_VERB_SPLIT: {
                const char *delim = " ";
                if (inst.operand != NO_CONST && inst.operand < vm->const_count) {
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
                        } else if (dlen > 0 && i + dlen <= s.len && memcmp(s.ptr + i, delim, dlen) == 0) {
                            is_match = true;
                        } else if (i == s.len) {
                            is_match = true;
                        }
                        if (is_match) {
                            size_t token_len = i - start;
                            if (token_len > 0 || (dlen > 0 && !(dlen == 1 && delim[0] == ' '))) {
                                val_list_append(&split_list, val_slice(s.ptr + start, token_len), &vm->iter_arena);
                            }
                            size_t step = (dlen > 0) ? dlen : 1;
                            start = i + step;
                            if (step > 1 && i < s.len) i += step - 1;
                        }
                    }
                    frame->acc = split_list;
                    frame->fld = split_list;
                    frame->fld_split = true;
                }
                break;
            }

            case OP_VERB_JOIN: {
                const char *delim = " ";
                if (inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    Value cval = vm->constants[inst.operand];
                    if (cval.type == VAL_SLICE) delim = slice_to_cstr(cval.as.slice, &vm->iter_arena);
                }
                Value src = (frame->acc.type == VAL_LIST) ? frame->acc : frame->fld;
                if (src.type == VAL_LIST) {
                    frame->acc = join_list_with_delim(src, delim, &vm->iter_arena);
                }
                break;
            }

            case OP_VERB_SORT: {
                bool has_key = (inst.slot_mask & PREP_BY) != 0 && inst.operand != NO_CONST && inst.operand < vm->const_count;
                Value sort_key = has_key ? vm->constants[inst.operand] : val_nil();

                if (frame->acc.type != VAL_LIST && has_key && sort_key.type == VAL_SLICE) {
                    Value sym_val = val_nil();
                    if (val_map_get(vm->global_symbols, sort_key.as.slice, &sym_val) && sym_val.type == VAL_LIST) {
                        frame->acc = val_clone_to_arena(sym_val, &vm->iter_arena);
                        has_key = false;
                    }
                }

                bool from_slice = false;
                if (frame->acc.type == VAL_SLICE) {
                    from_slice = true;
                    frame->acc = split_lines_to_list(frame->acc.as.slice, &vm->iter_arena);
                }

                if (frame->acc.type == VAL_LIST) {
                    Value list = frame->acc;
                    size_t count = val_list_len(list);
                    bool desc = (inst.adverbs & ADV_DESCENDING) != 0;
                    bool numeric = (inst.adverbs & ADV_NUMERICALLY) != 0;
                    bool casefold = (inst.adverbs & ADV_CASEFOLD) != 0;
                    bool unique = (inst.adverbs & ADV_UNIQUELY) != 0;

                    for (size_t i = 0; i < count; i++) {
                        for (size_t j = i + 1; j < count; j++) {
                            Value a = val_list_get(list, i);
                            Value b = val_list_get(list, j);
                            Value va = a, vb = b;
                            if (has_key && sort_key.type == VAL_SLICE) {
                                if (a.type == VAL_MAP) val_map_get(a, sort_key.as.slice, &va);
                                if (b.type == VAL_MAP) val_map_get(b, sort_key.as.slice, &vb);
                            }
                            int cmp = val_compare_ex(va, vb, numeric, casefold);
                            if ((desc && cmp < 0) || (!desc && cmp > 0)) {
                                val_list_set(&list, i, b);
                                val_list_set(&list, j, a);
                            }
                        }
                    }

                    if (unique && count > 1) {
                        Value dedup = val_list_create(&vm->iter_arena, count);
                        for (size_t i = 0; i < count; i++) {
                            Value cur_v = val_list_get(list, i);
                            if (i > 0) {
                                Value prev_v = val_list_get(list, i - 1);
                                Value va = prev_v, vb = cur_v;
                                if (has_key && sort_key.type == VAL_SLICE) {
                                    if (prev_v.type == VAL_MAP) val_map_get(prev_v, sort_key.as.slice, &va);
                                    if (cur_v.type == VAL_MAP) val_map_get(cur_v, sort_key.as.slice, &vb);
                                }
                                if (val_compare_ex(va, vb, numeric, casefold) == 0) continue;
                            }
                            val_list_append(&dedup, cur_v, &vm->iter_arena);
                        }
                        list = dedup;
                    }

                    frame->acc = from_slice ? join_list_with_delim(list, "\n", &vm->iter_arena) : list;
                }
                break;
            }

            case OP_VERB_REPLACE: {
                const char *old_str = "";
                const char *new_str = "";
                if (inst.slot_mask != NO_CONST && inst.slot_mask < vm->const_count) {
                    Value oval = vm->constants[inst.slot_mask];
                    if (oval.type == VAL_SLICE) old_str = slice_to_cstr(oval.as.slice, &vm->iter_arena);
                }
                if (inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    Value nval = vm->constants[inst.operand];
                    if (nval.type == VAL_SLICE) new_str = slice_to_cstr(nval.as.slice, &vm->iter_arena);
                }
                bool casefold = (inst.adverbs & ADV_CASEFOLD) != 0;
                bool file_mode = (inst.adverbs & (ADV_INPLACE | ADV_SIMULATE)) != 0 || vm->simulate_mode;

                if (file_mode) {
                    Value target_file = (frame->cur.type == VAL_SLICE) ? frame->cur : frame->acc;
                    if (target_file.type != VAL_SLICE) break;
                    char *target_str = slice_to_cstr(target_file.as.slice, &vm->iter_arena);
                    FILE *fp = fopen(target_str, "rb");
                    if (!fp) {
                        frame->err = val_cstr("replace: file not found");
                        break;
                    }
                    fseek(fp, 0, SEEK_END);
                    long fsize = ftell(fp);
                    if (fsize < 0) fsize = 0;
                    fseek(fp, 0, SEEK_SET);
                    char *old_buf = (char *)arena_alloc(&vm->iter_arena, (size_t)fsize + 1);
                    size_t nread = fread(old_buf, 1, (size_t)fsize, fp);
                    old_buf[nread] = '\0';
                    fclose(fp);

                    Value replaced = replace_all_slice((Slice){old_buf, nread}, old_str, new_str, casefold, &vm->iter_arena);
                    if ((inst.adverbs & ADV_SIMULATE) || vm->simulate_mode) {
                        journal_simulate_diff(target_str, old_buf, nread, replaced.as.slice.ptr, replaced.as.slice.len);
                    } else {
                        journal_backup_file(target_str);
                        FILE *out = fopen(target_str, "wb");
                        if (out) {
                            fwrite(replaced.as.slice.ptr, 1, replaced.as.slice.len, out);
                            fclose(out);
                        }
                    }
                    frame->acc = replaced;
                } else {
                    Value src = (frame->acc.type == VAL_SLICE) ? frame->acc : frame->cur;
                    if (src.type == VAL_SLICE) {
                        frame->acc = replace_all_slice(src.as.slice, old_str, new_str, casefold, &vm->iter_arena);
                    }
                }
                break;
            }

            case OP_VERB_ADD: {
                bool has_ord = (inst.slot_mask & SLOT_HAS_ORD) != 0;
                int ordinal = has_ord ? (int)(inst.adverbs & 0xFFF) : 0;
                Value to_add = frame->acc;
                if (has_ord && ordinal > 0) {
                    to_add = (ordinal == 999) ? vm_get_ordinal_field(vm, frame, -1) : vm_get_ordinal_field(vm, frame, ordinal - 1);
                }

                if (inst.slot_mask & PREP_WITH) {
                    Value left = pop();
                    frame->acc = add_numeric_values(left, to_add);
                } else if (inst.slot_mask & PREP_INTO) {
                    if (inst.operand == 0xFFFE) {
                        Value sum = add_numeric_values(vm->that_val, to_add);
                        vm->that_val = val_clone_to_arena(sum, &vm->perm_arena);
                        frame->acc = sum;
                    } else if (inst.operand != NO_CONST && inst.operand < vm->const_count) {
                        Value var_name = vm->constants[inst.operand];
                        Value curr = val_int(0);
                        val_map_get(vm->global_symbols, var_name.as.slice, &curr);
                        Value sum = add_numeric_values(curr, to_add);
                        val_map_set(&vm->global_symbols, var_name.as.slice, sum, &vm->perm_arena);
                        frame->acc = sum;
                    }
                }
                break;
            }

            case OP_VERB_COUNT: {
                Value src = (frame->acc.type != VAL_NIL) ? frame->acc : frame->cur;
                if (src.type == VAL_LIST) {
                    frame->acc = val_int((int64_t)val_list_len(src));
                } else if (src.type == VAL_MAP) {
                    frame->acc = val_int((int64_t)(src.as.map ? src.as.map->count : 0));
                } else if (src.type == VAL_SLICE) {
                    size_t lines = 0;
                    Slice s = src.as.slice;
                    for (size_t i = 0; i < s.len; i++) if (s.ptr[i] == '\n') lines++;
                    if (s.len > 0 && s.ptr[s.len - 1] != '\n') lines++;
                    frame->acc = val_int((int64_t)lines);
                } else {
                    frame->acc = val_int(0);
                }
                break;
            }

            case OP_VERB_RUN: {
                Value host_val = (inst.slot_mask & PREP_AT) ? pop() : val_nil();
                char *cmd = NULL;
                Value stdin_val = val_nil();

                if (inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    Value cval = vm->constants[inst.operand];
                    if (cval.type == VAL_SLICE) cmd = slice_to_cstr(cval.as.slice, &vm->iter_arena);
                    if (frame->acc.type == VAL_SLICE) stdin_val = frame->acc;
                } else if (frame->acc.type == VAL_SLICE) {
                    cmd = slice_to_cstr(frame->acc.as.slice, &vm->iter_arena);
                }

                if (cmd) {
                    if (inst.slot_mask & PREP_AT) {
                        char *host_str = val_to_string(host_val, &vm->iter_arena);
                        Value out_val = val_nil(), out_err = val_nil();
                        ssh_execute_remote(host_str, cmd, stdin_val, &out_val, &out_err, &vm->iter_arena);
                        frame->acc = out_val;
                        frame->err = out_err;
                        break;
                    }

                    // Check registered C ABI extern functions first
                    {
                        const char *p = cmd;
                        while (*p && isspace((unsigned char)*p)) p++;
                        const char *w_end = p;
                        while (*w_end && !isspace((unsigned char)*w_end)) w_end++;
                        char *verb_name = arena_strndup(&vm->iter_arena, p, (size_t)(w_end - p));
                        const char *rest = w_end;
                        while (*rest && isspace((unsigned char)*rest)) rest++;
                        Value ffi_arg = stdin_val;
                        if (*rest) {
                            size_t rlen = strlen(rest);
                            if (rlen >= 2 && rest[0] == '"' && rest[rlen - 1] == '"') {
                                ffi_arg = val_slice(rest + 1, rlen - 2);
                            } else {
                                bool ok = false;
                                int64_t iv = slice_to_i64((Slice){rest, rlen}, &ok);
                                if (ok) ffi_arg = val_int(iv);
                                else {
                                    double dv = slice_to_f64((Slice){rest, rlen}, &ok);
                                    ffi_arg = ok ? val_float(dv) : val_slice(rest, rlen);
                                }
                            }
                        }
                        Value ffi_out = val_nil();
                        if (ffi_invoke(verb_name, ffi_arg, &ffi_out, &vm->iter_arena)) {
                            frame->acc = ffi_out;
                            break;
                        }
                    }

                    // Check live shell abbreviations (`abbr`)
                    {
                        char abbr_exp[1024];
                        if (shell_abbr_expand_line(cmd, abbr_exp, sizeof(abbr_exp) - 4)) {
                            size_t elen = strlen(abbr_exp);
                            if (elen > 0 && abbr_exp[elen - 1] != '.') {
                                abbr_exp[elen++] = '.';
                                abbr_exp[elen] = '\0';
                            }
                            Parser ap;
                            parser_init(&ap, abbr_exp, elen, false, &vm->perm_arena);
                            ASTNode *aprog = parser_parse_program(&ap);
                            if (!ap.has_error && aprog) {
                                BytecodeEmitter abe;
                                bc_init(&abe, &vm->perm_arena);
                                if (bc_compile_program(&abe, aprog)) {
                                    Instruction *sc = vm->code;
                                    size_t sl = vm->code_len;
                                    Value *sk = vm->constants;
                                    size_t skc = vm->const_count;
                                    vm->code = abe.code;
                                    vm->code_len = abe.count;
                                    vm->constants = abe.constants;
                                    vm->const_count = abe.const_count;
                                    vm_run(vm);
                                    frame = &vm->frames[vm->frame_top];
                                    vm->code = sc;
                                    vm->code_len = sl;
                                    vm->constants = sk;
                                    vm->const_count = skc;
                                    break;
                                }
                            }
                        }
                    }

                    struct stat st;
                    if (stat(cmd, &st) == 0 && S_ISDIR(st.st_mode)) {
                        shell_cd(cmd);
                        frame->acc = val_cstr(arena_strdup(&vm->iter_arena, cmd));
                        break;
                    }
                    if ((inst.adverbs & (ADV_BACKGROUND | ADV_CONCURRENTLY)) != 0) {
                        int pid = 0;
                        void *handle = NULL;
                        if (posix_spawn_background(cmd, &pid, &handle)) {
                            int jid = shell_add_job(pid, cmd, handle);
                            Value jmap = val_map_create(&vm->iter_arena, 4);
                            val_map_set_str(&jmap, "id", val_int(jid), &vm->iter_arena);
                            val_map_set_str(&jmap, "pid", val_int(pid), &vm->iter_arena);
                            val_map_set_str(&jmap, "status", val_cstr("Running"), &vm->iter_arena);
                            val_map_set_str(&jmap, "command", val_cstr(cmd), &vm->iter_arena);
                            frame->acc = jmap;
                            printf("[Job %d started: PID %d] %s\n", jid, pid, cmd);
                            fflush(stdout);
                        } else {
                            frame->err = val_cstr("Failed to spawn background job");
                        }
                        break;
                    }
                    Value out_val = val_nil();
                    Value out_err = val_nil();
                    bool cmd_ok = posix_run_command(cmd, stdin_val, &out_val, &out_err, &vm->iter_arena);
                    frame->acc = out_val;
                    frame->err = out_err;
                    if (!cmd_ok) {
                        const char *p = cmd;
                        while (*p && isspace((unsigned char)*p)) p++;
                        const char *we = p;
                        while (*we && !isspace((unsigned char)*we) && *we != '.') we++;
                        size_t wlen = (size_t)(we - p);
                        const char *sug = help_suggest_keyword(p, wlen);
                        const char *ros = help_rosetta_tip(p);
                        if (sug) {
                            fprintf(stderr, "\033[1;31mUnknown command '%.*s'.\033[0m Did you mean '\033[1;32m%s\033[0m'?\n",
                                    (int)wlen, p, sug);
                        } else if (ros) {
                            fprintf(stderr, "\033[1;36mTip:\033[0m %s\n", ros);
                        }
                    }
                }
                break;
            }

            case OP_VERB_COMPILE: {
                char *script_path = (frame->acc.type == VAL_SLICE) ? slice_to_cstr(frame->acc.as.slice, &vm->iter_arena) : NULL;
                char *out_bin = "./test-bin";
                if (inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    Value oval = vm->constants[inst.operand];
                    if (oval.type == VAL_SLICE) out_bin = slice_to_cstr(oval.as.slice, &vm->iter_arena);
                }

                char self_exe[512] = "";
#ifdef _WIN32
                GetModuleFileNameA(NULL, self_exe, sizeof(self_exe));
#elif defined(__APPLE__)
                uint32_t bsize = (uint32_t)sizeof(self_exe);
                _NSGetExecutablePath(self_exe, &bsize);
#else
                ssize_t r = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
                if (r > 0) self_exe[r] = '\0';
#endif
                if (!bundle_compile_script(script_path, out_bin, self_exe)) {
                    frame->err = val_cstr("compile: failed to bundle binary");
                }
                break;
            }

            case OP_VERB_UNDO:
                journal_undo_last();
                break;

            case OP_VERB_PICK: {
                Value pick_src = frame->acc;
                if (pick_src.type == VAL_SLICE) {
                    pick_src = split_lines_to_list(pick_src.as.slice, &vm->iter_arena);
                } else if (pick_src.type == VAL_NIL && stream_sp > 0) {
                    StreamDesc *sd = stream_stack[stream_sp - 1];
                    pick_src = val_list_create(&vm->perm_arena, 32);
                    if (frame->cur.type != VAL_NIL) {
                        val_list_append(&pick_src, val_clone_to_arena(frame->cur, &vm->perm_arena), &vm->perm_arena);
                    }
                    Value rem = val_nil();
                    while (sd && sd->next && sd->next(sd, &rem, &vm->perm_arena)) {
                        val_list_append(&pick_src, rem, &vm->perm_arena);
                    }
                    if (sd) {
                        if (sd->close) sd->close(sd);
                        sd->close = NULL;
                        sd->next = NULL;
                    }
                }
                frame->acc = tui_pick(pick_src, &vm->iter_arena);
                break;
            }

            case OP_VERB_TABLE:
                tui_table(frame->acc, stdout);
                break;

            case OP_VERB_CHART:
                tui_chart(frame->acc, "bar", stdout);
                break;

            case OP_VERB_UNPACK: {
                Value byte_slice = frame->acc;
                const char *fmt = "u32 u8 u8";
                if (inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    Value fval = vm->constants[inst.operand];
                    if (fval.type == VAL_SLICE) fmt = slice_to_cstr(fval.as.slice, &vm->iter_arena);
                }
                Value out_vals[16];
                size_t out_count = 0;
                posix_unpack_bytes(byte_slice, fmt, out_vals, 16, &out_count);

                if (inst.slot_mask != NO_CONST && inst.slot_mask < vm->const_count &&
                    vm->constants[inst.slot_mask].type == VAL_SLICE) {
                    Slice names = vm->constants[inst.slot_mask].as.slice;
                    size_t idx = 0;
                    size_t p = 0;
                    while (p < names.len && idx < out_count) {
                        while (p < names.len && names.ptr[p] == ' ') p++;
                        size_t start = p;
                        while (p < names.len && names.ptr[p] != ' ') p++;
                        if (p > start) {
                            char *k = arena_strndup(&vm->perm_arena, names.ptr + start, p - start);
                            val_map_set(&vm->global_symbols, (Slice){k, p - start},
                                        val_clone_to_arena(out_vals[idx], &vm->perm_arena), &vm->perm_arena);
                            idx++;
                        }
                    }
                } else {
                    Value list = val_list_create(&vm->iter_arena, out_count);
                    for (size_t i = 0; i < out_count; i++) {
                        val_list_append(&list, out_vals[i], &vm->iter_arena);
                    }
                    frame->acc = list;
                }
                break;
            }

            case OP_VERB_INTERSECT: {
                Value with_val = val_nil();
                if (inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    with_val = vm->constants[inst.operand];
                    if (with_val.type == VAL_SLICE) {
                        struct stat st;
                        char *cstr = slice_to_cstr(with_val.as.slice, &vm->iter_arena);
                        if (stat(cstr, &st) != 0) {
                            Value sym = val_nil();
                            if (val_map_get(vm->global_symbols, with_val.as.slice, &sym)) with_val = sym;
                        }
                    }
                }
                frame->acc = posix_set_intersect(frame->acc, with_val, &vm->iter_arena);
                break;
            }

            case OP_VERB_EXCLUDE: {
                Value from_val = val_nil();
                if (inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    from_val = vm->constants[inst.operand];
                    if (from_val.type == VAL_SLICE) {
                        struct stat st;
                        char *cstr = slice_to_cstr(from_val.as.slice, &vm->iter_arena);
                        if (stat(cstr, &st) != 0) {
                            Value sym = val_nil();
                            if (val_map_get(vm->global_symbols, from_val.as.slice, &sym)) from_val = sym;
                        }
                    }
                }
                frame->acc = posix_set_exclude(frame->acc, from_val, &vm->iter_arena);
                break;
            }

            case OP_VERB_ZIP: {
                Value with_val = val_nil();
                if (inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    with_val = vm->constants[inst.operand];
                    if (with_val.type == VAL_SLICE) {
                        struct stat st;
                        char *cstr = slice_to_cstr(with_val.as.slice, &vm->iter_arena);
                        if (stat(cstr, &st) != 0) {
                            Value sym = val_nil();
                            if (val_map_get(vm->global_symbols, with_val.as.slice, &sym)) with_val = sym;
                        }
                    }
                }
                frame->acc = posix_set_zip(frame->acc, with_val, &vm->iter_arena);
                break;
            }

            case OP_VERB_REMOVE: {
                Value target = (frame->acc.type == VAL_SLICE) ? frame->acc : frame->cur;
                if (target.type != VAL_SLICE) break;
                char *path = slice_to_cstr(target.as.slice, &vm->iter_arena);
                struct stat st;
                if (stat(path, &st) != 0) {
                    frame->err = val_cstr("remove: file not found");
                    break;
                }
                if ((inst.adverbs & ADV_SIMULATE) || vm->simulate_mode) {
                    journal_simulate_diff(path, "<existing file>", 15, "", 0);
                    frame->acc = val_bool(true);
                } else {
                    if (!S_ISDIR(st.st_mode)) {
                        journal_backup_file(path);
                        bool ok = (remove(path) == 0);
                        frame->acc = val_bool(ok);
                        if (!ok) frame->err = val_cstr("remove: failed to delete file");
                    } else {
#ifdef _WIN32
                        bool ok = (_rmdir(path) == 0);
#else
                        bool ok = (rmdir(path) == 0);
#endif
                        frame->acc = val_bool(ok);
                        if (!ok) frame->err = val_cstr("remove: failed to remove directory");
                    }
                }
                break;
            }

            case OP_VERB_COPY:
            case OP_VERB_MOVE: {
                Value src_val = (frame->acc.type == VAL_SLICE) ? frame->acc : frame->cur;
                if (src_val.type != VAL_SLICE || inst.operand == NO_CONST || inst.operand >= vm->const_count) {
                    frame->err = val_cstr("copy/move: missing source or destination");
                    break;
                }
                char *src = slice_to_cstr(src_val.as.slice, &vm->iter_arena);
                char *dst = val_to_string(vm->constants[inst.operand], &vm->iter_arena);
                if ((inst.adverbs & ADV_SIMULATE) || vm->simulate_mode) {
                    journal_simulate_diff(dst, "", 0, src, strlen(src));
                    frame->acc = val_cstr(dst);
                } else {
                    if (op == OP_VERB_MOVE) journal_backup_file(src);
                    journal_backup_file(dst);
                    bool ok = false;
                    if (op == OP_VERB_COPY) {
                        ok = copy_file_bytes(src, dst);
                    } else {
                        if (rename(src, dst) == 0) ok = true;
                        else if (copy_file_bytes(src, dst) && remove(src) == 0) ok = true;
                    }
                    if (ok) frame->acc = val_cstr(dst);
                    else frame->err = val_cstr("copy/move: operation failed");
                }
                break;
            }

            case OP_VERB_EXIT: {
                int code = (frame->acc.type == VAL_INT) ? (int)frame->acc.as.i64 : 0;
                journal_commit_tx();
                exit(code);
            }

            case OP_VERB_DIRS:
                frame->acc = shell_get_dirs(&vm->iter_arena);
                break;

            case OP_VERB_BACK:
                shell_back();
                break;

            case OP_VERB_CD: {
                char *path = (frame->acc.type == VAL_SLICE) ? slice_to_cstr(frame->acc.as.slice, &vm->iter_arena) : ".";
                shell_cd(path);
                break;
            }

            case OP_VERB_JOBS:
                frame->acc = shell_list_jobs(&vm->iter_arena);
                break;

            case OP_VERB_RESUME: {
                bool bg = (inst.adverbs & ADV_BACKGROUND) != 0;
                int job_id = (inst.operand != NO_CONST) ? (int)inst.operand : 0;
                if (job_id == 0 && frame->acc.type == VAL_INT) job_id = (int)frame->acc.as.i64;
                shell_resume_job(job_id, bg);
                break;
            }

            case OP_VERB_KILL: {
                int target_id = (inst.operand != NO_CONST) ? (int)inst.operand : 0;
                if (target_id == 0 && (frame->acc.type == VAL_MAP || frame->cur.type == VAL_MAP)) {
                    Value mobj = (frame->acc.type == VAL_MAP) ? frame->acc : frame->cur;
                    Value pid_val = val_nil();
                    if (val_map_get_str(mobj, "pid", &pid_val) && pid_val.type == VAL_INT) {
                        target_id = (int)pid_val.as.i64;
                    } else if (val_map_get_str(mobj, "id", &pid_val) && pid_val.type == VAL_INT) {
                        target_id = (int)pid_val.as.i64;
                    }
                } else if (target_id == 0 && frame->acc.type == VAL_INT) {
                    target_id = (int)frame->acc.as.i64;
                }
                if (target_id > 0) {
                    bool ok = shell_kill_job(target_id);
                    frame->acc = val_bool(ok);
                    if (!ok) {
#ifdef _WIN32
                        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)target_id);
                        if (h) {
                            TerminateProcess(h, 1);
                            CloseHandle(h);
                            frame->acc = val_bool(true);
                        }
#else
                        if (kill((pid_t)target_id, SIGTERM) == 0) {
                            frame->acc = val_bool(true);
                        }
#endif
                    }
                }
                break;
            }

            case OP_VERB_REPEAT: {
                const char *with_prefix = NULL;
                const char *replace_old = NULL;
                const char *replace_new = NULL;
                if ((inst.slot_mask & PREP_REPLACING) && (inst.slot_mask & PREP_WITH)) {
                    Value new_v = pop();
                    Value old_v = pop();
                    replace_old = val_to_string(old_v, &vm->iter_arena);
                    replace_new = val_to_string(new_v, &vm->iter_arena);
                } else if (inst.slot_mask & PREP_WITH) {
                    Value pref_v = pop();
                    with_prefix = val_to_string(pref_v, &vm->iter_arena);
                }

                Value repeated = shell_repeat_history(vm->previous_cmd, with_prefix, replace_old, replace_new, &vm->iter_arena);
                frame->acc = repeated;
                if (repeated.type == VAL_SLICE && repeated.as.slice.len > 0) {
                    size_t rlen = repeated.as.slice.len;
                    char *src_dot = (char *)arena_alloc(&vm->perm_arena, rlen + 3);
                    memcpy(src_dot, repeated.as.slice.ptr, rlen);
                    if (src_dot[rlen - 1] != '.') {
                        src_dot[rlen++] = '.';
                    }
                    src_dot[rlen] = '\0';

                    Parser rep_p;
                    parser_init(&rep_p, src_dot, rlen, false, &vm->perm_arena);
                    ASTNode *rep_prog = parser_parse_program(&rep_p);
                    if (!rep_p.has_error && rep_prog) {
                        BytecodeEmitter rep_be;
                        bc_init(&rep_be, &vm->perm_arena);
                        if (bc_compile_program(&rep_be, rep_prog)) {
                            Instruction *saved_code = vm->code;
                            size_t saved_len = vm->code_len;
                            Value *saved_consts = vm->constants;
                            size_t saved_cc = vm->const_count;
                            bool saved_fail = vm->has_on_failure;
                            uint16_t saved_fail_ip = vm->on_failure_ip;

                            vm->code = rep_be.code;
                            vm->code_len = rep_be.count;
                            vm->constants = rep_be.constants;
                            vm->const_count = rep_be.const_count;

                            vm_run(vm);
                            frame = &vm->frames[vm->frame_top];

                            vm->code = saved_code;
                            vm->code_len = saved_len;
                            vm->constants = saved_consts;
                            vm->const_count = saved_cc;
                            vm->has_on_failure = saved_fail;
                            vm->on_failure_ip = saved_fail_ip;
                        }
                    }
                }
                break;
            }

            case OP_VERB_FETCH: {
                char *url_str = (frame->acc.type == VAL_SLICE) ? slice_to_cstr(frame->acc.as.slice, &vm->iter_arena) : NULL;
                if (!url_str && inst.operand != NO_CONST && inst.operand < vm->const_count) {
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
                        char *cstr = (fetched.type == VAL_SLICE) ? (char *)fetched.as.slice.ptr : val_to_string(fetched, &vm->iter_arena);
                        size_t clen = (fetched.type == VAL_SLICE) ? fetched.as.slice.len : strlen(cstr);
                        bool ok = false;
                        frame->acc = json_parse(cstr, clen, &vm->iter_arena, &ok);
                    } else if (inst.scanner == TOK_SCAN_YAML) {
                        char *cstr = (fetched.type == VAL_SLICE) ? (char *)fetched.as.slice.ptr : val_to_string(fetched, &vm->iter_arena);
                        size_t clen = (fetched.type == VAL_SLICE) ? fetched.as.slice.len : strlen(cstr);
                        bool ok = false;
                        frame->acc = yaml_parse(cstr, clen, &vm->iter_arena, &ok);
                    } else if (inst.scanner == TOK_SCAN_TOML) {
                        char *cstr = (fetched.type == VAL_SLICE) ? (char *)fetched.as.slice.ptr : val_to_string(fetched, &vm->iter_arena);
                        size_t clen = (fetched.type == VAL_SLICE) ? fetched.as.slice.len : strlen(cstr);
                        bool ok = false;
                        frame->acc = toml_parse(cstr, clen, &vm->iter_arena, &ok);
                    } else if (inst.scanner == TOK_SCAN_CSV) {
                        char *cstr = (fetched.type == VAL_SLICE) ? (char *)fetched.as.slice.ptr : val_to_string(fetched, &vm->iter_arena);
                        size_t clen = (fetched.type == VAL_SLICE) ? fetched.as.slice.len : strlen(cstr);
                        frame->acc = csv_parse(cstr, clen, &vm->iter_arena);
                    } else if (inst.scanner == TOK_SCAN_JSONL) {
                        char *cstr = (fetched.type == VAL_SLICE) ? (char *)fetched.as.slice.ptr : val_to_string(fetched, &vm->iter_arena);
                        size_t clen = (fetched.type == VAL_SLICE) ? fetched.as.slice.len : strlen(cstr);
                        frame->acc = jsonl_parse(cstr, clen, &vm->iter_arena);
                    } else {
                        frame->acc = fetched;
                    }
                }
                break;
            }

            case OP_VERB_JUMP: {
                char *kw = (frame->acc.type == VAL_SLICE) ? slice_to_cstr(frame->acc.as.slice, &vm->iter_arena) : NULL;
                if (!kw && inst.operand != NO_CONST && inst.operand < vm->const_count) {
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
                if (!cmd && inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    cmd = val_to_string(vm->constants[inst.operand], &vm->iter_arena);
                }
                if (!cmd) {
                    frame->err = val_cstr("benchmark: missing command");
                    break;
                }

                double *times = (double *)malloc((size_t)runs * sizeof(double));
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
                    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0;
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
                    Value split_list = split_lines_to_list(src.as.slice, &vm->iter_arena);
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
                    if (k[0] == '\0') continue;
                    bool found = false;
                    for (size_t j = 0; j < tally_count; j++) {
                        if (strcmp(tallies[j].key, k) == 0) {
                            tallies[j].count++;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        snprintf(tallies[tally_count].key, sizeof(tallies[tally_count].key), "%s", k);
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
                if (inst.operand != NO_CONST && inst.operand < vm->const_count) {
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
                            snprintf(key_buf, sizeof(key_buf), "%s", s);
                        }
                    } else {
                        char *s = val_to_string(item, &vm->iter_arena);
                        snprintf(key_buf, sizeof(key_buf), "%s", s);
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

            case OP_VERB_SEARCH: {
                Value dir_val = (inst.slot_mask & PREP_IN) ? pop() : val_nil();
                const char *root_dir = ".";
                if (dir_val.type != VAL_NIL) {
                    root_dir = val_to_string(dir_val, &vm->iter_arena);
                }
                const char *pat = (frame->acc.type == VAL_SLICE)
                    ? slice_to_cstr(frame->acc.as.slice, &vm->iter_arena)
                    : val_to_string(frame->acc, &vm->iter_arena);
                frame->acc = dev_search(pat, root_dir, inst.adverbs, &vm->iter_arena);
                break;
            }

            case OP_VERB_STAT: {
                const char *path = ".";
                if (frame->acc.type == VAL_SLICE && frame->acc.as.slice.len > 0) {
                    path = slice_to_cstr(frame->acc.as.slice, &vm->iter_arena);
                } else if (inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    path = val_to_string(vm->constants[inst.operand], &vm->iter_arena);
                }
                Value res = dev_stat_path(path, inst.adverbs, &vm->iter_arena);
                if (res.type == VAL_NIL) frame->err = val_cstr("stat: path not found");
                frame->acc = res;
                break;
            }

            case OP_VERB_VIEW: {
                Value src = (frame->acc.type != VAL_NIL) ? frame->acc : frame->cur;
                frame->acc = dev_view(src, &vm->iter_arena);
                break;
            }

            case OP_VERB_HASH: {
                const char *algo = "sha256";
                if (inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    algo = val_to_string(vm->constants[inst.operand], &vm->iter_arena);
                }
                Value src = (frame->acc.type != VAL_NIL) ? frame->acc : frame->cur;
                frame->acc = dev_hash(src, algo, &vm->iter_arena);
                break;
            }

            case OP_VERB_ENCODE:
            case OP_VERB_DECODE: {
                const char *fmt = "base64";
                if (inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    fmt = val_to_string(vm->constants[inst.operand], &vm->iter_arena);
                }
                Value src = (frame->acc.type != VAL_NIL) ? frame->acc : frame->cur;
                frame->acc = dev_encode(src, fmt, (op == OP_VERB_DECODE), &vm->iter_arena);
                break;
            }

            case OP_VERB_UUID:
                frame->acc = dev_uuid(&vm->iter_arena);
                break;

            case OP_VERB_DISK: {
                const char *path = NULL;
                if (frame->acc.type == VAL_SLICE && frame->acc.as.slice.len > 0) {
                    path = slice_to_cstr(frame->acc.as.slice, &vm->iter_arena);
                } else if (inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    path = val_to_string(vm->constants[inst.operand], &vm->iter_arena);
                }
                frame->acc = dev_disk_usage(path, &vm->iter_arena);
                break;
            }

            case OP_VERB_ABBR: {
                if (inst.slot_mask != NO_CONST && inst.slot_mask < vm->const_count &&
                    inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    const char *sname = val_to_string(vm->constants[inst.slot_mask], &vm->iter_arena);
                    const char *exp = val_to_string(vm->constants[inst.operand], &vm->iter_arena);
                    shell_abbr_set(sname, exp);
                    frame->acc = val_cstr(arena_strdup(&vm->iter_arena, exp));
                } else if (inst.slot_mask != NO_CONST && inst.slot_mask < vm->const_count) {
                    const char *sname = val_to_string(vm->constants[inst.slot_mask], &vm->iter_arena);
                    const char *exp = shell_abbr_get(sname);
                    frame->acc = exp ? val_cstr(arena_strdup(&vm->iter_arena, exp)) : val_nil();
                } else {
                    frame->acc = shell_abbr_list(&vm->iter_arena);
                }
                break;
            }

            case OP_VERB_PREPEND:
            case OP_VERB_APPEND: {
                bool is_prep = (op == OP_VERB_PREPEND);
                Value item_val = frame->acc;
                if (inst.slot_mask & PREP_WITH) {
                    Value base_list = pop();
                    if (base_list.type == VAL_LIST) {
                        size_t len = val_list_len(base_list);
                        Value out = val_list_create(&vm->iter_arena, len + 1);
                        if (is_prep) val_list_append(&out, item_val, &vm->iter_arena);
                        for (size_t i = 0; i < len; i++) {
                            val_list_append(&out, val_list_get(base_list, i), &vm->iter_arena);
                        }
                        if (!is_prep) val_list_append(&out, item_val, &vm->iter_arena);
                        frame->acc = out;
                    }
                    break;
                }
                if ((inst.slot_mask & PREP_INTO) && inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    Value tgt = vm->constants[inst.operand];
                    if (tgt.type == VAL_SLICE &&
                        (slice_eq_str(tgt.as.slice, "$PATH") || slice_eq_str(tgt.as.slice, "$path"))) {
                        char *dir = val_to_string(item_val, &vm->iter_arena);
                        frame->acc = shell_path_modify(dir, is_prep, &vm->iter_arena);
                    } else if (tgt.type == VAL_SLICE) {
                        Value existing = val_nil();
                        if (!val_map_get(vm->global_symbols, tgt.as.slice, &existing) || existing.type != VAL_LIST) {
                            existing = val_list_create(&vm->perm_arena, 4);
                        }
                        size_t len = val_list_len(existing);
                        Value out = val_list_create(&vm->perm_arena, len + 1);
                        if (is_prep) val_list_append(&out, val_clone_to_arena(item_val, &vm->perm_arena), &vm->perm_arena);
                        for (size_t i = 0; i < len; i++) {
                            val_list_append(&out, val_list_get(existing, i), &vm->perm_arena);
                        }
                        if (!is_prep) val_list_append(&out, val_clone_to_arena(item_val, &vm->perm_arena), &vm->perm_arena);
                        val_map_set(&vm->global_symbols, tgt.as.slice, out, &vm->perm_arena);
                        frame->acc = out;
                    }
                }
                break;
            }

            case OP_VERB_HELP:
            case OP_VERB_EXAMPLES: {
                const char *topic = NULL;
                if (frame->acc.type == VAL_SLICE && frame->acc.as.slice.len > 0) {
                    topic = slice_to_cstr(frame->acc.as.slice, &vm->iter_arena);
                } else if (inst.operand != NO_CONST && inst.operand < vm->const_count) {
                    topic = val_to_string(vm->constants[inst.operand], &vm->iter_arena);
                }
                frame->acc = help_show(topic, (op == OP_VERB_EXAMPLES), &vm->iter_arena);
                break;
            }

            case OP_VERB_TUTOR:
                tutor_run(vm, tui_is_interactive());
                frame->acc = val_bool(true);
                break;

            case OP_WATCH_LOOP: {
                const char *path = ".";
                if (inst.slot_mask != NO_CONST && inst.slot_mask < vm->const_count) {
                    Value pval = vm->constants[inst.slot_mask];
                    path = val_to_string(pval, &vm->iter_arena);
                }

                if (!vm->watch_initialized) {
                    vm->watch_initialized = true;
                    vm->watch_last_mtime = get_watch_mtime(path);
                    break;
                }

                if (vm->simulate_mode || (inst.adverbs & ADV_SIMULATE) || getenv("OTTO_WATCH_ONCE") || g_sigint_received) {
                    ip = inst.operand;
                    break;
                }

                while (!g_sigint_received) {
                    sleep_millis(150);
                    uint64_t cur_t = get_watch_mtime(path);
                    if (cur_t > vm->watch_last_mtime) {
                        vm->watch_last_mtime = cur_t;
                        break;
                    }
                }
                if (g_sigint_received && !vm->has_on_interrupt) {
                    ip = inst.operand;
                }
                break;
            }

            case OP_INTERVAL_WAIT: {
                if (vm->simulate_mode || (inst.adverbs & ADV_SIMULATE) || getenv("OTTO_WATCH_ONCE") || g_sigint_received) {
                    ip = inst.operand;
                    break;
                }
                Slice iv_slice = {"1s", 2};
                if (inst.slot_mask != NO_CONST && inst.slot_mask < vm->const_count &&
                    vm->constants[inst.slot_mask].type == VAL_SLICE) {
                    iv_slice = vm->constants[inst.slot_mask].as.slice;
                }
                uint32_t total_ms = parse_interval_ms(iv_slice);
                uint32_t waited = 0;
                while (waited < total_ms && !g_sigint_received) {
                    uint32_t step = (total_ms - waited > 50) ? 50 : (total_ms - waited);
                    sleep_millis(step);
                    waited += step;
                }
                if (g_sigint_received && !vm->has_on_interrupt) {
                    ip = inst.operand;
                }
                break;
            }

            default:
                break;
        }

        // Trigger `on failure:` handler if an unhandled error occurred
        if (frame->err.type != VAL_NIL && vm->has_on_failure) {
            bool handled_by_or = (ip < vm->code_len && vm->code[ip].opcode == OP_JUMP_IF_OK);
            if (!handled_by_or && vm->frame_top + 1 < MAX_FRAMES) {
                Value err_val = frame->err;
                frame->err = val_nil();
                vm->has_on_failure = false;
                CallFrame *hframe = &vm->frames[++vm->frame_top];
                memset(hframe, 0, sizeof(CallFrame));
                hframe->err = err_val;
                hframe->ret_ip = (uint32_t)ip;
                frame = hframe;
                ip = vm->on_failure_ip;
            }
        }
    }

    vm->that_val = val_clone_to_arena(vm->frames[0].acc, &vm->perm_arena);
    journal_commit_tx();
}
