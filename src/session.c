#include "session.h"
#include "charset.h"
#include "json.h"
#include "platform.h"
#include "version.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 260
#endif

#if PLAT_DOS
#define SEP '\\'
#else
#define SEP '/'
#endif

static int enabled;
static char dir[PATH_MAX];
static char name[16];
static char ses_path[PATH_MAX], txt_path[PATH_MAX];
static FILE *ses, *txt;
static int loading;     /* replaying a file: don't log the blocks again */

/* What the header of the next new session records. */
static buf_t sys_saved;
static char model_saved[64];
static char server_saved[256];

static void log_block(const char *role, const char *block_json);

void session_init(const char *d)
{
    snprintf(dir, sizeof dir, "%s", d);
    enabled = d[0] != 0;
    buf_init(&sys_saved);
    if (enabled)
        conv_log = log_block;
}

int session_enabled(void)
{
    return enabled;
}

const char *session_name(void)
{
    return name;
}

void tr_stamp(char *out, size_t n)
{
    time_t t = time(NULL);
    strftime(out, n, "%Y-%m-%d %H:%M:%S", localtime(&t));
}

static void sync_file(FILE *f)
{
    /* Commit to disk: on DOS a crash loses whatever is still buffered. */
    fflush(f);
    fsync(fileno(f));
}

static int exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static void join(char *out, size_t n, const char *file)
{
    size_t len = strlen(dir);
    if (len && (dir[len - 1] == '\\' || dir[len - 1] == '/' ||
                dir[len - 1] == ':'))
        snprintf(out, n, "%s%s", dir, file);
    else
        snprintf(out, n, "%s%c%s", dir, SEP, file);
}

/* foo.SES -> foo.TXT */
static void txt_for(char *out, size_t n, const char *ses_file)
{
    char *dot;
    snprintf(out, n, "%s", ses_file);
    dot = strrchr(out, '.');
    if (dot && !strpbrk(dot, "\\/"))
        *dot = 0;
    snprintf(out + strlen(out), n - strlen(out), ".TXT");
}

/* Upper-case base name without extension. */
static void name_from(const char *path)
{
    const char *b = path, *p;
    size_t i;
    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            b = p + 1;
    for (i = 0; b[i] && b[i] != '.' && i < sizeof name - 1; i++)
        name[i] = toupper((unsigned char)b[i]);
    name[i] = 0;
}

void session_new(const char *system, const char *model, const char *server)
{
    session_close();
    buf_clear(&sys_saved);
    buf_puts(&sys_saved, system);
    snprintf(model_saved, sizeof model_saved, "%s", model);
    snprintf(server_saved, sizeof server_saved, "%s", server);
}

static void write_header(void)
{
    buf_t b;
    char stamp[24];
    tr_stamp(stamp, sizeof stamp);
    buf_init(&b);
    buf_puts(&b, "{\"dosagent\":\"" VERSION "\",\"started\":");
    json_put_cstr(&b, stamp);
    buf_puts(&b, ",\"model\":");
    json_put_cstr(&b, model_saved);
    buf_puts(&b, ",\"server\":");
    json_put_cstr(&b, server_saved);
    buf_puts(&b, ",\"system\":");
    json_put_str(&b, sys_saved.p ? sys_saved.p : "", sys_saved.len);
    buf_puts(&b, "}\n");
    fwrite(b.p, 1, b.len, ses);
    sync_file(ses);
    buf_free(&b);

    fprintf(txt, "DOSAGENT %s session %s, started %s\n", VERSION, name, stamp);
    fprintf(txt, "Model %s via %s\n", model_saved, server_saved);
    sync_file(txt);
}

