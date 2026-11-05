#include "repl.h"
#include "lexer.h"
#include "parser.h"
#include "bc.h"
#include "tui.h"
#include "shell_env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <unistd.h>
#endif

static bool get_git_branch(char *out_branch, size_t max_len) {
    const char *try_paths[] = {".git/HEAD", "../.git/HEAD", "../../.git/HEAD"};
    for (size_t i = 0; i < 3; i++) {
        FILE *fp = fopen(try_paths[i], "r");
        if (fp) {
            char line[256];
            if (fgets(line, sizeof(line), fp)) {
                fclose(fp);
                const char *ref_prefix = "ref: refs/heads/";
                if (strncmp(line, ref_prefix, strlen(ref_prefix)) == 0) {
                    char *b = line + strlen(ref_prefix);
                    size_t blen = strlen(b);
                    while (blen > 0 && (b[blen - 1] == '\r' || b[blen - 1] == '\n')) b[--blen] = '\0';
                    strncpy(out_branch, b, max_len - 1);
                    out_branch[max_len - 1] = '\0';
                    return true;
                }
            } else {
                fclose(fp);
            }
        }
    }
    return false;
}

static void get_short_cwd(char *out, size_t max_len) {
    char cur[512] = "";
#ifdef _WIN32
    GetCurrentDirectoryA(sizeof(cur), cur);
#else
    getcwd(cur, sizeof(cur));
#endif
    for (char *p = cur; *p; p++) if (*p == '\\') *p = '/';
    char *last_slash = strrchr(cur, '/');
    if (last_slash && last_slash[1]) {
        strncpy(out, last_slash + 1, max_len - 1);
    } else {
        strncpy(out, cur[0] ? cur : "~", max_len - 1);
    }
    out[max_len - 1] = '\0';
}

static void repl_print_prompt(double last_ms) {
    char dir[128] = "";
    get_short_cwd(dir, sizeof(dir));

    char branch[64] = "";
    bool has_git = get_git_branch(branch, sizeof(branch));

    // Starship style: otto <dir> (branch) [12ms]
    printf("\033[1;36motto\033[0m \033[1;34m%s\033[0m", dir);
    if (has_git) {
        printf(" \033[33m(%s)\033[0m", branch);
    }
    if (last_ms > 0.0) {
        printf(" \033[2m[%.0fms]\033[0m", last_ms);
    }
    printf("\n");
    fflush(stdout);
}

static bool read_interactive_line(char *buf, size_t max_len, const char *prev_cmd) {
    if (!tui_is_interactive()) {
        printf("\033[1;32m>\033[0m ");
        fflush(stdout);
        if (!fgets(buf, (int)max_len, stdin)) return false;
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) buf[--len] = '\0';
        return true;
    }

    tui_enable_raw_mode();
    size_t len = 0;
    buf[0] = '\0';

    while (1) {
        const char *ghost = NULL;
        if (len > 0 && prev_cmd && strncmp(prev_cmd, buf, len) == 0 && strlen(prev_cmd) > len) {
            ghost = prev_cmd + len;
        }

        printf("\r\033[K\033[1;32m>\033[0m ");
        repl_print_highlighted(buf, len);
        if (ghost) {
            printf("\033[2m%s\033[0m", ghost);
            size_t glen = strlen(ghost);
            printf("\033[%dD", (int)glen);
        }
        fflush(stdout);

#ifdef _WIN32
        int ch = _getch();
        if (ch == 224 || ch == 0) {
            int ext = _getch();
            if (ext == 77 && ghost) { // Right arrow accepts ghost suggestion
                size_t glen = strlen(ghost);
                if (len + glen < max_len - 1) {
                    strcat(buf, ghost);
                    len += glen;
                }
            }
            continue;
        }
#else
        char ch_buf[8];
        int n = read(STDIN_FILENO, ch_buf, sizeof(ch_buf));
        if (n <= 0) break;
        int ch = (unsigned char)ch_buf[0];
        if (ch == 27 && n > 2 && ch_buf[1] == '[' && ch_buf[2] == 'C' && ghost) { // Right arrow
            size_t glen = strlen(ghost);
            if (len + glen < max_len - 1) {
                strcat(buf, ghost);
                len += glen;
            }
            continue;
        }
#endif

        if (ch == '\r' || ch == '\n') {
            printf("\r\033[K\033[1;32m>\033[0m ");
            repl_print_highlighted(buf, len);
            printf("\n");
            fflush(stdout);
            break;
        } else if (ch == '\t') {
            if (ghost) {
                size_t glen = strlen(ghost);
                if (len + glen < max_len - 1) {
                    strcat(buf, ghost);
                    len += glen;
                }
            }
        } else if (ch == '\b' || ch == 127) { // Backspace
            if (len > 0) {
                len--;
                buf[len] = '\0';
            }
        } else if (ch == 3) { // Ctrl+C
            buf[0] = '\0';
            len = 0;
            printf("\n");
            break;
        } else if (ch == 4) { // Ctrl+D
            if (len == 0) {
                tui_disable_raw_mode();
                return false;
            }
        } else if (ch >= 32 && ch < 127) {
            if (len < max_len - 1) {
                buf[len++] = (char)ch;
                buf[len] = '\0';
            }
        }
    }

    tui_disable_raw_mode();
    return true;
}

