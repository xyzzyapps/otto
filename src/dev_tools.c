#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif

#include "dev_tools.h"
#include "scanner.h"
#include "ast.h"
#include "tui.h"
#include "posix_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <strings.h>
#endif

// =========================================================
// Helper Utilities
// =========================================================

static void normalize_slashes(char *s) {
    for (; *s; s++) {
        if (*s == '\\') *s = '/';
    }
}

static const char *match_substr(const char *hay, size_t hlen, const char *needle, size_t nlen, bool casefold) {
    if (nlen == 0) return hay;
    if (nlen > hlen) return NULL;
    size_t limit = hlen - nlen;
    for (size_t i = 0; i <= limit; i++) {
        if (casefold) {
            bool ok = true;
            for (size_t j = 0; j < nlen; j++) {
                if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j])) {
                    ok = false;
                    break;
                }
            }
            if (ok) return hay + i;
        } else {
            if (memcmp(hay + i, needle, nlen) == 0) return hay + i;
        }
    }
    return NULL;
}

static void format_human_bytes(int64_t bytes, char *out, size_t out_sz) {
    if (bytes >= 1024LL * 1024LL * 1024LL) {
        snprintf(out, out_sz, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024LL * 1024LL) {
        snprintf(out, out_sz, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024LL) {
        snprintf(out, out_sz, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, out_sz, "%lld B", (long long)bytes);
    }
}

// =========================================================
// A1: Fast Multi-File Code Search (`search` verb)
// =========================================================

typedef struct IgnoreRules {
    char patterns[64][128];
    size_t count;
} IgnoreRules;

static void load_gitignore(const char *dir, IgnoreRules *rules) {
    char gi_path[512];
    snprintf(gi_path, sizeof(gi_path), "%s/.gitignore", (dir && dir[0]) ? dir : ".");
    FILE *fp = fopen(gi_path, "r");
    if (!fp && dir && strcmp(dir, ".") != 0) {
        fp = fopen(".gitignore", "r");
    }
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp) && rules->count < 64) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == '\n' || line[l - 1] == ' ')) line[--l] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#' || *p == '!') continue;
        if (*p == '/') p++;
        l = strlen(p);
        if (l > 0 && p[l - 1] == '/') p[--l] = '\0';
        if (l > 0 && l < 127) {
            memcpy(rules->patterns[rules->count], p, l + 1);
            rules->count++;
        }
    }
    fclose(fp);
}

static bool should_ignore_entry(const char *name, const char *rel_path, bool is_dir, const IgnoreRules *rules) {
    if (strcmp(name, ".git") == 0 || strcmp(name, "node_modules") == 0 ||
        strcmp(name, ".otto_journal") == 0 || strcmp(name, "__pycache__") == 0 ||
        strcmp(name, "target") == 0 || strcmp(name, ".DS_Store") == 0) {
        return true;
    }
    if (is_dir && (strcmp(name, "dist") == 0 || strcmp(name, ".vs") == 0 || strcmp(name, ".idea") == 0)) {
        return true;
    }
    for (size_t i = 0; i < rules->count; i++) {
        const char *pat = rules->patterns[i];
        if (scanner_glob_match(pat, name) || (rel_path && scanner_glob_match(pat, rel_path))) {
            return true;
        }
    }
    return false;
}

static void search_single_file(const char *filepath, const char *pattern, size_t plen, bool casefold, Value *results, Arena *arena) {
    if (val_list_len(*results) >= 2000) return;
    FILE *fp = fopen(filepath, "rb");
    if (!fp) return;

    // Binary check on first 512 bytes
    unsigned char probe[512];
    size_t pn = fread(probe, 1, sizeof(probe), fp);
    for (size_t i = 0; i < pn; i++) {
        if (probe[i] == 0) {
            fclose(fp);
            return;
        }
    }
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    if (fsz <= 0 || fsz > 16 * 1024 * 1024) {
        fclose(fp);
        return;
    }
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)fsz + 1);
    if (!buf) {
        fclose(fp);
        return;
    }
    size_t nread = fread(buf, 1, (size_t)fsz, fp);
    buf[nread] = '\0';
    fclose(fp);

    char norm_path[512];
    snprintf(norm_path, sizeof(norm_path), "%s", filepath);
    normalize_slashes(norm_path);
    const char *clean_path = (norm_path[0] == '.' && norm_path[1] == '/') ? norm_path + 2 : norm_path;
    char *file_dup = NULL;

    int line_no = 1;
    size_t pos = 0;
    while (pos < nread && val_list_len(*results) < 2000) {
        size_t line_end = pos;
        while (line_end < nread && buf[line_end] != '\n') line_end++;
        size_t llen = line_end - pos;
        if (llen > 0 && buf[pos + llen - 1] == '\r') llen--;

        const char *hit = match_substr(buf + pos, llen, pattern, plen, casefold);
        if (hit) {
            if (!file_dup) file_dup = arena_strdup(arena, clean_path);
            int col_no = (int)(hit - (buf + pos)) + 1;
            Slice trimmed = slice_trim((Slice){buf + pos, llen});
            if (trimmed.len > 240) trimmed.len = 240;
            char *tdup = arena_strndup(arena, trimmed.ptr, trimmed.len);

            Value m = val_map_create(arena, 4);
            val_map_set_str(&m, "file", val_slice(file_dup, strlen(file_dup)), arena);
            val_map_set_str(&m, "line", val_int(line_no), arena);
            val_map_set_str(&m, "col", val_int(col_no), arena);
            val_map_set_str(&m, "text", val_slice(tdup, trimmed.len), arena);
            val_list_append(results, m, arena);
        }

        pos = (line_end < nread) ? line_end + 1 : nread;
        line_no++;
    }

    free(buf);
}

