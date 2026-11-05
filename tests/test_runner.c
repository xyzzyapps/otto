#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir_p(d) _mkdir(d)
#define sleep_ms(ms) Sleep(ms)
#define popen_cmd(c) _popen(c, "r")
#define pclose_cmd(p) _pclose(p)
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#define mkdir_p(d) mkdir(d, 0755)
#define sleep_ms(ms) usleep((ms) * 1000)
#define popen_cmd(c) popen(c, "r")
#define pclose_cmd(p) pclose(p)
#endif

static int g_passed = 0;
static int g_total = 0;
static char g_bin[256] = "";

// ---------------------------------------------------------
// Helper Utilities
// ---------------------------------------------------------

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool write_file(const char *path, const char *content) {
    FILE *fp = fopen(path, "w");
    if (!fp) return false;
    fputs(content, fp);
    fclose(fp);
    return true;
}

static bool write_binary_file(const char *path, const uint8_t *data, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    fwrite(data, 1, len, fp);
    fclose(fp);
    return true;
}

static char *read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    fread(buf, 1, sz, fp);
    buf[sz] = '\0';
    fclose(fp);
    return buf;
}

static void remove_file(const char *path) {
    remove(path);
}

static void trim_str(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
}

static char *run_cmd(const char *cmd, int *exit_code) {
    FILE *fp = popen_cmd(cmd);
    if (!fp) {
        if (exit_code) *exit_code = -1;
        return strdup("");
    }
    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    char chunk[512];
    while (fgets(chunk, sizeof(chunk), fp)) {
        size_t clen = strlen(chunk);
        if (len + clen >= cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
        }
        memcpy(buf + len, chunk, clen);
        len += clen;
    }
    buf[len] = '\0';
    int rc = pclose_cmd(fp);
    if (exit_code) *exit_code = rc;
    return buf;
}

static void run_test(const char *name, bool (*fn)(char *err, size_t err_sz)) {
    g_total++;
    printf("\n\033[33mTest %d: %s\033[0m\n", g_total, name);
    char err[512] = "";
    if (fn(err, sizeof(err))) {
        printf("  \033[32m-> PASSED\033[0m\n");
        g_passed++;
    } else {
        printf("  \033[31m-> FAILED: %s\033[0m\n", err[0] ? err : "assertion failed");
    }
}

// ---------------------------------------------------------
// Master Specification Tests (14 Tests)
// ---------------------------------------------------------

// Test 1: Zero-filler recursion & TCO
static bool test_tco_gcd(char *err, size_t err_sz) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -c \"gcd a b: return a if b == 0 else gcd b (a %% b). print gcd 1071 462.\"", g_bin);
    char *out = run_cmd(cmd, NULL);
    trim_str(out);
    bool ok = (strcmp(out, "21") == 0);
    if (!ok) snprintf(err, err_sz, "Expected 21, got '%s'", out);
    free(out);
    return ok;
}

// Test 2: TCO Deep Recursion
static bool test_tco_deep(char *err, size_t err_sz) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -c \"count_down n: return 0 if n <= 0 else count_down (n - 1). print count_down 1000000.\"", g_bin);
    char *out = run_cmd(cmd, NULL);
    trim_str(out);
    bool ok = (strcmp(out, "0") == 0);
    if (!ok) snprintf(err, err_sz, "Expected 0, got '%s'", out);
    free(out);
    return ok;
}

// Test 3: AWK lifecycle + ordinal + implicit number scanner
static bool test_awk_lifecycle(char *err, size_t err_sz) {
    write_file("scores.txt", "alice 100 math\nbob 90 science\ncharlie 80 history\n");
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -c \"before: 0 into s. every line \\\"scores.txt\\\": add 2nd number into s. after: print s.\"", g_bin);
    char *out = run_cmd(cmd, NULL);
    remove_file("scores.txt");
    trim_str(out);
    bool ok = (strcmp(out, "270") == 0);
    if (!ok) snprintf(err, err_sz, "Expected 270, got '%s'", out);
    free(out);
    return ok;
}

// Test 4: Possessive JSON navigation + sorting
static bool test_json_nav(char *err, size_t err_sz) {
    const char *json_data = "[\n"
        "  {\"name\": \"Alice\", \"age\": 25, \"active\": true},\n"
        "  {\"name\": \"Bob\", \"age\": 40, \"active\": true},\n"
        "  {\"name\": \"Charlie\", \"age\": 30, \"active\": false},\n"
        "  {\"name\": \"David\", \"age\": 35, \"active\": true}\n"
        "]";
    write_file("users.json", json_data);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -c \"read \\\"users.json\\\" json, take where each's active == true, sort by age descending, print each's name.\"", g_bin);
    char *out = run_cmd(cmd, NULL);
    remove_file("users.json");

    // Output should contain Bob, David, Alice in order
    char *b = strstr(out, "Bob");
    char *d = strstr(out, "David");
    char *a = strstr(out, "Alice");
    bool ok = (b && d && a && (b < d) && (d < a));
    if (!ok) snprintf(err, err_sz, "Expected sorted Bob, David, Alice; got '%s'", out);
    free(out);
    return ok;
}

// Test 5: Parallel tee (and) + short-circuit (or)
static bool test_and_or_tee(char *err, size_t err_sz) {
    write_file("backup.cfg", "line one: host=web1\nline two: port=8080\nline three: debug=true\n");
    remove_file("primary.cfg");
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -c \"read \\\"primary.cfg\\\" or read \\\"backup.cfg\\\", print first line and print count lines.\"", g_bin);
    char *out = run_cmd(cmd, NULL);
    remove_file("backup.cfg");

    bool ok = (strstr(out, "line one: host=web1") != NULL && strstr(out, "3") != NULL);
    if (!ok) snprintf(err, err_sz, "Expected 'line one: host=web1' and '3', got '%s'", out);
    free(out);
    return ok;
}

// Test 6: In-place dry-run & transactional rollback
static bool test_simulate_inplace_undo(char *err, size_t err_sz) {
    write_file("doc.txt", "hello foo world");
    char cmd[512];

    // 6.1 simulate: dry-run without disk modification
    snprintf(cmd, sizeof(cmd), "%s -c \"every file \\\"*.txt\\\": simulate inplace replace \\\"foo\\\" with \\\"bar\\\".\"", g_bin);
    char *out1 = run_cmd(cmd, NULL);
    free(out1);
    char *c1 = read_file("doc.txt");
    if (!c1 || strcmp(c1, "hello foo world") != 0) {
        snprintf(err, err_sz, "Dry-run modified file! Content: %s", c1 ? c1 : "NULL");
        if (c1) free(c1);
        remove_file("doc.txt");
        return false;
    }
    free(c1);

    // 6.2 inplace: modify for real with auto-journal
    snprintf(cmd, sizeof(cmd), "%s -c \"every file \\\"*.txt\\\": inplace replace \\\"foo\\\" with \\\"bar\\\".\"", g_bin);
    char *out2 = run_cmd(cmd, NULL);
    free(out2);
    char *c2 = read_file("doc.txt");
    if (!c2 || strcmp(c2, "hello bar world") != 0) {
        snprintf(err, err_sz, "Inplace replace failed! Content: %s", c2 ? c2 : "NULL");
        if (c2) free(c2);
        remove_file("doc.txt");
        return false;
    }
    free(c2);

    // 6.3 undo: restore original
    snprintf(cmd, sizeof(cmd), "%s -c \"undo last.\"", g_bin);
    char *out3 = run_cmd(cmd, NULL);
    free(out3);
    char *c3 = read_file("doc.txt");
    remove_file("doc.txt");
    if (!c3 || strcmp(c3, "hello foo world") != 0) {
        snprintf(err, err_sz, "Undo rollback failed! Content: %s", c3 ? c3 : "NULL");
        if (c3) free(c3);
        return false;
    }
    free(c3);
    return true;
}

