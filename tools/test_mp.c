/* standalone test for src/mp.c — protocol + simultaneous-turn barrier, no engine.
   build: clang -I src tools/test_mp.c src/mp.c src/net.c -o /tmp/test_mp && /tmp/test_mp */
#include "mp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define PORT 23457
#define NCLIENTS 2
#define NTURNS 5

/* stub "game": just a turn counter + a marker payload, to prove serialize round-trips */
struct stub_s { int turn; };

static int st_serialize(void *ctx, uint8_t *buf, int buflen) {
    struct stub_s *s = ctx;
    if (buflen < 8) return -1;
    buf[0]=(uint8_t)(s->turn>>24); buf[1]=(uint8_t)(s->turn>>16); buf[2]=(uint8_t)(s->turn>>8); buf[3]=(uint8_t)s->turn;
    buf[4]='1'; buf[5]='o'; buf[6]='o'; buf[7]='m';
    return 8;
}
static int st_deserialize(void *ctx, const uint8_t *buf, int len) {
    struct stub_s *s = ctx;
    if (len < 8 || memcmp(buf+4, "1oom", 4) != 0) return -1;
    s->turn = (buf[0]<<24)|(buf[1]<<16)|(buf[2]<<8)|buf[3];
    return 0;
}
static int st_advance(void *ctx) { ((struct stub_s *)ctx)->turn++; return 0; }
static int st_turn(void *ctx) { return ((struct stub_s *)ctx)->turn; }
static int st_maxblob(void *ctx) { (void)ctx; return 1024; }

static mp_game_iface_t mk_iface(struct stub_s *s) {
    mp_game_iface_t gi;
    gi.ctx = s; gi.serialize = st_serialize; gi.deserialize = st_deserialize;
    gi.advance_turn = st_advance; gi.turn_number = st_turn; gi.max_blob_len = st_maxblob;
    gi.write_orders = NULL; gi.apply_orders = NULL; gi.on_state_loaded = NULL;
    gi.get_movement = NULL; gi.play_movement = NULL; gi.on_wait = NULL; gi.handle_decision = NULL; gi.on_spectate = NULL;
    return gi;
}

static int run_client(void) {
    usleep(250000);
    struct stub_s s = { -99 };
    mp_game_iface_t gi = mk_iface(&s);
    if (mp_client_run("127.0.0.1", PORT, NTURNS, &gi) != 0) { fprintf(stderr, "CLIENT: run failed\n"); return 2; }
    if (s.turn != NTURNS) { fprintf(stderr, "CLIENT: final turn %d != %d\n", s.turn, NTURNS); return 3; }
    printf("CLIENT[%d]: ok, ended at turn %d\n", (int)getpid(), s.turn);
    return 0;
}

int main(void) {
    pid_t c1 = fork(); if (c1 == 0) { _exit(run_client()); }
    pid_t c2 = fork(); if (c2 == 0) { _exit(run_client()); }
    struct stub_s s = { 0 };
    mp_game_iface_t gi = mk_iface(&s);
    int srv = mp_server_run(PORT, NCLIENTS, NTURNS, &gi);
    int st1=0, st2=0; waitpid(c1,&st1,0); waitpid(c2,&st2,0);
    int e1 = WIFEXITED(st1)?WEXITSTATUS(st1):99, e2 = WIFEXITED(st2)?WEXITSTATUS(st2):99;
    printf("server final turn=%d (expect %d)\n", s.turn, NTURNS);
    if (srv==0 && e1==0 && e2==0 && s.turn==NTURNS) { printf("\nMP BARRIER TEST: PASS\n"); return 0; }
    printf("\nMP BARRIER TEST: FAIL (srv=%d c1=%d c2=%d turn=%d)\n", srv, e1, e2, s.turn);
    return 1;
}
