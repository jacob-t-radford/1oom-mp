#ifndef INC_1OOM_GAME_DIPLO_H
#define INC_1OOM_GAME_DIPLO_H

#include "game_types.h"
#include "types.h"

struct game_s;

extern const int16_t game_diplo_tbl_reldiff[6];

extern void game_diplo_act(struct game_s *g, int dv, player_id_t pi, player_id_t pi2, int dtype, uint8_t pli1, int16_t dp2);
extern void game_diplo_break_treaty(struct game_s *g, player_id_t breaker, player_id_t victim);
extern void game_diplo_start_war(struct game_s *g, player_id_t pi1, player_id_t pi2);
extern void game_diplo_start_war_swap(struct game_s *g, player_id_t pi1, player_id_t pi2);
extern void game_diplo_break_trade(struct game_s *g, player_id_t pi/*breaker*/, player_id_t pi2);
extern void game_diplo_annoy(struct game_s *g, player_id_t pi1, player_id_t pi2, int n);
extern void game_diplo_battle_finish(struct game_s *g, int def, int att, int popdiff, uint32_t app_def, uint16_t biodamage, uint32_t app_att, planet_id_t planet_i);
extern void game_diplo_set_treaty(struct game_s *g, player_id_t pi1, player_id_t pi2, treaty_t treaty);
extern void game_diplo_set_trade(struct game_s *g, player_id_t pi1, player_id_t pi2, int bc);
extern void game_diplo_stop_war(struct game_s *g, player_id_t pi1, player_id_t pi2);
extern void game_diplo_esp_frame(struct game_s *g, player_id_t framed, player_id_t victim);

/* 1oom-mp teams: a team's foreign policy is decided by consensus. When a human in a team game tries
   to change the team's stance toward an enemy, game logic offers it to this hook FIRST; if the hook
   captures it (returns true) the change became a team proposal and the caller must NOT apply it.
   Returns false (apply normally) outside MP / team games, or when there are no human teammates to
   consult (AI teammates auto-follow). Installed by the classic UI; NULL elsewhere. */
typedef enum {
    MP_TEAM_STANCE_PEACE = 0, /* end a war (game_diplo_stop_war) */
    MP_TEAM_STANCE_NAP,       /* non-aggression pact */
    MP_TEAM_STANCE_BREAK,     /* break an existing treaty */
    MP_TEAM_STANCE_WAR,       /* declare war (human-enemy p2p only; AI war is via combat) */
    MP_TEAM_STANCE_ALLIANCE   /* form an alliance -- it binds the whole team (can drag it into wars), so it votes */
} mp_team_stance_t;
extern bool (*g_mp_team_stance_propose_hook)(struct game_s *g, player_id_t proposer, player_id_t enemy, mp_team_stance_t stance);
extern void game_diplo_limit_mood_treaty(struct game_s *g);
extern void game_diplo_mood_relax(struct game_s *g);
extern int16_t game_diplo_get_mood(struct game_s *g, player_id_t p1, player_id_t p2);
extern bool game_diplo_is_gone(struct game_s *g, player_id_t api, player_id_t pi);

#endif
