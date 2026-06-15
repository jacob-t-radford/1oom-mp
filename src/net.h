#ifndef INC_1OOM_NET_H
#define INC_1OOM_NET_H

/* 1oom-mp: minimal portable TCP transport with length-prefixed message framing.
   No engine dependencies, so it can be unit-tested standalone.
   Wire frame: [u32 length][payload]  (length is payload size, network byte order).
   Sockets are non-blocking; net_recv assembles whole frames from a per-conn buffer
   so a single server thread can multiplex many clients without blocking. */

#include <stdint.h>
#include <stdbool.h>

/* Max framed message. GAME_DATA save blobs are ~60KB (108 stars) up to a few
   hundred KB at 512 stars, so allow generous headroom. */
#define NET_MSG_MAX (1024 * 1024)

typedef struct net_conn_s net_conn_t;
typedef struct net_listener_s net_listener_t;

/* one-time init/shutdown (no-op on POSIX; needed for Winsock later) */
int net_init(void);
void net_shutdown(void);

/* ---- server ---- */
net_listener_t *net_listen(uint16_t port);
/* Non-blocking: returns a new connection if one is pending, else NULL. */
net_conn_t *net_accept(net_listener_t *l);
void net_listener_close(net_listener_t *l);

/* ---- client ---- */
/* Blocking connect (resolves host). Returns NULL on failure. */
net_conn_t *net_connect(const char *host, uint16_t port);

/* ---- framed messages ---- */
/* Send one whole message. Blocks until sent (handles partial writes). 0 ok, -1 err. */
int net_send(net_conn_t *c, const void *data, uint32_t len);
/* best-effort send: drops the frame instead of blocking if the send buffer is full (for spectate) */
int net_send_besteffort(net_conn_t *c, const void *data, uint32_t len);
/* Try to receive one whole message into buf.
     1  = a complete message is in buf, its size stored in *out_len
     0  = no complete message available yet (call again later)
    -1  = connection closed or error */
int net_recv(net_conn_t *c, void *buf, uint32_t bufsize, uint32_t *out_len);

void net_conn_close(net_conn_t *c);
bool net_conn_is_open(const net_conn_t *c);
const char *net_conn_addr(const net_conn_t *c); /* peer "ip:port" string */

#endif /* INC_1OOM_NET_H */
