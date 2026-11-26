/* Session files: every message block goes to a .SES file as it is added,
 * so a conversation can be resumed after a crash or restart, and a
 * readable transcript goes to a .TXT file of the same name. Files are
 * named YYMMDDnn in the session directory and created on the first
 * prompt, so starting and quitting leaves nothing behind. */
#ifndef SESSION_H
#define SESSION_H

#include <stddef.h>
#include "api.h"
#include "buf.h"

/* dir "" turns sessions off; the other calls then do nothing. */
void session_init(const char *dir);
int session_enabled(void);
/* Starts a new session (closing the current one); its files are created
 * by session_open or the first logged block. */
void session_new(const char *system, const char *model, const char *server);
void session_open(void);
void session_close(void);
/* Name of the current session (25112501), or "" if no files yet. */
const char *session_name(void);
/* Records a /model switch, so a resume picks the right model. */
void session_model(const char *model);
/* The session's title (UTF-8, one line): the first prompt unless set
 * with /title or by the model. session_default_title sets it only for a
 * session not yet written. session_set_title returns -1 if the file
 * could not be updated. */
int session_set_title(const char *utf8);
void session_default_title(const char *utf8);
const char *session_title(void);

/* Transcript text in the local charset; written only once the files are
 * open. tr_utf8 converts and puts prefix before every line. */
void tr_printf(const char *fmt, ...);
void tr_utf8(const char *prefix, const char *s, size_t n);
/* "2026-11-25 14:03:05" for now. */
void tr_stamp(char *out, size_t n);

/* Finds a session file: name "" lists the sessions, newest first, and
 * asks which one; else a name (25112501), a file name (25112501.SES) or
 * a path. */
int session_find(const char *name, char *path, size_t n);
/* Loads a .SES file into conv (which must be empty), gives back the
 * session's system prompt, model and server, and continues logging to
 * that session. Tool calls left without results by a crash get error
 * results. Shows the end of the transcript. Returns 0, or -1 with a
 * message printed. */
int session_load(const char *path, conv_t *conv, buf_t *system,
                 char *model, size_t model_n, char *server, size_t server_n);

#endif