void session_open(void)
{
    struct tm *tm;
    time_t t;
    char file[32];
    int nn;

    if (!enabled || ses)
        return;
    mkdir(dir, 0777);   /* fails harmlessly if it exists */
    t = time(NULL);
    tm = localtime(&t);
    for (nn = 1; nn <= 99; nn++) {
        snprintf(file, sizeof file, "%02d%02d%02d%02d.SES",
                 tm->tm_year % 100, tm->tm_mon + 1, tm->tm_mday, nn);
        join(ses_path, sizeof ses_path, file);
        txt_for(txt_path, sizeof txt_path, ses_path);
        if (!exists(ses_path) && !exists(txt_path))
            break;
    }
    if (nn > 99 || (ses = fopen(ses_path, "ab")) == NULL) {
        printf("warning: cannot create a session file in %s; sessions are "
               "off.\n", dir);
        enabled = 0;
        return;
    }
    if ((txt = fopen(txt_path, "a")) == NULL) {
        printf("warning: cannot create %s; sessions are off.\n", txt_path);
        fclose(ses);
        ses = NULL;
        enabled = 0;
        return;
    }
    name_from(ses_path);
    write_header();
}

void session_close(void)
{
    if (ses)
        fclose(ses);
    if (txt)
        fclose(txt);
    ses = txt = NULL;
    name[0] = 0;
}

static void log_block(const char *role, const char *block_json)
{
    if (loading)
        return;
    session_open();
    if (!ses)
        return;
    fprintf(ses, "{\"role\":\"%s\",\"block\":", role);
    fputs(block_json, ses);
    fputs("}\n", ses);
    sync_file(ses);
}

void session_model(const char *model)
{
    buf_t b;
    snprintf(model_saved, sizeof model_saved, "%s", model);
    if (!ses)
        return;
    buf_init(&b);
    buf_puts(&b, "{\"model\":");
    json_put_cstr(&b, model);
    buf_puts(&b, "}\n");
    fwrite(b.p, 1, b.len, ses);
    sync_file(ses);
    buf_free(&b);
}

/* ------------------------------------------------------------ transcript */

void tr_printf(const char *fmt, ...)
{
    va_list ap;
    if (!txt)
        return;
    va_start(ap, fmt);
    vfprintf(txt, fmt, ap);
    va_end(ap);
    sync_file(txt);
}

void tr_utf8(const char *prefix, const char *s, size_t n)
{
    buf_t loc;
    size_t i = 0;
    if (!txt)
        return;
    buf_init(&loc);
    utf8_to_local(&loc, s, n);
    while (i < loc.len) {
        size_t j = i;
        while (j < loc.len && loc.p[j] != '\n')
            j++;
        fputs(prefix, txt);
        fwrite(loc.p + i, 1, j - i - (j > i && loc.p[j - 1] == '\r'), txt);
        fputc('\n', txt);
        i = j + 1;
    }
    buf_free(&loc);
    sync_file(txt);
}

/* ------------------------------------------------------------ resume */

static int is_session_file(const char *f)
{
    int i;
    for (i = 0; i < 8; i++)
        if (!isdigit((unsigned char)f[i]))
            return 0;
    return strcasecmp(f + 8, ".SES") == 0;
}

int session_find(const char *want, char *path, size_t n)
{
    if (!enabled) {
        printf("Sessions are off; set session_dir in DOSAGENT.CFG.\n");
        return -1;
    }
    if (!want[0]) {
        char best[PATH_MAX] = "";
        struct dirent *e;
        DIR *d = opendir(dir);
        while (d && (e = readdir(d)) != NULL)
            if (is_session_file(e->d_name) && strcmp(e->d_name, best) > 0)
                snprintf(best, sizeof best, "%s", e->d_name);
        if (d)
            closedir(d);
        if (!best[0]) {
            printf("No sessions in %s.\n", dir);
            return -1;
        }
        join(path, n, best);
        return 0;
    }
    if (strpbrk(want, "\\/:"))
        snprintf(path, n, "%s", want);
    else
        join(path, n, want);
    if (!strchr(path + strlen(path) - strlen(want), '.'))
        snprintf(path + strlen(path), n - strlen(path), ".SES");
    if (!exists(path)) {
        printf("No session file %s.\n", path);
        return -1;
    }
    return 0;
}

