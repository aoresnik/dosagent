/* Minimal HTTP/1.1 client with keep-alive and chunked decoding. */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include "buf.h"

typedef struct {
    const char *host;
    int port;
    int tls;
} http_target;

/* Called with successive pieces of a 2xx response body. */
typedef void (*http_body_cb)(void *ctx, const char *data, size_t n);

#define HTTP_ERR_NET (-1)
#define HTTP_ERR_ABORT (-2)
#define HTTP_ERR_FATAL (-3) /* connection failed; retrying won't help */

/* POSTs body to path. headers holds extra header lines, each ending in
 * "\r\n". 2xx bodies go to cb; other bodies are collected in errbody.
 * Returns the HTTP status or a negative HTTP_ERR_* code. retry_after is
 * set from the Retry-After header (seconds) when present, else 0. */
int http_post(const http_target *t, const char *path, const char *headers,
              const char *body, size_t len, http_body_cb cb, void *ctx,
              buf_t *errbody, int *retry_after);

/* Drop the kept-alive connection (e.g. before running a long command). */
void http_disconnect(void);

#endif
