#include "config.h"

#include <stdlib.h> /* abs */

#include "game_diplo.h"
#include "comp.h"
#include "game.h"
#include "game_ai.h"
#include "game_num.h"
#include "game_planet.h"
#include "game_spy.h"
#include "game_tech.h"
#include "rnd.h"
#include "types.h"

/* -------------------------------------------------------------------------- */

const int16_t game_diplo_tbl_reldiff[6] = { -50, -30, -20, 0, 10, 20 };

/* -------------------------------------------------------------------------- */

/* 1oom-mp: stances are team-to-team. game_diplo_team_sync_stance() mirrors a just-changed treaty
   between two empires onto every cross-team member pair, so war/peace/NAP is whole-team-vs-whole-team
   (trade stays per-empire). It is called from the diplo primitives below and re-enters them for the
   teammate pairs; s_diplo_in_team_sync makes those re-entrant calls skip their own sync, so one
   top-level call converges every pair exactly once. No-op outside team games. */
static bool s_diplo_in_team_sync = false;
static void game_diplo_team_sync_stance(struct game_s *g, player_id_t pi, player_id_t pi2);

/* 1oom-mp teams: foreign-policy-by-consensus hook (see game_diplo.h). Installed by the classic UI. */
bool (*g_mp_team_stance_propose_hook)(struct game_s *g, player_id_t proposer, player_id_t enemy, mp_team_stance_t stance) = NULL;

/* -------------------------------------------------------------------------- */

void game_diplo_act(struct game_s *g, int dv, player_id_t pi, player_id_t pi2, int dtype, uint8_t pli1, int16_t dp2)
{
    empiretechorbit_t *e = &(g->eto[pi]);
    empiretechorbit_t *e2 = &(g->eto[pi2]);
    int v1;
    if (g->end == GAME_END_FINAL_WAR) {
        if (IS_AI(g, pi) && (dv < 0) && IS_HUMAN(g, pi2) && BOOLVEC_IS1(g->refuse, pi2) && (rnd_1_n(100, &g->seed) <= (dv / -10))) {
            player_id_t i = g->winner;
            e2->diplo_val[i] = 100;
            e2->diplo_type[i] = 59;
            e2->diplo_p2[i] = dp2;
            e2->diplo_p1[i] = pli1;
        }
        return;
    }
    if (0
      || (pi >= g->players) || (pi2 >= g->players)
      || (pi == pi2) || (dv == 0) || IS_HUMAN(g, pi2)
      || (e->treaty[pi2] == TREATY_FINAL_WAR)
    ) {
        return;
    }
    if (dv == -10000) {
        if (IS_HUMAN(g, pi) && (dtype != 0)) {
            e->diplo_val[pi2] = 10000;
            e->diplo_type[pi2] = dtype;
            e->diplo_p2[pi2] = dp2;
            e->diplo_p1[pi2] = pli1;
        }
        return;
    }
    if (dv > 0) {
        if (e2->relation1[pi] < 0) {
            dv *= 2;
            if ((e2->relation1[pi] + dv) > 10) {
                dv = 10 - e2->relation1[pi];
            }
        } else {
            int v4 = e2->relation1[pi] / 25 + 1;
            if (v4 == 0) {
                v4 = 1;
            }
            dv /= v4;
        }
    } else {
        if (e2->relation1[pi] > 0) {
            dv *= 2;
        } else {
            int v4 = e2->relation1[pi] / -25 + 1;
            if (v4 == 0) {
                v4 = 1;
            }
            dv /= v4;
        }
    }
    if ((e2->trait1 == TRAIT1_HONORABLE) && (dv < 0)) {
        dv *= 2;
    }
    if ((e2->trait1 == TRAIT1_XENOPHOBIC) && (dv < 0)) {
        dv = (dv * 3) / 2;
    }
    if ((e2->trait1 == TRAIT1_XENOPHOBIC) && (dv > 0)) {
        dv = (dv * 3) / 4;
    }
    v1 = dv;
    if ((e->race == RACE_HUMAN) && (dv > 0)) {
        dv *= 2;
    }
    if ((e2->treaty[pi] == TREATY_WAR) && (dtype >= 4) && (dtype <= 12) && (dv < 0)) {
        if (IS_HUMAN(g, pi)) {
            e2->mood_peace[pi] += dv / 4;
            e->mood_peace[pi2] -= dv / 4;
        }
        dv = 0;
    }
    if (IS_AI(g, pi) && IS_AI(g, pi2) && (g->year > 100)) {
        if (dv > 0) {
            dv *= 2;
        }
        if (dv < 0) {
            dv /= (g->difficulty / 2 + 1);
        }
    }
    if (dv != 0) {
        int v = e2->relation1[pi];
        v += dv;
        SETRANGE(v, -100, 100);
        e2->relation1[pi] = v;
        e2->mood_treaty[pi] += dv;
        e->mood_treaty[pi2] = e2->mood_treaty[pi];
        e2->mood_trade[pi] += dv;
        e->mood_trade[pi2] = e2->mood_trade[pi];
        e2->mood_tech[pi] += dv;
        e->mood_tech[pi2] = e2->mood_tech[pi];
        if (v1 < 0) {
            e->mood_peace[pi2] -= v1 / 4;
            e2->mood_peace[pi] += v1 / 4;
        } else {
            e2->mood_peace[pi] += v1;
        }
        if (IS_HUMAN(g, pi) && (dtype != 0) && (abs(dv) > abs(e->diplo_val[pi2]))) {
            e->diplo_val[pi2] = dv;
            e->diplo_type[pi2] = dtype;
            e->diplo_p2[pi2] = dp2;
            e->diplo_p1[pi2] = pli1;
        }
    }
    if (e2->treaty[pi] >= TREATY_WAR) {
        SETMAX(e2->relation1[pi], -25);
    }
    if (e2->treaty[pi] != TREATY_ALLIANCE) {
        SETMIN(e2->relation1[pi], 65);
    }
    e->relation1[pi2] = e2->relation1[pi];
}

