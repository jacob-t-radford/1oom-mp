/* 1oom-mp Phase B: serialize/apply a player's per-turn orders.
   Covers the player-controlled STATE: empire economy (tax, security, espionage
   allocation, research-field allocation), ship designs, and per-planet production
   (sliders + build + relocation + transport). Fleet movement is handled separately
   (it is action-based). Anti-cheat: the server only applies a player's OWN planets.

   Wire format uses fixed-size raw field regions (same engine build on both ends).
   Cross-architecture deployment would swap these for explicit byte order; the
   portable full-state path (the save blob) already handles GAME_DATA. */
#include "config.h"

#include <string.h>

#include "game.h"
#include "game_diplo.h"
#include "game_planet.h"
#include "game_mp_orders.h"
#include "game_shipdesign.h"
#include "game_tech.h"
#include "game_turn.h"
#include "rnd.h"

#define PUT(src, n) do { if (pos + (int)(n) > buflen) { return -1; } memcpy(buf + pos, (src), (n)); pos += (int)(n); } while (0)
#define GET(dst, n) do { if (pos + (int)(n) > len) { return -1; } memcpy((dst), buf + pos, (n)); pos += (int)(n); } while (0)

/* ---- human-to-human diplomacy action queues ----
   s_diplo_act: filled on the CLIENT by game_mp_diplo_record, drained by write_orders.
   s_diplo_pending: filled on the SERVER by apply_orders, applied by
   game_mp_diplo_apply_pending after the turn resolves. (Distinct processes use one
   or the other.) */
#define MP_DIPLO_ACT_MAX 16
struct mp_diplo_act_s { player_id_t actor, target; uint8_t verb, arg; uint8_t p[4]; };
static struct mp_diplo_act_s s_diplo_act[MP_DIPLO_ACT_MAX];
static int s_diplo_act_num = 0;
static struct mp_diplo_act_s s_diplo_pending[MP_DIPLO_ACT_MAX];
static int s_diplo_pending_num = 0;

bool game_mp_diplo_record_p(player_id_t actor, player_id_t target, uint8_t verb, uint8_t arg, const uint8_t *p)
{
    if (s_diplo_act_num >= MP_DIPLO_ACT_MAX) { return false; }
    s_diplo_act[s_diplo_act_num].actor = actor;
    s_diplo_act[s_diplo_act_num].target = target;
    s_diplo_act[s_diplo_act_num].verb = verb;
    s_diplo_act[s_diplo_act_num].arg = arg;
    for (int k = 0; k < 4; ++k) { s_diplo_act[s_diplo_act_num].p[k] = p ? p[k] : 0; }
    ++s_diplo_act_num;
    return true;
}

bool game_mp_diplo_record(player_id_t actor, player_id_t target, uint8_t verb, uint8_t arg)
{
    return game_mp_diplo_record_p(actor, target, verb, arg, NULL);
}

/* ---- colonize requests (client records a planet to colonize, server resolves) ---- */
#define MP_COLONIZE_MAX 16
struct mp_colonize_req_s { player_id_t pi; planet_id_t pli; char name[PLANET_NAME_LEN]; };
static planet_id_t s_colonize_act[MP_COLONIZE_MAX];
static int s_colonize_act_num = 0;
static struct mp_colonize_req_s s_colonize_pending[MP_COLONIZE_MAX];
static int s_colonize_pending_num = 0;

void game_mp_colonize_record(player_id_t pi, planet_id_t pli)
{
    (void)pi; /* actor is implicit: the player whose write_orders drains this */
    for (int i = 0; i < s_colonize_act_num; ++i) { if (s_colonize_act[i] == pli) { return; } }
    if (s_colonize_act_num < MP_COLONIZE_MAX) { s_colonize_act[s_colonize_act_num++] = pli; }
}

