/* TCP connections and the BearSSL TLS layer. */
#include "net.h"
#include "buf.h"
#include "platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __DJGPP__
#include <tcp.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#define net_closesocket close_s
#define net_select select_s
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define net_closesocket close
#define net_select select
#endif

#include "bearssl.h"

volatile int net_abort;
int net_fatal;

struct conn {
    int fd;
    int tls;
    br_sslio_context ioc;
};

/* ---------------------------------------------------------------- TLS state
 * One TLS client context for the whole program: keeping it alive between
 * connections lets BearSSL resume the previous session, which skips the
 * expensive key exchange on the next connection (a big deal on a 386). */
static br_ssl_client_context cc;
static br_x509_minimal_context xc;
static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
static br_x509_trust_anchor *anchors;
static size_t n_anchors, cap_anchors;
static int tls_ready;
static char last_host[256];
static long fixed_days;
/* The time certificates are checked against, and the validity range of the
 * certificate that failed the check (for the error message). */
static uint32_t now_days, now_secs;
static uint32_t bad_from, bad_until;
static int bad_time;
static br_sha256_context pool;
static int pool_ready;

static void pool_init(void)
{
    if (!pool_ready) {
        br_sha256_init(&pool);
        pool_ready = 1;
    }
}

void tls_add_entropy(const void *data, size_t n)
{
    pool_init();
    br_sha256_update(&pool, data, n);
}

static void pool_digest(const char *label, unsigned char out[32])
{
    br_sha256_context tmp;
    pool_init();
    tmp = pool;
    br_sha256_update(&tmp, label, strlen(label));
    br_sha256_out(&tmp, out);
    /* Ratchet the pool so successive digests differ. */
    br_sha256_update(&pool, out, 32);
}

void tls_load_seed(const char *path)
{
    unsigned char seed[64];
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    n = fread(seed, 1, sizeof seed, f);
    fclose(f);
    tls_add_entropy(seed, n);
}

void tls_save_seed(const char *path)
{
    unsigned char seed[32];
    FILE *f;
    plat_entropy();
    pool_digest("seed-file", seed);
    f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(seed, 1, sizeof seed, f);
    fclose(f);
}

void tls_set_fixed_date(long days)
{
    fixed_days = days;
}

static void dn_append(void *ctx, const void *data, size_t len)
{
    buf_append((buf_t *)ctx, data, len);
}

static unsigned char *memdup(const void *p, size_t n)
{
    unsigned char *d = xmalloc(n);
    memcpy(d, p, n);
    return d;
}

/* Turn one DER certificate into a trust anchor (its key and subject DN). */
static void add_anchor_der(const unsigned char *der, size_t len)
{
    br_x509_decoder_context dc;
    br_x509_pkey *pk;
    br_x509_trust_anchor *ta;
    buf_t dn;

    buf_init(&dn);
    br_x509_decoder_init(&dc, dn_append, &dn);
    br_x509_decoder_push(&dc, der, len);
    pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || br_x509_decoder_last_error(&dc) != 0) {
        buf_free(&dn);
        return;
    }
    if (n_anchors == cap_anchors) {
        cap_anchors = cap_anchors ? cap_anchors * 2 : 64;
        anchors = xrealloc(anchors, cap_anchors * sizeof *anchors);
    }
    ta = &anchors[n_anchors];
    memset(ta, 0, sizeof *ta);
    ta->dn.len = dn.len;
    ta->dn.data = (unsigned char *)buf_take(&dn);
    ta->flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
    switch (pk->key_type) {
    case BR_KEYTYPE_RSA:
        ta->pkey.key_type = BR_KEYTYPE_RSA;
        ta->pkey.key.rsa.n = memdup(pk->key.rsa.n, pk->key.rsa.nlen);
        ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta->pkey.key.rsa.e = memdup(pk->key.rsa.e, pk->key.rsa.elen);
        ta->pkey.key.rsa.elen = pk->key.rsa.elen;
        break;
    case BR_KEYTYPE_EC:
        ta->pkey.key_type = BR_KEYTYPE_EC;
        ta->pkey.key.ec.curve = pk->key.ec.curve;
        ta->pkey.key.ec.q = memdup(pk->key.ec.q, pk->key.ec.qlen);
        ta->pkey.key.ec.qlen = pk->key.ec.qlen;
        break;
    default:
        free(ta->dn.data);
        return;
    }
    n_anchors++;
}

