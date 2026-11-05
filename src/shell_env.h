#ifndef OTTO_SHELL_ENV_H
#define OTTO_SHELL_ENV_H

#include "value.h"
#include "arena.h"

struct VM;

// Directory navigation & ring buffer
void shell_cd(const char *path);
void shell_back(void);
Value shell_get_dirs(Arena *arena);
Value shell_jump(const char *keyword, Arena *arena);

// Job control
typedef struct ShellJob {
    int id;
    int pid;
    char command[256];
    char status[32]; // "Running", "Stopped", "Done"
    void *process_handle;
} ShellJob;

Value shell_list_jobs(Arena *arena);
bool shell_resume_job(int job_id, bool background);
int shell_add_job(int pid, const char *cmd, void *handle);
bool shell_kill_job(int job_or_pid);

// Startup, ~/.ottorc & automatic .env loader
void shell_load_rc(struct VM *vm);
void shell_load_dotenv(const char *dir_path);

// Persistent command history (~/.otto/history)
void shell_history_load(void);
void shell_history_add(const char *cmd);
size_t shell_history_count(void);
const char *shell_history_get(size_t idx);
Value shell_history_list(Arena *arena);

// Live abbreviations (Fish-style `abbr`)
void shell_abbr_set(const char *short_name, const char *expansion);
const char *shell_abbr_get(const char *short_name);
Value shell_abbr_list(Arena *arena);
bool shell_abbr_expand_line(const char *line, char *out, size_t max_len);

// Deduplicated $PATH modification (`prepend` / `append`)
Value shell_path_modify(const char *dir, bool prepend, Arena *arena);

// History substitution helpers
Value shell_repeat_history(Value prev_cmd, const char *with_prefix, const char *replace_old, const char *replace_new, Arena *arena);

#endif // OTTO_SHELL_ENV_H
