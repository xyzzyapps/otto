# `otto` (otto shell)

<p align="center">
  <img src="assets/otto-logo.svg" alt="otto shell logo" width="160" />
</p>

> **The Zero-Filler English Automation Shell & Systems Language**

`otto` is a fast, zero-dependency shell language written in C11. It replaces complex combinations of `bash`, `awk`, `sed`, `jq`, `make`, `sqlite3`, `curl`, `watchexec`, `zoxide`, `eza`, `hyperfine`, `miller`, `starship`, and `fzf` with a single static binary and clean, zero-filler English syntax.

---

## 7 Modern Command-Line Capabilities Built-in

| Traditional Utility Replaced | What Developers Usually Install | Native `otto` Syntax |
|:---|:---|:---|
| **`curl` + `jq`** | `curl -s url \| jq .field` | `fetch "https://api.../user" json, print each's name.` |
| **`watchexec` / `entr`** | `find src/ \| entr -r cargo test` | `watch file "src": run "zig build".` |
| **`zoxide` / `autojump`** | `z proj` | `jump "proj"` |
| **`eza --tree` / `fd`** | `eza --tree --ignore-glob ".git"` | `read "." tree, print.` or `every ignoring git file: print.` |
| **`hyperfine`** | `hyperfine --runs 10 'git status'` | `benchmark 10 "git status", table.` |
| **`miller` / `sort \| uniq -c`** | `mlr --csv count-distinct -f dept` | `read "fruits.txt", tally, table.` or `group by dept` |
| **`starship` + `fish`** | Starship prompt + fish ghost text | Dynamic `otto <dir> (branch) [ms] > ` prompt + inline ghost autocomplete |

---

## Key Highlights

- **Flexible Argument Syntax:** Use either concise zero-preposition transitive style (`replace "old" "new"`, `split ","`, `sort age`) or explicit English prepositions (`replace "old" with "new"`, `split by ","`, `sort by age`).
- **Natural Pronoun Registers:** Use `it` for current clause value, `that` for previous sentence output, and `them` for active list/token streams.
- **Implicit Stdin Stream Processing:** Pipe any stream into `otto -n -c 'print each.'` (`-n` / `--stdin`) and `otto` automatically handles the per-line stream loop.
- **Strict Zero-Filler Rule:** No cryptic sigils (`$@`, `awk '{print $2}'`), and no decorative fluff (`the`, `a`, `to`, `of`, `set`, `all`). Every word performs concrete work.
- **Ultra-Fast 4-Register VM:** Flat 64-bit bytecode interpreter with per-iteration arena recycle (`iter_arena`) for zero-allocation stream loops and guaranteed Tail Call Optimization ($O(1)$ stack recursion).
- **Embedded SQLite Engine:** Query and insert directly into `.db` files with automatic schema generation and native `where` query pushdown.
- **Transactional File Safety:** Preview file mutations with `simulate` (unified diff without touching disk) and roll back anytime with `undo last`.
- **Interactive TUI Built-in:** Native fuzzy selector (`pick`), box-drawing terminal grids (`table`), and Unicode charts (`chart`) with zero external packages.
- **Single-Binary Compiler:** Self-bundle any `.otto` script into a standalone, zero-dependency executable (`compile "tool.otto" into "./tool"`).
- **Interactive Login Ergonomics:** Quote-free command passthrough (`git commit -m "fix"`), directory ring buffer (`back`, `dirs`), `$ENV` synchronization, and history substitution (`repeat previous`, `previous's last`).

---

## 10-Second Showcase

### 1. AWK Replacement (Stream Aggregation)
```bash
# Sum scores from file
before: 0 into s. every line "scores.txt": add 2nd number into s. after: print s.
```

### 2. jq Replacement (JSON Filter & Sort)
```bash
read "users.json" json, take where each's active == true, sort by age descending, print each's name.
```

### 3. Safe Batch In-Place Refactoring (Dry-Run & Rollback)
```bash
# Preview diff without touching disk
every file "*.conf": simulate inplace replace "8080" with "9090".

# Apply with auto-journaling in ~/.otto/journal
every file "*.conf": inplace replace "8080" with "9090".

# Instant rollback if anything broke
undo last.
```

### 4. Native SQLite Database Stream
```bash
# Auto-creates table and inserts record
write {host: "web1", cpu: 85} onto "ops.db:metrics".

# Query with pushed-down WHERE clause
every row "ops.db:metrics" where cpu >= 80: print host.
```

### 5. Interactive Fuzzy Process Killer (replaces `ps + grep + fzf + kill`)
```bash
every process: pick, kill.
```

### 6. Native REST Stream & HTTP POST (replaces `curl + jq`)
```bash
# Fetch and parse JSON live
fetch "https://jsonplaceholder.typicode.com/todos/1" json, print each's title.

# Post JSON payload directly onto URL
write {status: "ok", service: "otto"} onto "https://httpbin.org/post", print.
```

### 7. Statistical Benchmarking (replaces `hyperfine`)
```bash
# Run 10 trials, calculate mean, min, max, stddev, and print as table
benchmark 10 "git status", table.
```

### 8. Frecency Smart Jumping (replaces `zoxide / autojump`)
```bash
# Jump to highest-frecency directory matching substring
jump "src"
```

### 9. Frequency Tally & Grouping (replaces `miller / sort | uniq -c`)
```bash
# Count occurrences and render terminal grid
read "access.log", tally, table.
```

### 10. File Change Watcher Loop (replaces `watchexec / entr`)
```bash
# Watch files and auto-rebuild on modification
watch file "src": run "zig build".
```

### 11. Asynchronous Job Control (replaces `&`, `jobs`, `fg`, `kill`)
```bash
# Launch a background process and track its PID/job ID
background run "python -m http.server 8080"

# List active jobs in a formatted terminal table
jobs, table.

# Resume job 1 in foreground or terminate
resume 1.
kill 1.
```

---

## Quick Start

### Build with `zig cc` (Zero Setup)
```powershell
$files = Get-ChildItem src/*.c | ForEach-Object { $_.FullName }
zig cc -Isrc -Ivendor/sqlite3 $files vendor/sqlite3/libsqlite3.a -lwininet -o otto.exe
```

### Run Verification Suites

#### Option 1: Portable Pure C Test Runner (Cross-Platform, No PowerShell Needed)
```bash
# Compile and run the unified C test suite (all 28 tests)
zig cc tests/test_runner.c -o test_runner.exe
./test_runner.exe
```

#### Option 2: PowerShell Runners
```powershell
# 14 Master Specification Tests
pwsh -File tests/run_master_tests.ps1

# 7 Modern Capabilities Tests
pwsh -File tests/test_new_features.ps1
```
*Output: `TEST RESULTS: 28 / 28 PASSED (100% SUCCESS RATE)`*

### Run Interactive Shell (Starship Prompt + Ghost Autocomplete)
```powershell
.\otto.exe
```

---

## Documentation

For a comprehensive guide covering all syntax, registers, verbs, build rules, FFI, and production recipes, see [**`TUTORIAL.md`**](file:///C:/Users/manic/Documents/PROG/oesh/TUTORIAL.md).