int tls_load_ca_file(const char *path)
{
    br_pem_decoder_context pc;
    FILE *f;
    buf_t der;
    unsigned char chunk[512];
    size_t got;
    int in_cert = 0;

    f = fopen(path, "rb");
    if (!f)
        return -1;
    buf_init(&der);
    br_pem_decoder_init(&pc);
    for (;;) {
        const unsigned char *p = chunk;
        got = fread(chunk, 1, sizeof chunk, f);
        if (got == 0) {
            /* A final newline makes the decoder notice a trailing END line. */
            chunk[0] = '\n';
            got = 1;
        }
        while (got > 0) {
            size_t used = br_pem_decoder_push(&pc, p, got);
            p += used;
            got -= used;
            switch (br_pem_decoder_event(&pc)) {
            case BR_PEM_BEGIN_OBJ: {
                const char *name = br_pem_decoder_name(&pc);
                in_cert = strcmp(name, "CERTIFICATE") == 0 ||
                          strcmp(name, "X509 CERTIFICATE") == 0;
                buf_clear(&der);
                br_pem_decoder_setdest(&pc, in_cert ? dn_append : NULL,
                                       in_cert ? &der : NULL);
                break;
            }
            case BR_PEM_END_OBJ:
                if (in_cert)
                    add_anchor_der((unsigned char *)der.p, der.len);
                in_cert = 0;
                break;
            case BR_PEM_ERROR:
                fprintf(stderr, "warning: PEM decode error in %s\n", path);
                br_pem_decoder_init(&pc);
                in_cert = 0;
                break;
            }
        }
        if (feof(f))
            break;
    }
    fclose(f);
    buf_free(&der);
    return (int)n_anchors;
}

static const char *tls_error_name(int err)
{
    switch (err) {
    case BR_ERR_X509_NOT_TRUSTED: return "certificate not trusted (check CA file)";
    case BR_ERR_X509_EXPIRED: return "certificate expired or not yet valid";
    case BR_ERR_X509_BAD_SERVER_NAME: return "certificate does not match host name";
    case BR_ERR_NO_RANDOM: return "no entropy for random generator";
    case BR_ERR_BAD_VERSION: return "server rejected TLS version";
    case BR_ERR_BAD_CIPHER_SUITE: return "no common cipher suite";
    case BR_ERR_IO: return "I/O error";
    default: return NULL;
    }
}

/* Days since 0000-01-01 (BearSSL's epoch) as YYYY-MM-DD. */
static void fmt_day(char *out, size_t n, uint32_t day)
{
    long z = (long)day - 719528L + 719468L; /* days since 0000-03-01 */
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1;
    long m = mp < 10 ? mp + 3 : mp - 9;
    long y = yoe + era * 400 + (m <= 2);
    snprintf(out, n, "%04ld-%02ld-%02ld", y, m, d);
}

/* Same rule as BearSSL's built-in check, but remembers what failed. */
static int check_time(void *ctx, uint32_t nb_days, uint32_t nb_secs,
                      uint32_t na_days, uint32_t na_secs)
{
    int r = 0;
    if (now_days < nb_days || (now_days == nb_days && now_secs < nb_secs))
        r = -1;
    else if (now_days > na_days || (now_days == na_days && now_secs > na_secs))
        r = 1;
    if (r) {
        bad_time = 1;
        bad_from = nb_days;
        bad_until = na_days;
    }
    return r;
}

static void report_tls_error(void)
{
    int err = br_ssl_engine_last_error(&cc.eng);
    const char *name;
    if (err == 0)
        return;
    if (err > BR_ERR_X509_OK && err < BR_ERR_X509_OK + 32)
        net_fatal = 1;
    if (err == BR_ERR_X509_EXPIRED && bad_time) {
        char from[48], until[48], now[48];
        fmt_day(from, sizeof from, bad_from);
        fmt_day(until, sizeof until, bad_until);
        fmt_day(now, sizeof now, now_days);
        fprintf(stderr, "TLS error: certificate is valid %s to %s, but the "
                        "date is %s.\nFix the clock or set tls_date.\n",
                from, until, now);
        return;
    }
    name = tls_error_name(err);
    if (name)
        fprintf(stderr, "TLS error: %s\n", name);
    else
        fprintf(stderr, "TLS error code %d\n", err);
}

/* ---------------------------------------------------------------- sockets */

int net_init(void)
{
#ifdef __DJGPP__
    int rc = sock_init();
    if (rc != 0) {
        fprintf(stderr, "Watt-32 init failed (%d). Is the packet driver loaded "
                        "and WATTCP.CFG set up?\n", rc);
        return -1;
    }
#endif
    return 0;
}

void net_release_driver(void)
{
#ifdef __DJGPP__
    _eth_release();
#endif
}

void net_reclaim_driver(void)
{
#ifdef __DJGPP__
    if (_eth_init() != 0)
        fprintf(stderr, "Cannot reopen the packet driver. If the command "
                        "left it in use, the network is down until "
                        "restart.\n");
#endif
}

/* Wait until fd is readable, polling the abort flag every 200 ms.
 * Returns 1 when readable, -1 on abort or error. */
static int wait_readable(int fd)
{
    for (;;) {
        fd_set rs;
        struct timeval tv;
        int rc;
        if (plat_poll_abort())
            net_abort = 1;
        if (net_abort)
            return -1;
        FD_ZERO(&rs);
        FD_SET(fd, &rs);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        rc = net_select(fd + 1, &rs, NULL, NULL, &tv);
        if (rc > 0)
            return 1;
        if (rc < 0 && errno != EINTR)
            return -1;
    }
}

