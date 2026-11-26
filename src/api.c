#include "api.h"
#include "charset.h"
#include "net.h"
#include "platform.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ conversation */

void (*conv_log)(const char *role, const char *block_json);

void conv_init(conv_t *c)
{
    memset(c, 0, sizeof *c);
}

void conv_free(conv_t *c)
{
    size_t i;
    for (i = 0; i < c->n; i++)
        buf_free(&c->m[i].content);
    free(c->m);
    conv_init(c);
}

void conv_append(conv_t *c, const char *role, const char *block_json)
{
    msg_t *m;
    if (c->n > 0 && strcmp(c->m[c->n - 1].role, role) == 0) {
        m = &c->m[c->n - 1];
        buf_putc(&m->content, ',');
    } else {
        if (c->n == c->cap) {
            c->cap = c->cap ? c->cap * 2 : 16;
            c->m = xrealloc(c->m, c->cap * sizeof *c->m);
        }
        m = &c->m[c->n++];
        snprintf(m->role, sizeof m->role, "%s", role);
        buf_init(&m->content);
    }
    buf_puts(&m->content, block_json);
    if (conv_log)
        conv_log(role, block_json);
}

void conv_add_user_text(conv_t *c, const char *utf8)
{
    buf_t b;
    buf_init(&b);
    buf_puts(&b, "{\"type\":\"text\",\"text\":");
    json_put_cstr(&b, utf8);
    buf_putc(&b, '}');
    conv_append(c, "user", b.p);
    buf_free(&b);
}

/* Rewrites one message if it holds unsigned thinking with text in it. */
static void drop_thinking_msg(msg_t *m)
{
    buf_t arr;
    jval *root, *b;
    char err[96];
    int found = 0;

    buf_init(&arr);
    buf_putc(&arr, '[');
    buf_append(&arr, m->content.p, m->content.len);
    buf_putc(&arr, ']');
    root = json_parse(arr.p, arr.len, err, sizeof err);
    buf_free(&arr);
    if (!root)
        return;
    for (b = root->child; b; b = b->next) {
        const char *type = json_get_str(b, "type");
        const char *sig = json_get_str(b, "signature");
        jval *t = json_get(b, "thinking");
        if (type && strcmp(type, "thinking") == 0 && (!sig || !*sig) &&
            t && t->t == J_STR && t->n > 0) {
            t->s[0] = 0;
            t->n = 0;
            found = 1;
        }
    }
    if (found) {
        buf_free(&m->content);
        buf_init(&m->content);
        for (b = root->child; b; b = b->next) {
            if (b != root->child)
                buf_putc(&m->content, ',');
            json_write(&m->content, b);
        }
    }
    json_free(root);
}

void conv_drop_thinking(conv_t *c)
{
    for (; c->thin_from < c->n; c->thin_from++)
        if (strcmp(c->m[c->thin_from].role, "assistant") == 0)
            drop_thinking_msg(&c->m[c->thin_from]);
}

/* ------------------------------------------------------------ result */

void api_result_free(api_result *r)
{
    size_t i;
    for (i = 0; i < r->n; i++) {
        free(r->b[i].type);
        free(r->b[i].json);
        free(r->b[i].id);
        free(r->b[i].name);
        json_free(r->b[i].input);
    }
    free(r->b);
    memset(r, 0, sizeof *r);
}

static void result_add(api_result *r, block_t *b)
{
    r->b = xrealloc(r->b, (r->n + 1) * sizeof *r->b);
    r->b[r->n++] = *b;
}

/* ------------------------------------------------------------ streaming */

/* A content block being assembled from stream deltas. */
typedef struct {
    int active;
    jval *start;       /* content_block from content_block_start */
    buf_t text, thinking, signature, json;
    int has_text, has_thinking, has_signature, has_json;
} pending_t;

typedef struct {
    const api_cfg *cfg;
    buf_t line;        /* current SSE line */
    buf_t data;        /* accumulated data: field of current event */
    buf_t whole;       /* the whole body, when not streaming */
    pending_t *pend;
    size_t npend;
    api_result *res;
    int got_stop;      /* saw message_stop */
    int stream_error;  /* saw an error event */
    char error_type[48];
    buf_t error_msg;
    int at_line_start; /* console cursor at column 0 */
    int in_thinking_print;
} stream_t;

