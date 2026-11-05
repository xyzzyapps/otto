#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "repl.h"
#include "lexer.h"
#include "parser.h"
#include "bc.h"
#include "tui.h"
#include "shell_env.h"
#include "help_tutor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
extern char **environ;
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
    if (!getcwd(cur, sizeof(cur))) cur[0] = '\0';
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

#define MAX_COMPLETIONS 64
#define MAX_COMP_LEN    128

static void add_completion_unique(char items[MAX_COMPLETIONS][MAX_COMP_LEN], size_t *count, const char *cand) {
    if (!cand || !cand[0] || *count >= MAX_COMPLETIONS) return;
    for (size_t i = 0; i < *count; i++) {
        if (strcmp(items[i], cand) == 0) return;
    }
    strncpy(items[*count], cand, MAX_COMP_LEN - 1);
    items[*count][MAX_COMP_LEN - 1] = '\0';
    (*count)++;
}

static size_t collect_completions(VM *vm, const char *buf, size_t cursor, size_t *out_word_start,
                                  char items[MAX_COMPLETIONS][MAX_COMP_LEN]) {
    size_t count = 0;
    size_t ws = cursor;
    while (ws > 0) {
        char c = buf[ws - 1];
        if (c == ' ' || c == '\t' || c == ',' || c == '(' || c == '[' || c == '{') break;
        ws--;
    }
    if (ws < cursor && buf[ws] == '"') {
        ws++;
    }
    *out_word_start = ws;

    size_t plen = cursor - ws;
    char prefix[256];
    if (plen >= sizeof(prefix)) plen = sizeof(prefix) - 1;
    memcpy(prefix, buf + ws, plen);
    prefix[plen] = '\0';

    // 1. $ENV completion
    if (prefix[0] == '$') {
        const char *epref = prefix + 1;
        size_t eplen = strlen(epref);
#ifdef _WIN32
        char **envp = _environ;
#else
        char **envp = environ;
#endif
        if (envp) {
            for (size_t i = 0; envp[i] && count < MAX_COMPLETIONS; i++) {
                const char *eq = strchr(envp[i], '=');
                if (!eq || eq == envp[i]) continue;
                size_t nlen = (size_t)(eq - envp[i]);
                if (nlen >= eplen && strncasecmp(envp[i], epref, eplen) == 0) {
                    char cand[MAX_COMP_LEN];
                    if (nlen + 2 < sizeof(cand)) {
                        cand[0] = '$';
                        memcpy(cand + 1, envp[i], nlen);
                        cand[nlen + 1] = '\0';
                        add_completion_unique(items, &count, cand);
                    }
                }
            }
        }
        return count;
    }

    // Check previous token for `git` subcommand completion
    bool prev_is_git = false;
    if (ws >= 4) {
        size_t pe = ws;
        while (pe > 0 && (buf[pe - 1] == ' ' || buf[pe - 1] == '\t' || buf[pe - 1] == '"')) pe--;
        size_t ps = pe;
        while (ps > 0 && buf[ps - 1] != ' ' && buf[ps - 1] != '\t' && buf[ps - 1] != ',') ps--;
        if (pe - ps == 3 && strncasecmp(buf + ps, "git", 3) == 0) {
            prev_is_git = true;
        }
    }

    if (prev_is_git) {
        static const char * const git_subs[] = {
            "status", "commits", "branches", "diff", "log", "add", "commit",
            "push", "pull", "checkout", "switch", "stash", "merge", "rebase", "fetch", NULL
        };
        for (size_t i = 0; git_subs[i]; i++) {
            if (strncmp(git_subs[i], prefix, plen) == 0) {
                add_completion_unique(items, &count, git_subs[i]);
            }
        }
    }

    bool has_slash = (strchr(prefix, '/') != NULL || strchr(prefix, '\\') != NULL ||
                      (prefix[0] == '.' && (prefix[1] == '/' || prefix[1] == '\\' || prefix[1] == '.')));

    // 2. Abbreviations, language keywords, and VM symbols (when not typing an explicit path)
    if (!has_slash && !prev_is_git) {
        if (ws == 0) {
            Value abbrs = shell_abbr_list(&vm->iter_arena);
            if (abbrs.type == VAL_LIST) {
                for (size_t i = 0; i < val_list_len(abbrs); i++) {
                    Value m = val_list_get(abbrs, i);
                    Value sv;
                    if (m.type == VAL_MAP && val_map_get_str(m, "short", &sv) && sv.type == VAL_SLICE) {
                        if (sv.as.slice.len >= plen && strncmp(sv.as.slice.ptr, prefix, plen) == 0) {
                            char *s = slice_to_cstr(sv.as.slice, &vm->iter_arena);
                            add_completion_unique(items, &count, s);
                        }
                    }
                }
            }
        }

        if (plen > 0) {
            const char * const *kws = lexer_all_keywords();
            for (size_t i = 0; kws[i] && count < MAX_COMPLETIONS; i++) {
                if (strncmp(kws[i], prefix, plen) == 0) {
                    add_completion_unique(items, &count, kws[i]);
                }
            }

            if (vm->global_symbols.type == VAL_MAP && vm->global_symbols.as.map) {
                ValueMap *gm = vm->global_symbols.as.map;
                for (size_t i = 0; i < gm->capacity && count < MAX_COMPLETIONS; i++) {
                    if (gm->entries[i].occupied) {
                        Slice k = gm->entries[i].key;
                        if (k.len >= plen && strncmp(k.ptr, prefix, plen) == 0) {
                            char *ks = slice_to_cstr(k, &vm->iter_arena);
                            add_completion_unique(items, &count, ks);
                        }
                    }
                }
            }
        }
    }

    // 3. Filesystem entries
    char dir_part[256] = "";
    char base_part[256] = "";
    const char *last_sep = NULL;
    for (const char *p = prefix; *p; p++) {
        if (*p == '/' || *p == '\\') last_sep = p;
    }
    if (last_sep) {
        size_t dlen = (size_t)(last_sep - prefix + 1);
        if (dlen < sizeof(dir_part)) {
            memcpy(dir_part, prefix, dlen);
            dir_part[dlen] = '\0';
        }
        strncpy(base_part, last_sep + 1, sizeof(base_part) - 1);
    } else {
        strncpy(base_part, prefix, sizeof(base_part) - 1);
    }
    size_t blen = strlen(base_part);

#ifdef _WIN32
    char search_glob[300];
    snprintf(search_glob, sizeof(search_glob), "%s*", dir_part[0] ? dir_part : "");
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search_glob, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            const char *name = fd.cFileName;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
            if (name[0] == '.' && base_part[0] != '.') continue;
            if (blen == 0 || strncasecmp(name, base_part, blen) == 0) {
                bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                char cand[MAX_COMP_LEN];
                snprintf(cand, sizeof(cand), "%s%s%s", dir_part, name, is_dir ? "/" : "");
                for (char *cp = cand; *cp; cp++) if (*cp == '\\') *cp = '/';
                add_completion_unique(items, &count, cand);
            }
        } while (count < MAX_COMPLETIONS && FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
#else
    const char *open_dir = dir_part[0] ? dir_part : ".";
    DIR *dp = opendir(open_dir);
    if (dp) {
        struct dirent *de;
        while (count < MAX_COMPLETIONS && (de = readdir(dp)) != NULL) {
            const char *name = de->d_name;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
            if (name[0] == '.' && base_part[0] != '.') continue;
            if (blen == 0 || strncmp(name, base_part, blen) == 0) {
                char full_p[512];
                snprintf(full_p, sizeof(full_p), "%s/%s", open_dir, name);
                struct stat st;
                bool is_dir = (stat(full_p, &st) == 0 && S_ISDIR(st.st_mode));
                char cand[MAX_COMP_LEN];
                snprintf(cand, sizeof(cand), "%s%s%s", dir_part, name, is_dir ? "/" : "");
                add_completion_unique(items, &count, cand);
            }
        }
        closedir(dp);
    }
#endif

    return count;
}

