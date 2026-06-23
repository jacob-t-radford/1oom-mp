/* 1oom-mp: simultaneous-turn MP protocol + server/client loops over net.c. */
#include "mp.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define MP_ERR(...) do { fprintf(stderr, "mp: " __VA_ARGS__); fflush(stderr); } while (0)
#define MP_MSG(...) do { fprintf(stdout, "mp: " __VA_ARGS__); fflush(stdout); } while (0)

/* ---- message framing: an mp message is a net frame of [u16 id][payload] ---- */

static int mp_send(net_conn_t *c, uint16_t id, const void *data, uint32_t len) {
    uint8_t *frame;
    int r;
    if (!c) { return 0; } /* 1oom-mp: a dropped player's slot is NULL -- a send to it is a no-op success, so broadcast loops skip it instead of treating it as an error and ending the game */
    frame = (uint8_t *)malloc(len + 2);
    if (!frame) { return -1; }
    frame[0] = (uint8_t)(id >> 8);
    frame[1] = (uint8_t)(id & 0xff);
    if (len && data) { memcpy(frame + 2, data, len); }
    r = net_send(c, frame, len + 2);
    free(frame);
    return r;
}

/* framed best-effort send: drops the message if the send buffer is full (for fire-and-forget spectate). */
static int mp_send_besteffort(net_conn_t *c, uint16_t id, const void *data, uint32_t len) {
    uint8_t *frame;
    if (!c) { return 0; } /* 1oom-mp: dropped slot -> no-op (see mp_send) */
    frame = (uint8_t *)malloc(len + 2);
    if (!frame) { return -1; }
    frame[0] = (uint8_t)(id >> 8);
    frame[1] = (uint8_t)(id & 0xff);
    if (len && data) { memcpy(frame + 2, data, len); }
    int r = net_send_besteffort(c, frame, len + 2);
    free(frame);
    return r;
}

/* receive one mp message into buf; on success buf holds the payload (id stripped).
   returns 1 = got (id,*datalen set), 0 = none yet, -1 = closed/error */
static int mp_recv(net_conn_t *c, uint16_t *id, uint8_t *buf, uint32_t bufsize, uint32_t *datalen) {
    uint32_t flen = 0;
    int r = net_recv(c, buf, bufsize, &flen);
    if (r != 1) { return r; }
    if (flen < 2) { *id = MP_MSG_NOP; *datalen = 0; return 1; }
    *id = (uint16_t)((buf[0] << 8) | buf[1]);
    *datalen = flen - 2;
    memmove(buf, buf + 2, *datalen);
    return 1;
}

/* block until a specific message id arrives (or error/close). timeout_secs > 0 gives up after that long
   (returns -1) so a frozen client can't hang the server forever; 0 = wait indefinitely. */
static int mp_recv_wait(net_conn_t *c, uint16_t want_id, uint8_t *buf, uint32_t bufsize, uint32_t *datalen, const mp_game_iface_t *gi, int reason, int timeout_secs) {
    time_t start = time(NULL);
    for (;;) {
        uint16_t id;
        int r = mp_recv(c, &id, buf, bufsize, datalen);
        if (r < 0) { return -1; }
        if (r == 1) {
            if (id == want_id) { return 0; }
            /* ignore other messages for now (Phase A) */
        } else {
            if ((timeout_secs > 0) && ((long)(time(NULL) - start) >= (long)timeout_secs)) { return -1; } /* 1oom-mp: timed out -> caller falls back to a default */
            if (gi && gi->on_wait) { gi->on_wait(gi->ctx, reason); }
            usleep(1000);
        }
    }
}

/* ---- interactive-decision channel: server blocks turn resolution to ask one client,
   the client answers from inside its post-submit wait loop. ---- */
#define MP_DECISION_BUF_MAX 65536
/* 1oom-mp: how long the server waits for a client's mid-resolution decision answer (combat move, bomb
   prompt, council vote, ...) before giving up and using the null-UI default, so one frozen client can't
   hang resolution -- and the whole game -- forever. Generous so a human just thinking isn't cut off. */
#define MP_DECISION_TIMEOUT_SECS 120
int (*g_mp_decision_hook)(int, int, const void *, int, void *, int) = NULL;
int (*g_mp_decision_hook_multi)(const int *, int, int, const void *, int, void *, int) = NULL;
void (*g_mp_spectate_hook)(int, const void *, int, int) = NULL;
void (*g_mp_movement_hook)(const uint8_t *, int) = NULL;
static net_conn_t **s_mp_conns = NULL;
static int s_mp_num_conns = 0;

/* ---- soft-ready client primitives (set only while an interactive planning phase runs) ---- */
void (*g_mp_cl_send_ready)(int, const uint8_t *, int) = NULL;
int (*g_mp_cl_poll)(void) = NULL;
static net_conn_t *s_cl_conn = NULL;       /* this client's connection (planning phase) */
static int s_cl_pid = 0;                   /* this client's player id */
static uint8_t *s_cl_blob = NULL;          /* shared recv buffer (borrowed from mp_client_run) */
static int s_cl_blobcap = 0;
static bool s_cl_resolving = false;        /* set when RESOLVE_START arrives -> end planning */
static bool s_cl_game_over = false;        /* set if the link dropped / game ended during planning */
static uint8_t s_cl_go_buf[256];           /* 1oom-mp: GAME_OVER ending payload (winner/type) */
static int s_cl_go_len = -1;               /* -1 = no real GAME_OVER (e.g. link drop); >=0 = play the ending */
static int s_cl_wait_kind = 0;             /* 0=none, 1=another player in combat, 2=galactic council in session (from MP_MSG_WAIT_STATUS) */

/* client: serialize+send a READY (latest orders + ready flag). Called by the UI adapter. */
static void cl_send_ready_impl(int ready, const uint8_t *orders, int olen) {
    if (!s_cl_conn) { return; }
    if (olen < 0) { olen = 0; }
    uint8_t *m = (uint8_t *)malloc(3 + (size_t)olen);
    if (!m) { return; }
    m[0] = (uint8_t)(s_cl_pid >> 8); m[1] = (uint8_t)s_cl_pid; m[2] = (uint8_t)(ready ? 1 : 0);
    if (olen) { memcpy(m + 3, orders, (size_t)olen); }
    mp_send(s_cl_conn, MP_MSG_READY, m, 3 + (uint32_t)olen);
    free(m);
}

/* ---- client live-diplomacy inbox: 0x20-0x2A messages arrive on the planning socket and are
   stashed here for the UI state machine to drain (decoupled so cl_poll never blocks on the UI). ---- */
#define MP_DIPLO_INBOX_CAP 32
#define MP_DIPLO_MSG_MAX 16
struct mp_diplo_inmsg_s { uint16_t id; uint8_t len; uint8_t data[MP_DIPLO_MSG_MAX]; };
static struct mp_diplo_inmsg_s s_cl_diplo_inbox[MP_DIPLO_INBOX_CAP];
static int s_cl_diplo_in_head = 0, s_cl_diplo_in_tail = 0;

static void cl_diplo_stash(uint16_t id, const uint8_t *data, uint32_t dl) {
    int nxt = (s_cl_diplo_in_head + 1) % MP_DIPLO_INBOX_CAP;
    if (nxt == s_cl_diplo_in_tail) { return; }           /* full: drop (all diplo msgs are user-paced) */
    struct mp_diplo_inmsg_s *m = &s_cl_diplo_inbox[s_cl_diplo_in_head];
    m->id = id; m->len = (dl > MP_DIPLO_MSG_MAX) ? MP_DIPLO_MSG_MAX : (uint8_t)dl;
    if (m->len) { memcpy(m->data, data, m->len); }
    s_cl_diplo_in_head = nxt;
}
/* UI: dequeue one diplo message -> payload length (>=0) + *id_out, or -1 if the inbox is empty. */
static int cl_diplo_recv_impl(uint16_t *id_out, uint8_t *buf, int buflen) {
    if (s_cl_diplo_in_tail == s_cl_diplo_in_head) { return -1; }
    struct mp_diplo_inmsg_s *m = &s_cl_diplo_inbox[s_cl_diplo_in_tail];
    if (id_out) { *id_out = m->id; }
    int n = m->len; if (n > buflen) { n = buflen; }
    if (n > 0) { memcpy(buf, m->data, n); }
    s_cl_diplo_in_tail = (s_cl_diplo_in_tail + 1) % MP_DIPLO_INBOX_CAP;
    return n;
}
/* UI: send one diplo message (0x20-0x2A) to the server. */
static void cl_diplo_send_impl(uint16_t id, const uint8_t *data, int len) {
    if (!s_cl_conn) { return; }
    if (len < 0) { len = 0; }
    mp_send(s_cl_conn, id, data, (uint32_t)len);
}
int (*g_mp_cl_diplo_recv)(uint16_t *, uint8_t *, int) = NULL;
void (*g_mp_cl_diplo_send)(uint16_t, const uint8_t *, int) = NULL;
void (*g_mp_cl_timer_notify)(int) = NULL;