static void con_print(stream_t *st, const char *s, size_t n)
{
    if (n == 0 || st->cfg->quiet)
        return;
    print_utf8(s, n);
    st->at_line_start = s[n - 1] == '\n';
}

static void con_newline(stream_t *st)
{
    if (!st->at_line_start && !st->cfg->quiet) {
        putchar('\n');
        st->at_line_start = 1;
    }
}

static pending_t *pend_at(stream_t *st, long idx)
{
    if (idx < 0 || idx > 1000)
        return NULL;
    if ((size_t)idx >= st->npend) {
        size_t n = (size_t)idx + 1;
        st->pend = xrealloc(st->pend, n * sizeof *st->pend);
        memset(st->pend + st->npend, 0, (n - st->npend) * sizeof *st->pend);
        st->npend = n;
    }
    return &st->pend[idx];
}

static void pend_reset(pending_t *p)
{
    json_free(p->start);
    buf_free(&p->text);
    buf_free(&p->thinking);
    buf_free(&p->signature);
    buf_free(&p->json);
    memset(p, 0, sizeof *p);
}

/* Serialize the finished block: the start object with streamed fields
 * filled in. Unknown fields are kept as received. */
static void finish_block(stream_t *st, pending_t *p)
{
    buf_t out;
    jval *c;
    block_t b;
    const char *type = json_get_str(p->start, "type");
    int wrote_text = 0, wrote_thinking = 0, wrote_sig = 0;
    jval *input = NULL;
    int input_bad = 0;

    memset(&b, 0, sizeof b);
    b.type = xstrdup(type ? type : "unknown");

    if (p->has_json) {
        char err[96];
        input = json_parse(p->json.p, p->json.len, err, sizeof err);
        if (!input || input->t != J_OBJ) {
            input_bad = 1;
            json_free(input);
            input = NULL;
        }
    }

    buf_init(&out);
    buf_putc(&out, '{');
    for (c = p->start->child; c; c = c->next) {
        if (c != p->start->child)
            buf_putc(&out, ',');
        json_put_cstr(&out, c->key);
        buf_putc(&out, ':');
        if (p->has_text && strcmp(c->key, "text") == 0) {
            json_put_str(&out, p->text.p ? p->text.p : "", p->text.len);
            wrote_text = 1;
        } else if (p->has_thinking && strcmp(c->key, "thinking") == 0) {
            json_put_str(&out, p->thinking.p ? p->thinking.p : "",
                         p->thinking.len);
            wrote_thinking = 1;
        } else if (p->has_signature && strcmp(c->key, "signature") == 0) {
            json_put_str(&out, p->signature.p, p->signature.len);
            wrote_sig = 1;
        } else if (p->has_json && !input_bad && strcmp(c->key, "input") == 0) {
            json_write(&out, input);
        } else {
            json_write(&out, c);
        }
    }
    if (p->has_text && !wrote_text) {
        buf_puts(&out, ",\"text\":");
        json_put_str(&out, p->text.p ? p->text.p : "", p->text.len);
    }
    if (p->has_thinking && !wrote_thinking) {
        buf_puts(&out, ",\"thinking\":");
        json_put_str(&out, p->thinking.p ? p->thinking.p : "", p->thinking.len);
    }
    if (p->has_signature && !wrote_sig) {
        buf_puts(&out, ",\"signature\":");
        json_put_str(&out, p->signature.p, p->signature.len);
    }
    buf_putc(&out, '}');
    b.json = buf_take(&out);

    if (strcmp(b.type, "tool_use") == 0) {
        const char *id = json_get_str(p->start, "id");
        const char *name = json_get_str(p->start, "name");
        b.id = xstrdup(id ? id : "");
        b.name = xstrdup(name ? name : "");
        if (input) {
            b.input = input;
            input = NULL;
        } else if (!input_bad) {
            jval *orig = json_get(p->start, "input");
            buf_t tmp;
            char err[16];
            buf_init(&tmp);
            if (orig)
                json_write(&tmp, orig);
            else
                buf_puts(&tmp, "{}");
            b.input = json_parse(tmp.p, tmp.len, err, sizeof err);
            buf_free(&tmp);
        }
    }
    json_free(input);
    result_add(st->res, &b);
}

