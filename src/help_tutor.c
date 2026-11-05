#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif

#include "help_tutor.h"
#include "lexer.h"
#include "parser.h"
#include "bc.h"
#include "vm.h"
#include "tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

// =========================================================
// C1: Built-In Cheat Sheets (`help` & `examples`)
// =========================================================

typedef struct HelpEntry {
    const char *topic;
    const char *category;
    const char *summary;
    const char *syntax;
    const char *examples;
} HelpEntry;

static const HelpEntry g_help_db[] = {
    { "search", "Dev Tools (replaces ripgrep)",
      "Fast recursive gitignore-aware multi-file text search returning {file, line, col, text} maps. Supports `-i` / `casefold`.",
      "search [-i | casefold] \"<pattern>\" [in \"<dir>\"]",
      "  search \"TODO\" in \"src\", table.\n"
      "  search -i \"error\" \"logs\", print count.\n"
      "  search \"vm_run\" \"src\", print first's file." },
    { "stat", "Dev Tools (replaces eza / fd)",
      "Inspect rich file metadata ({path, name, ext, size, age, modified, dir}). Supports `-r` / `recursively` and unit literals (10kb, 7d).",
      "stat [-r | recursively] [\"<path-or-glob>\"]",
      "  stat \"src\", sort size descending, take 5 lines, table.\n"
      "  stat -r \"src\", take where each's size > 10kb, table.\n"
      "  every file \"src/*\" where each's ext == \"h\": print each's name." },
    { "view", "Dev Tools (replaces bat / less)",
      "Syntax-highlighted file viewer with line numbers and interactive terminal paging.",
      "view \"<file>\"   or   <pipeline>, view.",
      "  view \"src/main.c\".\n"
      "  read \"README.md\", take 25 lines, view." },
    { "git", "Native Git Scanners (replaces delta / lazygit)",
      "Structured git scanners for status, commits, branches, and colorized diffs.",
      "read [git] status | commits | branches | diff",
      "  read status, table.\n"
      "  read commits, take 5 lines, table.\n"
      "  every branch where current == false: print name.\n"
      "  read diff, print." },
    { "status", "Native Git Scanners",
      "Scan working-tree git status into {state, file} maps.",
      "read status, table.",
      "  read status, table.\n"
      "  every status where state == \"M\": print file." },
    { "commits", "Native Git Scanners",
      "Scan recent git commit history into {hash, author, date, message} maps.",
      "read commits, table.",
      "  read commits, take 10 lines, table.\n"
      "  read commits, print first's hash." },
    { "branches", "Native Git Scanners",
      "Scan local git branches into {name, current} maps.",
      "read branches, table.",
      "  read branches, table.\n"
      "  every branch where current == true: print name." },
    { "diff", "Native Git Scanners",
      "Render delta-style ANSI syntax-colored git diff.",
      "read diff [\"<path>\"], print.",
      "  read diff, print.\n"
      "  read \"src/vm.c\" diff, print." },
    { "hash", "Crypto & Encoding (replaces openssl / sha256sum)",
      "Compute cryptographic SHA-256 (default) or IEEE CRC32 hex digests in pure C11.",
      "<expr>, hash [sha256 | crc32]",
      "  \"hello\", hash, print.\n"
      "  \"hello\", hash crc32, print.\n"
      "  read \"otto.exe\", hash sha256, print." },
    { "encode", "Crypto & Encoding (replaces base64 / xxd)",
      "Encode strings or bytes into base64 (default), hex, or url percent-encoding.",
      "<expr>, encode [base64 | hex | url]",
      "  \"hello world\", encode base64, print.\n"
      "  \"otto\", encode hex, print.\n"
      "  \"a b&c\", encode url, print." },
    { "decode", "Crypto & Encoding",
      "Decode base64 (default), hex, or url percent-encoded strings.",
      "<expr>, decode [base64 | hex | url]",
      "  \"aGVsbG8=\", decode base64, print.\n"
      "  \"6f74746f\", decode hex, print.\n"
      "  \"hello%20world\", decode url, print." },
    { "uuid", "Crypto & Encoding (replaces uuidgen)",
      "Generate a random RFC 4122 version 4 UUID.",
      "uuid, print.",
      "  uuid, print.\n"
      "  uuid into req_id. print req_id." },
    { "toml", "Structured Data (replaces tomlq)",
      "Parse TOML configuration files ([section] tables, arrays, numbers, booleans, strings).",
      "read \"<file.toml>\" toml",
      "  read \"Cargo.toml\" toml, print it's package's name.\n"
      "  read \"config.toml\" toml, print." },
    { "jsonl", "Structured Data (replaces jq -c)",
      "Parse or stream newline-delimited JSON (NDJSON / .jsonl) records.",
      "read \"<file.jsonl>\" jsonl   or   every jsonl \"<file.jsonl>\": ...",
      "  read \"events.jsonl\" jsonl, table.\n"
      "  every jsonl \"events.jsonl\" where level == \"error\": print msg." },
    { "disk", "System & Ops (replaces dust / duf)",
      "Show mounted drive space (no args) or recursive directory size breakdown (with path).",
      "disk   or   disk \"<dir>\"",
      "  disk, table.\n"
      "  disk \"src\", take 10 lines, table." },
    { "ports", "System & Ops (replaces lsof / netstat)",
      "Scan active local TCP/UDP ports into {proto, local, port, state, pid} maps.",
      "read ports, table.   or   every port where port == 8080: kill.",
      "  read ports, table.\n"
      "  every port where port == 8080: kill." },
    { "abbr", "Interactive Shell (Fish-style abbreviations)",
      "Define or list live abbreviations that expand automatically on Space or Enter in the REPL.",
      "abbr \"<short>\" \"<expansion>\"   or   abbr, table.",
      "  abbr \"gs\" \"read status, table\".\n"
      "  abbr \"gco\" \"git checkout\".\n"
      "  abbr, table." },
    { "prepend", "Environment & Lists (Fish fish_add_path)",
      "Deduplicated $PATH prepending or list front-insertion.",
      "prepend \"<dir>\" into $PATH",
      "  prepend \"./bin\" into $PATH.\n"
      "  [2, 3], prepend 1, print." },
    { "append", "Environment & Lists",
      "Deduplicated $PATH appending or list back-insertion.",
      "append \"<dir>\" into $PATH",
      "  append \"~/.local/bin\" into $PATH.\n"
      "  [1, 2], append 3, print." },
    { "read", "Core Verbs",
      "Read files, directory trees, processes, ports, git state, SQLite tables, JSON, YAML, TOML, CSV, or JSONL.",
      "read \"<source>\" [json | yaml | toml | csv | jsonl | tree]",
      "  read \"users.json\" json, table.\n"
      "  read \"ops.db:metrics\", table.\n"
      "  read processes, take 10 lines, table." },
    { "write", "Core Verbs",
      "Write or append (`onto`) text, maps, or lists to files, variables (`into`), SQLite tables, or HTTP POST URLs.",
      "write <expr> into <var-or-file>   |   write <expr> onto \"<file-or-url-or-db>\"",
      "  write \"hello\" into \"out.txt\".\n"
      "  write {host: \"web1\", cpu: 82} onto \"ops.db:metrics\".\n"
      "  write {ok: true} onto \"https://httpbin.org/post\", print." },
    { "take", "Core Verbs",
      "Filter items (`where`, `matching`), slice top N lines/bytes, or extract `urls`, `emails`, `numbers`, `words`.",
      "take [where <cond> | matching \"<pat>\" | <N> lines | first | urls | emails | numbers]",
      "  read \"app.log\", take matching \"ERROR\", print.\n"
      "  [10, 25, 40], take where each > 20, print.\n"
      "  \"reach dev@otto.sh\", take emails, print first." },
    { "drop", "Core Verbs",
      "Remove items matching a condition (`where`, `matching`) or drop leading/trailing lines/elements.",
      "drop [where <cond> | matching \"<pat>\" | <N> lines | first | last]",
      "  read \"config.ini\", drop matching \"#\", print.\n"
      "  [1, 2, 3, 4], drop first, print." },
    { "sort", "Core Verbs",
      "Sort lists, tables, or lines with optional `by <key>`, `descending`, `numerically`, `casefold`, `uniquely`.",
      "sort [[by] <key>] [descending] [numerically] [casefold] [uniquely]",
      "  read \"users.json\" json, sort age descending, table.\n"
      "  [\"b\", \"A\", \"b\"], sort casefold uniquely, print." },
    { "replace", "Core Verbs",
      "Replace substrings in pipeline values or `inplace` across files (with `simulate` dry-run & `undo` rollback).",
      "[simulate] [inplace] replace \"<old>\" [with] \"<new>\" [casefold]",
      "  \"hello foo\", replace \"foo\" \"bar\", print.\n"
      "  every file \"*.conf\": simulate inplace replace \"8080\" with \"9090\".\n"
      "  undo last." },
    { "every", "Stream Hooks",
      "Stream over lines, files, rows, processes, ports, git branches/commits, JSONL, or timer intervals (`every 2s:`).",
      "every [recursively] <scanner> [\"<source>\"] [where <cond>]: <pipeline>.",
      "  every line \"access.log\" matching \"500\": print first word.\n"
      "  every recursively file \"src/*.c\": print.\n"
      "  every 5s: read ports, count, print." },
    { "jobs", "Job Control",
      "Manage background OS processes (`&`, `background`, `jobs`, `resume`, `kill`).",
      "background run \"<cmd>\".   |   jobs, table.   |   resume 1.   |   kill 1.",
      "  background run \"python -m http.server 8000\".\n"
      "  jobs, table.\n"
      "  kill 1." },
    { "tasks", "Task Runner (replaces just / make)",
      "Define functions with `#` comments in an `Ottofile` and run `otto --tasks` or `otto task <name> [args]`.",
      "otto --tasks   |   otto task <name> [args...]",
      "  # Build release binary\n"
      "  build target: run \"zig cc src/*.c -o otto\".\n"
      "  # Then run from CLI: otto --tasks  or  otto task build release" },
    { "tutor", "Onboarding",
      "Launch the interactive 5-lesson hands-on Otto tutorial right in your terminal.",
      "tutor.   or   otto tutor",
      "  tutor." },
    { NULL, NULL, NULL, NULL, NULL }
};

