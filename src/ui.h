#ifndef INC_1OOM_UI_H
#define INC_1OOM_UI_H

/* API to ui/ */

#include "cfg.h"
#include "options.h"
#include "types.h"

extern const char *idstr_ui;

extern void ui_early_show_message_box(const char *msg);

extern int ui_early_init(void);
extern int ui_init(void);
extern int ui_late_init(void);
extern void ui_shutdown(void);

extern bool ui_use_audio;

extern const struct cmdline_options_s ui_cmdline_options[];
extern const struct cfg_items_s ui_cfg_items[];

#define UI_STRBUF_SIZE 1024
extern char *ui_get_strbuf(void);

extern void ui_play_intro(void);
extern void ui_play_ending_good(int race, const char *name);
extern void ui_play_ending_tyrant(int race, const char *name);
extern void ui_play_ending_funeral(int banner_live, int banner_dead);
extern void ui_play_ending_exile(const char *name);

typedef enum {
    MAIN_MENU_ACT_NEW_GAME,
    MAIN_MENU_ACT_CUSTOM_GAME,
    MAIN_MENU_ACT_CHALLENGE_GAME,
    MAIN_MENU_ACT_LOAD_GAME,
    MAIN_MENU_ACT_LOAD_GAME_MOO13,
    MAIN_MENU_ACT_CONTINUE_GAME,
    MAIN_MENU_ACT_QUIT_GAME,
    MAIN_MENU_ACT_TUTOR
} main_menu_action_t;

struct game_new_options_s;

extern main_menu_action_t ui_main_menu(struct game_new_options_s *newopts, struct game_new_options_s *customopts, struct game_new_options_s *challengeopts, int *load_game_i_ptr);

struct game_s;
struct game_end_s;

extern void ui_game_start(struct game_s *g);
extern void ui_game_end(struct game_s *g);

/* 1oom-mp: present a "waiting" frame while the client is blocked on the network,
   pumping input so the window stays responsive. reason: 0 = lobby/waiting to start,
   nonzero = waiting for other players to finish their simultaneous turn. */
extern void ui_mp_wait(int reason);

/* 1oom-mp: render a battle for a non-acting (spectating) player, with a banner. */
struct battle_s;
extern void ui_mp_battle_spectate(const struct battle_s *bt);

/* 1oom-mp battle-spectate event kinds. A spectate message that is exactly sizeof(battle_s) is a
   full state snapshot; anything smaller is one of these attack-animation events ([u8 kind] followed
   by int16 args) that the server's null UI emits and the spectator replays on its current battle,
   so the watcher sees the beam/bomb/etc. fire rather than just the before/after states. */
enum ui_mp_spec_e {
    UI_MP_SPEC_BEAM = 1,    /* args: attacker_i, target_i, wpni */
    UI_MP_SPEC_BOMB,        /* args: attacker_i, target_i, bombtype */
    UI_MP_SPEC_STASIS,      /* args: attacker_i, target_i */
    UI_MP_SPEC_STREAM1,     /* args: attacker_i, target_i */
    UI_MP_SPEC_STREAM2,     /* args: attacker_i, target_i */
    UI_MP_SPEC_BLACKHOLE,   /* args: attacker_i, target_i */
    UI_MP_SPEC_TECHNULL,    /* args: attacker_i, target_i */
    UI_MP_SPEC_REPULSE,     /* args: attacker_i, target_i, sx, sy */
    UI_MP_SPEC_RETREAT,     /* args: cur_item (the retreating ship) */
    UI_MP_SPEC_MOVE,        /* args: itemi, dest_sx, dest_sy — glide the ship to the destination */
    UI_MP_SPEC_DAMAGE,      /* args: target_i, target_x, target_y, dmg_lo, dmg_hi — hit/explosion */
    UI_MP_SPEC_COUNCIL,     /* council frame: [kind][struct election_s] — the shared council view, relayed to ALL humans */
    UI_MP_SPEC_COUNCIL_END, /* council adjourned: [kind] only — tear down the spectator's council view */
    UI_MP_SPEC_MISSILE,     /* missile flight: [kind][i16 missilei,x,y,tx,ty][struct battle_missile_s] — animate the missile traveling (the struct rides along since missiles launch after the last snapshot) */
};

/* 1oom-mp: glide a ship from its current hex to (sx,sy) on the spectator's battle screen (replays
   the move the server resolved). Self-paced (classic UI). No-op in the headless/cmdline UIs. */
extern void ui_mp_battle_glide(struct battle_s *bt, int itemi, int sx, int sy);

/* 1oom-mp soft-ready turn model. Set by the MP client adapter (game.c) only while a
   networked turn is being played; all NULL in single-player and for the legacy immediate-
   submit path. When ui_mp_turn_active() is true the starmap "Next Turn" button locks in your
   orders (submit + mark ready) instead of ending the turn: you keep browsing the map, editing
   any order clears the lock, and the turn resolves once every player is ready. */