/* Console output at the start of a content block. */
static void block_started(stream_t *st, const char *btype, jval *cb)
{
    if (btype && strcmp(btype, "thinking") == 0) {
        con_newline(st);
        con_print(st, st->cfg->show_thinking ? "[thinking] " :
                  "[thinking...]\n", st->cfg->show_thinking ? 11 : 14);
        st->in_thinking_print = st->cfg->show_thinking;
    } else if (btype && strcmp(btype, "redacted_thinking") == 0) {
        con_newline(st);
        con_print(st, "[thinking...]\n", 14);
    } else if (btype && strcmp(btype, "fallback") == 0) {
        const char *from = json_get_str(json_get(cb, "from"), "model");
        const char *to = json_get_str(json_get(cb, "to"), "model");
        con_newline(st);
        if (!st->cfg->quiet)
            printf("[%s declined; continuing on %s]\n",
                   from ? from : "model", to ? to : "fallback model");
    }
}

static void set_error(stream_t *st, const char *type, const char *msg)
{
    st->stream_error = 1;
    snprintf(st->error_type, sizeof st->error_type, "%s", type);
    buf_clear(&st->error_msg);
    buf_puts(&st->error_msg, msg);
}

static void on_event(stream_t *st, const char *data, size_t n)
{
    char err[96];
    jval *ev = json_parse(data, n, err, sizeof err);
    const char *type;

    if (!ev) {
        con_newline(st);
        if (!st->cfg->quiet)
            printf("[bad stream event: %s]\n", err);
        return;
    }
    type = json_get_str(ev, "type");
    if (!type) {
        /* nothing */
    } else if (strcmp(type, "message_start") == 0) {
        jval *msg = json_get(ev, "message");
        jval *usage = json_get(msg, "usage");
        const char *model = json_get_str(msg, "model");
        if (model)
            snprintf(st->res->model, sizeof st->res->model, "%s", model);
        st->res->in_tok = json_get_long(usage, "input_tokens", 0);
        st->res->cache_read = json_get_long(usage, "cache_read_input_tokens", 0);
        st->res->cache_write =
            json_get_long(usage, "cache_creation_input_tokens", 0);
    } else if (strcmp(type, "content_block_start") == 0) {
        pending_t *p = pend_at(st, json_get_long(ev, "index", -1));
        jval *cb = json_get(ev, "content_block");
        const char *btype = json_get_str(cb, "type");
        if (p && cb && cb->t == J_OBJ) {
            jval *prev, *cur;
            pend_reset(p);
            p->active = 1;
            /* Detach the block object from the event tree. */
            for (prev = NULL, cur = ev->child; cur && cur != cb; cur = cur->next)
                prev = cur;
            if (prev)
                prev->next = cb->next;
            else
                ev->child = cb->next;
            cb->next = NULL;
            p->start = cb;
            if (btype && strcmp(btype, "text") == 0)
                p->has_text = 1;
            block_started(st, btype, cb);
        }
    } else if (strcmp(type, "content_block_delta") == 0) {
        pending_t *p = pend_at(st, json_get_long(ev, "index", -1));
        jval *d = json_get(ev, "delta");
        const char *dtype = json_get_str(d, "type");
        if (p && p->active && dtype) {
            jval *v;
            if (strcmp(dtype, "text_delta") == 0 &&
                (v = json_get(d, "text")) && v->t == J_STR) {
                buf_append(&p->text, v->s, v->n);
                p->has_text = 1;
                con_print(st, v->s, v->n);
            } else if (strcmp(dtype, "thinking_delta") == 0 &&
                       (v = json_get(d, "thinking")) && v->t == J_STR) {
                buf_append(&p->thinking, v->s, v->n);
                p->has_thinking = 1;
                if (st->cfg->show_thinking)
                    con_print(st, v->s, v->n);
            } else if (strcmp(dtype, "signature_delta") == 0 &&
                       (v = json_get(d, "signature")) && v->t == J_STR) {
                buf_append(&p->signature, v->s, v->n);
                p->has_signature = 1;
            } else if (strcmp(dtype, "input_json_delta") == 0 &&
                       (v = json_get(d, "partial_json")) && v->t == J_STR) {
                buf_append(&p->json, v->s, v->n);
                p->has_json = 1;
            }
        }
    } else if (strcmp(type, "content_block_stop") == 0) {
        pending_t *p = pend_at(st, json_get_long(ev, "index", -1));
        if (p && p->active) {
            if (st->in_thinking_print) {
                con_newline(st);
                st->in_thinking_print = 0;
            }
            finish_block(st, p);
            pend_reset(p);
        }
    } else if (strcmp(type, "message_delta") == 0) {
        jval *d = json_get(ev, "delta");
        const char *sr = json_get_str(d, "stop_reason");
        if (sr)
            snprintf(st->res->stop_reason, sizeof st->res->stop_reason, "%s", sr);
        st->res->out_tok =
            json_get_long(json_get(ev, "usage"), "output_tokens", st->res->out_tok);
    } else if (strcmp(type, "message_stop") == 0) {
        st->got_stop = 1;
    } else if (strcmp(type, "error") == 0) {
        jval *e = json_get(ev, "error");
        const char *et = json_get_str(e, "type");
        const char *em = json_get_str(e, "message");
        set_error(st, et ? et : "error", em ? em : "(no message)");
    }
    json_free(ev);
}

