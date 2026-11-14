#include "http.h"
#include "net.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static conn_t *keep;
static char keep_host[256];
static int keep_port, keep_tls;

void http_disconnect(void)
{
    conn_close(keep);
    keep = NULL;
}

/* Buffered reader over a connection. */
typedef struct {
    conn_t *c;
    char buf[4096];
    size_t pos, len;
    int got_any; /* any response byte received */
} reader;

static int rd_fill(reader *r)
{
    int n = conn_read(r->c, r->buf, sizeof r->buf);
    if (n <= 0)
        return n;
    r->pos = 0;
    r->len = (size_t)n;
    r->got_any = 1;
    return n;
}

/* Reads one line (without CRLF) into line. Returns 0 on success. */
static int rd_line(reader *r, buf_t *line)
{
    buf_clear(line);
    for (;;) {
        while (r->pos < r->len) {
            char ch = r->buf[r->pos++];
            if (ch == '\n') {
                if (line->len > 0 && line->p[line->len - 1] == '\r')
                    line->p[--line->len] = 0;
                return 0;
            }
            buf_putc(line, ch);
            if (line->len > 16384)
                return -1;
        }
        if (rd_fill(r) <= 0)
            return -1;
    }
}

/* Deliver exactly n body bytes (or until EOF when n == (size_t)-1). */
static int rd_body(reader *r, size_t n, int ok, http_body_cb cb, void *ctx,
                   buf_t *errbody)
{
    while (n > 0) {
        size_t take;
        if (r->pos >= r->len) {
            int rc = rd_fill(r);
            if (rc == 0 && n == (size_t)-1)
                return 0;
            if (rc <= 0)
                return -1;
        }
        take = r->len - r->pos;
        if (n != (size_t)-1 && take > n)
            take = n;
        if (ok)
            cb(ctx, r->buf + r->pos, take);
        else if (errbody->len < 65536)
            buf_append(errbody, r->buf + r->pos, take);
        r->pos += take;
        if (n != (size_t)-1)
            n -= take;
    }
    return 0;
}

static int header_is(const char *line, const char *name, const char **value)
{
    size_t n = strlen(name);
    size_t i;
    for (i = 0; i < n; i++)
        if (tolower((unsigned char)line[i]) != name[i])
            return 0;
    if (line[n] != ':')
        return 0;
    line += n + 1;
    while (*line == ' ' || *line == '\t')
        line++;
    *value = line;
    return 1;
}

static int ci_contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i;
        for (i = 0; i < n; i++)
            if (tolower((unsigned char)hay[i]) != needle[i])
                break;
        if (i == n)
            return 1;
    }
    return 0;
}

static int do_request(conn_t *c, const http_target *t, const char *path,
                      const char *headers, const char *body, size_t len,
                      http_body_cb cb, void *ctx, buf_t *errbody,
                      int *retry_after, int *reusable, int *got_any)
{
    buf_t req, line;
    reader r;
    int status = 0, ok, chunked = 0, close_after = 0;
    long clen = -1;
    int rc = HTTP_ERR_NET;

    buf_init(&req);
    buf_init(&line);
    memset(&r, 0, sizeof r);
    r.c = c;
    *reusable = 0;
    *got_any = 0;

    buf_printf(&req, "POST %s HTTP/1.1\r\nHost: %s\r\n", path, t->host);
    buf_puts(&req, headers);
    buf_printf(&req, "Content-Length: %lu\r\nConnection: keep-alive\r\n\r\n",
               (unsigned long)len);
    if (conn_write(c, req.p, req.len) < 0 || conn_write(c, body, len) < 0 ||
        conn_flush(c) < 0)
        goto out;

    /* Status line; skip any 1xx interim responses. */
    for (;;) {
        if (rd_line(&r, &line) < 0)
            goto out;
        if (strncmp(line.p, "HTTP/1.", 7) != 0 || line.len < 12)
            goto out;
        status = atoi(line.p + 9);
        if (status >= 200)
            break;
        do {
            if (rd_line(&r, &line) < 0)
                goto out;
        } while (line.len > 0);
    }
    if (strncmp(line.p, "HTTP/1.0", 8) == 0)
        close_after = 1;

    for (;;) {
        const char *v;
        if (rd_line(&r, &line) < 0)
            goto out;
        if (line.len == 0)
            break;
        if (header_is(line.p, "content-length", &v))
            clen = atol(v);
        else if (header_is(line.p, "transfer-encoding", &v))
            chunked = ci_contains(v, "chunked");
        else if (header_is(line.p, "connection", &v))
            close_after = ci_contains(v, "close");
        else if (header_is(line.p, "retry-after", &v) && retry_after)
            *retry_after = atoi(v);
    }

    ok = status >= 200 && status < 300;
    if (chunked) {
        for (;;) {
            unsigned long n;
            if (rd_line(&r, &line) < 0)
                goto out;
            n = strtoul(line.p, NULL, 16);
            if (n == 0) {
                /* Trailers until the empty line. */
                do {
                    if (rd_line(&r, &line) < 0)
                        goto out;
                } while (line.len > 0);
                break;
            }
            if (rd_body(&r, n, ok, cb, ctx, errbody) < 0)
                goto out;
            if (rd_line(&r, &line) < 0) /* CRLF after chunk */
                goto out;
        }
    } else if (clen >= 0) {
        if (rd_body(&r, (size_t)clen, ok, cb, ctx, errbody) < 0)
            goto out;
    } else {
        rd_body(&r, (size_t)-1, ok, cb, ctx, errbody);
        close_after = 1;
    }
    *reusable = !close_after;
    rc = status;
out:
    *got_any = r.got_any;
    buf_free(&req);
    buf_free(&line);
    return rc;
}

int http_post(const http_target *t, const char *path, const char *headers,
              const char *body, size_t len, http_body_cb cb, void *ctx,
              buf_t *errbody, int *retry_after)
{
    int attempt;

    if (retry_after)
        *retry_after = 0;
    if (keep && (strcmp(keep_host, t->host) != 0 || keep_port != t->port ||
                 keep_tls != t->tls))
        http_disconnect();

    for (attempt = 0; attempt < 2; attempt++) {
        int reused = keep != NULL;
        int reusable, got_any, rc;

        if (!keep) {
            keep = conn_open(t->host, t->port, t->tls);
            if (!keep)
                return net_abort ? HTTP_ERR_ABORT
                     : net_fatal ? HTTP_ERR_FATAL : HTTP_ERR_NET;
            snprintf(keep_host, sizeof keep_host, "%s", t->host);
            keep_port = t->port;
            keep_tls = t->tls;
        }
        buf_clear(errbody);
        rc = do_request(keep, t, path, headers, body, len, cb, ctx, errbody,
                        retry_after, &reusable, &got_any);
        if (rc < 0 || !reusable)
            http_disconnect();
        if (net_abort)
            return HTTP_ERR_ABORT;
        /* A kept-alive connection the server already closed fails before
         * any response byte arrives: reconnect once and resend. */
        if (rc < 0 && reused && !got_any && !net_fatal)
            continue;
        return rc == HTTP_ERR_NET && net_fatal ? HTTP_ERR_FATAL : rc;
    }
    return HTTP_ERR_NET;
}