Value help_show(const char *topic, bool examples_only, Arena *arena) {
    if (!topic || !topic[0]) {
        if (examples_only) {
            const char *ex_all =
                "\033[1;36mOtto One-Liner Cookbook (`examples`)\033[0m\n"
                "  \033[1;33m# Search code (ripgrep replacement)\033[0m\n"
                "  search \"TODO\" in \"src\", table.\n\n"
                "  \033[1;33m# Rich file listing by size (eza + fd replacement)\033[0m\n"
                "  stat recursively \"src/*.c\", take where each's size > 10kb, sort size descending, table.\n\n"
                "  \033[1;33m# Git workflow (delta / lazygit replacement)\033[0m\n"
                "  read status, table.\n"
                "  read commits, take 5 lines, table.\n\n"
                "  \033[1;33m# JSON / TOML / CSV / SQLite pipelines (jq / sqlite3 replacement)\033[0m\n"
                "  read \"Cargo.toml\" toml, print it's package's name.\n"
                "  write {service: \"api\", ms: 18} onto \"metrics.db:latency\".\n"
                "  read \"metrics.db:latency\", table.\n\n"
                "  \033[1;33m# Safe in-place refactoring with dry-run & rollback\033[0m\n"
                "  every file \"*.txt\": simulate inplace replace \"old\" \"new\".\n"
                "  undo last.\n";
            fputs(ex_all, stdout);
            fflush(stdout);
            char *dup = arena_strdup(arena, ex_all);
            return val_slice(dup, strlen(dup));
        }

        const char *overview =
            "\033[1;36motto\033[0m — Zero-Filler English Shell & Modern CLI Suite\n"
            "\033[2m──────────────────────────────────────────────────────────────────────\033[0m\n"
            "  \033[1;32mCore Verbs\033[0m      read, write, print, take, drop, split, join, sort, replace,\n"
            "                  add, count, tally, group, table, chart, pick, unpack, undo\n"
            "  \033[1;32mDev Utilities\033[0m   search (rg), stat (eza/fd), view (bat), disk (dust/duf),\n"
            "                  hash (sha256/crc32), encode/decode (base64/hex/url), uuid\n"
            "  \033[1;32mData & Git\033[0m      json, yaml, toml, csv, jsonl, status, commits, branches, diff\n"
            "  \033[1;32mSystem & Shell\033[0m  cd, back, dirs, jump, ports, processes, jobs, resume, kill,\n"
            "                  abbr, prepend/append ($PATH), benchmark, compile, repeat\n"
            "  \033[1;32mStream Hooks\033[0m    before:, every <scanner>:, after:, watch:, on failure:\n"
            "  \033[1;32mPronouns\033[0m        it (active pipe), each (loop item), that (prev result), them\n"
            "\033[2m──────────────────────────────────────────────────────────────────────\033[0m\n"
            "  Try: \033[1;36mhelp \"search\"\033[0m  |  \033[1;36mexamples \"git\"\033[0m  |  \033[1;36mexamples\033[0m  |  \033[1;36mtutor\033[0m\n";
        fputs(overview, stdout);
        fflush(stdout);
        char *dup = arena_strdup(arena, overview);
        return val_slice(dup, strlen(dup));
    }

    for (size_t i = 0; g_help_db[i].topic; i++) {
        if (strcasecmp(g_help_db[i].topic, topic) == 0) {
            char buf[2048];
            if (examples_only) {
                snprintf(buf, sizeof(buf),
                         "\033[1;36m%s\033[0m — Examples:\n%s\n",
                         g_help_db[i].topic, g_help_db[i].examples);
            } else {
                snprintf(buf, sizeof(buf),
                         "\033[1;36m%s\033[0m  \033[2m[%s]\033[0m\n"
                         "  %s\n\n"
                         "  \033[1;33mSyntax:\033[0m   %s\n"
                         "  \033[1;33mExamples:\033[0m\n%s\n",
                         g_help_db[i].topic, g_help_db[i].category,
                         g_help_db[i].summary, g_help_db[i].syntax, g_help_db[i].examples);
            }
            fputs(buf, stdout);
            fflush(stdout);
            char *dup = arena_strdup(arena, buf);
            return val_slice(dup, strlen(dup));
        }
    }

    const char *sug = help_suggest_keyword(topic, strlen(topic));
    char msg[256];
    if (sug) {
        snprintf(msg, sizeof(msg), "No help entry for '%s'. Did you mean '%s'? (Try: help \"%s\")\n", topic, sug, sug);
    } else {
        snprintf(msg, sizeof(msg), "No help entry for '%s'. Type 'help' to see all topics.\n", topic);
    }
    fputs(msg, stdout);
    fflush(stdout);
    char *dup = arena_strdup(arena, msg);
    return val_slice(dup, strlen(dup));
}

