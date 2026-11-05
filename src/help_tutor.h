#ifndef OTTO_HELP_TUTOR_H
#define OTTO_HELP_TUTOR_H

#include <stdbool.h>
#include <stddef.h>
#include "value.h"
#include "arena.h"

struct VM;

// C1: Built-in Cheat Sheets (`help` and `examples` verbs)
Value help_show(const char *topic, bool examples_only, Arena *arena);

// C2: Damerau-Levenshtein keyword suggestions & Bash-to-Otto Rosetta hints
const char *help_suggest_keyword(const char *word, size_t len);
const char *help_rosetta_tip(const char *raw_cmd);

// C3: Visual caret `^~~~` error diagnostics & Interactive 5-Lesson Tutor
void help_print_diagnostic(const char *source, size_t len, int err_line, int err_col, const char *err_msg);
void tutor_run(struct VM *vm, bool interactive);

// A6: Native `Ottofile` / `tasks.otto` Task Runner (`otto --tasks`, `otto task <name>`)
const char *ottofile_find_default(void);
bool ottofile_list_tasks(const char *filepath);
bool ottofile_run_task(struct VM *vm, const char *filepath, const char *task_name, int argc, char **argv);

#endif // OTTO_HELP_TUTOR_H
