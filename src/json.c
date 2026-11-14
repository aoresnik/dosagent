#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    const char *end;
    char *err;
    size_t errlen;
    int depth;
} parser;

static void set_err(parser *ps, const char *msg)
{
    if (ps->err && ps->err[0] == 0)
        snprintf(ps->err, ps->errlen, "%s at offset %ld", msg,
                 (long)(ps->end - ps->p));
}

static void skip_ws(parser *ps)
{
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static jval *new_val(jtype t)
{
    jval *v = xmalloc(sizeof *v);
    memset(v, 0, sizeof *v);
    v->t = t;
    return v;
}

static void put_utf8(buf_t *b, unsigned long cp)
{
    char c[4];
    if (cp < 0x80) {
        c[0] = (char)cp;
        buf_append(b, c, 1);
    } else if (cp < 0x800) {
        c[0] = (char)(0xC0 | (cp >> 6));
        c[1] = (char)(0x80 | (cp & 0x3F));
        buf_append(b, c, 2);
    } else if (cp < 0x10000) {
        c[0] = (char)(0xE0 | (cp >> 12));
        c[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        c[2] = (char)(0x80 | (cp & 0x3F));
        buf_append(b, c, 3);
    } else {
        c[0] = (char)(0xF0 | (cp >> 18));
        c[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        c[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        c[3] = (char)(0x80 | (cp & 0x3F));
        buf_append(b, c, 4);
    }
}

static int hex4(const char *p, unsigned long *out)
{
    unsigned long v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned long)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/* Parses a string starting at the opening quote. */
static char *parse_string(parser *ps, size_t *outlen)
{
    buf_t b;
    buf_init(&b);
    ps->p++; /* opening quote */
    for (;;) {
        const char *start = ps->p;
        while (ps->p < ps->end && *ps->p != '"' && *ps->p != '\\')
            ps->p++;
        buf_append(&b, start, (size_t)(ps->p - start));
        if (ps->p >= ps->end) {
            set_err(ps, "unterminated string");
            buf_free(&b);
            return NULL;
        }
        if (*ps->p == '"') {
            ps->p++;
            break;
        }
        /* backslash escape */
        ps->p++;
        if (ps->p >= ps->end)
            break;
        switch (*ps->p) {
        case '"': buf_putc(&b, '"'); break;
        case '\\': buf_putc(&b, '\\'); break;
        case '/': buf_putc(&b, '/'); break;
        case 'b': buf_putc(&b, '\b'); break;
        case 'f': buf_putc(&b, '\f'); break;
        case 'n': buf_putc(&b, '\n'); break;
        case 'r': buf_putc(&b, '\r'); break;
        case 't': buf_putc(&b, '\t'); break;
        case 'u': {
            unsigned long cp, lo;
            if (ps->end - ps->p < 5 || !hex4(ps->p + 1, &cp)) {
                set_err(ps, "bad \\u escape");
                buf_free(&b);
                return NULL;
            }
            ps->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && ps->end - ps->p >= 7 &&
                ps->p[1] == '\\' && ps->p[2] == 'u' && hex4(ps->p + 3, &lo) &&
                lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ps->p += 6;
            } else if (cp >= 0xD800 && cp < 0xE000) {
                cp = 0xFFFD; /* lone surrogate */
            }
            put_utf8(&b, cp);
            break;
        }
        default:
            set_err(ps, "bad escape");
            buf_free(&b);
            return NULL;
        }
        ps->p++;
    }
    *outlen = b.len;
    return buf_take(&b);
}

static jval *parse_value(parser *ps);

static jval *parse_container(parser *ps, int is_obj)
{
    jval *v = new_val(is_obj ? J_OBJ : J_ARR);
    jval **tail = &v->child;
    char close = is_obj ? '}' : ']';

    if (++ps->depth > 64) {
        set_err(ps, "nesting too deep");
        json_free(v);
        return NULL;
    }
    ps->p++;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == close) {
        ps->p++;
        ps->depth--;
        return v;
    }
    for (;;) {
        char *key = NULL;
        jval *item;
        skip_ws(ps);
        if (is_obj) {
            size_t klen;
            if (ps->p >= ps->end || *ps->p != '"') {
                set_err(ps, "expected key");
                goto fail;
            }
            key = parse_string(ps, &klen);
            if (!key)
                goto fail;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                free(key);
                set_err(ps, "expected ':'");
                goto fail;
            }
            ps->p++;
        }
        item = parse_value(ps);
        if (!item) {
            free(key);
            goto fail;
        }
        item->key = key;
        *tail = item;
        tail = &item->next;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == close) {
            ps->p++;
            break;
        }
        set_err(ps, "expected ',' or close bracket");
        goto fail;
    }
    ps->depth--;
    return v;
fail:
    json_free(v);
    return NULL;
}

static int match_lit(parser *ps, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(ps->end - ps->p) >= n && memcmp(ps->p, lit, n) == 0) {
        ps->p += n;
        return 1;
    }
    return 0;
}

