/* Growable byte buffer. Always kept NUL-terminated for convenience. */
#ifndef BUF_H
#define BUF_H

#include <stddef.h>

typedef struct {
    char *p;
    size_t len;
    size_t cap;
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
void buf_clear(buf_t *b);
void buf_reserve(buf_t *b, size_t extra);
void buf_append(buf_t *b, const void *data, size_t n);
void buf_puts(buf_t *b, const char *s);
void buf_putc(buf_t *b, char c);
void buf_printf(buf_t *b, const char *fmt, ...);
/* Detach the storage; caller frees. Buffer is left empty. */
char *buf_take(buf_t *b);

/* Allocation helpers that abort on failure. */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void die(const char *fmt, ...);

#endif
