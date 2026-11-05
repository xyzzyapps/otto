# `otto` (otto shell)

<p align="center">
  <img src="assets/otto-logo.svg" alt="otto shell logo" width="160" />
</p>

> **The Zero-Filler English Automation Shell & Systems Language**

`otto` is an ultra-fast, zero-dependency interactive login shell and automation language written in pure C11. It replaces complex combinations of `bash`, `fish`, `awk`, `sed`, `jq`, `rg`, `fd`, `eza`, `bat`, `delta`, `just`, `direnv`, `duf`, `dust`, `lsof`, `sqlite3`, `curl`, `watchexec`, `zoxide`, `hyperfine`, `miller`, `starship`, and `fzf` with a **single static binary** and clean, zero-filler English sentences.

---

## Prebuilt Static Binaries

Download standalone binaries (zero runtime dependencies) from [**GitHub Releases (`v1.0.0`)**](https://github.com/xyzzyapps/otto/releases/tag/v1.0.0):

| Platform | Architecture | Binary |
|:---|:---|:---|
| **Windows** | `x86_64` | [`otto-windows-x86_64.exe`](https://github.com/xyzzyapps/otto/releases/latest/download/otto-windows-x86_64.exe) |
| **Linux** | `x86_64` | [`otto-linux-x86_64`](https://github.com/xyzzyapps/otto/releases/latest/download/otto-linux-x86_64) |
| **Linux** | `ARM64` (`aarch64`) | [`otto-linux-aarch64`](https://github.com/xyzzyapps/otto/releases/latest/download/otto-linux-aarch64) |
| **macOS** | `Apple Silicon` (`M1–M4`) | [`otto-macos-aarch64`](https://github.com/xyzzyapps/otto/releases/latest/download/otto-macos-aarch64) |
| **macOS** | `Intel` (`x86_64`) | [`otto-macos-x86_64`](https://github.com/xyzzyapps/otto/releases/latest/download/otto-macos-x86_64) |

---

## Modern CLI Utilities Built Natively Into `otto`

| Traditional Utility Replaced | What Developers Usually Install | Native `otto` Syntax |
|:---|:---|:---|
| **`ripgrep` (`rg`) / `grep`** | `rg -i "TODO" src/` | `search -i "TODO" in "src", table.` _(or `search casefold`)_ |
| **`eza -la` / `fd` / `stat`** | `eza -la --sort=size` | `stat "src/*", sort size descending, table.` |
| **`bat` / `less`** | `bat src/main.c` | `view "src/main.c".` |
| **`git` + `delta` / `lazygit`** | `git log --oneline` / `git status -s` | `read commits, table.` &bull; `read status, table.` &bull; `read diff, print.` |
| **`curl` + `jq` / `yq` / `taplo`** | `curl -s url \| jq .` / TOML & JSONL | `fetch "https://api.../user" json` &bull; `read "Cargo.toml" toml` &bull; `every jsonl "events.jsonl": ...` |
| **`sha256sum` / `base64` / `uuidgen`** | `echo -n hi \| sha256sum` | `"hi", hash sha256.` &bull; `encode base64` / `decode url` &bull; `uuid.` |
| **`lsof` / `ss` / `netstat`** | `lsof -i :8080` | `every port where port == :8080: kill.` or `read ports, table.` |
| **`duf` / `dust` / `du` / `df`** | `duf` & `dust src/` | `disk, table.` (mounts) &bull; `disk "src", table.` (folder breakdown) |
| **`just` / `make`** | `just --list` & `just deploy prod` | `otto --tasks` &bull; `otto task deploy prod` (reads `Ottofile`) |
| **`direnv` / `dotenv`** | `eval "$(direnv hook bash)"` | Automatic `.env` loading on startup and `cd` / `jump` / `back` |
| **`watchexec` / `entr`** | `find src/ \| entr -r zig build` | `watch file "src": run "zig build".` |
| **`zoxide` / `autojump`** | `z proj` | `jump "proj"` |
| **`hyperfine`** | `hyperfine --runs 10 'git status'` | `benchmark 10 "git status", table.` |
| **`miller` / `sort \| uniq -c`** | `mlr --csv count-distinct -f dept` | `read "fruits.txt", tally, table.` or `group by dept` |
| **`fish` + `starship` + `fzf`** | Prompt + autosuggest + `abbr` + `Ctrl+R` | Dynamic `otto <dir> (branch) [ms]` prompt, `Tab` menu, `Ctrl+R` fuzzy history, `abbr` |

---

## Key Highlights

- **Fish-Class Interactive Shell:** Full cursor editing (`Left`/`Right`/`Home`/`End`/`Ctrl+W`/`Ctrl+U`/`Ctrl+K`), cycling `Tab` completion menu (verbs, variables, `$ENV`, `git` subcommands, file paths), persistent `~/.otto/history` with `Up`/`Down` prefix filtering & `Ctrl+R` fuzzy picker, live `abbr` abbreviations, and deduplicated `prepend`/`append` on `$PATH`.
- **Beginner-Friendly Onboarding:** Built-in interactive 5-lesson walkthrough (`otto tutor`), offline cheat sheets (`help`, `examples`), visual `^~~~` caret error diagnostics, typo "Did you mean?" suggestions, and Bash-to-Otto Rosetta tips.
- **Flexible Argument Syntax:** Choose concise zero-preposition transitive style (`replace "old" "new"`, `search "TODO" "src"`, `split ","`, `sort age`) or explicit prepositions (`replace "old" with "new"`, `search "TODO" in "src"`, `split by ","`, `sort by age`).
- **Natural Pronoun Registers & Unit Literals:** Use `it`, `that`, and `them` to flow data across clauses and sentences, plus human unit literals (`10mb`, `4kb`, `500ms`, `7d`, `:8080`).
- **Strict Zero-Filler Rule:** No cryptic sigils (`$@`, `awk '{print $2}'`), and no decorative fluff (`the`, `to`, `of`, `an`, `set`, `all`). Every word performs concrete work.
- **Embedded SQLite & Transactional Safety:** Query `.db` files with native `where` SQL pushdown, preview file mutations via `simulate` (unified diff without touching disk), and roll back anytime with `undo last`.
- **Single-Binary Compiler:** Self-bundle any `.otto` script into a standalone executable (`compile "tool.otto" into "./tool"`).

---

## 15-Second Showcase

### 1. Multi-File `.gitignore`-Aware Code Search & Rich File Metadata (`rg` + `fd` + `eza`)
```bash
# Search codebase respecting .gitignore (supports -i or casefold)
search -i "TODO" in "src", table.

# Filter files recursively using human unit literals (supports -r or recursively)
every -r file "src/*" where each's size > 10kb: print each's name.
stat -r "src", sort size descending, take 5, table.
```

### 2. Syntax-Highlighted Pager & Native Structured Git Scanners (`bat` + `delta`)
```bash
# View source file with syntax colors, line numbers, and interactive pager
view "src/vm.c".

# Query git status, commits, branches, and colored diffs as structured streams
read status, table.
read commits, take 5, table.
read diff, print.
```

### 3. Crypto, Encoding, UUIDs, TOML & JSONL (`sha256sum` + `base64` + `taplo` + `jq`)
```bash
"hello world", hash sha256, print.
"otto shell", encode base64, decode base64, print.
uuid, print.

read "Cargo.toml" toml into cfg. print cfg's package's name.
every jsonl "events.jsonl" where each's status >= 500: print each's service.
```

### 4. Port Management, Disk Breakdown & `Ottofile` Task Runner (`lsof` + `duf` + `dust` + `just`)
```bash
# Inspect listening sockets and mounted filesystems
read ports, table.
disk, table.
disk "src", table.

# List and execute recipes defined in Ottofile / tasks.otto
otto --tasks
otto task build release
```

### 5. Fish-Style Abbreviations, `$PATH` Hygiene & Beginner Tutor
```bash
# Register live abbreviations (expands on Space/Enter in REPL and in scripts)
abbr "gs" "read status, table"
abbr "gc" "git commit -m"

# Deduplicated $PATH management (no more manual colon string surgery)
prepend "~/.local/bin" into $PATH.

# Launch built-in cheat sheets or the 5-minute interactive tutor
otto help search
otto examples git
otto tutor
```

### 6. Safe Batch Refactoring, Embedded SQLite & Standalone Binary Bundling
```bash
# Preview unified diff without touching disk, apply with journaling, or roll back
every file "*.conf": simulate inplace replace "8080" "9090".
every file "*.conf": inplace replace "8080" "9090".
undo last.

# Auto-create SQLite schema and query with pushed-down SQL WHERE
write {host: "web1", cpu: 85} onto "ops.db:metrics".
every row "ops.db:metrics" where cpu >= 80: print host.

# Bundle script into a zero-dependency native binary
compile "deploy.otto" into "./deploy".
```

---

## Quick Start

### Build from Source with `zig cc` (Zero Setup)
```powershell
zig cc -O2 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -c vendor/sqlite3/sqlite3.c -o sqlite3.o
$srcs = Get-ChildItem src/*.c | ForEach-Object { $_.FullName }
zig cc -std=c11 -O2 -Isrc -Ivendor/sqlite3 $srcs sqlite3.o -lwininet -lws2_32 -ladvapi32 -o otto.exe
```

### Run Portable Pure C Verification Suite (All 34 Tests)
```bash
zig cc -O2 tests/test_runner.c -o test_runner.exe
./test_runner.exe
```
*Output: `TEST RESULTS: 34 / 34 PASSED (100% SUCCESS RATE)`*

### Launch Interactive Login Shell or Interactive Tutor
```bash
./otto          # Interactive shell with Starship prompt, Tab menu, Ctrl+R & ghost autocomplete
./otto tutor    # 5-lesson interactive beginner walkthrough
```

---

## Documentation

For the complete guide covering grammar, registers, developer CLI utilities, git integration, TOML/JSONL/crypto, `Ottofile` tasks, interactive REPL shortcuts, SQLite streams, FFI, and production recipes, see [**`TUTORIAL.md`**](TUTORIAL.md).