static jval *parse_value(parser *ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end) {
        set_err(ps, "unexpected end");
        return NULL;
    }
    switch (*ps->p) {
    case '{':
        return parse_container(ps, 1);
    case '[':
        return parse_container(ps, 0);
    case '"': {
        size_t n;
        char *s = parse_string(ps, &n);
        jval *v;
        if (!s)
            return NULL;
        v = new_val(J_STR);
        v->s = s;
        v->n = n;
        return v;
    }
    case 't':
        if (match_lit(ps, "true")) return new_val(J_TRUE);
        break;
    case 'f':
        if (match_lit(ps, "false")) return new_val(J_FALSE);
        break;
    case 'n':
        if (match_lit(ps, "null")) return new_val(J_NULL);
        break;
    default:
        if (*ps->p == '-' || (*ps->p >= '0' && *ps->p <= '9')) {
            const char *start = ps->p;
            jval *v;
            while (ps->p < ps->end && strchr("+-0123456789.eE", *ps->p))
                ps->p++;
            v = new_val(J_NUM);
            v->n = (size_t)(ps->p - start);
            v->s = xstrndup(start, v->n);
            return v;
        }
    }
    set_err(ps, "unexpected character");
    return NULL;
}

jval *json_parse(const char *text, size_t len, char *err, size_t errlen)
{
    parser ps;
    jval *v;

    ps.p = text;
    ps.end = text + len;
    ps.err = err;
    ps.errlen = errlen;
    ps.depth = 0;
    if (err && errlen)
        err[0] = 0;
    v = parse_value(&ps);
    if (!v)
        return NULL;
    skip_ws(&ps);
    if (ps.p != ps.end) {
        set_err(&ps, "trailing garbage");
        json_free(v);
        return NULL;
    }
    return v;
}

void json_free(jval *v)
{
    while (v) {
        jval *next = v->next;
        json_free(v->child);
        free(v->s);
        free(v->key);
        free(v);
        v = next;
    }
}

jval *json_get(const jval *obj, const char *key)
{
    jval *c;
    if (!obj || obj->t != J_OBJ)
        return NULL;
    for (c = obj->child; c; c = c->next)
        if (c->key && strcmp(c->key, key) == 0)
            return c;
    return NULL;
}

const char *json_get_str(const jval *obj, const char *key)
{
    jval *v = json_get(obj, key);
    return (v && v->t == J_STR) ? v->s : NULL;
}

long json_get_long(const jval *obj, const char *key, long dflt)
{
    jval *v = json_get(obj, key);
    return (v && v->t == J_NUM) ? strtol(v->s, NULL, 10) : dflt;
}

int json_get_bool(const jval *obj, const char *key, int dflt)
{
    jval *v = json_get(obj, key);
    if (v && v->t == J_TRUE) return 1;
    if (v && v->t == J_FALSE) return 0;
    return dflt;
}

void json_put_str(buf_t *b, const char *s, size_t n)
{
    static const char hexd[] = "0123456789abcdef";
    size_t i, run = 0;

    buf_reserve(b, n + 2);
    buf_putc(b, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char ubuf[7];
        if (c == '"') esc = "\\\"";
        else if (c == '\\') esc = "\\\\";
        else if (c == '\n') esc = "\\n";
        else if (c == '\r') esc = "\\r";
        else if (c == '\t') esc = "\\t";
        else if (c < 0x20 || c == 0x7F) {
            ubuf[0] = '\\'; ubuf[1] = 'u'; ubuf[2] = '0'; ubuf[3] = '0';
            ubuf[4] = hexd[c >> 4]; ubuf[5] = hexd[c & 15]; ubuf[6] = 0;
            esc = ubuf;
        }
        if (esc) {
            buf_append(b, s + i - run, run);
            run = 0;
            buf_puts(b, esc);
        } else {
            run++;
        }
    }
    buf_append(b, s + n - run, run);
    buf_putc(b, '"');
}

void json_put_cstr(buf_t *b, const char *s)
{
    json_put_str(b, s, strlen(s));
}

void json_write(buf_t *b, const jval *v)
{
    const jval *c;
    switch (v->t) {
    case J_NULL: buf_puts(b, "null"); break;
    case J_FALSE: buf_puts(b, "false"); break;
    case J_TRUE: buf_puts(b, "true"); break;
    case J_NUM: buf_append(b, v->s, v->n); break;
    case J_STR: json_put_str(b, v->s, v->n); break;
    case J_ARR:
    case J_OBJ:
        buf_putc(b, v->t == J_ARR ? '[' : '{');
        for (c = v->child; c; c = c->next) {
            if (c != v->child)
                buf_putc(b, ',');
            if (v->t == J_OBJ) {
                json_put_cstr(b, c->key ? c->key : "");
                buf_putc(b, ':');
            }
            json_write(b, c);
        }
        buf_putc(b, v->t == J_ARR ? ']' : '}');
        break;
    }
}