/* A non-streamed reply: one message object. Its blocks go through the same
 * console output and serialization as streamed ones. */
static void on_message(stream_t *st)
{
    char err[96];
    jval *msg = json_parse(st->whole.p ? st->whole.p : "", st->whole.len,
                           err, sizeof err);
    jval *usage, *content, *cb;
    const char *s;

    if (!msg || msg->t != J_OBJ) {
        set_error(st, "invalid_response", err);
        json_free(msg);
        return;
    }
    if ((cb = json_get(msg, "error")) != NULL) {
        const char *et = json_get_str(cb, "type");
        const char *em = json_get_str(cb, "message");
        set_error(st, et ? et : "error", em ? em : "(no message)");
        json_free(msg);
        return;
    }
    if ((s = json_get_str(msg, "model")) != NULL)
        snprintf(st->res->model, sizeof st->res->model, "%s", s);
    if ((s = json_get_str(msg, "stop_reason")) != NULL)
        snprintf(st->res->stop_reason, sizeof st->res->stop_reason, "%s", s);
    usage = json_get(msg, "usage");
    st->res->in_tok = json_get_long(usage, "input_tokens", 0);
    st->res->out_tok = json_get_long(usage, "output_tokens", 0);
    st->res->cache_read = json_get_long(usage, "cache_read_input_tokens", 0);
    st->res->cache_write = json_get_long(usage, "cache_creation_input_tokens", 0);

    content = json_get(msg, "content");
    while (content && content->t == J_ARR && (cb = content->child) != NULL) {
        pending_t p;
        const char *btype = json_get_str(cb, "type");
        jval *v;
        content->child = cb->next;
        cb->next = NULL;
        if (cb->t != J_OBJ) {
            json_free(cb);
            continue;
        }
        block_started(st, btype, cb);
        if (btype && strcmp(btype, "text") == 0 &&
            (v = json_get(cb, "text")) && v->t == J_STR)
            con_print(st, v->s, v->n);
        else if (btype && strcmp(btype, "thinking") == 0 &&
                 st->cfg->show_thinking && (v = json_get(cb, "thinking")) &&
                 v->t == J_STR)
            con_print(st, v->s, v->n);
        if (st->in_thinking_print) {
            con_newline(st);
            st->in_thinking_print = 0;
        }
        memset(&p, 0, sizeof p);
        p.start = cb;
        finish_block(st, &p);
        pend_reset(&p);
    }
    st->got_stop = 1;
    json_free(msg);
}