static void search_walk_dir(const char *dir, const char *pattern, size_t plen, bool casefold,
                            const IgnoreRules *rules, Value *results, Arena *arena, int depth) {
    if (depth > 16 || val_list_len(*results) >= 2000) return;

#ifdef _WIN32
    char spec[512];
    snprintf(spec, sizeof(spec), "%s/*", (dir && dir[0]) ? dir : ".");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(spec, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        char child[512];
        if (!dir || strcmp(dir, ".") == 0) snprintf(child, sizeof(child), "%s", fd.cFileName);
        else snprintf(child, sizeof(child), "%s/%s", dir, fd.cFileName);
        normalize_slashes(child);

        if (should_ignore_entry(fd.cFileName, child, is_dir, rules)) continue;
        if (is_dir) {
            search_walk_dir(child, pattern, plen, casefold, rules, results, arena, depth + 1);
        } else {
            search_single_file(child, pattern, plen, casefold, results, arena);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir((dir && dir[0]) ? dir : ".");
    if (!d) return;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[512];
        if (!dir || strcmp(dir, ".") == 0) snprintf(child, sizeof(child), "%s", entry->d_name);
        else snprintf(child, sizeof(child), "%s/%s", dir, entry->d_name);

        struct stat st;
        bool is_dir = false;
        if (stat(child, &st) == 0) is_dir = S_ISDIR(st.st_mode);
        if (should_ignore_entry(entry->d_name, child, is_dir, rules)) continue;
        if (is_dir) {
            search_walk_dir(child, pattern, plen, casefold, rules, results, arena, depth + 1);
        } else {
            search_single_file(child, pattern, plen, casefold, results, arena);
        }
    }
    closedir(d);
#endif
}

Value dev_search(const char *pattern, const char *root_dir, uint16_t adverbs, Arena *arena) {
    Value results = val_list_create(arena, 32);
    if (!pattern || !pattern[0]) return results;

    const char *target = (root_dir && root_dir[0]) ? root_dir : ".";
    bool casefold = (adverbs & ADV_CASEFOLD) != 0;
    size_t plen = strlen(pattern);

    struct stat st;
    if (stat(target, &st) == 0 && !S_ISDIR(st.st_mode)) {
        search_single_file(target, pattern, plen, casefold, &results, arena);
        return results;
    }

    IgnoreRules rules;
    memset(&rules, 0, sizeof(rules));
    load_gitignore(target, &rules);
    search_walk_dir(target, pattern, plen, casefold, &rules, &results, arena, 0);
    return results;
}

// =========================================================
// A2: Rich File Metadata Objects (`stat` verb & properties)
// =========================================================

static Value build_stat_map(const char *filepath, const struct stat *st, Arena *arena) {
    char norm[512];
    snprintf(norm, sizeof(norm), "%s", filepath);
    normalize_slashes(norm);
    const char *clean = (norm[0] == '.' && norm[1] == '/') ? norm + 2 : norm;

    const char *slash = strrchr(clean, '/');
    const char *base = slash ? slash + 1 : clean;
    const char *dot = strrchr(base, '.');
    const char *ext = (dot && dot != base && dot[1] != '\0') ? dot + 1 : "";

    time_t now = time(NULL);
    int64_t age = (now > st->st_mtime) ? (int64_t)(now - st->st_mtime) : 0;

    char mod_buf[32] = "";
    struct tm *tm_info = localtime(&st->st_mtime);
    if (tm_info) {
        strftime(mod_buf, sizeof(mod_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    }

    Value m = val_map_create(arena, 8);
    char *pdup = arena_strdup(arena, clean);
    char *ndup = arena_strdup(arena, base);
    char *edup = arena_strdup(arena, ext);
    char *mdup = arena_strdup(arena, mod_buf);

    val_map_set_str(&m, "path", val_slice(pdup, strlen(pdup)), arena);
    val_map_set_str(&m, "name", val_slice(ndup, strlen(ndup)), arena);
    val_map_set_str(&m, "ext", val_slice(edup, strlen(edup)), arena);
    val_map_set_str(&m, "size", val_int((int64_t)st->st_size), arena);
    val_map_set_str(&m, "age", val_int(age), arena);
    val_map_set_str(&m, "modified", val_slice(mdup, strlen(mdup)), arena);
    val_map_set_str(&m, "dir", val_bool(S_ISDIR(st->st_mode)), arena);
    return m;
}

Value dev_stat_path(const char *path, uint16_t adverbs, Arena *arena) {
    const char *target = (path && path[0]) ? path : ".";
    bool has_glob = (strchr(target, '*') != NULL || strchr(target, '?') != NULL);

    if (!has_glob) {
        struct stat st;
        if (stat(target, &st) != 0) return val_nil();
        if (!S_ISDIR(st.st_mode)) {
            return build_stat_map(target, &st, arena);
        }
        // Directory: stat its entries
        char glob_pat[512];
        snprintf(glob_pat, sizeof(glob_pat), "%s/*", target);
        StreamDesc *sd = scanner_open_stream(TOK_SCAN_FILES, glob_pat, adverbs, arena);
        Value list = val_list_create(arena, 32);
        Value item;
        while (sd && sd->next && sd->next(sd, &item, arena)) {
            if (item.type == VAL_SLICE) {
                char *cp = slice_to_cstr(item.as.slice, arena);
                struct stat cst;
                if (stat(cp, &cst) == 0) {
                    val_list_append(&list, build_stat_map(cp, &cst, arena), arena);
                }
            }
        }
        if (sd && sd->close) sd->close(sd);
        return list;
    }

    StreamDesc *sd = scanner_open_stream(TOK_SCAN_FILES, target, adverbs, arena);
    Value list = val_list_create(arena, 32);
    Value item;
    while (sd && sd->next && sd->next(sd, &item, arena)) {
        if (item.type == VAL_SLICE) {
            char *cp = slice_to_cstr(item.as.slice, arena);
            struct stat cst;
            if (stat(cp, &cst) == 0) {
                val_list_append(&list, build_stat_map(cp, &cst, arena), arena);
            }
        }
    }
    if (sd && sd->close) sd->close(sd);
    return list;
}

Value dev_stat_prop(Slice path_slice, Slice prop_slice, Arena *arena, bool *handled) {
    if (handled) *handled = false;
    if (slice_eq_str(prop_slice, "len") || slice_eq_str(prop_slice, "length")) {
        if (handled) *handled = true;
        return val_int((int64_t)path_slice.len);
    }

    bool is_file_prop = slice_eq_str(prop_slice, "size") ||
                        slice_eq_str(prop_slice, "ext") ||
                        slice_eq_str(prop_slice, "age") ||
                        slice_eq_str(prop_slice, "modified") ||
                        slice_eq_str(prop_slice, "dir") ||
                        slice_eq_str(prop_slice, "name") ||
                        slice_eq_str(prop_slice, "exists");
    if (!is_file_prop) return val_nil();

    if (handled) *handled = true;
    char *cpath = slice_to_cstr(path_slice, arena);
    struct stat st;
    bool exists = (stat(cpath, &st) == 0);
    if (slice_eq_str(prop_slice, "exists")) {
        return val_bool(exists);
    }
    if (!exists) return val_nil();

    Value m = build_stat_map(cpath, &st, arena);
    Value out = val_nil();
    val_map_get(m, prop_slice, &out);
    return out;
}

// =========================================================
// A3: Syntax-Highlighted Pager (`view` verb)
// =========================================================

static bool is_code_keyword(const char *w, size_t len) {
    static const char * const kws[] = {
        "int", "char", "void", "bool", "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "int64_t", "double", "float", "struct", "typedef", "enum", "union", "const", "static",
        "if", "else", "for", "while", "switch", "case", "default", "break", "continue", "return",
        "true", "false", "NULL", "nil", "fn", "pub", "let", "mut", "var", "function", "async",
        "await", "import", "from", "export", "class", "def", "every", "before", "after", "watch",
        "read", "write", "print", "take", "drop", "sort", "replace", "search", "stat", "view",
        "where", "into", "onto", "with", "by", "and", "or", "unless", NULL
    };
    for (size_t i = 0; kws[i]; i++) {
        if (strlen(kws[i]) == len && memcmp(w, kws[i], len) == 0) return true;
    }
    return false;
}

static void format_highlighted_line(const char *line, size_t len, int line_no, char *out, size_t out_sz) {
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, out_sz - pos, "\033[2m%4d │\033[0m ", line_no);

    // Diff or Markdown heading fast paths
    if (len > 0 && line[0] == '+' && (len == 1 || line[1] != '+')) {
        snprintf(out + pos, out_sz - pos, "\033[32m%.*s\033[0m", (int)len, line);
        return;
    }
    if (len > 0 && line[0] == '-' && (len == 1 || line[1] != '-')) {
        snprintf(out + pos, out_sz - pos, "\033[31m%.*s\033[0m", (int)len, line);
        return;
    }

    size_t i = 0;
    while (i < len && pos + 32 < out_sz) {
        char c = line[i];
        // Line comment: // or #
        if ((c == '/' && i + 1 < len && line[i + 1] == '/') ||
            (c == '#' && (i == 0 || isspace((unsigned char)line[i - 1])))) {
            pos += (size_t)snprintf(out + pos, out_sz - pos, "\033[2;32m%.*s\033[0m", (int)(len - i), line + i);
            break;
        }
        // String literal
        if (c == '"' || c == '\'') {
            char q = c;
            size_t start = i++;
            while (i < len && line[i] != q) {
                if (line[i] == '\\' && i + 1 < len) i++;
                i++;
            }
            if (i < len && line[i] == q) i++;
            pos += (size_t)snprintf(out + pos, out_sz - pos, "\033[33m%.*s\033[0m", (int)(i - start), line + start);
            continue;
        }
        // Number
        if (isdigit((unsigned char)c) && (i == 0 || !isalnum((unsigned char)line[i - 1]))) {
            size_t start = i;
            while (i < len && (isalnum((unsigned char)line[i]) || line[i] == '.')) i++;
            pos += (size_t)snprintf(out + pos, out_sz - pos, "\033[35m%.*s\033[0m", (int)(i - start), line + start);
            continue;
        }
        // Identifier / keyword
        if (isalpha((unsigned char)c) || c == '_') {
            size_t start = i;
            while (i < len && (isalnum((unsigned char)line[i]) || line[i] == '_')) i++;
            size_t wlen = i - start;
            if (is_code_keyword(line + start, wlen)) {
                pos += (size_t)snprintf(out + pos, out_sz - pos, "\033[1;36m%.*s\033[0m", (int)wlen, line + start);
            } else {
                if (pos + wlen + 1 < out_sz) {
                    memcpy(out + pos, line + start, wlen);
                    pos += wlen;
                    out[pos] = '\0';
                }
            }
            continue;
        }
        out[pos++] = c;
        out[pos] = '\0';
        i++;
    }
}

static int get_terminal_rows(void) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        int r = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        if (r > 5) return r;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 5) {
        return (int)ws.ws_row;
    }
#endif
    return 24;
}