static int read_file(const char *path, buf_t *out)
{
    FILE *f = fopen(path, "rb");
    char chunk[2048];
    size_t k;
    if (!f)
        return -1;
    while ((k = fread(chunk, 1, sizeof chunk, f)) > 0)
        buf_append(out, chunk, k);
    fclose(f);
    return 0;
}

/* One line of a .SES file. Returns 1 if it was understood. */
static int load_line(char *s, size_t len, conv_t *conv, buf_t *system,
                     char *model, size_t model_n, char *server,
                     size_t server_n, int *header)
{
    static const char pre[] = "{\"role\":\"";
    char err[96];
    jval *v;

    if (len > sizeof pre && memcmp(s, pre, sizeof pre - 1) == 0) {
        /* {"role":"user","block":{...}}: the block is used byte for byte. */
        char *role = s + sizeof pre - 1, *q = strchr(role, '"');
        char *block;
        if (!q || strncmp(q, "\",\"block\":", 10) != 0 || s[len - 1] != '}')
            return 0;
        *q = 0;
        block = q + 10;
        s[len - 1] = 0;
        v = json_parse(block, strlen(block), err, sizeof err);
        if (!v || v->t != J_OBJ ||
            (strcmp(role, "user") != 0 && strcmp(role, "assistant") != 0)) {
            json_free(v);
            return 0;
        }
        json_free(v);
        conv_append(conv, role, block);
        return 1;
    }
    v = json_parse(s, len, err, sizeof err);
    if (!v || v->t != J_OBJ) {
        json_free(v);
        return 0;
    }
    if (json_get(v, "system")) {
        jval *sp = json_get(v, "system");
        const char *m = json_get_str(v, "model"), *sv = json_get_str(v, "server");
        if (sp->t == J_STR)
            buf_append(system, sp->s, sp->n);
        snprintf(model, model_n, "%s", m ? m : "");
        snprintf(server, server_n, "%s", sv ? sv : "");
        *header = 1;
    } else if (json_get_str(v, "model")) {
        snprintf(model, model_n, "%s", json_get_str(v, "model"));
    } else {
        json_free(v);
        return 0;
    }
    json_free(v);
    return 1;
}

static int has_id(const jval *arr, const char *key, const char *id)
{
    const jval *b;
    for (b = arr ? arr->child : NULL; b; b = b->next) {
        const char *s = json_get_str(b, key);
        if (s && strcmp(s, id) == 0)
            return 1;
    }
    return 0;
}

static jval *parse_content(const msg_t *m)
{
    buf_t b;
    char err[96];
    jval *v;
    buf_init(&b);
    buf_putc(&b, '[');
    buf_append(&b, m->content.p, m->content.len);
    buf_putc(&b, ']');
    v = json_parse(b.p, b.len, err, sizeof err);
    buf_free(&b);
    return v;
}

/* A crash between a tool call and its result leaves a tool_use the API
 * would reject without a tool_result; answer it with an error. */
static int close_tool_calls(conv_t *conv)
{
    size_t a;
    jval *calls, *results = NULL;
    const jval *b;
    int added = 0;

    if (conv->n == 0)
        return 0;
    a = conv->n - 1;
    if (strcmp(conv->m[a].role, "user") == 0) {
        if (a == 0)
            return 0;
        results = parse_content(&conv->m[a]);
        a--;
    }
    if (strcmp(conv->m[a].role, "assistant") != 0) {
        json_free(results);
        return 0;
    }
    calls = parse_content(&conv->m[a]);
    for (b = calls ? calls->child : NULL; b; b = b->next) {
        const char *type = json_get_str(b, "type"), *id = json_get_str(b, "id");
        buf_t r;
        if (!type || strcmp(type, "tool_use") != 0 || !id ||
            has_id(results, "tool_use_id", id))
            continue;
        buf_init(&r);
        buf_puts(&r, "{\"type\":\"tool_result\",\"tool_use_id\":");
        json_put_cstr(&r, id);
        buf_puts(&r, ",\"content\":\"No result: DOSAGENT stopped before this "
                     "call finished, so it may or may not have run.\","
                     "\"is_error\":true}");
        conv_append(conv, "user", r.p);
        buf_free(&r);
        added++;
    }
    json_free(calls);
    json_free(results);
    return added;
}

