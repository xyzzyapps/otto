#ifndef OTTO_JOURNAL_H
#define OTTO_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include "value.h"

// Initialize journal directory (~/.otto/journal or ./.otto/journal)
void journal_init(void);

// Begin a transaction (generates timestamp & txid)
void journal_begin_tx(void);

// Backup a file before mutation into current transaction
bool journal_backup_file(const char *filepath);

// Commit transaction
void journal_commit_tx(void);

// Rollback latest transaction
bool journal_undo_last(void);

// Simulate helper: prints diff preview between old_content and new_content
void journal_simulate_diff(const char *filepath, const char *old_content, size_t old_len, const char *new_content, size_t new_len);

#endif // OTTO_JOURNAL_H
