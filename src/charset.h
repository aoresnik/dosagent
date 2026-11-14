/* Conversion between the local 8-bit code page and the UTF-8 that the API
 * requires. On DOS the local charset is CP437; on the host it is UTF-8
 * (only sanitised, so invalid bytes cannot break the JSON). */
#ifndef CHARSET_H
#define CHARSET_H

#include <stddef.h>
#include "buf.h"

/* 1 = local text is CP437, 0 = local text is UTF-8. */
extern int charset_cp437;

/* Local bytes -> valid UTF-8. */
void local_to_utf8(buf_t *out, const char *in, size_t n);
/* UTF-8 -> local bytes (unmappable characters become '?'). */
void utf8_to_local(buf_t *out, const char *in, size_t n);
/* Print UTF-8 text on the console in the local charset. */
void print_utf8(const char *s, size_t n);

#endif