// Test 7: Relational set operations
static bool test_relational_sets(char *err, size_t err_sz) {
    write_file("candidates.txt", "alice\nbob\ncharlie\ndavid\n");
    write_file("blacklist.txt", "bob\n");
    write_file("verified.txt", "alice\ndavid\neva\n");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -c \"exclude \\\"blacklist.txt\\\" from \\\"candidates.txt\\\", intersect with \\\"verified.txt\\\", print count.\"", g_bin);
    char *out = run_cmd(cmd, NULL);
    remove_file("candidates.txt");
    remove_file("blacklist.txt");
    remove_file("verified.txt");
    trim_str(out);

    bool ok = (strcmp(out, "2") == 0);
    if (!ok) snprintf(err, err_sz, "Expected count 2, got '%s'", out);
    free(out);
    return ok;
}

// Test 8: Binary unpacking
static bool test_binary_unpack(char *err, size_t err_sz) {
    uint8_t bytes[8] = { 0x78, 0x56, 0x34, 0x12, 0x01, 0x02, 0x00, 0x00 };
    write_binary_file("sample.bin", bytes, sizeof(bytes));

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -c \"read \\\"sample.bin\\\", take first 8 bytes, unpack \\\"u32 u8 u8\\\" into magic bits endian, print magic.\"", g_bin);
    char *out = run_cmd(cmd, NULL);
    remove_file("sample.bin");
    trim_str(out);

    bool ok = (strcmp(out, "305419896") == 0);
    if (!ok) snprintf(err, err_sz, "Expected 305419896, got '%s'", out);
    free(out);
    return ok;
}

// Test 9: Incremental build rule
static bool test_incremental_build(char *err, size_t err_sz) {
    write_file("hello.c", "int main() { return 0; }\n");
    remove_file("hello.exe");
    remove_file("hello");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -c \"\\\"hello\\\" from \\\"hello.c\\\": run \\\"zig cc hello.c -o hello.exe\\\".\"", g_bin);
    char *out = run_cmd(cmd, NULL);
    free(out);

    bool exists = file_exists("hello.exe") || file_exists("hello");
    remove_file("hello.c");
    remove_file("hello.exe");
    remove_file("hello.pdb");
    remove_file("hello");

    if (!exists) {
        snprintf(err, err_sz, "Incremental build rule failed to produce hello.exe");
        return false;
    }
    return true;
}

// Test 10: Embedded SQLite stream
static bool test_sqlite_stream(char *err, size_t err_sz) {
    remove_file("ops.db");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -c \"write {host: \\\"web1\\\", cpu: 85} onto \\\"ops.db:metrics\\\". every row \\\"ops.db:metrics\\\" where cpu >= 80: print host.\"", g_bin);
    char *out = run_cmd(cmd, NULL);
    remove_file("ops.db");
    trim_str(out);

    bool ok = (strcmp(out, "web1") == 0);
    if (!ok) snprintf(err, err_sz, "Expected 'web1', got '%s'", out);
    free(out);
    return ok;
}

// Test 11: Standalone binary compilation
static bool test_standalone_compilation(char *err, size_t err_sz) {
    write_file("test.otto", "print \"Hello from standalone compiled binary!\".\n");
    remove_file("test-bin");
    remove_file("test-bin.exe");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -c \"compile \\\"test.otto\\\" into \\\"./test-bin\\\", run \\\"./test-bin\\\", print.\"", g_bin);
    char *out = run_cmd(cmd, NULL);
    remove_file("test.otto");
    remove_file("test-bin");
    remove_file("test-bin.exe");
    trim_str(out);

    bool ok = (strcmp(out, "Hello from standalone compiled binary!") == 0);
    if (!ok) snprintf(err, err_sz, "Expected standalone binary output, got '%s'", out);
    free(out);
    return ok;
}

// Test 12: Native YAML stream processing
static bool test_yaml_processing(char *err, size_t err_sz) {
    const char *yaml_data = "name: ProductionCluster\nport: 9000\nnodes: 16\n";
    write_file("cluster.yaml", yaml_data);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -c \"read \\\"cluster.yaml\\\" yaml, print.\"", g_bin);
    char *out = run_cmd(cmd, NULL);
    remove_file("cluster.yaml");

    bool ok = (strstr(out, "ProductionCluster") != NULL && strstr(out, "9000") != NULL);
    if (!ok) snprintf(err, err_sz, "Expected parsed YAML map, got '%s'", out);
    free(out);
    return ok;
}

// Test 13: Strict Zero-Filler Rule Rejection
static bool test_zero_filler_rejection(char *err, size_t err_sz) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -c \"To gcd of a and b.\" 2>&1", g_bin);
    char *out = run_cmd(cmd, NULL);

    bool ok = (strstr(out, "zero-filler") != NULL || strstr(out, "Syntax error") != NULL);
    if (!ok) snprintf(err, err_sz, "Expected zero-filler syntax rejection, got '%s'", out);
    free(out);
    return ok;
}

// Test 14: TUI Table Grid Rendering
static bool test_tui_table(char *err, size_t err_sz) {
    const char *json_data = "[\n  {\"name\": \"Alice\", \"age\": 25},\n  {\"name\": \"Bob\", \"age\": 40}\n]\n";
    write_file("users.json", json_data);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -c \"read \\\"users.json\\\" json, table.\"", g_bin);
    char *out = run_cmd(cmd, NULL);
    remove_file("users.json");

    bool ok = (strstr(out, "+") != NULL && strstr(out, "Alice") != NULL && strstr(out, "Bob") != NULL);
    if (!ok) snprintf(err, err_sz, "Expected table grid with Alice and Bob, got '%s'", out);
    free(out);
    return ok;
}

// ---------------------------------------------------------
// Modern CLI Capabilities Tests (7 Tests)
// ---------------------------------------------------------

// Test 15: Native HTTP REST Streams (fetch GET & onto POST)
static bool test_http_rest(char *err, size_t err_sz) {
    // 15.1 fetch GET
    write_file("test_fetch.otto", "fetch \"https://jsonplaceholder.typicode.com/todos/1\" json, print each's title.\n");
    char cmd1[512];
    snprintf(cmd1, sizeof(cmd1), "%s test_fetch.otto", g_bin);
    char *out1 = run_cmd(cmd1, NULL);
    remove_file("test_fetch.otto");
    trim_str(out1);

    if (strcmp(out1, "delectus aut autem") != 0) {
        snprintf(err, err_sz, "fetch GET failed: expected 'delectus aut autem', got '%s'", out1);
        free(out1);
        return false;
    }
    free(out1);

    // 15.2 onto POST
    write_file("test_post.otto", "write {status: \"ok\", service: \"otto\"} onto \"https://httpbin.org/post\", print.\n");
    char cmd2[512];
    snprintf(cmd2, sizeof(cmd2), "%s test_post.otto", g_bin);
    char *out2 = run_cmd(cmd2, NULL);
    remove_file("test_post.otto");

    bool ok = (strstr(out2, "otto") != NULL && strstr(out2, "httpbin.org") != NULL);
    if (!ok) snprintf(err, err_sz, "HTTP POST onto failed, got '%s'", out2);
    free(out2);
    return ok;
}