/* client: stream this player's live plan snapshot to teammates (best-effort -- only the latest
   snapshot matters, so a dropped one self-heals on the next frame). */
static void cl_team_plan_send_impl(const void *data, int len) {
    if (!s_cl_conn) { return; }
    if (len < 0) { len = 0; }
    mp_send_besteffort(s_cl_conn, MP_MSG_TEAM_PLAN, data, (uint32_t)len);
}
void (*g_mp_cl_team_plan_send)(const void *, int) = NULL;
void (*g_mp_team_plan_recv)(const void *, int) = NULL;

/* client: send a typed chat line to the server (best-effort; the server stamps the sender + broadcasts). */
static void cl_chat_send_impl(const void *data, int len) {
    if (!s_cl_conn) { return; }
    if (len < 0) { len = 0; }
    if (len > MP_CHAT_MAX) { len = MP_CHAT_MAX; }
    mp_send_besteffort(s_cl_conn, MP_MSG_CHAT, data, (uint32_t)len);
}
void (*g_mp_cl_chat_send)(const void *, int) = NULL;
void (*g_mp_chat_recv)(const void *, int) = NULL;

/* client: service the socket once during planning. The server is silent until everyone is
   ready, then sends RESOLVE_START; returns 1 once that arrives (or the link drops), which
   tells the interactive turn UI to stop and hand off to the resolution wait loop. */
static int cl_poll_impl(void) {
    uint16_t id; uint32_t dl;
    if (s_cl_resolving || s_cl_game_over) { return 1; }
    if (!s_cl_conn || !s_cl_blob) { return 0; }
    int r = mp_recv(s_cl_conn, &id, s_cl_blob, s_cl_blobcap, &dl);
    if (r < 0) { s_cl_game_over = true; return 1; }
    if (r == 1) {
        if (id == MP_MSG_RESOLVE_START) { s_cl_resolving = true; return 1; }
        if (id == MP_MSG_GAME_OVER) { s_cl_go_len = (dl <= sizeof(s_cl_go_buf)) ? (int)dl : 0; if (s_cl_go_len > 0) { memcpy(s_cl_go_buf, s_cl_blob, (size_t)s_cl_go_len); } s_cl_game_over = true; return 1; } /* stash the ending payload */
        if (id == MP_MSG_TIMER_START) { if (g_mp_cl_timer_notify) { g_mp_cl_timer_notify((dl >= 1) ? (int)s_cl_blob[0] : 60); } return 0; }
        if (id == MP_MSG_TIMER_CANCEL) { if (g_mp_cl_timer_notify) { g_mp_cl_timer_notify(-1); } return 0; }
        if ((id >= MP_MSG_DIPLO_INVITE) && (id <= MP_MSG_DIPLO_CANCEL)) { cl_diplo_stash(id, s_cl_blob, dl); return 0; }
        if (id == MP_MSG_TEAM_STANCE) { cl_diplo_stash(id, s_cl_blob, dl); return 0; } /* 1oom-mp teams: drains through the same diplo inbox/pump */
        if ((id == MP_MSG_TEAM_PLAN) && g_mp_team_plan_recv) { g_mp_team_plan_recv(s_cl_blob, dl); return 0; }
        if ((id == MP_MSG_CHAT) && g_mp_chat_recv) { g_mp_chat_recv(s_cl_blob, dl); return 0; }
        /* nothing else is expected before resolution; ignore */
    }
    return 0;
}

/* ---- client lobby primitives (set only while the pre-game lobby is active) ---- */
int (*g_mp_cl_lobby_poll)(struct mp_lobby_s *) = NULL;
void (*g_mp_cl_lobby_set)(int, int) = NULL;
static const mp_game_iface_t *s_cl_gi = NULL;   /* for deserializing the start-of-game state */
static struct mp_lobby_s s_cl_lobby;            /* latest lobby state from the server */
static bool s_cl_lobby_started = false;         /* set once GAME_DATA arrives -> game begins */

/* client: send one lobby field change (field,value) for this player. */
static void cl_lobby_set_impl(int field, int value) {
    if (!s_cl_conn) { return; }
    uint8_t p[2] = { (uint8_t)field, (uint8_t)value };
    mp_send(s_cl_conn, MP_MSG_LOBBY_PICK, p, 2);
}

/* client: service the socket once during the lobby. Copies the latest state into *out and returns
   >0 once the game has started (initial state loaded), <0 if the link dropped, else 0. */
static int cl_lobby_poll_impl(struct mp_lobby_s *out) {
    if (s_cl_lobby_started) { if (out) { *out = s_cl_lobby; } return 1; }
    if (s_cl_game_over) { return -1; }
    if (!s_cl_conn || !s_cl_blob) { return 0; }
    uint16_t id; uint32_t dl;
    int r = mp_recv(s_cl_conn, &id, s_cl_blob, s_cl_blobcap, &dl);
    if (r < 0) { s_cl_game_over = true; return -1; }
    if (r == 1) {
        if (id == MP_MSG_LOBBY) {
            uint32_t n = (dl < sizeof(s_cl_lobby)) ? dl : (uint32_t)sizeof(s_cl_lobby);
            memcpy(&s_cl_lobby, s_cl_blob, n);
        } else if (id == MP_MSG_GAME_DATA) {
            if (!s_cl_gi || s_cl_gi->deserialize(s_cl_gi->ctx, s_cl_blob, dl) != 0) { s_cl_game_over = true; return -1; }
            if (s_cl_gi->on_state_loaded) { s_cl_gi->on_state_loaded(s_cl_gi->ctx, 1); }
            s_cl_lobby_started = true;
            if (out) { *out = s_cl_lobby; }
            return 1;
        } else if (id == MP_MSG_GAME_OVER) {
            s_cl_game_over = true; return -1;
        }
    }
    if (out) { *out = s_cl_lobby; }
    return 0;
}

static void mp_spectate_send(int player_id, const void *data, int len, int reliable) {
    if ((player_id < 0) || (player_id >= s_mp_num_conns) || !s_mp_conns[player_id] || (len < 0)) { return; }
    if (reliable) {
        /* one-shot battle animation (beam/damage/move/missile): never drop it -- losing the damage event
           is why a kill "didn't show until the next turn". Reliable paces the server to the client, which
           is what we want for a fight the player is watching. */
        mp_send(s_mp_conns[player_id], MP_MSG_SPECTATE, data, (uint32_t)len);
    } else {
        mp_send_besteffort(s_mp_conns[player_id], MP_MSG_SPECTATE, data, (uint32_t)len); /* big snapshot/council frame: drop if buffer full; the next one resyncs */
    }
}

/* server -> all clients: a battle just started (active=1) / finished (active=0). A client only waiting
   on the turn can then say "another player is in combat" instead of showing a generic wait screen. */
static void mp_wait_status_bcast(int active) {
    uint8_t b = (uint8_t)active; /* 0=clear, 1=combat, 2=council */
    for (int i = 0; i < s_mp_num_conns; ++i) {
        if (s_mp_conns[i]) { mp_send(s_mp_conns[i], MP_MSG_WAIT_STATUS, &b, 1); }
    }
}

/* server -> all clients: broadcast this turn's movement-replay snapshot the moment it's captured
   (movement-start), so clients animate fleet movement BEFORE the combat/bomb/ground result screens
   that fire later in resolution. Exposed via g_mp_movement_hook (fired from mp_premove_capture). */
static void mp_movement_send(const uint8_t *buf, int len) {
    if (len <= 0) { return; }
    for (int i = 0; i < s_mp_num_conns; ++i) {
        if (s_mp_conns[i]) { mp_send(s_mp_conns[i], MP_MSG_TURN_MOVE, buf, (uint32_t)len); }
    }
}

