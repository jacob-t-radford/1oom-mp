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
#include "log.h"
#include "rnd.h"
#include "ui.h" /* 1oom-mp: ui_mp_active gates the econ-action recording to live MP clients */
#include "comp.h" /* SETRANGE for econ clamps */

#define PUT(src, n) do { if (pos + (int)(n) > buflen) { return -1; } memcpy(buf + pos, (src), (n)); pos += (int)(n); } while (0)
#define GET(dst, n) do { if (pos + (int)(n) > len) { return -1; } memcpy((dst), buf + pos, (n)); pos += (int)(n); } while (0)

/* ---- human-to-human diplomacy action queues ----
   s_diplo_act: filled on the CLIENT by game_mp_diplo_record, drained by write_orders.
   s_diplo_pending: filled on the SERVER by apply_orders, applied by
   game_mp_diplo_apply_pending after the turn resolves. (Distinct processes use one
   or the other.) */
#define MP_DIPLO_ACT_MAX 32 /* 1oom-mp: per-turn diplomacy actions; well above any real turn (was 16) */
struct mp_diplo_act_s { player_id_t actor, target; uint8_t verb, arg; uint8_t p[4]; };
static struct mp_diplo_act_s s_diplo_act[MP_DIPLO_ACT_MAX];
static int s_diplo_act_num = 0;
static struct mp_diplo_act_s s_diplo_pending[MP_DIPLO_ACT_MAX];
static int s_diplo_pending_num = 0;

bool game_mp_diplo_record_p(player_id_t actor, player_id_t target, uint8_t verb, uint8_t arg, const uint8_t *p)
{
    if (s_diplo_act_num >= MP_DIPLO_ACT_MAX) { log_warning("MP: diplomacy-action queue full (%d) -- dropping action verb=%d target=%d (raise MP_DIPLO_ACT_MAX)\n", MP_DIPLO_ACT_MAX, verb, target); return false; }
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
#define MP_COLONIZE_MAX 64 /* 1oom-mp: per-turn colonizations; raised from 16 so a big batch of colony ships arriving together isn't silently dropped */
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
    else { log_warning("MP: colonize queue full (%d) -- dropping colonize of planet %d (raise MP_COLONIZE_MAX)\n", MP_COLONIZE_MAX, (int)pli); }
}

/* ---- 1oom-mp: planning-phase economy actions (see game_mp_orders.h). Each record is one client-side
   action whose effect the serialized planning fields don't carry; the server re-applies it
   authoritatively at order-apply time. Where possible the server RECOMPUTES amounts from its own
   state (base-scrap refund); client-supplied BC values are clamped. ---- */
#define MP_ECON_MAX 48
enum { MP_ECON_BC = 1, MP_ECON_SCRAP_BASES, MP_ECON_TRADE, MP_ECON_TECH_FROM, MP_ECON_TECH_TO, MP_ECON_TRIBUTE, MP_ECON_RELATION };
struct mp_econ_act_s { uint8_t kind; uint8_t actor; uint8_t pa; uint8_t f; uint8_t t; int32_t v; uint16_t pli; };
static struct mp_econ_act_s s_econ_act[MP_ECON_MAX];
static int s_econ_act_num = 0;

static void econ_record(uint8_t kind, player_id_t actor, player_id_t pa, uint8_t f, uint8_t t, int v, uint16_t pli)
{
    struct mp_econ_act_s *r;
    if (!ui_mp_active || game_mp_is_server) { return; } /* only a live MP client queues; SP/server apply directly */
    if (s_econ_act_num >= MP_ECON_MAX) { log_warning("MP: econ queue full -- dropping action kind %d\n", kind); return; }
    r = &s_econ_act[s_econ_act_num++];
    r->kind = kind; r->actor = (uint8_t)actor; r->pa = (uint8_t)pa; r->f = f; r->t = t; r->v = (int32_t)v; r->pli = pli;
}