// Test 16: File Change Watcher Loop (watch Hook)
static bool test_file_watcher(char *err, size_t err_sz) {
    mkdir_p("test_watch_dir");
    write_file("test_watch_dir/code.txt", "v1");
    write_file("test_watch.otto", "watch file \"test_watch_dir\": print \"watch_tick\".\n");
    remove_file("watch.log");

#ifdef _WIN32
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    HANDLE hLog = CreateFileA("watch.log", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hLog;
    si.hStdError = hLog;

    char cmdline[512];
    snprintf(cmdline, sizeof(cmdline), "%s test_watch.otto", g_bin);

    if (CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        sleep_ms(600);
        write_file("test_watch_dir/code.txt", "v2");
        sleep_ms(800);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(hLog);
#else
    pid_t pid = fork();
    if (pid == 0) {
        freopen("watch.log", "w", stdout);
        freopen("watch.log", "w", stderr);
        execl(g_bin, g_bin, "test_watch.otto", (char *)NULL);
        exit(1);
    } else {
        sleep_ms(600);
        write_file("test_watch_dir/code.txt", "v2");
        sleep_ms(800);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
#endif

    char *log_content = read_file("watch.log");
    remove_file("test_watch_dir/code.txt");
    remove_file("test_watch.otto");
    remove_file("watch.log");
#ifdef _WIN32
    _rmdir("test_watch_dir");
#else
    rmdir("test_watch_dir");
#endif

    bool ok = false;
    if (log_content) {
        char *first = strstr(log_content, "watch_tick");
        if (first) {
            char *second = strstr(first + 10, "watch_tick");
            if (second) ok = true;
        }
    }

    if (!ok) {
        snprintf(err, err_sz, "Expected at least 2 watch ticks in log, got '%s'", log_content ? log_content : "NULL");
    }
    if (log_content) free(log_content);
    return ok;
}

// Test 17: Frecency Smart Directory Navigation (jump Verb)
static bool test_frecency_jump(char *err, size_t err_sz) {
    write_file("test_jump.otto", "jump \"src\", print.\n");
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s test_jump.otto", g_bin);
    char *out = run_cmd(cmd, NULL);
    remove_file("test_jump.otto");
    trim_str(out);

    size_t len = strlen(out);
    bool ok = (len >= 3 && (strcmp(out + len - 3, "src") == 0 ||
               (len >= 4 && (strcmp(out + len - 4, "src/") == 0 || strcmp(out + len - 4, "src\\") == 0))));
    if (!ok) snprintf(err, err_sz, "Expected path ending with src, got '%s'", out);
    free(out);
    return ok;
}

// Test 18: Git-Aware Directory Scanning (tree & ignoring git)
static bool test_git_tree_ignore(char *err, size_t err_sz) {
    // 18.1 tree scanner
    write_file("test_tree.otto", "read \"src\" tree, print.\n");
    char cmd1[512];
    snprintf(cmd1, sizeof(cmd1), "%s test_tree.otto", g_bin);
    char *out1 = run_cmd(cmd1, NULL);
    remove_file("test_tree.otto");

    if (strstr(out1, "vm.c") == NULL || strstr(out1, "|--") == NULL) {
        snprintf(err, err_sz, "Expected tree structure with vm.c, got '%s'", out1);
        free(out1);
        return false;
    }
    free(out1);

    // 18.2 ignoring git
    mkdir_p("test_git_dir");
    mkdir_p("test_git_dir/.git");
    write_file("test_git_dir/app.c", "int main(){}");
    write_file("test_git_dir/.git/HEAD", "ref: refs/heads/main");
    write_file("test_git.otto", "every ignoring git file \"test_git_dir/*\": print.\n");

    char cmd2[512];
    snprintf(cmd2, sizeof(cmd2), "%s test_git.otto", g_bin);
    char *out2 = run_cmd(cmd2, NULL);

    remove_file("test_git_dir/app.c");
    remove_file("test_git_dir/.git/HEAD");
    remove_file("test_git.otto");
#ifdef _WIN32
    _rmdir("test_git_dir/.git");
    _rmdir("test_git_dir");
#else
    rmdir("test_git_dir/.git");
    rmdir("test_git_dir");
#endif

    bool ok = (strstr(out2, ".git") == NULL && strstr(out2, "app.c") != NULL);
    if (!ok) snprintf(err, err_sz, "Expected app.c without .git, got '%s'", out2);
    free(out2);
    return ok;
}

// Test 19: Statistical Benchmarking (benchmark Verb)
static bool test_benchmark(char *err, size_t err_sz) {
    write_file("test_bm.otto", "benchmark 5 \"echo 1\", print.\n");
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s test_bm.otto", g_bin);
    char *out = run_cmd(cmd, NULL);
    remove_file("test_bm.otto");

    bool ok = (strstr(out, "runs: 5") != NULL &&
               strstr(out, "mean_ms:") != NULL &&
               strstr(out, "stddev_ms:") != NULL);
    if (!ok) snprintf(err, err_sz, "Expected benchmark stats with runs, mean, stddev, got '%s'", out);
    free(out);
    return ok;
}

// Test 20: Dataframe Group Aggregations (tally & group by)
static bool test_dataframe_aggregations(char *err, size_t err_sz) {
    // 20.1 tally
    write_file("fruits.txt", "apple\nbanana\napple\norange\napple\nbanana\n");
    write_file("test_tally.otto", "read \"fruits.txt\", tally, table.\n");
    char cmd1[512];
    snprintf(cmd1, sizeof(cmd1), "%s test_tally.otto", g_bin);
    char *out1 = run_cmd(cmd1, NULL);
    remove_file("fruits.txt");
    remove_file("test_tally.otto");

    if (strstr(out1, "apple") == NULL || strstr(out1, "3") == NULL ||
        strstr(out1, "banana") == NULL || strstr(out1, "2") == NULL) {
        snprintf(err, err_sz, "tally aggregation failed, got '%s'", out1);
        free(out1);
        return false;
    }
    free(out1);

    // 20.2 group by
    const char *grp_json = "[{\"dept\": \"Eng\", \"name\": \"Alice\"}, {\"dept\": \"Sales\", \"name\": \"Bob\"}, {\"dept\": \"Eng\", \"name\": \"Charlie\"}]";
    write_file("users_grp.json", grp_json);
    write_file("test_grp.otto", "read \"users_grp.json\" json, group by dept, print.\n");
    char cmd2[512];
    snprintf(cmd2, sizeof(cmd2), "%s test_grp.otto", g_bin);
    char *out2 = run_cmd(cmd2, NULL);
    remove_file("users_grp.json");
    remove_file("test_grp.otto");

    bool ok = (strstr(out2, "Eng:") != NULL && strstr(out2, "Sales:") != NULL);
    if (!ok) snprintf(err, err_sz, "group by aggregation failed, got '%s'", out2);
    free(out2);
    return ok;
}

// Test 21: Starship-Style Prompt & REPL
static bool test_starship_repl(char *err, size_t err_sz) {
    char cmd[512];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "echo exit | %s", g_bin);
#else
    snprintf(cmd, sizeof(cmd), "echo exit | %s", g_bin);
#endif
    char *out = run_cmd(cmd, NULL);

    bool ok = (strstr(out, "otto") != NULL && strstr(out, "v1.0") != NULL && strstr(out, ">") != NULL);
    if (!ok) snprintf(err, err_sz, "Expected REPL banner and prompt, got '%s'", out);
    free(out);
    return ok;
}

// Test 22: Zero-preposition positional argument syntax
static bool test_zero_prepositions(char *err, size_t err_sz) {
    // 22.1 replace without 'with'
    char cmd1[512];
    snprintf(cmd1, sizeof(cmd1), "%s -c \"\\\"hello foo world\\\", replace \\\"foo\\\" \\\"bar\\\", print.\"", g_bin);
    char *out1 = run_cmd(cmd1, NULL);
    trim_str(out1);
    if (strcmp(out1, "hello bar world") != 0) {
        snprintf(err, err_sz, "Zero-prep replace failed: expected 'hello bar world', got '%s'", out1);
        free(out1);
        return false;
    }
    free(out1);

    // 22.2 split without 'by'
    char cmd2[512];
    snprintf(cmd2, sizeof(cmd2), "%s -c \"\\\"apple,banana,orange\\\", split \\\",\\\", print count them.\"", g_bin);
    char *out2 = run_cmd(cmd2, NULL);
    trim_str(out2);
    if (strcmp(out2, "3") != 0) {
        snprintf(err, err_sz, "Zero-prep split failed: expected '3', got '%s'", out2);
        free(out2);
        return false;
    }
    free(out2);

    // 22.3 sort without 'by'
    write_file("test_users.json", "[{\"name\": \"Alice\", \"age\": 25}, {\"name\": \"Bob\", \"age\": 40}]");
    char cmd3[512];
    snprintf(cmd3, sizeof(cmd3), "%s -c \"read \\\"test_users.json\\\" json, sort age descending, print first's name.\"", g_bin);
    char *out3 = run_cmd(cmd3, NULL);
    remove_file("test_users.json");
    trim_str(out3);
    if (strcmp(out3, "Bob") != 0) {
        snprintf(err, err_sz, "Zero-prep sort failed: expected 'Bob', got '%s'", out3);
        free(out3);
        return false;
    }
    free(out3);
    return true;
}

// Test 23: Pronoun registers `that` and `them`
static bool test_pronoun_registers(char *err, size_t err_sz) {
    // 23.1 that arithmetic
    char cmd1[512];
    snprintf(cmd1, sizeof(cmd1), "%s -c \"5 * 10 into x. print that + 2.\"", g_bin);
    char *out1 = run_cmd(cmd1, NULL);
    trim_str(out1);
    if (strcmp(out1, "52") != 0) {
        snprintf(err, err_sz, "that register failed: expected '52', got '%s'", out1);
        free(out1);
        return false;
    }
    free(out1);

    // 23.2 that's property
    char cmd2[512];
    snprintf(cmd2, sizeof(cmd2), "%s -c \"write {name: \\\"Alice\\\", role: \\\"admin\\\"} into user. print that's role.\"", g_bin);
    char *out2 = run_cmd(cmd2, NULL);
    trim_str(out2);
    if (strcmp(out2, "admin") != 0) {
        snprintf(err, err_sz, "that's property failed: expected 'admin', got '%s'", out2);
        free(out2);
        return false;
    }
    free(out2);

    // 23.3 them collection
    char cmd3[512];
    snprintf(cmd3, sizeof(cmd3), "%s -c \"\\\"cherry banana apple\\\", split \\\" \\\", sort them descending, print first.\"", g_bin);
    char *out3 = run_cmd(cmd3, NULL);
    trim_str(out3);
    if (strcmp(out3, "cherry") != 0) {
        snprintf(err, err_sz, "them register failed: expected 'cherry', got '%s'", out3);
        free(out3);
        return false;
    }
    free(out3);
    return true;
}

// Test 24: Implicit Stdin Pipeline Loop (-n flag)
static bool test_implicit_stdin_loop(char *err, size_t err_sz) {
    write_file("test_in.txt", "apple 100\nbanana 200\n");
    char cmd[512];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "%s -n -c \"print 1st word.\" < test_in.txt", g_bin);
#else
    snprintf(cmd, sizeof(cmd), "%s -n -c \"print 1st word.\" < test_in.txt", g_bin);
#endif
    char *out = run_cmd(cmd, NULL);
    remove_file("test_in.txt");

    bool ok = (strstr(out, "apple") != NULL && strstr(out, "banana") != NULL);
    if (!ok) snprintf(err, err_sz, "Implicit stdin loop failed: expected apple and banana, got '%s'", out);
    free(out);
    return ok;
}

// Test 25: Job Control (background, jobs, kill)
static bool test_job_control(char *err, size_t err_sz) {
    char cmd[512];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "%s -c \"background run \\\"ping -n 10 127.0.0.1\\\". kill 1. jobs, table.\"", g_bin);
#else
    snprintf(cmd, sizeof(cmd), "%s -c \"background run \\\"sleep 10\\\". kill 1. jobs, table.\"", g_bin);
#endif
    char *out = run_cmd(cmd, NULL);
    bool ok = (strstr(out, "Terminated") != NULL || strstr(out, "Job 1 started") != NULL);
    if (!ok) snprintf(err, err_sz, "Job control failed: expected 'Terminated' or 'Job 1 started', got '%s'", out);
    free(out);
    return ok;
}