static int mp_decision_rpc(int player_id, int dtype, const void *req, int req_len, void *resp, int resp_buflen) {
    if ((player_id < 0) || (player_id >= s_mp_num_conns) || !s_mp_conns[player_id]) { return -1; }
    net_conn_t *c = s_mp_conns[player_id];
    if (dtype == MP_DEC_BATTLE_INIT) { mp_wait_status_bcast(1); } /* tell idle clients a battle is running */
    else if (dtype == MP_DEC_BATTLE_END) { mp_wait_status_bcast(0); }
    else if (dtype == MP_DEC_ELECTION_VOTE || dtype == MP_DEC_ELECTION_ACCEPT) { mp_wait_status_bcast(2); } /* galactic council in session */
    if (req_len < 0) { req_len = 0; }
    {   /* DECISION_REQ: [u16 dtype][req] */
        uint8_t *m = (uint8_t *)malloc(2 + req_len);
        if (!m) { return -1; }
        m[0] = (uint8_t)(dtype >> 8); m[1] = (uint8_t)dtype;
        if (req_len && req) { memcpy(m + 2, req, req_len); }
        int sr = mp_send(c, MP_MSG_DECISION_REQ, m, 2 + (uint32_t)req_len);
        free(m);
        if (sr != 0) { return -1; }
        MP_MSG("MPDBG rpc1: dtype=%d player=%d sent, waiting resp\n", dtype, player_id);
    }
    {   /* block for DECISION_RESP: [u16 dtype][resp] */
        uint8_t *buf = (uint8_t *)malloc(MP_DECISION_BUF_MAX);
        if (!buf) { return -1; }
        uint32_t dl = 0;
        int r = mp_recv_wait(c, MP_MSG_DECISION_RESP, buf, MP_DECISION_BUF_MAX, &dl, NULL, 0, MP_DECISION_TIMEOUT_SECS);
        MP_MSG("MPDBG rpc1: dtype=%d player=%d resp r=%d dl=%u\n", dtype, player_id, r, dl);
        if ((r != 0) || (dl < 2)) { free(buf); return -1; }
        int rlen = (int)dl - 2;
        if (rlen > resp_buflen) { rlen = resp_buflen; }
        if (rlen > 0) { memcpy(resp, buf + 2, rlen); }
        free(buf);
        return rlen;
    }
}

/* ask N players the same decision in parallel: send all requests, then collect all
   answers (so e.g. a battle's autoresolve prompt appears on both sides at once). */
static int mp_decision_rpc_multi(const int *players, int n, int dtype, const void *req, int req_len, void *resps, int resp_stride) {
    if (req_len < 0) { req_len = 0; }
    for (int i = 0; i < n; ++i) {
        int p = players[i];
        if ((p < 0) || (p >= s_mp_num_conns) || !s_mp_conns[p]) { return -1; }
    }
    if (dtype == MP_DEC_BATTLE_INIT) { mp_wait_status_bcast(1); } /* tell idle clients a battle is running */
    else if (dtype == MP_DEC_BATTLE_END) { mp_wait_status_bcast(0); }
    else if (dtype == MP_DEC_ELECTION_VOTE || dtype == MP_DEC_ELECTION_ACCEPT) { mp_wait_status_bcast(2); } /* galactic council in session */
    {   /* send every request first -> the prompts pop simultaneously */
        uint8_t *m = (uint8_t *)malloc(2 + req_len);
        if (!m) { return -1; }
        m[0] = (uint8_t)(dtype >> 8); m[1] = (uint8_t)dtype;
        if (req_len && req) { memcpy(m + 2, req, req_len); }
        for (int i = 0; i < n; ++i) {
            if (mp_send(s_mp_conns[players[i]], MP_MSG_DECISION_REQ, m, 2 + (uint32_t)req_len) != 0) { free(m); return -1; }
        }
        free(m);
        MP_MSG("MPDBG rpcN: dtype=%d sent to %d player(s)\n", dtype, n);
    }
    {   /* collect each player's answer into its slot */
        uint8_t *buf = (uint8_t *)malloc(MP_DECISION_BUF_MAX);
        if (!buf) { return -1; }
        for (int i = 0; i < n; ++i) {
            uint32_t dl = 0;
            MP_MSG("MPDBG rpcN: dtype=%d waiting resp from player[%d]=%d\n", dtype, i, players[i]);
            if ((mp_recv_wait(s_mp_conns[players[i]], MP_MSG_DECISION_RESP, buf, MP_DECISION_BUF_MAX, &dl, NULL, 0, MP_DECISION_TIMEOUT_SECS) != 0) || (dl < 2)) { MP_MSG("MPDBG rpcN: dtype=%d resp FAIL from player[%d]=%d\n", dtype, i, players[i]); free(buf); return -1; }
            MP_MSG("MPDBG rpcN: dtype=%d got resp from player[%d]=%d\n", dtype, i, players[i]);
            int rlen = (int)dl - 2;
            if (rlen > resp_stride) { rlen = resp_stride; }
            if (rlen > 0) { memcpy((uint8_t *)resps + (size_t)i * resp_stride, buf + 2, rlen); }
        }
        free(buf);
    }
    return 0;
}

/* server: apply one client's lobby field change, with validation. Returns true if the state
   changed (so it must be re-broadcast). slot is the requesting client's player id. */
static bool mp_lobby_apply(struct mp_lobby_s *lob, int slot, uint8_t field, uint8_t value, int num_clients) {
    switch (field) {
        case MP_LOBBY_F_RACE:
            for (int j = 0; j < num_clients; ++j) { if (j != slot && lob->slot[j].race == value) { return false; } } /* no two players share a race */
            if (lob->slot[slot].race == value) { return false; }
            lob->slot[slot].race = value;
            return true;
        case MP_LOBBY_F_BANNER:
            for (int j = 0; j < num_clients; ++j) { if (j != slot && lob->slot[j].banner == value) { return false; } } /* nor a color */
            if (lob->slot[slot].banner == value) { return false; }
            lob->slot[slot].banner = value;
            return true;
        case MP_LOBBY_F_READY:
            if (value && lob->slot[slot].race == 0xff) { return false; } /* must pick a race before readying */
            lob->slot[slot].ready = (value != 0);
            return true;
        case MP_LOBBY_F_NUM_AI:
            if (slot != 0) { return false; } /* host only */
            if (value > MP_MAX_PLAYERS - num_clients) { value = (uint8_t)(MP_MAX_PLAYERS - num_clients); }
            if (lob->num_ai == value) { return false; }
            lob->num_ai = value;
            for (int j = 0; j < num_clients; ++j) { lob->slot[j].ready = 0; } /* settings changed: re-confirm */
            return true;
        case MP_LOBBY_F_GALAXY:
            if (slot != 0) { return false; } /* host only */
            if (value > 5) { value = 5; } /* GALAXY_SIZE_NUM-1, kept literal to stay engine-agnostic */
            if (lob->galaxy_size == value) { return false; }
            lob->galaxy_size = value;
            for (int j = 0; j < num_clients; ++j) { lob->slot[j].ready = 0; }
            return true;
        case MP_LOBBY_F_TEAM: {
            int tslot = value >> 4, team = value & 0x0f;
            int total = lob->num_humans + lob->num_ai;
            if (total > MP_MAX_PLAYERS) { total = MP_MAX_PLAYERS; }
            if (tslot >= total) { return false; }
            if (!((tslot == slot) || (slot == 0))) { return false; } /* set your own team, or host sets anyone's */
            if (team > MP_MAX_PLAYERS) { team = MP_MAX_PLAYERS; }
            if (lob->slot[tslot].team == team) { return false; }
            lob->slot[tslot].team = (uint8_t)team;
            for (int j = 0; j < num_clients; ++j) { lob->slot[j].ready = 0; } /* team change: re-confirm */
            return true;
        }
        case MP_LOBBY_F_DIFFICULTY:
            if (slot != 0) { return false; } /* host only */
            if (value > 4) { value = 4; } /* DIFFICULTY_NUM-1, kept literal to stay engine-agnostic */
            if (lob->difficulty == value) { return false; }
            lob->difficulty = value;
            for (int j = 0; j < num_clients; ++j) { lob->slot[j].ready = 0; } /* settings changed: re-confirm */
            return true;
        case MP_LOBBY_F_AI_RACE: {
            if (slot != 0) { return false; } /* host only */
            int aidx = value >> 4, r = value & 0x0f;
            int total = lob->num_humans + lob->num_ai;
            if (total > MP_MAX_PLAYERS) { total = MP_MAX_PLAYERS; }
            if ((aidx < lob->num_humans) || (aidx >= total)) { return false; } /* must target an AI slot */
            for (int j = 0; j < total; ++j) { if (j != aidx && lob->slot[j].race == r) { return false; } } /* keep races distinct */
            if (lob->slot[aidx].race == r) { return false; }
            lob->slot[aidx].race = (uint8_t)r;
            return true;
        }
        case MP_LOBBY_F_TIMER:
            if (slot != 0) { return false; } /* host only */
            if (lob->turn_timer_secs == value) { return false; }
            lob->turn_timer_secs = value;
            return true; /* no ready-reset: timer change doesn't affect strategic decisions */
        default:
            return false;
    }
}

/* -------------------------------------------------------------------------- */