// =========================================================
// C2: Damerau-Levenshtein Suggestions & Rosetta Hints
// =========================================================

static int damerau_levenshtein(const char *a, size_t alen, const char *b, size_t blen) {
    if (alen > 30 || blen > 30) return 99;
    int dp[32][32];
    for (size_t i = 0; i <= alen; i++) dp[i][0] = (int)i;
    for (size_t j = 0; j <= blen; j++) dp[0][j] = (int)j;

    for (size_t i = 1; i <= alen; i++) {
        for (size_t j = 1; j <= blen; j++) {
            int ca = tolower((unsigned char)a[i - 1]);
            int cb = tolower((unsigned char)b[j - 1]);
            int cost = (ca == cb) ? 0 : 1;
            int del = dp[i - 1][j] + 1;
            int ins = dp[i][j - 1] + 1;
            int sub = dp[i - 1][j - 1] + cost;
            int best = del < ins ? del : ins;
            if (sub < best) best = sub;

            if (i > 1 && j > 1 &&
                tolower((unsigned char)a[i - 1]) == tolower((unsigned char)b[j - 2]) &&
                tolower((unsigned char)a[i - 2]) == tolower((unsigned char)b[j - 1])) {
                int trans = dp[i - 2][j - 2] + 1;
                if (trans < best) best = trans;
            }
            dp[i][j] = best;
        }
    }
    return dp[alen][blen];
}

