#ifndef OTTO_TUI_H
#define OTTO_TUI_H

#include "value.h"
#include "arena.h"

// Terminal raw mode helpers
void tui_enable_raw_mode(void);
void tui_disable_raw_mode(void);
bool tui_is_interactive(void);

// TUI Verbs
Value tui_pick(Value items, Arena *arena);
void tui_table(Value items, FILE *out);
void tui_chart(Value items, const char *kind, FILE *out);

#endif // OTTO_TUI_H