/* ---- live human-to-human diplomacy: server referee (planning phase, store-and-forward) ---- */
struct mp_diplo_sess_s {
    bool active;                 /* a session exists in this slot (keyed by proposer id) */
    uint16_t sid;
    int proposer, responder;
    bool joined_p, joined_r;     /* each side has opted into the modal */
    bool open;                   /* both joined -> modal open + both marked busy */
};
static struct mp_diplo_sess_s s_diplo_sess[MP_MAX_PLAYERS]; /* keyed by proposer id */
static int s_diplo_invite_to[MP_MAX_PLAYERS];               /* invite_to[from]=to (un-accepted), -1 = none */
static bool s_diplo_busy[MP_MAX_PLAYERS];                   /* in an OPEN session -> cannot ready */
static uint16_t s_diplo_next_sid = 1;

static void mp_diplo_reset(void) {
    for (int i = 0; i < MP_MAX_PLAYERS; ++i) { s_diplo_sess[i].active = false; s_diplo_invite_to[i] = -1; s_diplo_busy[i] = false; }
}
static void mp_diplo_send3(net_conn_t *c, uint16_t msg, int a, int b, int v) {
    if (!c) { return; }
    uint8_t p[5] = { (uint8_t)(a >> 8), (uint8_t)a, (uint8_t)(b >> 8), (uint8_t)b, (uint8_t)v };
    mp_send(c, msg, p, 5);
}
static int mp_diplo_find_member(int player) {
    for (int i = 0; i < MP_MAX_PLAYERS; ++i) { if (s_diplo_sess[i].active && ((s_diplo_sess[i].proposer == player) || (s_diplo_sess[i].responder == player))) { return i; } }
    return -1;
}
static int mp_diplo_find_sid(uint16_t sid) {
    for (int i = 0; i < MP_MAX_PLAYERS; ++i) { if (s_diplo_sess[i].active && (s_diplo_sess[i].sid == sid)) { return i; } }
    return -1;
}
/* create a session keyed by `proposer`; both sides [Join] before it opens. resp_joined=true when the
   responder got here by accepting an invite (its [Now] click IS its join). */
static void mp_diplo_open_session(net_conn_t **conns, int num, int proposer, int responder, bool resp_joined) {
    struct mp_diplo_sess_s *s = &s_diplo_sess[proposer];
    s->active = true; s->sid = s_diplo_next_sid++; if (s_diplo_next_sid == 0) { s_diplo_next_sid = 1; }
    s->proposer = proposer; s->responder = responder;
    s->joined_p = false; s->joined_r = resp_joined; s->open = false;
    s_diplo_invite_to[proposer] = -1; s_diplo_invite_to[responder] = -1;
    if (proposer < num) { mp_diplo_send3(conns[proposer], MP_MSG_DIPLO_READY, proposer, responder, 0); } /* proposer: [Join] */
}
static void mp_diplo_server_handle(net_conn_t **conns, int num, int from_i, uint16_t id, const uint8_t *blob, uint32_t dl) {
    if (id == MP_MSG_DIPLO_INVITE) {
        if (dl < 4) { return; }
        int from = (blob[0] << 8) | blob[1], to = (blob[2] << 8) | blob[3];
        if ((from != from_i) || (to < 0) || (to >= num) || (to == from)) { return; }
        if (s_diplo_busy[to] || s_diplo_busy[from] || (mp_diplo_find_member(from) >= 0)) {
            mp_diplo_send3(conns[from], MP_MSG_DIPLO_INVITE_RESULT, from, to, 1); /* busy */
            return;
        }
        if (s_diplo_invite_to[to] == from) { /* crossing invites -> collapse; lower id proposes */
            int prop = (from < to) ? from : to, resp = (from < to) ? to : from;
            mp_diplo_open_session(conns, num, prop, resp, false);
            mp_diplo_send3(conns[resp], MP_MSG_DIPLO_READY, prop, resp, 0); /* both sides [Join] */
            return;
        }
        s_diplo_invite_to[from] = to;
        mp_diplo_send3(conns[to], MP_MSG_DIPLO_INVITE_NOTIFY, from, to, 0);
        mp_diplo_send3(conns[from], MP_MSG_DIPLO_INVITE_RESULT, from, to, 0); /* pending */
    } else if (id == MP_MSG_DIPLO_ACCEPT) {
        if (dl < 5) { return; }
        int from = (blob[0] << 8) | blob[1], to = (blob[2] << 8) | blob[3], acc = blob[4];
        if ((to != from_i) || (s_diplo_invite_to[from] != to)) { return; } /* must answer a real pending invite */
        if (!acc) { s_diplo_invite_to[from] = -1; mp_diplo_send3(conns[from], MP_MSG_DIPLO_INVITE_RESULT, from, to, 2); return; } /* declined */
        mp_diplo_open_session(conns, num, from, to, true); /* responder's [Now] is its join */
    } else if (id == MP_MSG_DIPLO_JOIN) {
        /* a player is in <=1 session, so resolve by membership (the proposer has no sid yet) */
        int s = mp_diplo_find_member(from_i);
        if (s < 0) { return; }
        if (from_i == s_diplo_sess[s].proposer) { s_diplo_sess[s].joined_p = true; }
        else if (from_i == s_diplo_sess[s].responder) { s_diplo_sess[s].joined_r = true; }
    } else if ((id == MP_MSG_DIPLO_PROPOSAL) || (id == MP_MSG_DIPLO_RESPONSE)) {
        /* in-session relay: forward the proposal/answer to the peer, session stays open */
        int s = mp_diplo_find_member(from_i);
        if (s < 0) { return; }
        int peer = (from_i == s_diplo_sess[s].proposer) ? s_diplo_sess[s].responder : s_diplo_sess[s].proposer;
        if ((peer >= 0) && (peer < num)) { mp_send(conns[peer], id, blob, dl); }
    } else if ((id == MP_MSG_DIPLO_SESSION_END) || (id == MP_MSG_DIPLO_CANCEL)) {
        if (dl < 2) { return; }
        int s = mp_diplo_find_sid((uint16_t)((blob[0] << 8) | blob[1]));
        if (s < 0) { return; }
        int prop = s_diplo_sess[s].proposer, resp = s_diplo_sess[s].responder;
        int peer = (from_i == prop) ? resp : prop;
        if ((peer >= 0) && (peer < num)) { mp_send(conns[peer], id, blob, dl); } /* forward to peer */
        s_diplo_busy[prop] = false; s_diplo_busy[resp] = false; s_diplo_sess[s].active = false;
        MP_MSG("server DIAG: diplo_busy CLEARED players %d,%d (session ended)\n", prop, resp);
    } else if (id == MP_MSG_TEAM_STANCE) {
        /* 1oom-mp teams: store-and-forward a foreign-policy consensus message to its addressee. */
        if (dl < 4) { return; }
        int from = (blob[0] << 8) | blob[1], to = (blob[2] << 8) | blob[3];
        if ((from != from_i) || (to < 0) || (to >= num)) { return; }
        if (conns[to]) { mp_send(conns[to], MP_MSG_TEAM_STANCE, blob, dl); }
    }
}
/* each barrier iteration: promote fully-joined sessions to OPEN (sends SESSION_OPEN to both) */
static void mp_diplo_server_tick(net_conn_t **conns, int num) {
    for (int i = 0; i < MP_MAX_PLAYERS; ++i) {
        struct mp_diplo_sess_s *s = &s_diplo_sess[i];
        if (s->active && (!s->open) && s->joined_p && s->joined_r) {
            s->open = true; s_diplo_busy[s->proposer] = true; s_diplo_busy[s->responder] = true;
            MP_MSG("server DIAG: diplo_busy SET players %d,%d (session opened)\n", s->proposer, s->responder);
            for (int role = 0; role < 2; ++role) {
                int who = role ? s->responder : s->proposer;
                if ((who >= 0) && (who < num) && conns[who]) {
                    uint8_t p[7] = { (uint8_t)(s->sid >> 8), (uint8_t)s->sid, (uint8_t)(s->proposer >> 8), (uint8_t)s->proposer, (uint8_t)(s->responder >> 8), (uint8_t)s->responder, (uint8_t)role };
                    mp_send(conns[who], MP_MSG_DIPLO_SESSION_OPEN, p, 7);
                }
            }
        }
    }
}
/* end of turn: un-accepted invites lapse (decision #1) */
static void mp_diplo_expire_invites(net_conn_t **conns, int num) {
    for (int from = 0; from < num; ++from) {
        int to = s_diplo_invite_to[from];
        if (to >= 0) { s_diplo_invite_to[from] = -1; mp_diplo_send3(conns[from], MP_MSG_DIPLO_INVITE_RESULT, from, to, 3); } /* expired */
    }
}

