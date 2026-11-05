#include "ssh_remote.h"
#include "posix_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ssh_execute_remote(const char *host, const char *remote_cmd, Value stdin_val, Value *out_acc, Value *out_err, Arena *arena) {
    if (!host || !host[0] || !remote_cmd || !remote_cmd[0]) return false;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "ssh -o BatchMode=yes %s \"%s\"", host, remote_cmd);

    return posix_run_command(cmd, stdin_val, out_acc, out_err, arena);
}
