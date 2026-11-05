#include "tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <conio.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#include <termios.h>
#endif

bool tui_is_interactive(void) {
    return isatty(fileno(stdin)) && isatty(fileno(stdout));
}

#ifdef _WIN32
static DWORD orig_in_mode = 0;
static DWORD orig_out_mode = 0;
static bool raw_enabled = false;

void tui_enable_raw_mode(void) {
    if (raw_enabled) return;
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleMode(hIn, &orig_in_mode);
    GetConsoleMode(hOut, &orig_out_mode);

    DWORD in_mode = orig_in_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    DWORD out_mode = orig_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hIn, in_mode);
    SetConsoleMode(hOut, out_mode);
    raw_enabled = true;
}

void tui_disable_raw_mode(void) {
    if (!raw_enabled) return;
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleMode(hIn, orig_in_mode);
    SetConsoleMode(hOut, orig_out_mode);
    raw_enabled = false;
}
#else
static struct termios orig_termios;
static bool raw_enabled = false;

void tui_enable_raw_mode(void) {
    if (raw_enabled) return;
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_enabled = true;
}

void tui_disable_raw_mode(void) {
    if (!raw_enabled) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    raw_enabled = false;
}
#endif

// Fuzzy match score
static bool fuzzy_match(const char *pattern, const char *str) {
    if (!pattern || !pattern[0]) return true;
    while (*pattern && *str) {
        if (tolower((unsigned char)*pattern) == tolower((unsigned char)*str)) {
            pattern++;
        }
        str++;
    }
    return *pattern == '\0';
}

Value tui_pick(Value items, Arena *arena) {
    if (items.type != VAL_LIST || val_list_len(items) == 0) {
        return items;
    }

    size_t count = val_list_len(items);
    if (!tui_is_interactive()) {
        // Non-interactive fallback: pick first item
        return val_list_get(items, 0);
    }

    tui_enable_raw_mode();

    char query[128] = "";
    size_t qlen = 0;
    int selected = 0;

    // Filtered indices
    int *matches = (int *)malloc(count * sizeof(int));

    while (1) {
        int match_count = 0;
        for (size_t i = 0; i < count; i++) {
            Value item = val_list_get(items, i);
            char *s = val_to_string(item, arena);
            if (fuzzy_match(query, s)) {
                matches[match_count++] = (int)i;
            }
        }

        if (selected >= match_count) selected = match_count > 0 ? match_count - 1 : 0;
        if (selected < 0) selected = 0;

        // Render prompt and visible items (up to 10)
        fprintf(stderr, "\r\033[K\033[1;36m> %s\033[0m\n", query);
        int max_show = match_count < 10 ? match_count : 10;
        for (int i = 0; i < max_show; i++) {
            int idx = matches[i];
            Value item = val_list_get(items, (size_t)idx);
            char *s = val_to_string(item, arena);
            if (i == selected) {
                fprintf(stderr, "\r\033[K\033[1;32m> %s\033[0m\n", s);
            } else {
                fprintf(stderr, "\r\033[K  %s\n", s);
            }
        }
        for (int i = max_show; i < 10; i++) {
            fprintf(stderr, "\r\033[K\n");
        }

        // Move cursor back up (11 lines)
        fprintf(stderr, "\033[11A\r");

        // Read key
#ifdef _WIN32
        int ch = _getch();
        if (ch == 224 || ch == 0) {
            int arrow = _getch();
            if (arrow == 72) { // Up
                if (selected > 0) selected--;
            } else if (arrow == 80) { // Down
                if (selected + 1 < match_count) selected++;
            }
            continue;
        }
#else
        char ch_buf[4];
        int n = read(STDIN_FILENO, ch_buf, sizeof(ch_buf));
        int ch = (n > 0) ? (unsigned char)ch_buf[0] : 0;
        if (ch == 27 && n >= 3 && ch_buf[1] == '[') {
            if (ch_buf[2] == 'A') { // Up
                if (selected > 0) selected--;
            } else if (ch_buf[2] == 'B') { // Down
                if (selected + 1 < match_count) selected++;
            }
            continue;
        }
#endif

        if (ch == 13 || ch == 10) { // Enter
            break;
        } else if (ch == 27 || ch == 3) { // Escape or Ctrl-C
            selected = -1;
            break;
        } else if (ch == 8 || ch == 127) { // Backspace
            if (qlen > 0) query[--qlen] = '\0';
        } else if (ch >= 32 && ch <= 126 && qlen + 1 < sizeof(query)) {
            query[qlen++] = (char)ch;
            query[qlen] = '\0';
        }
    }

    // Clear the 11 lines
    for (int i = 0; i <= 10; i++) {
        fprintf(stderr, "\r\033[K\n");
    }
    fprintf(stderr, "\033[11A\r");

    tui_disable_raw_mode();

    Value chosen = val_nil();
    if (selected >= 0) {
        int chosen_idx = matches[selected];
        chosen = val_list_get(items, (size_t)chosen_idx);
    }
    free(matches);
    return chosen;
}