Value dev_view(Value input, Arena *arena) {
    const char *title = "<buffer>";
    const char *text = "";
    size_t tlen = 0;

    if (input.type == VAL_SLICE) {
        bool has_nl = (memchr(input.as.slice.ptr, '\n', input.as.slice.len) != NULL);
        if (!has_nl && input.as.slice.len > 0 && input.as.slice.len < 500) {
            char *p = slice_to_cstr(input.as.slice, arena);
            FILE *fp = fopen(p, "rb");
            if (fp) {
                fseek(fp, 0, SEEK_END);
                long fsz = ftell(fp);
                if (fsz < 0) fsz = 0;
                fseek(fp, 0, SEEK_SET);
                char *buf = (char *)arena_alloc(arena, (size_t)fsz + 1);
                tlen = fread(buf, 1, (size_t)fsz, fp);
                buf[tlen] = '\0';
                fclose(fp);
                text = buf;
                title = p;
            } else {
                text = input.as.slice.ptr;
                tlen = input.as.slice.len;
            }
        } else {
            text = input.as.slice.ptr;
            tlen = input.as.slice.len;
        }
    } else {
        char *s = val_to_string(input, arena);
        text = s;
        tlen = strlen(s);
    }

    // Split into lines
    size_t cap = 128, count = 0;
    char **rendered = (char **)arena_alloc(arena, cap * sizeof(char *));
    size_t pos = 0;
    while (pos <= tlen) {
        if (pos == tlen && count > 0) break;
        size_t end = pos;
        while (end < tlen && text[end] != '\n') end++;
        size_t llen = end - pos;
        if (llen > 0 && text[pos + llen - 1] == '\r') llen--;

        char line_buf[2048];
        format_highlighted_line(text + pos, llen, (int)(count + 1), line_buf, sizeof(line_buf));
        if (count >= cap) {
            size_t ncap = cap * 2;
            char **nr = (char **)arena_alloc(arena, ncap * sizeof(char *));
            memcpy(nr, rendered, count * sizeof(char *));
            rendered = nr;
            cap = ncap;
        }
        rendered[count++] = arena_strdup(arena, line_buf);
        if (end >= tlen) break;
        pos = end + 1;
    }

    int term_rows = get_terminal_rows();
    bool use_pager = tui_is_interactive() && !getenv("OTTO_NO_PAGER") && ((int)count + 4 > term_rows);

    if (!use_pager) {
        printf("\033[2m─────┼────────────────────────────────────────────────────────\033[0m\n");
        printf("\033[2m     │\033[0m \033[1m%s\033[0m \033[2m(%zu lines)\033[0m\n", title, count);
        printf("\033[2m─────┼────────────────────────────────────────────────────────\033[0m\n");
        for (size_t i = 0; i < count; i++) {
            printf("%s\n", rendered[i]);
        }
        printf("\033[2m─────┴────────────────────────────────────────────────────────\033[0m\n");
        fflush(stdout);
        return val_slice(text, tlen);
    }

    // Interactive terminal pager
    tui_enable_raw_mode();
    size_t top = 0;
    size_t page_sz = (size_t)(term_rows > 5 ? term_rows - 4 : 18);
    while (1) {
        printf("\033[2J\033[H");
        printf("\033[2m─────┼────────────────────────────────────────────────────────\033[0m\n");
        printf("\033[2m     │\033[0m \033[1m%s\033[0m \033[2m(lines %zu-%zu / %zu)\033[0m\n",
               title, top + 1, (top + page_sz < count ? top + page_sz : count), count);
        printf("\033[2m─────┼────────────────────────────────────────────────────────\033[0m\n");
        for (size_t i = 0; i < page_sz && top + i < count; i++) {
            printf("%s\n", rendered[top + i]);
        }
        printf("\033[7m [SPACE/j/k/q] %s \033[0m", title);
        fflush(stdout);

        int ch = 0;
#ifdef _WIN32
        ch = _getch();
        if (ch == 224 || ch == 0) {
            int ext = _getch();
            if (ext == 72) ch = 'k';
            else if (ext == 80) ch = 'j';
            else if (ext == 81) ch = ' ';
            else if (ext == 73) ch = 'b';
        }
#else
        unsigned char cbuf[4];
        ssize_t nr = read(STDIN_FILENO, cbuf, sizeof(cbuf));
        if (nr <= 0) break;
        ch = cbuf[0];
        if (ch == 27 && nr >= 3 && cbuf[1] == '[') {
            if (cbuf[2] == 'A') ch = 'k';
            else if (cbuf[2] == 'B') ch = 'j';
        }
#endif
        if (ch == 'q' || ch == 'Q' || ch == 27 || ch == 3) break;
        if (ch == 'j' || ch == '\r' || ch == '\n') {
            if (top + page_sz < count) top++;
        } else if (ch == 'k') {
            if (top > 0) top--;
        } else if (ch == ' ' || ch == 'f') {
            if (top + page_sz < count) top += page_sz;
        } else if (ch == 'b') {
            top = (top > page_sz) ? (top - page_sz) : 0;
        }
    }
    printf("\r\033[K\n");
    tui_disable_raw_mode();
    return val_slice(text, tlen);
}

