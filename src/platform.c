#include "platform.h"
#include "net.h"

#include <ctype.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t got_sigint;

static void on_sigint(int sig)
{
    (void)sig;
    got_sigint = 1;
    net_abort = 1;
    signal(SIGINT, on_sigint);
}

void plat_clear_abort(void)
{
    got_sigint = 0;
    net_abort = 0;
}

#ifdef __DJGPP__
/* ------------------------------------------------------------------ DOS */
#include <conio.h>
#include <dos.h>
#include <dpmi.h>
#include <crt0.h>
#include <go32.h>
#include <keys.h>
#include <pc.h>
#include <unistd.h>
#include <sys/farptr.h>

/* Show file names as DOS stores them (HELLO.TXT, not hello.txt). */
int _crt0_startup_flags = _CRT0_FLAG_PRESERVE_FILENAME_CASE;

void plat_init(void)
{
    /* With TZ unset, DJGPP's time zone code reads uninitialized memory and
     * time() can come back decades off (seen when the same program runs
     * twice in a row), which breaks certificate checks. Default to treating
     * the DOS clock as UTC; set TZ yourself if it runs on local time. */
    if (!getenv("TZ"))
        putenv("TZ=UTC0");
    signal(SIGINT, on_sigint);
}

char *plat_getcwd(char *out, size_t n)
{
    char *p;

    if (!getcwd(out, n))
        return NULL;
    if (out[0] && out[1] == ':')
        out[0] = toupper((unsigned char)out[0]);
    for (p = out; *p; p++)
        if (*p == '/')
            *p = '\\';
    return out;
}

void plat_describe(buf_t *out)
{
    unsigned short cols = _farpeekw(_dos_ds, 0x44A);
    unsigned char rows = _farpeekb(_dos_ds, 0x484) + 1;
    buf_printf(out, "MS-DOS compatible OS reporting version %d.%02d (%s), "
                    "%ux%u text screen",
               _osmajor, _osminor, _os_flavor, cols, rows ? rows : 25);
}

int plat_poll_abort(void)
{
    while (kbhit()) {
        int k = getkey();
        if (k == 27)
            got_sigint = 1;
    }
    return got_sigint;
}

void plat_entropy(void)
{
    struct {
        uclock_t uc;
        time_t t;
        unsigned long ticks;
        unsigned char pit[16];
    } e;
    int i;

    e.uc = uclock();
    e.t = time(NULL);
    e.ticks = _farpeekl(_dos_ds, 0x46C);
    /* Low byte of PIT channel 0, sampled between busy loops. */
    for (i = 0; i < 16; i++) {
        volatile int spin = (i * 37) & 255;
        while (spin--)
            ;
        outportb(0x43, 0x00);
        e.pit[i] = inportb(0x40);
        (void)inportb(0x40);
    }
    tls_add_entropy(&e, sizeof e);
}

/* ------------------------------------------------------- line editor */

#define HIST_MAX 32
static char *hist[HIST_MAX];
static int hist_n;

void plat_add_history(const char *line)
{
    if (!line[0] || (hist_n > 0 && strcmp(hist[hist_n - 1], line) == 0))
        return;
    if (hist_n == HIST_MAX) {
        free(hist[0]);
        memmove(hist, hist + 1, (HIST_MAX - 1) * sizeof hist[0]);
        hist_n--;
    }
    hist[hist_n] = strdup(line);
    if (hist[hist_n])
        hist_n++;
}

/* The text being edited is drawn straight into video memory, starting at
 * the cursor position where input began, wrapping at the right edge. The
 * screen is scrolled with newlines through DOS when the text would run
 * past the bottom row, which moves the start up. */
typedef struct {
    char *s;
    int len, cur, max;
    int row, col;   /* where the text starts, 0-based */
    int rows, cols;
    int attr;
} edit_t;

static void edit_cursor(edit_t *e)
{
    int off = e->col + e->cur;
    ScreenSetCursor(e->row + off / e->cols, off % e->cols);
}