void repl_print_highlighted(const char *input, size_t len) {
    Lexer lex;
    lexer_init(&lex, input, len, true);

    Token tok;
    size_t last_pos = 0;

    while ((tok = lexer_next(&lex)).type != TOK_EOF) {
        if (tok.type == TOK_ERROR) {
            if (tok.lexeme.len > 0 && token_is_filler(tok.lexeme.ptr, tok.lexeme.len)) {
                size_t tok_start = (size_t)(tok.lexeme.ptr - input);
                if (tok_start > last_pos) {
                    fwrite(input + last_pos, 1, tok_start - last_pos, stdout);
                }
                printf("\033[1;31m%.*s\033[0m", (int)tok.lexeme.len, tok.lexeme.ptr); // Red filler warning
                last_pos = tok_start + tok.lexeme.len;
                continue;
            }
            break;
        }

        size_t tok_start = (size_t)(tok.lexeme.ptr - input);
        if (tok.type == TOK_STRING && tok_start > 0) {
            tok_start -= 1; // account for opening quote
        }
        if (tok_start > last_pos) {
            fwrite(input + last_pos, 1, tok_start - last_pos, stdout);
        }

        if (token_is_hook(tok.type)) {
            printf("\033[1;34m%.*s\033[0m", (int)tok.lexeme.len, tok.lexeme.ptr); // Bold blue
        } else if (token_is_verb(tok.type)) {
            printf("\033[1;36m%.*s\033[0m", (int)tok.lexeme.len, tok.lexeme.ptr); // Bold cyan
        } else if (token_is_prep(tok.type)) {
            printf("\033[1;33m%.*s\033[0m", (int)tok.lexeme.len, tok.lexeme.ptr); // Yellow
        } else if (token_is_adverb(tok.type)) {
            printf("\033[1;35m%.*s\033[0m", (int)tok.lexeme.len, tok.lexeme.ptr); // Magenta
        } else if (token_is_scanner(tok.type)) {
            printf("\033[1;32m%.*s\033[0m", (int)tok.lexeme.len, tok.lexeme.ptr); // Green
        } else if (tok.type == TOK_STRING) {
            printf("\033[32m\"%.*s\"\033[0m", (int)tok.lexeme.len, tok.lexeme.ptr);
            last_pos = tok_start + tok.lexeme.len + 2;
            continue;
        } else if (tok.type == TOK_NUMBER || tok.type == TOK_UNIT_LIT) {
            printf("\033[34m%.*s\033[0m", (int)tok.lexeme.len, tok.lexeme.ptr);
        } else {
            fwrite(tok.lexeme.ptr, 1, tok.lexeme.len, stdout);
        }
        last_pos = tok_start + tok.lexeme.len;
    }

    if (last_pos < len) {
        fwrite(input + last_pos, 1, len - last_pos, stdout);
    }
}

void repl_run(VM *vm) {
    printf("\033[1;36motto\033[0m v1.0 [Zero-Filler Automation Shell & Systems Language]\n");
    printf("Type commands, one-liners, or 'exit' to quit.\n\n");

    shell_load_rc(vm);

    char line_buf[2048];
    double last_ms = 0.0;

    while (1) {
        repl_print_prompt(last_ms);
        char *prev_cmd_str = (vm->previous_cmd.type == VAL_SLICE) ? slice_to_cstr(vm->previous_cmd.as.slice, &vm->iter_arena) : NULL;
        if (!read_interactive_line(line_buf, sizeof(line_buf), prev_cmd_str)) {
            break;
        }

        size_t len = strlen(line_buf);
        while (len > 0 && (line_buf[len - 1] == '\r' || line_buf[len - 1] == '\n')) {
            line_buf[--len] = '\0';
        }

        if (len == 0) continue;
        if (strcmp(line_buf, "exit") == 0 || strcmp(line_buf, "exit.") == 0) break;

        size_t raw_len = len;
        bool is_repeat_cmd = (strncasecmp(line_buf, "repeat", 6) == 0 && (line_buf[6] == '\0' || line_buf[6] == ' ' || line_buf[6] == '.'));

        // Implicit Period (Task 8.1): If not ending in '.' or ':' or ']', append '.'
        if (line_buf[len - 1] != '.' && line_buf[len - 1] != ':' && line_buf[len - 1] != ']') {
            line_buf[len++] = '.';
            line_buf[len] = '\0';
        }

        Arena parse_arena;
        arena_init(&parse_arena, 64 * 1024);

        Parser parser;
        parser_init(&parser, line_buf, len, true, &parse_arena);
        ASTNode *prog = parser_parse_program(&parser);

        if (parser.has_error) {
            printf("\033[1;31mSyntax error: %s\033[0m\n", parser.error_msg);
            arena_free(&parse_arena);
            continue;
        }

        BytecodeEmitter be;
        bc_init(&be, &vm->perm_arena);
        if (bc_compile_program(&be, prog)) {
            vm->code = be.code;
            vm->code_len = be.count;
            vm->constants = be.constants;
            vm->const_count = be.const_count;

            // Execute instructions with timing
#ifdef _WIN32
            LARGE_INTEGER qf, q0, q1;
            QueryPerformanceFrequency(&qf);
            QueryPerformanceCounter(&q0);
#else
            struct timespec ts0, ts1;
            clock_gettime(CLOCK_MONOTONIC, &ts0);
#endif
            vm_run(vm);

#ifdef _WIN32
            QueryPerformanceCounter(&q1);
            last_ms = ((double)(q1.QuadPart - q0.QuadPart) * 1000.0) / (double)qf.QuadPart;
#else
            clock_gettime(CLOCK_MONOTONIC, &ts1);
            last_ms = (ts1.tv_sec - ts0.tv_sec) * 1000.0 + (ts1.tv_nsec - ts0.tv_nsec) / 1000000.0;
#endif
        }

        if (!is_repeat_cmd) {
            vm_record_history(vm, (Slice){line_buf, raw_len});
        }

        arena_free(&parse_arena);
    }
}