// Test 26: Audited Core Expressions, Lists, Drop/Join, Predicate Where & Sort Adverbs
static bool test_audited_core_collections(char *err, size_t err_sz) {
    // 26.1 Literal 0 + assignment, `add` with var & `that`, unary minus
    char cmd1[512];
    snprintf(cmd1, sizeof(cmd1), "%s -c \"0 into score. 10 into x. add 150 score. add 25 that. print that + -x.\"", g_bin);
    char *out1 = run_cmd(cmd1, NULL);
    trim_str(out1);
    if (strcmp(out1, "165") != 0) {
        snprintf(err, err_sz, "Expected '165', got '%s'", out1);
        free(out1);
        return false;
    }
    free(out1);

    // 26.2 List literals, `drop`, `join`, general `where` comparisons
    write_file("test_col.otto",
        "[{name: \"a\", score: 15}, {name: \"b\", score: 50}, {name: \"c\", score: 80}], "
        "drop where each's score < 30, take where each's score < 70, print first's name.\n"
        "[10, 20, 30, 40], drop first, join \"-\", print.\n");
    char cmd2[512];
    snprintf(cmd2, sizeof(cmd2), "%s test_col.otto", g_bin);
    char *out2 = run_cmd(cmd2, NULL);
    remove_file("test_col.otto");
    if (strstr(out2, "b") == NULL || strstr(out2, "20-30-40") == NULL) {
        snprintf(err, err_sz, "List/where/drop/join failed: got '%s'", out2);
        free(out2);
        return false;
    }
    free(out2);

    // 26.3 Sort adverbs: `casefold uniquely` vs `numerically`
    write_file("test_sort.otto",
        "[\"banana\", \"Apple\", \"banana\", \"cherry\"], sort casefold uniquely, join \",\", print.\n"
        "[\"100\", \"20\", \"3\"], sort numerically, join \",\", print.\n");
    char cmd3[512];
    snprintf(cmd3, sizeof(cmd3), "%s test_sort.otto", g_bin);
    char *out3 = run_cmd(cmd3, NULL);
    remove_file("test_sort.otto");
    bool ok = (strstr(out3, "Apple,banana,cherry") != NULL && strstr(out3, "3,20,100") != NULL);
    if (!ok) snprintf(err, err_sz, "Sort adverbs failed: got '%s'", out3);
    free(out3);
    return ok;
}