// =========================================================
// A4: Native Git Scanners (`status`, `commits`, `branches`, `diff`)
// =========================================================

Value dev_git_scan(OttoTokenType scanner, const char *arg, Arena *arena) {
    Value raw_out = val_nil(), raw_err = val_nil();

    if (scanner == TOK_SCAN_STATUS) {
        posix_run_command("git --no-pager status --porcelain", val_nil(), &raw_out, &raw_err, arena);
        Value list = val_list_create(arena, 16);
        if (raw_out.type != VAL_SLICE || raw_out.as.slice.len == 0) return list;
        const char *p = raw_out.as.slice.ptr;
        const char *end = p + raw_out.as.slice.len;
        while (p < end) {
            const char *nl = p;
            while (nl < end && *nl != '\n') nl++;
            size_t llen = (size_t)(nl - p);
            if (llen > 0 && p[llen - 1] == '\r') llen--;
            if (llen >= 3) {
                Slice st_s = slice_trim((Slice){p, 2});
                Slice fn_s = slice_trim((Slice){p + 3, llen - 3});
                Value m = val_map_create(arena, 4);
                char *sdup = arena_strndup(arena, st_s.ptr, st_s.len);
                char *fdup = arena_strndup(arena, fn_s.ptr, fn_s.len);
                val_map_set_str(&m, "state", val_slice(sdup, st_s.len), arena);
                val_map_set_str(&m, "file", val_slice(fdup, fn_s.len), arena);
                val_list_append(&list, m, arena);
            }
            p = (nl < end) ? nl + 1 : end;
        }
        return list;
    }

    if (scanner == TOK_SCAN_COMMITS) {
        posix_run_command("git --no-pager log -n 30 --date=short \"--pretty=format:%h\t%an\t%ad\t%s\"",
                          val_nil(), &raw_out, &raw_err, arena);
        Value list = val_list_create(arena, 32);
        if (raw_out.type != VAL_SLICE || raw_out.as.slice.len == 0) return list;
        const char *p = raw_out.as.slice.ptr;
        const char *end = p + raw_out.as.slice.len;
        while (p < end) {
            const char *nl = p;
            while (nl < end && *nl != '\n') nl++;
            size_t llen = (size_t)(nl - p);
            if (llen > 0 && p[llen - 1] == '\r') llen--;
            if (llen > 0) {
                const char *t1 = (const char *)memchr(p, '\t', llen);
                const char *t2 = t1 ? (const char *)memchr(t1 + 1, '\t', (size_t)(p + llen - (t1 + 1))) : NULL;
                const char *t3 = t2 ? (const char *)memchr(t2 + 1, '\t', (size_t)(p + llen - (t2 + 1))) : NULL;
                if (t1 && t2 && t3) {
                    Value m = val_map_create(arena, 4);
                    val_map_set_str(&m, "hash", val_slice(arena_strndup(arena, p, (size_t)(t1 - p)), (size_t)(t1 - p)), arena);
                    val_map_set_str(&m, "author", val_slice(arena_strndup(arena, t1 + 1, (size_t)(t2 - t1 - 1)), (size_t)(t2 - t1 - 1)), arena);
                    val_map_set_str(&m, "date", val_slice(arena_strndup(arena, t2 + 1, (size_t)(t3 - t2 - 1)), (size_t)(t3 - t2 - 1)), arena);
                    size_t mlen = (size_t)(p + llen - (t3 + 1));
                    val_map_set_str(&m, "message", val_slice(arena_strndup(arena, t3 + 1, mlen), mlen), arena);
                    val_list_append(&list, m, arena);
                }
            }
            p = (nl < end) ? nl + 1 : end;
        }
        return list;
    }

    if (scanner == TOK_SCAN_BRANCHES) {
        posix_run_command("git --no-pager branch --no-color", val_nil(), &raw_out, &raw_err, arena);
        Value list = val_list_create(arena, 8);
        if (raw_out.type != VAL_SLICE || raw_out.as.slice.len == 0) return list;
        const char *p = raw_out.as.slice.ptr;
        const char *end = p + raw_out.as.slice.len;
        while (p < end) {
            const char *nl = p;
            while (nl < end && *nl != '\n') nl++;
            size_t llen = (size_t)(nl - p);
            if (llen > 0 && p[llen - 1] == '\r') llen--;
            if (llen >= 2) {
                bool cur = (p[0] == '*');
                Slice bname = slice_trim((Slice){p + 2, llen - 2});
                if (bname.len > 0) {
                    Value m = val_map_create(arena, 4);
                    val_map_set_str(&m, "name", val_slice(arena_strndup(arena, bname.ptr, bname.len), bname.len), arena);
                    val_map_set_str(&m, "current", val_bool(cur), arena);
                    val_list_append(&list, m, arena);
                }
            }
            p = (nl < end) ? nl + 1 : end;
        }
        return list;
    }

    if (scanner == TOK_SCAN_DIFF) {
        char cmd[512];
        if (arg && arg[0] && strcmp(arg, "stdin") != 0) {
            snprintf(cmd, sizeof(cmd), "git --no-pager diff --no-color -- \"%s\"", arg);
        } else {
            snprintf(cmd, sizeof(cmd), "git --no-pager diff --no-color");
        }
        posix_run_command(cmd, val_nil(), &raw_out, &raw_err, arena);
        if (raw_out.type != VAL_SLICE || raw_out.as.slice.len == 0) return val_slice("", 0);

        // Delta-style syntax colorizing
        size_t cap = raw_out.as.slice.len * 2 + 256;
        char *out = (char *)arena_alloc(arena, cap);
        size_t wp = 0;
        const char *p = raw_out.as.slice.ptr;
        const char *end = p + raw_out.as.slice.len;
        while (p < end && wp + 128 < cap) {
            const char *nl = p;
            while (nl < end && *nl != '\n') nl++;
            size_t llen = (size_t)(nl - p);
            if (llen > 0 && p[llen - 1] == '\r') llen--;

            const char *col = NULL;
            if (llen >= 4 && strncmp(p, "diff", 4) == 0) col = "\033[1;33m";
            else if (llen >= 2 && strncmp(p, "@@", 2) == 0) col = "\033[36m";
            else if (llen >= 1 && p[0] == '+' && !(llen >= 3 && strncmp(p, "+++", 3) == 0)) col = "\033[32m";
            else if (llen >= 1 && p[0] == '-' && !(llen >= 3 && strncmp(p, "---", 3) == 0)) col = "\033[31m";

            if (col && wp + llen + 16 < cap) {
                wp += (size_t)snprintf(out + wp, cap - wp, "%s%.*s\033[0m", col, (int)llen, p);
            } else if (wp + llen + 2 < cap) {
                memcpy(out + wp, p, llen);
                wp += llen;
            }
            if (nl < end && wp + 2 < cap) out[wp++] = '\n';
            p = (nl < end) ? nl + 1 : end;
        }
        out[wp] = '\0';
        return val_slice(out, wp);
    }

    return val_nil();
}

