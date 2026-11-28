#include "tools.h"
#include "charset.h"
#include "net.h"
#include "platform.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifndef PATH_MAX
#define PATH_MAX 260
#endif

static char out_path[PATH_MAX];
static long max_output = 16000;

#define DESC_RUN \
    "Run a command line and return its exit code and output (stdout and " \
    "stderr). stdin is NUL. 'cd' and drive changes (e.g. 'D:') are handled " \
    "by the harness and persist between calls."

static const char tools_common[] =
    "[{\"name\":\"run_command\",\"description\":\"" DESC_RUN
#if PLAT_DOS
    " Runs via COMMAND.COM, which has no && or ||: give one command per "
    "call, and check its exit code before the next call. For a longer "
    "sequence, write a batch file and run that. There is no timeout: never "
    "start a program that "
    "waits for keyboard input or does not exit on its own. Set "
    "capture_screen for programs that write to video memory instead of "
    "stdout.\","
#else
    " Runs via /bin/sh.\","
#endif
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"The command line.\"}"
#if PLAT_DOS
    ",\"capture_screen\":{\"type\":\"boolean\",\"description\":"
    "\"Also return the text screen contents after the command exits.\"}"
#endif
    "},\"required\":[\"command\"]}},"

    "{\"name\":\"read_file\",\"description\":\"Read a text file. Returns "
    "numbered lines (the numbers are not part of the file). Use offset and "
    "limit to page through large files.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},"
    "\"offset\":{\"type\":\"integer\",\"description\":\"First line to "
    "return, 1-based. Default 1.\"},"
    "\"limit\":{\"type\":\"integer\",\"description\":\"Maximum number of "
    "lines. Default 400.\"}},\"required\":[\"path\"]}},"

    "{\"name\":\"write_file\",\"description\":\"Create or overwrite a text "
    "file with the given content. Line endings are converted for the "
    "platform.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},"
    "\"required\":[\"path\",\"content\"]}},"

    "{\"name\":\"edit_file\",\"description\":\"Replace exact text in a file. "
    "old_str must match exactly once unless replace_all is true. Read the "
    "file first.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},\"old_str\":{\"type\":\"string\"},"
    "\"new_str\":{\"type\":\"string\"},\"replace_all\":{\"type\":\"boolean\"}},"
    "\"required\":[\"path\",\"old_str\",\"new_str\"]}},"

    "{\"name\":\"list_dir\",\"description\":\"List a directory: names, "
    "sizes and dates.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"Default: current "
    "directory.\"}}}}"
#if PLAT_DOS
    ",{\"name\":\"read_screen\",\"description\":\"Return the current text "
    "screen contents.\",\"input_schema\":{\"type\":\"object\","
    "\"properties\":{}}}"
#endif
    "]";

void tools_init(const char *temp_dir, long maxout)
{
    char cwd[PATH_MAX];
    const char *dir = temp_dir;
    size_t n;

    if (!dir || !dir[0])
        dir = getcwd(cwd, sizeof cwd) ? cwd : ".";
    n = strlen(dir);
    snprintf(out_path, sizeof out_path, "%s%sAGENTOUT.TMP", dir,
             (n && (dir[n - 1] == '/' || dir[n - 1] == '\\')) ? "" : "/");
    if (maxout > 0)
        max_output = maxout;
}

const char *tools_json(void)
{
    return tools_common;
}

int tool_needs_confirm(const char *name)
{
    return strcmp(name, "run_command") == 0 || strcmp(name, "write_file") == 0 ||
           strcmp(name, "edit_file") == 0;
}

/* ------------------------------------------------------------ helpers */

#if PLAT_DOS
/* COMMAND.COM has no && or ||; it passes them to the program as arguments,
 * so a chained command runs only its first part, with garbage arguments. */
static int has_chain_op(const char *cmd)
{
    int quoted = 0;
    for (; *cmd; cmd++) {
        if (*cmd == '"')
            quoted = !quoted;
        else if (!quoted && (cmd[0] == '&' || cmd[0] == '|') &&
                 cmd[1] == cmd[0])
            return 1;
    }
    return 0;
}
#endif

static const char *arg_str(const jval *in, const char *key)
{
    return json_get_str(in, key);
}

/* Model-supplied UTF-8 string -> local charset C string (caller frees). */
static char *to_local(const char *utf8)
{
    buf_t b;
    buf_init(&b);
    utf8_to_local(&b, utf8, strlen(utf8));
    return buf_take(&b);
}

static int read_whole(const char *path, buf_t *out)
{
    FILE *f = fopen(path, "rb");
    char chunk[2048];
    size_t n;
    if (!f)
        return -1;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        buf_append(out, chunk, n);
    fclose(f);
    return 0;
}

