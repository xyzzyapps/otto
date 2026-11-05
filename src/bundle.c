#include "bundle.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

bool bundle_has_embedded_bytecode(const char *exe_path, uint8_t **out_bytecode, size_t *out_len) {
    if (!exe_path) return false;
    FILE *fp = fopen(exe_path, "rb");
    if (!fp) return false;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long file_size = ftell(fp);
    if (file_size < 16) {
        fclose(fp);
        return false;
    }

    // Check last 8 bytes for MAGIC
    fseek(fp, file_size - 8, SEEK_SET);
    uint64_t magic = 0;
    if (fread(&magic, 1, 8, fp) != 8 || magic != OTTO_MAGIC) {
        fclose(fp);
        return false;
    }

    // Read bytecode length (preceding 8 bytes)
    fseek(fp, file_size - 16, SEEK_SET);
    uint64_t bc_len = 0;
    if (fread(&bc_len, 1, 8, fp) != 8 || (long)bc_len > file_size - 16) {
        fclose(fp);
        return false;
    }

    // Seek and read bytecode
    fseek(fp, file_size - 16 - (long)bc_len, SEEK_SET);
    uint8_t *buffer = (uint8_t *)malloc((size_t)bc_len);
    if (!buffer || fread(buffer, 1, (size_t)bc_len, fp) != bc_len) {
        if (buffer) free(buffer);
        fclose(fp);
        return false;
    }

    fclose(fp);
    if (out_bytecode) *out_bytecode = buffer;
    if (out_len) *out_len = (size_t)bc_len;
    return true;
}

// Serialize bytecode emitter into dynamic buffer
static uint8_t *serialize_bytecode(BytecodeEmitter *be, size_t *out_len) {
    size_t cap = 1024 + be->count * sizeof(Instruction);
    size_t len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);

    // Header: num_instructions (u32), num_constants (u32)
    uint32_t num_inst = (uint32_t)be->count;
    uint32_t num_const = (uint32_t)be->const_count;

    memcpy(buf + len, &num_inst, 4); len += 4;
    memcpy(buf + len, &num_const, 4); len += 4;

    // Instructions
    size_t inst_bytes = be->count * sizeof(Instruction);
    memcpy(buf + len, be->code, inst_bytes);
    len += inst_bytes;

    // Constants
    for (size_t i = 0; i < be->const_count; i++) {
        Value c = be->constants[i];
        uint8_t type = (uint8_t)c.type;
        while (len + 32 + (c.type == VAL_SLICE ? c.as.slice.len : 0) >= cap) {
            cap *= 2;
            buf = (uint8_t *)realloc(buf, cap);
        }
        buf[len++] = type;
        if (type == VAL_INT) {
            memcpy(buf + len, &c.as.i64, 8); len += 8;
        } else if (type == VAL_FLOAT) {
            memcpy(buf + len, &c.as.f64, 8); len += 8;
        } else if (type == VAL_BOOL) {
            uint8_t b = c.as.i64 ? 1 : 0;
            buf[len++] = b;
        } else if (type == VAL_SLICE) {
            uint32_t slen = (uint32_t)c.as.slice.len;
            memcpy(buf + len, &slen, 4); len += 4;
            memcpy(buf + len, c.as.slice.ptr, slen); len += slen;
        } else if (type == VAL_FUNC) {
            memcpy(buf + len, &c.as.func.ip_offset, 4); len += 4;
            buf[len++] = c.as.func.arity;
            memcpy(buf + len, &c.as.func.slot_mask, 2); len += 2;
        }
    }

    *out_len = len;
    return buf;
}

