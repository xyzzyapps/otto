#include "journal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir_p(p) _mkdir(p)
#else
#include <unistd.h>
#define mkdir_p(p) mkdir(p, 0755)
#endif

static char g_journal_root[512] = "";
static char g_current_tx_dir[512] = "";
static bool g_in_tx = false;

static void ensure_dir(const char *dir) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            tmp[i] = '\0';
            mkdir_p(tmp);
            tmp[i] = '/';
        }
    }
    mkdir_p(tmp);
}

void journal_init(void) {
    if (g_journal_root[0]) return;
    const char *home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
    if (!home) home = ".";
    snprintf(g_journal_root, sizeof(g_journal_root), "%s/.otto/journal", home);
    ensure_dir(g_journal_root);
}

void journal_begin_tx(void) {
    journal_init();
    time_t now = time(NULL);
    static int tx_counter = 1;
#ifdef _WIN32
    unsigned long pid = (unsigned long)GetCurrentProcessId();
    unsigned long tick = (unsigned long)GetTickCount();
#else
    unsigned long pid = (unsigned long)getpid();
    unsigned long tick = (unsigned long)clock();
#endif
    snprintf(g_current_tx_dir, sizeof(g_current_tx_dir), "%s/%ld-%lu-%lu-%04d",
             g_journal_root, (long)now, pid, tick, tx_counter++);
    ensure_dir(g_current_tx_dir);
    g_in_tx = true;

    // Save as "latest.txt" pointer in journal root
    char latest_path[512];
    snprintf(latest_path, sizeof(latest_path), "%s/latest.txt", g_journal_root);
    FILE *fp = fopen(latest_path, "w");
    if (fp) {
        fputs(g_current_tx_dir, fp);
        fclose(fp);
    }
}

bool journal_backup_file(const char *filepath) {
    if (!g_in_tx) {
        journal_begin_tx();
    }

    char manifest_path[512];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.bin", g_current_tx_dir);

    FILE *in = fopen(filepath, "rb");
    if (!in) {
        // File doesn't exist yet: record __NEW__ so undo removes newly created file
        FILE *mf = fopen(manifest_path, "a");
        if (mf) {
            fprintf(mf, "%s\t__NEW__\n", filepath);
            fclose(mf);
        }
        return true;
    }

    // Create a safe sanitized unique backup filename inside g_current_tx_dir
    const char *base = strrchr(filepath, '/');
    if (!base) base = strrchr(filepath, '\\');
    base = base ? base + 1 : filepath;

    static int bak_seq = 1;
    char backup_path[512];
    snprintf(backup_path, sizeof(backup_path), "%s/%04d_%s.bak", g_current_tx_dir, bak_seq++, base);

    FILE *out = fopen(backup_path, "wb");
    if (!out) {
        fclose(in);
        return false;
    }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);

    // Append to manifest.bin: original_path \t backup_path \n
    FILE *mf = fopen(manifest_path, "a");
    if (mf) {
        fprintf(mf, "%s\t%s\n", filepath, backup_path);
        fclose(mf);
    }
    return true;
}

void journal_commit_tx(void) {
    g_in_tx = false;
}

bool journal_undo_last(void) {
    journal_init();
    char latest_path[512];
    snprintf(latest_path, sizeof(latest_path), "%s/latest.txt", g_journal_root);
    FILE *fp = fopen(latest_path, "r");
    if (!fp) {
        printf("undo: no previous transaction found\n");
        return false;
    }

    char tx_dir[512];
    if (!fgets(tx_dir, sizeof(tx_dir), fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    size_t len = strlen(tx_dir);
    while (len > 0 && (tx_dir[len - 1] == '\r' || tx_dir[len - 1] == '\n')) tx_dir[--len] = '\0';

    char manifest_path[512];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.bin", tx_dir);
    FILE *mf = fopen(manifest_path, "r");
    if (!mf) {
        printf("undo: empty or missing manifest in %s\n", tx_dir);
        return false;
    }

    int restored_count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), mf)) {
        size_t llen = strlen(line);
        while (llen > 0 && (line[llen - 1] == '\r' || line[llen - 1] == '\n')) line[--llen] = '\0';
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        const char *orig_file = line;
        const char *bak_file = tab + 1;

        if (strcmp(bak_file, "__NEW__") == 0) {
            if (remove(orig_file) == 0) {
                restored_count++;
            }
            continue;
        }

        // Copy bak_file -> orig_file
        FILE *in = fopen(bak_file, "rb");
        if (in) {
            FILE *out = fopen(orig_file, "wb");
            if (out) {
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
                    fwrite(buf, 1, n, out);
                }
                fclose(out);
                restored_count++;
            }
            fclose(in);
        }
    }
    fclose(mf);

    // Remove latest.txt
    remove(latest_path);
    printf("undo: successfully restored %d files from %s\n", restored_count, tx_dir);
    return true;
}

void journal_simulate_diff(const char *filepath, const char *old_content, size_t old_len, const char *new_content, size_t new_len) {
    printf("\033[1;33m[simulate] %s\033[0m\n", filepath);
    // Simple line diff preview
    printf("\033[31m--- %s (original)\033[0m\n", filepath);
    printf("\033[32m+++ %s (simulated)\033[0m\n", filepath);

    const char *o = old_content;
    const char *o_end = old_content + old_len;
    const char *n = new_content;
    const char *n_end = new_content + new_len;

    while (o < o_end || n < n_end) {
        const char *o_nl = o;
        while (o_nl < o_end && *o_nl != '\n') o_nl++;
        size_t o_linelen = o_nl - o;

        const char *n_nl = n;
        while (n_nl < n_end && *n_nl != '\n') n_nl++;
        size_t n_linelen = n_nl - n;

        if (o_linelen != n_linelen || memcmp(o, n, o_linelen) != 0) {
            if (o < o_end) {
                printf("\033[31m- %.*s\033[0m\n", (int)o_linelen, o);
            }
            if (n < n_end) {
                printf("\033[32m+ %.*s\033[0m\n", (int)n_linelen, n);
            }
        }

        o = (o_nl < o_end) ? o_nl + 1 : o_end;
        n = (n_nl < n_end) ? n_nl + 1 : n_end;
    }
}
