#ifndef INC_1OOM_MP_H
#define INC_1OOM_MP_H

/* 1oom-mp: simultaneous-turn multiplayer protocol + server/client loops, layered
   on the net.c transport. Engine-agnostic: the game is accessed through an
   mp_game_iface_t of callbacks, so this module unit-tests standalone and the real
   engine wiring is a thin adapter (serialize=game_save_blob_save, etc.). */

#include <stdint.h>
#include <stdbool.h>

#define MP_PROTO_VERSION 1
#define MP_MAX_PLAYERS   6

/* message ids. S->C and C->S noted. Mirrors the 2018 protocol's intent. */
enum mp_msg_e {
    MP_MSG_NOP       = 0x00,
    MP_MSG_HELLO     = 0x01, /* C->S: [u16 proto_version] */
    MP_MSG_WELCOME   = 0x02, /* S->C: [u16 player_id][u16 num_players] */
    MP_MSG_LOBBY     = 0x03, /* S->C: [struct mp_lobby_s] = the shared lobby state (broadcast on change) */
    MP_MSG_LOBBY_PICK= 0x13, /* C->S: [u8 field][u8 value] = set one of my lobby fields (see mp_lobby_field_e) */
    MP_MSG_GAME_DATA = 0x08, /* S->C: [state blob] (save-format, authoritative) */
    MP_MSG_TURN_MOVE = 0x09, /* S->C: [pre-movement state blob] = animate this turn's fleet movement, sent just before GAME_DATA */
    MP_MSG_SPECTATE  = 0x0a, /* S->C: [battle_s] = a battle update to re-render (no reply); for watching the other side's turn */
    MP_MSG_RESOLVE_START = 0x0c, /* S->C: (empty) everyone is ready; ends the client's interactive planning phase, resolution (movement/combat/GAME_DATA) follows */
    MP_MSG_WAIT_STATUS = 0x0d, /* S->C: [u8 active] = a battle is (1) / is no longer (0) in progress, so waiting clients can say so */
    MP_MSG_TURN_INPUT= 0x11, /* C->S: [u16 player_id][orders blob] = submit my turn (legacy: immediate ready) */
    MP_MSG_READY     = 0x12, /* C->S: [u16 player_id][u8 ready][orders blob] = refresh my latest orders + ready flag; the server resolves once all clients are ready (soft-ready turn model) */
    /* --- live human-to-human diplomacy (planning-phase channel; server is a store-and-forward referee) --- */
    MP_MSG_DIPLO_INVITE       = 0x20, /* C->S: [u16 from][u16 to] = request a live audience */
    MP_MSG_DIPLO_INVITE_NOTIFY= 0x21, /* S->C: [u16 from][u16 to][u8 reason] = an invite is pending for 'to' (re-broadcast each tick) */
    MP_MSG_DIPLO_INVITE_RESULT= 0x22, /* S->C: [u16 from][u16 to][u8 status] = to the proposer (0 pending,1 busy,2 declined,3 expired) */
    MP_MSG_DIPLO_ACCEPT        = 0x23, /* C->S: [u16 from][u16 to][u8 accept] = responder answers the invite (1=[Now],0=decline) */
    MP_MSG_DIPLO_READY         = 0x24, /* S->C: [u16 from][u16 to] = responder accepted; proposer gets [Join]/[Later] */
    MP_MSG_DIPLO_JOIN          = 0x25, /* C->S: [u16 sid] = "I have reached a stopping point and am entering the session" */
    MP_MSG_DIPLO_SESSION_OPEN  = 0x26, /* S->C: [u16 sid][u16 proposer][u16 responder][u8 your_role] = both joined, open the modal */
    MP_MSG_DIPLO_PROPOSAL      = 0x27, /* C->S->C: [u16 sid][u8 kind][fields] = one proposal to answer (phase 2) */
    MP_MSG_DIPLO_RESPONSE      = 0x28, /* C->S->C: [u16 sid][u8 kind][i16 answer] = the human's accept/reject/counter (phase 2) */
    MP_MSG_DIPLO_SESSION_END   = 0x29, /* both: [u16 sid][u8 outcome][u8 verb][u8 arg] = the audience finished */
    MP_MSG_DIPLO_CANCEL        = 0x2a, /* both: [u16 sid][u8 why] = aborted (esc/disconnect) */
    MP_MSG_DECISION_REQ = 0x30, /* S->C: [u16 dtype][req] = a mid-resolution interactive decision the server is blocking on */
    MP_MSG_DECISION_RESP= 0x31, /* C->S: [u16 dtype][resp] = the human's answer */
    MP_MSG_GAME_OVER = 0x40, /* S->C: session ended */
};

