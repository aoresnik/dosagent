/* Things that differ between the DOS target and the host test build. */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>
#include "buf.h"

#ifdef __DJGPP__
#define PLAT_DOS 1
#define PLAT_NULL_DEVICE "NUL"
#else
#define PLAT_DOS 0
#define PLAT_NULL_DEVICE "/dev/null"
#endif

void plat_init(void);
/* Short description of the OS for the system prompt. */
void plat_describe(buf_t *out);
/* Nonzero if the user asked to abort (Esc on DOS, Ctrl-C everywhere). */
int plat_poll_abort(void);
void plat_clear_abort(void);
/* Feed timer/clock jitter into the TLS entropy pool. */
void plat_entropy(void);
/* Read a line of input (no trailing newline). -1 on EOF. On DOS the line
 * can be edited (cursor keys, Home/End, Del, Ins, Up/Down for history) and
 * keystroke timing is mixed into the entropy pool. */
int plat_read_line(char *out, size_t n);
/* Remember a line for Up/Down in plat_read_line (DOS only). */
void plat_add_history(const char *line);
/* Wait for a single key press and return it. */
int plat_getkey(void);
/* Append the text-mode screen contents (CP437 bytes, one line per row,
 * trailing blanks trimmed). Returns 0 if unsupported. */
int plat_read_screen(buf_t *out);
void plat_sleep_ms(unsigned ms);
/* Current directory as the user would write it (C:\WORK on DOS). */
char *plat_getcwd(char *out, size_t n);

#endif
