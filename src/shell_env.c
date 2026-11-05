#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif

#include "shell_env.h"
#include "vm.h"
#include "parser.h"
#include "bc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define chdir _chdir
#define getcwd _getcwd
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <strings.h>
#endif

// Directory Ring Buffer & Frecency Table
typedef struct DirEntry {
    char path[512];
    int freq;
    int recency;
} DirEntry;

static DirEntry g_dirs[64];
static size_t g_dir_count = 0;
static int g_visit_clock = 0;

void shell_cd(const char *path) {
    char cur[512];
    if (getcwd(cur, sizeof(cur))) {
        g_visit_clock++;
        // Check if already in history
        bool found = false;
        for (size_t i = 0; i < g_dir_count; i++) {
            if (strcmp(g_dirs[i].path, cur) == 0) {
                g_dirs[i].freq++;
                g_dirs[i].recency = g_visit_clock;
                found = true;
                break;
            }
        }
        if (!found) {
            if (g_dir_count < 64) {
                strncpy(g_dirs[g_dir_count].path, cur, 511);
                g_dirs[g_dir_count].freq = 1;
                g_dirs[g_dir_count].recency = g_visit_clock;
                g_dir_count++;
            } else {
                // Replace lowest frecency entry
                size_t min_idx = 0;
                int min_score = 999999;
                for (size_t i = 0; i < 64; i++) {
                    int score = g_dirs[i].freq * 10 + g_dirs[i].recency;
                    if (score < min_score) {
                        min_score = score;
                        min_idx = i;
                    }
                }
                strncpy(g_dirs[min_idx].path, cur, 511);
                g_dirs[min_idx].freq = 1;
                g_dirs[min_idx].recency = g_visit_clock;
            }
        }
    }
    if (chdir(path) == 0) {
        shell_load_dotenv(".");
    }
}

void shell_back(void) {
    if (g_dir_count > 0) {
        const char *prev = g_dirs[g_dir_count - 1].path;
        if (chdir(prev) == 0) {
            shell_load_dotenv(".");
        }
    }
}

Value shell_get_dirs(Arena *arena) {
    Value list = val_list_create(arena, g_dir_count > 0 ? g_dir_count : 4);
    for (size_t i = 0; i < g_dir_count; i++) {
        char *s = arena_strdup(arena, g_dirs[i].path);
        val_list_append(&list, val_slice(s, strlen(s)), arena);
    }
    return list;
}

static bool str_contains_case_insensitive(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (strncasecmp(haystack + i, needle, nlen) == 0) return true;
    }
    return false;
}

Value shell_jump(const char *keyword, Arena *arena) {
    if (!keyword || !keyword[0]) return val_nil();

    // 1. Check frecency history for best matching path
    int best_score = -1;
    size_t best_idx = 0;
    bool found = false;

    for (size_t i = 0; i < g_dir_count; i++) {
        if (str_contains_case_insensitive(g_dirs[i].path, keyword)) {
            int score = g_dirs[i].freq * 20 + g_dirs[i].recency * 2;
            if (score > best_score) {
                best_score = score;
                best_idx = i;
                found = true;
            }
        }
    }

    if (found) {
        char *target = arena_strdup(arena, g_dirs[best_idx].path);
        shell_cd(target);
        return val_slice(target, strlen(target));
    }

    // 2. Fallback: check if direct directory
#ifdef _WIN32
    struct _stat st;
    if (_stat(keyword, &st) == 0 && (st.st_mode & _S_IFDIR)) {
        char full[512] = "";
        _fullpath(full, keyword, sizeof(full));
        char *target = arena_strdup(arena, full[0] ? full : keyword);
        shell_cd(target);
        return val_slice(target, strlen(target));
    }
#else
    struct stat st;
    if (stat(keyword, &st) == 0 && S_ISDIR(st.st_mode)) {
        char full[512] = "";
        char *rp = realpath(keyword, full);
        (void)rp;
        char *target = arena_strdup(arena, full[0] ? full : keyword);
        shell_cd(target);
        return val_slice(target, strlen(target));
    }
#endif

    return val_nil();
}

// Job Control
static ShellJob g_jobs[16];
static size_t g_job_count = 0;

int shell_add_job(int pid, const char *cmd, void *handle) {
    if (g_job_count >= 16) return -1;
    ShellJob *j = &g_jobs[g_job_count++];
    j->id = (int)g_job_count;
    j->pid = pid;
    strncpy(j->command, cmd, sizeof(j->command) - 1);
    strcpy(j->status, "Running");
    j->process_handle = handle;
    return j->id;
}

