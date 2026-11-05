#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm.h"
#include "parser.h"
#include "bc.h"
#include "bundle.h"
#include "repl.h"
#include "shell_env.h"
#include "help_tutor.h"
#include "tui.h"

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
        help_print_diagnostic(source, len, parser.error_line, parser.error_col, parser.error_msg);
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
        shell_load_dotenv(".");
        bundle_load_into_vm(&vm, embedded_bc, embedded_len);
        vm_run(&vm);
        free(embedded_bc);
        vm_destroy(&vm);
        return 0;
    }

    // 2. Command Line Modes
    VM vm;
    vm_init(&vm);
    shell_load_dotenv(".");

    bool is_one_liner = false;
    bool stdin_loop = false;
    bool rpc_mode = false;
    bool list_tasks_mode = false;
    const char *custom_ottofile = NULL;
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
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            custom_ottofile = argv[++i];
        } else if (strcmp(argv[i], "--tasks") == 0 || (!stdin_loop && !code_str && !script_file && strcmp(argv[i], "tasks") == 0)) {
            list_tasks_mode = true;
        } else if (!stdin_loop && !code_str && !script_file && strcmp(argv[i], "task") == 0) {
            const char *tf = custom_ottofile ? custom_ottofile : ottofile_find_default();
            if (!tf) {
                fprintf(stderr, "No Ottofile or tasks.otto found in current directory.\n");
                vm_destroy(&vm);
                return 1;
            }
            if (i + 1 >= argc) {
                ottofile_list_tasks(tf);
                vm_destroy(&vm);
                return 0;
            }
            const char *tname = argv[i + 1];
            int rem_argc = argc - (i + 2);
            char **rem_argv = (rem_argc > 0) ? &argv[i + 2] : NULL;
            bool ok = ottofile_run_task(&vm, tf, tname, rem_argc, rem_argv);
            vm_destroy(&vm);
            return ok ? 0 : 1;
        } else if (!stdin_loop && !code_str && !script_file && strcmp(argv[i], "tutor") == 0) {
            shell_load_rc(&vm);
            tutor_run(&vm, tui_is_interactive());
            vm_destroy(&vm);
            return 0;
        } else if (!stdin_loop && !code_str && !script_file &&
                   (strcmp(argv[i], "help") == 0 || strcmp(argv[i], "examples") == 0)) {
            bool ex_only = (strcmp(argv[i], "examples") == 0);
            const char *topic = (i + 1 < argc) ? argv[i + 1] : NULL;
            help_show(topic, ex_only, &vm.iter_arena);
            vm_destroy(&vm);
            return 0;
        } else if (strcmp(argv[i], "--rpc") == 0) {
            rpc_mode = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("otto 1.0.0 (Zero-Filler English Automation Shell)\n");
            vm_destroy(&vm);
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: otto [options] [script.otto | subcommand]\n\n"
                   "Subcommands:\n"
                   "  tutor                 Launch interactive 5-lesson Otto walkthrough\n"
                   "  help [topic]          Show built-in cheat sheet & syntax reference\n"
                   "  examples [topic]      Show copy-pasteable one-liner examples\n"
                   "  tasks                 List recipes in Ottofile / tasks.otto\n"
                   "  task <name> [args...] Run a task recipe from Ottofile / tasks.otto\n\n"
                   "Options:\n"
                   "  -c <code>             Execute inline one-liner\n"
                   "  -n, --stdin           Implicit stdin stream loop (process lines automatically)\n"
                   "  -f <Ottofile>         Specify custom Ottofile path for `tasks` / `task`\n"
                   "  --tasks               List recipes in Ottofile / tasks.otto\n"
                   "  --rpc                 Agentless SSH RPC mode (executes stdin bytecode)\n"
                   "  -v, --version         Display version\n"
                   "  -h, --help            Display help information\n");
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

    if (list_tasks_mode) {
        const char *tf = custom_ottofile ? custom_ottofile : ottofile_find_default();
        if (!tf) {
            fprintf(stderr, "No Ottofile or tasks.otto found in current directory.\n");
            vm_destroy(&vm);
            return 1;
        }
        bool ok = ottofile_list_tasks(tf);
        vm_destroy(&vm);
        return ok ? 0 : 1;
    }

    if (rpc_mode) {
        fseek(stdin, 0, SEEK_END);
        long bsize = ftell(stdin);
        fseek(stdin, 0, SEEK_SET);
        if (bsize > 0) {
            uint8_t *bc_buf = (uint8_t *)malloc((size_t)bsize);
            if (fread(bc_buf, 1, (size_t)bsize, stdin) > 0) {
                bundle_load_into_vm(&vm, bc_buf, (size_t)bsize);
                vm_run(&vm);
            }
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
            char *src = (char *)malloc((size_t)fsize + 1);
            size_t nread = fread(src, 1, (size_t)fsize, fp);
            src[nread] = '\0';
            fclose(fp);
            run_source(&vm, src, nread, false, stdin_loop);
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