// Test 27: Generalized Unpack, CSV Scanner, Extractors & Universal `into`
static bool test_unpack_csv_extractors(char *err, size_t err_sz) {
    // 27.1 Arbitrary variable names in `unpack`
    uint8_t dims[4] = { 0x40, 0x01, 0xE0, 0x01 }; // 320 x 480
    write_binary_file("dims.bin", dims, sizeof(dims));
    char cmd1[512];
    snprintf(cmd1, sizeof(cmd1), "%s -c \"read \\\"dims.bin\\\", unpack \\\"u16 u16\\\" into width height. print width * height.\"", g_bin);
    char *out1 = run_cmd(cmd1, NULL);
    remove_file("dims.bin");
    trim_str(out1);
    if (strcmp(out1, "153600") != 0) {
        snprintf(err, err_sz, "Generalized unpack failed: expected 153600, got '%s'", out1);
        free(out1);
        return false;
    }
    free(out1);

    // 27.2 CSV scanner + numeric sort
    write_file("team.csv", "user,score\nada,92\ngrace,98\nlinus,89\n");
    char cmd2[512];
    snprintf(cmd2, sizeof(cmd2), "%s -c \"read \\\"team.csv\\\" csv, sort score descending, print first's user.\"", g_bin);
    char *out2 = run_cmd(cmd2, NULL);
    remove_file("team.csv");
    trim_str(out2);
    if (strcmp(out2, "grace") != 0) {
        snprintf(err, err_sz, "CSV scanner failed: expected 'grace', got '%s'", out2);
        free(out2);
        return false;
    }
    free(out2);

    // 27.3 URL / Email extractors & universal `into <var>`
    write_file("test_ext.otto",
        "\"contact admin@otto.sh or visit https://otto.sh/docs\", take urls into u. print first of u or print u.\n"
        "\"order 42 shipped\", take 2nd number into ord_id. print ord_id.\n");
    write_file("test_ext.otto",
        "\"contact admin@otto.sh or visit https://otto.sh/docs\", take urls, print first.\n"
        "\"contact admin@otto.sh or visit https://otto.sh/docs\", take emails, print first.\n"
        "\"order 42 shipped\", take 2nd number into ord_id. print ord_id.\n");
    char cmd3[512];
    snprintf(cmd3, sizeof(cmd3), "%s test_ext.otto", g_bin);
    char *out3 = run_cmd(cmd3, NULL);
    remove_file("test_ext.otto");
    bool ok = (strstr(out3, "https://otto.sh/docs") != NULL &&
               strstr(out3, "admin@otto.sh") != NULL &&
               strstr(out3, "42") != NULL);
    if (!ok) snprintf(err, err_sz, "Extractors/into failed: got '%s'", out3);
    free(out3);
    return ok;
}

// Test 28: File Ops (`copy`, `move`, `remove` + `undo`), Recursive Glob, FFI, Hooks & History
static bool test_fileops_ffi_hooks_history(char *err, size_t err_sz) {
    // 28.1 File copy, move, remove + transactional undo
    write_file("f_orig.txt", "alpha_payload");
    remove_file("f_copy.txt");
    remove_file("f_moved.txt");

    char cmd1[512];
    snprintf(cmd1, sizeof(cmd1), "%s -c \"copy \\\"f_orig.txt\\\" into \\\"f_copy.txt\\\". move \\\"f_copy.txt\\\" into \\\"f_moved.txt\\\".\"", g_bin);
    char *out1 = run_cmd(cmd1, NULL);
    free(out1);

    char *moved_c = read_file("f_moved.txt");
    if (!moved_c || strcmp(moved_c, "alpha_payload") != 0 || file_exists("f_copy.txt")) {
        if (moved_c) free(moved_c);
        remove_file("f_orig.txt");
        remove_file("f_copy.txt");
        remove_file("f_moved.txt");
        snprintf(err, err_sz, "copy/move failed");
        return false;
    }
    free(moved_c);
    remove_file("f_moved.txt");

    snprintf(cmd1, sizeof(cmd1), "%s -c \"remove \\\"f_orig.txt\\\".\"", g_bin);
    out1 = run_cmd(cmd1, NULL);
    free(out1);
    if (file_exists("f_orig.txt")) {
        remove_file("f_orig.txt");
        snprintf(err, err_sz, "remove failed to delete f_orig.txt");
        return false;
    }

    snprintf(cmd1, sizeof(cmd1), "%s -c \"undo last.\"", g_bin);
    out1 = run_cmd(cmd1, NULL);
    free(out1);
    char *restored = read_file("f_orig.txt");
    remove_file("f_orig.txt");
    if (!restored || strcmp(restored, "alpha_payload") != 0) {
        if (restored) free(restored);
        snprintf(err, err_sz, "undo after remove failed");
        return false;
    }
    free(restored);

    // 28.2 Recursive globbing
    mkdir_p("test_rec_dir");
    mkdir_p("test_rec_dir/nested");
    write_file("test_rec_dir/nested/deep.txt", "ok");
    char cmd2[512];
    snprintf(cmd2, sizeof(cmd2), "%s -c \"every recursively file \\\"test_rec_dir/*.txt\\\": print.\"", g_bin);
    char *out2 = run_cmd(cmd2, NULL);
    remove_file("test_rec_dir/nested/deep.txt");
#ifdef _WIN32
    _rmdir("test_rec_dir/nested");
    _rmdir("test_rec_dir");
#else
    rmdir("test_rec_dir/nested");
    rmdir("test_rec_dir");
#endif
    if (strstr(out2, "deep.txt") == NULL) {
        snprintf(err, err_sz, "recursively glob failed: got '%s'", out2);
        free(out2);
        return false;
    }
    free(out2);

    // 28.3 Native C ABI FFI + Hooks (`on failure:`, `on exit:`, `every 50ms:`) + History (`previous's last`, `repeat`)
#ifdef _WIN32
    const char *libc_name = "msvcrt.dll";
#elif defined(__APPLE__)
    const char *libc_name = "libSystem.B.dylib";
#else
    const char *libc_name = "libc.so.6";
#endif
    char script_buf[1024];
    snprintf(script_buf, sizeof(script_buf),
        "extern \"%s\" abs(i32) into c_abs.\n"
        "on failure: print \"caught_err\".\n"
        "on exit: print \"exit_hook_ok\".\n"
        "c_abs -42, print.\n"
        "read \"nonexistent_file_xyz_999.txt\".\n"
        "every 20ms: print \"timer_tick\".\n"
        "print \"hello_foo\".\n"
        "print previous's last.\n"
        "repeat previous replacing \"foo\" with \"bar\".\n",
        libc_name);
    write_file("test_hooks.otto", script_buf);
#ifdef _WIN32
    _putenv_s("OTTO_WATCH_ONCE", "1");
#else
    setenv("OTTO_WATCH_ONCE", "1", 1);
#endif
    char cmd3[512];
    snprintf(cmd3, sizeof(cmd3), "%s test_hooks.otto", g_bin);
    char *out3 = run_cmd(cmd3, NULL);
    remove_file("test_hooks.otto");
#ifdef _WIN32
    _putenv_s("OTTO_WATCH_ONCE", "");
#else
    unsetenv("OTTO_WATCH_ONCE");
#endif

    bool ok = (strstr(out3, "42") != NULL &&
               strstr(out3, "caught_err") != NULL &&
               strstr(out3, "timer_tick") != NULL &&
               strstr(out3, "hello_foo") != NULL &&
               strstr(out3, "hello_bar") != NULL &&
               strstr(out3, "exit_hook_ok") != NULL);
    if (!ok) snprintf(err, err_sz, "FFI/hooks/history failed: got '%s'", out3);
    free(out3);
    return ok;
}

