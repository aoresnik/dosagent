/* Byte-stream connections: plain TCP (BSD sockets on the host, Watt-32 on
 * DOS) optionally wrapped in TLS via BearSSL. */
#ifndef NET_H
#define NET_H

#include <stddef.h>

typedef struct conn conn_t;

/* One-time stack initialisation (Watt-32 sock_init on DOS). 0 on success. */
int net_init(void);

/* Give the packet driver up while another program runs, and take it back
 * after. Watt-32 receives all packet types, so while it holds the driver a
 * child's own TCP/IP stack (mTCP PING, HTGET) gets no packets. No-ops on
 * the host. */
void net_release_driver(void);
void net_reclaim_driver(void);

/* Set from a SIGINT handler / keyboard poll; aborts blocking I/O. */
extern volatile int net_abort;
/* Set when the last conn_open failed in a way retrying won't fix (bad
 * certificate, wrong clock, no CA file). */
extern int net_fatal;

/* use_tls: wrap in TLS and verify the server against the loaded trust
 * anchors. Returns NULL and prints a message on failure. */
conn_t *conn_open(const char *host, int port, int use_tls);
/* Both return bytes transferred, 0 on orderly EOF (read only), -1 on error. */
int conn_write(conn_t *c, const void *data, size_t n);
int conn_read(conn_t *c, void *data, size_t n);
int conn_flush(conn_t *c);
void conn_close(conn_t *c);

/* TLS setup. */
int tls_load_ca_file(const char *path); /* returns number of anchors */
/* Mix bytes into the entropy pool (keystroke timings etc.). */
void tls_add_entropy(const void *data, size_t n);
/* Persisted seed so entropy accumulates across runs. */
void tls_load_seed(const char *path);
void tls_save_seed(const char *path);
/* 0 = check certificate dates against the clock; otherwise days since
 * 0000-01-01 to use instead (lets a machine with a dead CMOS clock work). */
void tls_set_fixed_date(long days);

#endif