/* 1oom-mp live teammate visibility: serialize this player's IN-PROGRESS plan for teammates --
   en-route fleets (x,y,dest), queued colonizes, and per-owned-planet production sliders. Compact
   and best-effort streamed each frame; the viewer overlays it on their starmap. */
int game_mp_write_team_plan(const struct game_s *g, player_id_t pi, uint8_t *buf, int buflen)
{
    int pos = 0;
    uint16_t v, cnt;
    int cntpos;
    v = (uint16_t)pi; PUT(&v, 2);
    /* en-route fleets I own */
    cntpos = pos; cnt = 0; PUT(&cnt, 2);
    for (int i = 0; i < g->enroute_num; ++i) {
        const fleet_enroute_t *r = &g->enroute[i];
        if (r->owner != pi) { continue; }
        v = r->x; PUT(&v, 2); v = r->y; PUT(&v, 2); v = (uint16_t)r->dest; PUT(&v, 2);
        ++cnt;
    }
    memcpy(buf + cntpos, &cnt, 2);
    /* colonizes I queued this turn */
    cntpos = pos; cnt = 0; PUT(&cnt, 2);
    for (int i = 0; i < s_colonize_act_num; ++i) {
        v = (uint16_t)s_colonize_act[i]; PUT(&v, 2);
        ++cnt;
    }
    memcpy(buf + cntpos, &cnt, 2);
    /* full planet snapshot for the worlds I own, so a teammate's read-only panel shows EXACT
       production figures (not just proportional slider bars) -- sliders + economy ride along. */
    cntpos = pos; cnt = 0; PUT(&cnt, 2);
    for (int i = 0; i < g->galaxy_stars; ++i) {
        const planet_t *p = &g->planet[i];
        if (p->owner != pi) { continue; }
        v = (uint16_t)i; PUT(&v, 2);
        PUT(p, (int)sizeof(planet_t));
        ++cnt;
    }
    memcpy(buf + cntpos, &cnt, 2);
    /* planets where I have ships orbiting RIGHT NOW, so a teammate's map drops the orbit icon
       for a ship I just sent en-route this turn (else it shows in orbit AND en-route at once). */
    cntpos = pos; cnt = 0; PUT(&cnt, 2);
    for (int i = 0; i < g->galaxy_stars; ++i) {
        const fleet_orbit_t *o = &g->eto[pi].orbit[i];
        int any = 0;
        for (int j = 0; j < g->eto[pi].shipdesigns_num; ++j) { if (o->ships[j]) { any = 1; break; } }
        if (!any) { continue; }
        v = (uint16_t)i; PUT(&v, 2);
        ++cnt;
    }
    memcpy(buf + cntpos, &cnt, 2);
    return pos;
}