/* Prints the last lines of the transcript, to show where it left off. */
static void show_tail(int lines)
{
    buf_t b;
    size_t i;
    int seen = 0;
    if (!txt_path[0] || !name[0])
        return;
    buf_init(&b);
    if (read_file(txt_path, &b) < 0 || b.len == 0) {
        buf_free(&b);
        return;
    }
    i = b.len;
    if (b.p[i - 1] == '\n')
        i--;
    while (i > 0 && seen < lines)
        if (b.p[--i] == '\n')
            seen++;
    if (b.p[i] == '\n')
        i++;
    for (; i < b.len; i++)
        if (b.p[i] != '\r')
            putchar(b.p[i]);
    buf_free(&b);
}

int session_load(const char *path, conv_t *conv, buf_t *system,
                 char *model, size_t model_n, char *server, size_t server_n)
{
    buf_t raw;
    size_t i = 0;
    int header = 0, bad = 0, added;
    char stamp[24];

    buf_init(&raw);
    if (read_file(path, &raw) < 0) {
        printf("Cannot read %s.\n", path);
        return -1;
    }
    buf_clear(system);
    loading = 1;
    while (i < raw.len) {
        size_t j = i, len;
        while (j < raw.len && raw.p[j] != '\n')
            j++;
        len = j - i;
        if (len && raw.p[i + len - 1] == '\r')
            len--;
        /* A last line without a newline was cut short by a crash. */
        if (j == raw.len ||
            (len && !load_line(raw.p + i, len, conv, system, model, model_n,
                               server, server_n, &header)))
            bad++;
        i = j + 1;
    }
    loading = 0;
    if (!header) {
        printf("%s is not a DOSAGENT session file.\n", path);
        buf_free(&raw);
        conv_free(conv);
        return -1;
    }

    session_close();
    snprintf(ses_path, sizeof ses_path, "%s", path);
    txt_for(txt_path, sizeof txt_path, ses_path);
    ses = fopen(ses_path, "ab");
    txt = fopen(txt_path, "a");
    if (!ses || !txt) {
        printf("warning: cannot write to %s; this session is not being "
               "recorded.\n", ses ? txt_path : ses_path);
        session_close();
    } else {
        name_from(ses_path);
        if (raw.len && raw.p[raw.len - 1] != '\n')
            fputc('\n', ses);
    }
    buf_free(&raw);

    buf_clear(&sys_saved);
    buf_append(&sys_saved, system->p ? system->p : "", system->len);
    snprintf(model_saved, sizeof model_saved, "%s", model);
    snprintf(server_saved, sizeof server_saved, "%s", server);

    printf("Resumed session %s (%lu messages). Its transcript ends:\n\n",
           name[0] ? name : "?", (unsigned long)conv->n);
    show_tail(15);
    printf("\n");
    tr_stamp(stamp, sizeof stamp);
    tr_printf("\n--- Resumed %s ---\n", stamp);
    if (bad)
        printf("warning: skipped %d damaged line%s in %s.\n", bad,
               bad == 1 ? "" : "s", path);
    added = close_tool_calls(conv);
    if (added) {
        printf("%d unfinished tool call%s marked as interrupted.\n", added,
               added == 1 ? "" : "s");
        tr_printf("[%d unfinished tool call%s marked as interrupted.]\n",
                  added, added == 1 ? "" : "s");
    }
    return 0;
}