Value shell_list_jobs(Arena *arena) {
    // Refresh background job status before listing
    for (size_t i = 0; i < g_job_count; i++) {
        if (strcmp(g_jobs[i].status, "Running") == 0) {
#ifdef _WIN32
            if (g_jobs[i].process_handle) {
                DWORD code = 0;
                if (GetExitCodeProcess((HANDLE)g_jobs[i].process_handle, &code) && code != STILL_ACTIVE) {
                    strcpy(g_jobs[i].status, "Done");
                }
            }
#else
            int st = 0;
            pid_t res = waitpid((pid_t)g_jobs[i].pid, &st, WNOHANG);
            if (res > 0) {
                strcpy(g_jobs[i].status, "Done");
            }
#endif
        }
    }

    Value list = val_list_create(arena, g_job_count > 0 ? g_job_count : 4);
    for (size_t i = 0; i < g_job_count; i++) {
        Value jmap = val_map_create(arena, 4);
        val_map_set_str(&jmap, "id", val_int(g_jobs[i].id), arena);
        val_map_set_str(&jmap, "pid", val_int(g_jobs[i].pid), arena);
        val_map_set_str(&jmap, "status", val_cstr(g_jobs[i].status), arena);
        val_map_set_str(&jmap, "command", val_cstr(g_jobs[i].command), arena);
        val_list_append(&list, jmap, arena);
    }
    return list;
}

bool shell_resume_job(int job_id, bool background) {
    for (size_t i = 0; i < g_job_count; i++) {
        if (g_jobs[i].id == job_id || job_id == 0) { // 0 = last job
#ifdef _WIN32
            if (g_jobs[i].process_handle && !background) {
                WaitForSingleObject((HANDLE)g_jobs[i].process_handle, INFINITE);
                strcpy(g_jobs[i].status, "Done");
            }
#else
            if (!background) {
                waitpid((pid_t)g_jobs[i].pid, NULL, 0);
                strcpy(g_jobs[i].status, "Done");
            }
#endif
            return true;
        }
    }
    return false;
}

bool shell_kill_job(int job_or_pid) {
    for (size_t i = 0; i < g_job_count; i++) {
        if (g_jobs[i].id == job_or_pid || g_jobs[i].pid == job_or_pid) {
#ifdef _WIN32
            if (g_jobs[i].process_handle) {
                TerminateProcess((HANDLE)g_jobs[i].process_handle, 1);
                CloseHandle((HANDLE)g_jobs[i].process_handle);
                g_jobs[i].process_handle = NULL;
            }
#else
            kill((pid_t)g_jobs[i].pid, SIGTERM);
#endif
            strcpy(g_jobs[i].status, "Terminated");
            return true;
        }
    }
    return false;
}

void shell_load_rc(VM *vm) {
    if (!vm) return;
    const char *home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
    if (!home) return;
    char rc_path[512];
    snprintf(rc_path, sizeof(rc_path), "%s/.ottorc", home);
    FILE *fp = fopen(rc_path, "rb");
    if (!fp) return;

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0) {
        fclose(fp);
        return;
    }

    char *src = (char *)malloc((size_t)fsize + 1);
    size_t nread = fread(src, 1, (size_t)fsize, fp);
    src[nread] = '\0';
    fclose(fp);

    Arena rc_arena;
    arena_init(&rc_arena, 64 * 1024);
    Parser parser;
    parser_init(&parser, src, nread, false, &rc_arena);
    ASTNode *prog = parser_parse_program(&parser);
    if (!parser.has_error && prog) {
        BytecodeEmitter be;
        bc_init(&be, &vm->perm_arena);
        if (bc_compile_program(&be, prog)) {
            vm->code = be.code;
            vm->code_len = be.count;
            vm->constants = be.constants;
            vm->const_count = be.const_count;
            vm_run(vm);
        }
    }
    arena_free(&rc_arena);
    free(src);
}

Value shell_repeat_history(Value prev_cmd, const char *with_prefix, const char *replace_old, const char *replace_new, Arena *arena) {
    if (prev_cmd.type != VAL_SLICE) return prev_cmd;
    Slice s = prev_cmd.as.slice;
    char buf[1024];

    if (replace_old && replace_new) {
        // Replace old with new in previous command
        char *found = strstr(s.ptr, replace_old);
        if (found) {
            size_t prefix_len = found - s.ptr;
            snprintf(buf, sizeof(buf), "%.*s%s%s", (int)prefix_len, s.ptr, replace_new, found + strlen(replace_old));
            char *res = arena_strdup(arena, buf);
            return val_slice(res, strlen(res));
        }
    }

    if (with_prefix) {
        snprintf(buf, sizeof(buf), "%s %.*s", with_prefix, (int)s.len, s.ptr);
        char *res = arena_strdup(arena, buf);
        return val_slice(res, strlen(res));
    }

    return prev_cmd;
}

// =========================================================
// Automatic .env Directory Loader
// =========================================================