/* interactive-decision types carried over the DECISION_REQ/RESP channel (the server
   pauses turn resolution and asks the owning client in real time) */
enum mp_decision_e {
    MP_DEC_NONE          = 0,
    MP_DEC_PING          = 1, /* synthetic round-trip self-test */
    MP_DEC_BOMB          = 2, /* bomb an invaded planet? (resp: 1 byte bool) */
    MP_DEC_ELECTION_VOTE = 3,
    MP_DEC_ELECTION_ACCEPT = 4,
    MP_DEC_SPY_STEAL     = 5,
    MP_DEC_SPY_SABOTAGE  = 6,
    MP_DEC_AUDIENCE      = 7,
    MP_DEC_BATTLE_TURN   = 8, /* one ship-action (resp: 1 byte ui_battle_action_t) */
    MP_DEC_BATTLE_INIT   = 9, /* battle start: autoresolve prompt + UI setup (resp: 1 byte mode) */
    MP_DEC_BATTLE_END    = 10, /* battle end: tear down the battle UI */
    MP_DEC_GROUND        = 11, /* ground-invasion result + animation (req: struct ground_s; shown to both human sides; resp: 1 byte ack) */
    MP_DEC_BOMB_SHOW     = 12, /* orbital-bombing result (req: attacker/owner/planet + pop/fact damage; shown to both human sides; resp: 1 byte ack) */
};

/* AUDIENCE relay subtypes: which ui_audience_* call the server is running on the human's behalf.
   Payload: [u8 subtype][u8 newtech_pi][audience_s][u16 buf_len + buf][u8 nstr + (u16 len+str)*][u8 has_cond + cond*].
   ASK_* replies with [i16 selection]; the rest reply empty. */
enum mp_audience_sub_e {
    MP_AUD_START = 0, MP_AUD_SHOW1 = 1, MP_AUD_SHOW2 = 2, MP_AUD_SHOW3 = 3,
    MP_AUD_ASK2A = 4, MP_AUD_ASK2B = 5, MP_AUD_ASK3 = 6, MP_AUD_ASK4 = 7,
    MP_AUD_NEWTECH = 8, MP_AUD_END = 9
};

/* Shared pre-game lobby state. The server owns it and broadcasts the whole struct (MP_MSG_LOBBY)
   on every change; clients edit their own fields via MP_MSG_LOBBY_PICK [u8 field][u8 value].
   Fixed-size plain bytes so it ships by memcpy (engine-agnostic: races/banners/galaxy size are
   just small ints here, mapped to game enums in the setup_game adapter). */
struct mp_lobby_s {
    uint8_t num_humans;   /* connected human players (= client count, fixed at host start) */
    uint8_t num_ai;       /* host-set: AI opponents (0 .. MP_MAX_PLAYERS - num_humans) */
    uint8_t galaxy_size;  /* host-set: galaxy size (0..5) */
    uint8_t difficulty;   /* host-set: AI difficulty (0..4) — global, all AIs share it */
    struct {
        uint8_t race;     /* 0xff = not yet chosen */
        uint8_t banner;   /* 0xff = not yet chosen */
        uint8_t ready;    /* 0/1 */
        uint8_t connected;/* 0/1 */
        uint8_t team;     /* 0 = free-for-all (no team); 1..6 = team number (teammates start allied) */
    } slot[MP_MAX_PLAYERS];
};