/* server: relay a player's live plan snapshot to its teammates only (best-effort). */
static void mp_team_plan_server_handle(net_conn_t **conns, int num, int from_i, const uint8_t *blob, uint32_t dl, const mp_game_iface_t *gi) {
    int team;
    if (!gi->get_team) { return; }
    team = gi->get_team(gi->ctx, from_i);
    if (team == 0) { return; } /* sender isn't on a team -> nobody to share with */
    for (int j = 0; j < num; ++j) {
        if ((j != from_i) && (gi->get_team(gi->ctx, j) == team)) {
            mp_send_besteffort(conns[j], MP_MSG_TEAM_PLAN, blob, dl);
        }
    }
}

/* server: a client sent a chat line -> stamp the sender id and broadcast to EVERYONE (incl. the sender,
   so their own line echoes into their history). Best-effort: chat is not game state. */
static void mp_chat_server_handle(net_conn_t **conns, int num, int from_i, const uint8_t *blob, uint32_t dl) {
    uint8_t m[2 + MP_CHAT_MAX];
    uint32_t n;
    if (dl > MP_CHAT_MAX) { dl = MP_CHAT_MAX; }
    m[0] = (uint8_t)((from_i >> 8) & 0xff);   /* sender id, big-endian (matches the diplo messages) */
    m[1] = (uint8_t)(from_i & 0xff);
    if (dl) { memcpy(m + 2, blob, dl); }
    n = 2 + dl;
    for (int j = 0; j < num; ++j) {
        if (conns[j]) { mp_send_besteffort(conns[j], MP_MSG_CHAT, m, n); }
    }
}

/* read + validate a just-connected client's HELLO ([u16 proto][u32 wire_id]) with a short timeout.
   The client sends it immediately on connect, so this returns within a round-trip in the normal case;
   a silent / mismatched-build client is refused (caller closes it). Returns true iff the handshake is OK. */
