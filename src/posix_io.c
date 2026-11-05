#include "posix_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define popen _popen
#define pclose _pclose
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

bool posix_run_command(const char *cmd_line, Value stdin_val, Value *out_acc, Value *out_err, Arena *arena) {
    if (!cmd_line || !cmd_line[0]) return false;

#ifdef _WIN32
    // Create pipes for child stdin and stdout
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hStdInRd = NULL, hStdInWr = NULL;
    HANDLE hStdOutRd = NULL, hStdOutWr = NULL;

    CreatePipe(&hStdInRd, &hStdInWr, &sa, 0);
    SetHandleInformation(hStdInWr, HANDLE_FLAG_INHERIT, 0);

    CreatePipe(&hStdOutRd, &hStdOutWr, &sa, 0);
    SetHandleInformation(hStdOutRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdInput = hStdInRd;
    si.hStdOutput = hStdOutWr;
    si.hStdError = hStdOutWr;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    // Construct command line: cmd.exe /c "..." or raw
    char cmd_buf[2048];
    char normalized[2048];
    strncpy(normalized, cmd_line, sizeof(normalized) - 1);
    normalized[sizeof(normalized) - 1] = '\0';
    if (normalized[0] == '.' && normalized[1] == '/') {
        normalized[1] = '\\';
    }
    snprintf(cmd_buf, sizeof(cmd_buf), "cmd.exe /c %s", normalized);

    if (!CreateProcessA(NULL, cmd_buf, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(hStdInRd); CloseHandle(hStdInWr);
        CloseHandle(hStdOutRd); CloseHandle(hStdOutWr);
        if (out_err) *out_err = val_cstr("Failed to execute process");
        return false;
    }

    CloseHandle(hStdInRd);
    CloseHandle(hStdOutWr);

    // Write stdin if available
    if (stdin_val.type == VAL_SLICE && stdin_val.as.slice.len > 0) {
        DWORD written = 0;
        WriteFile(hStdInWr, stdin_val.as.slice.ptr, (DWORD)stdin_val.as.slice.len, &written, NULL);
    }
    CloseHandle(hStdInWr);

    // Read stdout
    size_t cap = 4096;
    size_t len = 0;
    char *out_buf = (char *)malloc(cap);

    DWORD read_bytes = 0;
    char read_chunk[1024];
    while (ReadFile(hStdOutRd, read_chunk, sizeof(read_chunk), &read_bytes, NULL) && read_bytes > 0) {
        while (len + read_bytes + 1 >= cap) {
            cap *= 2;
            out_buf = (char *)realloc(out_buf, cap);
        }
        memcpy(out_buf + len, read_chunk, read_bytes);
        len += read_bytes;
    }
    CloseHandle(hStdOutRd);
    out_buf[len] = '\0';

    if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Trim trailing newline
    while (len > 0 && (out_buf[len - 1] == '\r' || out_buf[len - 1] == '\n')) {
        out_buf[--len] = '\0';
    }

    char *res = arena_strndup(arena, out_buf, len);
    free(out_buf);

    if (out_acc) *out_acc = val_slice(res, len);

    if (exit_code != 0) {
        if (out_err) *out_err = val_cstr("Process exited with non-zero status");
        return false;
    }
    return true;
#else
    FILE *fp = popen(cmd_line, "r");
    if (!fp) return false;
    char chunk[1024];
    size_t cap = 4096;
    size_t len = 0;
    char *out_buf = (char *)malloc(cap);
    while (fgets(chunk, sizeof(chunk), fp)) {
        size_t clen = strlen(chunk);
        while (len + clen + 1 >= cap) {
            cap *= 2;
            out_buf = (char *)realloc(out_buf, cap);
        }
        memcpy(out_buf + len, chunk, clen);
        len += clen;
    }
    int status = pclose(fp);
    out_buf[len] = '\0';
    while (len > 0 && (out_buf[len - 1] == '\r' || out_buf[len - 1] == '\n')) out_buf[--len] = '\0';
    char *res = arena_strndup(arena, out_buf, len);
    free(out_buf);
    if (out_acc) *out_acc = val_slice(res, len);
    return status == 0;
#endif
}

bool posix_spawn_background(const char *cmd_line, int *out_pid, void **out_handle) {
    if (!cmd_line || !cmd_line[0]) return false;
#ifdef _WIN32
    char cmd_buf[2048];
    char normalized[2048];
    strncpy(normalized, cmd_line, sizeof(normalized) - 1);
    normalized[sizeof(normalized) - 1] = '\0';
    if (normalized[0] == '.' && normalized[1] == '/') {
        normalized[1] = '\\';
    }
    snprintf(cmd_buf, sizeof(cmd_buf), "cmd.exe /c %s", normalized);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, cmd_buf, NULL, NULL, FALSE, CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    if (out_pid) *out_pid = (int)pi.dwProcessId;
    if (out_handle) *out_handle = (void *)pi.hProcess;
    return true;
#else
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd_line, (char *)NULL);
        _exit(127);
    }
    if (out_pid) *out_pid = (int)pid;
    if (out_handle) *out_handle = (void *)(intptr_t)pid;
    return true;
#endif
}