void game_diplo_break_treaty(struct game_s *g, player_id_t breaker, player_id_t victim)
{
    empiretechorbit_t *eb, *ev;
    int w;
    if ((breaker >= PLAYER_NUM) || (victim >= PLAYER_NUM)) {
        return;
    }
    eb = &(g->eto[breaker]);
    ev = &(g->eto[victim]);
    /* 1oom-mp: same-team empires are a locked alliance -- their treaty can never be broken. */
    if ((g->mp_team[breaker] != 0) && (g->mp_team[breaker] == g->mp_team[victim])) {
        return;
    }
    if (ev->treaty[breaker] >= TREATY_WAR) {
        return;
    }
    w = 0;
    if (eb->treaty[victim] == TREATY_NONAGGRESSION) {
        w = -10;
    }
    if (eb->treaty[victim] == TREATY_ALLIANCE) {
        w = -20;
    }
    if (g->ai_id != GAME_AI_CLASSIC) {
        w = -w;
    }
    if (eb->trait1 == TRAIT1_HONORABLE) {
        w *= 2;
    }
    eb->trust[victim] -= w;
    if (eb->treaty[victim] == TREATY_ALLIANCE) {
        int v = eb->relation2[victim];
        v -= w;
        SETMAX(v, -100);
        eb->relation2[victim] = v;
        ev->relation2[breaker] = v;
    }
    if (w != 0) {
        int v;
        eb->broken_treaty[victim] = eb->treaty[victim];
        if (g->ai_id == GAME_AI_CLASSIC) {
            /* WASBUG MOO1 blames both parties for breaking treaties */
            ev->broken_treaty[breaker] = eb->treaty[victim];
        }
        v = eb->relation1[victim] - rnd_1_n(20, &g->seed);
        SETMAX(v, -100);
        eb->relation1[victim] = v;
        ev->relation1[breaker] = v;
    }
    eb->treaty[victim] = TREATY_NONE;
    ev->treaty[breaker] = TREATY_NONE;
    eb->attack_bounty[victim] = PLAYER_NONE;
    ev->attack_bounty[breaker] = PLAYER_NONE;
    eb->mood_treaty[victim] = -200;
    eb->mood_trade[victim] = -200;
    eb->mood_tech[victim] = -200;
    eb->mood_peace[victim] = -200;
    ev->mood_treaty[breaker] = -200;
    ev->mood_trade[breaker] = -200;
    ev->mood_tech[breaker] = -200;
    ev->mood_peace[breaker] = -200;
    game_diplo_team_sync_stance(g, breaker, victim); /* 1oom-mp: break it with their whole team */
}