static bool mp_server_hello_ok(net_conn_t *c, const mp_game_iface_t *gi) {
    uint8_t buf[64]; uint16_t id; uint32_t dl;
    for (int tries = 0; tries < 3000; ++tries) { /* ~3s budget; HELLO normally arrives in <1ms */
        int r = mp_recv(c, &id, buf, sizeof(buf), &dl);
        if (r < 0) { MP_ERR("server: client dropped before HELLO\n"); return false; }
        if (r == 1) {
            if (id != MP_MSG_HELLO) { continue; } /* ignore anything stray, keep waiting for HELLO */
            int proto = (dl >= 2) ? ((buf[0] << 8) | buf[1]) : 0;
            uint32_t wid = (dl >= 6) ? (((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 8) | (uint32_t)buf[5]) : 0;
            if (proto != MP_PROTO_VERSION) { MP_ERR("server: rejecting client -- protocol v%d, server is v%d (update the client)\n", proto, MP_PROTO_VERSION); return false; }
            if (gi->wire_id) {
                uint32_t mine = (uint32_t)gi->wire_id(gi->ctx);
                if (wid != mine) { MP_ERR("server: rejecting client -- wire fingerprint %08x != server %08x (mismatched build)\n", wid, mine); return false; }
            }
            return true;
        }
        usleep(1000);
    }
    MP_ERR("server: rejecting client -- no HELLO within timeout\n");
    return false;
}

/* 1oom-mp: mid-game reconnect. When a slot is dropped (its empire is coasting, see the barrier), accept
   a re-launched client into it so the player can rejoin without restarting the whole game. v1 claims the
   LOWEST dropped slot -- unambiguous for the common single-drop case (multiple simultaneous drops are
   first-come, noted as a limitation). The client is told via the WELCOME reconnect flag (5th byte = 1) to
   skip the lobby and load the state we send right after. Returns the reclaimed slot, or -1 (nobody / no
   slot / bad build). Called from the barrier only while a slot is actually dropped. */
static int mp_server_try_reconnect(net_listener_t *l, net_conn_t **conns, int num_clients, const mp_game_iface_t *gi, uint8_t *blob, int blobcap) {
    net_conn_t *c = net_accept(l);
    int slot = -1, len;
    if (!c) { return -1; }
    if (!mp_server_hello_ok(c, gi)) { net_conn_close(c); return -1; } /* must be the same build */
    for (int i = 0; i < num_clients; ++i) { if (!conns[i]) { slot = i; break; } }
    if (slot < 0) { MP_MSG("server: reconnect refused -- no dropped slot (game full)\n"); net_conn_close(c); return -1; }
    {   /* WELCOME with the reconnect flag so the client skips the lobby and waits for the state below */
        uint8_t w[5] = { (uint8_t)(slot >> 8), (uint8_t)slot, (uint8_t)(num_clients >> 8), (uint8_t)num_clients, 1 };
        if (mp_send(c, MP_MSG_WELCOME, w, 5) != 0) { net_conn_close(c); return -1; }
    }
    len = gi->serialize(gi->ctx, blob, blobcap);
    if ((len <= 0) || (mp_send(c, MP_MSG_GAME_DATA, blob, (uint32_t)len) != 0)) { net_conn_close(c); return -1; }
    conns[slot] = c; /* live again -- the barrier waits for its READY from now on; s_mp_conns aliases this */
    MP_MSG("server: player %d reconnected\n", slot);
    return slot;
}

int mp_server_run(uint16_t port, int num_clients, int max_turns, const mp_game_iface_t *gi, int open_lobby) {
    if (num_clients < 1 || num_clients > MP_MAX_PLAYERS) { MP_ERR("bad num_clients %d\n", num_clients); return -1; }
    net_listener_t *l = net_listen(port);
    if (!l) { return -1; }
    net_conn_t *conns[MP_MAX_PLAYERS] = {0};
    uint8_t *porders[MP_MAX_PLAYERS] = {0}; /* each client's latest orders (applied at resolution) */
    int blobcap = gi->max_blob_len(gi->ctx);
    uint8_t *blob = (uint8_t *)malloc(blobcap);
    if (!blob) { net_listener_close(l); return -1; }
    for (int i = 0; i < num_clients; ++i) {
        porders[i] = (uint8_t *)malloc(blobcap);
        if (!porders[i]) { for (int j = 0; j < i; ++j) { free(porders[j]); } free(blob); net_listener_close(l); return -1; }
    }
    int rc = 0;
    bool game_over_natural = false; /* 1oom-mp: true only when the game ended for real (not a drop/error), so GAME_OVER can carry the ending */

    /* --- lobby: accept clients ---
       fixed mode: block until exactly num_clients connect. open mode: block only for the host
       (slot 0); the rest join during the lobby loop below, up to `cap`. */
    int cap = num_clients;
    if (open_lobby && gi->setup_game) {
        MP_MSG("server: OPEN lobby on port %u -- waiting for host, then up to %d human(s)\n", (unsigned)port, cap);
        for (;;) { net_conn_t *c = net_accept(l); if (c) { if (!mp_server_hello_ok(c, gi)) { net_conn_close(c); continue; } conns[0] = c; MP_MSG("server: host connected (%s)\n", net_conn_addr(c)); break; } usleep(2000); }
        num_clients = 1;
    } else {
        open_lobby = 0; /* no lobby to drive (resume/legacy) -> fall back to the fixed accept */
        MP_MSG("server: waiting for %d client(s) on port %u\n", num_clients, (unsigned)port);
        for (int i = 0; i < num_clients; ) {
            net_conn_t *c = net_accept(l);
            if (c) {
                if (!mp_server_hello_ok(c, gi)) { net_conn_close(c); continue; } /* refuse a mismatched-build client */
                conns[i] = c;
                ++i;
                MP_MSG("server: client %d/%d connected (%s)\n", i, num_clients, net_conn_addr(c));
            } else {
                usleep(2000);
            }
        }
    }

    /* arm the interactive-decision channel so the null UI can ask clients mid-resolution */
    s_mp_conns = conns;
    s_mp_num_conns = num_clients;
    g_mp_decision_hook = mp_decision_rpc;
    g_mp_decision_hook_multi = mp_decision_rpc_multi;
    g_mp_spectate_hook = mp_spectate_send;
    g_mp_movement_hook = mp_movement_send;

    /* --- assign empires + send initial state --- */
    for (int i = 0; i < num_clients; ++i) {
        uint8_t w[4] = { (uint8_t)(i >> 8), (uint8_t)i, (uint8_t)(num_clients >> 8), (uint8_t)num_clients };
        if (mp_send(conns[i], MP_MSG_WELCOME, w, 4) != 0) { rc = -1; goto done; }
    }
    /* per-game turn timer (host-set in lobby; settable via MP_TIMER env for no-lobby mode too) */
    uint8_t turn_timer_secs = 60;
    { const char *e; if ((e = getenv("MP_TIMER"))) { int v = atoi(e); if ((v >= 0) && (v <= 255)) { turn_timer_secs = (uint8_t)v; } } }

    /* --- lobby: maintain a shared state every client edits live (race/color/ready, plus the
       host's AI count & galaxy size). Re-broadcast on each change; once all clients are ready,
       build the game from it. --- */
    if (gi->setup_game) {
        struct mp_lobby_s lob;
        memset(&lob, 0, sizeof(lob));
        lob.num_humans = (uint8_t)num_clients;
        lob.num_ai = (num_clients < MP_MAX_PLAYERS) ? 1 : 0; /* default 1 AI if there's room */
        lob.galaxy_size = 1;                                 /* default medium */
        lob.difficulty = 2;                                  /* default average (DIFFICULTY_AVERAGE) */
        lob.open_lobby = open_lobby ? 1 : 0;
        lob.turn_timer_secs = turn_timer_secs;               /* default from env/hardcoded; host can adjust in lobby UI */
        { const char *e; /* host-launch overrides for the lobby defaults (still editable in the lobby UI) */
          if ((e = getenv("MP_GSIZE"))) { int v = atoi(e); if ((v >= 0) && (v <= 3)) { lob.galaxy_size = (uint8_t)v; } }
          if ((e = getenv("MP_DIFF")))  { int v = atoi(e); if ((v >= 0) && (v <= 4)) { lob.difficulty = (uint8_t)v; } }
          if ((e = getenv("MP_AI")))    { int v = atoi(e); if ((v >= 0) && (v <= MP_MAX_PLAYERS - num_clients)) { lob.num_ai = (uint8_t)v; } } }
        for (int i = 0; i < MP_MAX_PLAYERS; ++i) {
            lob.slot[i].race = 0xff; lob.slot[i].banner = 0xff; lob.slot[i].ready = 0;
            lob.slot[i].connected = (i < num_clients) ? 1 : 0; /* AI slots start race-unset (= random) */
        }
        if (getenv("MP_LOBBYDEMO")) { /* test aid: pre-seed picks so a screenshot shows portraits/flags */
            for (int i = 0; i < num_clients; ++i) { lob.slot[i].race = (uint8_t)((i * 3) % 10); lob.slot[i].banner = (uint8_t)(i % 6); }
        }
        bool dirty = true, started = false, start_req = false;
        while (!started) {
            if (dirty) {
                for (int i = 0; i < num_clients; ++i) {
                    if (mp_send(conns[i], MP_MSG_LOBBY, (const uint8_t *)&lob, sizeof(lob)) != 0) { rc = -1; goto done; }
                }
                dirty = false;
            }
            /* open mode: admit additional humans up to the cap (non-blocking), welcome each into the
               next contiguous slot, and clamp the AI count so humans+AI never exceeds MP_MAX_PLAYERS. */
            if (open_lobby && (num_clients < cap)) {
                net_conn_t *c = net_accept(l);
                if (c && !mp_server_hello_ok(c, gi)) { net_conn_close(c); c = NULL; } /* refuse a mismatched-build joiner */
                if (c) {
                    int s = num_clients;
                    conns[s] = c;
                    lob.slot[s].connected = 1; lob.slot[s].race = 0xff; lob.slot[s].banner = 0xff; lob.slot[s].ready = 0; lob.slot[s].team = 0;
                    num_clients = s + 1;
                    lob.num_humans = (uint8_t)num_clients;
                    if (((int)lob.num_humans + (int)lob.num_ai) > MP_MAX_PLAYERS) { lob.num_ai = (uint8_t)(MP_MAX_PLAYERS - lob.num_humans); }
                    { uint8_t w[4] = { (uint8_t)(s >> 8), (uint8_t)s, (uint8_t)(num_clients >> 8), (uint8_t)num_clients };
                      if (mp_send(c, MP_MSG_WELCOME, w, 4) != 0) { rc = -1; goto done; } }
                    MP_MSG("server: client joined open lobby -> slot %d (%d/%d humans, %s)\n", s, num_clients, cap, net_conn_addr(c));
                    dirty = true;
                }
            }
            for (int i = 0; i < num_clients; ++i) {
                uint16_t id; uint32_t dl;
                int r = mp_recv(conns[i], &id, blob, blobcap, &dl);
                if (r < 0) { MP_ERR("server: client %d dropped in lobby\n", i); rc = -1; goto done; }
                if (r != 1) { continue; }
                if (id == MP_MSG_LOBBY_PICK && dl >= 2) {
                    if (open_lobby && (blob[0] == MP_LOBBY_F_START)) {
                        if (i == 0) { start_req = true; } /* only the host (slot 0) may start the game */
                    } else if (mp_lobby_apply(&lob, i, blob[0], blob[1], num_clients)) {
                        dirty = true;
                    }
                }
            }
            if (open_lobby) {
                /* the host pressed Start: go once every other connected human is ready */
                if (start_req) {
                    started = true;
                    for (int i = 1; i < num_clients; ++i) { if (!lob.slot[i].ready) { started = false; break; } }
                    if (!started) { start_req = false; } /* someone un-readied -- wait for a fresh Start */
                }
            } else {
                started = true;
                for (int i = 0; i < num_clients; ++i) { if (!lob.slot[i].ready) { started = false; break; } }
            }
            if (!started) { usleep(2000); }
        }
        if (open_lobby) { lob.slot[0].ready = 1; } /* the host's Start stands in for a Ready */
        turn_timer_secs = lob.turn_timer_secs; /* take the host's final choice */
        if (gi->setup_game(gi->ctx, &lob) != 0) { MP_ERR("server: setup_game failed\n"); rc = -1; goto done; }
        MP_MSG("server: game created from lobby (humans=%d ai=%d gsize=%d timer=%ds)\n", lob.num_humans, lob.num_ai, lob.galaxy_size, turn_timer_secs);
    }
    s_mp_num_conns = num_clients; /* an open lobby may have grown the human count -- lock it in for the decision/spectate hooks */
    {
        int len = gi->serialize(gi->ctx, blob, blobcap);
        if (len <= 0) { MP_ERR("server: serialize failed\n"); rc = -1; goto done; }
        for (int i = 0; i < num_clients; ++i) {
            if (mp_send(conns[i], MP_MSG_GAME_DATA, blob, len) != 0) { rc = -1; goto done; }
        }
        MP_MSG("server: sent initial state (%d bytes), turn %d\n", len, gi->turn_number(gi->ctx));
    }

    /* --- soft-ready barrier loop --- */
    mp_diplo_reset();
    for (int turn = 0; (max_turns == 0) || (turn < max_turns); ++turn) {
        /* Collect each client's latest orders + ready flag; resolve once everyone is ready.
           A legacy TURN_INPUT counts as "ready now" with those orders, so old and soft-ready
           clients interoperate. Orders are buffered per client and applied once, at resolution
           (re-applying a re-submitted order stream could double event-style orders). */
        bool ready[MP_MAX_PLAYERS] = {0};
        int porders_len[MP_MAX_PLAYERS];
        for (int i = 0; i < num_clients; ++i) { porders_len[i] = 0; }
        mp_diplo_expire_invites(conns, num_clients);     /* un-accepted invites from last turn lapse (decision #1) */
        /* 1oom-mp: also tear down any leftover session + busy flag at the turn boundary. A live audience
           is a within-planning-phase thing: an OPEN session marks both players busy, and busy blocks the
           barrier, so an open session can never legitimately survive into the next turn. The only sessions
           that reach here are STALE half-open ones (a handshake that stalled -- e.g. a crossing invite
           whose responder never joined, or an accept whose proposer never joined): active==true but
           open==false, so they don't block the barrier and the game keeps running, yet mp_diplo_find_member
           keeps reporting both players as engaged -> "already engaged in another audience" for the rest of
           the game. Clearing them each turn makes any stalled handshake self-heal at the next turn. */
        for (int i = 0; i < MP_MAX_PLAYERS; ++i) { s_diplo_sess[i].active = false; s_diplo_busy[i] = false; }
        bool all_ready = false;
        bool timer_sent = false; /* have we broadcast TIMER_START this turn? */
        int wait_log = 0; /* DIAG: throttle the "waiting" log to ~once/2s while stuck */
        while (!all_ready) {
            /* 1oom-mp: while a slot is dropped, let a re-launched client rejoin that (coasting) empire. */
            { int dropped = 0; for (int i = 0; i < num_clients; ++i) { if (!conns[i]) { dropped = 1; break; } }
              if (dropped) { mp_server_try_reconnect(l, conns, num_clients, gi, blob, blobcap); } }
            for (int i = 0; i < num_clients; ++i) {
                uint16_t id; uint32_t dl;
                int r;
                if (!conns[i]) { continue; } /* 1oom-mp: a dropped player's slot -- its empire coasts */
                r = mp_recv(conns[i], &id, blob, blobcap, &dl);
                if (r < 0) {
                    /* 1oom-mp: a client dropped. Don't end the whole game for everyone -- close its slot
                       and carry on for the remaining humans. Its empire coasts on its last orders, and its
                       combat auto-resolves (the null-UI decision default, since its conn is now NULL). End
                       only when EVERYONE has gone. The autosave is current, so a full reconnect via -mpload
                       is still available if they'd rather resume with the dropped player back. */
                    int live = 0;
                    MP_ERR("server: client %d dropped -- continuing without it (its empire coasts)\n", i);
                    net_conn_close(conns[i]); conns[i] = NULL;
                    for (int j = 0; j < num_clients; ++j) { if (conns[j]) { ++live; } }
                    if (live == 0) { MP_MSG("server: all clients gone -- ending\n"); goto done; }
                    continue;
                }
                if (r != 1) { continue; }
                /* orders apply to the SERVER-assigned empire id (i), not any client-claimed one. */
                if (id == MP_MSG_TURN_INPUT) {           /* legacy: [u16 pid][orders] = ready now */
                    int olen = (dl >= 2) ? (int)dl - 2 : 0;
                    if (olen > blobcap) { olen = blobcap; }
                    if (olen > 0) { memcpy(porders[i], blob + 2, (size_t)olen); }
                    porders_len[i] = olen; ready[i] = true;
                    MP_MSG("server DIAG: turn %d TURN_INPUT from player %d (ready, %d B)\n", turn, i, olen);
                } else if (id == MP_MSG_READY) {         /* [u16 pid][u8 ready][orders] */
                    if (dl >= 3) {
                        int olen = (int)dl - 3;
                        if (olen > blobcap) { olen = blobcap; }
                        if (olen > 0) { memcpy(porders[i], blob + 3, (size_t)olen); }
                        porders_len[i] = olen; ready[i] = (blob[2] != 0);
                        MP_MSG("server DIAG: turn %d READY from player %d = %d (%d B)\n", turn, i, blob[2] != 0, olen);
                    }
                } else if (id == MP_MSG_TEAM_PLAN) {     /* live teammate-plan snapshot -> relay to teammates */
                    mp_team_plan_server_handle(conns, num_clients, i, blob, dl, gi);
                } else if (id == MP_MSG_SAVE_REQUEST) {  /* a player chose Esc -> Save -> write a named snapshot */
                    MP_MSG("server: save requested by player %d\n", i);
                    if (gi->save_request) { gi->save_request(gi->ctx, i); }
                } else if (id == MP_MSG_CHAT) {          /* a chat line -> stamp sender + broadcast to all */
                    mp_chat_server_handle(conns, num_clients, i, blob, dl);
                } else {                                 /* live-diplomacy messages (0x20-0x2A) */
                    mp_diplo_server_handle(conns, num_clients, i, id, blob, dl);
                }
            }
            mp_diplo_server_tick(conns, num_clients);    /* promote fully-joined sessions to OPEN */
            /* count ready (not diplo-busy) players; determine if only one straggler remains */
            all_ready = true;
            /* count ready vs blocking among LIVE slots; a dropped (NULL) slot is treated as ready so it
               can't block the turn (graceful-drop). nlive/nready drive Benjamin's one-straggler countdown.
               A player in an OPEN diplo session is held un-ready until it ends (decision #2). */
            int nlive = 0, nready = 0, block_i = -1;
            for (int i = 0; i < num_clients; ++i) {
                if (!conns[i]) { continue; }
                ++nlive;
                if (ready[i] && !s_diplo_busy[i]) { ++nready; } else { all_ready = false; if (block_i < 0) { block_i = i; } }
            }
            if (!all_ready) {
                if ((wait_log++ % 1000) == 0) { MP_MSG("server DIAG: turn %d WAITING -- player %d blocking (ready=%d diplo_busy=%d)\n", turn, block_i, ready[block_i] ? 1 : 0, s_diplo_busy[block_i] ? 1 : 0); }
                /* arm/cancel the client-side countdown: fire when exactly one live player hasn't readied up yet */
                if (turn_timer_secs > 0 && nlive >= 2) {
                    bool one_left = (nready == nlive - 1);
                    if (one_left && !timer_sent) {
                        uint8_t secs = turn_timer_secs;
                        for (int i = 0; i < num_clients; ++i) { mp_send(conns[i], MP_MSG_TIMER_START, &secs, 1); }
                        timer_sent = true;
                    } else if (!one_left && timer_sent) {
                        for (int i = 0; i < num_clients; ++i) { mp_send(conns[i], MP_MSG_TIMER_CANCEL, NULL, 0); }
                        timer_sent = false;
                    }
                }
                usleep(2000);
            }
        }
        /* everyone is ready: end the clients' planning phase, then apply orders + resolve */
        for (int i = 0; i < num_clients; ++i) {
            if (mp_send(conns[i], MP_MSG_RESOLVE_START, NULL, 0) != 0) { rc = -1; goto done; }
        }
        for (int i = 0; i < num_clients; ++i) {
            if (gi->apply_orders && porders_len[i] > 0) {
                int n = gi->apply_orders(gi->ctx, i, porders[i], porders_len[i]);
                if (n > 0) { MP_MSG("server: applied %d planet order(s) for player %d\n", n, i); }
                else if (n < 0) { MP_ERR("server: malformed orders from player %d (apply returned %d) -- ignored\n", i, n); }
            }
        }
        /* every client has submitted -> resolve the turn authoritatively */
        int over = 0;
        if (gi->advance_turn) {
            int ar = gi->advance_turn(gi->ctx);
            if (ar < 0) { MP_ERR("server: advance failed\n"); rc = -1; goto done; }
            over = (ar > 0);
        }
        /* stream this turn's fleet-movement snapshot (state as of movement-start) so
           clients can animate ships moving, just before the authoritative state. */
        if (gi->get_movement) {
            int mlen = gi->get_movement(gi->ctx, blob, blobcap);
            if (mlen > 0) {
                for (int i = 0; i < num_clients; ++i) {
                    if (mp_send(conns[i], MP_MSG_TURN_MOVE, blob, mlen) != 0) { rc = -1; goto done; }
                }
            }
        }
        /* broadcast the resolved state (the final turn too, so clients see the ending) */
        int len = gi->serialize(gi->ctx, blob, blobcap);
        if (len <= 0) { rc = -1; goto done; }
        for (int i = 0; i < num_clients; ++i) {
            if (mp_send(conns[i], MP_MSG_GAME_DATA, blob, len) != 0) { rc = -1; goto done; }
        }
        MP_MSG("server: resolved turn -> now turn %d, rebroadcast %d bytes\n", gi->turn_number(gi->ctx), len);
        if (over) { MP_MSG("server: game ended at turn %d\n", gi->turn_number(gi->ctx)); game_over_natural = true; goto done; }
    }

done:
    g_mp_decision_hook = NULL;
    g_mp_decision_hook_multi = NULL;
    g_mp_spectate_hook = NULL;
    g_mp_movement_hook = NULL;
    s_mp_conns = NULL;
    s_mp_num_conns = 0;
    {   /* 1oom-mp: on a real finish, ship each client ITS OWN ending (per-player so competing human teams
           see the right win/loss outcome); on a drop/error send an empty GAME_OVER (clients just exit). */
        for (int i = 0; i < num_clients; ++i) {
            uint8_t gobuf[256]; int golen = 0;
            if (!conns[i]) { continue; }
            if (game_over_natural && gi->get_game_over) { golen = gi->get_game_over(gi->ctx, i, gobuf, (int)sizeof(gobuf)); if (golen < 0) { golen = 0; } }
            mp_send(conns[i], MP_MSG_GAME_OVER, golen ? gobuf : NULL, (uint32_t)golen);
            net_conn_close(conns[i]);
        }
    }
    for (int i = 0; i < MP_MAX_PLAYERS; ++i) { free(porders[i]); }
    free(blob);
    net_listener_close(l);
    return rc;
}

/* -------------------------------------------------------------------------- */

int mp_cl_player_id(void) { return s_cl_pid; }

int mp_client_run(const char *host, uint16_t port, int max_turns, const mp_game_iface_t *gi) {
    net_conn_t *c;
    s_cl_go_len = -1; /* 1oom-mp: no ending received yet */
    c = net_connect(host, port);
    if (!c) { return -1; }
    int blobcap = gi->max_blob_len(gi->ctx);
    uint8_t *blob = (uint8_t *)malloc(blobcap);
    if (!blob) { net_conn_close(c); return -1; }
    uint8_t *respbuf = (uint8_t *)malloc(MP_DECISION_BUF_MAX);
    if (!respbuf) { free(blob); net_conn_close(c); return -1; }
    int rc = 0;
    uint32_t dl = 0;

    /* handshake */
    {   /* HELLO: [u16 proto][u32 wire_id]. The server refuses us if either differs from its own build. */
        uint32_t wid = gi->wire_id ? (uint32_t)gi->wire_id(gi->ctx) : 0;
        uint8_t h[6] = { 0, MP_PROTO_VERSION, (uint8_t)(wid >> 24), (uint8_t)(wid >> 16), (uint8_t)(wid >> 8), (uint8_t)wid };
        if (mp_send(c, MP_MSG_HELLO, h, 6) != 0) { rc = -1; goto done; }
    }
    if (mp_recv_wait(c, MP_MSG_WELCOME, blob, blobcap, &dl, gi, MP_WAIT_LOBBY, 0) != 0) { rc = -1; goto done; }
    int player_id = (dl >= 2) ? ((blob[0] << 8) | blob[1]) : 0;
    int num_players = (dl >= 4) ? ((blob[2] << 8) | blob[3]) : 1;
    int is_reconnect = (dl >= 5) ? blob[4] : 0; /* 1oom-mp: server reclaimed a dropped slot for us -> skip the lobby, load current state */
    s_cl_pid = player_id; /* 1oom-mp: valid from here on (the lobby block, skipped on reconnect, also set it) */
    MP_MSG("client: welcome - player %d of %d%s\n", player_id, num_players, is_reconnect ? " (reconnect)" : "");

    /* lobby: if the server runs one, drive the interactive lobby until the game starts. run_lobby
       loops on g_mp_cl_lobby_poll/_set; the poll loads the initial GAME_DATA when it arrives. */
    bool got_initial = false;
    if (gi->run_lobby && !is_reconnect) { /* 1oom-mp: a reconnect skips the lobby -- the state follows immediately */
        s_cl_conn = c; s_cl_pid = player_id; s_cl_blob = blob; s_cl_blobcap = blobcap; s_cl_gi = gi;
        s_cl_lobby_started = false; s_cl_game_over = false;
        memset(&s_cl_lobby, 0, sizeof(s_cl_lobby));
        g_mp_cl_lobby_poll = cl_lobby_poll_impl; g_mp_cl_lobby_set = cl_lobby_set_impl;
        int lr = gi->run_lobby(gi->ctx, player_id);
        g_mp_cl_lobby_poll = NULL; g_mp_cl_lobby_set = NULL;
        s_cl_conn = NULL; s_cl_blob = NULL; s_cl_gi = NULL;
        if (lr < 0 || !s_cl_lobby_started) { rc = (lr < 0 && !s_cl_game_over) ? 0 : -1; goto done; } /* quit / link drop */
        got_initial = true;
        MP_MSG("client: game started from lobby, turn %d\n", gi->turn_number(gi->ctx));
    }

    /* initial state (no-lobby/legacy path; the lobby loads it itself when present) */
    if (!got_initial) {
        if (mp_recv_wait(c, MP_MSG_GAME_DATA, blob, blobcap, &dl, gi, MP_WAIT_LOBBY, 0) != 0) { rc = -1; goto done; }
        if (gi->deserialize(gi->ctx, blob, dl) != 0) { MP_ERR("client: load state failed\n"); rc = -1; goto done; }
        if (gi->on_state_loaded) { gi->on_state_loaded(gi->ctx, 1); }
        MP_MSG("client: got initial state (%u bytes), turn %d\n", dl, gi->turn_number(gi->ctx));
    }

    /* turn loop. Two ways to submit a turn:
        - soft-ready (default): run the turn UI interactively; the player keeps playing, toggles
          Ready (the UI hooks send MP_MSG_READY with the latest orders), and the server replies
          RESOLVE_START once everyone is ready, which ends the planning phase.
        - legacy (MP_LEGACYTURN): serialize all orders up front and send TURN_INPUT, then wait.
       Both then share the resolution wait below (movement / combat decisions / GAME_DATA). */
    bool soft = (getenv("MP_LEGACYTURN") == NULL);
    for (int turn = 0; (max_turns == 0) || (turn < max_turns); ++turn) {
        if (soft) {
            s_cl_conn = c; s_cl_pid = player_id; s_cl_blob = blob; s_cl_blobcap = blobcap;
            s_cl_resolving = false; s_cl_game_over = false;
            g_mp_cl_send_ready = cl_send_ready_impl;
            g_mp_cl_poll = cl_poll_impl;
            g_mp_cl_diplo_recv = cl_diplo_recv_impl; g_mp_cl_diplo_send = cl_diplo_send_impl;
            g_mp_cl_team_plan_send = cl_team_plan_send_impl;
            g_mp_cl_chat_send = cl_chat_send_impl;
            if (gi->write_orders) { (void)gi->write_orders(gi->ctx, player_id, blob + 2, blobcap - 2); }
            g_mp_cl_poll = NULL; g_mp_cl_send_ready = NULL; s_cl_conn = NULL; s_cl_blob = NULL;
            g_mp_cl_diplo_recv = NULL; g_mp_cl_diplo_send = NULL;
            g_mp_cl_team_plan_send = NULL;
            g_mp_cl_chat_send = NULL;
            if (s_cl_game_over) { MP_MSG("client: game over\n"); goto done; }
        } else {
            /* submit my turn: [u16 player_id][orders blob] */
            blob[0] = (uint8_t)(player_id & 0xff);
            blob[1] = (uint8_t)((player_id >> 8) & 0xff);
            int olen = 0;
            if (gi->write_orders) {
                olen = gi->write_orders(gi->ctx, player_id, blob + 2, blobcap - 2);
                if (olen < 0) { olen = 0; }
            }
            if (mp_send(c, MP_MSG_TURN_INPUT, blob, 2 + (uint32_t)olen) != 0) { rc = -1; goto done; }
        }
        /* wait for next state (ignore GAME_OVER -> stop) */
        s_cl_wait_kind = 0; /* re-driven by the server's wait-status broadcasts below */
        uint16_t id;
        bool got = false;
        while (!got) {
            int r = mp_recv(c, &id, blob, blobcap, &dl);
            if (r < 0) { rc = -1; goto done; }
            if (r == 1) {
                if (id == MP_MSG_TURN_MOVE) {
                    /* animate this turn's fleet movement before the final state lands */
                    if (gi->play_movement) { gi->play_movement(gi->ctx, player_id, blob, dl); }
                } else if (id == MP_MSG_SPECTATE) {
                    /* watch the other side's battle turn — re-render, no reply */
                    if (gi->on_spectate) { gi->on_spectate(gi->ctx, blob, dl); }
                } else if (id == MP_MSG_DECISION_REQ) {
                    /* the server paused resolution to ask us a decision — answer it */
                    int dtype = (dl >= 2) ? ((blob[0] << 8) | blob[1]) : 0;
                    MP_MSG("MPDBG client: got DECISION_REQ dtype=%d\n", dtype);
                    int rlen = 0;
                    if (gi->handle_decision) {
                        rlen = gi->handle_decision(gi->ctx, dtype, blob + 2, (int)dl - 2, respbuf + 2, MP_DECISION_BUF_MAX - 2);
                    }
                    if (rlen < 0) { rlen = 0; }
                    respbuf[0] = (uint8_t)(dtype >> 8);
                    respbuf[1] = (uint8_t)dtype;
                    if (mp_send(c, MP_MSG_DECISION_RESP, respbuf, 2 + (uint32_t)rlen) != 0) { rc = -1; goto done; }
                } else if (id == MP_MSG_GAME_DATA) {
                    if (gi->deserialize(gi->ctx, blob, dl) != 0) { rc = -1; goto done; }
                    if (gi->on_state_loaded) { gi->on_state_loaded(gi->ctx, 0); }
                    MP_MSG("client: turn advanced -> turn %d\n", gi->turn_number(gi->ctx));
                    got = true;
                } else if (id == MP_MSG_GAME_OVER) {
                    MP_MSG("client: game over\n");
                    s_cl_go_len = (dl <= sizeof(s_cl_go_buf)) ? (int)dl : 0; /* stash the ending payload */
                    if (s_cl_go_len > 0) { memcpy(s_cl_go_buf, blob, (size_t)s_cl_go_len); }
                    goto done;
                } else if (id == MP_MSG_WAIT_STATUS) {
                    s_cl_wait_kind = (dl >= 1) ? blob[0] : 0;
                }
            } else {
                if (gi->on_wait) { gi->on_wait(gi->ctx, (s_cl_wait_kind == 2) ? MP_WAIT_COUNCIL : ((s_cl_wait_kind == 1) ? MP_WAIT_COMBAT : MP_WAIT_TURN)); }
                usleep(1000);
            }
        }
    }

done:
    if (gi->on_game_over && (s_cl_go_len >= 0)) { gi->on_game_over(gi->ctx, s_cl_go_buf, s_cl_go_len); } /* 1oom-mp: play the ending sequence on a real finish (not a link drop) */
    free(blob);
    free(respbuf);
    net_conn_close(c);
    return rc;
}
