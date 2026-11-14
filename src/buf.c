#include "buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void die(const char *fmt, ...)
{
    va_list ap;
    fputs("\nFATAL: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die("out of memory (%lu bytes)", (unsigned long)n);
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        die("out of memory (%lu bytes)", (unsigned long)n);
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

void buf_init(buf_t *b)
{
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(buf_t *b)
{
    free(b->p);
    buf_init(b);
}

void buf_clear(buf_t *b)
{
    b->len = 0;
    if (b->p)
        b->p[0] = 0;
}

void buf_reserve(buf_t *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    if (need <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need)
        cap *= 2;
    b->p = xrealloc(b->p, cap);
    b->cap = cap;
}

void buf_append(buf_t *b, const void *data, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = 0;
}

void buf_puts(buf_t *b, const char *s)
{
    buf_append(b, s, strlen(s));
}

void buf_putc(buf_t *b, char c)
{
    buf_append(b, &c, 1);
}

void buf_printf(buf_t *b, const char *fmt, ...)
{
    va_list ap;
    char tmp[256];
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof tmp) {
        buf_append(b, tmp, (size_t)n);
        return;
    }
    buf_reserve(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

char *buf_take(buf_t *b)
{
    char *p = b->p;
    if (!p)
        p = xstrdup("");
    buf_init(b);
    return p;
}
