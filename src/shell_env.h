#ifndef OTTO_SHELL_ENV_H
#define OTTO_SHELL_ENV_H

#include "value.h"
#include "arena.h"

// Directory navigation & ring buffer
void shell_cd(const char *path, Arena *arena);
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

// Startup & ~/.ottorc execution
void shell_load_rc(void);

// History substitution helpers
Value shell_repeat_history(Value prev_cmd, const char *with_prefix, const char *replace_old, const char *replace_new, Arena *arena);

#endif // OTTO_SHELL_ENV_H