void game_mp_econ_record_bc(player_id_t actor, int bc) { econ_record(MP_ECON_BC, actor, PLAYER_NONE, 0, 0, bc, 0); }
void game_mp_econ_record_scrap_bases(player_id_t actor, planet_id_t pli, int n) { econ_record(MP_ECON_SCRAP_BASES, actor, PLAYER_NONE, 0, 0, n, (uint16_t)pli); }
void game_mp_econ_record_trade(player_id_t actor, player_id_t pa, int bc) { econ_record(MP_ECON_TRADE, actor, pa, 0, 0, bc, 0); }
void game_mp_econ_record_tech_from(player_id_t actor, player_id_t pa, uint8_t field, uint8_t tech) { econ_record(MP_ECON_TECH_FROM, actor, pa, field, tech, 0, 0); }
void game_mp_econ_record_tech_to(player_id_t actor, player_id_t pa, uint8_t field, uint8_t tech) { econ_record(MP_ECON_TECH_TO, actor, pa, field, tech, 0, 0); }
void game_mp_econ_record_tribute(player_id_t actor, player_id_t pa, int bc) { econ_record(MP_ECON_TRIBUTE, actor, pa, 0, 0, bc, 0); }
void game_mp_econ_record_relation(player_id_t actor, player_id_t pa, int rel) { econ_record(MP_ECON_RELATION, actor, pa, 0, 0, rel, 0); }