/* fields a client may set via MP_MSG_LOBBY_PICK. NUM_AI/GALAXY are accepted only from slot 0 (host). */
enum mp_lobby_field_e {
    MP_LOBBY_F_RACE   = 0, /* value = race (0..RACE_NUM-1) */
    MP_LOBBY_F_BANNER = 1, /* value = banner (0..BANNER_NUM-1) */
    MP_LOBBY_F_READY  = 2, /* value = 0/1 */
    MP_LOBBY_F_NUM_AI = 3, /* host only */
    MP_LOBBY_F_GALAXY = 4, /* host only */
    MP_LOBBY_F_AI_RACE= 5, /* host only: value = (ai_slot << 4) | race -- set an AI empire's race */
    MP_LOBBY_F_DIFFICULTY = 6, /* host only: value = difficulty 0..4 */
    MP_LOBBY_F_TEAM = 7,       /* value = (slot << 4) | team -- set a player's team (own slot, or host any) */
};

/* The game, abstracted. ctx is opaque (the real impl passes a struct game_s *). */
typedef struct mp_game_iface_s {
    void *ctx;
    /* write authoritative state into buf (<= buflen); return byte length or <0 */
    int (*serialize)(void *ctx, uint8_t *buf, int buflen);
    /* load authoritative state from buf; return 0 ok, <0 error */
    int (*deserialize)(void *ctx, const uint8_t *buf, int len);
    /* resolve exactly one turn (apply orders already set, run movement/combat/etc.);
       return 0 ok, <0 error. May be NULL on the client. */
    int (*advance_turn)(void *ctx);
    /* current turn/year, for logging/validation */
    int (*turn_number)(void *ctx);
    /* max bytes serialize may need (>= a full state blob) */
    int (*max_blob_len)(void *ctx);
    /* client: take player_id's turn and serialize its orders into buf; return
       length or <0. NULL => spectator (submits no orders). */
    int (*write_orders)(void *ctx, int player_id, uint8_t *buf, int buflen);
    /* server: apply player_id's submitted orders to the authoritative game; return
       count applied or <0. NULL => orders ignored. */
    int (*apply_orders)(void *ctx, int player_id, const uint8_t *buf, int len);
    /* client: called after each GAME_DATA load to prepare the game for play/display
       (recompute derived values; first!=0 on the initial load). NULL => skip. */
    void (*on_state_loaded)(void *ctx, int first);
    /* server: after advance_turn, copy the snapshot of state-as-of-movement-start
       into buf; return length (0 = nothing to animate). Streamed as TURN_MOVE so
       clients can play the fleet movement. NULL => no movement playback. */
    int (*get_movement)(void *ctx, uint8_t *buf, int buflen);
    /* client: a TURN_MOVE blob arrived — load it and animate the fleet movement
       from player_id's perspective (then the authoritative GAME_DATA follows).
       NULL => ignore TURN_MOVE. */
    void (*play_movement)(void *ctx, int player_id, const uint8_t *buf, int len);
    /* client: called repeatedly while blocked on the network so the UI can pump
       events + show a "waiting" frame (reason: MP_WAIT_*). NULL => just sleep. */
    void (*on_wait)(void *ctx, int reason);
    /* client: a DECISION_REQ arrived mid-resolution — run the matching UI, write the
       answer into resp (<= resp_buflen), return its length (<0 = error). NULL => no
       answer (0-length), i.e. the server falls back to a default. */
    int (*handle_decision)(void *ctx, int dtype, const uint8_t *req, int req_len, uint8_t *resp, int resp_buflen);
    /* client: a SPECTATE update arrived — re-render the battle from it (no reply). NULL => ignore. */
    void (*on_spectate)(void *ctx, const uint8_t *data, int len);
    /* client: run the interactive pre-game lobby (pick race/color, ready up, host sets AI count &
       galaxy size). Loops until the server starts the game, pumping the lobby via g_mp_cl_lobby_*.
       my_id = this player's slot. Return 0 when the game starts (state already loaded), <0 to quit.
       NULL => no lobby (legacy: the game was created up front). */
    int (*run_lobby)(void *ctx, int my_id);
    /* server: all players are ready; create the game from the final lobby (per-slot race/banner,
       AI count, galaxy size). Called once, before the initial state is serialized. NULL => the game
       was already created up front (legacy). Return 0 ok, <0 error. */
    int (*setup_game)(void *ctx, const struct mp_lobby_s *lobby);
} mp_game_iface_t;