/* SSE framing: "data:" lines accumulate until a blank line. */
static void on_body(void *ctx, const char *data, size_t n)
{
    stream_t *st = ctx;
    size_t i;
    if (!st->cfg->stream) {
        buf_append(&st->whole, data, n);
        return;
    }
    for (i = 0; i < n; i++) {
        char ch = data[i];
        if (ch != '\n') {
            buf_putc(&st->line, ch);
            continue;
        }
        if (st->line.len > 0 && st->line.p[st->line.len - 1] == '\r')
            st->line.p[--st->line.len] = 0;
        if (st->line.len == 0) {
            if (st->data.len > 0)
                on_event(st, st->data.p, st->data.len);
            buf_clear(&st->data);
        } else if (strncmp(st->line.p, "data:", 5) == 0) {
            const char *v = st->line.p + 5;
            if (*v == ' ')
                v++;
            if (st->data.len > 0)
                buf_putc(&st->data, '\n');
            buf_puts(&st->data, v);
        }
        buf_clear(&st->line);
    }
}

/* After a mid-output fallback, blocks before the last fallback marker that
 * belong to the declined attempt must not be echoed back: only its text
 * survives (see the API's refusal fallback rules). */
static void filter_fallback(api_result *r)
{
    size_t i, j, last = (size_t)-1;
    for (i = 0; i < r->n; i++)
        if (strcmp(r->b[i].type, "fallback") == 0)
            last = i;
    if (last == (size_t)-1)
        return;
    for (i = 0, j = 0; i < r->n; i++) {
        if (i < last && strcmp(r->b[i].type, "text") != 0) {
            free(r->b[i].type);
            free(r->b[i].json);
            free(r->b[i].id);
            free(r->b[i].name);
            json_free(r->b[i].input);
            continue;
        }
        r->b[j++] = r->b[i];
    }
    r->n = j;
}

/* ------------------------------------------------------------ request */

static void build_body(buf_t *b, const api_cfg *cfg, const char *system,
                       const char *tools_json, const conv_t *conv)
{
    size_t i;
    buf_puts(b, "{\"model\":");
    json_put_cstr(b, cfg->model);
    buf_printf(b, ",\"max_tokens\":%ld,\"stream\":%s", cfg->max_tokens,
               cfg->stream ? "true" : "false");
    if (cfg->thinking) {
        buf_puts(b, ",\"thinking\":{\"type\":\"adaptive\"");
        if (cfg->show_thinking)
            buf_puts(b, ",\"display\":\"summarized\"");
        buf_putc(b, '}');
    }
    if (cfg->effort && cfg->effort[0]) {
        buf_puts(b, ",\"output_config\":{\"effort\":");
        json_put_cstr(b, cfg->effort);
        buf_putc(b, '}');
    }
    if (cfg->fallbacks)
        buf_puts(b, ",\"fallbacks\":\"default\"");
    if (cfg->cache)
        buf_puts(b, ",\"cache_control\":{\"type\":\"ephemeral\"}");
    buf_puts(b, ",\"system\":");
    json_put_cstr(b, system);
    if (tools_json) {
        buf_puts(b, ",\"tools\":");
        buf_puts(b, tools_json);
    }
    buf_puts(b, ",\"messages\":[");
    for (i = 0; i < conv->n; i++) {
        if (i)
            buf_putc(b, ',');
        buf_puts(b, "{\"role\":\"");
        buf_puts(b, conv->m[i].role);
        buf_puts(b, "\",\"content\":[");
        buf_append(b, conv->m[i].content.p, conv->m[i].content.len);
        buf_puts(b, "]}");
    }
    buf_puts(b, "]}");
}

static void print_http_error(int status, const buf_t *body)
{
    char err[64];
    jval *v = json_parse(body->p ? body->p : "", body->len, err, sizeof err);
    const char *msg = json_get_str(json_get(v, "error"), "message");
    const char *type = json_get_str(json_get(v, "error"), "type");
    printf("API error %d", status);
    if (type)
        printf(" (%s)", type);
    if (msg) {
        fputs(": ", stdout);
        print_utf8(msg, strlen(msg));
    } else if (body->len) {
        printf(": %.300s", body->p);
    }
    putchar('\n');
    json_free(v);
}

