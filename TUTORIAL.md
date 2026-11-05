# `otto` (otto shell) — The Zero-Filler Automation Shell & Systems Language: Complete Tutorial

<p align="center">
  <img src="assets/otto-logo.svg" alt="otto shell logo" width="120" />
</p>

Welcome to `otto` (`otto shell`), a modern, commercial-grade, portable automation shell and systems scripting language. `otto` combines the expressive readability of English one-liners with the raw speed and zero-dependency portability of a C11/Zig static binary.

This tutorial covers every feature of `otto` from basic pipeline ergonomics to advanced systems orchestration, transactional file safety, embedded SQLite streams, interactive TUI components, single-binary compilation, and the 7 modern CLI utilities.

---

## Table of Contents

1. [Philosophy & The Zero-Filler Rule](#1-philosophy--the-zero-filler-rule)
2. [Grammar: The 8 Parts of Speech](#2-grammar-the-8-parts-of-speech)
3. [The 4-Register Virtual Machine](#3-the-4-register-virtual-machine)
4. [Installation & Execution Modes](#4-installation--execution-modes)
5. [Basic Pipelines & Lifecycle Hooks](#5-basic-pipelines--lifecycle-hooks)
6. [Scanners & Ordinals (Stream Processing)](#6-scanners--ordinals-stream-processing)
7. [Structured Data: JSON & YAML](#7-structured-data-json--yaml)
8. [Embedded SQLite Streams](#8-embedded-sqlite-streams)
9. [Transactional File Safety (`simulate` & `undo`)](#9-transactional-file-safety-simulate--undo)
10. [Interactive TUI Verbs (`pick`, `table`, `chart`)](#10-interactive-tui-verbs-pick-table-chart)
11. [Functions & Tail Call Optimization (TCO)](#11-functions--tail-call-optimization-tco)
12. [Binary Formats (`unpack`) & Relational Algebra](#12-binary-formats-unpack--relational-algebra)
13. [Incremental Build Rules (`from` Engine)](#13-incremental-build-rules-from-engine)
14. [Single-Binary Compiler (`compile` Verb)](#14-single-binary-compiler-compile-verb)
15. [Zero-Boilerplate C ABI FFI (`extern` Hook)](#15-zero-boilerplate-c-abi-ffi-extern-hook)
16. [Agentless Fleet Orchestration (`at` Preposition)](#16-agentless-fleet-orchestration-at-preposition)
17. [Interactive Login Shell Ergonomics](#17-interactive-login-shell-ergonomics)
18. [Real-World Production Recipes](#18-real-world-production-recipes)
19. [The 7 Modern CLI Capabilities](#19-the-7-modern-cli-capabilities)
20. [Process & Job Control (`background`, `jobs`, `resume`, `kill`)](#20-process--job-control-background-jobs-resume-kill)
21. [Developer CLI Replacements (`search`, `stat`, `view` & Unit Literals)](#21-developer-cli-replacements-search-stat-view--unit-literals)
22. [Native Structured Git Integration (`status`, `commits`, `branches`, `diff`)](#22-native-structured-git-integration-status-commits-branches-diff)
23. [Crypto, Encoding, UUIDs, TOML & JSONL Streams](#23-crypto-encoding-uuids-toml--jsonl-streams)
24. [System Inspection, `.env` Auto-Loading & `Ottofile` Task Runner](#24-system-inspection-env-auto-loading--ottofile-task-runner)
25. [Fish-Class Interactive Shell & Beginner Onboarding (`abbr`, `Ctrl+R`, `help`, `tutor`)](#25-fish-class-interactive-shell--beginner-onboarding-abbr-ctrlr-help-tutor)

---

## 1. Philosophy & The Zero-Filler Rule

Traditional shell scripting suffers from two extremes:
1. **Cryptic Symbols:** Bash, AWK, and `sed` rely on dense sigils (`$@`, `$$`, `!$`, `awk '{print $2}'`, `sed -i 's/foo/bar/g'`).
2. **Decorative Fluff:** Natural-language attempts like AppleScript or SQL add decorative words (`the`, `a`, `to`, `of`, `set`, `all`) that add noise without semantic value.

`otto` enforces the **Strict Zero-Filler Rule**:
> **Every single word in a sentence must perform semantic work.** Decorative English filler words (`the`, `to`, `of`, `an`, `set`, `all`) are rejected by the lexer at compile time.

### Example: Rejecting Decorative Words
```bash
# REJECTED (Throws Syntax Error: decorative filler word 'to', 'of'):
To gcd of a and b.

# VALID (Clean, unambiguous, strict zero-filler):
gcd a b: return a if b == 0 else gcd b (a % b).
```

Sentences end with a period (`.`) in scripts or automated pipelines. In interactive REPL mode, pressing `Enter` automatically supplies the terminating period.

---

## 2. Grammar: The 8 Parts of Speech

Every valid token in `otto` belongs to one of eight grammatical classes:

| Class | Description | Examples |
|---|---|---|
| **Hooks** | Lifecycle triggers & definitions | `before:`, `every:`, `after:`, `on:`, `watch:`, `extern:` |
| **Verbs** | Actions performed on streams or values | `read`, `write`, `print`, `take`, `drop`, `sort`, `replace`, `add`, `count`, `run`, `compile`, `undo`, `pick`, `table`, `chart`, `unpack`, `search`, `stat`, `view`, `hash`, `encode`, `decode`, `uuid`, `disk`, `abbr`, `prepend`, `append`, `help`, `examples`, `tutor` |
| **Scanners** | Tokenizers for unstructured or structured streams | `line`, `lines`, `word`, `words`, `number`, `numbers`, `byte`, `bytes`, `file`, `files`, `process`, `processes`, `row`, `rows`, `json`, `jsonl`, `csv`, `yaml`, `toml`, `tree`, `port`, `ports`, `status`, `commits`, `branches`, `diff`, `host`, `hosts` |
| **Ordinals** | Positional indexers & field selectors | `first` (`1st`), `second` (`2nd`), `third` (`3rd`), `fourth` (`4th`), `last`, `nth`, `top`, `each` |
| **Adverbs** | Modifiers specifying verb execution semantics | `recursively`, `numerically`, `descending`, `uniquely`, `inplace`, `concurrently`, `casefold`, `simulate`, `parallel`, `background`, `ignoring git` |
| **Prepositions** | Target slots for inputs, outputs, and parameters | `into`, `onto`, `from`, `with`, `by`, `where`, `matching`, `at`, `in`, `discard`, `replacing` |
| **Conjunctions** | Stream routing and pipeline logic | `,` (pipe), `and` (parallel tee), `or` (fallback/error recovery) |
| **Registers** | Fast hardware and execution state tokens | `it` (`acc`), `each` (`cur`/`item`), `them` (`fld`), `that`, `error`, `previous` |

---

## 3. The 4-Register Virtual Machine

`otto` does not interpret AST trees at runtime. Instead, programs compile directly into a 64-bit flat bytecode array executed by a register-based VM:

1. **`acc` (Accumulator):** Holds the primary result of the most recent expression, command, or verb. It flows through `,` pipeline conjunctions. Expressed via the pronoun `it`.
2. **`cur` (`item`):** Holds the active stream element inside an `every` loop iteration. Expressed via the pronoun `each`.
3. **`fld` (Ordinal Field Vector):** Lazily tokenizes the current line into positional words and numbers when accessed via ordinals (`first word`, `2nd number`).
4. **`err` (Error Register):** Holds runtime errors or process exit codes. If `err` is non-empty, execution short-circuits to the nearest `or` clause.
5. **`that` (Prior Sentence Output):** Preserves the accumulator output of the immediately preceding sentence across periods (`.`), enabling natural sentence-to-sentence data pipelining.
6. **`them` (Active Collection / List):** Refers to the current list of items generated by verbs like `split`, `fetch`, or `read`.

### Zero-Allocation Stream Loops
`otto` utilizes an **iteration arena** (`iter_arena`). During each iteration of an `every` stream loop, intermediate allocations (split lines, temporary regex results, string joins) are reset to offset 0 in $O(1)$ time, guaranteeing steady-state memory usage even across terabyte-scale log streams.

---

## 4. Flexible Argument Syntax: Zero-Preposition vs Prepositional

`otto` supports both concise zero-preposition transitive style and explicit prepositional phrasing. You can freely choose the style that feels most natural:

| Verb Operation | Zero-Preposition Positional Syntax | Explicit Prepositional Syntax |
|:---|:---|:---|
| **String Substitution** | `replace "old" "new"` | `replace "old" with "new"` |
| **Stream Splitting** | `split ","` | `split by ","` |
| **Ordering** | `sort age descending` | `sort by age descending` |
| **Aggregation / Bucketing**| `group dept` | `group by dept` |
| **Arithmetic Mutation** | `add 10 total` | `add 10 into total` |
| **Set Difference** | `exclude "tests"` | `exclude from "tests"` |
| **Binary Compilation** | `compile "tool.otto" "./tool"` | `compile "tool.otto" into "./tool"` |
| **File Copy / Move** | `copy "a" "b"` / `move "a" "b"` | `copy "a" into "b"` / `move "a" into "b"` |
| **Code Search** | `search "TODO" "src"` | `search "TODO" in "src"` |
| **Live Abbreviations** | `abbr "gs" "read status, table"` | `abbr "gs" with "read status, table"` |
| **PATH / List Mutation** | `prepend "~/.local/bin" $PATH` | `prepend "~/.local/bin" into $PATH` |
| **Hashing & Encoding** | `hash sha256` / `encode base64` | `hash with "sha256"` / `encode with "base64"` |

### Pronoun Pipelining Across Sentences
Sentences in `otto` can chain results effortlessly using `that` and `them`:
```powershell
# 'that' refers to the prior sentence's evaluation
100 into score. add 50 that. print that.
# Output: 150

# 'them' refers to active token list
"cherry banana apple", split " ", sort them descending, print first.
# Output: cherry
```

### Implicit Stdin Stream Processing (`-n` / `--stdin`)
When piped into from another program or shell pipeline, passing `-n` (or `--stdin`) establishes an implicit stream loop over lines without requiring explicit `every line: ...`:
```bash
cat access.log | otto -n -c 'print each.'
```

---

## 4. Installation & Execution Modes

### Compilation from Source
`otto` is written in standard C11 and compiles cleanly with `zig cc`:

```powershell
# Compile single binary
$files = Get-ChildItem src/*.c | ForEach-Object { $_.FullName }
zig cc -Isrc -Ivendor/sqlite3 $files vendor/sqlite3/libsqlite3.a -lwininet -o otto.exe
```

### Execution Modes

1. **One-Liner Execution (`-c`):**
   ```powershell
   .\otto.exe -c 'print "Hello world!".'
   ```

2. **File Script Execution:**
   ```powershell
   .\otto.exe myscript.otto
   ```

3. **Interactive Login REPL:**
   ```powershell
   .\otto.exe
   ```

4. **Agentless SSH RPC Mode (`--rpc`):**
   Used by remote `at host` clauses to stream and execute bytecode payloads without remote installation.

---

## 5. Basic Pipelines & Lifecycle Hooks

### Pipeline Conjunctions: `,`, `and`, and `or`

- **Pipe (`,`):** Passes `acc` from left to right:
  ```powershell
  read "access.log", take 5 lines, print.
  ```

- **Parallel Tee (`and`):** Duplicates the input accumulator into both clauses simultaneously:
  ```powershell
  read "backup.cfg", print first line and print count lines.
  ```

- **Fallback Recovery (`or`):** Short-circuits if the left side fails:
  ```powershell
  read "primary.cfg" or read "backup.cfg", print.
  ```

### Lifecycle Blocks: `before:`, `every:`, `after:`

`otto` eliminates AWK's cryptic syntax with explicit lifecycle hooks:

```powershell
# Sum scores from file
before: 0 into s.
every line "scores.txt": add 2nd number into s.
after: print s.
```

- `before:` runs once before any streams begin.
- `every:` iterates over every element in the stream, resetting `iter_arena` between cycles.
- `after:` executes once the stream hits EOF.

---

## 6. Scanners & Ordinals (Stream Processing)

### Scanners
Scanners instruct `otto` how to slice inputs:
- `line` / `lines`: Reads input line by line.
- `word` / `words`: Extracts whitespace-delimited tokens.
- `number` / `numbers`: Extracts integer and floating-point values.
- `file` / `files`: Glob-scans directories (e.g., `every file "*.log": ...`).
- `process` / `processes`: Enumerate running operating system processes.

### Ordinals & Genitives
Ordinals access structured elements:
- `first`, `second`, `third`, `fourth`, `last`, `nth` (e.g., `5th`, `10th`).
- Possessive genitive `'s` accesses map properties: `each's name`, `item's active`.

```powershell
# Extract process information
every process: print first word and print 2nd number.

# Ordinal extraction from text
every line "audit.log": print 4th word.
```

---

## 7. Structured Data: JSON & YAML

`otto` includes native, high-performance parsers for JSON and YAML. No external dependencies like `jq` or `yq` are required.

### JSON Filtering & Navigation
```powershell
# Parse JSON array of objects, filter, sort, and print
read "users.json" json, take where each's active == true, sort by age descending, print each's name.
```

### YAML Stream Processing
```powershell
# Parse and inspect YAML configuration
read "cluster.yaml" yaml, print.
```

---

## 8. Embedded SQLite Streams

`otto` embeds SQLite 3. Databases work identically to plain text files. 

### Writing / Appending to SQLite Tables
Using the syntax `"database.db:table"` automatically infers columns, types, and creates the table schema if it does not exist:

```powershell
# Auto-creates metrics table with schema (host TEXT, cpu INTEGER) and inserts row
write {host: "web1", cpu: 85} onto "ops.db:metrics".
```

### Querying SQLite with WHERE Pushdown
When iterating over SQLite rows with `where`, `otto` automatically compiles the expression down into a native SQL query pushdown:

```powershell
# Native SQL query pushdown: SELECT * FROM metrics WHERE cpu >= 80;
every row "ops.db:metrics" where cpu >= 80: print host.
```

---

## 9. Transactional File Safety (`simulate` & `undo`)

Modifying files in production can be dangerous. `otto` provides a copy-on-write transaction log.

### Dry-Run Simulation (`simulate`)
Prepending `simulate` prints a unified diff without touching disk:

```powershell
# Dry-run preview: shows diff without modifying disk
every file "*.conf": simulate inplace replace "8080" with "9090".
```

### In-Place Atomic Mutations
```powershell
# Executes replacement and automatically writes original copy to ~/.otto/journal
every file "*.conf": inplace replace "8080" with "9090".
```

### Instant Rollback (`undo last`)
If anything breaks, revert the last batch transaction with a single verb:

```powershell
# Restores files to their exact pre-mutation state
undo last.
```

---

## 10. Interactive TUI Verbs (`pick`, `table`, `chart`)

`otto` eliminates the need to install `fzf`, `column`, or `gnuplot`.

### Interactive Fuzzy Selector (`pick`)
Streams current items into an inline fuzzy selector:

```powershell
# Fuzzy kill processes interactively
every process: pick, kill.

# Fuzzy file opening
every file "*.txt": pick into selected.
```

### Terminal Grid (`table`)
Renders JSON, YAML, or list data into an aligned terminal box grid:

```powershell
read "users.json" json, table.
```

Output:
```
+------+-------+
| age  | name  |
+======+=======+
| 25   | Alice |
| 40   | Bob   |
+------+-------+
```

### Terminal Charts (`chart`)
Renders sparklines or bar histograms using Unicode block glyphs:

```powershell
every line "access.log": take 4th word, tally, chart.
```

---

## 11. Functions & Tail Call Optimization (TCO)

### Defining Functions
Functions are declared with `name param1 param2: body.` syntax:

```powershell
# Greatest common divisor
gcd a b: return a if b == 0 else gcd b (a % b).
print gcd 1071 462.
# Outputs: 21
```

### Guaranteed Deep TCO
Recursive calls in tail position execute in $O(1)$ stack space by overwriting local frame registers, preventing call stack overflow:

```powershell
# 1,000,000 iterations in constant stack memory
count_down n: return 0 if n <= 0 else count_down (n - 1).
print count_down 1000000.
# Outputs: 0
```

---

## 12. Binary Formats (`unpack`) & Relational Algebra

### Binary Unpacking (`unpack`)
Parse raw binary headers directly into registers and variables:

```powershell
# Unpack little-endian uint32, uint8, uint8
read "sample.bin", take first 8 bytes, unpack "u32 u8 u8" into magic bits endian, print magic.
```

### Relational Set Algebra (`exclude`, `intersect`, `zip`)
Perform set operations directly on newline-separated streams or lists:

```powershell
# Exclude blacklisted items, intersect with verified users, and count matches
exclude "blacklist.txt" from "candidates.txt", intersect with "verified.txt", print count.
```

---

## 13. Incremental Build Rules (`from` Engine)

Replace Makefiles with natural dependency statements:

```powershell
# Rebuilds hello.exe only if hello.c is newer than hello or hello is missing
"hello" from "hello.c": run "zig cc hello.c -o hello.exe".
```

The VM checks `stat()` modification timestamps (`st_mtime`) on source and target before deciding whether to execute the rule body.

---

## 14. Single-Binary Compiler (`compile` Verb)

`otto` allows self-bundling scripts into standalone, zero-dependency binaries.

### Creating a Standalone Binary
```powershell
compile "deploy.otto" into "./deploy-tool".
```

### How It Works:
1. The VM compiles `deploy.otto` into a compact binary bytecode chunk.
2. It clones the active `otto` runtime stub.
3. It appends the bytecode, byte length, and an 8-byte magic footer (`OTTO_MAGIC`) to the end of the executable.
4. On launch, the resulting executable inspects its own footer; if present, it loads and runs the embedded program directly without launching a shell prompt.

---

## 15. Zero-Boilerplate C ABI FFI (`extern` Hook)

Bind directly to native shared libraries (`.so`, `.dll`, `.dylib`) without writing C glue:

```powershell
# Bind to standard C library puts
extern "msvcrt.dll" puts (string) into c_puts.
run c_puts "Hello from C FFI!".
```

---

## 16. Agentless Fleet Orchestration (`at` Preposition)

Run commands and pipelines across a fleet of remote servers over SSH without installing agents:

```powershell
# Stream AST clauses over SSH, return structured output into local table
every host "servers.txt": concurrently at each (read "/var/log/syslog", take lines matching "OOM", count) into report.
after: print report.
```

---

## 17. Interactive Login Shell Ergonomics

When running interactively, `otto` provides modern login ergonomics:

### 1. Passthrough Command Mode
External commands and flags don't require quotes or `run`:
```powershell
# Direct invocation
git commit -m "fix issue"

# Piped into native verbs
git log --oneline -n 50, take lines matching "fix", table.
```

### 2. Stateful Directory Navigation
- **Bare Path `cd`:** Typing `../src` or `/var/log` automatically changes directories.
- **Directory Ring Buffer:**
  - `back`: Returns to previous directory.
  - `dirs`: Emits visited directory stack.
  - `dirs, pick, cd`: Interactive fuzzy directory jumping.

### 3. Environment Variable Synchronization
Reading or writing `$ENV_VAR` synchronizes directly with the process environment:
```powershell
# Append to PATH
"/opt/bin:$PATH" into $PATH
```

### 4. History Substitutions via Ordinals
Replace bash bang-history with readable register tokens:
- `repeat previous`: Re-executes previous command (`!!`).
- `repeat previous with "sudo"`: Prepend command (`sudo !!`).
- `previous's last`: Grabs the last argument (`!$`).
  ```powershell
  mkdir "/tmp/build", cd previous's last
  ```

---

## 18. Real-World Production Recipes

### Automated Log Analysis & Alerting
```powershell
every line "access.log" where each's status >= 500: tally by ip, sort descending, table.
```

### Database Health Monitor
```powershell
every 5s: read "/proc/loadavg", take first number into load, write {time: now, load: load} onto "monitor.db:system".
```

### Clean Inactive Artifacts
```powershell
every file "builds/*" where each's age > 30d: simulate remove.
```

---

## 19. The 7 Modern CLI Capabilities

`otto` natively includes 7 capabilities modeled after the most popular modern terminal utilities, eliminating external dependencies.

### 19.1 Native HTTP REST Streams (`fetch` & `onto` POST)
Replaces `curl` and `jq`:
- **HTTP GET with Native Parsing:**
  ```powershell
  # Stream raw text or parse JSON/YAML directly
  fetch "https://api.github.com/zen", print.
  fetch "https://jsonplaceholder.typicode.com/todos/1" json, print each's title.
  ```
- **HTTP POST via `onto` Preposition:**
  ```powershell
  write {status: "ok", service: "otto"} onto "https://httpbin.org/post", print.
  ```

### 19.2 File Change Watcher Loop (`watch` Hook)
Replaces `entr`, `watchexec`, and `nodemon`:
```powershell
# Auto-rebuild on any file modification
watch file "src": run "zig build".
```

### 19.3 Frecency Smart Directory Navigation (`jump` Verb)
Replaces `zoxide` and `autojump`:
- Computes frecency scores ($F \times 10 + R$) across visited paths in the directory ring buffer:
  ```powershell
  # Jumps directly into the highest-ranked matching project directory
  jump "src"
  ```

### 19.4 Git-Aware Directory Scanning (`tree` & `ignoring git`)
Replaces `eza --tree`, `fd`, and `ripgrep` exclusions:
- **Tree Scanner:**
  ```powershell
  # Renders visual hierarchy of files and subdirectories
  read "src" tree, print.
  ```
- **`ignoring git` Adverb:**
  ```powershell
  # Automatically ignores .git/ internals
  every ignoring git file "src/*": print.
  ```

### 19.5 Statistical Benchmarking (`benchmark` Verb)
Replaces `hyperfine`:
- High-precision microsecond timers measuring `runs`, `mean_ms`, `min_ms`, `max_ms`, and `stddev_ms`:
  ```powershell
  # Run 10 iterations and render statistics
  benchmark 10 "git status", table.
  ```

### 19.6 Dataframe Group Aggregations (`group by` & `tally`)
Replaces `miller` and `awk | sort | uniq -c`:
- **Frequency Tally:**
  ```powershell
  # Counts distinct values and outputs sorted frequencies
  read "fruits.txt", tally, table.
  ```
- **Property Grouping:**
  ```powershell
  # Groups rows or JSON maps by property
  read "sales.json" json, group by dept, print.
  ```

### 19.7 Starship-Style Prompt & Inline Ghost Autocomplete
Replaces `starship` and `fish` autosuggestions:
- **Starship Prompt:** Shows current directory, git branch (`(main)`), and last command execution duration (`[12ms]`):
  ```
  🦦 otto oesh (main) [4ms]
  > 
  ```
- **Inline Ghost Text:** Automatically suggests matching commands from history in dim gray text as you type. Press `Right Arrow` or `Tab` to accept.

---

## 20. Process & Job Control (`background`, `jobs`, `resume`, `kill`)

`otto` replaces legacy Unix background operators (`&`, `nohup`), process status listings (`ps`, `jobs`), and foregrounding commands (`fg`, `kill`) with first-class zero-filler sentence verbs:

### 20.1 Launching Background Tasks (`background run`)
Use the `background` adverb before `run` to spawn long-running child processes asynchronously without blocking the shell:
```powershell
background run "python -m http.server 8080".
```
The shell registers the job, assigns an internal sequential `id` and OS `pid`, and returns a structured record:
```
[Job 1 started: PID 17402] python -m http.server 8080
```

### 20.2 Inspecting Background Jobs (`jobs`)
The `jobs` verb returns a stream list of active and recent background tasks:
```powershell
# Print raw list of jobs
jobs, print.

# Render structured job table with PID, status, and command
jobs, table.
```
Output:
```
+------+-------+------------------------------+---------+
| id   | pid   | command                      | status  |
+======+=======+==============================+=========+
| 1    | 17402 | python -m http.server 8080   | Running |
+------+-------+------------------------------+---------+
```

### 20.3 Bringing Jobs to Foreground (`resume`)
To wait for a background job to finish and bring its execution to the foreground:
```powershell
# Wait for job 1 to complete
resume 1.

# Resume the most recent job
resume.
```

### 20.4 Terminating Jobs and Processes (`kill`)
Terminate background tasks by job ID, OS PID, or by piping from fuzzy selection:
```powershell
# Terminate job 1
kill 1.

# Terminate process by PID directly
kill 17402.

# Interactive fuzzy search and termination
every process: pick, kill.
```

---

## 21. Developer CLI Replacements (`search`, `stat`, `view` & Unit Literals)

`otto` replaces `ripgrep` (`rg`), `fd`, `eza`, and `bat` natively with structured map streams — no text scraping required.

### 21.1 Multi-File `.gitignore`-Aware Code Search (`search`)
Searches directories recursively while automatically respecting `.gitignore` rules and skipping `.git`, `node_modules`, `dist`, and binary files. Returns a list of `{file, line, col, text}` maps:

```powershell
# Case-insensitive search across src/ rendered as a box table
search casefold "TODO" in "src", table.

# Zero-preposition shorthand + interactive fuzzy picker
search "parse_command" "src", pick, print each's file.
```

### 21.2 Rich File Metadata & Unit Literals (`stat`, `kb`/`mb`/`gb`, `ms`/`s`/`d`, `:port`)
The `stat` verb returns structured `{path, name, ext, size, age, modified, dir}` metadata maps. In addition, file-path slices inside `every file` loops lazily expose `.size`, `.ext`, `.age`, `.modified`, `.name`, and `.dir` via `'s` genitives, and first-class unit literals (`10kb`, `5mb`, `2gb`, `500ms`, `30d`, `:8080`) evaluate directly in expressions:

```powershell
# List largest files in src/
stat recursively "src/*", sort size descending, take 10, table.

# Filter files larger than 10 KB modified within the last 7 days
every recursively file "src/*" where each's size > 10kb: print each's name.
```

### 21.3 Syntax-Highlighted File Viewer & Pager (`view`)
Replaces `bat` and `less` with automatic syntax coloring (C, JS/TS, Python, Rust, Go, JSON, YAML, TOML, Markdown, Otto), line numbers, header borders, and an interactive pager (`SPACE` / `j` / `k` / `q`) when attached to a terminal:

```powershell
view "src/vm.c".
```

---

## 22. Native Structured Git Integration (`status`, `commits`, `branches`, `diff`)

Instead of parsing fragile `git` text output with `awk` or `cut`, `otto` provides native git scanners (with optional `git` prefix for readability):

```powershell
# Working-tree status as {state, file} maps
read status, table.
every git status where each's state == "M": print each's file.

# Commit history as {hash, author, date, message} maps
read git commits, take 10, table.

# Branches as {name, current} maps
read git branches, table.

# Syntax-colored delta-style unified diff
read diff, print.
read "src/vm.c" diff, print.
```

---

## 23. Crypto, Encoding, UUIDs, TOML & JSONL Streams

### 23.1 Hashing, Encoding & UUID v4 (`hash`, `encode`, `decode`, `uuid`)
Replaces `sha256sum`, `cksum`, `base64`, `xxd`, `python -c "import urllib.parse..."`, and `uuidgen`:

```powershell
# FIPS 180-4 SHA-256 and IEEE CRC32 (works on strings or files)
"hello world", hash sha256, print.
"hello world", hash crc32, print.

# Base64, Hex, and RFC 3986 URL encoding/decoding
"otto shell", encode base64, decode base64, print.
"otto", encode hex, print.
"query with spaces & symbols", encode url, print.

# Cryptographically random RFC 4122 UUID v4
uuid, print.
```

### 23.2 Native TOML & Streaming JSONL (`toml` & `jsonl`)
Parse `Cargo.toml` / `pyproject.toml` configurations and stream multi-gigabyte `.jsonl` / NDJSON logs line-by-line in constant $O(1)$ iteration arena memory:

```powershell
# Parse TOML tables and nested [section.subsection] keys
read "Cargo.toml" toml into cfg.
print cfg's package's name.

# Stream JSON Lines with constant memory footprint
every jsonl "events.jsonl" where each's code >= 500: print each's service.
```

---

## 24. System Inspection, `.env` Auto-Loading & `Ottofile` Task Runner

### 24.1 Network Port Scanner (`ports`) & Disk Usage (`disk`)
Replaces `lsof -i`, `ss -tulnp`, `netstat`, `duf`, and `dust`:

```powershell
# Inspect active TCP/UDP sockets as {proto, local, port, state, pid} maps
read ports, table.
every port where port == :8080: kill.

# Mounted filesystem capacity summary (duf replacement)
disk, table.

# Directory size breakdown sorted largest-first (dust replacement)
disk "src", table.
```

### 24.2 Automatic `.env` Directory Environment Loader (`direnv` Replacement)
Whenever `otto` starts or changes directories (`cd`, `jump`, `back`), it automatically checks for a `.env` file in the target directory and loads `KEY=VALUE` / `export KEY="VALUE"` pairs into process `$ENV` variables without external shell hooks.

### 24.3 Native `Ottofile` / `tasks.otto` Project Task Runner (`just` / `make` Replacement)
Define zero-filler functions with `#` docstrings in an `Ottofile` (or `tasks.otto`):

```powershell
# Build release binary
build mode:
    print "Building in " + mode + " mode...".
    run "zig build -Doptimize=ReleaseFast".

# Run full portable test suite
test:
    run "./test_runner".
```

List and run tasks directly from the CLI:
```bash
otto --tasks              # Lists tasks, parameters, and # docstrings
otto task build release   # Invokes `build "release".`
otto -f custom.otto tasks # Use a custom task file
```

---

## 25. Fish-Class Interactive Shell & Beginner Onboarding (`abbr`, `Ctrl+R`, `help`, `tutor`)

### 25.1 Interactive REPL Keyboard Shortcuts
| Shortcut | Action |
|:---|:---|
| **`Tab`** | Context-aware completion menu (cycling through verbs, variables, `abbr`, `$ENV`, `git` subcommands, and file paths) |
| **`Right Arrow` / `End` / `Ctrl+E`** | Accept inline gray ghost history autosuggestion |
| **`Up` / `Down`** | Prefix-filtered persistent history navigation (`~/.otto/history`) |
| **`Ctrl+R`** | Full-screen interactive fuzzy history search (powered by `pick`) |
| **`Ctrl+Left` / `Ctrl+Right`** | Jump cursor backward / forward by word |
| **`Home` / `Ctrl+A`** | Move cursor to start of line |
| **`Ctrl+W` / `Ctrl+U` / `Ctrl+K`** | Delete previous word / kill to start / kill to end of line |
| **`Ctrl+L`** | Clear terminal screen and redraw Starship prompt |

### 25.2 Live Fish-Style Abbreviations (`abbr`) & Deduplicated `$PATH` (`prepend` / `append`)
Unlike opaque `alias` wrappers, `abbr` expands visibly in the prompt when you press `Space` or `Enter` so command history stays readable and self-documenting (save favorites in `~/.ottorc`):

```powershell
abbr "gs" "read status, table".
abbr "gl" "read commits, take 10, table".

# List all registered abbreviations
abbr, table.

# Clean, deduplicated $PATH management
prepend "~/.local/bin" into $PATH.
append "/opt/tools/bin" into $PATH.
```

### 25.3 Built-In Cheat Sheets, Visual Diagnostics & Interactive Tutor
`otto` is designed so beginners never get stuck:
- **Offline Cheat Sheets (`help` & `examples`):**
  ```powershell
  help.              # Browse all categorized topics
  help search.       # Syntax + explanation + examples for `search`
  examples git.      # Copy-pasteable git one-liners
  ```
- **Visual Caret Diagnostics, Typo Suggestions & Rosetta Hints:**
  Mistyped verbs (`prnit "hi"`) suggest the closest keyword (`Did you mean 'print'?`), `=` vs `into` errors underline the exact column with `^~~~`, and muscle-memory Unix commands (`ll`, `grep`, `cat`, `lsof`) display idiomatic `otto` equivalents.
- **5-Lesson Interactive Walkthrough (`otto tutor`):**
  Run `otto tutor` from your terminal (or `tutor.` inside the REPL) for a hands-on 5-minute guided tour executing live `otto` pipelines step by step.