const char *help_suggest_keyword(const char *word, size_t len) {
    if (!word || len < 2 || len > 24) return NULL;
    const char * const *kws = lexer_all_keywords();
    const char *best_kw = NULL;
    int best_dist = 99;
    int max_allowed = (len <= 3) ? 1 : 2;

    for (size_t i = 0; kws[i]; i++) {
        size_t klen = strlen(kws[i]);
        if (klen == len && strncasecmp(word, kws[i], len) == 0) return NULL; // already exact keyword
        int d = damerau_levenshtein(word, len, kws[i], klen);
        if (d <= max_allowed && d < best_dist) {
            best_dist = d;
            best_kw = kws[i];
        }
    }
    return best_kw;
}

const char *help_rosetta_tip(const char *raw_cmd) {
    if (!raw_cmd) return NULL;
    while (*raw_cmd && isspace((unsigned char)*raw_cmd)) raw_cmd++;
    const char *end = raw_cmd;
    while (*end && !isspace((unsigned char)*end) && *end != '.') end++;
    size_t len = (size_t)(end - raw_cmd);
    if (len == 0) return NULL;

    typedef struct RosettaPair {
        const char *unix_cmd;
        const char *tip;
    } RosettaPair;

    static const RosettaPair pairs[] = {
        { "ls",     "In otto, try: stat \".\", table.   or   read \".\" tree, print." },
        { "ll",     "In otto, try: stat \".\", sort size descending, table." },
        { "cat",    "In otto, try: view \"file.txt\".   or   read \"file.txt\", print." },
        { "less",   "In otto, try: view \"file.txt\"." },
        { "bat",    "In otto, try: view \"file.txt\"." },
        { "grep",   "In otto, try: search \"pattern\" in \".\", table." },
        { "rg",     "In otto, try: search \"pattern\" in \"src\", table." },
        { "find",   "In otto, try: every recursively file \"*.c\": print." },
        { "fd",     "In otto, try: stat recursively \"*.c\", table." },
        { "ps",     "In otto, try: read processes, table." },
        { "lsof",   "In otto, try: read ports, table." },
        { "du",     "In otto, try: disk \".\", table." },
        { "df",     "In otto, try: disk, table." },
        { "curl",   "In otto, try: fetch \"https://example.com/api\" json, table." },
        { "jq",     "In otto, try: read \"data.json\" json, print each's name." },
        { "head",   "In otto, try: read \"file.txt\", take 10 lines, print." },
        { "tail",   "In otto, try: read \"file.txt\", print last line." },
        { "wc",     "In otto, try: read \"file.txt\", print count lines." },
        { "sed",    "In otto, try: replace \"old\" \"new\" inplace." },
        { "alias",  "In otto, try: abbr \"short\" \"expansion\"." },
        { "export", "In otto, try: \"value\" into $VAR_NAME." },
        { "man",    "In otto, try: help \"<topic>\"  or  examples \"<topic>\"." },
        { "tldr",   "In otto, try: examples \"<topic>\"." },
        { NULL, NULL }
    };

    for (size_t i = 0; pairs[i].unix_cmd; i++) {
        if (strlen(pairs[i].unix_cmd) == len && strncasecmp(raw_cmd, pairs[i].unix_cmd, len) == 0) {
            return pairs[i].tip;
        }
    }
    return NULL;
}

