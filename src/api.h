/* Conversation history and the streaming Messages API call. */
#ifndef API_H
#define API_H

#include "buf.h"
#include "http.h"
#include "json.h"

/* A message's content is stored as serialized JSON content blocks joined
 * by commas (the array without its brackets). History is append-only:
 * blocks are echoed back byte-for-byte as received, which keeps thinking
 * signatures valid and the prompt cache warm. */
typedef struct {
    char role[12];
    buf_t content;
} msg_t;

typedef struct {
    msg_t *m;
    size_t n, cap;
    size_t thin_from;   /* conv_drop_thinking has done messages before this */
} conv_t;

void conv_init(conv_t *c);
void conv_free(conv_t *c);
/* Appends to the last message if it has the same role, else starts one. */
void conv_append(conv_t *c, const char *role, const char *block_json);
void conv_add_user_text(conv_t *c, const char *utf8);
/* Empties the text of thinking blocks without a signature (local servers
 * send those) in the messages so far. Called before a new prompt, it
 * leaves the model only the current turn's reasoning, as llama.cpp's
 * chat templates do with preserve_thinking off. Signed (Claude) blocks
 * are never changed. */
void conv_drop_thinking(conv_t *c);

typedef struct {
    http_target target;
    const char *path;
    const char *api_key;
    const char *model;
    const char *effort;     /* "" = model default */
    long max_tokens;
    int thinking;           /* send adaptive thinking config */
    int show_thinking;      /* ask for summarized thinking and print it */
    int fallbacks;          /* server-side refusal fallbacks */
    int cache;              /* automatic prompt caching */
    int stream;             /* 0 = whole reply at once (fewer packets) */
} api_cfg;

typedef struct {
    char *type;
    char *json;   /* complete serialized block, ready to echo back */
    char *id;     /* tool_use only */
    char *name;   /* tool_use only */
    jval *input;  /* tool_use only; NULL if the input JSON was invalid */
} block_t;

typedef struct {
    block_t *b;
    size_t n;
    char stop_reason[32];
    char model[64];
    long in_tok, out_tok, cache_read, cache_write;
} api_result;

void api_result_free(api_result *r);

#define API_OK 0
#define API_FAIL (-1)
#define API_ABORTED (-2)

/* Sends the conversation and prints the reply to the console as it streams
 * in (or all at once if cfg->stream is 0). On API_OK
 * the result holds the assistant's content blocks, already filtered as the
 * API requires for echoing back (see fallback handling in api.c). */
int api_send(const api_cfg *cfg, const char *system, const char *tools_json,
             const conv_t *conv, api_result *out);

#endif
