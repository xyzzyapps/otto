#include "shell_env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define chdir _chdir
#define getcwd _getcwd
#else
#include <unistd.h>
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

void shell_cd(const char *path, Arena *arena) {
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
    chdir(path);
}

void shell_back(void) {
    if (g_dir_count > 0) {
        const char *prev = g_dirs[g_dir_count - 1].path;
        chdir(prev);
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
        shell_cd(target, arena);
        return val_slice(target, strlen(target));
    }

    // 2. Fallback: check if direct directory
    struct _stat st;
    if (_stat(keyword, &st) == 0 && (st.st_mode & _S_IFDIR)) {
        char full[512] = "";
#ifdef _WIN32
        _fullpath(full, keyword, sizeof(full));
#else
        realpath(keyword, full);
#endif
        char *target = arena_strdup(arena, full[0] ? full : keyword);
        shell_cd(target, arena);
        return val_slice(target, strlen(target));
    }

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
#endif
            return true;
        }
    }
    return false;
}

void shell_load_rc(void) {
    const char *home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
    if (!home) return;
    char rc_path[512];
    snprintf(rc_path, sizeof(rc_path), "%s/.ottorc", home);
    FILE *fp = fopen(rc_path, "r");
    if (!fp) {
        snprintf(rc_path, sizeof(rc_path), "%s/.shxrc", home);
        fp = fopen(rc_path, "r");
    }
    if (fp) {
        // Will be executed if present
        fclose(fp);
    }
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
