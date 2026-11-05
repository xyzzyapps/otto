#include "scanner.h"
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <io.h>
#else
#include <dirent.h>
#include <sys/types.h>
#endif

bool scanner_glob_match(const char *pattern, const char *string) {
    if (!pattern || !string) return false;
    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*') pattern++;
            if (!*pattern) return true;
            while (*string) {
                if (scanner_glob_match(pattern, string)) return true;
                string++;
            }
            return false;
        } else if (*pattern == '?') {
            if (!*string) return false;
            pattern++;
            string++;
        } else {
            if (*pattern != *string) return false;
            pattern++;
            string++;
        }
    }
    return *string == '\0';
}

// File Lines Stream
typedef struct FileLineStream {
    FILE *fp;
    char buffer[4096];
    bool close_on_finish;
} FileLineStream;

static bool file_lines_next(StreamDesc *s, Value *out, Arena *arena) {
    FileLineStream *fl = (FileLineStream *)s->handle;
    if (!fl || !fl->fp) return false;

    if (fgets(fl->buffer, sizeof(fl->buffer), fl->fp)) {
        size_t len = strlen(fl->buffer);
        // Trim trailing newline
        while (len > 0 && (fl->buffer[len - 1] == '\r' || fl->buffer[len - 1] == '\n')) {
            fl->buffer[--len] = '\0';
        }
        // Allocate in arena
        char *str = arena_strndup(arena, fl->buffer, len);
        *out = val_slice(str, len);
        return true;
    }
    s->eof = true;
    return false;
}

static void file_lines_close(StreamDesc *s) {
    FileLineStream *fl = (FileLineStream *)s->handle;
    if (fl && fl->fp) {
        if (fl->close_on_finish) fclose(fl->fp);
        fl->fp = NULL;
    }
}

// Directory Files Stream
typedef struct FileGlobStream {
#ifdef _WIN32
    HANDLE hFind;
    WIN32_FIND_DATAA findData;
    bool started;
    char dir_prefix[260];
    char pattern[260];
#else
    DIR *dir;
    char dir_prefix[260];
    char pattern[260];
#endif
    uint16_t adverbs;
} FileGlobStream;