static int raw_read(int fd, void *data, size_t n)
{
    int rc;
    if (wait_readable(fd) < 0)
        return -1;
    rc = (int)recv(fd, data, n, 0);
    return rc;
}

static int raw_write(int fd, const void *data, size_t n)
{
    if (net_abort)
        return -1;
    return (int)send(fd, data, n, 0);
}

static int sock_read_cb(void *ctx, unsigned char *buf, size_t len)
{
    int rc = raw_read(*(int *)ctx, buf, len);
    return rc > 0 ? rc : -1;
}

static int sock_write_cb(void *ctx, const unsigned char *buf, size_t len)
{
    int rc = raw_write(*(int *)ctx, buf, len);
    return rc > 0 ? rc : -1;
}

static int tcp_connect(const char *host, int port)
{
    struct hostent *he;
    struct sockaddr_in sa;
    int fd;

    he = gethostbyname(host);
    if (!he || he->h_addrtype != AF_INET) {
        fprintf(stderr, "Cannot resolve %s\n", host);
        return -1;
    }
    fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket() failed\n");
        return -1;
    }
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        fprintf(stderr, "Cannot connect to %s:%d\n", host, port);
        net_closesocket(fd);
        return -1;
    }
    return fd;
}

static int tls_setup(const char *host)
{
    unsigned char seed[32];
    time_t now;
    long days, secs;

    if (!tls_ready) {
        if (n_anchors == 0) {
            fprintf(stderr, "No trusted CA certificates loaded (cacert=...)\n");
            net_fatal = 1;
            return -1;
        }
        br_ssl_client_init_full(&cc, &xc, anchors, n_anchors);
        br_ssl_engine_set_buffer(&cc.eng, iobuf, sizeof iobuf, 1);
        tls_ready = 1;
    }
    plat_entropy();
    pool_digest("tls-seed", seed);
    br_ssl_engine_inject_entropy(&cc.eng, seed, sizeof seed);

    now = time(NULL);
    days = (long)(now / 86400) + 719528L; /* 1970-01-01 is day 719528 */
    secs = (long)(now % 86400);
    if (fixed_days > 0)
        days = fixed_days;
    now_days = (uint32_t)days;
    now_secs = (uint32_t)secs;
    bad_time = 0;
    br_x509_minimal_set_time_callback(&xc, NULL, check_time);

    /* Resume the previous session when talking to the same host. */
    if (!br_ssl_client_reset(&cc, host, strcmp(host, last_host) == 0)) {
        report_tls_error();
        return -1;
    }
    snprintf(last_host, sizeof last_host, "%s", host);
    return 0;
}

conn_t *conn_open(const char *host, int port, int use_tls)
{
    conn_t *c;
    int fd;

    net_abort = 0;
    net_fatal = 0;
    fd = tcp_connect(host, port);
    if (fd < 0)
        return NULL;
    c = xmalloc(sizeof *c);
    memset(c, 0, sizeof *c);
    c->fd = fd;
    c->tls = use_tls;
    if (use_tls) {
        if (tls_setup(host) < 0) {
            net_closesocket(fd);
            free(c);
            return NULL;
        }
        br_sslio_init(&c->ioc, &cc.eng, sock_read_cb, &c->fd, sock_write_cb,
                      &c->fd);
    }
    return c;
}

int conn_write(conn_t *c, const void *data, size_t n)
{
    if (c->tls) {
        if (br_sslio_write_all(&c->ioc, data, n) < 0) {
            if (!net_abort)
                report_tls_error();
            return -1;
        }
        return (int)n;
    } else {
        const char *p = data;
        size_t left = n;
        while (left > 0) {
            int rc = raw_write(c->fd, p, left);
            if (rc <= 0)
                return -1;
            p += rc;
            left -= (size_t)rc;
        }
        return (int)n;
    }
}

int conn_flush(conn_t *c)
{
    if (c->tls && br_sslio_flush(&c->ioc) < 0) {
        if (!net_abort)
            report_tls_error();
        return -1;
    }
    return 0;
}

int conn_read(conn_t *c, void *data, size_t n)
{
    if (c->tls) {
        int rc = br_sslio_read(&c->ioc, data, n);
        if (rc < 0) {
            /* A closed engine with no error is an orderly EOF. */
            if (br_ssl_engine_current_state(&cc.eng) == BR_SSL_CLOSED &&
                br_ssl_engine_last_error(&cc.eng) == 0)
                return 0;
            if (!net_abort)
                report_tls_error();
            return -1;
        }
        return rc;
    }
    return raw_read(c->fd, data, n);
}

void conn_close(conn_t *c)
{
    if (!c)
        return;
    net_closesocket(c->fd);
    free(c);
}
