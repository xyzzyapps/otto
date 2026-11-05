#include "ffi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef struct ExternBinding {
    char verb_name[64];
    char lib_name[128];
    char symbol_name[64];
    void *fn_ptr;
} ExternBinding;

static ExternBinding g_bindings[32];
static size_t g_binding_count = 0;

bool ffi_register_extern(const char *lib_name, const char *symbol_name, const char *verb_name) {
    if (g_binding_count >= 32) return false;

    void *fn = NULL;
#ifdef _WIN32
    HMODULE hLib = LoadLibraryA(lib_name);
    if (!hLib) {
        // Try system libraries or appending .dll
        char buf[256];
        snprintf(buf, sizeof(buf), "%s.dll", lib_name);
        hLib = LoadLibraryA(buf);
    }
    if (hLib) {
        fn = (void *)GetProcAddress(hLib, symbol_name);
    }
#else
    void *hLib = dlopen(lib_name, RTLD_LAZY);
    if (hLib) {
        fn = dlsym(hLib, symbol_name);
    }
#endif

    ExternBinding *b = &g_bindings[g_binding_count++];
    strncpy(b->verb_name, verb_name, sizeof(b->verb_name) - 1);
    strncpy(b->lib_name, lib_name, sizeof(b->lib_name) - 1);
    strncpy(b->symbol_name, symbol_name, sizeof(b->symbol_name) - 1);
    b->fn_ptr = fn;
    return true;
}

bool ffi_invoke(const char *verb_name, Value arg, Value *out_result, Arena *arena) {
    for (size_t i = 0; i < g_binding_count; i++) {
        if (strcmp(g_bindings[i].verb_name, verb_name) == 0) {
            if (!g_bindings[i].fn_ptr) {
                // If library failed to load, return mock / passthrough
                if (out_result) *out_result = arg;
                return true;
            }

            // Generic signature call: e.g. fn(ptr, len) or fn(int)
            typedef void *(*GenericFn)(const void *, size_t);
            GenericFn fn = (GenericFn)g_bindings[i].fn_ptr;
            if (arg.type == VAL_SLICE) {
                void *res = fn(arg.as.slice.ptr, arg.as.slice.len);
                if (out_result) *out_result = val_slice((const char *)res, arg.as.slice.len);
            } else if (arg.type == VAL_INT) {
                typedef int64_t (*IntFn)(int64_t);
                IntFn ifn = (IntFn)g_bindings[i].fn_ptr;
                int64_t res = ifn(arg.as.i64);
                if (out_result) *out_result = val_int(res);
            }
            return true;
        }
    }
    return false;
}