bool bundle_load_into_vm(VM *vm, const uint8_t *data, size_t len) {
    if (len < 8) return false;
    size_t offset = 0;

    uint32_t num_inst = 0;
    uint32_t num_const = 0;
    memcpy(&num_inst, data + offset, 4); offset += 4;
    memcpy(&num_const, data + offset, 4); offset += 4;

    vm->code_len = num_inst;
    vm->code = (Instruction *)arena_alloc(&vm->perm_arena, num_inst * sizeof(Instruction));
    memcpy(vm->code, data + offset, num_inst * sizeof(Instruction));
    offset += num_inst * sizeof(Instruction);

    vm->const_count = num_const;
    vm->constants = (Value *)arena_alloc(&vm->perm_arena, (num_const + 8) * sizeof(Value));

    for (size_t i = 0; i < num_const; i++) {
        uint8_t type = data[offset++];
        Value c = val_nil();
        if (type == VAL_INT) {
            memcpy(&c.as.i64, data + offset, 8); offset += 8;
            c.type = VAL_INT;
        } else if (type == VAL_FLOAT) {
            memcpy(&c.as.f64, data + offset, 8); offset += 8;
            c.type = VAL_FLOAT;
        } else if (type == VAL_BOOL) {
            c.as.i64 = data[offset++];
            c.type = VAL_BOOL;
        } else if (type == VAL_SLICE) {
            uint32_t slen = 0;
            memcpy(&slen, data + offset, 4); offset += 4;
            char *s = arena_strndup(&vm->perm_arena, (const char *)data + offset, slen);
            offset += slen;
            c = val_slice(s, slen);
        } else if (type == VAL_FUNC) {
            c.type = VAL_FUNC;
            memcpy(&c.as.func.ip_offset, data + offset, 4); offset += 4;
            c.as.func.arity = data[offset++];
            memcpy(&c.as.func.slot_mask, data + offset, 2); offset += 2;
        }
        vm->constants[i] = c;
    }

    return true;
}

bool bundle_compile_script(const char *script_path, const char *out_binary_path, const char *self_exe) {
    // 1. Read script file
    FILE *sfp = fopen(script_path, "rb");
    if (!sfp) return false;
    fseek(sfp, 0, SEEK_END);
    long slen = ftell(sfp);
    fseek(sfp, 0, SEEK_SET);
    char *source = (char *)malloc(slen + 1);
    fread(source, 1, slen, sfp);
    source[slen] = '\0';
    fclose(sfp);

    // 2. Parse and compile
    Arena arena;
    arena_init(&arena, 256 * 1024);
    Parser parser;
    parser_init(&parser, source, slen, false, &arena);
    ASTNode *prog = parser_parse_program(&parser);
    if (parser.has_error || !prog) {
        free(source);
        arena_free(&arena);
        return false;
    }

    BytecodeEmitter be;
    bc_init(&be, &arena);
    bc_compile_program(&be, prog);

    size_t bc_len = 0;
    uint8_t *bc_data = serialize_bytecode(&be, &bc_len);

    // 3. Copy self_exe to out_binary_path
    // On Windows, also create out_binary_path.exe if not ending in .exe
    char target_path[512];
    snprintf(target_path, sizeof(target_path), "%s", out_binary_path);

#ifdef _WIN32
    if (!strstr(target_path, ".exe")) {
        strncat(target_path, ".exe", sizeof(target_path) - strlen(target_path) - 1);
    }
#endif

    FILE *self_fp = fopen(self_exe, "rb");
    if (!self_fp) {
        free(source);
        free(bc_data);
        arena_free(&arena);
        return false;
    }

    FILE *out_fp = fopen(target_path, "wb");
    if (!out_fp) {
        fclose(self_fp);
        free(source);
        free(bc_data);
        arena_free(&arena);
        return false;
    }

    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), self_fp)) > 0) {
        fwrite(chunk, 1, n, out_fp);
    }
    fclose(self_fp);

    // 4. Append serialized bytecode
    fwrite(bc_data, 1, bc_len, out_fp);

    // 5. Append length (u64) and MAGIC (u64)
    uint64_t len_u64 = (uint64_t)bc_len;
    uint64_t magic_u64 = OTTO_MAGIC;
    fwrite(&len_u64, 1, 8, out_fp);
    fwrite(&magic_u64, 1, 8, out_fp);
    fclose(out_fp);

#ifdef _WIN32
    // If target didn't have .exe, also copy/alias to bare name
    if (strcmp(target_path, out_binary_path) != 0) {
        CopyFileA(target_path, out_binary_path, FALSE);
    }
#else
    chmod(target_path, 0755);
#endif

    free(source);
    free(bc_data);
    arena_free(&arena);
    return true;
}