extern bool (*ui_mp_turn_active)(void);          /* is a soft-ready MP turn in progress? */
extern void (*ui_mp_turn_set_ready)(int ready);  /* submit current orders + set/clear my ready */
extern bool (*ui_mp_turn_is_ready)(void);        /* am I currently locked in (for the banner)? */
extern bool (*ui_mp_turn_poll)(void);            /* pump the net once; true once the turn resolved */

/* 1oom-mp interactive pre-game lobby: pick race/color, ready up, and (host = slot 0) set the AI
   count and galaxy size. Loops until the game starts, pumping the shared state via g_mp_cl_lobby_*.
   my_id = this player's slot. Returns 0 once the game starts (state loaded), <0 if the player quit. */
extern int ui_mp_lobby_run(int my_id);

/* 1oom-mp live human-to-human diplomacy (classic UI). _pump is called each starmap frame: it drains
   the diplo inbox and returns true when an incoming audience request should be surfaced, at which
   point the main loop runs _handle (the responder side: receive-now/decline, then the live session).
   The proposer side runs inline from the AUDIENCE action. NULL transport (single-player) is a no-op. */
extern bool ui_mp_diplo_pump(int pi);
extern void ui_mp_diplo_handle(struct game_s *g, int pi);
/* 1oom-mp live teammate visibility: _tick streams my plan to teammates each planning frame; the
   starmap overlay reads teammates' relayed fleets via _active / _fleet_total / _fleet_get. */
extern void ui_mp_team_plan_tick(void);
extern bool ui_mp_team_plan_active(int player);
extern int ui_mp_team_plan_fleet_total(void);
extern bool ui_mp_team_plan_fleet_get(int idx, int *owner, int *x, int *y, int *dest);

/* 1oom-mp: true when this classic UI is running as a networked client (set by game.c's
   game_mp_join). Used to enable client-side turn-start prompts that the headless server
   resolves silently (e.g. the planet-discovery notification). False in single-player. */
extern bool ui_mp_active;

extern void ui_sound_play_sfx(int sfxi);

typedef enum {
    UI_TURN_ACT_NEXT_TURN,
    UI_TURN_ACT_LOAD_GAME,
    UI_TURN_ACT_QUIT_GAME
} ui_turn_action_t;

extern ui_turn_action_t ui_game_turn(struct game_s *g, int *load_game_i_ptr, int pi);

extern void *ui_gmap_basic_init(struct game_s *g, bool show_player_switch);
extern void ui_gmap_basic_start_player(void *ctx, int pi);
extern void ui_gmap_basic_start_frame(void *ctx, int pi);
extern void ui_gmap_basic_draw_frame(void *ctx, int pi);
extern void ui_gmap_basic_finish_frame(void *ctx, int pi);
extern void ui_gmap_basic_shutdown(void *ctx);

extern uint8_t *ui_gfx_get_ship(int look);
extern uint8_t *ui_gfx_get_planet(int look);
extern uint8_t *ui_gfx_get_rock(int look);

struct battle_s;

#define UI_BATTLE_ACT_CLICK(_x_, _y_)    BATTLE_XY_SET((_x_), (_y_))
#define UI_BATTLE_ACT_GET_X(_v_)    BATTLE_XY_GET_X(_v_)
#define UI_BATTLE_ACT_GET_Y(_v_)    BATTLE_XY_GET_Y(_v_)
#define UI_BATTLE_ACT_NONE      0x80
#define UI_BATTLE_ACT_WAIT      0x81
#define UI_BATTLE_ACT_DONE      0x82
#define UI_BATTLE_ACT_RETREAT   0x83
#define UI_BATTLE_ACT_AUTO      0x84
#define UI_BATTLE_ACT_MISSILE   0x85
#define UI_BATTLE_ACT_PLANET    0x86
#define UI_BATTLE_ACT_SCAN      0x87
#define UI_BATTLE_ACT_SPECIAL   0x88

typedef uint8_t ui_battle_action_t;

typedef enum {
    UI_BATTLE_AUTORESOLVE_OFF,
    UI_BATTLE_AUTORESOLVE_AUTO,
    UI_BATTLE_AUTORESOLVE_RETREAT
} ui_battle_autoresolve_t;

typedef enum {
    UI_BATTLE_BOMB_BOMB,
    UI_BATTLE_BOMB_BIO,
    UI_BATTLE_BOMB_WARPDIS
} ui_battle_bomb_t;

extern ui_battle_autoresolve_t ui_battle_init(struct battle_s *bt);
extern void ui_battle_shutdown(struct battle_s *bt, bool colony_destroyed, int winner);