// =========================================================
// C3: Visual Caret `^` Diagnostics & Interactive Tutor
// =========================================================

void help_print_diagnostic(const char *source, size_t len, int err_line, int err_col, const char *err_msg) {
    if (err_line < 1) err_line = 1;
    if (err_col < 1) err_col = 1;

    fprintf(stderr, "\033[1;31mSyntax error\033[0m (line %d, col %d): %s\n",
            err_line, err_col, err_msg ? err_msg : "invalid syntax");

    if (!source || len == 0) return;

    // Locate err_line in source
    int cur_line = 1;
    size_t line_start = 0;
    for (size_t i = 0; i < len && cur_line < err_line; i++) {
        if (source[i] == '\n') {
            cur_line++;
            line_start = i + 1;
        }
    }
    size_t line_end = line_start;
    while (line_end < len && source[line_end] != '\n' && source[line_end] != '\r') {
        line_end++;
    }
    size_t line_len = line_end - line_start;

    fprintf(stderr, "\033[2m%4d │\033[0m %.*s\n", err_line, (int)line_len, source + line_start);

    // Measure offending word length at err_col
    size_t off = (size_t)(err_col - 1);
    size_t wlen = 1;
    const char *word_ptr = NULL;
    if (off < line_len) {
        word_ptr = source + line_start + off;
        while (off + wlen < line_len && !isspace((unsigned char)source[line_start + off + wlen]) &&
               source[line_start + off + wlen] != ',' && source[line_start + off + wlen] != '.') {
            wlen++;
        }
    }

    fprintf(stderr, "\033[2m     │\033[0m ");
    for (int s = 1; s < err_col; s++) fputc(' ', stderr);
    fprintf(stderr, "\033[1;31m^");
    for (size_t t = 1; t < wlen && t < 24; t++) fputc('~', stderr);
    fprintf(stderr, "\033[0m\n");

    // Contextual fix hints
    if (word_ptr && wlen > 0) {
        if (token_is_filler(word_ptr, wlen)) {
            fprintf(stderr, "  \033[1;36mHint:\033[0m Otto uses strict zero-filler English — omit '%.*s' and pass arguments directly.\n",
                    (int)wlen, word_ptr);
            return;
        }
        if (word_ptr[0] == '=' && (wlen == 1 || word_ptr[1] != '=')) {
            fprintf(stderr, "  \033[1;36mHint:\033[0m Use 'into' for assignment (e.g. `42 into x.`) or '==' for comparison.\n");
            return;
        }
        const char *sug = help_suggest_keyword(word_ptr, wlen);
        if (sug) {
            fprintf(stderr, "  \033[1;36mHint:\033[0m Did you mean '\033[1;32m%s\033[0m'?\n", sug);
            return;
        }
        const char *rosetta = help_rosetta_tip(word_ptr);
        if (rosetta) {
            fprintf(stderr, "  \033[1;36mTip:\033[0m %s\n", rosetta);
        }
    }
}