static void set_env_portable(const char *key, const char *val) {
#ifdef _WIN32
    _putenv_s(key, val);
#else
    setenv(key, val, 1);
#endif
}

void shell_load_dotenv(const char *dir_path) {
    char env_file[512];
    snprintf(env_file, sizeof(env_file), "%s/.env", (dir_path && dir_path[0]) ? dir_path : ".");
    FILE *fp = fopen(env_file, "r");
    if (!fp) return;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == '\n')) line[--l] = '\0';
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;
        if (strncmp(p, "export ", 7) == 0) {
            p += 7;
            while (*p && isspace((unsigned char)*p)) p++;
        }
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        Slice ks = slice_trim((Slice){p, strlen(p)});
        Slice vs = slice_trim((Slice){eq + 1, strlen(eq + 1)});
        if (ks.len == 0 || ks.len >= 128) continue;

        if (vs.len >= 2 && ((vs.ptr[0] == '"' && vs.ptr[vs.len - 1] == '"') ||
                            (vs.ptr[0] == '\'' && vs.ptr[vs.len - 1] == '\''))) {
            vs.ptr++;
            vs.len -= 2;
        } else {
            // Strip inline comment outside quotes
            for (size_t i = 0; i < vs.len; i++) {
                if (vs.ptr[i] == '#' && (i == 0 || isspace((unsigned char)vs.ptr[i - 1]))) {
                    vs = slice_trim((Slice){vs.ptr, i});
                    break;
                }
            }
        }

        char kbuf[128], vbuf[896];
        memcpy(kbuf, ks.ptr, ks.len);
        kbuf[ks.len] = '\0';
        size_t vlen = vs.len < sizeof(vbuf) - 1 ? vs.len : sizeof(vbuf) - 1;
        memcpy(vbuf, vs.ptr, vlen);
        vbuf[vlen] = '\0';
        set_env_portable(kbuf, vbuf);
    }
    fclose(fp);
}

// =========================================================
// Persistent History (~/.otto/history)
// =========================================================

#define MAX_HISTORY_ENTRIES 512
static char *g_history[MAX_HISTORY_ENTRIES];
static size_t g_history_count = 0;
static bool g_history_loaded = false;

static bool resolve_history_path(char *out, size_t out_sz) {
    const char *custom = getenv("OTTO_HISTORY_FILE");
    if (custom && custom[0]) {
        snprintf(out, out_sz, "%s", custom);
        return true;
    }
    const char *home = getenv("USERPROFILE");
    if (!home || !home[0]) home = getenv("HOME");
    if (!home || !home[0]) return false;

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.otto", home);
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    snprintf(out, out_sz, "%s/history", dir);
    return true;
}

static void history_push_mem(const char *cmd) {
    if (!cmd || !cmd[0]) return;
    if (g_history_count > 0 && strcmp(g_history[g_history_count - 1], cmd) == 0) return;
    if (g_history_count >= MAX_HISTORY_ENTRIES) {
        free(g_history[0]);
        memmove(&g_history[0], &g_history[1], (MAX_HISTORY_ENTRIES - 1) * sizeof(char *));
        g_history_count = MAX_HISTORY_ENTRIES - 1;
    }
    size_t l = strlen(cmd);
    char *dup = (char *)malloc(l + 1);
    if (dup) {
        memcpy(dup, cmd, l + 1);
        g_history[g_history_count++] = dup;
    }
}

void shell_history_load(void) {
    if (g_history_loaded) return;
    g_history_loaded = true;
    char hpath[512];
    if (!resolve_history_path(hpath, sizeof(hpath))) return;
    FILE *fp = fopen(hpath, "r");
    if (!fp) return;
    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == '\n')) line[--l] = '\0';
        if (l > 0) history_push_mem(line);
    }
    fclose(fp);
}

void shell_history_add(const char *cmd) {
    if (!cmd) return;
    Slice s = slice_trim((Slice){cmd, strlen(cmd)});
    if (s.len == 0) return;
    char clean[2048];
    size_t cl = s.len < sizeof(clean) - 1 ? s.len : sizeof(clean) - 1;
    memcpy(clean, s.ptr, cl);
    clean[cl] = '\0';

    if (!g_history_loaded) shell_history_load();
    size_t before = g_history_count;
    const char *prev = (before > 0) ? g_history[before - 1] : NULL;
    if (prev && strcmp(prev, clean) == 0) return;

    history_push_mem(clean);

    char hpath[512];
    if (resolve_history_path(hpath, sizeof(hpath))) {
        FILE *fp = fopen(hpath, "a");
        if (fp) {
            fprintf(fp, "%s\n", clean);
            fclose(fp);
        }
    }
}

size_t shell_history_count(void) {
    if (!g_history_loaded) shell_history_load();
    return g_history_count;
}