/* 1oom-mp: end-of-turn consolidated combat report. One record per auto-resolved space battle, carrying
   both sides' per-design ship losses (by sprite `look`) so each client renders it from its own side.
   Replayed concurrently at state load in place of the per-battle result screens. */
struct ui_combat_ships_s { uint8_t look; uint8_t hull; uint16_t before; uint16_t after; };
struct ui_combat_report_s {
    uint16_t planet_i;       /* >= galaxy_stars -> deep space */
    int16_t winner_party;    /* winning party id, or -1 */
    uint8_t party[2];        /* s[SIDE_L].party, s[SIDE_R].party */
    uint8_t nitems[2];       /* distinct designs present at battle start, per side */
    struct ui_combat_ships_s ships[2][6]; /* NUM_SHIPDESIGNS: before/after counts per design */
    uint16_t bases_before[2]; /* defending planet's missile bases at battle start, per side */
    uint16_t bases_after[2];  /* ...and at battle end (only the planet-owner side is non-zero) */
};
extern void ui_combat_report(struct game_s *g, int pi, const struct ui_combat_report_s *reps, int n);

extern void ui_battle_draw_misshield(const struct battle_s *bt, int target_i, int target_x, int target_y, int missile_i);
extern void ui_battle_draw_damage(const struct battle_s *bt, int target_i, int target_x, int target_y, uint32_t damage);
extern void ui_battle_draw_explos_small(const struct battle_s *bt, int x, int y);
extern void ui_battle_draw_basic(const struct battle_s *bt);
extern void ui_battle_draw_basic_copy(const struct battle_s *bt);
extern void ui_battle_draw_missile(const struct battle_s *bt, int missilei, int x, int y, int tx, int ty);
extern void ui_battle_draw_cloaking(const struct battle_s *bt, int from, int to, int sx, int sy);
extern void ui_battle_draw_arena(const struct battle_s *bt, int itemi, int dmode);
extern void ui_battle_draw_item(const struct battle_s *bt, int itemi, int x, int y);
extern void ui_battle_draw_bomb_attack(const struct battle_s *bt, int attacker_i, int target_i, ui_battle_bomb_t bombtype);
extern void ui_battle_draw_beam_attack(const struct battle_s *bt, int attacker_i, int target_i, int wpni);
extern void ui_battle_draw_stasis(const struct battle_s *bt, int attacker_i, int target_i);
extern void ui_battle_draw_pulsar(const struct battle_s *bt, int attacker_i, int ptype, const uint32_t *dmgtbl);
extern void ui_battle_draw_stream1(const struct battle_s *bt, int attacker_i, int target_i);
extern void ui_battle_draw_stream2(const struct battle_s *bt, int attacker_i, int target_i);
extern void ui_battle_draw_blackhole(const struct battle_s *bt, int attacker_i, int target_i);
extern void ui_battle_draw_technull(const struct battle_s *bt, int attacker_i, int target_i);
extern void ui_battle_draw_repulse(const struct battle_s *bt, int attacker_i, int target_i, int sx, int sy);
extern void ui_battle_draw_retreat(const struct battle_s *bt);
extern void ui_battle_draw_bottom(const struct battle_s *bt);
extern void ui_battle_draw_planetinfo(const struct battle_s *bt, bool side_r);
extern void ui_battle_draw_scan(const struct battle_s *bt, bool side_r);
extern void ui_battle_draw_finish(const struct battle_s *bt);

extern void ui_battle_area_setup(const struct battle_s *bt);
extern void ui_battle_turn_pre(const struct battle_s *bt);
extern void ui_battle_turn_post(const struct battle_s *bt);
extern ui_battle_action_t ui_battle_turn(const struct battle_s *bt);
extern void ui_battle_ai_pre(const struct battle_s *bt);
extern bool ui_battle_ai_post(const struct battle_s *bt);

extern int ui_spy_steal(struct game_s *g, int spy, int target, uint8_t flags_field);
extern void ui_spy_stolen(struct game_s *g, int pi, int spy, int field, uint8_t tech);

typedef enum {
    UI_SABOTAGE_FACT, /*0*/
    UI_SABOTAGE_BASES, /*1*/
    UI_SABOTAGE_REVOLT, /*2*/
    UI_SABOTAGE_NONE /*-1*/
} ui_sabotage_t;

extern ui_sabotage_t ui_spy_sabotage_ask(struct game_s *g, int spy, int target, planet_id_t *planetptr);
extern int ui_spy_sabotage_done(struct game_s *g, int pi, int spy, int target, ui_sabotage_t act, int other1, int other2, planet_id_t planet, int snum);

extern void ui_newtech(struct game_s *g, int pi);