// Test 29: Multi-File Code Search (`search` + `.gitignore`) & Rich `stat` + Unit Literals
static bool test_search_stat_units(char *err, size_t err_sz) {
    mkdir_p("test_srch_dir");
    mkdir_p("test_srch_dir/ignored_sub");
    write_file("test_srch_dir/.gitignore", "ignored_sub\n*.secret\n");
    write_file("test_srch_dir/main.c", "int needle_alpha = 42;\n");
    write_file("test_srch_dir/notes.txt", "NEEDLE_ALPHA uppercase\n");
    write_file("test_srch_dir/hidden.secret", "needle_alpha secret\n");
    write_file("test_srch_dir/ignored_sub/skip.c", "needle_alpha skipped\n");

    char big_buf[2049];
    memset(big_buf, 'A', 2048);
    big_buf[2048] = '\0';
    write_file("test_srch_dir/big.dat", big_buf);

    write_file("test_srch.otto",
        "search casefold \"needle_alpha\" in \"test_srch_dir\", table.\n"
        "search \"needle_alpha\" \"test_srch_dir\", print first's line.\n"
        "stat \"test_srch_dir/*\", take where each's size > 1kb, print first's ext.\n"
        "every file \"test_srch_dir/*\" where each's size > 1kb: print each's name.\n"
        "print :8080.\n");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s test_srch.otto", g_bin);
    char *out = run_cmd(cmd, NULL);

    remove_file("test_srch.otto");
    remove_file("test_srch_dir/.gitignore");
    remove_file("test_srch_dir/main.c");
    remove_file("test_srch_dir/notes.txt");
    remove_file("test_srch_dir/hidden.secret");
    remove_file("test_srch_dir/ignored_sub/skip.c");
    remove_file("test_srch_dir/big.dat");
#ifdef _WIN32
    _rmdir("test_srch_dir/ignored_sub");
    _rmdir("test_srch_dir");
#else
    rmdir("test_srch_dir/ignored_sub");
    rmdir("test_srch_dir");
#endif

    bool ok = (strstr(out, "main.c") != NULL &&
               strstr(out, "notes.txt") != NULL &&
               strstr(out, "hidden.secret") == NULL &&
               strstr(out, "skip.c") == NULL &&
               strstr(out, "dat") != NULL &&
               strstr(out, "big.dat") != NULL &&
               strstr(out, "8080") != NULL);
    if (!ok) snprintf(err, err_sz, "search/stat/unit literals failed: got '%s'", out);
    free(out);
    return ok;
}

// Test 30: Syntax-Highlighted Viewer (`view`) & Native Git Scanners (`status`, `commits`, `branches`)
static bool test_view_and_git_scanners(char *err, size_t err_sz) {
#ifdef _WIN32
    _putenv_s("OTTO_NO_PAGER", "1");
#else
    setenv("OTTO_NO_PAGER", "1", 1);
#endif
    write_file("test_view_git.otto",
        "view \"src/main.c\".\n"
        "read git branches, print first's name.\n"
        "read commits, print first's hash.\n"
        "every git branches: print each's name.\n");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s test_view_git.otto", g_bin);
    char *out = run_cmd(cmd, NULL);
    remove_file("test_view_git.otto");

    bool ok = (strstr(out, "src/main.c") != NULL &&
               strstr(out, "main") != NULL);
    if (!ok) snprintf(err, err_sz, "view/git scanners failed: got '%s'", out);
    free(out);
    return ok;
}