int game_mp_write_orders(const struct game_s *g, player_id_t pi, uint8_t *buf, int buflen)
{
    int pos = 0;
    const empiretechorbit_t *e = &g->eto[pi];
    /* empire economy + espionage + research-field allocation */
    PUT(&e->tax, sizeof(e->tax));
    PUT(&e->security, sizeof(e->security));
    PUT(e->spying, sizeof(e->spying));
    PUT(e->spymode, sizeof(e->spymode));
    PUT(e->tech.slider, sizeof(e->tech.slider));
    PUT(e->tech.slider_lock, sizeof(e->tech.slider_lock));
    PUT(e->tech.project, sizeof(e->tech.project)); /* the human's chosen research target per field */
    /* ship designs (the 6 design slots) + how many designs the player has, so the server
       recomputes the cost of newly-designed slots. Without the count, a freshly designed slot
       (index >= the server's stale count) keeps cost 0 and game_turn_build_ship spins forever. */
    PUT(g->srd[pi].design, sizeof(g->srd[pi].design));
    PUT(&e->shipdesigns_num, 1);
    /* treaties: so a treaty the player settled with an AI in a client-side audience reaches the
       server. Only AI-target changes are applied (human<->human goes through the proposal flow). */
    PUT(e->treaty, sizeof(e->treaty));
    /* per-planet production for each owned planet */
    for (int i = 0; i < g->galaxy_stars; ++i) {
        const planet_t *p = &g->planet[i];
        if (p->owner != pi) {
            continue;
        }
        uint16_t idx = (uint16_t)i;
        PUT(&idx, 2);
        PUT(p->slider, sizeof(p->slider));
        PUT(p->slider_lock, sizeof(p->slider_lock));
        PUT(&p->buildship, sizeof(p->buildship));
        PUT(&p->reloc, sizeof(p->reloc));
        PUT(&p->trans_num, sizeof(p->trans_num));
        PUT(&p->trans_dest, sizeof(p->trans_dest));
        PUT(p->name, sizeof(p->name)); /* 1oom-mp: relay a renamed planet so it doesn't revert at resolution */
    }
    {
        uint16_t term = 0xffff;
        PUT(&term, 2);
    }
    /* fleet movement: the player's orbit ship-counts + its in-transit fleets and
       transports (capturing new sends and redirects done this turn). */
    PUT(e->orbit, sizeof(e->orbit));
    {
        int cntpos = pos;
        uint16_t cnt = 0;
        PUT(&cnt, 2);
        for (int i = 0; i < g->enroute_num; ++i) {
            if (g->enroute[i].owner == pi) {
                PUT(&g->enroute[i], sizeof(fleet_enroute_t));
                ++cnt;
            }
        }
        memcpy(buf + cntpos, &cnt, 2);
    }
    {
        int cntpos = pos;
        uint16_t cnt = 0;
        PUT(&cnt, 2);
        for (int i = 0; i < g->transport_num; ++i) {
            if (g->transport[i].owner == pi) {
                PUT(&g->transport[i], sizeof(transport_t));
                ++cnt;
            }
        }
        memcpy(buf + cntpos, &cnt, 2);
    }
    /* diplomacy actions queued by this player this turn */
    {
        int cntpos = pos;
        uint16_t cnt = 0;
        PUT(&cnt, 2);
        for (int i = 0; i < s_diplo_act_num; ++i) {
            if (s_diplo_act[i].actor != pi) { continue; }
            uint8_t verb = s_diplo_act[i].verb;
            uint16_t target = s_diplo_act[i].target;
            uint8_t arg = s_diplo_act[i].arg;
            PUT(&verb, 1);
            PUT(&target, 2);
            PUT(&arg, 1);
            PUT(s_diplo_act[i].p, 4);
            ++cnt;
        }
        memcpy(buf + cntpos, &cnt, 2);
    }
    /* colonize requests queued by this player this turn */
    {
        int cntpos = pos;
        uint16_t cnt = 0;
        PUT(&cnt, 2);
        for (int i = 0; i < s_colonize_act_num; ++i) {
            uint16_t pli = (uint16_t)s_colonize_act[i];
            PUT(&pli, 2);
            /* 1oom-mp: carry the name the player typed so a just-colonized planet keeps it */
            PUT(g->planet[s_colonize_act[i]].name, PLANET_NAME_LEN);
            ++cnt;
        }
        memcpy(buf + cntpos, &cnt, 2);
    }
    /* NB: do NOT clear the queued records here. Serialization must be side-effect-free so the
       soft-ready client can re-serialize every frame to detect changes (and re-submit the same
       orders) without losing colonize/diplo actions. They're cleared at turn start instead, via
       game_mp_orders_reset(). */
    return pos;
}

/* client: drop the diplo/colonize actions queued last turn. Called at the start of each turn
   (before the turn UI re-queues this turn's), so re-submitting orders mid-turn keeps them. */
void game_mp_orders_reset(void)
{
    s_diplo_act_num = 0;
    s_colonize_act_num = 0;
}

