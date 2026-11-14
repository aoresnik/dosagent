/* Minimal JSON: a malloc-based DOM parser and an escaping writer.
 * Strings are stored unescaped as UTF-8 with an explicit length (they may
 * contain NUL). Numbers are kept as their source text. */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include "buf.h"

typedef enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jval {
    jtype t;
    char *s;            /* J_STR / J_NUM: NUL-terminated text */
    size_t n;           /* length of s */
    char *key;          /* member name when inside an object */
    struct jval *child; /* first element / member */
    struct jval *next;  /* next sibling */
} jval;

/* Returns NULL on error and writes a message to err. */
jval *json_parse(const char *text, size_t len, char *err, size_t errlen);
void json_free(jval *v);

jval *json_get(const jval *obj, const char *key);
/* String member or NULL if absent / not a string. */
const char *json_get_str(const jval *obj, const char *key);
long json_get_long(const jval *obj, const char *key, long dflt);
int json_get_bool(const jval *obj, const char *key, int dflt);

/* Writer. json_put_str emits a quoted, escaped string. */
void json_put_str(buf_t *b, const char *s, size_t n);
void json_put_cstr(buf_t *b, const char *s);
void json_write(buf_t *b, const jval *v);

#endif