void game_diplo_start_war(struct game_s *g, player_id_t pi, player_id_t pi2)
{
    empiretechorbit_t *e, *e2;
    if ((pi >= PLAYER_NUM) || (pi2 >= PLAYER_NUM)) {
        return;
    }
    e = &(g->eto[pi]);
    e2 = &(g->eto[pi2]);
    /* 1oom-mp: same-team empires are a locked alliance -- they can never go to war. */
    if ((g->mp_team[pi] != 0) && (g->mp_team[pi] == g->mp_team[pi2])) {
        return;
    }
    if (IS_HUMAN(g, pi) && (e->have_met[pi2] == 0)) {
        e->have_met[pi2] = 1;
    }
    if (IS_HUMAN(g, pi2) && (e2->have_met[pi] == 0)) {
        e2->have_met[pi] = 1;
    }
    if (e->treaty[pi2] >= TREATY_WAR) {
        return;
    }
    /*6165b*/
    game_diplo_break_trade(g, pi, pi2);
    game_diplo_break_treaty(g, pi, pi2);
    e->relation2[pi2] -= 5;
    e2->relation2[pi] -= 5;
    e2->relation1[pi] = e->relation1[pi2] = -75 - rnd_1_n(25, &g->seed);
    e->treaty[pi2] = TREATY_WAR;
    e2->treaty[pi] = TREATY_WAR;
    e->mood_treaty[pi2] = -200;
    e->mood_trade[pi2] = -200;
    e->mood_tech[pi2] = -200;
    e->mood_peace[pi2] = -130;
    e2->mood_treaty[pi] = -200;
    e2->mood_trade[pi] = -200;
    e2->mood_tech[pi] = -200;
    e2->mood_peace[pi] = -130;
    game_diplo_team_sync_stance(g, pi, pi2); /* 1oom-mp: war is whole-team-vs-whole-team */
}

void game_diplo_start_war_swap(struct game_s *g, player_id_t pi, player_id_t pi2)
{
    if (g->ai_id != GAME_AI_CLASSIC) {
        /* WASBUG? game_diplo_start_war is called in a few places using odd order of parameters */
        player_id_t t = pi; pi = pi2; pi2 = t;
    }
    game_diplo_start_war(g, pi, pi2);
}

void game_diplo_break_trade(struct game_s *g, player_id_t pi, player_id_t pi2)
{
    empiretechorbit_t *e, *e2;
    if ((pi >= PLAYER_NUM) || (pi2 >= PLAYER_NUM)) {
        return;
    }
    e = &(g->eto[pi]);
    e2 = &(g->eto[pi2]);
    e->trade_bc[pi2] = 0;
    e2->trade_bc[pi] = 0;
    e->trade_established_bc[pi2] = 0;
    e2->trade_established_bc[pi] = 0;
    e->trade_percent[pi2] = 0;
    e2->trade_percent[pi] = 0;
    e->attack_bounty[pi2] = PLAYER_NONE;
    e2->attack_bounty[pi] = PLAYER_NONE;
    SETMAX(e->relation1[pi2], -100);
    e2->relation1[pi] = e->relation1[pi2];
    if (e->treaty[pi2] >= TREATY_WAR) {
        return;
    }
#if 0
    if (e->trade_bc[pi2] != 0) { /* never true */
        /* ignored */
    }
#endif
}

void game_diplo_annoy(struct game_s *g, player_id_t p1, player_id_t p2, int n)
{
    empiretechorbit_t *e1 = &(g->eto[p1]);
    n *= 10;
    e1->mood_trade[p2] -= n;
    e1->mood_treaty[p2] -= n;
    e1->mood_tech[p2] -= n;
    e1->mood_peace[p2] -= n;
}

void game_diplo_battle_finish(struct game_s *g, int def, int att, int popdiff, uint32_t app_def, uint16_t biodamage, uint32_t app_att, planet_id_t planet_i)
{
    player_id_t claim, side_z, side_ai;
    uint32_t app_ai;
    planet_t *p;
    int offense;
    if ((att >= PLAYER_NUM) || (def >= PLAYER_NUM)) {
        return;
    }
    p = &(g->planet[planet_i]);
    claim = p->claim;
    if ((claim == PLAYER_NONE) || ((claim != att) && (claim != def))) {
        if (IS_AI(g, att)) {
            side_z = def;
            side_ai = att;
            app_ai = app_att;
        } else {
            side_z = att;
            side_ai = def;
            app_ai = app_def;
        }
    } else {
        if (claim == att) {
            side_z = def;
            side_ai = att;
            app_ai = app_att;
        } else {
            side_z = att;
            side_ai = def;
            app_ai = app_def;
        }
    }
    if (biodamage) {
        for (player_id_t i = PLAYER_0; i < g->players; ++i) {
            if ((i != side_z) && IS_AI(g, i)) {
                game_diplo_act(g, -10, side_z, i, 11, planet_i, 0);
            }
        }
    }
    popdiff *= 5;
    SETMIN(app_ai, 50);
    biodamage *= 10;
    offense = MAX(popdiff, app_ai);
    SETMAX(offense, biodamage);
    if (IS_AI(g, side_ai)) {
        int extra;
        if (biodamage > 0) {
            extra = 11;
        } else if (popdiff > 0) {
            extra = 10;
        } else {
            extra = 9;
        }
        game_diplo_act(g, -offense, side_z, side_ai, extra, planet_i, 0);
    }
    if ((offense >= 30) && IS_HUMAN(g, side_z)) {
        empiretechorbit_t *ea = &(g->eto[side_ai]);
        empiretechorbit_t *ez = &(g->eto[side_z]);
        for (player_id_t i = PLAYER_0; i < g->players; ++i) {
            if (1
              && (i != side_z) && (i != side_ai) && IS_AI(g, i)
              && ((ea->treaty[i] >= TREATY_WAR) || (ez->bounty_collect[i] == side_ai)) /* FIXME BUG? should be attack_bounty? */
            ) {
                if (ez->attack_bounty[i] == side_ai) {
                    ez->bounty_collect[i] = i;
                } else if (game_planet_get_random(g, side_ai) == PLANET_NONE) {
                    game_diplo_act(g, offense, side_z, i, 3, planet_i, 0);
                }
            }
        }
    }
}