/* server: apply one economy record for player pi (validated/clamped; amounts recomputed where possible). */
static void econ_apply(struct game_s *g, player_id_t pi, const struct mp_econ_act_s *r)
{
    empiretechorbit_t *e = &g->eto[pi];
    player_id_t pa = (player_id_t)r->pa;
    int v = (int)r->v;
    switch (r->kind) {
        case MP_ECON_BC:
            SETRANGE(v, -100000, 100000); /* scrap refunds / audience gifts are well under this */
            if ((v < 0) && (e->reserve_bc < (uint32_t)(-v))) { e->reserve_bc = 0; }
            else { e->reserve_bc += v; }
            break;
        case MP_ECON_SCRAP_BASES:
            if (r->pli < g->galaxy_stars) {
                planet_t *p = &g->planet[r->pli];
                if (p->owner == pi) {
                    int n = v;
                    SETRANGE(n, 0, (int)p->missile_bases);
                    p->missile_bases -= n;
                    e->reserve_bc += (n * game_get_base_cost(g, pi)) / 4; /* refund recomputed from OUR state */
                }
            }
            break;
        case MP_ECON_TRADE:
            if ((pa < g->players) && (pa != pi) && IS_AI(g, pa)) {
                SETRANGE(v, 0, 32000);
                game_diplo_set_trade(g, pi, pa, v);
            }
            break;
        case MP_ECON_TECH_FROM:
            if ((pa < g->players) && (pa != pi)) {
                game_tech_get_new(g, pi, (tech_field_t)r->f, r->t, TECHSOURCE_TRADE, pa, PLAYER_NONE, false);
            }
            break;
        case MP_ECON_TECH_TO:
            if ((pa < g->players) && (pa != pi) && IS_AI(g, pa)) {
                game_tech_get_new(g, pa, (tech_field_t)r->f, r->t, TECHSOURCE_TRADE, pi, PLAYER_NONE, false);
            }
            break;
        case MP_ECON_TRIBUTE:
            if ((pa < g->players) && (pa != pi) && IS_AI(g, pa)) {
                SETRANGE(v, 0, 32000);
                if (e->reserve_bc < (uint32_t)v) { v = (int)e->reserve_bc; }
                e->reserve_bc -= v;
                g->eto[pa].reserve_bc += v;
            }
            break;
        case MP_ECON_RELATION:
            /* the audience's net relation outcome (tribute goodwill, threats, annoyance) -- the
               client computed it with the same code; adopt it for both directions, clamped */
            if ((pa < g->players) && (pa != pi) && IS_AI(g, pa)) {
                SETRANGE(v, -100, 100);
                e->relation1[pa] = (int16_t)v;
                g->eto[pa].relation1[pi] = (int16_t)v;
            }
            break;
        default:
            break;
    }
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
        PUT(&p->reserve, sizeof(p->reserve)); /* 1oom-mp: relay reserve-transfer-to-planet (treasury debit derived server-side) */
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
    /* economy actions queued by this player this turn (scrap refunds, audience economics) */
    {
        int cntpos = pos;
        uint16_t cnt = 0;
        PUT(&cnt, 2);
        for (int i = 0; i < s_econ_act_num; ++i) {
            const struct mp_econ_act_s *r = &s_econ_act[i];
            if (r->actor != (uint8_t)pi) { continue; }
            PUT(&r->kind, 1);
            PUT(&r->pa, 1);
            PUT(&r->f, 1);
            PUT(&r->t, 1);
            PUT(&r->v, 4);
            PUT(&r->pli, 2);
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
    s_econ_act_num = 0;
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
        uint32_t reserve_new;
        GET(sl, sizeof(sl));
        GET(lk, sizeof(lk));
        GET(&buildship, 1);
        GET(&reloc, sizeof(reloc));
        GET(&trans_num, sizeof(trans_num));
        GET(&trans_dest, sizeof(trans_dest));
        GET(name, sizeof(name));
        GET(&reserve_new, sizeof(reserve_new));
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
        /* 1oom-mp: apply a reserve-to-planet transfer. The treasury debit is DERIVED from the
           increase in this planet's reserve vs the server's authoritative value, then taken out of
           reserve_bc -- so it composes cleanly with tribute gifts and tax income (pure subtraction,
           never an absolute overwrite). The transfer screen can only ADD to a planet's reserve, so a
           decrease is ignored defensively, and the debit is capped by the treasury on hand. */
        if (reserve_new > p->reserve) {
            uint32_t spent = reserve_new - p->reserve;
            if (spent > e->reserve_bc) { spent = e->reserve_bc; }
            e->reserve_bc -= spent;
            p->reserve += spent;
        }
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
            fleet_enroute_t r;
            GET(&r, sizeof(fleet_enroute_t)); /* always consume the wire entry so the parse stays aligned */
            /* clamp instead of returning -1 mid-loop: the old early-return left enroute[] half-rewritten
               with a stale enroute_num (corrupting the SHARED fleet array the server then resolves on).
               Overflow means the galaxy-wide cap was hit; drop the excess, keep state consistent. */
            if (n < FLEET_ENROUTE_MAX) {
                r.owner = pi; /* anti-cheat: a client can only move its own ships */
                g->enroute[n++] = r;
            }
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
            transport_t tr;
            GET(&tr, sizeof(transport_t)); /* always consume so the parse stays aligned; clamp rather than corrupt */
            if (n < TRANSPORT_MAX) {
                tr.owner = pi;
                g->transport[n++] = tr;
            }
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
            if ((target < g->players) && ((player_id_t)target != pi)) {
                if (s_diplo_pending_num < MP_DIPLO_ACT_MAX) {
                    s_diplo_pending[s_diplo_pending_num].actor = pi;
                    s_diplo_pending[s_diplo_pending_num].target = (player_id_t)target;
                    s_diplo_pending[s_diplo_pending_num].verb = verb;
                    s_diplo_pending[s_diplo_pending_num].arg = arg;
                    memcpy(s_diplo_pending[s_diplo_pending_num].p, p, 4);
                    ++s_diplo_pending_num;
                } else { log_warning("MP: server diplo-pending queue full (%d) -- dropping action from player %d\n", MP_DIPLO_ACT_MAX, (int)pi); }
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
            if (pli < g->galaxy_stars) {
                if (s_colonize_pending_num < MP_COLONIZE_MAX) {
                    s_colonize_pending[s_colonize_pending_num].pi = pi;
                    s_colonize_pending[s_colonize_pending_num].pli = (planet_id_t)pli;
                    memcpy(s_colonize_pending[s_colonize_pending_num].name, cname, PLANET_NAME_LEN);
                    ++s_colonize_pending_num;
                } else { log_warning("MP: server colonize-pending queue full (%d) -- dropping colonize from player %d\n", MP_COLONIZE_MAX, (int)pi); }
            }
        }
    }
    /* economy actions: parse + apply immediately (pre-resolution), so refunds/gifts are in the
       treasury for this turn's production, matching what the player saw locally. */
    {
        uint16_t ecnt = 0;
        GET(&ecnt, 2);
        for (int i = 0; i < ecnt; ++i) {
            struct mp_econ_act_s r;
            GET(&r.kind, 1);
            GET(&r.pa, 1);
            GET(&r.f, 1);
            GET(&r.t, 1);
            GET(&r.v, 4);
            GET(&r.pli, 2);
            econ_apply(g, pi, &r);
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
