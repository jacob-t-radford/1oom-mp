#ifndef INC_1OOM_GAME_MP_ORDERS_H
#define INC_1OOM_GAME_MP_ORDERS_H

#include "types.h"

struct game_s;

/* 1oom-mp Phase B: serialize/apply a player's per-turn production orders (the
   per-planet sliders SHIP/DEF/IND/ECO/TECH + locks + build choice + relocation).
   This is the core of "playing your empire's economy". Fleets/diplomacy/spy later. */

/* Write player pi's owned-planet production orders into buf. Returns length or <0.
   Side-effect-free: safe to call repeatedly (the soft-ready client does, to detect changes). */
extern int game_mp_write_orders(const struct game_s *g, player_id_t pi, uint8_t *buf, int buflen);
extern int game_mp_write_team_plan(const struct game_s *g, player_id_t pi, uint8_t *buf, int buflen);

/* Clear the diplo/colonize actions queued last turn. Call at the start of each turn. */
extern void game_mp_orders_reset(void);

/* Apply orders for player pi onto g. Only planets pi actually owns are touched
   (anti-cheat). Returns number of planets applied, or <0 on malformed input. */
extern int game_mp_apply_orders(struct game_s *g, player_id_t pi, const uint8_t *buf, int len);

/* ---- human-to-human diplomacy actions, carried in the order stream ---- */
enum game_mp_diplo_verb_e {
    MP_DIPLO_NONE = 0,
    MP_DIPLO_DECLARE_WAR,       /* actor -> target: bilateral war (unilateral, no reply) */
    MP_DIPLO_BREAK_TREATY,      /* actor breaks its treaty with target (unilateral) */
    MP_DIPLO_PROPOSE_NAP,       /* actor proposes; target answers next turn */
    MP_DIPLO_PROPOSE_ALLIANCE,
    MP_DIPLO_PROPOSE_PEACE,
    MP_DIPLO_ACCEPT,            /* actor accepts target's pending proposal (arg = the PROPOSE_* verb) */
    MP_DIPLO_REJECT,            /* actor rejects target's pending proposal */
    MP_DIPLO_PROPOSE_TRADE,     /* consensual: p[0..1] = BC/yr amount (u16 LE) */
    MP_DIPLO_PROPOSE_TECH,      /* consensual: p = {want_field, want_tech, give_field, give_tech} (proposer's view) */
    MP_DIPLO_TRIBUTE_BC,        /* unilateral gift: p[0..1] = BC amount (u16 LE) from actor to target */
    MP_DIPLO_TRIBUTE_TECH,      /* unilateral gift: p = {field, tech} from actor to target */
    MP_DIPLO_BREAK_TRADE,       /* unilateral: cancel the trade agreement with target */
};

/* client: queue a diplomacy action by the local human (drained by write_orders).
   `arg` carries extra data (for ACCEPT: the PROPOSE_* verb being accepted, since the
   proposal mailbox is cleared by turn processing before the response resolves).
   Returns true if recorded. */
extern bool game_mp_diplo_record(player_id_t actor, player_id_t target, uint8_t verb, uint8_t arg);

/* like game_mp_diplo_record but also carries up to 4 parameter bytes (trade BC amount, tech
   field/tech ids). p may be NULL (treated as all-zero). */
extern bool game_mp_diplo_record_p(player_id_t actor, player_id_t target, uint8_t verb, uint8_t arg, const uint8_t *p);

/* diplo_type[] sentinel marking a pending human-to-human proposal (the proposed
   PROPOSE_* verb is stored alongside in diplo_val[]). Out of range of every value
   game_turn_process acts on, so it's only ever read by our own code then cleared. */
#define MP_DIPLO_PROPOSAL_MARK 200

/* server: apply the diplomacy actions parsed from this turn's orders. MUST run
   AFTER game_turn_process — it sets treaties / queues proposals into state that
   turn processing would otherwise read and clear. */
extern void game_mp_diplo_apply_pending(struct game_s *g);

/* ---- 1oom-mp: planning-phase ECONOMY actions, carried in the order stream. These fix the
   "did it locally, reverted at end of turn" class for actions whose effects live outside the
   serialized planning fields: treasury refunds/gifts, missile-base scrap, and the economic side
   of a client-side audience with an AI (trade agreement, tech exchange, tribute, relations).
   The record functions no-op outside a live MP client, so SP and the server are unaffected. ---- */
extern void game_mp_econ_record_bc(player_id_t actor, int bc);                 /* treasury delta (scrap refund +, payment -) */
extern void game_mp_econ_record_scrap_bases(player_id_t actor, planet_id_t pli, int n); /* server recomputes the refund */
extern void game_mp_econ_record_trade(player_id_t actor, player_id_t pa, int bc);       /* trade agreement set with AI pa */
extern void game_mp_econ_record_tech_from(player_id_t actor, player_id_t pa, uint8_t field, uint8_t tech); /* AI gave us a tech */
extern void game_mp_econ_record_tech_to(player_id_t actor, player_id_t pa, uint8_t field, uint8_t tech);   /* we gave AI a tech */
extern void game_mp_econ_record_tribute(player_id_t actor, player_id_t pa, int bc);      /* BC from us to AI pa */
extern void game_mp_econ_record_relation(player_id_t actor, player_id_t pa, int rel);    /* audience net relation result */

/* client: queue a request to colonize planet pli (drained by write_orders). */
extern void game_mp_colonize_record(player_id_t pi, planet_id_t pli);

/* server: resolve this turn's colonize requests (with a fair random tie-break for a
   contested planet). Run BEFORE game_turn_process so the new colony plays this turn. */
extern void game_mp_colonize_apply_pending(struct game_s *g);

#endif
