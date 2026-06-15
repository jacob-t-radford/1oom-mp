/* standalone loopback test for src/net.c — no engine deps.
   build: clang -I src tools/test_net.c src/net.c -o /tmp/test_net && /tmp/test_net */
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define PORT 23456
#define NMSG 4

static uint32_t msg_sizes[NMSG] = { 5, 1000, 200000, 1 };

static void fill(uint8_t *b, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; ++i) { b[i] = (uint8_t)((i * 31 + seed) & 0xff); }
}

static int run_client(void) {
    usleep(200000); /* let server bind/listen */
    net_conn_t *c = net_connect("127.0.0.1", PORT);
    if (!c) { fprintf(stderr, "CLIENT: connect failed\n"); return 2; }
    uint8_t *snd = malloc(NET_MSG_MAX), *rcv = malloc(NET_MSG_MAX);
    int rc = 0;
    for (int m = 0; m < NMSG; ++m) {
        uint32_t n = msg_sizes[m];
        fill(snd, n, m + 1);
        if (net_send(c, snd, n) != 0) { fprintf(stderr, "CLIENT: send %d failed\n", m); rc = 3; break; }
        /* wait for echo */
        uint32_t got = 0; int r;
        for (;;) {
            r = net_recv(c, rcv, NET_MSG_MAX, &got);
            if (r == 1) break;
            if (r < 0) { fprintf(stderr, "CLIENT: recv %d closed\n", m); rc = 4; break; }
            usleep(1000);
        }
        if (rc) break;
        if (got != n || memcmp(snd, rcv, n) != 0) {
            fprintf(stderr, "CLIENT: echo %d MISMATCH (sent %u got %u)\n", m, n, got);
            rc = 5; break;
        }
        printf("CLIENT: msg %d ok (%u bytes round-tripped)\n", m, n);
    }
    free(snd); free(rcv);
    net_conn_close(c);
    return rc;
}

static int run_server(void) {
    net_listener_t *l = net_listen(PORT);
    if (!l) { fprintf(stderr, "SERVER: listen failed\n"); return 10; }
    net_conn_t *c = NULL;
    for (int i = 0; i < 5000 && !c; ++i) { c = net_accept(l); if (!c) usleep(1000); }
    if (!c) { fprintf(stderr, "SERVER: no client\n"); net_listener_close(l); return 11; }
    uint8_t *buf = malloc(NET_MSG_MAX);
    int rc = 0;
    for (int m = 0; m < NMSG; ++m) {
        uint32_t got = 0; int r;
        for (;;) {
            r = net_recv(c, buf, NET_MSG_MAX, &got);
            if (r == 1) break;
            if (r < 0) { fprintf(stderr, "SERVER: recv %d closed\n", m); rc = 12; break; }
            usleep(1000);
        }
        if (rc) break;
        if (net_send(c, buf, got) != 0) { fprintf(stderr, "SERVER: echo %d failed\n", m); rc = 13; break; }
    }
    free(buf);
    net_conn_close(c);
    net_listener_close(l);
    return rc;
}

int main(void) {
    net_init();
    pid_t pid = fork();
    if (pid == 0) { _exit(run_client()); }
    int srv = run_server();
    int st = 0; waitpid(pid, &st, 0);
    int cli = WIFEXITED(st) ? WEXITSTATUS(st) : 99;
    net_shutdown();
    if (srv == 0 && cli == 0) { printf("\nTRANSPORT TEST: PASS\n"); return 0; }
    printf("\nTRANSPORT TEST: FAIL (server=%d client=%d)\n", srv, cli);
    return 1;
}