extern bool ui_explore(struct game_s *g, int pi, planet_id_t planet_i, bool by_scanner, bool flag_colony_ship);
extern void ui_landing(struct game_s *g, int pi, planet_id_t planet_i);

extern bool ui_bomb_ask(struct game_s *g, int pi, planet_id_t planet_i, int pop_inbound);
extern void ui_bomb_show(struct game_s *g, int pi, int attacker_i, int owner_i, planet_id_t planet_i, int popdmg, int factdmg, bool play_music, bool hide_other);
/* 1oom-mp: one human attacker's bombable target, for the parallel batched bomb prompt */
struct ui_bomb_target_s { uint16_t planet_i; uint8_t attacker; uint16_t pop_inbound; uint16_t popdmg; uint16_t factdmg; };
/* Ask about all listed targets at once. In MP, fans out to every human attacker IN PARALLEL and fills
   decided[k] for each target; returns true. In single-player it returns false and the caller falls
   back to the per-planet ui_bomb_ask. */
extern bool ui_bomb_ask_batch(struct game_s *g, const struct ui_bomb_target_s *targets, int n, bool *decided);
/* 1oom-mp: one human spymaster's sabotage opportunity (player vs target) + its decision (act + planet) */
struct ui_spy_sab_target_s { uint8_t player; uint8_t target; };
struct ui_spy_sab_dec_s { int16_t act; uint16_t planet; };
/* Ask about all listed sabotage opportunities at once. In MP, fans out to every human spymaster IN
   PARALLEL and fills out[k] (act+planet) for each; returns true. In single-player it returns false
   and the caller falls back to the per-target ui_spy_sabotage_ask. */
extern bool ui_spy_sabotage_batch(struct game_s *g, const struct ui_spy_sab_target_s *targets, int n, struct ui_spy_sab_dec_s *out);
/* 1oom-mp: one human spy's tech-steal opportunity (spy vs target) + the available-fields bitmask. */
struct ui_spy_steal_target_s { uint8_t spy; uint8_t target; uint8_t flags; };
/* Ask about all listed steal opportunities at once. In MP, fans out to every human spy IN PARALLEL and
   fills out[k] (chosen tech field, -1 = none) for each; returns true. In single-player it returns false
   and the caller falls back to the per-target ui_spy_steal. */
extern bool ui_spy_steal_batch(struct game_s *g, const struct ui_spy_steal_target_s *targets, int n, int16_t *out);
/* 1oom-mp: non-blocking variant of ui_spy_sabotage_done for the no-framing-choice case. In MP the client
   buffers + replays it at state load (concurrent, never blocks the other player); in single-player it
   shows the result screen immediately (just ui_spy_sabotage_done, return ignored). */
extern void ui_spy_sabotage_show(struct game_s *g, int pi, int spy, int target, ui_sabotage_t act, int other1, int other2, planet_id_t planet, int snum);

extern void ui_turn_pre(const struct game_s *g);
extern void ui_turn_msg(struct game_s *g, int pi, const char *str);

struct ground_s;
extern void ui_ground(struct game_s *g, struct ground_s *gr);

struct news_s;
extern void ui_news_start(void);
extern void ui_news(struct game_s *g, struct news_s *ns);
extern void ui_news_end(void);

struct election_s;
extern void ui_election_start(struct election_s *el);
extern void ui_election_show(struct election_s *el);
extern void ui_election_delay(struct election_s *el, int delay);
extern int ui_election_vote(struct election_s *el, int player_i);
extern bool ui_election_accept(struct election_s *el, int player_i);
extern void ui_election_end(struct election_s *el);
/* 1oom-mp: build/free the election gfx context for a relayed (uictx=NULL) election decision */
extern void ui_election_ctx_load(struct election_s *el);
extern void ui_election_ctx_free(struct election_s *el);
/* 1oom-mp: render one frame of the council chamber from a relayed (uictx-set) election -- the
   shared synchronous council view shown to all non-voting players. */
extern void ui_election_spectate(struct election_s *el);

struct audience_s;
extern void ui_audience_start(struct audience_s *au);
extern void ui_audience_show1(struct audience_s *au);
extern void ui_audience_show2(struct audience_s *au);
extern void ui_audience_show3(struct audience_s *au);
extern int16_t ui_audience_ask2a(struct audience_s *au);
extern int16_t ui_audience_ask2b(struct audience_s *au);
extern int16_t ui_audience_ask3(struct audience_s *au);
extern int16_t ui_audience_ask4(struct audience_s *au);
extern void ui_audience_newtech(struct audience_s *au, int pi);
extern void ui_audience_end(struct audience_s *au);

extern void ui_newships(struct game_s *g, int pi);

extern void ui_copyprotection_check(struct game_s *g);
extern void ui_copyprotection_lose(struct game_s *g, struct game_end_s *ge);

#endif
