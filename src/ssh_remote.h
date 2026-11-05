#ifndef OTTO_SSH_REMOTE_H
#define OTTO_SSH_REMOTE_H

#include "value.h"
#include "arena.h"

// Execute command at remote host via SSH
bool ssh_execute_remote(const char *host, const char *remote_cmd, Value stdin_val, Value *out_acc, Value *out_err, Arena *arena);

#endif // OTTO_SSH_REMOTE_H