static bool file_glob_next(StreamDesc *s, Value *out, Arena *arena) {
    FileGlobStream *fg = (FileGlobStream *)s->handle;
    if (!fg) return false;

#ifdef _WIN32
    if (!fg->started) {
        fg->started = true;
        char search_path[512];
        if (fg->dir_prefix[0]) {
            snprintf(search_path, sizeof(search_path), "%s/*", fg->dir_prefix);
        } else {
            snprintf(search_path, sizeof(search_path), "*");
        }
        fg->hFind = FindFirstFileA(search_path, &fg->findData);
        if (fg->hFind == INVALID_HANDLE_VALUE) {
            s->eof = true;
            return false;
        }
    } else {
        if (!FindNextFileA(fg->hFind, &fg->findData)) {
            s->eof = true;
            return false;
        }
    }

    do {
        if (strcmp(fg->findData.cFileName, ".") != 0 && strcmp(fg->findData.cFileName, "..") != 0) {
            if ((fg->adverbs & ADV_IGNORING_GIT) && strcmp(fg->findData.cFileName, ".git") == 0) {
                continue;
            }
            if (scanner_glob_match(fg->pattern, fg->findData.cFileName)) {
                char full_path[512];
                if (fg->dir_prefix[0]) {
                    snprintf(full_path, sizeof(full_path), "%s/%s", fg->dir_prefix, fg->findData.cFileName);
                } else {
                    snprintf(full_path, sizeof(full_path), "%s", fg->findData.cFileName);
                }
                char *res = arena_strdup(arena, full_path);
                *out = val_slice(res, strlen(res));
                return true;
            }
        }
    } while (FindNextFileA(fg->hFind, &fg->findData));
#else
    struct dirent *entry;
    while ((entry = readdir(fg->dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if ((fg->adverbs & ADV_IGNORING_GIT) && strcmp(entry->d_name, ".git") == 0) continue;
        if (scanner_glob_match(fg->pattern, entry->d_name)) {
            char full_path[512];
            if (fg->dir_prefix[0]) {
                snprintf(full_path, sizeof(full_path), "%s/%s", fg->dir_prefix, entry->d_name);
            } else {
                snprintf(full_path, sizeof(full_path), "%s", entry->d_name);
            }
            char *res = arena_strdup(arena, full_path);
            *out = val_slice(res, strlen(res));
            return true;
        }
    }
#endif

    s->eof = true;
    return false;
}

static void file_glob_close(StreamDesc *s) {
    FileGlobStream *fg = (FileGlobStream *)s->handle;
    if (fg) {
#ifdef _WIN32
        if (fg->hFind && fg->hFind != INVALID_HANDLE_VALUE) {
            FindClose(fg->hFind);
            fg->hFind = INVALID_HANDLE_VALUE;
        }
#else
        if (fg->dir) {
            closedir(fg->dir);
            fg->dir = NULL;
        }
#endif
    }
}

// Process Stream
typedef struct ProcessStream {
#ifdef _WIN32
    HANDLE hSnap;
    PROCESSENTRY32 pe;
    bool started;
#else
    FILE *pipe;
#endif
} ProcessStream;

static bool process_stream_next(StreamDesc *s, Value *out, Arena *arena) {
    ProcessStream *ps = (ProcessStream *)s->handle;
    if (!ps) return false;

#ifdef _WIN32
    if (!ps->started) {
        ps->started = true;
        ps->hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (ps->hSnap == INVALID_HANDLE_VALUE) {
            s->eof = true;
            return false;
        }
        ps->pe.dwSize = sizeof(PROCESSENTRY32);
        if (!Process32First(ps->hSnap, &ps->pe)) {
            s->eof = true;
            return false;
        }
    } else {
        if (!Process32Next(ps->hSnap, &ps->pe)) {
            s->eof = true;
            return false;
        }
    }

    Value pmap = val_map_create(arena, 4);
    val_map_set_str(&pmap, "pid", val_int((int64_t)ps->pe.th32ProcessID), arena);
    char *name = arena_strdup(arena, ps->pe.szExeFile);
    val_map_set_str(&pmap, "name", val_slice(name, strlen(name)), arena);
    *out = pmap;
    return true;
#else
    s->eof = true;
    return false;
#endif
}

static void process_stream_close(StreamDesc *s) {
    ProcessStream *ps = (ProcessStream *)s->handle;
    if (ps) {
#ifdef _WIN32
        if (ps->hSnap && ps->hSnap != INVALID_HANDLE_VALUE) {
            CloseHandle(ps->hSnap);
            ps->hSnap = INVALID_HANDLE_VALUE;
        }
#endif
    }
}

// Tree Directory Stream
typedef struct TreeEntry {
    char display[512];
} TreeEntry;

typedef struct TreeStream {
    TreeEntry *entries;
    size_t count;
    size_t cap;
    size_t index;
} TreeStream;

static void tree_add_entry(TreeStream *ts, const char *line, Arena *arena) {
    if (ts->count >= ts->cap) {
        size_t new_cap = ts->cap ? ts->cap * 2 : 64;
        TreeEntry *new_entries = (TreeEntry *)arena_alloc(arena, new_cap * sizeof(TreeEntry));
        if (ts->entries && ts->count > 0) {
            memcpy(new_entries, ts->entries, ts->count * sizeof(TreeEntry));
        }
        ts->entries = new_entries;
        ts->cap = new_cap;
    }
    strncpy(ts->entries[ts->count].display, line, 511);
    ts->entries[ts->count].display[511] = '\0';
    ts->count++;
}

#ifdef _WIN32
static void tree_walk(const char *dir_path, const char *prefix, uint16_t adverbs, TreeStream *ts, Arena *arena, int depth) {
    if (depth > 12) return;
    char search_pattern[512];
    snprintf(search_pattern, sizeof(search_pattern), "%s/*", (dir_path && dir_path[0]) ? dir_path : ".");

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search_pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    char names[256][260];
    bool is_dir[256];
    size_t n_count = 0;

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if ((adverbs & ADV_IGNORING_GIT) && strcmp(fd.cFileName, ".git") == 0) continue;
        if (n_count < 256) {
            strncpy(names[n_count], fd.cFileName, 259);
            names[n_count][259] = '\0';
            is_dir[n_count] = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            n_count++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    for (size_t i = 0; i < n_count; i++) {
        bool is_last = (i == n_count - 1);
        char line[512];
        snprintf(line, sizeof(line), "%s%s %s%s",
                 prefix,
                 is_last ? "+-- " : "|-- ",
                 names[i],
                 is_dir[i] ? "/" : "");
        tree_add_entry(ts, line, arena);

        if (is_dir[i]) {
            char next_dir[512];
            snprintf(next_dir, sizeof(next_dir), "%s/%s", (dir_path && dir_path[0]) ? dir_path : ".", names[i]);
            char next_prefix[512];
            snprintf(next_prefix, sizeof(next_prefix), "%s%s", prefix, is_last ? "    " : "|   ");
            tree_walk(next_dir, next_prefix, adverbs, ts, arena, depth + 1);
        }
    }
}
#else
static void tree_walk(const char *dir_path, const char *prefix, uint16_t adverbs, TreeStream *ts, Arena *arena, int depth) {
    if (depth > 12) return;
    DIR *d = opendir((dir_path && dir_path[0]) ? dir_path : ".");
    if (!d) return;

    char names[256][260];
    bool is_dir[256];
    size_t n_count = 0;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if ((adverbs & ADV_IGNORING_GIT) && strcmp(entry->d_name, ".git") == 0) continue;
        if (n_count < 256) {
            strncpy(names[n_count], entry->d_name, 259);
            names[n_count][259] = '\0';
            is_dir[n_count] = (entry->d_type == DT_DIR);
            n_count++;
        }
    }
    closedir(d);

    for (size_t i = 0; i < n_count; i++) {
        bool is_last = (i == n_count - 1);
        char line[512];
        snprintf(line, sizeof(line), "%s%s %s%s",
                 prefix,
                 is_last ? "+-- " : "|-- ",
                 names[i],
                 is_dir[i] ? "/" : "");
        tree_add_entry(ts, line, arena);

        if (is_dir[i]) {
            char next_dir[512];
            snprintf(next_dir, sizeof(next_dir), "%s/%s", (dir_path && dir_path[0]) ? dir_path : ".", names[i]);
            char next_prefix[512];
            snprintf(next_prefix, sizeof(next_prefix), "%s%s", prefix, is_last ? "    " : "|   ");
            tree_walk(next_dir, next_prefix, adverbs, ts, arena, depth + 1);
        }
    }
}
#endif

static bool tree_stream_next(StreamDesc *s, Value *out, Arena *arena) {
    TreeStream *ts = (TreeStream *)s->handle;
    if (!ts || ts->index >= ts->count) {
        s->eof = true;
        return false;
    }
    char *res = arena_strdup(arena, ts->entries[ts->index++].display);
    *out = val_slice(res, strlen(res));
    return true;
}

static void tree_stream_close(StreamDesc *s) {
    (void)s;
}

StreamDesc *scanner_open_stream(OttoTokenType scanner, const char *source, uint16_t adverbs, Arena *arena) {
    StreamDesc *sd = (StreamDesc *)arena_alloc_zero(arena, sizeof(StreamDesc));
    sd->eof = false;
    sd->source = source;

    if (scanner == TOK_SCAN_TREE) {
        TreeStream *ts = (TreeStream *)arena_alloc_zero(arena, sizeof(TreeStream));
        const char *root = (source && source[0]) ? source : ".";
        tree_walk(root, "", adverbs, ts, arena, 0);
        sd->handle = ts;
        sd->next = tree_stream_next;
        sd->close = tree_stream_close;
        return sd;
    }

    if (scanner == TOK_SCAN_FILE || scanner == TOK_SCAN_FILES) {
        FileGlobStream *fg = (FileGlobStream *)arena_alloc_zero(arena, sizeof(FileGlobStream));
        fg->adverbs = adverbs;
        const char *pat = source ? source : "*";
        // Separate directory and filename pattern
        const char *last_slash = strrchr(pat, '/');
        if (!last_slash) last_slash = strrchr(pat, '\\');

        if (last_slash) {
            size_t dlen = last_slash - pat;
            size_t copy_len = dlen < sizeof(fg->dir_prefix) - 1 ? dlen : sizeof(fg->dir_prefix) - 1;
            strncpy(fg->dir_prefix, pat, copy_len);
            fg->dir_prefix[copy_len] = '\0';
            strncpy(fg->pattern, last_slash + 1, sizeof(fg->pattern) - 1);
        } else {
            fg->dir_prefix[0] = '\0';
            strncpy(fg->pattern, pat, sizeof(fg->pattern) - 1);
        }

#ifndef _WIN32
        fg->dir = opendir(fg->dir_prefix[0] ? fg->dir_prefix : ".");
#endif
        sd->handle = fg;
        sd->next = file_glob_next;
        sd->close = file_glob_close;
        return sd;
    }

    if (scanner == TOK_SCAN_PROCESS || scanner == TOK_SCAN_PROCESSES) {
        ProcessStream *ps = (ProcessStream *)arena_alloc_zero(arena, sizeof(ProcessStream));
        sd->handle = ps;
        sd->next = process_stream_next;
        sd->close = process_stream_close;
        return sd;
    }

    // Default to File lines / Stdin
    FileLineStream *fl = (FileLineStream *)arena_alloc_zero(arena, sizeof(FileLineStream));
    if (source && source[0] && strcmp(source, "stdin") != 0 && strcmp(source, "-") != 0) {
        fl->fp = fopen(source, "r");
        fl->close_on_finish = true;
    } else {
        fl->fp = stdin;
        fl->close_on_finish = false;
    }
    if (!fl->fp) {
        sd->eof = true;
        return sd;
    }
    sd->handle = fl;
    sd->next = file_lines_next;
    sd->close = file_lines_close;
    return sd;
}

Value scanner_extract_all(OttoTokenType scanner, Value input, Arena *arena) {
    if (input.type != VAL_SLICE) return input;
    Slice s = input.as.slice;

    if (scanner == TOK_SCAN_NUMBER || scanner == TOK_SCAN_NUMBERS) {
        Value list = val_list_create(arena, 8);
        size_t i = 0;
        while (i < s.len) {
            while (i < s.len && !isdigit((unsigned char)s.ptr[i]) && s.ptr[i] != '-' && s.ptr[i] != '+') i++;
            if (i >= s.len) break;
            size_t start = i;
            if (s.ptr[i] == '-' || s.ptr[i] == '+') i++;
            while (i < s.len && isdigit((unsigned char)s.ptr[i])) i++;
            if (i < s.len && s.ptr[i] == '.' && i + 1 < s.len && isdigit((unsigned char)s.ptr[i+1])) {
                i++;
                while (i < s.len && isdigit((unsigned char)s.ptr[i])) i++;
            }
            Slice num_slice = {s.ptr + start, i - start};
            bool ok;
            int64_t val = slice_to_i64(num_slice, &ok);
            if (ok) {
                val_list_append(&list, val_int(val), arena);
            }
        }
        return list;
    }

    if (scanner == TOK_SCAN_WORD || scanner == TOK_SCAN_WORDS) {
        Value list = val_list_create(arena, 8);
        size_t i = 0;
        while (i < s.len) {
            while (i < s.len && isspace((unsigned char)s.ptr[i])) i++;
            if (i >= s.len) break;
            size_t start = i;
            while (i < s.len && !isspace((unsigned char)s.ptr[i])) i++;
            val_list_append(&list, val_slice(s.ptr + start, i - start), arena);
        }
        return list;
    }

    return input;
}

Value scanner_list_processes(Arena *arena) {
    Value list = val_list_create(arena, 32);
    StreamDesc *sd = scanner_open_stream(TOK_SCAN_PROCESS, NULL, 0, arena);
    Value item;
    while (sd->next(sd, &item, arena)) {
        val_list_append(&list, item, arena);
    }
    if (sd->close) sd->close(sd);
    return list;
}