static const char *find_ghost_suggestion(const char *buf, size_t len, const char *prev_cmd) {
    if (len == 0) return NULL;
    size_t hcount = shell_history_count();
    for (size_t i = hcount; i > 0; i--) {
        const char *h = shell_history_get(i - 1);
        if (h && strncmp(h, buf, len) == 0 && strlen(h) > len) {
            return h + len;
        }
    }
    if (prev_cmd && strncmp(prev_cmd, buf, len) == 0 && strlen(prev_cmd) > len) {
        return prev_cmd + len;
    }
    return NULL;
}

typedef enum {
    RKEY_NONE = 0,
    RKEY_CHAR,
    RKEY_LEFT,
    RKEY_RIGHT,
    RKEY_UP,
    RKEY_DOWN,
    RKEY_HOME,
    RKEY_END,
    RKEY_DELETE,
    RKEY_CTRL_LEFT,
    RKEY_CTRL_RIGHT
} ReplKeyKind;

static void clear_completion_strip(bool *menu_visible, size_t *comp_count, int *comp_idx) {
    if (*menu_visible) {
        printf("\n\r\033[K\033[A");
        *menu_visible = false;
    }
    *comp_count = 0;
    *comp_idx = -1;
}

static bool read_interactive_line(VM *vm, char *buf, size_t max_len, const char *prev_cmd, double last_ms) {
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
    size_t cursor = 0;
    buf[0] = '\0';

    int hist_idx = -1;
    char saved_draft[2048] = "";
    char hist_prefix[256] = "";

    char comp_items[MAX_COMPLETIONS][MAX_COMP_LEN];
    size_t comp_count = 0;
    int comp_idx = -1;
    size_t comp_word_start = 0;
    size_t comp_applied_len = 0;
    bool comp_menu_visible = false;

    while (1) {
        const char *ghost = (cursor == len && !comp_menu_visible) ? find_ghost_suggestion(buf, len, prev_cmd) : NULL;

        printf("\r\033[K\033[1;32m>\033[0m ");
        repl_print_highlighted(buf, len);
        if (ghost) {
            printf("\033[2m%s\033[0m", ghost);
            size_t glen = strlen(ghost);
            if (glen > 0) printf("\033[%dD", (int)glen);
        }
        if (cursor < len) {
            printf("\033[%dD", (int)(len - cursor));
        }

        if (comp_menu_visible && comp_count > 1) {
            printf("\n\r\033[K");
            size_t start_i = 0;
            if (comp_idx >= 6) start_i = (size_t)(comp_idx - 5);
            size_t shown = 0;
            for (size_t i = start_i; i < comp_count && shown < 7; i++, shown++) {
                if ((int)i == comp_idx) {
                    printf("\033[7;36m %s \033[0m ", comp_items[i]);
                } else {
                    printf("\033[2m%s\033[0m  ", comp_items[i]);
                }
            }
            if (comp_count > start_i + shown) {
                printf("\033[2m(+%zu)\033[0m", comp_count - (start_i + shown));
            }
            printf("\033[A\r\033[%dC", 2 + (int)cursor);
        }
        fflush(stdout);

        ReplKeyKind kind = RKEY_NONE;
        int ch = 0;

#ifdef _WIN32
        ch = _getch();
        if (ch == 224 || ch == 0) {
            int ext = _getch();
            if (ext == 75) kind = RKEY_LEFT;
            else if (ext == 77) kind = RKEY_RIGHT;
            else if (ext == 72) kind = RKEY_UP;
            else if (ext == 80) kind = RKEY_DOWN;
            else if (ext == 71) kind = RKEY_HOME;
            else if (ext == 79) kind = RKEY_END;
            else if (ext == 83) kind = RKEY_DELETE;
            else if (ext == 115) kind = RKEY_CTRL_LEFT;
            else if (ext == 116) kind = RKEY_CTRL_RIGHT;
        } else {
            kind = RKEY_CHAR;
        }
#else
        char ch_buf[8];
        int n = (int)read(STDIN_FILENO, ch_buf, sizeof(ch_buf));
        if (n <= 0) break;
        ch = (unsigned char)ch_buf[0];
        if (ch == 27) {
            if (n >= 3 && (ch_buf[1] == '[' || ch_buf[1] == 'O')) {
                if (ch_buf[2] == 'D') kind = RKEY_LEFT;
                else if (ch_buf[2] == 'C') kind = RKEY_RIGHT;
                else if (ch_buf[2] == 'A') kind = RKEY_UP;
                else if (ch_buf[2] == 'B') kind = RKEY_DOWN;
                else if (ch_buf[2] == 'H') kind = RKEY_HOME;
                else if (ch_buf[2] == 'F') kind = RKEY_END;
                else if (n >= 4 && ch_buf[2] == '3' && ch_buf[3] == '~') kind = RKEY_DELETE;
                else if (n >= 4 && ch_buf[2] == '1' && ch_buf[3] == '~') kind = RKEY_HOME;
                else if (n >= 4 && ch_buf[2] == '4' && ch_buf[3] == '~') kind = RKEY_END;
                else if (n >= 6 && ch_buf[2] == '1' && ch_buf[3] == ';' && ch_buf[4] == '5' && ch_buf[5] == 'D') kind = RKEY_CTRL_LEFT;
                else if (n >= 6 && ch_buf[2] == '1' && ch_buf[3] == ';' && ch_buf[4] == '5' && ch_buf[5] == 'C') kind = RKEY_CTRL_RIGHT;
            } else if (n == 2 && ch_buf[1] == 'b') {
                kind = RKEY_CTRL_LEFT;
            } else if (n == 2 && ch_buf[1] == 'f') {
                kind = RKEY_CTRL_RIGHT;
            }
        } else {
            kind = RKEY_CHAR;
        }
#endif

        // Map Emacs control chords to navigation actions
        if (kind == RKEY_CHAR) {
            if (ch == 1) kind = RKEY_HOME;       // Ctrl+A
            else if (ch == 5) kind = RKEY_END;   // Ctrl+E
            else if (ch == 2) kind = RKEY_LEFT;  // Ctrl+B
            else if (ch == 6) kind = RKEY_RIGHT; // Ctrl+F
        }

        // Clear completion strip on any key except Tab
        if (!(kind == RKEY_CHAR && ch == '\t')) {
            clear_completion_strip(&comp_menu_visible, &comp_count, &comp_idx);
        }

        if (kind == RKEY_LEFT) {
            if (cursor > 0) cursor--;
            continue;
        }
        if (kind == RKEY_RIGHT) {
            if (cursor < len) {
                cursor++;
            } else if (ghost) {
                size_t glen = strlen(ghost);
                if (len + glen < max_len - 1) {
                    memcpy(buf + len, ghost, glen + 1);
                    len += glen;
                    cursor = len;
                }
            }
            continue;
        }
        if (kind == RKEY_HOME) {
            cursor = 0;
            continue;
        }
        if (kind == RKEY_END) {
            if (cursor == len && ghost) {
                size_t glen = strlen(ghost);
                if (len + glen < max_len - 1) {
                    memcpy(buf + len, ghost, glen + 1);
                    len += glen;
                }
            }
            cursor = len;
            continue;
        }
        if (kind == RKEY_CTRL_LEFT) {
            while (cursor > 0 && buf[cursor - 1] == ' ') cursor--;
            while (cursor > 0 && buf[cursor - 1] != ' ') cursor--;
            continue;
        }
        if (kind == RKEY_CTRL_RIGHT) {
            while (cursor < len && buf[cursor] != ' ') cursor++;
            while (cursor < len && buf[cursor] == ' ') cursor++;
            continue;
        }
        if (kind == RKEY_DELETE) {
            if (cursor < len) {
                memmove(buf + cursor, buf + cursor + 1, len - cursor);
                len--;
            }
            continue;
        }
        if (kind == RKEY_UP) {
            size_t hcount = shell_history_count();
            if (hcount == 0) continue;
            if (hist_idx < 0) {
                strncpy(saved_draft, buf, sizeof(saved_draft) - 1);
                saved_draft[sizeof(saved_draft) - 1] = '\0';
                size_t pcopy = cursor < sizeof(hist_prefix) - 1 ? cursor : sizeof(hist_prefix) - 1;
                memcpy(hist_prefix, buf, pcopy);
                hist_prefix[pcopy] = '\0';
                hist_idx = (int)hcount;
            }
            size_t hplen = strlen(hist_prefix);
            int search = hist_idx - 1;
            while (search >= 0) {
                const char *cand = shell_history_get((size_t)search);
                if (cand && (hplen == 0 || strncmp(cand, hist_prefix, hplen) == 0)) {
                    hist_idx = search;
                    strncpy(buf, cand, max_len - 1);
                    buf[max_len - 1] = '\0';
                    len = strlen(buf);
                    cursor = len;
                    break;
                }
                search--;
            }
            continue;
        }
        if (kind == RKEY_DOWN) {
            if (hist_idx < 0) continue;
            size_t hcount = shell_history_count();
            size_t hplen = strlen(hist_prefix);
            int search = hist_idx + 1;
            bool found = false;
            while ((size_t)search < hcount) {
                const char *cand = shell_history_get((size_t)search);
                if (cand && (hplen == 0 || strncmp(cand, hist_prefix, hplen) == 0)) {
                    hist_idx = search;
                    strncpy(buf, cand, max_len - 1);
                    buf[max_len - 1] = '\0';
                    len = strlen(buf);
                    cursor = len;
                    found = true;
                    break;
                }
                search++;
            }
            if (!found) {
                hist_idx = -1;
                strncpy(buf, saved_draft, max_len - 1);
                buf[max_len - 1] = '\0';
                len = strlen(buf);
                cursor = len;
            }
            continue;
        }

        if (kind != RKEY_CHAR) continue;

        if (ch == '\r' || ch == '\n') {
            printf("\r\033[K\033[1;32m>\033[0m ");
            repl_print_highlighted(buf, len);
            printf("\n");
            fflush(stdout);
            break;
        } else if (ch == '\t') {
            if (!comp_menu_visible) {
                size_t ws = 0;
                comp_count = collect_completions(vm, buf, cursor, &ws, comp_items);
                if (comp_count == 0) {
                    if (ghost) {
                        size_t glen = strlen(ghost);
                        if (len + glen < max_len - 1) {
                            memcpy(buf + len, ghost, glen + 1);
                            len += glen;
                            cursor = len;
                        }
                    }
                } else {
                    comp_word_start = ws;
                    size_t orig_wlen = cursor - ws;
                    size_t new_wlen = strlen(comp_items[0]);
                    if (len - orig_wlen + new_wlen < max_len - 1) {
                        memmove(buf + ws + new_wlen, buf + cursor, len - cursor + 1);
                        memcpy(buf + ws, comp_items[0], new_wlen);
                        len = len - orig_wlen + new_wlen;
                        cursor = ws + new_wlen;
                        comp_applied_len = new_wlen;
                        if (comp_count > 1) {
                            comp_menu_visible = true;
                            comp_idx = 0;
                        }
                    }
                }
            } else if (comp_count > 1) {
                comp_idx = (comp_idx + 1) % (int)comp_count;
                size_t new_wlen = strlen(comp_items[comp_idx]);
                if (len - comp_applied_len + new_wlen < max_len - 1) {
                    memmove(buf + comp_word_start + new_wlen,
                            buf + comp_word_start + comp_applied_len,
                            len - (comp_word_start + comp_applied_len) + 1);
                    memcpy(buf + comp_word_start, comp_items[comp_idx], new_wlen);
                    len = len - comp_applied_len + new_wlen;
                    cursor = comp_word_start + new_wlen;
                    comp_applied_len = new_wlen;
                }
            }
        } else if (ch == 18) { // Ctrl+R: Interactive fuzzy history search
            Value hlist = shell_history_list(&vm->iter_arena);
            size_t hlen = val_list_len(hlist);
            if (hlen > 0) {
                Value rev = val_list_create(&vm->iter_arena, hlen);
                for (size_t i = hlen; i > 0; i--) {
                    val_list_append(&rev, val_list_get(hlist, i - 1), &vm->iter_arena);
                }
                printf("\n");
                Value picked = tui_pick(rev, &vm->iter_arena);
                tui_enable_raw_mode();
                repl_print_prompt(last_ms);
                if (picked.type == VAL_SLICE && picked.as.slice.len < max_len - 1) {
                    memcpy(buf, picked.as.slice.ptr, picked.as.slice.len);
                    buf[picked.as.slice.len] = '\0';
                    len = picked.as.slice.len;
                    cursor = len;
                }
            }
        } else if (ch == 11) { // Ctrl+K: kill to end of line
            buf[cursor] = '\0';
            len = cursor;
        } else if (ch == 21) { // Ctrl+U: kill to start of line
            memmove(buf, buf + cursor, len - cursor + 1);
            len -= cursor;
            cursor = 0;
        } else if (ch == 23) { // Ctrl+W: delete previous word
            size_t old_cur = cursor;
            while (cursor > 0 && buf[cursor - 1] == ' ') cursor--;
            while (cursor > 0 && buf[cursor - 1] != ' ') cursor--;
            memmove(buf + cursor, buf + old_cur, len - old_cur + 1);
            len -= (old_cur - cursor);
        } else if (ch == 12) { // Ctrl+L: clear screen
            printf("\033[2J\033[H");
            repl_print_prompt(last_ms);
        } else if (ch == '\b' || ch == 127) { // Backspace
            if (cursor > 0) {
                memmove(buf + cursor - 1, buf + cursor, len - cursor + 1);
                cursor--;
                len--;
            }
        } else if (ch == 3) { // Ctrl+C
            buf[0] = '\0';
            len = 0;
            cursor = 0;
            printf("^C\n");
            break;
        } else if (ch == 4) { // Ctrl+D
            if (len == 0) {
                tui_disable_raw_mode();
                return false;
            }
        } else if (ch == ' ' && cursor == len && len > 0 && strchr(buf, ' ') == NULL) {
            // Live Fish-style abbreviation expansion on Space after first word
            const char *exp = shell_abbr_get(buf);
            if (exp && strlen(exp) + 2 < max_len) {
                snprintf(buf, max_len, "%s ", exp);
                len = strlen(buf);
                cursor = len;
            } else if (len < max_len - 1) {
                buf[len++] = ' ';
                buf[len] = '\0';
                cursor = len;
            }
        } else if (ch >= 32 && ch < 127) {
            if (len < max_len - 1) {
                memmove(buf + cursor + 1, buf + cursor, len - cursor + 1);
                buf[cursor] = (char)ch;
                cursor++;
                len++;
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
                printf("\033[1;31m%.*s\033[0m", (int)tok.lexeme.len, tok.lexeme.ptr);
                last_pos = tok_start + tok.lexeme.len;
                continue;
            }
            break;
        }

        size_t tok_start = (size_t)(tok.lexeme.ptr - input);
        if (tok.type == TOK_STRING && tok_start > 0) {
            tok_start -= 1;
        }
        if (tok_start > last_pos) {
            fwrite(input + last_pos, 1, tok_start - last_pos, stdout);
        }

        if (token_is_hook(tok.type)) {
            printf("\033[1;34m%.*s\033[0m", (int)tok.lexeme.len, tok.lexeme.ptr);
        } else if (token_is_verb(tok.type)) {
            printf("\033[1;36m%.*s\033[0m", (int)tok.lexeme.len, tok.lexeme.ptr);
        } else if (token_is_prep(tok.type)) {
            printf("\033[1;33m%.*s\033[0m", (int)tok.lexeme.len, tok.lexeme.ptr);
        } else if (token_is_adverb(tok.type)) {
            printf("\033[1;35m%.*s\033[0m", (int)tok.lexeme.len, tok.lexeme.ptr);
        } else if (token_is_scanner(tok.type)) {
            printf("\033[1;32m%.*s\033[0m", (int)tok.lexeme.len, tok.lexeme.ptr);
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
    printf("Type commands, 'help', 'tutor', or 'exit' to quit.\n\n");

    shell_history_load();
    shell_load_dotenv(".");
    shell_load_rc(vm);

    char line_buf[2048];
    double last_ms = 0.0;

    while (1) {
        repl_print_prompt(last_ms);
        char *prev_cmd_str = (vm->previous_cmd.type == VAL_SLICE) ? slice_to_cstr(vm->previous_cmd.as.slice, &vm->iter_arena) : NULL;
        if (!read_interactive_line(vm, line_buf, sizeof(line_buf), prev_cmd_str, last_ms)) {
            break;
        }

        size_t len = strlen(line_buf);
        while (len > 0 && (line_buf[len - 1] == '\r' || line_buf[len - 1] == '\n')) {
            line_buf[--len] = '\0';
        }

        if (len == 0) continue;
        if (strcmp(line_buf, "exit") == 0 || strcmp(line_buf, "exit.") == 0) break;

        // Expand live abbreviations on Enter as well
        char expanded_buf[2048];
        if (shell_abbr_expand_line(line_buf, expanded_buf, sizeof(expanded_buf))) {
            strncpy(line_buf, expanded_buf, sizeof(line_buf) - 1);
            line_buf[sizeof(line_buf) - 1] = '\0';
            len = strlen(line_buf);
        }

        char raw_saved[2048];
        strncpy(raw_saved, line_buf, sizeof(raw_saved) - 1);
        raw_saved[sizeof(raw_saved) - 1] = '\0';
        size_t raw_len = len;

        bool is_repeat_cmd = (strncasecmp(line_buf, "repeat", 6) == 0 && (line_buf[6] == '\0' || line_buf[6] == ' ' || line_buf[6] == '.'));

        // Implicit Period: If not ending in '.' or ':' or ']', append '.'
        if (line_buf[len - 1] != '.' && line_buf[len - 1] != ':' && line_buf[len - 1] != ']') {
            if (len + 1 < sizeof(line_buf)) {
                line_buf[len++] = '.';
                line_buf[len] = '\0';
            }
        }

        Arena parse_arena;
        arena_init(&parse_arena, 64 * 1024);

        Parser parser;
        parser_init(&parser, line_buf, len, true, &parse_arena);
        ASTNode *prog = parser_parse_program(&parser);

        if (parser.has_error) {
            help_print_diagnostic(line_buf, len, parser.error_line, parser.error_col, parser.error_msg);
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
            vm_record_history(vm, (Slice){raw_saved, raw_len});
            shell_history_add(raw_saved);
        }

        arena_free(&parse_arena);
    }
}
