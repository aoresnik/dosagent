#include "charset.h"

#include <stdio.h>

int charset_cp437 = 0;

/* Unicode code points for CP437 bytes 0x80-0xFF. */
static const unsigned short cp437_hi[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

static void put_cp(buf_t *out, unsigned long cp)
{
    char c[4];
    if (cp < 0x80) {
        c[0] = (char)cp;
        buf_append(out, c, 1);
    } else if (cp < 0x800) {
        c[0] = (char)(0xC0 | (cp >> 6));
        c[1] = (char)(0x80 | (cp & 0x3F));
        buf_append(out, c, 2);
    } else if (cp < 0x10000) {
        c[0] = (char)(0xE0 | (cp >> 12));
        c[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        c[2] = (char)(0x80 | (cp & 0x3F));
        buf_append(out, c, 3);
    } else {
        c[0] = (char)(0xF0 | (cp >> 18));
        c[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        c[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        c[3] = (char)(0x80 | (cp & 0x3F));
        buf_append(out, c, 4);
    }
}

/* Decode one UTF-8 sequence. Returns its length, or 0 if invalid. */
static size_t utf8_decode(const unsigned char *s, size_t n, unsigned long *cp)
{
    size_t len, i;
    unsigned long v;
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    if ((s[0] & 0xE0) == 0xC0) { len = 2; v = s[0] & 0x1F; }
    else if ((s[0] & 0xF0) == 0xE0) { len = 3; v = s[0] & 0x0F; }
    else if ((s[0] & 0xF8) == 0xF0) { len = 4; v = s[0] & 0x07; }
    else return 0;
    if (len > n)
        return 0;
    for (i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80)
            return 0;
        v = (v << 6) | (s[i] & 0x3F);
    }
    if ((len == 2 && v < 0x80) || (len == 3 && v < 0x800) ||
        (len == 4 && (v < 0x10000 || v > 0x10FFFF)) ||
        (v >= 0xD800 && v < 0xE000))
        return 0;
    *cp = v;
    return len;
}

void local_to_utf8(buf_t *out, const char *in, size_t n)
{
    const unsigned char *s = (const unsigned char *)in;
    size_t i = 0;
    buf_reserve(out, n + n / 4);
    while (i < n) {
        if (s[i] < 0x80) {
            size_t run = i;
            while (run < n && s[run] < 0x80)
                run++;
            buf_append(out, s + i, run - i);
            i = run;
        } else if (charset_cp437) {
            put_cp(out, cp437_hi[s[i] - 0x80]);
            i++;
        } else {
            unsigned long cp;
            size_t len = utf8_decode(s + i, n - i, &cp);
            if (len) {
                buf_append(out, s + i, len);
                i += len;
            } else {
                put_cp(out, 0xFFFD);
                i++;
            }
        }
    }
}

static int cp437_from_unicode(unsigned long cp)
{
    int i;
    for (i = 0; i < 128; i++)
        if (cp437_hi[i] == cp)
            return 0x80 + i;
    /* A few common substitutions. */
    switch (cp) {
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2013: case 0x2014: return '-';
    case 0x2026: return -2; /* "..." */
    case 0x2192: return 0x1A; /* arrow */
    case 0x2190: return 0x1B;
    case 0x2022: return 0x07;
    case 0x2713: case 0x2714: return 0xFB; /* check mark -> sqrt sign */
    }
    return '?';
}

void utf8_to_local(buf_t *out, const char *in, size_t n)
{
    const unsigned char *s = (const unsigned char *)in;
    size_t i = 0;
    if (!charset_cp437) {
        buf_append(out, in, n);
        return;
    }
    while (i < n) {
        unsigned long cp;
        size_t len = utf8_decode(s + i, n - i, &cp);
        if (!len) {
            buf_putc(out, '?');
            i++;
            continue;
        }
        i += len;
        if (cp < 0x80) {
            buf_putc(out, (char)cp);
        } else {
            int c = cp437_from_unicode(cp);
            if (c == -2)
                buf_puts(out, "...");
            else
                buf_putc(out, (char)c);
        }
    }
}

void print_utf8(const char *s, size_t n)
{
    buf_t b;
    buf_init(&b);
    utf8_to_local(&b, s, n);
    fwrite(b.p ? b.p : "", 1, b.len, stdout);
    fflush(stdout);
    buf_free(&b);
}