bool posix_build_needs_rebuild(const char *target, const char *src) {
    struct stat st_tgt, st_src;
    char tgt_path[512];
    snprintf(tgt_path, sizeof(tgt_path), "%s", target);

    if (stat(tgt_path, &st_tgt) != 0) {
#ifdef _WIN32
        snprintf(tgt_path, sizeof(tgt_path), "%s.exe", target);
        if (stat(tgt_path, &st_tgt) != 0) {
            return true;
        }
#else
        return true;
#endif
    }
    if (stat(src, &st_src) == 0) {
        if (st_tgt.st_mtime < st_src.st_mtime) {
            return true;
        }
    }
    return false;
}

static Value load_lines_from_val_or_file(Value v, Arena *arena) {
    if (v.type == VAL_LIST) return v;
    if (v.type == VAL_SLICE) {
        char *path = slice_to_cstr(v.as.slice, arena);
        FILE *fp = fopen(path, "r");
        if (fp) {
            Value list = val_list_create(arena, 16);
            char line[1024];
            while (fgets(line, sizeof(line), fp)) {
                size_t len = strlen(line);
                while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) line[--len] = '\0';
                char *s = arena_strndup(arena, line, len);
                val_list_append(&list, val_slice(s, len), arena);
            }
            fclose(fp);
            return list;
        } else {
            // Treat as single string line
            Value list = val_list_create(arena, 1);
            val_list_append(&list, v, arena);
            return list;
        }
    }
    return val_list_create(arena, 0);
}

Value posix_set_exclude(Value blacklist_val, Value candidates_val, Arena *arena) {
    Value blist = load_lines_from_val_or_file(blacklist_val, arena);
    Value clist = load_lines_from_val_or_file(candidates_val, arena);
    Value result = val_list_create(arena, val_list_len(clist));

    size_t ccount = val_list_len(clist);
    size_t bcount = val_list_len(blist);

    for (size_t c = 0; c < ccount; c++) {
        Value citem = val_list_get(clist, c);
        bool in_blacklist = false;
        for (size_t b = 0; b < bcount; b++) {
            Value bitem = val_list_get(blist, b);
            if (val_equal(citem, bitem)) {
                in_blacklist = true;
                break;
            }
        }
        if (!in_blacklist) {
            val_list_append(&result, citem, arena);
        }
    }
    return result;
}

Value posix_set_intersect(Value a_val, Value b_val, Arena *arena) {
    Value alist = load_lines_from_val_or_file(a_val, arena);
    Value blist = load_lines_from_val_or_file(b_val, arena);
    Value result = val_list_create(arena, val_list_len(alist));

    size_t acount = val_list_len(alist);
    size_t bcount = val_list_len(blist);

    for (size_t a = 0; a < acount; a++) {
        Value aitem = val_list_get(alist, a);
        for (size_t b = 0; b < bcount; b++) {
            Value bitem = val_list_get(blist, b);
            if (val_equal(aitem, bitem)) {
                val_list_append(&result, aitem, arena);
                break;
            }
        }
    }
    return result;
}

Value posix_set_zip(Value a, Value b, Arena *arena) {
    size_t len_a = val_list_len(a);
    size_t len_b = val_list_len(b);
    size_t min_len = len_a < len_b ? len_a : len_b;
    Value result = val_list_create(arena, min_len);

    for (size_t i = 0; i < min_len; i++) {
        Value pair = val_list_create(arena, 2);
        val_list_append(&pair, val_list_get(a, i), arena);
        val_list_append(&pair, val_list_get(b, i), arena);
        val_list_append(&result, pair, arena);
    }
    return result;
}

// Unpack binary bytes according to format string: e.g. "u32 u8 u8"
bool posix_unpack_bytes(Value byte_slice, const char *fmt, Value *out_values, size_t max_values, size_t *out_count) {
    if (byte_slice.type != VAL_SLICE) return false;
    const uint8_t *p = (const uint8_t *)byte_slice.as.slice.ptr;
    size_t rem = byte_slice.as.slice.len;
    size_t count = 0;

    const char *f = fmt;
    while (*f && rem > 0 && count < max_values) {
        while (*f && isspace((unsigned char)*f)) f++;
        if (!*f) break;

        if (strncmp(f, "u32", 3) == 0 || strncmp(f, "i32", 3) == 0) {
            if (rem < 4) break;
            uint32_t val = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            out_values[count++] = val_int((int64_t)val);
            p += 4; rem -= 4; f += 3;
        } else if (strncmp(f, "u16", 3) == 0 || strncmp(f, "i16", 3) == 0) {
            if (rem < 2) break;
            uint16_t val = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
            out_values[count++] = val_int((int64_t)val);
            p += 2; rem -= 2; f += 3;
        } else if (strncmp(f, "u8", 2) == 0 || strncmp(f, "i8", 2) == 0) {
            if (rem < 1) break;
            uint8_t val = p[0];
            out_values[count++] = val_int((int64_t)val);
            p += 1; rem -= 1; f += 2;
        } else if (strncmp(f, "u64", 3) == 0 || strncmp(f, "i64", 3) == 0) {
            if (rem < 8) break;
            uint64_t val = 0;
            for (int b = 0; b < 8; b++) val |= ((uint64_t)p[b] << (b * 8));
            out_values[count++] = val_int((int64_t)val);
            p += 8; rem -= 8; f += 3;
        } else {
            f++;
        }
    }

    if (out_count) *out_count = count;
    return count > 0;
}
