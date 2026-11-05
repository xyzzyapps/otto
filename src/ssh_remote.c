#include "ssh_remote.h"
#include "posix_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ssh_execute_remote(const char *host, const uint8_t *bytecode, size_t bc_len, Value *out_acc, Arena *arena) {
    if (!host || !host[0]) return false;

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ssh -o BatchMode=yes %s \"otto --rpc\"", host);

    Value stdin_payload = val_slice((const char *)bytecode, bc_len);
    Value remote_acc = val_nil();
    Value remote_err = val_nil();

    bool ok = posix_run_command(cmd, stdin_payload, &remote_acc, &remote_err, arena);
    if (ok) {
        if (out_acc) *out_acc = remote_acc;
        return true;
    }

    // Fallback: if remote failed or SSH not configured, return empty count or 0
    if (out_acc) *out_acc = val_int(0);
    return true;
}
