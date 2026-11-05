#ifndef OTTO_HTTP_CLIENT_H
#define OTTO_HTTP_CLIENT_H

#include "value.h"
#include "arena.h"
#include <stdbool.h>

bool http_is_url(const char *target);
Value http_fetch(const char *url, Arena *arena, Value *out_err);
Value http_post(const char *url, Value body, Arena *arena, Value *out_err);

#endif // OTTO_HTTP_CLIENT_H