static int retryable_status(int status)
{
    return status == 408 || status == 429 || status == 500 || status == 502 ||
           status == 503 || status == 504 || status == 529;
}

int api_send(const api_cfg *cfg, const char *system, const char *tools_json,
             const conv_t *conv, api_result *out)
{
    buf_t body, headers, errbody;
    int attempt, rc = API_FAIL;

    buf_init(&body);
    buf_init(&headers);
    buf_init(&errbody);
    build_body(&body, cfg, system, tools_json, conv);

    buf_puts(&headers, cfg->stream ? "Content-Type: application/json\r\n"
                                     "Accept: text/event-stream\r\n"
                                   : "Content-Type: application/json\r\n"
                                     "Accept: application/json\r\n");
    buf_puts(&headers, "User-Agent: dosagent/" VERSION "\r\n"
                       "anthropic-version: 2023-06-01\r\n");
    if (cfg->api_key && cfg->api_key[0])
        buf_printf(&headers, "x-api-key: %s\r\n", cfg->api_key);
    if (cfg->fallbacks)
        buf_puts(&headers, "anthropic-beta: server-side-fallback-2026-07-01\r\n");

    for (attempt = 0; attempt < 5; attempt++) {
        stream_t st;
        int status, retry_after = 0;
        unsigned wait;
        size_t i;

        memset(&st, 0, sizeof st);
        st.cfg = cfg;
        st.res = out;
        st.at_line_start = 1;
        api_result_free(out);
        if (!cfg->stream && !cfg->quiet) {
            printf("[waiting for reply...]\n");
            fflush(stdout);
        }

        status = http_post(&cfg->target, cfg->path, headers.p, body.p, body.len,
                           on_body, &st, &errbody, &retry_after);
        for (i = 0; i < st.npend; i++)
            pend_reset(&st.pend[i]);
        free(st.pend);
        buf_free(&st.line);
        buf_free(&st.data);
        if (!cfg->stream && status >= 200 && status < 300)
            on_message(&st);
        buf_free(&st.whole);
        con_newline(&st);

        if (status == HTTP_ERR_ABORT) {
            if (!cfg->quiet)
                printf("[interrupted]\n");
            buf_free(&st.error_msg);
            rc = API_ABORTED;
            break;
        }
        if (status >= 200 && status < 300 && st.got_stop && !st.stream_error) {
            buf_free(&st.error_msg);
            filter_fallback(out);
            rc = API_OK;
            break;
        }

        if (cfg->quiet) {
            /* A side request: no messages, no retries. */
            buf_free(&st.error_msg);
            break;
        } else if (status >= 200 && status < 300) {
            if (st.stream_error) {
                printf("Stream error (%s): ", st.error_type);
                print_utf8(st.error_msg.p, st.error_msg.len);
                putchar('\n');
            } else {
                printf("Connection lost mid-response.\n");
            }
        } else if (status > 0) {
            print_http_error(status, &errbody);
        } else {
            printf("Network error talking to %s.\n", cfg->target.host);
        }
        buf_free(&st.error_msg);

        if (status == HTTP_ERR_FATAL)
            break;
        if (status > 0 && status != 200 && !retryable_status(status))
            break;
        if (status == 200 && st.stream_error &&
            strcmp(st.error_type, "overloaded_error") != 0 &&
            strcmp(st.error_type, "api_error") != 0)
            break;
        if (attempt == 4)
            break;
        wait = retry_after > 0 ? (unsigned)retry_after : (2u << attempt);
        if (wait > 60)
            wait = 60;
        printf("Retrying in %u s (Esc to cancel)...\n", wait);
        for (i = 0; i < wait * 10; i++) {
            plat_sleep_ms(100);
            if (plat_poll_abort())
                break;
        }
        if (plat_poll_abort()) {
            rc = API_ABORTED;
            break;
        }
    }
    if (rc != API_OK)
        api_result_free(out);
    buf_free(&body);
    buf_free(&headers);
    buf_free(&errbody);
    return rc;
}