static void run_tutor_snippet(VM *vm, const char *code) {
    Arena a;
    arena_init(&a, 32 * 1024);
    Parser p;
    parser_init(&p, code, strlen(code), false, &a);
    ASTNode *prog = parser_parse_program(&p);
    if (!p.has_error && prog) {
        BytecodeEmitter be;
        bc_init(&be, &vm->perm_arena);
        if (bc_compile_program(&be, prog)) {
            Instruction *saved_c = vm->code;
            size_t saved_l = vm->code_len;
            Value *saved_k = vm->constants;
            size_t saved_kc = vm->const_count;

            vm->code = be.code;
            vm->code_len = be.count;
            vm->constants = be.constants;
            vm->const_count = be.const_count;
            vm_run(vm);

            vm->code = saved_c;
            vm->code_len = saved_l;
            vm->constants = saved_k;
            vm->const_count = saved_kc;
        }
    } else if (p.has_error) {
        help_print_diagnostic(code, strlen(code), p.error_line, p.error_col, p.error_msg);
    }
    arena_free(&a);
}

void tutor_run(VM *vm, bool interactive) {
    typedef struct Lesson {
        const char *title;
        const char *explain;
        const char *sample;
    } Lesson;

    static const Lesson lessons[5] = {
        { "Lesson 1/5: Sentences, Pipes (`,`) & Pronouns",
          "Every statement in Otto is a clean English sentence ending in `.` (optional in REPL).\n"
          "  Commas `,` pipe data left-to-right, and `that` recalls the previous sentence's result.",
          "\"otto makes shell scripting joyful\", split \" \", print count them." },
        { "Lesson 2/5: Structured Data & `table`",
          "Otto streams native Maps and Lists instead of fragile text columns.\n"
          "  Filter with `take where`, sort with `sort <key> descending`, and render with `table`.",
          "[{name: \"Ada\", score: 98}, {name: \"Linus\", score: 91}], sort score descending, table." },
        { "Lesson 3/5: Built-in Developer Utilities",
          "No need to install rg, fd, eza, bat, or jq — `search`, `stat`, `view`, `hash`, and `uuid` are built in!",
          "\"hello otto\", hash sha256, print." },
        { "Lesson 4/5: Zero-Preposition & Prepositional Freedom",
          "Write `replace \"old\" with \"new\"` OR concise `replace \"old\" \"new\"` — both compile to identical bytecode.",
          "\"version 1.0\", replace \"1.0\" \"2.0\", print." },
        { "Lesson 5/5: Zero-Filler Functions & Tail-Call Speed",
          "Define functions with `name arg1 arg2: body.` — recursive tail calls run in O(1) stack space.",
          "fact n res: return res if n <= 1 else fact (n - 1) (res * n). print fact 6 1." }
    };

    printf("\n\033[1;36m============================================================\033[0m\n");
    printf("\033[1;36m  Welcome to OTTO TUTOR — 5-Minute Interactive Walkthrough\033[0m\n");
    printf("\033[1;36m============================================================\033[0m\n");

    bool use_prompt = interactive && tui_is_interactive();

    for (int i = 0; i < 5; i++) {
        printf("\n\033[1;33m%s\033[0m\n  %s\n", lessons[i].title, lessons[i].explain);
        printf("  \033[1;32mTry:\033[0m  \033[36m%s\033[0m\n", lessons[i].sample);

        if (!use_prompt) {
            printf("  \033[2mOutput:\033[0m ");
            fflush(stdout);
            run_tutor_snippet(vm, lessons[i].sample);
            continue;
        }

        printf("  \033[2m(Press Enter to run the example, type your own Otto command, or 'q' to quit)\033[0m\n");
        printf("  \033[1;32mtutor>\033[0m ");
        fflush(stdout);

        char line[1024];
        if (!fgets(line, sizeof(line), stdin)) break;
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == '\n')) line[--l] = '\0';
        if (strcmp(line, "q") == 0 || strcmp(line, "quit") == 0) {
            printf("Tutor exited. Type 'tutor' anytime to resume!\n");
            return;
        }
        const char *to_run = (l == 0) ? lessons[i].sample : line;
        char buf[1040];
        snprintf(buf, sizeof(buf), "%s", to_run);
        size_t bl = strlen(buf);
        if (bl > 0 && buf[bl - 1] != '.') {
            buf[bl++] = '.';
            buf[bl] = '\0';
        }
        run_tutor_snippet(vm, buf);
    }

    printf("\n\033[1;32m✔ Tutorial complete!\033[0m Type `\033[1;36mhelp\033[0m` or `\033[1;36mexamples\033[0m` anytime.\n\n");
    fflush(stdout);
}