static int write_whole(const char *path, const char *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    if (fwrite(data, 1, n, f) != n) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static int has_crlf(const buf_t *b)
{
    return b->len > 1 && b->p && strstr(b->p, "\r\n") != NULL;
}

/* Local file bytes -> UTF-8 with LF line endings. */
static void file_to_model(buf_t *out, const char *data, size_t n)
{
    buf_t lf;
    size_t i;
    buf_init(&lf);
    buf_reserve(&lf, n);
    for (i = 0; i < n; i++) {
        if (data[i] == '\r' && i + 1 < n && data[i + 1] == '\n')
            continue;
        if (data[i] == 0x1A && i + 1 == n) /* DOS EOF marker */
            continue;
        lf.p[lf.len++] = data[i];
    }
    if (lf.p)
        lf.p[lf.len] = 0;
    local_to_utf8(out, lf.p ? lf.p : "", lf.len);
    buf_free(&lf);
}

/* UTF-8 with LF -> local bytes, optionally with CRLF. */
static void model_to_file(buf_t *out, const char *utf8, size_t n, int crlf)
{
    buf_t loc;
    size_t i;
    buf_init(&loc);
    utf8_to_local(&loc, utf8, n);
    for (i = 0; i < loc.len; i++) {
        if (loc.p[i] == '\n' && crlf && (i == 0 || loc.p[i - 1] != '\r'))
            buf_putc(out, '\r');
        buf_putc(out, loc.p[i]);
    }
    buf_free(&loc);
}

static int fail(buf_t *res, const char *fmt, const char *arg)
{
    buf_t tmp;
    buf_init(&tmp);
    buf_printf(&tmp, fmt, arg ? arg : "", strerror(errno));
    local_to_utf8(res, tmp.p, tmp.len);
    buf_free(&tmp);
    return 1;
}

/* Keep the head and tail of oversized output. */
static void cap_output(buf_t *b)
{
    size_t keep = (size_t)max_output / 2;
    buf_t out;
    if (b->len <= (size_t)max_output)
        return;
    buf_init(&out);
    buf_append(&out, b->p, keep);
    buf_printf(&out, "\n[... %lu bytes omitted ...]\n",
               (unsigned long)(b->len - 2 * keep));
    buf_append(&out, b->p + b->len - keep, keep);
    buf_free(b);
    *b = out;
}

/* Show the first lines of some local text on the console, indented. */
static void echo_lines(const char *s, size_t n, int max_lines)
{
    int lines = 0;
    size_t i = 0;
    while (i < n && lines < max_lines) {
        size_t j = i;
        while (j < n && s[j] != '\n')
            j++;
        printf("  | %.*s\n", (int)(j - i > 76 ? 76 : j - i), s + i);
        lines++;
        i = j + 1;
    }
    if (i < n)
        printf("  | ...\n");
}

/* ------------------------------------------------------------ run_command */

static int is_cd(const char *cmd, const char **arg)
{
    while (*cmd == ' ')
        cmd++;
    if (isalpha((unsigned char)cmd[0]) && cmd[1] == ':' &&
        (cmd[2] == 0 || cmd[2] == ' ')) {
        *arg = cmd;
        return 1;
    }
    if ((strncmp(cmd, "cd", 2) == 0 || strncmp(cmd, "CD", 2) == 0 ||
         strncmp(cmd, "Cd", 2) == 0 || strncmp(cmd, "cD", 2) == 0) &&
        (cmd[2] == 0 || cmd[2] == ' ' || cmd[2] == '\\' || cmd[2] == '.' ||
         cmd[2] == '/')) {
        cmd += 2;
        while (*cmd == ' ')
            cmd++;
        *arg = cmd;
        return 1;
    }
    return 0;
}

static int run_command(const jval *in, buf_t *res)
{
    const char *cmd_u = arg_str(in, "command");
    char *cmd;
    const char *cd_arg;
    int saved_in, saved_out, saved_err, fd, nul, status;
    buf_t out, screen;
    int got_screen = 0;

    if (!cmd_u || !cmd_u[0]) {
        buf_puts(res, "Missing 'command'.");
        return 1;
    }
    cmd = to_local(cmd_u);

    if (is_cd(cmd, &cd_arg)) {
        char cwd[PATH_MAX];
        char target[PATH_MAX];
        size_t n;
        snprintf(target, sizeof target, "%s", cd_arg);
        n = strlen(target);
        while (n > 0 && target[n - 1] == ' ')
            target[--n] = 0;
        if (n > 0 && chdir(target) != 0) {
            int r = fail(res, "cd %s: %s", target);
            free(cmd);
            return r;
        }
        if (plat_getcwd(cwd, sizeof cwd))
            local_to_utf8(res, cwd, strlen(cwd));
        free(cmd);
        return 0;
    }

    fflush(stdout);
    fflush(stderr);
    fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
    if (fd < 0) {
        int r = fail(res, "cannot create %s: %s", out_path);
        free(cmd);
        return r;
    }
    nul = open(PLAT_NULL_DEVICE, O_RDONLY | O_BINARY);
    saved_in = dup(0);
    saved_out = dup(1);
    saved_err = dup(2);
    if (nul >= 0)
        dup2(nul, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);

    net_release_driver();
    status = system(cmd);

    dup2(saved_in, 0);
    dup2(saved_out, 1);
    dup2(saved_err, 2);
    net_reclaim_driver();
    close(saved_in);
    close(saved_out);
    close(saved_err);
    if (nul >= 0)
        close(nul);

    buf_init(&screen);
    if (json_get_bool(in, "capture_screen", 0))
        got_screen = plat_read_screen(&screen);

#if !PLAT_DOS
    if (status != -1 && (status & 0x7f) == 0)
        status = (status >> 8) & 0xff;
#endif

    buf_init(&out);
    read_whole(out_path, &out);
    remove(out_path);
    cap_output(&out);
    if (out.len)
        echo_lines(out.p, out.len, 12);

    buf_printf(res, "exit code: %d\n", status);
    if (out.len)
        file_to_model(res, out.p, out.len);
    else
        buf_puts(res, "(no output on stdout/stderr)\n");
    if (got_screen) {
        buf_puts(res, "\n--- screen ---\n");
        local_to_utf8(res, screen.p, screen.len);
    }
    buf_free(&out);
    buf_free(&screen);
    free(cmd);
    return 0;
}

/* ------------------------------------------------------------ files */

static int read_file(const jval *in, buf_t *res)
{
    const char *path_u = arg_str(in, "path");
    long offset = json_get_long(in, "offset", 1);
    long limit = json_get_long(in, "limit", 400);
    char *path;
    buf_t raw, text;
    long line = 1, shown = 0, total;
    size_t i, start;
    int truncated = 0;

    if (!path_u) {
        buf_puts(res, "Missing 'path'.");
        return 1;
    }
    if (offset < 1)
        offset = 1;
    if (limit < 1)
        limit = 400;
    path = to_local(path_u);
    buf_init(&raw);
    if (read_whole(path, &raw) < 0) {
        int r = fail(res, "cannot read %s: %s", path);
        free(path);
        buf_free(&raw);
        return r;
    }
    free(path);
    if (raw.len && memchr(raw.p, 0, raw.len < 4096 ? raw.len : 4096)) {
        buf_printf(res, "Binary file (%lu bytes), not shown.",
                   (unsigned long)raw.len);
        buf_free(&raw);
        return 1;
    }
    buf_init(&text);
    file_to_model(&text, raw.p ? raw.p : "", raw.len);
    buf_free(&raw);

    for (i = 0, start = 0; i <= text.len; i++) {
        if (i < text.len && text.p[i] != '\n')
            continue;
        if (i == text.len && start == i)
            break; /* no partial last line */
        if (line >= offset && shown < limit) {
            if (res->len > (size_t)max_output) {
                truncated = 1;
            } else {
                buf_printf(res, "%5ld\t", line);
                buf_append(res, text.p + start, i - start);
                buf_putc(res, '\n');
                shown++;
            }
        }
        line++;
        start = i + 1;
    }
    total = line - 1;
    if (total == 0)
        buf_puts(res, "(empty file)\n");
    else if (shown < total)
        buf_printf(res, "[file has %ld lines; showed %ld from line %ld%s]\n",
                   total, shown, offset,
                   truncated ? "; output size limit reached" : "");
    buf_free(&text);
    return 0;
}

static int write_file(const jval *in, buf_t *res)
{
    const char *path_u = arg_str(in, "path");
    jval *content = json_get(in, "content");
    char *path;
    buf_t data;
    int rc;

    if (!path_u || !content || content->t != J_STR) {
        buf_puts(res, "Missing 'path' or 'content'.");
        return 1;
    }
    path = to_local(path_u);
    buf_init(&data);
    model_to_file(&data, content->s, content->n, PLAT_DOS);
    rc = write_whole(path, data.p ? data.p : "", data.len);
    if (rc < 0)
        rc = fail(res, "cannot write %s: %s", path);
    else
        buf_printf(res, "Wrote %lu bytes.", (unsigned long)data.len);
    free(path);
    buf_free(&data);
    return rc;
}

static int edit_file(const jval *in, buf_t *res)
{
    const char *path_u = arg_str(in, "path");
    jval *old_v = json_get(in, "old_str");
    jval *new_v = json_get(in, "new_str");
    int all = json_get_bool(in, "replace_all", 0);
    char *path;
    buf_t raw, text, edited, data;
    size_t count = 0, i;
    int crlf, rc = 0;

    if (!path_u || !old_v || old_v->t != J_STR || !new_v || new_v->t != J_STR) {
        buf_puts(res, "Missing 'path', 'old_str' or 'new_str'.");
        return 1;
    }
    if (old_v->n == 0) {
        buf_puts(res, "old_str must not be empty (use write_file to create "
                      "files).");
        return 1;
    }
    path = to_local(path_u);
    buf_init(&raw);
    if (read_whole(path, &raw) < 0) {
        rc = fail(res, "cannot read %s: %s", path);
        free(path);
        buf_free(&raw);
        return rc;
    }
    crlf = has_crlf(&raw);
    buf_init(&text);
    file_to_model(&text, raw.p ? raw.p : "", raw.len);
    buf_free(&raw);

    for (i = 0; i + old_v->n <= text.len; i++)
        if (memcmp(text.p + i, old_v->s, old_v->n) == 0) {
            count++;
            i += old_v->n - 1;
        }
    if (count == 0 || (count > 1 && !all)) {
        if (count == 0)
            buf_puts(res, "old_str not found in file. Read the file and copy "
                          "the text exactly.");
        else
            buf_printf(res, "old_str found %lu times; add context to make it "
                            "unique or set replace_all.", (unsigned long)count);
        free(path);
        buf_free(&text);
        return 1;
    }

    buf_init(&edited);
    for (i = 0; i < text.len;) {
        if (i + old_v->n <= text.len &&
            memcmp(text.p + i, old_v->s, old_v->n) == 0) {
            buf_append(&edited, new_v->s, new_v->n);
            i += old_v->n;
        } else {
            buf_putc(&edited, text.p[i++]);
        }
    }
    buf_init(&data);
    model_to_file(&data, edited.p ? edited.p : "", edited.len, crlf);
    if (write_whole(path, data.p ? data.p : "", data.len) < 0)
        rc = fail(res, "cannot write %s: %s", path);
    else
        buf_printf(res, "Replaced %lu occurrence%s.", (unsigned long)count,
                   count == 1 ? "" : "s");
    free(path);
    buf_free(&text);
    buf_free(&edited);
    buf_free(&data);
    return rc;
}

typedef struct {
    char *name;
    int dir;
    long size;
    time_t mtime;
} dent;

static int dent_cmp(const void *a, const void *b)
{
    const dent *x = a, *y = b;
    if (x->dir != y->dir)
        return y->dir - x->dir;
    return strcmp(x->name, y->name);
}

static int list_dir(const jval *in, buf_t *res)
{
    const char *path_u = arg_str(in, "path");
    char *path = to_local(path_u && path_u[0] ? path_u : ".");
    DIR *d = opendir(path);
    struct dirent *e;
    dent *ents = NULL;
    size_t n = 0, i;
    buf_t tmp;

    if (!d) {
        int r = fail(res, "cannot open %s: %s", path);
        free(path);
        return r;
    }
    while ((e = readdir(d)) != NULL) {
        char full[PATH_MAX];
        struct stat sb;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        ents = xrealloc(ents, (n + 1) * sizeof *ents);
        ents[n].name = xstrdup(e->d_name);
        ents[n].dir = 0;
        ents[n].size = 0;
        ents[n].mtime = 0;
        if (stat(full, &sb) == 0) {
            ents[n].dir = S_ISDIR(sb.st_mode);
            ents[n].size = (long)sb.st_size;
            ents[n].mtime = sb.st_mtime;
        }
        n++;
    }
    closedir(d);
    qsort(ents, n, sizeof *ents, dent_cmp);

    buf_init(&tmp);
    for (i = 0; i < n; i++) {
        char date[20] = "";
        struct tm *tm = ents[i].mtime ? localtime(&ents[i].mtime) : NULL;
        if (tm)
            strftime(date, sizeof date, "%Y-%m-%d %H:%M", tm);
        if (i < 500) {
            if (ents[i].dir)
                buf_printf(&tmp, "%-14s %10s  %s\n", ents[i].name, "<DIR>", date);
            else
                buf_printf(&tmp, "%-14s %10ld  %s\n", ents[i].name,
                           ents[i].size, date);
        }
        free(ents[i].name);
    }
    if (n > 500)
        buf_printf(&tmp, "[%lu more entries not shown]\n",
                   (unsigned long)(n - 500));
    if (n == 0)
        buf_puts(&tmp, "(empty directory)\n");
    local_to_utf8(res, tmp.p, tmp.len);
    buf_free(&tmp);
    free(ents);
    free(path);
    return 0;
}

static int read_screen(buf_t *res)
{
    buf_t s;
    buf_init(&s);
    if (!plat_read_screen(&s)) {
        buf_free(&s);
        buf_puts(res, "Screen is not in a text mode (or not supported here).");
        return 1;
    }
    local_to_utf8(res, s.p, s.len);
    buf_free(&s);
    return 0;
}

/* ------------------------------------------------------------ dispatch */

static void put_preview(buf_t *out, const char *label, const jval *v)
{
    buf_t loc;
    if (!v || v->t != J_STR)
        return;
    buf_init(&loc);
    utf8_to_local(&loc, v->s, v->n);
    buf_printf(out, "  %s\n", label);
    {
        int lines = 0;
        size_t i = 0;
        while (i < loc.len && lines < 8) {
            size_t j = i;
            while (j < loc.len && loc.p[j] != '\n')
                j++;
            buf_printf(out, "  | %.*s\n", (int)(j - i > 74 ? 74 : j - i),
                       loc.p + i);
            lines++;
            i = j + 1;
        }
        if (i < loc.len)
            buf_puts(out, "  | ...\n");
    }
    buf_free(&loc);
}

int tool_reject(const char *name, const jval *in, buf_t *result)
{
#if PLAT_DOS
    const char *cmd = in && in->t == J_OBJ ? arg_str(in, "command") : NULL;
    if (strcmp(name, "run_command") == 0 && cmd && has_chain_op(cmd)) {
        buf_puts(result,
                 "Not run: COMMAND.COM has no && or ||. It would pass them "
                 "to the first program as arguments. Run each command in "
                 "its own run_command call and check its exit code, or "
                 "write a batch file with write_file and run that.");
        return 1;
    }
#else
    (void)name;
    (void)in;
    (void)result;
#endif
    return 0;
}

void tool_describe(const char *name, const jval *in, buf_t *out)
{
    const char *path = arg_str(in, "path");
    buf_t p;
    buf_init(&p);
    if (path)
        utf8_to_local(&p, path, strlen(path));
    if (strcmp(name, "run_command") == 0) {
        const char *cmd = arg_str(in, "command");
        buf_puts(out, "Run: ");
        if (cmd)
            utf8_to_local(out, cmd, strlen(cmd));
        if (json_get_bool(in, "capture_screen", 0))
            buf_puts(out, "  [+screen]");
        buf_putc(out, '\n');
    } else if (strcmp(name, "write_file") == 0) {
        jval *c = json_get(in, "content");
        buf_printf(out, "Write %s (%lu bytes)\n", p.p ? p.p : "?",
                   (unsigned long)(c && c->t == J_STR ? c->n : 0));
        put_preview(out, "content:", c);
    } else if (strcmp(name, "edit_file") == 0) {
        buf_printf(out, "Edit %s%s\n", p.p ? p.p : "?",
                   json_get_bool(in, "replace_all", 0) ? " (all)" : "");
        put_preview(out, "old:", json_get(in, "old_str"));
        put_preview(out, "new:", json_get(in, "new_str"));
    } else if (strcmp(name, "read_file") == 0) {
        buf_printf(out, "Read %s\n", p.p ? p.p : "?");
    } else if (strcmp(name, "list_dir") == 0) {
        buf_printf(out, "List %s\n", p.p ? p.p : ".");
    } else {
        buf_printf(out, "%s\n", name);
    }
    buf_free(&p);
}

int tool_run(const char *name, const jval *in, buf_t *result)
{
    if (!in || in->t != J_OBJ) {
        buf_puts(result, "Tool input was not a valid JSON object.");
        return 1;
    }
    if (strcmp(name, "run_command") == 0)
        return run_command(in, result);
    if (strcmp(name, "read_file") == 0)
        return read_file(in, result);
    if (strcmp(name, "write_file") == 0)
        return write_file(in, result);
    if (strcmp(name, "edit_file") == 0)
        return edit_file(in, result);
    if (strcmp(name, "list_dir") == 0)
        return list_dir(in, result);
    if (strcmp(name, "read_screen") == 0)
        return read_screen(result);
    buf_printf(result, "Unknown tool: %s", name);
    return 1;
}