int game_mp_apply_orders(struct game_s *g, player_id_t pi, const uint8_t *buf, int len)
{
    int pos = 0, applied = 0;
    empiretechorbit_t *e = &g->eto[pi];
    GET(&e->tax, sizeof(e->tax));
    GET(&e->security, sizeof(e->security));
    GET(e->spying, sizeof(e->spying));
    GET(e->spymode, sizeof(e->spymode));
    GET(e->tech.slider, sizeof(e->tech.slider));
    GET(e->tech.slider_lock, sizeof(e->tech.slider_lock));
    {
        uint8_t proj[TECH_FIELD_NUM];
        GET(proj, sizeof(proj));
        /* apply the human's research-target picks (their own empire). game_tech_start_next
           sets project + cost and keeps the accumulated investment. */
        for (tech_field_t f = 0; f < TECH_FIELD_NUM; ++f) {
            if ((proj[f] != 0) && (proj[f] != e->tech.project[f])) {
                game_tech_start_next(g, pi, f, proj[f]);
            }
        }
    }
    GET(g->srd[pi].design, sizeof(g->srd[pi].design));
    {   /* design count (so costs are recomputed for newly designed slots) */
        uint8_t ndesigns;
        GET(&ndesigns, 1);
        if ((ndesigns >= 1) && (ndesigns <= NUM_SHIPDESIGNS)) { e->shipdesigns_num = ndesigns; }
    }
    {   /* treaty changes the player made with AI empires (e.g. via a client-side audience).
           Human<->human treaties are left to the proposal flow, so only AI targets are applied. */
        treaty_t tr[PLAYER_NUM];
        GET(tr, sizeof(tr));
        for (player_id_t t = PLAYER_0; t < g->players; ++t) {
            if ((t == pi) || !IS_AI(g, t)) { continue; }
            treaty_t cur = e->treaty[t], want = tr[t];
            if (want == cur) { continue; }
            if (want == TREATY_WAR) { game_diplo_start_war(g, pi, t); }
            else if (cur == TREATY_WAR) { game_diplo_stop_war(g, pi, t); if (want != TREATY_NONE) { game_diplo_set_treaty(g, pi, t, want); } }
            else { game_diplo_set_treaty(g, pi, t, want); }
        }
    }
    for (;;) {
        uint16_t idx;
        GET(&idx, 2);
        if (idx == 0xffff) {
            break;
        }
        if (idx >= g->galaxy_stars) {
            return -1;
        }
        planet_t *p = &g->planet[idx];
        int16_t sl[PLANET_SLIDER_NUM];
        uint16_t lk[PLANET_SLIDER_NUM], reloc, trans_num, trans_dest;
        uint8_t buildship;
        char name[PLANET_NAME_LEN];
        GET(sl, sizeof(sl));
        GET(lk, sizeof(lk));
        GET(&buildship, 1);
        GET(&reloc, sizeof(reloc));
        GET(&trans_num, sizeof(trans_num));
        GET(&trans_dest, sizeof(trans_dest));
        GET(name, sizeof(name));
        name[PLANET_NAME_LEN - 1] = '\0'; /* keep it a valid string */
        /* anti-cheat: only touch planets this player actually owns */
        if (p->owner != pi) {
            continue;
        }
        if ((buildship >= NUM_SHIPDESIGNS) && (buildship != BUILDSHIP_STARGATE)) {
            buildship = 0;
        }
        memcpy(p->slider, sl, sizeof(sl));
        memcpy(p->slider_lock, lk, sizeof(lk));
        p->buildship = buildship;
        p->reloc = reloc;
        p->trans_num = trans_num;
        p->trans_dest = trans_dest;
        memcpy(p->name, name, sizeof(p->name)); /* 1oom-mp: persist a renamed planet */
        ++applied;
    }
    /* fleet movement: copy orbit ship-counts; replace this player's in-transit
       fleets/transports (keep other players' entries, append the client's). */
    GET(e->orbit, sizeof(e->orbit));
    {
        int n = 0;
        for (int i = 0; i < g->enroute_num; ++i) {
            if (g->enroute[i].owner != pi) {
                if (n != i) { g->enroute[n] = g->enroute[i]; }
                ++n;
            }
        }
        uint16_t cnt;
        GET(&cnt, 2);
        for (int i = 0; i < cnt; ++i) {
            if (n >= FLEET_ENROUTE_MAX) { return -1; }
            GET(&g->enroute[n], sizeof(fleet_enroute_t));
            g->enroute[n].owner = pi; /* anti-cheat: a client can only move its own ships */
            ++n;
        }
        g->enroute_num = (uint16_t)n;
    }
    {
        int n = 0;
        for (int i = 0; i < g->transport_num; ++i) {
            if (g->transport[i].owner != pi) {
                if (n != i) { g->transport[n] = g->transport[i]; }
                ++n;
            }
        }
        uint16_t cnt;
        GET(&cnt, 2);
        for (int i = 0; i < cnt; ++i) {
            if (n >= TRANSPORT_MAX) { return -1; }
            GET(&g->transport[n], sizeof(transport_t));
            g->transport[n].owner = pi;
            ++n;
        }
        g->transport_num = (uint16_t)n;
    }
    /* diplomacy actions: parse + stash for game_mp_diplo_apply_pending (applied
       AFTER game_turn_process, which would otherwise read/clear queued proposals). */
    {
        uint16_t dcnt = 0;
        GET(&dcnt, 2);
        for (int i = 0; i < dcnt; ++i) {
            uint8_t verb, arg, p[4]; uint16_t target;
            GET(&verb, 1);
            GET(&target, 2);
            GET(&arg, 1);
            GET(p, 4);
            if ((target < g->players) && ((player_id_t)target != pi) && (s_diplo_pending_num < MP_DIPLO_ACT_MAX)) {
                s_diplo_pending[s_diplo_pending_num].actor = pi;
                s_diplo_pending[s_diplo_pending_num].target = (player_id_t)target;
                s_diplo_pending[s_diplo_pending_num].verb = verb;
                s_diplo_pending[s_diplo_pending_num].arg = arg;
                memcpy(s_diplo_pending[s_diplo_pending_num].p, p, 4);
                ++s_diplo_pending_num;
            }
        }
    }
    /* colonize requests: parse + stash for game_mp_colonize_apply_pending */
    {
        uint16_t ccnt = 0;
        GET(&ccnt, 2);
        for (int i = 0; i < ccnt; ++i) {
            uint16_t pli;
            char cname[PLANET_NAME_LEN];
            GET(&pli, 2);
            /* 1oom-mp: read the carried name (must stay in sync with the PUT above) */
            GET(cname, PLANET_NAME_LEN);
            cname[PLANET_NAME_LEN - 1] = '\0';
            if ((pli < g->galaxy_stars) && (s_colonize_pending_num < MP_COLONIZE_MAX)) {
                s_colonize_pending[s_colonize_pending_num].pi = pi;
                s_colonize_pending[s_colonize_pending_num].pli = (planet_id_t)pli;
                memcpy(s_colonize_pending[s_colonize_pending_num].name, cname, PLANET_NAME_LEN);
                ++s_colonize_pending_num;
            }
        }
    }
    return applied;
}

