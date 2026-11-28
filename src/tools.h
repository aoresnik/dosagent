/* Tools the model can call, and their local implementations. */
#ifndef TOOLS_H
#define TOOLS_H

#include "buf.h"
#include "json.h"

/* Set once at startup. */
void tools_init(const char *temp_dir, long max_output);
/* JSON array of tool definitions for the request. */
const char *tools_json(void);
/* Nonzero if the tool changes the machine and needs user approval. */
int tool_needs_confirm(const char *name);
/* Nonzero if the call cannot work as written and should not be run (or
 * shown for approval); result then receives the reason for the model. */
int tool_reject(const char *name, const jval *input, buf_t *result);
/* One-line (or few-line) human readable summary of a call, local charset. */
void tool_describe(const char *name, const jval *input, buf_t *out);
/* Runs the tool. result receives UTF-8 text for the model. Returns 1 if the
 * result is an error. */
int tool_run(const char *name, const jval *input, buf_t *result);

#endif