/* Redraws from position from to the end, blanking up to old_len. */
static void edit_draw(edit_t *e, int from, int old_len)
{
    int end = e->len > old_len ? e->len : old_len;
    int last_row = e->row + (e->col + e->len) / e->cols;
    int i;
    if (last_row >= e->rows) {
        int k = last_row - e->rows + 1;
        ScreenSetCursor(e->rows - 1, 0);
        while (k-- > 0) {
            putchar('\n');
            e->row--;
        }
        fflush(stdout);
    }
    for (i = from; i < end; i++) {
        int off = e->col + i;
        int row = e->row + off / e->cols;
        if (row >= 0 && row < e->rows)
            ScreenPutChar(i < e->len ? (unsigned char)e->s[i] : ' ', e->attr,
                          off % e->cols, row);
    }
    edit_cursor(e);
}

static void edit_set(edit_t *e, const char *text)
{
    int old_len = e->len;
    int n = (int)strlen(text);
    if (n > e->max)
        n = e->max;
    memcpy(e->s, text, n);
    e->len = e->cur = n;
    edit_draw(e, 0, old_len);
}

static void edit_delete(edit_t *e, int at, int count)
{
    int old_len = e->len;
    memmove(e->s + at, e->s + at + count, e->len - at - count);
    e->len -= count;
    e->cur = at;
    edit_draw(e, at, old_len);
}

static int word_left(const edit_t *e)
{
    int p = e->cur;
    while (p > 0 && e->s[p - 1] == ' ')
        p--;
    while (p > 0 && e->s[p - 1] != ' ')
        p--;
    return p;
}

static int word_right(const edit_t *e)
{
    int p = e->cur;
    while (p < e->len && e->s[p] != ' ')
        p++;
    while (p < e->len && e->s[p] == ' ')
        p++;
    return p;
}

/* Keys: arrows, Home/End, Ctrl-arrows by word, Del, Backspace,
 * Ctrl-Backspace (word), Ctrl-K (to end), Ins (overwrite), Up/Down
 * (history), Esc (clear), Enter, Ctrl-Z on an empty line (EOF). */
int plat_read_line(char *out, size_t n)
{
    edit_t e;
    int overwrite = 0, hi = hist_n, last_k = 0;
    uclock_t last_uc = 0;
    char *draft = NULL;
    int ch;

    fflush(stdout);
    e.s = out;
    e.len = e.cur = 0;
    e.rows = ScreenRows();
    e.cols = ScreenCols();
    ScreenGetCursor(&e.row, &e.col);
    /* Keep the whole text on screen. */
    e.max = (int)n - 1;
    if (e.max > (e.rows - 1) * e.cols - e.col)
        e.max = (e.rows - 1) * e.cols - e.col;
    e.attr = 0x07;
    if (e.col > 0)
        ScreenGetChar(&ch, &e.attr, e.col - 1, e.row);

    for (;;) {
        int k = getkey();
        uclock_t uc = uclock();
        tls_add_entropy(&uc, sizeof uc);
        tls_add_entropy(&k, sizeof k);
        /* QEMU's BIOS queues cursor-block keys twice, 2-6 ms apart. Key
         * repeat is never faster than 33 ms, so drop such duplicates. */
        if (k >= 0x100 && k == last_k &&
            uc - last_uc < UCLOCKS_PER_SEC / 50) {
            last_uc = uc;
            continue;
        }
        last_k = k;
        last_uc = uc;
        switch (k) {
        case '\r':
        case '\n':
            e.cur = e.len;
            edit_cursor(&e);
            putchar('\n');
            out[e.len] = 0;
            free(draft);
            _setcursortype(_NORMALCURSOR);
            return e.len;
        case 26: /* Ctrl-Z */
            if (e.len == 0) {
                putchar('\n');
                out[0] = 0;
                free(draft);
                _setcursortype(_NORMALCURSOR);
                return -1;
            }
            break;
        case K_Left:
        case K_ELeft:
            if (e.cur > 0)
                e.cur--;
            break;
        case K_Right:
        case K_ERight:
            if (e.cur < e.len)
                e.cur++;
            break;
        case K_Home:
        case K_EHome:
            e.cur = 0;
            break;
        case K_End:
        case K_EEnd:
            e.cur = e.len;
            break;
        case K_Control_Left:
        case K_Control_ELeft:
            e.cur = word_left(&e);
            break;
        case K_Control_Right:
        case K_Control_ERight:
            e.cur = word_right(&e);
            break;
        case K_BackSpace:
            if (e.cur > 0)
                edit_delete(&e, e.cur - 1, 1);
            break;
        case K_Control_Backspace:
            if (e.cur > 0) {
                int p = word_left(&e);
                edit_delete(&e, p, e.cur - p);
            }
            break;
        case K_Delete:
        case K_EDelete:
            if (e.cur < e.len) {
                int p = e.cur;
                edit_delete(&e, p, 1);
            }
            break;
        case K_Control_K:
            if (e.cur < e.len) {
                int old_len = e.len;
                e.len = e.cur;
                edit_draw(&e, e.cur, old_len);
            }
            break;
        case K_Insert:
        case K_EInsert:
            overwrite = !overwrite;
            _setcursortype(overwrite ? _SOLIDCURSOR : _NORMALCURSOR);
            break;
        case 27: /* Esc */
            edit_set(&e, "");
            break;
        case K_Up:
        case K_EUp:
            if (hi > 0) {
                if (hi == hist_n) {
                    free(draft);
                    out[e.len] = 0;
                    draft = strdup(out);
                }
                edit_set(&e, hist[--hi]);
            }
            break;
        case K_Down:
        case K_EDown:
            if (hi < hist_n) {
                hi++;
                edit_set(&e, hi < hist_n ? hist[hi] : draft ? draft : "");
            }
            break;
        default:
            if (k < 32 || k > 255)
                break;
            if (overwrite && e.cur < e.len) {
                e.s[e.cur] = (char)k;
                edit_draw(&e, e.cur, e.len);
                e.cur++;
            } else if (e.len < e.max) {
                int old_len = e.len;
                memmove(e.s + e.cur + 1, e.s + e.cur, e.len - e.cur);
                e.s[e.cur] = (char)k;
                e.len++;
                edit_draw(&e, e.cur, old_len);
                e.cur++;
            }
            break;
        }
        edit_cursor(&e);
    }
}