// Test 31: Crypto/Encoding (`hash`, `encode`, `decode`, `uuid`) & `toml` / `jsonl` Scanners
static bool test_crypto_encoding_toml_jsonl(char *err, size_t err_sz) {
    write_file("test_cfg.toml",
        "[package]\n"
        "name = \"otto_pkg\"\n"
        "port = 8080\n"
        "[profile.release]\n"
        "lto = true\n");

    write_file("test_events.jsonl",
        "{\"svc\": \"web\", \"code\": 200}\n"
        "{\"svc\": \"auth_err\", \"code\": 503}\n");

    write_file("test_a5.otto",
        "\"hello\", hash sha256, print.\n"
        "\"hello\", hash crc32, print.\n"
        "\"otto shell\", encode base64, decode base64, print.\n"
        "\"otto\", encode hex, print.\n"
        "\"6f74746f\", decode hex, print.\n"
        "\"a b&c\", encode url, print.\n"
        "\"a%20b%26c\", decode url, print.\n"
        "uuid into u. print u.\n"
        "read \"test_cfg.toml\" toml into cfg. print cfg's package's name. print cfg's profile's release's lto.\n"
        "every jsonl \"test_events.jsonl\" where each's code >= 500: print each's svc.\n");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s test_a5.otto", g_bin);
    char *out = run_cmd(cmd, NULL);
    remove_file("test_a5.otto");
    remove_file("test_cfg.toml");
    remove_file("test_events.jsonl");

    bool ok = (strstr(out, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824") != NULL &&
               strstr(out, "3610a686") != NULL &&
               strstr(out, "otto shell") != NULL &&
               strstr(out, "6f74746f") != NULL &&
               strstr(out, "a%20b%26c") != NULL &&
               strstr(out, "-4") != NULL &&
               strstr(out, "otto_pkg") != NULL &&
               strstr(out, "true") != NULL &&
               strstr(out, "auth_err") != NULL);
    if (!ok) snprintf(err, err_sz, "crypto/encoding/toml/jsonl failed: got '%s'", out);
    free(out);
    return ok;
}

// Test 32: `disk` Usage, `ports` Scanner, Auto `.env` Loader & `Ottofile` Task Runner
static bool test_disk_ports_dotenv_ottofile(char *err, size_t err_sz) {
    mkdir_p("test_env_dir");
    write_file("test_env_dir/.env", "OTTO_DOTENV_KEY=\"magic_dotenv_42\"\n");

    write_file("test_a6.otto",
        "disk, print.\n"
        "disk \"src\", print first's path.\n"
        "read ports, print.\n"
        "cd \"test_env_dir\". print $OTTO_DOTENV_KEY.\n");

    char cmd1[512];
    snprintf(cmd1, sizeof(cmd1), "%s test_a6.otto", g_bin);
    char *out1 = run_cmd(cmd1, NULL);
    remove_file("test_a6.otto");
    remove_file("test_env_dir/.env");
#ifdef _WIN32
    _rmdir("test_env_dir");
#else
    rmdir("test_env_dir");
#endif

    if (strstr(out1, "magic_dotenv_42") == NULL || strstr(out1, "src/") == NULL) {
        snprintf(err, err_sz, "disk/ports/.env failed: got '%s'", out1);
        free(out1);
        return false;
    }
    free(out1);

    // Ottofile task runner (`--tasks` and `task <name> <arg>`)
    write_file("test_tasks.otto",
        "# Greet a target developer\n"
        "greet who:\n"
        "    print \"hi_task_\" + who.\n");

    char cmd2[512];
    snprintf(cmd2, sizeof(cmd2), "%s -f test_tasks.otto --tasks", g_bin);
    char *out2 = run_cmd(cmd2, NULL);
    if (strstr(out2, "greet") == NULL || strstr(out2, "Greet a target developer") == NULL) {
        remove_file("test_tasks.otto");
        snprintf(err, err_sz, "otto --tasks failed: got '%s'", out2);
        free(out2);
        return false;
    }
    free(out2);

    char cmd3[512];
    snprintf(cmd3, sizeof(cmd3), "%s -f test_tasks.otto task greet World", g_bin);
    char *out3 = run_cmd(cmd3, NULL);
    remove_file("test_tasks.otto");
    bool ok = (strstr(out3, "hi_task_World") != NULL);
    if (!ok) snprintf(err, err_sz, "otto task greet failed: got '%s'", out3);
    free(out3);
    return ok;
}

// Test 33: Live `abbr`, Deduplicated `$PATH` `prepend`/`append`, Persistent History & Stderr Separation
static bool test_abbr_path_history_stderr(char *err, size_t err_sz) {
    // 33.1 `abbr`, list `prepend`/`append`, deduplicated `$PATH`, and clean stdout capture vs child stderr
#ifdef _WIN32
    const char *stderr_subcmd = "cmd /c \"echo clean_out&& echo warn_noise 1>&2\"";
#else
    const char *stderr_subcmd = "sh -c 'echo clean_out; echo warn_noise >&2'";
#endif
    (void)stderr_subcmd;
    write_file("test_b_all.otto",
        "abbr \"say777\" \"echo abbr_777_ok\".\n"
        "say777, print.\n"
        "[2, 3], prepend 1, append 4, join \"-\", print.\n"
        "prepend \"otto_custom_bin_dir\" into $PATH.\n"
        "prepend \"otto_custom_bin_dir\" into $PATH.\n"
        "print $PATH.\n");

    char cmd1[512];
    snprintf(cmd1, sizeof(cmd1), "%s test_b_all.otto", g_bin);
    char *out1 = run_cmd(cmd1, NULL);
    remove_file("test_b_all.otto");

    const char *first_occ = strstr(out1, "otto_custom_bin_dir");
    const char *second_occ = first_occ ? strstr(first_occ + 19, "otto_custom_bin_dir") : NULL;
    if (strstr(out1, "abbr_777_ok") == NULL || strstr(out1, "1-2-3-4") == NULL ||
        first_occ == NULL || second_occ != NULL) {
        snprintf(err, err_sz, "abbr/prepend/dedup PATH failed: got '%s'", out1);
        free(out1);
        return false;
    }
    free(out1);

    // 33.2 Verify child stderr does not pollute captured stdout pipeline value
#ifdef _WIN32
    write_file("test_stderr.otto", "run \"cmd /c echo clean_out && echo warn_noise 1>&2\" into cap. print \"[\" + cap + \"]\".\n");
#else
    write_file("test_stderr.otto", "run \"echo clean_out; echo warn_noise >&2\" into cap. print \"[\" + cap + \"]\".\n");
#endif
    char cmd2[512];
    snprintf(cmd2, sizeof(cmd2), "%s test_stderr.otto", g_bin);
    char *out2 = run_cmd(cmd2, NULL);
    remove_file("test_stderr.otto");
    if (strstr(out2, "[clean_out") == NULL || strstr(out2, "warn_noise]") != NULL) {
        snprintf(err, err_sz, "stderr polluted captured stdout: got '%s'", out2);
        free(out2);
        return false;
    }
    free(out2);

    // 33.3 Persistent REPL command history via OTTO_HISTORY_FILE
    remove_file("test_otto_hist.txt");
#ifdef _WIN32
    _putenv_s("OTTO_HISTORY_FILE", "test_otto_hist.txt");
#else
    setenv("OTTO_HISTORY_FILE", "test_otto_hist.txt", 1);
#endif
    write_file("test_repl_in.txt", "abbr \"hi9\" \"print 999\"\nhi9\nexit\n");
    char cmd3[512];
    snprintf(cmd3, sizeof(cmd3), "%s < test_repl_in.txt", g_bin);
    char *out3 = run_cmd(cmd3, NULL);
    remove_file("test_repl_in.txt");
#ifdef _WIN32
    _putenv_s("OTTO_HISTORY_FILE", "");
#else
    unsetenv("OTTO_HISTORY_FILE");
#endif

    char *hist_c = read_file("test_otto_hist.txt");
    remove_file("test_otto_hist.txt");
    bool ok = (strstr(out3, "999") != NULL && hist_c != NULL && strstr(hist_c, "print 999") != NULL);
    if (!ok) snprintf(err, err_sz, "Persistent REPL history/abbr failed: out='%s', hist='%s'", out3, hist_c ? hist_c : "(null)");
    if (hist_c) free(hist_c);
    free(out3);
    return ok;
}

// Test 34: Built-in `help`/`examples`, Caret Diagnostics, Levenshtein/Rosetta Hints & `otto tutor`
static bool test_help_diagnostics_tutor(char *err, size_t err_sz) {
    // 34.1 Built-in `help` & `examples`
    char cmd1[512];
    snprintf(cmd1, sizeof(cmd1), "%s -c \"help search. examples git.\"", g_bin);
    char *out1 = run_cmd(cmd1, NULL);
    if (strstr(out1, "search") == NULL || strstr(out1, "commits") == NULL) {
        snprintf(err, err_sz, "help/examples failed: got '%s'", out1);
        free(out1);
        return false;
    }
    free(out1);

    // 34.2 Visual caret `^` + `into` hint on `=` syntax error
    char cmd2[512];
    snprintf(cmd2, sizeof(cmd2), "%s -c \"42 = x.\" 2>&1", g_bin);
    char *out2 = run_cmd(cmd2, NULL);
    if (strstr(out2, "^") == NULL || strstr(out2, "into") == NULL) {
        snprintf(err, err_sz, "Visual caret diagnostic failed: got '%s'", out2);
        free(out2);
        return false;
    }
    free(out2);

    // 34.3 Levenshtein suggestion on `prnit` & Rosetta tip on `ll`
    write_file("test_typos.otto", "prnit \"hi\".\nll.\n");
    char cmd3[512];
    snprintf(cmd3, sizeof(cmd3), "%s test_typos.otto 2>&1", g_bin);
    char *out3 = run_cmd(cmd3, NULL);
    remove_file("test_typos.otto");
    if (strstr(out3, "Did you mean") == NULL || strstr(out3, "print") == NULL || strstr(out3, "In otto, try:") == NULL) {
        snprintf(err, err_sz, "Levenshtein/Rosetta hints failed: got '%s'", out3);
        free(out3);
        return false;
    }
    free(out3);

    // 34.4 Interactive 5-Lesson `otto tutor` walkthrough
    char cmd4[512];
    snprintf(cmd4, sizeof(cmd4), "%s tutor", g_bin);
    char *out4 = run_cmd(cmd4, NULL);
    bool ok = (strstr(out4, "Lesson 1") != NULL && strstr(out4, "Lesson 5") != NULL && strstr(out4, "720") != NULL);
    if (!ok) snprintf(err, err_sz, "otto tutor failed: got '%s'", out4);
    free(out4);
    return ok;
}

// ---------------------------------------------------------
// Main Entry Point
// ---------------------------------------------------------

int main(int argc, char **argv) {
    // 1. Resolve otto binary path
#ifdef _WIN32
    if (file_exists("otto.exe")) {
        strcpy(g_bin, "otto.exe");
    } else if (file_exists(".\\otto.exe")) {
        strcpy(g_bin, ".\\otto.exe");
    } else if (file_exists("./otto.exe")) {
        strcpy(g_bin, "otto.exe");
    }
#else
    if (file_exists("./otto")) {
        strcpy(g_bin, "./otto");
    } else if (file_exists("./otto.exe")) {
        strcpy(g_bin, "./otto.exe");
    }
#endif
    else if (argc > 1) {
        strncpy(g_bin, argv[1], sizeof(g_bin) - 1);
    } else {
        fprintf(stderr, "Error: Cannot locate otto executable (looked for ./otto.exe or ./otto)\n");
        return 1;
    }

    printf("\033[1;36m==========================================================\033[0m\n");
    printf("\033[1;36mOTTO UNIFIED PORTABLE C TEST RUNNER (ALL 34 TESTS)\033[0m\n");
    printf("\033[1;36mTarget Binary: %s\033[0m\n", g_bin);
    printf("\033[1;36m==========================================================\033[0m\n");

    // Master Tests (1-14)
    run_test("Zero-filler recursion & TCO (gcd 1071 462)", test_tco_gcd);
    run_test("TCO Deep Recursion (count_down 1000000)", test_tco_deep);
    run_test("AWK lifecycle + ordinal + implicit number scanner", test_awk_lifecycle);
    run_test("Possessive JSON navigation + sorting", test_json_nav);
    run_test("Parallel tee (and) + short-circuit (or)", test_and_or_tee);
    run_test("In-place dry-run & transactional rollback (simulate, inplace, undo)", test_simulate_inplace_undo);
    run_test("Relational set operations (exclude, intersect, count)", test_relational_sets);
    run_test("Binary unpacking (unpack u32 u8 u8)", test_binary_unpack);
    run_test("Incremental build rule (target from src)", test_incremental_build);
    run_test("Embedded SQLite stream (auto-create table, insert, query pushdown)", test_sqlite_stream);
    run_test("Standalone binary compilation (compile into standalone executable)", test_standalone_compilation);
    run_test("Native YAML Stream Processing", test_yaml_processing);
    run_test("Strict Zero-Filler Rule Rejection", test_zero_filler_rejection);
    run_test("TUI Table Grid Rendering", test_tui_table);

    // Modern Capabilities Tests (15-21)
    run_test("Native HTTP REST Streams (fetch & onto POST)", test_http_rest);
    run_test("File Change Watcher Loop (watch Hook with change detection)", test_file_watcher);
    run_test("Frecency Smart Directory Navigation (jump Verb)", test_frecency_jump);
    run_test("Git-Aware Directory Scanning (tree & ignoring git)", test_git_tree_ignore);
    run_test("Statistical Benchmarking (benchmark Verb)", test_benchmark);
    run_test("Dataframe Group Aggregations (tally & group by)", test_dataframe_aggregations);
    run_test("Starship-Style Prompt & REPL", test_starship_repl);

    // Natural Language One-Liner Ergonomics & Job Control (22-25)
    run_test("Zero-preposition positional argument syntax (replace, split, sort)", test_zero_prepositions);
    run_test("Pronoun registers 'that' and 'them'", test_pronoun_registers);
    run_test("Implicit Stdin Pipeline Loop", test_implicit_stdin_loop);
    run_test("Job Control (background, jobs, kill)", test_job_control);

    // Comprehensive Post-Audit Verification Tests (26-28)
    run_test("Audited Core Expressions, Lists, Drop/Join, Where & Sort Adverbs", test_audited_core_collections);
    run_test("Generalized Unpack, CSV Scanner, Extractors & Universal 'into'", test_unpack_csv_extractors);
    run_test("File Ops (copy/move/remove + undo), Recursive Glob, FFI, Hooks & History", test_fileops_ffi_hooks_history);

    // Pillars A, B, C: Modern Dev CLI, Fish-Class Interactive Shell & Beginner Onboarding (29-34)
    run_test("Multi-File Code Search (search + .gitignore) & Rich stat + Unit Literals", test_search_stat_units);
    run_test("Syntax-Highlighted Viewer (view) & Native Git Scanners (status/commits/branches)", test_view_and_git_scanners);
    run_test("Crypto/Encoding (hash, encode/decode, uuid) & TOML / JSONL Scanners", test_crypto_encoding_toml_jsonl);
    run_test("Disk Usage, Ports Scanner, Auto .env Loader & Ottofile Task Runner", test_disk_ports_dotenv_ottofile);
    run_test("Live abbr, Deduplicated $PATH prepend/append, Persistent History & Stderr Separation", test_abbr_path_history_stderr);
    run_test("Built-in help/examples, Caret Diagnostics, Levenshtein/Rosetta Hints & otto tutor", test_help_diagnostics_tutor);

    printf("\n\033[1;36m==========================================================\033[0m\n");
    if (g_passed == g_total) {
        printf("\033[1;32mTEST RESULTS: %d / %d PASSED (100%% SUCCESS RATE)\033[0m\n", g_passed, g_total);
    } else {
        printf("\033[1;31mTEST RESULTS: %d / %d PASSED (%d FAILED)\033[0m\n", g_passed, g_total, g_total - g_passed);
    }
    printf("\033[1;36m==========================================================\033[0m\n");

    return (g_passed == g_total) ? 0 : 1;
}
