#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm.h"
#include "parser.h"
#include "bc.h"
#include "bundle.h"
#include "repl.h"

#ifdef _WIN32
#include <windows.h>
#endif

static void run_source(VM *vm, const char *source, size_t len, bool is_one_liner, bool stdin_loop) {
    Arena parse_arena;
    arena_init(&parse_arena, 128 * 1024);

    Parser parser;
    parser_init(&parser, source, len, false, &parse_arena);
    ASTNode *prog = parser_parse_program(&parser);

    if (parser.has_error) {
        fprintf(stderr, "Syntax error: %s\n", parser.error_msg);
        arena_free(&parse_arena);
        return;
    }

    BytecodeEmitter be;
    bc_init(&be, &vm->perm_arena);
    be.is_one_liner = is_one_liner;
    be.stdin_loop = stdin_loop;
    if (bc_compile_program(&be, prog)) {
        vm->code = be.code;
        vm->code_len = be.count;
        vm->constants = be.constants;
        vm->const_count = be.const_count;

        extern void vm_run(VM *vm);
        vm_run(vm);
    }

    arena_free(&parse_arena);
}

int main(int argc, char **argv) {
    // 1. Check for embedded bytecode footer (Single-Binary Bundler)
    char self_exe[512] = "";
#ifdef _WIN32
    GetModuleFileNameA(NULL, self_exe, sizeof(self_exe));
#else
    if (argc > 0) strncpy(self_exe, argv[0], sizeof(self_exe) - 1);
#endif

    uint8_t *embedded_bc = NULL;
    size_t embedded_len = 0;
    if (bundle_has_embedded_bytecode(self_exe, &embedded_bc, &embedded_len)) {
        VM vm;
        vm_init(&vm);
        bundle_load_into_vm(&vm, embedded_bc, embedded_len);
        extern void vm_run(VM *vm);
        vm_run(&vm);
        free(embedded_bc);
        vm_destroy(&vm);
        return 0;
    }

    // 2. Command Line Modes
    VM vm;
    vm_init(&vm);

    bool is_one_liner = false;
    bool stdin_loop = false;
    bool rpc_mode = false;
    const char *code_str = NULL;
    const char *script_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--stdin") == 0 || strcmp(argv[i], "-s") == 0) {
            stdin_loop = true;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            is_one_liner = true;
            code_str = argv[++i];
        } else if ((strcmp(argv[i], "-nc") == 0 || strcmp(argv[i], "-cn") == 0) && i + 1 < argc) {
            stdin_loop = true;
            is_one_liner = true;
            code_str = argv[++i];
        } else if (strcmp(argv[i], "--rpc") == 0) {
            rpc_mode = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("otto 1.0.0 (Zero-Filler English Automation Shell)\n");
            vm_destroy(&vm);
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: otto [options] [script.otto]\n\n"
                   "Options:\n"
                   "  -c <code>       Execute inline one-liner\n"
                   "  -n, --stdin     Implicit stdin stream loop (process lines automatically)\n"
                   "  --rpc           Agentless SSH RPC mode (executes stdin bytecode)\n"
                   "  -v, --version   Display version\n"
                   "  -h, --help      Display help information\n");
            vm_destroy(&vm);
            return 0;
        } else if (argv[i][0] != '-') {
            if (stdin_loop && !code_str && !script_file) {
                FILE *tfp = fopen(argv[i], "rb");
                if (tfp) {
                    fclose(tfp);
                    script_file = argv[i];
                } else {
                    is_one_liner = true;
                    code_str = argv[i];
                }
            } else {
                script_file = argv[i];
            }
        }
    }

    if (rpc_mode) {
        fseek(stdin, 0, SEEK_END);
        long bsize = ftell(stdin);
        fseek(stdin, 0, SEEK_SET);
        if (bsize > 0) {
            uint8_t *bc_buf = (uint8_t *)malloc(bsize);
            fread(bc_buf, 1, bsize, stdin);
            bundle_load_into_vm(&vm, bc_buf, bsize);
            extern void vm_run(VM *vm);
            vm_run(&vm);
            free(bc_buf);
        }
        vm_destroy(&vm);
        return 0;
    }

    if (code_str) {
        run_source(&vm, code_str, strlen(code_str), is_one_liner, stdin_loop);
        vm_destroy(&vm);
        return 0;
    }

    if (script_file) {
        FILE *fp = fopen(script_file, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long fsize = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            char *src = (char *)malloc(fsize + 1);
            fread(src, 1, fsize, fp);
            src[fsize] = '\0';
            fclose(fp);
            run_source(&vm, src, fsize, false, stdin_loop);
            free(src);
        } else {
            fprintf(stderr, "Cannot open script: %s\n", script_file);
        }
        vm_destroy(&vm);
        return 0;
    }

    // 3. Interactive REPL Mode
    repl_run(&vm);
    vm_destroy(&vm);
    return 0;
}
