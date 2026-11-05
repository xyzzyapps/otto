#ifndef OTTO_SSH_REMOTE_H
#define OTTO_SSH_REMOTE_H

#include "value.h"
#include "arena.h"

// Execute clause or command at remote host via SSH RPC
bool ssh_execute_remote(const char *host, const uint8_t *bytecode, size_t bc_len, Value *out_acc, Arena *arena);

#endif // OTTO_SSH_REMOTE_H