// =========================================================
// A6: Native `Ottofile` Task Runner (`--tasks` & `task`)
// =========================================================

const char *ottofile_find_default(void) {
    static const char * const names[] = { "Ottofile", "ottofile", "tasks.otto", NULL };
    for (size_t i = 0; names[i]; i++) {
        FILE *fp = fopen(names[i], "rb");
        if (fp) {
            fclose(fp);
            return names[i];
        }
    }
    return NULL;
}

static bool parse_task_header(const char *line, size_t len, char *out_name, size_t name_sz, char *out_params, size_t params_sz) {
    // Must start at column 0 with an identifier, optional ident params, followed by ':'
    if (len == 0 || !isalpha((unsigned char)line[0])) return false;
    const char *colon = (const char *)memchr(line, ':', len);
    if (!colon) return false;
    // Reject URLs or build rules `"target" from "src":`
    size_t hlen = (size_t)(colon - line);
    if (hlen == 0 || hlen > 120) return false;

    Lexer lex;
    lexer_init(&lex, line, hlen, false);
    Token first = lexer_next(&lex);
    if (first.type != TOK_IDENT) return false;
    if (token_is_hook(first.type) || token_is_verb(first.type)) return false;

    size_t ncpy = first.lexeme.len < name_sz - 1 ? first.lexeme.len : name_sz - 1;
    memcpy(out_name, first.lexeme.ptr, ncpy);
    out_name[ncpy] = '\0';

    out_params[0] = '\0';
    size_t ppos = 0;
    while (1) {
        Token pt = lexer_next(&lex);
        if (pt.type == TOK_EOF) break;
        if (pt.type != TOK_IDENT) return false;
        if (ppos > 0 && ppos + 1 < params_sz) out_params[ppos++] = ' ';
        if (ppos + pt.lexeme.len < params_sz) {
            memcpy(out_params + ppos, pt.lexeme.ptr, pt.lexeme.len);
            ppos += pt.lexeme.len;
            out_params[ppos] = '\0';
        }
    }
    return true;
}