// =========================================================
// A5: Pure C11 SHA-256, CRC32, Base64/Hex/URL & UUID v4
// =========================================================

static uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_compute(const uint8_t *data, size_t len, uint8_t digest[32]) {
    static const uint32_t K[64] = {
        0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
        0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
        0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
        0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
        0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
        0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
        0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
        0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
    };
    uint32_t H[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    size_t padded_len = ((len + 9 + 63) / 64) * 64;
    uint8_t *msg = (uint8_t *)calloc(padded_len, 1);
    if (len > 0) memcpy(msg, data, len);
    msg[len] = 0x80;
    uint64_t bit_len = (uint64_t)len * 8ULL;
    for (int i = 0; i < 8; i++) {
        msg[padded_len - 1 - i] = (uint8_t)((bit_len >> (i * 8)) & 0xFF);
    }

    for (size_t off = 0; off < padded_len; off += 64) {
        uint32_t W[64];
        for (int t = 0; t < 16; t++) {
            W[t] = ((uint32_t)msg[off + t * 4] << 24) |
                   ((uint32_t)msg[off + t * 4 + 1] << 16) |
                   ((uint32_t)msg[off + t * 4 + 2] << 8) |
                   ((uint32_t)msg[off + t * 4 + 3]);
        }
        for (int t = 16; t < 64; t++) {
            uint32_t s0 = rotr32(W[t - 15], 7) ^ rotr32(W[t - 15], 18) ^ (W[t - 15] >> 3);
            uint32_t s1 = rotr32(W[t - 2], 17) ^ rotr32(W[t - 2], 19) ^ (W[t - 2] >> 10);
            W[t] = W[t - 16] + s0 + W[t - 7] + s1;
        }
        uint32_t a = H[0], b = H[1], c = H[2], d = H[3], e = H[4], f = H[5], g = H[6], h = H[7];
        for (int t = 0; t < 64; t++) {
            uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = h + S1 + ch + K[t] + W[t];
            uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        H[0] += a; H[1] += b; H[2] += c; H[3] += d;
        H[4] += e; H[5] += f; H[6] += g; H[7] += h;
    }
    free(msg);

    for (int i = 0; i < 8; i++) {
        digest[i * 4]     = (uint8_t)((H[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = (uint8_t)((H[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = (uint8_t)((H[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = (uint8_t)(H[i] & 0xFF);
    }
}

static uint32_t crc32_compute(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (-(int32_t)(crc & 1U)));
        }
    }
    return ~crc;
}

Value dev_hash(Value input, const char *algo, Arena *arena) {
    const uint8_t *bytes = (const uint8_t *)"";
    size_t blen = 0;
    if (input.type == VAL_SLICE) {
        bytes = (const uint8_t *)input.as.slice.ptr;
        blen = input.as.slice.len;
    } else if (input.type != VAL_NIL) {
        char *s = val_to_string(input, arena);
        bytes = (const uint8_t *)s;
        blen = strlen(s);
    }

    if (algo && strncasecmp(algo, "crc32", 5) == 0) {
        uint32_t c = crc32_compute(bytes, blen);
        char *hex = (char *)arena_alloc(arena, 9);
        snprintf(hex, 9, "%08x", c);
        return val_slice(hex, 8);
    }

    uint8_t digest[32];
    sha256_compute(bytes, blen, digest);
    char *hex = (char *)arena_alloc(arena, 65);
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return val_slice(hex, 64);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Value dev_encode(Value input, const char *fmt, bool decode, Arena *arena) {
    const uint8_t *src = (const uint8_t *)"";
    size_t len = 0;
    if (input.type == VAL_SLICE) {
        src = (const uint8_t *)input.as.slice.ptr;
        len = input.as.slice.len;
    } else if (input.type != VAL_NIL) {
        char *s = val_to_string(input, arena);
        src = (const uint8_t *)s;
        len = strlen(s);
    }

    const char *mode = (fmt && fmt[0]) ? fmt : "base64";

    if (strncasecmp(mode, "hex", 3) == 0) {
        if (!decode) {
            char *out = (char *)arena_alloc(arena, len * 2 + 1);
            for (size_t i = 0; i < len; i++) {
                snprintf(out + i * 2, 3, "%02x", src[i]);
            }
            out[len * 2] = '\0';
            return val_slice(out, len * 2);
        } else {
            char *out = (char *)arena_alloc(arena, len / 2 + 1);
            size_t wp = 0;
            for (size_t i = 0; i + 1 < len; i += 2) {
                int hi = hex_nibble((char)src[i]);
                int lo = hex_nibble((char)src[i + 1]);
                if (hi >= 0 && lo >= 0) out[wp++] = (char)((hi << 4) | lo);
            }
            out[wp] = '\0';
            return val_slice(out, wp);
        }
    }

    if (strncasecmp(mode, "url", 3) == 0) {
        if (!decode) {
            char *out = (char *)arena_alloc(arena, len * 3 + 1);
            size_t wp = 0;
            for (size_t i = 0; i < len; i++) {
                unsigned char c = src[i];
                if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                    out[wp++] = (char)c;
                } else {
                    wp += (size_t)snprintf(out + wp, 4, "%%%02X", c);
                }
            }
            out[wp] = '\0';
            return val_slice(out, wp);
        } else {
            char *out = (char *)arena_alloc(arena, len + 1);
            size_t wp = 0;
            for (size_t i = 0; i < len; i++) {
                if (src[i] == '%' && i + 2 < len) {
                    int hi = hex_nibble((char)src[i + 1]);
                    int lo = hex_nibble((char)src[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        out[wp++] = (char)((hi << 4) | lo);
                        i += 2;
                        continue;
                    }
                } else if (src[i] == '+') {
                    out[wp++] = ' ';
                    continue;
                }
                out[wp++] = (char)src[i];
            }
            out[wp] = '\0';
            return val_slice(out, wp);
        }
    }

    // Default: RFC 4648 Base64
    static const char b64_tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!decode) {
        size_t olen = ((len + 2) / 3) * 4;
        char *out = (char *)arena_alloc(arena, olen + 1);
        size_t wp = 0;
        for (size_t i = 0; i < len; i += 3) {
            uint32_t v = ((uint32_t)src[i]) << 16;
            if (i + 1 < len) v |= ((uint32_t)src[i + 1]) << 8;
            if (i + 2 < len) v |= (uint32_t)src[i + 2];
            out[wp++] = b64_tbl[(v >> 18) & 0x3F];
            out[wp++] = b64_tbl[(v >> 12) & 0x3F];
            out[wp++] = (i + 1 < len) ? b64_tbl[(v >> 6) & 0x3F] : '=';
            out[wp++] = (i + 2 < len) ? b64_tbl[v & 0x3F] : '=';
        }
        out[wp] = '\0';
        return val_slice(out, wp);
    } else {
        int inv[256];
        for (int i = 0; i < 256; i++) inv[i] = -1;
        for (int i = 0; i < 64; i++) inv[(unsigned char)b64_tbl[i]] = i;

        char *out = (char *)arena_alloc(arena, (len * 3) / 4 + 4);
        size_t wp = 0;
        uint32_t buf = 0;
        int bits = 0;
        for (size_t i = 0; i < len; i++) {
            unsigned char c = src[i];
            if (c == '=') break;
            int val = inv[c];
            if (val < 0) continue;
            buf = (buf << 6) | (uint32_t)val;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out[wp++] = (char)((buf >> bits) & 0xFF);
            }
        }
        out[wp] = '\0';
        return val_slice(out, wp);
    }
}

Value dev_uuid(Arena *arena) {
    uint8_t rnd[16];
    bool ok = false;
#ifdef _WIN32
    typedef BOOLEAN (WINAPI *RtlGenRandomFn)(PVOID, ULONG);
    HMODULE adv = LoadLibraryA("advapi32.dll");
    if (adv) {
        RtlGenRandomFn fn = (RtlGenRandomFn)(uintptr_t)GetProcAddress(adv, "SystemFunction036");
        if (fn && fn(rnd, sizeof(rnd))) ok = true;
        FreeLibrary(adv);
    }
#else
    FILE *ur = fopen("/dev/urandom", "rb");
    if (ur) {
        if (fread(rnd, 1, sizeof(rnd), ur) == sizeof(rnd)) ok = true;
        fclose(ur);
    }
#endif
    if (!ok) {
        static uint64_t seq = 0x9E3779B97F4A7C15ULL;
        uint64_t seed = (uint64_t)time(NULL) ^ (++seq * 0xBF58476D1CE4E5B9ULL);
#ifdef _WIN32
        LARGE_INTEGER pc;
        QueryPerformanceCounter(&pc);
        seed ^= (uint64_t)pc.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 16);
#else
        seed ^= (uint64_t)getpid() << 16;
#endif
        for (int i = 0; i < 16; i++) {
            seed ^= seed >> 12;
            seed ^= seed << 25;
            seed ^= seed >> 27;
            rnd[i] = (uint8_t)((seed * 0x2545F4914F6CDD1DULL) >> 56);
        }
    }

    // RFC 4122 version 4 & variant 1
    rnd[6] = (uint8_t)((rnd[6] & 0x0F) | 0x40);
    rnd[8] = (uint8_t)((rnd[8] & 0x3F) | 0x80);

    char *u = (char *)arena_alloc(arena, 37);
    snprintf(u, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7],
             rnd[8], rnd[9], rnd[10], rnd[11], rnd[12], rnd[13], rnd[14], rnd[15]);
    return val_slice(u, 36);
}

// =========================================================
// A6: Port Scanner (`ports`) & Disk Usage (`disk`)
// =========================================================

Value dev_scan_ports(Arena *arena) {
    Value list = val_list_create(arena, 32);
    Value out = val_nil(), err = val_nil();

#ifdef _WIN32
    posix_run_command("netstat -ano", val_nil(), &out, &err, arena);
    if (out.type != VAL_SLICE || out.as.slice.len == 0) return list;

    const char *p = out.as.slice.ptr;
    const char *end = p + out.as.slice.len;
    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n') nl++;
        size_t llen = (size_t)(nl - p);
        if (llen > 0 && p[llen - 1] == '\r') llen--;

        char line[512];
        size_t clen = llen < sizeof(line) - 1 ? llen : sizeof(line) - 1;
        memcpy(line, p, clen);
        line[clen] = '\0';

        char *toks[8];
        int tc = 0;
        char *cur = line;
        while (*cur && tc < 8) {
            while (*cur && isspace((unsigned char)*cur)) cur++;
            if (!*cur) break;
            toks[tc++] = cur;
            while (*cur && !isspace((unsigned char)*cur)) cur++;
            if (*cur) *cur++ = '\0';
        }

        if (tc >= 4 && (strcasecmp(toks[0], "TCP") == 0 || strcasecmp(toks[0], "UDP") == 0)) {
            const char *proto = toks[0];
            const char *local = toks[1];
            const char *state = (strcasecmp(proto, "TCP") == 0 && tc >= 5) ? toks[3] : "BOUND";
            const char *pid_s = (strcasecmp(proto, "TCP") == 0 && tc >= 5) ? toks[4] : toks[3];

            const char *colon = strrchr(local, ':');
            if (colon && isdigit((unsigned char)colon[1])) {
                int64_t port_num = strtoll(colon + 1, NULL, 10);
                int64_t pid_num = strtoll(pid_s, NULL, 10);
                Value m = val_map_create(arena, 6);
                val_map_set_str(&m, "proto", val_cstr(arena_strdup(arena, proto)), arena);
                val_map_set_str(&m, "local", val_cstr(arena_strdup(arena, local)), arena);
                val_map_set_str(&m, "port", val_int(port_num), arena);
                val_map_set_str(&m, "state", val_cstr(arena_strdup(arena, state)), arena);
                val_map_set_str(&m, "pid", val_int(pid_num), arena);
                val_list_append(&list, m, arena);
            }
        }
        p = (nl < end) ? nl + 1 : end;
    }
#else
    posix_run_command("ss -tulnp 2>/dev/null || netstat -an 2>/dev/null", val_nil(), &out, &err, arena);
    if (out.type != VAL_SLICE || out.as.slice.len == 0) return list;

    const char *p = out.as.slice.ptr;
    const char *end = p + out.as.slice.len;
    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n') nl++;
        size_t llen = (size_t)(nl - p);
        if (llen > 0 && p[llen - 1] == '\r') llen--;

        char line[512];
        size_t clen = llen < sizeof(line) - 1 ? llen : sizeof(line) - 1;
        memcpy(line, p, clen);
        line[clen] = '\0';

        char *toks[8];
        int tc = 0;
        char *cur = line;
        while (*cur && tc < 8) {
            while (*cur && isspace((unsigned char)*cur)) cur++;
            if (!*cur) break;
            toks[tc++] = cur;
            while (*cur && !isspace((unsigned char)*cur)) cur++;
            if (*cur) *cur++ = '\0';
        }

        if (tc >= 5 && (strncasecmp(toks[0], "tcp", 3) == 0 || strncasecmp(toks[0], "udp", 3) == 0)) {
            const char *local = toks[4];
            const char *colon = strrchr(local, ':');
            if (!colon) colon = strrchr(local, '.');
            if (colon && isdigit((unsigned char)colon[1])) {
                int64_t port_num = strtoll(colon + 1, NULL, 10);
                int64_t pid_num = 0;
                if (tc >= 7) {
                    const char *pid_tag = strstr(toks[6], "pid=");
                    if (pid_tag) pid_num = strtoll(pid_tag + 4, NULL, 10);
                }
                Value m = val_map_create(arena, 6);
                val_map_set_str(&m, "proto", val_cstr(arena_strdup(arena, toks[0])), arena);
                val_map_set_str(&m, "local", val_cstr(arena_strdup(arena, local)), arena);
                val_map_set_str(&m, "port", val_int(port_num), arena);
                val_map_set_str(&m, "state", val_cstr(arena_strdup(arena, toks[1])), arena);
                val_map_set_str(&m, "pid", val_int(pid_num), arena);
                val_list_append(&list, m, arena);
            }
        }
        p = (nl < end) ? nl + 1 : end;
    }
#endif
    return list;
}

static int64_t sum_dir_bytes(const char *path, int depth) {
    if (depth > 16) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (int64_t)st.st_size;

    int64_t total = 0;
#ifdef _WIN32
    char spec[512];
    snprintf(spec, sizeof(spec), "%s/*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(spec, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total += sum_dir_bytes(child, depth + 1);
        } else {
            total += (int64_t)(((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        total += sum_dir_bytes(child, depth + 1);
    }
    closedir(d);
#endif
    return total;
}

Value dev_disk_usage(const char *path, Arena *arena) {
    if (!path || !path[0]) {
        // Mounted filesystems summary (duf replacement)
        Value list = val_list_create(arena, 8);
#ifdef _WIN32
        char drives[256];
        DWORD n = GetLogicalDriveStringsA(sizeof(drives) - 1, drives);
        char *d = drives;
        while (n > 0 && *d) {
            ULARGE_INTEGER free_avail, total_b, total_free;
            if (GetDiskFreeSpaceExA(d, &free_avail, &total_b, &total_free) && total_b.QuadPart > 0) {
                double tot_gb = (double)total_b.QuadPart / (1024.0 * 1024.0 * 1024.0);
                double fr_gb = (double)total_free.QuadPart / (1024.0 * 1024.0 * 1024.0);
                double us_gb = tot_gb - fr_gb;
                int pct = (int)((us_gb / tot_gb) * 100.0 + 0.5);
                Value m = val_map_create(arena, 6);
                val_map_set_str(&m, "mount", val_cstr(arena_strdup(arena, d)), arena);
                val_map_set_str(&m, "total_gb", val_float(((int)(tot_gb * 10.0)) / 10.0), arena);
                val_map_set_str(&m, "used_gb", val_float(((int)(us_gb * 10.0)) / 10.0), arena);
                val_map_set_str(&m, "free_gb", val_float(((int)(fr_gb * 10.0)) / 10.0), arena);
                val_map_set_str(&m, "use_pct", val_int(pct), arena);
                val_list_append(&list, m, arena);
            }
            d += strlen(d) + 1;
        }
#else
        Value out = val_nil(), err = val_nil();
        posix_run_command("df -kP", val_nil(), &out, &err, arena);
        if (out.type == VAL_SLICE && out.as.slice.len > 0) {
            const char *p = out.as.slice.ptr;
            const char *end = p + out.as.slice.len;
            bool first = true;
            while (p < end) {
                const char *nl = p;
                while (nl < end && *nl != '\n') nl++;
                if (!first) {
                    char line[512];
                    size_t llen = (size_t)(nl - p);
                    size_t clen = llen < sizeof(line) - 1 ? llen : sizeof(line) - 1;
                    memcpy(line, p, clen);
                    line[clen] = '\0';
                    char fs[128], cap_s[32], mnt[256];
                    long long tot_k = 0, used_k = 0, avail_k = 0;
                    if (sscanf(line, "%127s %lld %lld %lld %31s %255s", fs, &tot_k, &used_k, &avail_k, cap_s, mnt) == 6 && tot_k > 0) {
                        Value m = val_map_create(arena, 6);
                        val_map_set_str(&m, "mount", val_cstr(arena_strdup(arena, mnt)), arena);
                        val_map_set_str(&m, "total_gb", val_float(((int)((double)tot_k / (1024.0 * 102.4))) / 10.0), arena);
                        val_map_set_str(&m, "used_gb", val_float(((int)((double)used_k / (1024.0 * 102.4))) / 10.0), arena);
                        val_map_set_str(&m, "free_gb", val_float(((int)((double)avail_k / (1024.0 * 102.4))) / 10.0), arena);
                        val_map_set_str(&m, "use_pct", val_int(atoi(cap_s)), arena);
                        val_list_append(&list, m, arena);
                    }
                }
                first = false;
                p = (nl < end) ? nl + 1 : end;
            }
        }
#endif
        return list;
    }

    // Directory / File recursive breakdown (dust replacement)
    struct stat st;
    if (stat(path, &st) != 0) return val_nil();
    if (!S_ISDIR(st.st_mode)) {
        char hbuf[32];
        format_human_bytes((int64_t)st.st_size, hbuf, sizeof(hbuf));
        Value m = val_map_create(arena, 4);
        val_map_set_str(&m, "path", val_cstr(arena_strdup(arena, path)), arena);
        val_map_set_str(&m, "bytes", val_int((int64_t)st.st_size), arena);
        val_map_set_str(&m, "size", val_cstr(arena_strdup(arena, hbuf)), arena);
        return m;
    }

    char glob_pat[512];
    snprintf(glob_pat, sizeof(glob_pat), "%s/*", path);
    StreamDesc *sd = scanner_open_stream(TOK_SCAN_FILES, glob_pat, 0, arena);
    Value list = val_list_create(arena, 32);
    Value item;
    while (sd && sd->next && sd->next(sd, &item, arena)) {
        if (item.type == VAL_SLICE) {
            char *cp = slice_to_cstr(item.as.slice, arena);
            normalize_slashes(cp);
            int64_t b = sum_dir_bytes(cp, 0);
            char hbuf[32];
            format_human_bytes(b, hbuf, sizeof(hbuf));
            Value m = val_map_create(arena, 4);
            val_map_set_str(&m, "path", val_cstr(arena_strdup(arena, cp)), arena);
            val_map_set_str(&m, "bytes", val_int(b), arena);
            val_map_set_str(&m, "size", val_cstr(arena_strdup(arena, hbuf)), arena);
            val_list_append(&list, m, arena);
        }
    }
    if (sd && sd->close) sd->close(sd);

    // Sort largest first
    size_t cnt = val_list_len(list);
    for (size_t i = 0; i < cnt; i++) {
        for (size_t j = i + 1; j < cnt; j++) {
            Value a = val_list_get(list, i);
            Value b = val_list_get(list, j);
            Value ba = val_int(0), bb = val_int(0);
            val_map_get_str(a, "bytes", &ba);
            val_map_get_str(b, "bytes", &bb);
            if (bb.as.i64 > ba.as.i64) {
                val_list_set(&list, i, b);
                val_list_set(&list, j, a);
            }
        }
    }
    return list;
}
