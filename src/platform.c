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

int plat_read_line(char *out, size_t n)
{
    size_t len = 0;
    for (;;) {
        int k = getkey();
        uclock_t uc = uclock();
        tls_add_entropy(&uc, sizeof uc);
        tls_add_entropy(&k, sizeof k);
        if (k == '\r' || k == '\n') {
            putchar('\n');
            break;
        } else if (k == 8) {
            if (len > 0) {
                len--;
                fputs("\b \b", stdout);
            }
        } else if (k == 27) {
            while (len > 0) {
                len--;
                fputs("\b \b", stdout);
            }
        } else if (k == 26 && len == 0) { /* Ctrl-Z */
            putchar('\n');
            return -1;
        } else if (k >= 32 && k < 256 && len + 1 < n) {
            out[len++] = (char)k;
            putchar(k);
        }
        fflush(stdout);
    }
    out[len] = 0;
    return (int)len;
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