void game_diplo_set_treaty(struct game_s *g, player_id_t p1, player_id_t p2, treaty_t treaty)
{
    empiretechorbit_t *e1 = &(g->eto[p1]);
    empiretechorbit_t *e2 = &(g->eto[p2]);
    /* 1oom-mp: same-team empires are a locked alliance -- never set their treaty to anything else
       (guards the election FINAL_WAR path and any stray MP diplomacy order from splitting a team). */
    if ((g->mp_team[p1] != 0) && (g->mp_team[p1] == g->mp_team[p2]) && (treaty != TREATY_ALLIANCE)) {
        return;
    }
    e1->treaty[p2] = treaty;
    e2->treaty[p1] = treaty;
    game_diplo_team_sync_stance(g, p1, p2); /* 1oom-mp: peace/NAP propagate to the whole teams too */
}

void game_diplo_set_trade(struct game_s *g, player_id_t p1, player_id_t p2, int bc)
{
    empiretechorbit_t *e1 = &(g->eto[p1]);
    empiretechorbit_t *e2 = &(g->eto[p2]);
    int v;
    if (bc == 0) {
        return;
    }
    if (e1->trade_established_bc[p2] < bc) {
        v = ((e1->trade_percent[p2] * e1->trade_established_bc[p2]) - (bc * 30)) / (e1->trade_established_bc[p2] + bc);
    } else {
        v = e1->trade_percent[p2];
    }
    SETMIN(v, 100);
    e1->trade_bc[p2] = bc;
    e1->trade_established_bc[p2] = bc;  /* FIXME BUG? should be < bc to make the variable have a purpose */
    e1->trade_percent[p2] = v;
    e2->trade_bc[p1] = bc;
    e2->trade_established_bc[p1] = bc;
    e2->trade_percent[p1] = v;
}

void game_diplo_stop_war(struct game_s *g, player_id_t p1, player_id_t p2)
{
    empiretechorbit_t *e1 = &(g->eto[p1]);
    empiretechorbit_t *e2 = &(g->eto[p2]);
    game_diplo_set_treaty(g, p1, p2, TREATY_NONE);
    ADDSATT(e1->relation1[p2], 40, 100);
    e2->relation1[p1] = e1->relation1[p2];
    if (IS_HUMAN(g, p1) /* && IS_AI(g, p2) */) {
        g->evn.ceasefire[p1][p2] = rnd_0_nm1(6, &g->seed) + 8;
    }
    if (IS_HUMAN(g, p2) /* && IS_AI(g, p1) */) {
        g->evn.ceasefire[p2][p1] = rnd_0_nm1(6, &g->seed) + 8;
    }
}

void game_diplo_esp_frame(struct game_s *g, player_id_t framed, player_id_t victim)
{
    int v;
    v = -(rnd_1_n(12, &g->seed) + rnd_1_n(12, &g->seed));
    game_diplo_act(g, v, framed, victim, 5, 0, 0);
}

void game_diplo_limit_mood_treaty(struct game_s *g)
{
    for (player_id_t p1 = PLAYER_0; p1 < g->players; ++p1) {
        empiretechorbit_t *e = &(g->eto[p1]);
        for (player_id_t p2 = PLAYER_0; p2 < g->players; ++p2) {
            if (p1 != p2) {
                SETRANGE(e->mood_treaty[p2], -200, 120);
            }
        }
    }
}