bool ottofile_list_tasks(const char *filepath) {
    const char *path = (filepath && filepath[0]) ? filepath : ottofile_find_default();
    if (!path) {
        fprintf(stderr, "No Ottofile or tasks.otto found in current directory.\n");
        return false;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "Cannot open task file: %s\n", path);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) sz = 0;
    fseek(fp, 0, SEEK_SET);
    char *src = (char *)malloc((size_t)sz + 1);
    size_t nr = fread(src, 1, (size_t)sz, fp);
    src[nr] = '\0';
    fclose(fp);

    printf("\033[1;36mAvailable tasks in %s:\033[0m\n", path);
    char last_comment[256] = "";
    size_t task_count = 0;

    const char *p = src;
    const char *end = src + nr;
    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n') nl++;
        size_t llen = (size_t)(nl - p);
        if (llen > 0 && p[llen - 1] == '\r') llen--;

        Slice trimmed = slice_trim((Slice){p, llen});
        if (trimmed.len > 0 && trimmed.ptr[0] == '#') {
            Slice doc = slice_trim((Slice){trimmed.ptr + 1, trimmed.len - 1});
            size_t cl = doc.len < sizeof(last_comment) - 1 ? doc.len : sizeof(last_comment) - 1;
            memcpy(last_comment, doc.ptr, cl);
            last_comment[cl] = '\0';
        } else if (trimmed.len == 0) {
            last_comment[0] = '\0';
        } else {
            char tname[64], tparams[128];
            if (parse_task_header(p, llen, tname, sizeof(tname), tparams, sizeof(tparams))) {
                char sig[200];
                if (tparams[0]) snprintf(sig, sizeof(sig), "%s %s", tname, tparams);
                else snprintf(sig, sizeof(sig), "%s", tname);
                if (last_comment[0]) {
                    printf("  \033[1;32m%-24s\033[0m # %s\n", sig, last_comment);
                } else {
                    printf("  \033[1;32m%-24s\033[0m\n", sig);
                }
                task_count++;
            }
            last_comment[0] = '\0';
        }
        p = (nl < end) ? nl + 1 : end;
    }
    if (task_count == 0) {
        printf("  (no tasks defined)\n");
    }
    free(src);
    return true;
}

bool ottofile_run_task(VM *vm, const char *filepath, const char *task_name, int argc, char **argv) {
    const char *path = (filepath && filepath[0]) ? filepath : ottofile_find_default();
    if (!path) {
        fprintf(stderr, "No Ottofile or tasks.otto found in current directory.\n");
        return false;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "Cannot open task file: %s\n", path);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) sz = 0;
    fseek(fp, 0, SEEK_SET);

    size_t extra = 1024;
    char *combined = (char *)malloc((size_t)sz + extra);
    size_t nr = fread(combined, 1, (size_t)sz, fp);
    fclose(fp);

    size_t wp = nr;
    combined[wp++] = '\n';
    wp += (size_t)snprintf(combined + wp, extra - 2, "%s", task_name);
    for (int i = 0; i < argc && wp + 128 < (size_t)sz + extra; i++) {
        bool num_ok = false;
        slice_to_i64((Slice){argv[i], strlen(argv[i])}, &num_ok);
        if (num_ok) {
            wp += (size_t)snprintf(combined + wp, (size_t)sz + extra - wp, " %s", argv[i]);
        } else {
            wp += (size_t)snprintf(combined + wp, (size_t)sz + extra - wp, " \"%s\"", argv[i]);
        }
    }
    combined[wp++] = '.';
    combined[wp++] = '\n';
    combined[wp] = '\0';

    Arena a;
    arena_init(&a, 128 * 1024);
    Parser p;
    parser_init(&p, combined, wp, false, &a);
    ASTNode *prog = parser_parse_program(&p);
    bool ok = false;
    if (p.has_error) {
        help_print_diagnostic(combined, wp, p.error_line, p.error_col, p.error_msg);
    } else if (prog) {
        BytecodeEmitter be;
        bc_init(&be, &vm->perm_arena);
        if (bc_compile_program(&be, prog)) {
            vm->code = be.code;
            vm->code_len = be.count;
            vm->constants = be.constants;
            vm->const_count = be.const_count;
            vm_run(vm);
            ok = true;
        }
    }
    arena_free(&a);
    free(combined);
    return ok;
}