int plat_getkey(void)
{
    int k = getkey();
    uclock_t uc = uclock();
    tls_add_entropy(&uc, sizeof uc);
    return k;
}

int plat_read_screen(buf_t *out)
{
    unsigned char mode = _farpeekb(_dos_ds, 0x449);
    unsigned short cols = _farpeekw(_dos_ds, 0x44A);
    unsigned char rows = _farpeekb(_dos_ds, 0x484) + 1;
    unsigned long base;
    unsigned char line[264];
    unsigned r, c;

    if (mode != 2 && mode != 3 && mode != 7)
        return 0; /* graphics mode */
    if (rows < 2 || rows > 60)
        rows = 25;
    if (cols == 0 || cols > 132)
        cols = 80;
    base = (mode == 7 ? 0xB0000UL : 0xB8000UL) + _farpeekw(_dos_ds, 0x44E);
    for (r = 0; r < rows; r++) {
        int end = 0;
        for (c = 0; c < cols; c++) {
            unsigned char ch = _farpeekb(_dos_ds, base + (r * cols + c) * 2);
            line[c] = ch ? ch : ' ';
            if (line[c] != ' ')
                end = (int)c + 1;
        }
        buf_append(out, line, (size_t)end);
        buf_putc(out, '\n');
    }
    return 1;
}

void plat_sleep_ms(unsigned ms)
{
    delay(ms);
}

#else
/* ------------------------------------------------------------ POSIX host */
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>

void plat_init(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL); /* no SA_RESTART: interrupt blocking reads */
}

char *plat_getcwd(char *out, size_t n)
{
    return getcwd(out, n);
}

void plat_describe(buf_t *out)
{
    struct utsname u;
    if (uname(&u) == 0)
        buf_printf(out, "%s %s (host test build of the DOS harness)",
                   u.sysname, u.release);
    else
        buf_puts(out, "POSIX host (test build)");
}

int plat_poll_abort(void)
{
    return got_sigint;
}

void plat_entropy(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    tls_add_entropy(&tv, sizeof tv);
}

void plat_add_history(const char *line)
{
    (void)line;
}

int plat_read_line(char *out, size_t n)
{
    size_t len;
    if (!fgets(out, (int)n, stdin))
        return -1;
    len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        out[--len] = 0;
    return (int)len;
}

int plat_getkey(void)
{
    char line[64];
    if (!fgets(line, sizeof line, stdin))
        return -1;
    return (unsigned char)line[0];
}

int plat_read_screen(buf_t *out)
{
    (void)out;
    return 0;
}

void plat_sleep_ms(unsigned ms)
{
    usleep(ms * 1000u);
}
#endif