void tui_table(Value items, FILE *out) {
    if (!out) out = stdout;
    if (items.type == VAL_MAP) {
        Arena single_arena;
        arena_init(&single_arena, 4096);
        Value wrap = val_list_create(&single_arena, 1);
        val_list_append(&wrap, items, &single_arena);
        tui_table(wrap, out);
        arena_free(&single_arena);
        return;
    }
    if (items.type != VAL_LIST || val_list_len(items) == 0) {
        val_println(items, out);
        return;
    }

    size_t count = val_list_len(items);
    Value first = val_list_get(items, 0);

    // If list of maps, display columns
    if (first.type == VAL_MAP && first.as.map) {
        ValueMap *m0 = first.as.map;
        char *col_names[32];
        size_t col_widths[32];
        size_t num_cols = 0;

        for (size_t i = 0; i < m0->capacity && num_cols < 32; i++) {
            if (m0->entries[i].occupied) {
                size_t klen = m0->entries[i].key.len;
                char *name = (char *)malloc(klen + 1);
                memcpy(name, m0->entries[i].key.ptr, klen);
                name[klen] = '\0';
                col_names[num_cols] = name;
                col_widths[num_cols] = klen > 4 ? klen : 4;
                num_cols++;
            }
        }

        // Calculate max column widths
        Arena temp_arena;
        arena_init(&temp_arena, 32 * 1024);

        for (size_t r = 0; r < count; r++) {
            Value row = val_list_get(items, r);
            if (row.type == VAL_MAP) {
                for (size_t c = 0; c < num_cols; c++) {
                    Value cell;
                    if (val_map_get_str(row, col_names[c], &cell)) {
                        char *s = val_to_string(cell, &temp_arena);
                        size_t slen = strlen(s);
                        if (slen > col_widths[c]) col_widths[c] = slen < 40 ? slen : 40;
                    }
                }
            }
        }

        // Print header top border
        fputs("+", out);
        for (size_t c = 0; c < num_cols; c++) {
            for (size_t k = 0; k < col_widths[c] + 2; k++) fputc('-', out);
            fputs("+", out);
        }
        fputc('\n', out);

        // Print column headers
        fputs("|", out);
        for (size_t c = 0; c < num_cols; c++) {
            fprintf(out, " %-*s |", (int)col_widths[c], col_names[c]);
        }
        fputc('\n', out);

        // Header separator
        fputs("+", out);
        for (size_t c = 0; c < num_cols; c++) {
            for (size_t k = 0; k < col_widths[c] + 2; k++) fputc('=', out);
            fputs("+", out);
        }
        fputc('\n', out);

        // Print rows
        for (size_t r = 0; r < count; r++) {
            Value row = val_list_get(items, r);
            fputs("|", out);
            for (size_t c = 0; c < num_cols; c++) {
                Value cell = val_nil();
                val_map_get_str(row, col_names[c], &cell);
                char *s = val_to_string(cell, &temp_arena);
                fprintf(out, " %-*.*s |", (int)col_widths[c], (int)col_widths[c], s);
            }
            fputc('\n', out);
        }

        // Bottom border
        fputs("+", out);
        for (size_t c = 0; c < num_cols; c++) {
            for (size_t k = 0; k < col_widths[c] + 2; k++) fputc('-', out);
            fputs("+", out);
        }
        fputc('\n', out);

        for (size_t c = 0; c < num_cols; c++) free(col_names[c]);
        arena_free(&temp_arena);
    } else {
        // Simple list table
        for (size_t i = 0; i < count; i++) {
            val_println(val_list_get(items, i), out);
        }
    }
}

void tui_chart(Value items, const char *kind, FILE *out) {
    if (!out) out = stdout;
    if (items.type != VAL_LIST || val_list_len(items) == 0) return;

    size_t count = val_list_len(items);
    static const char *bars[] = {" ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"};
    static const char *sparks[] = {" ", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

    double max_val = 0.0;
    for (size_t i = 0; i < count; i++) {
        Value it = val_list_get(items, i);
        double val = (it.type == VAL_INT) ? (double)it.as.i64 : ((it.type == VAL_FLOAT) ? it.as.f64 : 0.0);
        if (val > max_val) max_val = val;
    }
    if (max_val <= 0.0) max_val = 1.0;

    if (kind && strcmp(kind, "bar") == 0) {
        // Horizontal bar histogram with 1/8th sub-character precision
        for (size_t i = 0; i < count; i++) {
            Value it = val_list_get(items, i);
            double val = (it.type == VAL_INT) ? (double)it.as.i64 : ((it.type == VAL_FLOAT) ? it.as.f64 : 0.0);
            double scaled = (val / max_val) * 40.0;
            if (scaled < 0.0) scaled = 0.0;
            int full_blocks = (int)scaled;
            int frac_idx = (int)((scaled - (double)full_blocks) * 8.0);
            fprintf(out, "%3zu | ", i);
            for (int b = 0; b < full_blocks; b++) fputs(bars[8], out);
            if (frac_idx > 0 && frac_idx < 8) fputs(bars[frac_idx], out);
            fprintf(out, " (%g)\n", val);
        }
    } else {
        // Inline sparkline
        for (size_t i = 0; i < count; i++) {
            Value it = val_list_get(items, i);
            double val = (it.type == VAL_INT) ? (double)it.as.i64 : ((it.type == VAL_FLOAT) ? it.as.f64 : 0.0);
            int idx = (int)((val / max_val) * 7.0);
            if (idx < 0) idx = 0;
            if (idx > 7) idx = 7;
            fputs(sparks[idx], out);
        }
        fputc('\n', out);
    }
}