const char *shell_history_get(size_t idx) {
    if (!g_history_loaded) shell_history_load();
    if (idx >= g_history_count) return NULL;
    return g_history[idx];
}

Value shell_history_list(Arena *arena) {
    if (!g_history_loaded) shell_history_load();
    Value list = val_list_create(arena, g_history_count > 0 ? g_history_count : 4);
    for (size_t i = g_history_count; i > 0; i--) {
        char *dup = arena_strdup(arena, g_history[i - 1]);
        val_list_append(&list, val_slice(dup, strlen(dup)), arena);
    }
    return list;
}

// =========================================================
// Live Abbreviations (`abbr`)
// =========================================================

typedef struct AbbrEntry {
    char name[64];
    char expansion[512];
} AbbrEntry;

static AbbrEntry g_abbrs[64];
static size_t g_abbr_count = 0;

void shell_abbr_set(const char *short_name, const char *expansion) {
    if (!short_name || !short_name[0] || !expansion) return;
    for (size_t i = 0; i < g_abbr_count; i++) {
        if (strcmp(g_abbrs[i].name, short_name) == 0) {
            snprintf(g_abbrs[i].expansion, sizeof(g_abbrs[i].expansion), "%s", expansion);
            return;
        }
    }
    if (g_abbr_count < 64) {
        snprintf(g_abbrs[g_abbr_count].name, sizeof(g_abbrs[g_abbr_count].name), "%s", short_name);
        snprintf(g_abbrs[g_abbr_count].expansion, sizeof(g_abbrs[g_abbr_count].expansion), "%s", expansion);
        g_abbr_count++;
    }
}

const char *shell_abbr_get(const char *short_name) {
    if (!short_name || !short_name[0]) return NULL;
    for (size_t i = 0; i < g_abbr_count; i++) {
        if (strcmp(g_abbrs[i].name, short_name) == 0) {
            return g_abbrs[i].expansion;
        }
    }
    return NULL;
}

Value shell_abbr_list(Arena *arena) {
    Value list = val_list_create(arena, g_abbr_count > 0 ? g_abbr_count : 4);
    for (size_t i = 0; i < g_abbr_count; i++) {
        Value m = val_map_create(arena, 4);
        val_map_set_str(&m, "abbr", val_cstr(arena_strdup(arena, g_abbrs[i].name)), arena);
        val_map_set_str(&m, "expansion", val_cstr(arena_strdup(arena, g_abbrs[i].expansion)), arena);
        val_list_append(&list, m, arena);
    }
    return list;
}

bool shell_abbr_expand_line(const char *line, char *out, size_t max_len) {
    if (!line || g_abbr_count == 0) return false;
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    const char *end = p;
    while (*end && !isspace((unsigned char)*end) && *end != '.' && *end != ',') end++;
    size_t wlen = (size_t)(end - p);
    if (wlen == 0 || wlen >= 64) return false;

    char word[64];
    memcpy(word, p, wlen);
    word[wlen] = '\0';

    const char *exp = shell_abbr_get(word);
    if (!exp) return false;

    size_t lead = (size_t)(p - line);
    snprintf(out, max_len, "%.*s%s%s", (int)lead, line, exp, end);
    return true;
}

// =========================================================
// Deduplicated $PATH Helpers (`prepend` / `append`)
// =========================================================

Value shell_path_modify(const char *dir, bool prepend, Arena *arena) {
    if (!dir || !dir[0]) {
        const char *cur = getenv("PATH");
        return cur ? val_cstr(arena_strdup(arena, cur)) : val_slice("", 0);
    }
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    const char *cur = getenv("PATH");
    if (!cur) cur = "";

    size_t dlen = strlen(dir);
    size_t clen = strlen(cur);
    char *filtered = (char *)malloc(clen + dlen + 8);
    size_t wp = 0;

    if (prepend) {
        memcpy(filtered + wp, dir, dlen);
        wp += dlen;
    }

    const char *p = cur;
    while (*p) {
        const char *next = strchr(p, sep);
        size_t slen = next ? (size_t)(next - p) : strlen(p);
        if (slen > 0) {
            bool same = (slen == dlen &&
#ifdef _WIN32
                         strncasecmp(p, dir, dlen) == 0
#else
                         strncmp(p, dir, dlen) == 0
#endif
                        );
            if (!same) {
                if (wp > 0) filtered[wp++] = sep;
                memcpy(filtered + wp, p, slen);
                wp += slen;
            }
        }
        if (!next) break;
        p = next + 1;
    }

    if (!prepend) {
        if (wp > 0) filtered[wp++] = sep;
        memcpy(filtered + wp, dir, dlen);
        wp += dlen;
    }
    filtered[wp] = '\0';

    set_env_portable("PATH", filtered);
    char *res = arena_strndup(arena, filtered, wp);
    free(filtered);
    return val_slice(res, wp);
}