void game_diplo_mood_relax(struct game_s *g)
{
    for (player_id_t p1 = PLAYER_0; p1 < g->players; ++p1) {
        empiretechorbit_t *e = &(g->eto[p1]);
        for (player_id_t p2 = PLAYER_0; p2 < g->players; ++p2) {
            if (p1 != p2) {
                if (e->mood_treaty[p2] < 100) {
                    e->mood_treaty[p2] += 10;
                }
                if (e->mood_trade[p2] < 100) {
                    e->mood_trade[p2] += 10;
                }
                if (e->mood_tech[p2] < 100) {
                    e->mood_tech[p2] += 10;
                }
                if (e->mood_peace[p2] < 100) {
                    e->mood_peace[p2] += 10;
                }
            }
        }
    }
}

int16_t game_diplo_get_mood(struct game_s *g, player_id_t p1, player_id_t p2)
{
    if (g->end == GAME_END_FINAL_WAR) {
        return 0;
    } else {
        empiretechorbit_t *e = &(g->eto[p1]);
        int16_t vmin, v;
        vmin = e->mood_treaty[p2];
        v = e->mood_trade[p2];
        SETMIN(vmin, v);
        v = e->mood_tech[p2];
        SETMIN(vmin, v);
        v = e->mood_peace[p2];
        SETMIN(vmin, v);
        return vmin;
    }
}

bool game_diplo_is_gone(struct game_s *g, player_id_t api, player_id_t pi)
{
    if (IS_HUMAN(g, pi)) {
        return false;   /* FIXME remote multiplayer */
    } else {
        const empiretechorbit_t *e = &(g->eto[api]);
        int16_t v, vr;
        vr = game_diplo_get_mood(g, api, pi);
        v = e->trust[pi] /*+ e->relation1[pi]*/ + vr + game_diplo_tbl_reldiff[g->eto[pi].trait1];
        if (((v /*- e->relation1[pi]*/) <= -100) || (vr <= -100)) {
            return true;
        }
    }
    return false;
}

/* 1oom-mp: see the forward declaration near the top. After a stance between pi and pi2 changes,
   mirror the resulting treaty onto every (member-of-pi's-team, member-of-pi2's-team) pair so the
   whole teams share it. Reuses the primitives (so each mirrored pair gets correct relations/mood),
   guarded by s_diplo_in_team_sync so the primitives' own sync calls no-op while we run. A solo
   empire is its own team of one. Trade is intentionally not mirrored; FINAL_WAR is left alone. */
static void game_diplo_team_sync_stance(struct game_s *g, player_id_t pi, player_id_t pi2)
{
    uint8_t ta, tb;
    treaty_t tr;
    if (s_diplo_in_team_sync) {
        return; /* a primitive we just called is reporting back -- the top-level loop already has it */
    }
    if ((pi >= g->players) || (pi2 >= g->players) || (pi == pi2)) {
        return;
    }
    if (g->end == GAME_END_FINAL_WAR) {
        return; /* leave the endgame all-vs-all machinery alone */
    }
    ta = g->mp_team[pi];
    tb = g->mp_team[pi2];
    if ((ta == 0) && (tb == 0)) {
        return; /* neither empire is on a team -> nothing to mirror */
    }
    tr = g->eto[pi].treaty[pi2];
    if (tr == TREATY_FINAL_WAR) {
        return;
    }
    s_diplo_in_team_sync = true;
    for (player_id_t tx = PLAYER_0; tx < g->players; ++tx) {
        if (!((tx == pi) || ((ta != 0) && (g->mp_team[tx] == ta)))) {
            continue; /* tx is not pi and not on pi's team */
        }
        for (player_id_t ty = PLAYER_0; ty < g->players; ++ty) {
            treaty_t cur;
            if (!((ty == pi2) || ((tb != 0) && (g->mp_team[ty] == tb)))) {
                continue; /* ty is not pi2 and not on pi2's team */
            }
            if (tx == ty) {
                continue;
            }
            if ((tx == pi) && (ty == pi2)) {
                continue; /* the originating pair, already set */
            }
            if ((g->mp_team[tx] != 0) && (g->mp_team[tx] == g->mp_team[ty])) {
                continue; /* never alter a stance within a team */
            }
            cur = g->eto[tx].treaty[ty];
            if (cur == tr) {
                continue; /* already matches */
            }
            if (tr == TREATY_WAR) {
                game_diplo_start_war(g, tx, ty);
            } else {
                if (cur == TREATY_WAR) {
                    game_diplo_stop_war(g, tx, ty); /* leave war cleanly (relations/ceasefire) first */
                }
                game_diplo_set_treaty(g, tx, ty, tr);
            }
        }
    }
    s_diplo_in_team_sync = false;
}