/* server-side hook: when non-NULL (set while mp_server_run is active), the null UI
   calls this to ask player_id's client a mid-resolution decision and BLOCK for the
   answer. Writes the answer into resp, returns its length (<0 on error/disconnect).
   NULL outside MP (the null UI then returns its own default). */
extern int (*g_mp_decision_hook)(int player_id, int dtype, const void *req, int req_len, void *resp, int resp_buflen);

/* like g_mp_decision_hook but asks N players the SAME request in PARALLEL (all requests
   sent first, then all answers collected) so e.g. both sides see a battle's autoresolve
   prompt at once. resps is N slots of resp_stride bytes; returns 0 ok, <0 on error. */
extern int (*g_mp_decision_hook_multi)(const int *players, int n, int dtype, const void *req, int req_len, void *resps, int resp_stride);

/* server -> a watching client: push a battle update (fire-and-forget, no reply) so the
   player not currently acting can watch the battle progress. NULL outside MP. */
extern void (*g_mp_spectate_hook)(int player_id, const void *data, int len);

/* server -> all clients: broadcast this turn's movement-replay snapshot (MP_MSG_TURN_MOVE) at the
   moment it is captured (movement-start), so clients animate fleet movement BEFORE the combat / bomb
   / ground result screens that fire later in resolution. NULL outside an active server. */
extern void (*g_mp_movement_hook)(const uint8_t *buf, int len);

/* client-side soft-ready primitives, set by mp_client_run only while a soft-ready turn is in
   progress (NULL otherwise). The UI adapter calls these: _send_ready submits the player's
   latest orders + ready flag; _poll services the socket once (non-blocking) and returns 1
   once the server has resolved the turn (the new state has already been applied). */
extern void (*g_mp_cl_send_ready)(int ready, const uint8_t *orders, int olen);
extern int (*g_mp_cl_poll)(void);

/* client-side lobby primitives, set by mp_client_run only while the pre-game lobby is active (NULL
   otherwise). _poll services the socket once (non-blocking): it copies the latest lobby state into
   *out and returns 1 once the game has started (the initial state is already loaded), else 0. _set
   sends one field change (see mp_lobby_field_e) for this player. */
extern int (*g_mp_cl_lobby_poll)(struct mp_lobby_s *out);
extern void (*g_mp_cl_lobby_set)(int field, int value);

/* client-side live-diplomacy primitives, set by mp_client_run only during a soft-ready turn (NULL
   otherwise). _recv dequeues one pending diplo message (0x20-0x2A): returns its payload length (>=0)
   and sets *id_out, or -1 if none are queued. _send transmits one diplo message to the server. */
extern int (*g_mp_cl_diplo_recv)(uint16_t *id_out, uint8_t *buf, int buflen);
extern void (*g_mp_cl_diplo_send)(uint16_t id, const uint8_t *data, int len);

/* on_wait reason codes */
#define MP_WAIT_LOBBY  0 /* waiting for the game to start / other players to join */
#define MP_WAIT_TURN   1 /* waiting for other players to finish their simultaneous turn */
#define MP_WAIT_COMBAT 3 /* waiting while another player resolves an interactive battle */
#define MP_WAIT_BATTLE 2 /* waiting during a battle (the other side's turn) — keep the arena, just pump */
#define MP_WAIT_COUNCIL 4 /* waiting while the Galactic Council is in session (another emperor voting) */

/* Run the authoritative server: accept num_clients, assign empires, broadcast
   state, then loop the simultaneous-turn barrier (wait all END_TURN, advance,
   rebroadcast) until max_turns (0 = until a client disconnects). Returns 0 ok. */
int mp_server_run(uint16_t port, int num_clients, int max_turns, const mp_game_iface_t *gi);

/* Run a client: connect, sync initial state, then each turn submit END_TURN and
   apply the rebroadcast state. (Phase A submits immediately; real client submits
   after the human plays.) Returns 0 ok. */
int mp_client_run(const char *host, uint16_t port, int max_turns, const mp_game_iface_t *gi);

/* the local client's player id (valid during an MP client session). */
extern int mp_cl_player_id(void);

#endif /* INC_1OOM_MP_H */