void game_mp_colonize_apply_pending(struct game_s *g)
{
    /* fair tie-break for a contested planet: shuffle so the winner isn't always the
       lowest player id, then the first valid claim wins (later claims see it owned). */
    for (int i = s_colonize_pending_num - 1; i > 0; --i) {
        int j = (int)rnd_0_nm1((uint16_t)(i + 1), &g->seed);
        struct mp_colonize_req_s tmp = s_colonize_pending[i];
        s_colonize_pending[i] = s_colonize_pending[j];
        s_colonize_pending[j] = tmp;
    }
    for (int i = 0; i < s_colonize_pending_num; ++i) {
        game_planet_colonize_with_ship(g, s_colonize_pending[i].pi, s_colonize_pending[i].pli);
        /* 1oom-mp: colonize set the default (star) name; restore the player's chosen name */
        if (s_colonize_pending[i].name[0]) {
            memcpy(g->planet[s_colonize_pending[i].pli].name, s_colonize_pending[i].name, PLANET_NAME_LEN);
        }
    }
    s_colonize_pending_num = 0;
}

void game_mp_diplo_apply_pending(struct game_s *g)
{
    for (int i = 0; i < s_diplo_pending_num; ++i) {
        player_id_t a = s_diplo_pending[i].actor, t = s_diplo_pending[i].target;
        uint8_t arg = s_diplo_pending[i].arg;
        const uint8_t *p = s_diplo_pending[i].p;
        if ((a >= g->players) || (t >= g->players) || (a == t)) { continue; }
        switch (s_diplo_pending[i].verb) {
            case MP_DIPLO_DECLARE_WAR:
                game_diplo_start_war(g, a, t);
                break;
            case MP_DIPLO_BREAK_TREATY:
                game_diplo_break_treaty(g, a, t);
                break;
            case MP_DIPLO_PROPOSE_NAP:
            case MP_DIPLO_PROPOSE_ALLIANCE:
            case MP_DIPLO_PROPOSE_PEACE:
                /* queue the proposal in target t's synced mailbox; t answers next turn */
                g->eto[t].diplo_type[a] = MP_DIPLO_PROPOSAL_MARK;
                g->eto[t].diplo_val[a] = (int16_t)s_diplo_pending[i].verb;
                break;
            case MP_DIPLO_ACCEPT:
                /* a (responder) accepts t's (proposer's) proposal; arg = the PROPOSE_* verb. */
                if (arg == MP_DIPLO_PROPOSE_PEACE) {
                    game_diplo_stop_war(g, a, t);
                } else if (arg == MP_DIPLO_PROPOSE_ALLIANCE) {
                    game_diplo_set_treaty(g, a, t, TREATY_ALLIANCE);
                } else if (arg == MP_DIPLO_PROPOSE_NAP) {
                    game_diplo_set_treaty(g, a, t, TREATY_NONAGGRESSION);
                } else if (arg == MP_DIPLO_PROPOSE_TRADE) {
                    game_diplo_set_trade(g, a, t, (int)(p[0] | (p[1] << 8)));
                } else if (arg == MP_DIPLO_PROPOSE_TECH) {
                    /* proposer t wanted {p0,p1} from responder a, and gave {p2,p3} to a */
                    game_tech_get_new(g, t, (tech_field_t)p[0], p[1], TECHSOURCE_TRADE, a, PLAYER_NONE, false);
                    game_tech_get_new(g, a, (tech_field_t)p[2], p[3], TECHSOURCE_TRADE, t, PLAYER_NONE, false);
                }
                g->eto[a].diplo_type[t] = 0;
                break;
            case MP_DIPLO_REJECT:
                g->eto[a].diplo_type[t] = 0;
                break;
            case MP_DIPLO_BREAK_TRADE:
                game_diplo_break_trade(g, a, t);
                break;
            case MP_DIPLO_TRIBUTE_BC: {
                /* unilateral gift: a (giver) -> t (receiver) */
                int amt = (int)(p[0] | (p[1] << 8));
                if (amt > (int)g->eto[a].reserve_bc) { amt = (int)g->eto[a].reserve_bc; }
                g->eto[a].reserve_bc -= (uint32_t)amt;
                g->eto[t].reserve_bc += (uint32_t)amt;
                break;
            }
            case MP_DIPLO_TRIBUTE_TECH:
                /* unilateral gift: a (giver) hands tech {p0,p1} to t (receiver) */
                game_tech_get_new(g, t, (tech_field_t)p[0], p[1], TECHSOURCE_TRADE, a, PLAYER_NONE, false);
                break;
            default:
                break;
        }
    }
    s_diplo_pending_num = 0;
}
