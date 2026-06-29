#include "config.h"

#include "ui.h"
#include "mp.h"
#include "game.h"
#include "game_audience.h"
#include "game_diplo.h"
#include "game_mp_orders.h"
#include "game_design.h"
#include "game_aux.h"
#include "game_misc.h"
#include "game_num.h"
#include "game_spy.h"
#include "game_str.h"
#include "game_tech.h"
#include "game_turn.h"
#include "game_turn_start.h"
#include "hw.h"
#include "lbx.h"
#include "lbxfont.h"
#include "lbxgfx.h"
#include "lbxpal.h"
#include "lib.h"
#include "log.h"
#include "types.h"
#include "uibasescrap.h"
#include "uicaught.h"
#include "uicursor.h"
#include "uidefs.h"
#include "uidelay.h"
#include "uidesign.h"
#include "uidraw.h"
#include "uiempirereport.h"
#include "uiempirestatus.h"
#include "uifleet.h"
#include "uigmap.h"
#include "uigameopts.h"
#include "uiobj.h"
#include "uipal.h"
#include "uiplanets.h"
#include "uiraces.h"
#include "uisearch.h"
#include "uispecs.h"
#include "uistarmap.h"
#include "uistarview.h"
#include "uiswitch.h"
#include "uitech.h"

/* -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- */

/* present any pending human-to-human proposals at the start of this player's turn */
/* 1oom-mp: in single-player, first contact with another empire is announced by that empire's
   welcome audience -- but there's no AI to drive that between two humans, so neither side gets the
   introduction. Replay it client-side: when have_met (synced + symmetric) newly flips for a human
   empire, show a first-contact greeting. Bidirectional -- both players get it. Reset per game. */
static bool s_mp_met_known[PLAYER_NUM];
static bool s_mp_met_known_valid = false;

void ui_mp_contact_notify_reset(void) { s_mp_met_known_valid = false; }

static void ui_mp_contact_incoming(struct game_s *g, player_id_t pi)
{
    bool first = !s_mp_met_known_valid;
    for (player_id_t j = PLAYER_0; j < g->players; ++j) {
        if (j == pi) { continue; }
        bool met = (g->eto[pi].have_met[j] != 0);
        bool newly = met && (!s_mp_met_known[j]);
        s_mp_met_known[j] = met;
        if (first || !newly || !IS_HUMAN(g, j)) { continue; } /* AI first contact uses the AI audience relay */
        {   /* show the other leader + a greeting, like the AI's welcome */
            static char contactbuf[192];
            struct audience_s au = {0};
            au.g = g; au.ph = pi; au.pa = j;
            ui_audience_start(&au);
            /* the other player's leader greets you; name their race for a bit of flavor */
            lib_sprintf(contactbuf, sizeof(contactbuf),
                        "So -- another empire reaches for these stars. We are the %s. Whether our meeting ends in alliance or in ash is for us both to decide.",
                        game_str_tbl_races[g->eto[j].race]);
            au.buf = contactbuf;
            ui_audience_show1(&au);
            ui_audience_end(&au);
        }
    }
    s_mp_met_known_valid = true;
}

static void ui_mp_diplo_incoming(struct game_s *g, player_id_t pi)
{
    for (player_id_t j = PLAYER_0; j < g->players; ++j) {
        if ((j == pi) || !IS_HUMAN(g, j)) { continue; }
        if (g->eto[pi].diplo_type[j] != MP_DIPLO_PROPOSAL_MARK) { continue; }
        uint8_t verb = (uint8_t)g->eto[pi].diplo_val[j];
        struct audience_s au = {0};
        au.g = g; au.ph = pi; au.pa = j;
        ui_audience_start(&au);
        au.buf = (verb == MP_DIPLO_PROPOSE_PEACE) ? "We propose a peace treaty. Do you accept?"
               : (verb == MP_DIPLO_PROPOSE_ALLIANCE) ? "We propose an alliance. Do you accept?"
               : "We propose a non-aggression pact. Do you accept?";
        au.strtbl[0] = "Accept"; au.strtbl[1] = "Reject"; au.strtbl[2] = NULL;
        int16_t sel = ui_audience_ask4(&au);
        uint8_t resp = (sel == 0) ? MP_DIPLO_ACCEPT : MP_DIPLO_REJECT;
        game_mp_diplo_record(pi, j, resp, verb);
        g->eto[pi].diplo_type[j] = 0; /* answered this turn */
        au.buf = (resp == MP_DIPLO_ACCEPT) ? "Excellent. So it is agreed." : "A pity.";
        ui_audience_show1(&au);
        ui_audience_end(&au);
    }
}

/* ===================== 1oom-mp: live (synchronous) human-to-human diplomacy =====================
   The ui_mp_diplo_outgoing/_incoming pair above is the legacy async mailbox (propose now, answer
   next turn). This is the real-time replacement: the proposer and responder meet in a live audience
   during the planning phase. mp.c is a store-and-forward referee over the planning socket; here we
   drive the invite->accept->join handshake, then reuse the AI audience UI for the actual exchange.
   The agreed treaty rides the EXISTING order stream: the responder records MP_DIPLO_ACCEPT (which
   game_mp_diplo_apply_pending applies bilaterally at this turn's resolution), so no new game logic. */

#define MP_DIPLO_CL_MSGMAX 16

static void diplo_cl_send(uint16_t id, const uint8_t *p, int n) { if (g_mp_cl_diplo_send) { g_mp_cl_diplo_send(id, p, n); } }
static void diplo_cl_invite(int from, int to) { uint8_t p[4] = { (uint8_t)(from >> 8), (uint8_t)from, (uint8_t)(to >> 8), (uint8_t)to }; diplo_cl_send(MP_MSG_DIPLO_INVITE, p, 4); }
static void diplo_cl_accept(int from, int to, int acc) { uint8_t p[5] = { (uint8_t)(from >> 8), (uint8_t)from, (uint8_t)(to >> 8), (uint8_t)to, (uint8_t)acc }; diplo_cl_send(MP_MSG_DIPLO_ACCEPT, p, 5); }
static void diplo_cl_join(void) { diplo_cl_send(MP_MSG_DIPLO_JOIN, NULL, 0); }
static void diplo_cl_proposal(int sid, int verb) { uint8_t p[3] = { (uint8_t)(sid >> 8), (uint8_t)sid, (uint8_t)verb }; diplo_cl_send(MP_MSG_DIPLO_PROPOSAL, p, 3); }
/* proposal carrying up to 4 parameter bytes (trade amount / tech ids) after the verb */
static void diplo_cl_proposal_p(int sid, int verb, const uint8_t *pp, int np) {
    uint8_t b[3 + 4];
    b[0] = (uint8_t)(sid >> 8); b[1] = (uint8_t)sid; b[2] = (uint8_t)verb;
    if (np > 4) { np = 4; }
    for (int i = 0; i < np; ++i) { b[3 + i] = pp[i]; }
    diplo_cl_send(MP_MSG_DIPLO_PROPOSAL, b, 3 + np);
}
static void diplo_cl_response(int sid, int acc, int verb) { uint8_t p[4] = { (uint8_t)(sid >> 8), (uint8_t)sid, (uint8_t)acc, (uint8_t)verb }; diplo_cl_send(MP_MSG_DIPLO_RESPONSE, p, 4); }
static void diplo_cl_end(int sid) { uint8_t p[5] = { (uint8_t)(sid >> 8), (uint8_t)sid, 0, 0, 0 }; diplo_cl_send(MP_MSG_DIPLO_SESSION_END, p, 5); }

/* Net-pumping modal wait. Draws `msg` centered, and each frame pumps the planning socket (so the
   handshake progresses) + the diplo inbox. Returns the id of the first diplo message received
   (payload copied to out, *outlen clamped), 0 if the user pressed Esc, or -1 if the link dropped /
   the turn began resolving. */
static int ui_mp_diplo_wait(const char *msg, uint8_t *out, int *outlen)
{
    int ret = -1;
    bool done = false;
    while (!done) {
        if (g_mp_cl_poll && g_mp_cl_poll()) { ret = -1; break; } /* RESOLVE_START or link dead */
        if (g_mp_cl_diplo_recv) {
            uint16_t id; uint8_t buf[MP_DIPLO_CL_MSGMAX];
            int n = g_mp_cl_diplo_recv(&id, buf, sizeof(buf));
            if (n >= 0) {
                int cp = (out && outlen) ? ((n < *outlen) ? n : *outlen) : 0;
                if (cp > 0) { memcpy(out, buf, (size_t)cp); }
                if (outlen) { *outlen = cp; }
                ret = (int)id; break;
            }
        }
        ui_delay_prepare();
        if (ui_audience_draw_waiting(msg)) {
            /* 1oom-mp: keep the alien's audience screen up while we wait for the other human, with
               `msg` as the status line, instead of a black "waiting" frame. */
            lbxfont_select(2, 6, 0, 0);
            lbxfont_print_str_center(160, 180, "(Esc to cancel)", UI_SCREEN_W, ui_scale);
        } else {
            ui_draw_erase_buf();
            lbxfont_select(2, 0xd, 0, 0);
            lbxfont_print_str_center(160, 88, msg, UI_SCREEN_W, ui_scale);
            lbxfont_select(2, 6, 0, 0);
            lbxfont_print_str_center(160, 100, "(Esc to cancel)", UI_SCREEN_W, ui_scale);
        }
        uiobj_table_clear();
        (void)uiobj_add_inputkey(MOO_KEY_ESCAPE);
        uiobj_finish_frame();
        if (uiobj_handle_input_cond() == UIOBJI_ESC) { ret = 0; done = true; }
        ui_delay_ticks_or_click(2);
    }
    uiobj_table_clear();
    return ret;
}

/* show a one-line message on the opponent's audience screen (portrait + text + click to dismiss) */
static void ui_mp_diplo_msgbox(struct game_s *g, player_id_t pi, player_id_t pa, const char *msg)
{
    struct audience_s au = {0};
    au.g = g; au.ph = pi; au.pa = pa;
    ui_audience_start(&au);
    au.buf = msg;
    ui_audience_show1(&au);
    ui_audience_end(&au);
}

/* 1oom-mp: Esc -> Save in a networked game. The single-player save would only write a LOCAL,
   non-resumable file on this machine; instead ask the authoritative SERVER to write a named snapshot
   (mp_save_<game_id>_y<year>.blob, resumable with -mpload), then confirm. */
void ui_mp_save_game(struct game_s *g)
{
    int pi;
    if (!g_mp_cl_diplo_send) { return; } /* not in a live MP turn */
    g_mp_cl_diplo_send(MP_MSG_SAVE_REQUEST, NULL, 0);
    pi = mp_cl_player_id();
    if ((pi >= 0) && (pi < g->players)) {
        ui_mp_diplo_msgbox(g, (player_id_t)pi, (player_id_t)pi, "Our records are committed to the archives. (Game saved on the server.)");
    }
}

/* proposer side of an open session: pick a treaty action (the legacy menu), relay it live, and
   show the responder's live answer. Consensual deals are applied by the RESPONDER's ACCEPT record;
   unilateral acts (declare war / break treaty) are recorded here. */
/* optimistic local application of an agreed deal, so it takes effect on this client immediately
   rather than only after the next state sync. The server applies the same change authoritatively at
   this turn's resolution (from the responder's recorded order), so the two converge; a rare
   divergence self-corrects on the next GAME_DATA. game_diplo_* are bilateral. */
static void mp_diplo_apply_local(struct game_s *g, player_id_t a, player_id_t b, uint8_t verb)
{
    switch (verb) {
        case MP_DIPLO_PROPOSE_ALLIANCE: game_diplo_set_treaty(g, a, b, TREATY_ALLIANCE); break;
        case MP_DIPLO_PROPOSE_NAP:      game_diplo_set_treaty(g, a, b, TREATY_NONAGGRESSION); break;
        case MP_DIPLO_PROPOSE_PEACE:    game_diplo_stop_war(g, a, b); break;
        case MP_DIPLO_DECLARE_WAR:      game_diplo_start_war(g, a, b); break;
        case MP_DIPLO_BREAK_TREATY:     game_diplo_break_treaty(g, a, b); break;
        case MP_DIPLO_BREAK_TRADE:      game_diplo_break_trade(g, a, b); break;
        default: break;
    }
}

/* 1oom-mp: show a one-line notice on the SHARED session audience and wait for a click. The alien's
   portrait stays up the whole time (no end/start), so the conversation never blanks to black. */
static void diplo_au_msg(struct audience_s *au, const char *msg)
{
    au->buf = msg;
    ui_audience_show1(au);
}

/* relay already sent; block for the responder's live answer. Returns 1 accepted, 0 rejected, -1 if
   they departed / the link dropped (a "departed" notice is shown in that case). The shared session
   audience stays on screen throughout -- the wait loop keeps the portrait up instead of a black frame. */
static int mp_diplo_await_answer(struct audience_s *au)
{
    uint8_t buf[MP_DIPLO_CL_MSGMAX]; int len = sizeof(buf);
    int id = ui_mp_diplo_wait("Awaiting their decision...", buf, &len);
    if (id == MP_MSG_DIPLO_RESPONSE) { return ((len >= 3) && buf[2]) ? 1 : 0; }
    if (id == MP_MSG_DIPLO_CANCEL || id == MP_MSG_DIPLO_SESSION_END) {
        diplo_au_msg(au, "They have departed.");
    }
    return -1;
}

/* "Form Trade Agreement": offer a BC/year amount (scaled to the smaller economy, exactly like the
   AI audience menu), relay it, and on a live accept set the trade locally (server confirms at
   resolution from the responder's ACCEPT record). */
static void mp_diplo_propose_trade(struct game_s *g, player_id_t pi, player_id_t pa, int sid, struct audience_s *au)
{
    int cur = (int)g->eto[pi].trade_bc[pa];
    int prod = (int)((g->eto[pi].total_production_bc < g->eto[pa].total_production_bc) ? g->eto[pi].total_production_bc : g->eto[pa].total_production_bc);
    int want, num = 0;
    uint16_t bctbl[AUDIENCE_BC_MAX];
    char strbuf[AUDIENCE_BC_MAX][32];
    prod /= 4; if (prod > 32000) { prod = 32000; }
    want = (prod / 25) * 25 - cur;
    if (want <= 0) { diplo_au_msg(au, "There is no room for a larger trade agreement."); return; }
    if (want < 125) {
        num = want / 25;
        for (int i = 0; i < num; ++i) { bctbl[i] = (uint16_t)(cur + 25 * (i + 1)); }
    } else {
        num = AUDIENCE_BC_MAX;
        bctbl[0] = (uint16_t)(want / 5 + cur);
        bctbl[1] = (uint16_t)((((want * 2) / 5) / 25) * 25 + cur);
        bctbl[2] = (uint16_t)((((want * 3) / 5) / 25) * 25 + cur);
        bctbl[3] = (uint16_t)((((want * 4) / 5) / 25) * 25 + cur);
        bctbl[4] = (uint16_t)(want + cur);
    }
    if (num < 1) { return; }
    for (int i = 0; i < num; ++i) {
        lib_sprintf(strbuf[i], sizeof(strbuf[i]), "%s %u %s", game_str_au_bull, bctbl[i], game_str_au_bcpery);
        au->strtbl[i] = strbuf[i];
    }
    au->strtbl[num] = "Forget it";
    au->strtbl[num + 1] = NULL;
    au->buf = "Propose a trade agreement of:";
    int16_t sel = ui_audience_ask4(au);
    if ((sel < 0) || (sel >= num)) { return; }
    {
        uint16_t amt = bctbl[sel];
        uint8_t pp[2] = { (uint8_t)(amt & 0xff), (uint8_t)(amt >> 8) };
        diplo_cl_proposal_p(sid, MP_DIPLO_PROPOSE_TRADE, pp, 2);
        int acc = mp_diplo_await_answer(au);
        if (acc >= 0) {
            if (acc) { game_diplo_set_trade(g, pi, pa, (int)amt); }
            diplo_au_msg(au, acc ? "Excellent. So it is agreed." : "They refuse our offer.");
        }
    }
}

/* "Exchange Technology": pick a tech to receive (one they have, we lack) and one to give in
   return (one we have, they lack), relay the swap, and on accept the techs transfer at the next
   state sync (granted server-side from the responder's ACCEPT record). */
static void mp_diplo_propose_tech(struct game_s *g, player_id_t pi, player_id_t pa, int sid, struct audience_s *au)
{
    struct spy_esp_s s[1];
    tech_field_t want_f[TECH_SPY_MAX], give_f[TECH_SPY_MAX];
    uint8_t want_t[TECH_SPY_MAX], give_t[TECH_SPY_MAX];
    char namebuf[TECH_SPY_MAX][48];
    int want_num = 0, give_num = 0;
    s->spy = pi; s->target = pa; /* techs pa has that pi lacks = what we can RECEIVE */
    if (game_spy_esp_sub1(g, s, 0, 1) > 0) {
        want_num = (s->tnum < (AUDIENCE_STR_MAX - 2)) ? s->tnum : (AUDIENCE_STR_MAX - 2); /* leave room for "Forget it" + NULL */
        for (int i = 0; i < want_num; ++i) { want_f[i] = s->tbl_field[i]; want_t[i] = s->tbl_tech2[i]; }
    }
    s->spy = pa; s->target = pi; /* techs pi has that pa lacks = what we can GIVE */
    if (game_spy_esp_sub1(g, s, 0, 0) > 0) {
        give_num = (s->tnum < (AUDIENCE_STR_MAX - 2)) ? s->tnum : (AUDIENCE_STR_MAX - 2); /* leave room for "Forget it" + NULL */
        for (int i = 0; i < give_num; ++i) { give_f[i] = s->tbl_field[i]; give_t[i] = s->tbl_tech2[i]; }
    }
    if (want_num == 0) { diplo_au_msg(au, "They have no technology we lack."); return; }
    if (give_num == 0) { diplo_au_msg(au, "We have no technology they lack."); return; }
    for (int i = 0; i < want_num; ++i) { au->strtbl[i] = game_tech_get_name(g->gaux, want_f[i], want_t[i], namebuf[i], sizeof(namebuf[i])); }
    au->strtbl[want_num] = "Forget it";
    au->strtbl[want_num + 1] = NULL;
    au->buf = "Which of their technologies do you desire?";
    int16_t selw = ui_audience_ask4(au);
    if ((selw < 0) || (selw >= want_num)) { return; }
    for (int i = 0; i < give_num; ++i) { au->strtbl[i] = game_tech_get_name(g->gaux, give_f[i], give_t[i], namebuf[i], sizeof(namebuf[i])); }
    au->strtbl[give_num] = "Forget it";
    au->strtbl[give_num + 1] = NULL;
    au->buf = "Which of ours will you give in exchange?";
    int16_t selg = ui_audience_ask4(au);
    if ((selg < 0) || (selg >= give_num)) { return; }
    {
        uint8_t pp[4] = { (uint8_t)want_f[selw], want_t[selw], (uint8_t)give_f[selg], give_t[selg] };
        diplo_cl_proposal_p(sid, MP_DIPLO_PROPOSE_TECH, pp, 4);
        int acc = mp_diplo_await_answer(au);
        if (acc >= 0) {
            if (acc) { game_tech_get_new(g, pi, (tech_field_t)pp[0], pp[1], TECHSOURCE_TRADE, pa, PLAYER_NONE, false); } /* 1oom-mp: optimistic -- the tech we wanted is usable THIS turn (server re-grants authoritatively at resolution) */
            diplo_au_msg(au, acc ? "Excellent. The exchange is agreed." : "They refuse our offer.");
        }
    }
}

/* proposer side of an open session: a top-level menu mirroring the AI audience (propose treaty /
   form trade / exchange technology / break treaty or trade), one action per session. Consensual
   deals are relayed and applied by the responder's ACCEPT record; unilateral acts are recorded here. */
static void ui_mp_diplo_session_propose(struct game_s *g, player_id_t pi, player_id_t pa, int sid, struct audience_s *au)
{
    treaty_t tr = g->eto[pi].treaty[pa];
    const char *mopts[6]; int mcat[6]; int mn = 0;
    mopts[mn] = "Propose treaty"; mcat[mn] = 0; ++mn;
    mopts[mn] = "Form trade agreement"; mcat[mn] = 1; ++mn;
    mopts[mn] = "Exchange technology"; mcat[mn] = 2; ++mn;
    mopts[mn] = "Break treaty or trade"; mcat[mn] = 3; ++mn;
    mopts[mn] = "Forget it"; mcat[mn] = 4; ++mn;
    for (int i = 0; i < mn; ++i) { au->strtbl[i] = mopts[i]; }
    au->strtbl[mn] = NULL;
    au->buf = "What is your will?";
    int16_t msel = ui_audience_ask4(au);
    int cat = ((msel >= 0) && (msel < mn)) ? mcat[msel] : 4;
    if (cat == 1) { mp_diplo_propose_trade(g, pi, pa, sid, au); return; }
    if (cat == 2) { mp_diplo_propose_tech(g, pi, pa, sid, au); return; }
    if (cat == 4) { return; }
    {
        const char *opts[AUDIENCE_STR_MAX];
        uint8_t verbs[AUDIENCE_STR_MAX];
        int n = 0;
        if (cat == 0) { /* propose a treaty (consensual) */
            if (tr == TREATY_WAR) { opts[n] = "Propose peace treaty"; verbs[n] = MP_DIPLO_PROPOSE_PEACE; ++n; }
            else {
                if (tr == TREATY_NONE) { opts[n] = "Non-aggression pact"; verbs[n] = MP_DIPLO_PROPOSE_NAP; ++n; }
                if ((tr == TREATY_NONE) || (tr == TREATY_NONAGGRESSION)) { opts[n] = "Alliance"; verbs[n] = MP_DIPLO_PROPOSE_ALLIANCE; ++n; }
            }
        } else { /* cat == 3: break / declare (unilateral) */
            /* 1oom-mp teams: never offer hostile acts against a teammate -- the alliance is locked.
               (Only the per-empire trade agreement can still be broken.) War/break-treaty against an
               ENEMY is routed through team consensus, not applied unilaterally from here. */
            bool teammate = (g->mp_team[pi] != 0) && (g->mp_team[pi] == g->mp_team[pa]);
            if ((tr != TREATY_WAR) && !teammate) { opts[n] = "Declare war"; verbs[n] = MP_DIPLO_DECLARE_WAR; ++n; }
            if (((tr == TREATY_NONAGGRESSION) || (tr == TREATY_ALLIANCE)) && !teammate) { opts[n] = "Break treaty"; verbs[n] = MP_DIPLO_BREAK_TREATY; ++n; }
            if (g->eto[pi].trade_bc[pa] != 0) { opts[n] = "Break trade agreement"; verbs[n] = MP_DIPLO_BREAK_TRADE; ++n; }
        }
        if (n == 0) { diplo_au_msg(au, "There is nothing to propose."); return; }
        opts[n] = "Forget it"; verbs[n] = MP_DIPLO_NONE; ++n;
        for (int i = 0; i < n; ++i) { au->strtbl[i] = opts[i]; }
        au->strtbl[n] = NULL;
        au->buf = "What is your will?";
        int16_t sel = ui_audience_ask4(au);
        uint8_t verb = ((sel >= 0) && (sel < n)) ? verbs[sel] : MP_DIPLO_NONE;
        if (verb == MP_DIPLO_NONE) { return; }
        if ((verb == MP_DIPLO_DECLARE_WAR) || (verb == MP_DIPLO_BREAK_TREATY) || (verb == MP_DIPLO_BREAK_TRADE)) {
            game_mp_diplo_record(pi, pa, verb, 0); /* unilateral: applies at resolution */
            mp_diplo_apply_local(g, pi, pa, verb); /* and immediately, locally */
            diplo_cl_proposal(sid, verb); /* 1oom-mp: relay it so the TARGET is notified + applies it too (was: never relayed -> target saw nothing, war waited for next turn) */
            diplo_au_msg(au, "It is done.");
            return;
        }
        /* consensual treaty proposal: relay + await the live answer (portrait stays up via the shared au) */
        diplo_cl_proposal(sid, verb);
        {
            int acc = mp_diplo_await_answer(au);
            if (acc >= 0) {
                if (acc) { mp_diplo_apply_local(g, pi, pa, verb); }
                diplo_au_msg(au, acc ? "Excellent. So it is agreed." : "They refuse our offer.");
            }
        }
    }
}

/* responder side of an open session: wait for the proposer's offer, then accept/reject it live.
   On accept, record MP_DIPLO_ACCEPT so the treaty applies bilaterally at this turn's resolution. */
static void ui_mp_diplo_session_respond(struct game_s *g, player_id_t pi, player_id_t pa, int sid, struct audience_s *au)
{
    uint8_t buf[MP_DIPLO_CL_MSGMAX]; int len = sizeof(buf);
    /* 1oom-mp: the shared session audience is already on screen; the wait loop keeps the alien's
       portrait up with this status line -- no black/map frame while we await their envoy. */
    int id = ui_mp_diplo_wait("Receiving their envoy...", buf, &len);
    if (id != MP_MSG_DIPLO_PROPOSAL) { return; } /* session ended / cancelled without an offer */
    uint8_t verb = (len >= 3) ? buf[2] : MP_DIPLO_NONE;
    uint8_t pp[4] = { 0, 0, 0, 0 };
    int pn = len - 3;
    if (pn < 0) { pn = 0; } if (pn > 4) { pn = 4; }
    for (int i = 0; i < pn; ++i) { pp[i] = buf[3 + i]; }
    if ((verb == MP_DIPLO_DECLARE_WAR) || (verb == MP_DIPLO_BREAK_TREATY) || (verb == MP_DIPLO_BREAK_TRADE)) {
        /* 1oom-mp: a unilateral act, not a proposal -> apply it locally (live immediately, like the
           proposer) and show a notice on the alien portrait, instead of a bogus accept/reject prompt. */
        mp_diplo_apply_local(g, pi, pa, verb);
        diplo_au_msg(au,
              (verb == MP_DIPLO_DECLARE_WAR)  ? "They have declared war upon us!"
            : (verb == MP_DIPLO_BREAK_TREATY) ? "They have broken our treaty!"
            :                                   "They have broken our trade agreement!");
        return;
    }
    char msg[200];
    if (verb == MP_DIPLO_PROPOSE_TRADE) {
        lib_sprintf(msg, sizeof(msg), "They propose a trade agreement of %d %s. Do you accept?", pp[0] | (pp[1] << 8), game_str_au_bcpery);
        au->buf = msg;
    } else if (verb == MP_DIPLO_PROPOSE_TECH) {
        char wn[48], gn[48];
        game_tech_get_name(g->gaux, (tech_field_t)pp[0], pp[1], wn, sizeof(wn)); /* the tech they want from us */
        game_tech_get_name(g->gaux, (tech_field_t)pp[2], pp[3], gn, sizeof(gn)); /* the tech they offer us */
        lib_sprintf(msg, sizeof(msg), "They offer %s in exchange for your %s. Do you accept?", gn, wn);
        au->buf = msg;
    } else {
        au->buf = (verb == MP_DIPLO_PROPOSE_PEACE) ? "They propose a peace treaty. Do you accept?"
               : (verb == MP_DIPLO_PROPOSE_ALLIANCE) ? "They propose an alliance. Do you accept?"
               : "They propose a non-aggression pact. Do you accept?";
    }
    au->strtbl[0] = "Accept"; au->strtbl[1] = "Reject"; au->strtbl[2] = NULL;
    int16_t sel = ui_audience_ask4(au);
    int acc = (sel == 0) ? 1 : 0;
    diplo_cl_response(sid, acc, verb);
    if (acc) {
        if ((verb == MP_DIPLO_PROPOSE_TRADE) || (verb == MP_DIPLO_PROPOSE_TECH)) {
            game_mp_diplo_record_p(pi, pa, MP_DIPLO_ACCEPT, verb, pp); /* authoritative at resolution */
            if (verb == MP_DIPLO_PROPOSE_TRADE) { game_diplo_set_trade(g, pi, pa, pp[0] | (pp[1] << 8)); } /* optimistic */
            else { game_tech_get_new(g, pi, (tech_field_t)pp[2], pp[3], TECHSOURCE_TRADE, pa, PLAYER_NONE, false); } /* 1oom-mp: optimistic -- the offered tech is usable THIS turn (server re-grants authoritatively at resolution) */
        } else {
            game_mp_diplo_record(pi, pa, MP_DIPLO_ACCEPT, verb); /* authoritative at resolution */
            mp_diplo_apply_local(g, pi, pa, verb);               /* and immediately, locally */
        }
    }
    diplo_au_msg(au, acc ? "Excellent. So it is agreed." : "A pity.");
}

/* enter an open session in the given role (0 = proposer, 1 = responder), then close it. */
static void ui_mp_diplo_run_session(struct game_s *g, player_id_t pi, const uint8_t *buf, int len, struct audience_s *existing_au)
{
    int sid = (len >= 2) ? ((buf[0] << 8) | buf[1]) : 0;
    int proposer = (len >= 4) ? ((buf[2] << 8) | buf[3]) : pi;
    int responder = (len >= 6) ? ((buf[4] << 8) | buf[5]) : pi;
    int role = (len >= 7) ? buf[6] : 0;
    player_id_t peer = (role == 0) ? (player_id_t)responder : (player_id_t)proposer;
    if (peer >= g->players) { diplo_cl_end(sid); return; }
    /* 1oom-mp: ONE audience for the whole conversation -- single fade-in on entry, single fade-out on
       exit; every step reuses this portrait so it never blacks out mid-talk. The RESPONDER reaches
       here with the portrait ALREADY up + faded in (from accepting the envoy) and passes it in via
       `existing_au` -- we must NOT start/end our own, because the end would fade to black and the
       responder's next step is a wait (no fade-in step) so it would never come back. The PROPOSER
       arrives from the starmap with no audience (existing_au == NULL), so we start + end one here. */
    struct audience_s au_local = {0};
    struct audience_s *au = existing_au;
    if (!au) {
        au_local.g = g; au_local.ph = pi; au_local.pa = peer;
        ui_audience_start(&au_local);
        au = &au_local;
    }
    if (role == 0) { ui_mp_diplo_session_propose(g, pi, peer, sid, au); }
    else { ui_mp_diplo_session_respond(g, pi, peer, sid, au); }
    if (!existing_au) { ui_audience_end(&au_local); }
    diplo_cl_end(sid);
}

/* 1oom-mp: live p2p audience handshake is NON-INTERRUPTING. An incoming request becomes a starmap
   notification (ui_mp_diplo_invite_pending) the player answers when ready; the proposer doesn't block.
   ui_mp_diplo_pump advances both sides each frame and the main loop only breaks out to enter the
   (still synchronous) session, or to report that my own invite bounced. */
static int s_mp_diplo_invite_to = -1;       /* proposer: who I invited (names a bounce notice), -1 none */
static int s_mp_diplo_invite_result = 0;    /* proposer: 1 busy / 2 declined / 3 expired; 0 = none */
static bool s_mp_diplo_sess_open = false;   /* a session opened for me -> enter it */
static uint8_t s_mp_diplo_sess_buf[MP_DIPLO_CL_MSGMAX];
static int s_mp_diplo_sess_len = 0;

/* proposer entry: fired from the AUDIENCE action when the target is human. Sends the invite and
   returns immediately -- the player keeps playing; the pump pulls them into the session once the
   target accepts, or surfaces a bounce notice. */
static void ui_mp_diplo_initiate(struct game_s *g, player_id_t pi, player_id_t opponi)
{
    if (!g_mp_cl_diplo_send) { return; } /* not a live MP turn */
    diplo_cl_invite(pi, opponi);
    s_mp_diplo_invite_to = opponi;
    ui_mp_diplo_msgbox(g, pi, opponi, "Our envoy is dispatched; they will receive us when they are ready.");
}

/* incoming invite waiting for me to answer (proposer id), -1 = none. A notification, not a breakout. */
static int s_mp_diplo_invite_from = -1;

/* ---- 1oom-mp teams: foreign policy by consensus -------------------------------------------------
   A human's stance change toward an enemy (peace / NAP / break-treaty; war on a human enemy) is
   offered to every HUMAN teammate as a proposal -- AI teammates auto-follow. It enacts only on
   unanimous assent, then applies team-wide via the stage-1 sync inside the diplo primitives.
   Messages ride MP_MSG_TEAM_STANCE (byte 4 = kind) through the same inbox/pump as the p2p audience. */
#define MP_TEAM_KIND_PROPOSE 0
#define MP_TEAM_KIND_VOTE    1
#define MP_TEAM_KIND_ENACT   2
static void team_stance_send(int kind, int from, int to, int enemy, int stance, int accept) {
    uint8_t p[8] = { (uint8_t)(from >> 8), (uint8_t)from, (uint8_t)(to >> 8), (uint8_t)to,
                     (uint8_t)kind, (uint8_t)enemy, (uint8_t)stance, (uint8_t)accept };
    diplo_cl_send(MP_MSG_TEAM_STANCE, p, 8);
}
/* proposer side: my pending outgoing proposal + the bitmask of teammates I still await a vote from. */
static int s_team_prop_enemy = -1;
static uint8_t s_team_prop_stance = 0;
static uint32_t s_team_prop_await = 0;
static int s_team_prop_result = 0;   /* 1 approved, 2 rejected, 0 none -> a pump breakout for _handle */
static int s_team_prop_announce = -1;/* enemy to confirm "proposal sent" once back on the starmap (avoids a nested modal), -1 none */
static bool s_team_prop_busy = false;/* 1oom-mp: the pending confirm is a "one proposal already out, please wait" notice, not "sent" */
/* teammate side: an incoming proposal to vote on (drives the starmap banner). */
static int s_team_vote_from = -1, s_team_vote_enemy = -1;
static uint8_t s_team_vote_stance = 0;
/* teammate side: an approved proposal to apply locally this frame (deferred from _pump, needs g). */
static int s_team_enact_from = -1, s_team_enact_enemy = -1;
static uint8_t s_team_enact_stance = 0;

static void mp_team_stance_apply(struct game_s *g, player_id_t actor, player_id_t enemy, uint8_t stance) {
    switch (stance) {
        case MP_TEAM_STANCE_PEACE: game_diplo_stop_war(g, actor, enemy); break;
        case MP_TEAM_STANCE_NAP:   game_diplo_set_treaty(g, actor, enemy, TREATY_NONAGGRESSION); break;
        case MP_TEAM_STANCE_BREAK:  game_diplo_break_treaty(g, actor, enemy); break;
        case MP_TEAM_STANCE_WAR:    game_diplo_start_war(g, actor, enemy); break;
        default: break;
    }
}
static uint8_t team_stance_to_diplo_verb(uint8_t stance) {
    switch (stance) {
        case MP_TEAM_STANCE_PEACE: return MP_DIPLO_PROPOSE_PEACE;
        case MP_TEAM_STANCE_NAP:   return MP_DIPLO_PROPOSE_NAP;
        case MP_TEAM_STANCE_BREAK:  return MP_DIPLO_BREAK_TREATY;
        case MP_TEAM_STANCE_WAR:    return MP_DIPLO_DECLARE_WAR;
        default: return MP_DIPLO_NONE;
    }
}
/* installed as g_mp_team_stance_propose_hook. Relays the stance to each human teammate and returns
   true (captured) so the caller does NOT apply it; false when there is nobody to consult. */
static bool mp_team_stance_propose(struct game_s *g, player_id_t pi, player_id_t enemy, mp_team_stance_t stance) {
    uint32_t await = 0;
    if (!g_mp_cl_diplo_send) { return false; }   /* not a live MP turn */
    if (g->mp_team[pi] == 0) { return false; }    /* not on a team */
    if (s_team_prop_enemy >= 0) { s_team_prop_announce = enemy; s_team_prop_busy = true; return true; } /* one proposal at a time: notify (don't apply/send a 2nd), so it isn't silently dropped */
    for (player_id_t t = PLAYER_0; t < g->players; ++t) {
        if ((t == pi) || (g->mp_team[t] != g->mp_team[pi]) || (!IS_HUMAN(g, t))) { continue; }
        await |= (1u << t);
    }
    if (await == 0) { return false; }             /* no human teammates -> caller applies (AI auto-follows) */
    for (player_id_t t = PLAYER_0; t < g->players; ++t) {
        if (await & (1u << t)) { team_stance_send(MP_TEAM_KIND_PROPOSE, (int)pi, (int)t, (int)enemy, (int)stance, 0); }
    }
    s_team_prop_enemy = enemy; s_team_prop_stance = (uint8_t)stance; s_team_prop_await = await; s_team_prop_result = 0;
    s_team_prop_announce = enemy; /* confirmation is shown after the audience closes (a nested modal would be unsafe) */
    return true;
}


/* async p2p diplo pump: called each starmap frame. Drains the inbox and advances the invite handshake
   WITHOUT interrupting play. A pending incoming invite becomes a notification; READY auto-joins (only
   as the proposer); SESSION_OPEN is captured to enter next. Returns true ONLY when the main loop must
   break out -- to enter an opened session, or to report that my own invite bounced. */
bool ui_mp_diplo_pump(int pi)
{
    if (g_mp_cl_diplo_recv) {
        uint16_t id; uint8_t buf[MP_DIPLO_CL_MSGMAX]; int n;
        while ((n = g_mp_cl_diplo_recv(&id, buf, sizeof(buf))) >= 0) {
            if (id == MP_MSG_DIPLO_INVITE_NOTIFY) {
                int from = (n >= 2) ? ((buf[0] << 8) | buf[1]) : -1;
                int to   = (n >= 4) ? ((buf[2] << 8) | buf[3]) : -1;
                if ((to == pi) && (from >= 0)) { s_mp_diplo_invite_from = from; }
            } else if (id == MP_MSG_DIPLO_INVITE_RESULT) {
                int status = (n >= 5) ? buf[4] : 0;
                if (status != 0) { s_mp_diplo_invite_result = status; } /* my invite bounced */
            } else if (id == MP_MSG_DIPLO_READY) {
                int proposer = (n >= 2) ? ((buf[0] << 8) | buf[1]) : -1;
                if (proposer == pi) { diplo_cl_join(); } /* proposer joins; both await SESSION_OPEN */
            } else if (id == MP_MSG_DIPLO_SESSION_OPEN) {
                int cp = (n > (int)sizeof(s_mp_diplo_sess_buf)) ? (int)sizeof(s_mp_diplo_sess_buf) : n;
                if (cp < 0) { cp = 0; }
                if (cp > 0) { memcpy(s_mp_diplo_sess_buf, buf, (size_t)cp); }
                s_mp_diplo_sess_len = cp;
                s_mp_diplo_sess_open = true;
                s_mp_diplo_invite_from = -1; s_mp_diplo_invite_to = -1;
            } else if (id == MP_MSG_DIPLO_CANCEL) {
                s_mp_diplo_invite_from = -1; s_mp_diplo_invite_to = -1;
            } else if (id == MP_MSG_TEAM_STANCE) {
                int tfrom = (n >= 2) ? ((buf[0] << 8) | buf[1]) : -1;
                int tto   = (n >= 4) ? ((buf[2] << 8) | buf[3]) : -1;
                int tkind = (n >= 5) ? buf[4] : 0;
                int tenemy= (n >= 6) ? buf[5] : -1;
                int tst   = (n >= 7) ? buf[6] : 0;
                int tacc  = (n >= 8) ? buf[7] : 0;
                if (tto != pi) { /* not addressed to me */ }
                else if (tkind == MP_TEAM_KIND_PROPOSE) {                 /* a teammate asks me to vote */
                    s_team_vote_from = tfrom; s_team_vote_enemy = tenemy; s_team_vote_stance = (uint8_t)tst;
                } else if (tkind == MP_TEAM_KIND_VOTE) {                  /* a vote on my proposal (proposer side) */
                    /* 1oom-mp: only count a vote from a teammate we are ACTUALLY awaiting -- bound tfrom
                       (1u<<tfrom is UB for tfrom>=32) and require its await bit set, so a stray / duplicate
                       / spoofed vote can't clear the wrong bit or drive the tally to "approved" without
                       real unanimous assent. */
                    if ((tfrom >= 0) && (tfrom < MP_MAX_PLAYERS)
                     && (s_team_prop_enemy == tenemy) && (s_team_prop_stance == (uint8_t)tst) && (s_team_prop_result == 0)
                     && (s_team_prop_await & (1u << tfrom))) {
                        if (!tacc) { s_team_prop_result = 2; s_team_prop_await = 0; }              /* any refusal kills it */
                        else { s_team_prop_await &= ~(1u << tfrom); if (s_team_prop_await == 0) { s_team_prop_result = 1; } }
                    }
                } else if (tkind == MP_TEAM_KIND_ENACT) {                 /* my proposal passed; apply locally (needs g -> _handle) */
                    s_team_enact_from = tfrom; s_team_enact_enemy = tenemy; s_team_enact_stance = (uint8_t)tst;
                }
            }
        }
    }
    return s_mp_diplo_sess_open || (s_mp_diplo_invite_result != 0) || (s_team_prop_result != 0) || (s_team_enact_from >= 0) || (s_team_prop_announce >= 0);
}

/* starmap: proposer id of a pending incoming audience request (for the notification banner), else -1. */
int ui_mp_diplo_invite_pending(void) { return s_mp_diplo_invite_from; }

/* starmap: proposer id of a pending TEAM stance proposal awaiting my vote (notification banner), else -1. */
int ui_mp_team_vote_pending(void) { return s_team_vote_from; }


/* main-loop entry: reached when ui_mp_diplo_pump asked to break out, OR when the player answered the
   invite notification on the starmap. Routes by state: a just-opened session -> run it; my invite
   bounced -> notice; an incoming invite I chose to answer -> receive/decline (accept -> the pump
   captures SESSION_OPEN and enters the session next tick). */
void ui_mp_diplo_handle(struct game_s *g, int pi)
{
    if (s_team_prop_announce >= 0) {             /* confirm a just-sent proposal, now that the audience has closed */
        player_id_t e = (player_id_t)s_team_prop_announce; bool busy = s_team_prop_busy;
        s_team_prop_announce = -1; s_team_prop_busy = false;
        ui_mp_diplo_msgbox(g, pi, e, busy
            ? "An envoy is already consulting our allies; we must await their answer." /* 1oom-mp: a 2nd proposal while one is pending -- tell the player, don't silently drop it */
            : "Our envoy will consult our allies; we shall have their answer soon.");
        return;
    }
    if (s_team_enact_from >= 0) {                /* a teammate's proposal passed -> apply it on my client too */
        player_id_t a = (player_id_t)s_team_enact_from, e = (player_id_t)s_team_enact_enemy; uint8_t st = s_team_enact_stance;
        s_team_enact_from = -1;
        if ((a >= g->players) || (e >= g->players)) { return; } /* 1oom-mp: reject out-of-range ids from the wire (would OOB-index eto[]/race tables in apply+msgbox) */
        mp_team_stance_apply(g, a, e, st);
        ui_mp_diplo_msgbox(g, pi, e, "Our alliance has agreed a new course; it is done.");
        return;
    }
    if (s_team_prop_result != 0) {               /* my proposal resolved (proposer side) */
        int res = s_team_prop_result; player_id_t e = (player_id_t)s_team_prop_enemy; uint8_t st = s_team_prop_stance;
        s_team_prop_result = 0; s_team_prop_enemy = -1; s_team_prop_await = 0;
        if (res == 1) {
            mp_team_stance_apply(g, (player_id_t)pi, e, st);                                       /* optimistic + team-wide */
            if (IS_HUMAN(g, e)) { game_mp_diplo_record((player_id_t)pi, e, team_stance_to_diplo_verb(st), 0); } /* authoritative for a human enemy */
            for (player_id_t t = PLAYER_0; t < g->players; ++t) {
                if ((t != (player_id_t)pi) && (g->mp_team[t] == g->mp_team[pi]) && IS_HUMAN(g, t)) {
                    team_stance_send(MP_TEAM_KIND_ENACT, (int)pi, (int)t, (int)e, (int)st, 0);     /* teammates apply same-turn */
                }
            }
            ui_mp_diplo_msgbox(g, pi, e, "Our allies assent. It is done.");
        } else {
            ui_mp_diplo_msgbox(g, pi, e, "Our allies refuse; the proposal is withdrawn.");
        }
        return;
    }
    if (s_team_vote_from >= 0) {                  /* an ally asks me to assent to a stance (teammate side) */
        int from = s_team_vote_from; player_id_t e = (player_id_t)s_team_vote_enemy; uint8_t st = s_team_vote_stance;
        s_team_vote_from = -1;
        if ((from < 0) || (from >= (int)g->players) || (e >= g->players)) { return; } /* 1oom-mp: reject out-of-range ids from the wire (au.pa + game_str_tbl_race[eto[e].race]) */
        const char *act = (st == MP_TEAM_STANCE_PEACE) ? "make peace with"
                        : (st == MP_TEAM_STANCE_NAP)   ? "sign a non-aggression pact with"
                        : (st == MP_TEAM_STANCE_WAR)   ? "declare war on"
                        :                                "break our treaty with";
        struct audience_s au = {0}; char msg[200];
        au.g = g; au.ph = pi; au.pa = (player_id_t)from;
        ui_audience_start(&au);
        lib_sprintf(msg, sizeof(msg), "Our ally proposes that we %s the %s. Do you assent?", act, game_str_tbl_race[g->eto[e].race]);
        au.buf = msg; au.strtbl[0] = "Assent"; au.strtbl[1] = "Refuse"; au.strtbl[2] = NULL;
        int16_t sel = ui_audience_ask4(&au);
        ui_audience_end(&au);
        team_stance_send(MP_TEAM_KIND_VOTE, (int)pi, from, (int)e, (int)st, (sel == 0) ? 1 : 0);
        return;
    }
    if (s_mp_diplo_sess_open) {
        s_mp_diplo_sess_open = false;
        ui_mp_diplo_run_session(g, pi, s_mp_diplo_sess_buf, s_mp_diplo_sess_len, NULL); /* proposer: no audience up yet -> start + end one inside */
        return;
    }
    if (s_mp_diplo_invite_result != 0) {
        int st = s_mp_diplo_invite_result; s_mp_diplo_invite_result = 0;
        player_id_t pa = (s_mp_diplo_invite_to >= 0) ? (player_id_t)s_mp_diplo_invite_to : (player_id_t)pi;
        s_mp_diplo_invite_to = -1;
        if (pa != (player_id_t)pi) {
            ui_mp_diplo_msgbox(g, pi, pa,
                (st == 1) ? "They are engaged in another audience." :
                (st == 2) ? "They declined to receive our envoy." :
                            "The moment passed. Try again next turn.");
        }
        return;
    }
    int from = s_mp_diplo_invite_from;
    s_mp_diplo_invite_from = -1;
    if ((from < 0) || (from >= g->players) || (from == pi)) { return; }
    struct audience_s au = {0};
    au.g = g; au.ph = pi; au.pa = from;
    ui_audience_start(&au);
    au.buf = "They request an audience. Will you receive their envoy now?";
    au.strtbl[0] = "Receive now"; au.strtbl[1] = "Decline"; au.strtbl[2] = NULL;
    int16_t sel = ui_audience_ask4(&au);
    if (sel != 0) { ui_audience_end(&au); diplo_cl_accept(from, pi, 0); return; } /* decline */
    diplo_cl_accept(from, pi, 1); /* accept */
    /* 1oom-mp: keep the alien's audience screen up (no flicker out to the starmap) while the server
       opens the session -- `au` stays active so ui_mp_diplo_wait paints the portrait. Then run the
       session ON THIS SAME `au` (pass it in so run_session does NOT start/end its own -- otherwise it
       fades to black with no fade-in before the proposal arrives, leaving the responder stuck on a
       black screen) and fade out once afterwards. (Consumes SESSION_OPEN here, so the pump's
       sess_open path won't double-enter.) */
    {
        uint8_t sbuf[MP_DIPLO_CL_MSGMAX]; int slen = sizeof(sbuf);
        int id = ui_mp_diplo_wait("Receiving their envoy...", sbuf, &slen);
        if (id == MP_MSG_DIPLO_SESSION_OPEN) { ui_mp_diplo_run_session(g, pi, sbuf, slen, &au); }
        ui_audience_end(&au);
    }
}

/* 1oom-mp: a human's colony ships wait in orbit (the server doesn't auto-colonize);
   at turn start, offer the colonize screen for each one and record the player's yes. */
static void ui_mp_colonize_incoming(struct game_s *g, player_id_t pi)
{
    bool any = false;
    for (planet_id_t pli = 0; pli < g->galaxy_stars; ++pli) {
        if (!game_planet_can_colonize_with_ship(g, pi, pli)) { continue; }
        if (ui_explore(g, pi, pli, false, true)) { /* shows the planet + asks to colonize */
            int slot = mp_colony_ship_for(g, pi, pli); /* find the colony ship while pli is still unowned */
            game_mp_colonize_record(pi, pli);
            /* consume the colony ship NOW (it disappears from orbit) so the player can't move it
               away and strand the colony. The consume rides along in the synced orbit, and the
               server claims the colony from the order without re-requiring the ship. */
            if ((slot >= 0) && (g->eto[pi].orbit[pli].ships[slot] > 0)) {
                g->eto[pi].orbit[pli].ships[slot]--;
            }
            /* optimistic: paint the colony as ours right now so it feels instant (the server's
               authoritative claim follows; a rare contested loss corrects on the next sync). */
            g->planet[pli].owner = pi;
            g->planet[pli].pop = 2;
            any = true;
        }
    }
    if (any) {
        game_update_production(g); /* refresh empire/planet display for the new colony */
        /* a new colony is a refuel point, so it extends fuel range: recompute reachability now,
           else a destination only in range BECAUSE of this fresh colony reads as out of range
           (game_update_within_range runs at turn load, before this optimistic colonize). */
        game_update_within_range(g);
    }
}

/* 1oom-mp: in MP, exploration is resolved on the headless server, so the discovery screen
   (ui_explore) never runs for the human. Replay it client-side at turn start: notify about any
   world newly revealed this turn that we don't own and can't colonize now (colony-ship arrivals
   are already shown by ui_mp_colonize_incoming). A planet's per-player "explored" bit only ever
   goes 0->1, so a diff against what we've already seen detects fresh discoveries. */
static BOOLVEC_DECLARE(s_mp_explored_known, PLANETS_MAX);
static bool s_mp_explored_known_valid = false;

static void ui_mp_discovery_incoming(struct game_s *g, player_id_t pi)
{
    bool first = !s_mp_explored_known_valid; /* turn 1: seed the set, don't pop up initial knowledge */
    for (planet_id_t pli = 0; pli < g->galaxy_stars; ++pli) {
        if (BOOLVEC_IS0(g->planet[pli].explored, pi)) { continue; }
        bool newly = BOOLVEC_IS0(s_mp_explored_known, pli);
        BOOLVEC_SET1(s_mp_explored_known, pli);
        if (first || !newly) { continue; }
        if (g->planet[pli].owner == pi) { continue; }                     /* mine (colonize handled it) */
        if (game_planet_can_colonize_with_ship(g, pi, pli)) { continue; } /* colony ship -> colonize prompt */
        ui_explore(g, pi, pli, false, false);                             /* the discovery notification */
    }
    s_mp_explored_known_valid = true;
}

/* 1oom-mp: the server's null UI can't ask the human which tech to research, so
   project[] stalls at 0. Recompute the per-field choices from synced project/
   investment state and run the selection screen; the chosen project[] is sent back
   via the order stream. */
/* MP: the completed-tech announcement (g->evn.newtech) is neither serialized nor reset by the state
   sync, so we detect completions ourselves by diffing the synced per-field completed-count
   turn-over-turn and rebuilding the newtech entries client-side (reset in ui_game_start). */
/* researchcompleted[] is kept SORTED BY TECH ID (game_tech_get_new), NOT in completion order, so we
   must NOT diff by array index: when a lower-id tech completes it shifts higher-id techs up an index and
   an index-diff would re-announce the field's top tech (the "got Fusion Bombs again" bug). Track the
   actual SET of announced tech ids per field (256-bit mask) and announce by membership instead, so each
   tech is announced exactly once regardless of where the sort places it. */
static uint8_t s_mp_tech_seen[TECH_FIELD_NUM][32];
static bool s_mp_tech_snap_valid = false;

#define MP_TECH_SEEN_GET(f, t) (s_mp_tech_seen[f][(t) >> 3] &  (uint8_t)(1u << ((t) & 7)))
#define MP_TECH_SEEN_SET(f, t) (s_mp_tech_seen[f][(t) >> 3] |= (uint8_t)(1u << ((t) & 7)))

void ui_mp_tech_notify_reset(void) { s_mp_tech_snap_valid = false; }

static void ui_mp_research_select(struct game_s *g, player_id_t pi)
{
    g->evn.newtech[pi].num = 0; /* client newtech isn't synced/reset -- clear to avoid cross-turn buildup */
    game_tech_finish_new(g, pi);
    if (!s_mp_tech_snap_valid) {
        /* first turn: mark every already-completed tech as seen so we don't announce the starting set */
        memset(s_mp_tech_seen, 0, sizeof(s_mp_tech_seen));
        for (tech_field_t f = 0; f < TECH_FIELD_NUM; ++f) {
            int cnt = g->eto[pi].tech.completed[f];
            for (int i = 0; i < cnt; ++i) { MP_TECH_SEEN_SET(f, g->srd[pi].researchcompleted[f][i]); }
        }
        s_mp_tech_snap_valid = true;
    } else {
        for (tech_field_t f = 0; f < TECH_FIELD_NUM; ++f) {
            int cnt = g->eto[pi].tech.completed[f];
            for (int i = 0; i < cnt; ++i) {
                uint8_t tech = g->srd[pi].researchcompleted[f][i];
                int n;
                if (MP_TECH_SEEN_GET(f, tech)) { continue; } /* already announced */
                n = g->evn.newtech[pi].num;
                if (n >= NEWTECH_MAX) { break; }             /* announce the rest next turn */
                MP_TECH_SEEN_SET(f, tech);
                newtech_t *nt = &(g->evn.newtech[pi].d[n]);
                nt->field = f;
                nt->tech = tech;
                nt->source = TECHSOURCE_RESEARCH;
                nt->v06 = 0; nt->stolen_from = PLAYER_NONE; nt->frame = false;
                nt->other1 = PLAYER_NONE; nt->other2 = PLAYER_NONE;
                g->evn.newtech[pi].num = n + 1;
            }
        }
    }
    /* ui_newtech announces what we just completed (rebuilt above) AND lets us pick the next target. */
    bool show = (g->evn.newtech[pi].num > 0);
    for (tech_field_t f = 0; (!show) && (f < TECH_FIELD_NUM); ++f) {
        if (game_tech_can_choose(g, pi, f)) { show = true; }
    }
    if (show) { ui_newtech(g, pi); }
}

ui_turn_action_t ui_game_turn(struct game_s *g, int *load_game_i_ptr, int pi)
{
    int scrapi = -1;
    int opponi = -1;
    /* 1oom-mp teams: a stance proposal lives for one turn. Clear any that didn't resolve so a
       disconnected / AFK / just-eliminated ally can't leave the proposer stuck awaiting a vote that
       will never come (mirrors how an unanswered audience invite lapses at the turn boundary). */
    s_team_prop_enemy = -1; s_team_prop_stance = 0; s_team_prop_await = 0; s_team_prop_result = 0; s_team_prop_announce = -1; s_team_prop_busy = false;
    s_team_vote_from = -1; s_team_enact_from = -1;
    /* 1oom-mp: likewise drop any leftover p2p audience invite/session state at turn start (mirrors the
       server's per-turn session teardown). Otherwise an invite that lapsed unanswered leaves a stale
       "<race> requests an audience" prompt, and a half-finished handshake leaves us thinking we're still
       mid-session -- the client/server disagree and diplomacy appears stuck ("engaged in an audience"). */
    s_mp_diplo_invite_from = -1; s_mp_diplo_invite_to = -1; s_mp_diplo_invite_result = 0; s_mp_diplo_sess_open = false;
    if (g->gaux->local_players > 1) {
        while (ui_switch_1_opts(g, pi)) {
            switch (ui_gameopts(g, load_game_i_ptr)) {
                case GAMEOPTS_DONE:
                    break;
                 case GAMEOPTS_LOAD:
                    return UI_TURN_ACT_LOAD_GAME;
                 case GAMEOPTS_QUIT:
                    return UI_TURN_ACT_QUIT_GAME;
            }
        }
        ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP;
        ui_starmap_set_pos_focus(g, pi);
    }
    ui_data.start_planet_focus_i = g->planet_focus_i[pi];
    if (ui_mp_active) {
        /* center this client's map on ITS OWN home the first turn: ui_game_start centers on the
           first human (player 0), which is wrong for every other player's client. */
        static bool s_mp_centered = false;
        if (!s_mp_centered) { s_mp_centered = true; ui_starmap_set_pos_focus(g, pi); }
    }
    game_turn_start_messages(g, pi);
    /* 1oom-mp: the turn-start replays (news / combat report) end with the palette faded out and draw
       mode 2. Restore the normal game palette before the injected turn-start prompts (research, etc.)
       and the starmap, or they render black. Harmless when no replay ran (re-applies bank 0). */
    if (ui_mp_active) {
        lbxpal_select(0, -1, 0);
        lbxpal_set_update_range(0, 0xff);
        lbxpal_build_colortables();
        ui_draw_finish_mode = 0;
    }
    /* 1oom-mp: these turn-start prompts (research/colonize/discovery, ...) are injected outside the
       normal screen flow, so the cursor is still the starmap's; force the normal pointer for them. */
    if (ui_mp_active) { ui_cursor_setup_area(1, &ui_cursor_area_tbl[0]); }
    if (ui_mp_active) { ui_mp_contact_incoming(g, pi); } /* 1oom-mp: first-contact welcome between humans */
    ui_mp_diplo_incoming(g, pi); /* 1oom-mp: surface any incoming human-to-human proposals */
    /* 1oom-mp: turn-start prompt ORDER. Show newly scouted worlds FIRST, then the tech we
       completed/found this turn (incl. artifacts, right after the discoveries that earned them),
       and only THEN the colonize decision -- so you've seen all the new planets (and any artifact
       tech) before deciding whether to settle a colony ship. (Was colonize -> discovery -> tech,
       which forced the colonize call before the rest of the map was even revealed.) */
    if (ui_mp_active) { ui_mp_discovery_incoming(g, pi); } /* 1oom-mp: notify about newly scouted worlds */
    ui_mp_research_select(g, pi); /* 1oom-mp: announce completed/found tech + let the human pick research targets */
    ui_mp_colonize_incoming(g, pi); /* 1oom-mp: offer the colonize prompt for waiting colony ships -- last, after the map + tech */
    BOOLVEC_CLEAR(ui_data.starmap.select_prio_fleet, FLEET_ENROUTE_MAX);
    BOOLVEC_CLEAR(ui_data.starmap.select_prio_trans, TRANSPORT_MAX);
    while (1) {
        if (ui_mp_turn_active && ui_mp_turn_active()) {
            /* soft-ready MP turn: if the server resolved while we were planning (everyone is
               ready), end the turn so the client runs the resolution + loads the new state.
               Staying ready while editing is fine -- changes are re-submitted automatically. */
            if (ui_mp_turn_poll && ui_mp_turn_poll()) {
                ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP;
                ui_data.news.flag_also = false;
                return UI_TURN_ACT_NEXT_TURN;
            }
        }
        ui_cursor_setup_area(1, &ui_cursor_area_tbl[0]);
        ui_data.starmap.xhold = 0;
        ui_data.starmap.yhold = 0;
        if (g->evn.build_finished_num[pi] > 0) {
            uint8_t pli;
            for (pli = 0; pli < g->galaxy_stars; ++pli) {
                if (g->planet[pli].finished[0] & (~(1 << FINISHED_SHIP))) {
                    break;
                }
            }
            if (pli < g->galaxy_stars) {
                g->planet_focus_i[pi] = pli;
            } else {
                g->evn.build_finished_num[pi] = 0;
                if (ui_extra_enabled) {
                    g->planet_focus_i[pi] = ui_data.start_planet_focus_i;
                }
            }
            if (ui_data.ui_main_loop_action != UI_MAIN_LOOP_PLANET_SHIPS) {
                ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP;
            }
            ui_starmap_set_pos_focus(g, pi);
        }
        ui_data.flag_scrap_for_new_design = false;
        switch (ui_data.ui_main_loop_action) {
            case UI_MAIN_LOOP_STARMAP:
                ui_cursor_setup_area(2, &ui_cursor_area_tbl[3]);
                ui_starmap_do(g, pi);
                break;
            case UI_MAIN_LOOP_RELOC:
                ui_cursor_setup_area(2, &ui_cursor_area_tbl[1]);
                ui_starmap_reloc(g, pi);
                break;
            case UI_MAIN_LOOP_PLANET_SHIPS:
                ui_cursor_setup_area(2, &ui_cursor_area_tbl[3]);
                ui_starmap_ships(g, pi);
                break;
            case UI_MAIN_LOOP_TRANS:
                ui_cursor_setup_area(2, &ui_cursor_area_tbl[1]);
                ui_starmap_trans(g, pi);
                break;
            case UI_MAIN_LOOP_ORBIT_OWN_SEL:
                BOOLVEC_CLEAR(ui_data.starmap.select_prio_fleet, FLEET_ENROUTE_MAX);
                BOOLVEC_CLEAR(ui_data.starmap.select_prio_trans, TRANSPORT_MAX);
                ui_cursor_setup_area(2, &ui_cursor_area_tbl[3]);
                ui_starmap_orbit_own(g, pi);
                break;
            case UI_MAIN_LOOP_ORBIT_EN_SEL:
                BOOLVEC_CLEAR(ui_data.starmap.select_prio_fleet, FLEET_ENROUTE_MAX);
                BOOLVEC_CLEAR(ui_data.starmap.select_prio_trans, TRANSPORT_MAX);
                ui_cursor_setup_area(2, &ui_cursor_area_tbl[3]);
                ui_starmap_orbit_en(g, pi);
                break;
            case UI_MAIN_LOOP_TRANSPORT_SEL:
                if (BOOLVEC_IS1(ui_data.starmap.select_prio_trans, ui_data.starmap.fleet_selected)) {
                    BOOLVEC_CLEAR(ui_data.starmap.select_prio_fleet, FLEET_ENROUTE_MAX);
                    BOOLVEC_CLEAR(ui_data.starmap.select_prio_trans, TRANSPORT_MAX);
                }
                BOOLVEC_SET1(ui_data.starmap.select_prio_trans, ui_data.starmap.fleet_selected);
                ui_cursor_setup_area(2, &ui_cursor_area_tbl[3]);
                ui_starmap_transport(g, pi);
                break;
            case UI_MAIN_LOOP_ENROUTE_SEL:
                if (BOOLVEC_IS1(ui_data.starmap.select_prio_fleet, ui_data.starmap.fleet_selected)) {
                    BOOLVEC_CLEAR(ui_data.starmap.select_prio_fleet, FLEET_ENROUTE_MAX);
                    BOOLVEC_CLEAR(ui_data.starmap.select_prio_trans, TRANSPORT_MAX);
                }
                BOOLVEC_SET1(ui_data.starmap.select_prio_fleet, ui_data.starmap.fleet_selected);
                ui_cursor_setup_area(2, &ui_cursor_area_tbl[3]);
                ui_starmap_enroute(g, pi);
                break;
            case UI_MAIN_LOOP_GAMEOPTS:
                switch (ui_gameopts(g, load_game_i_ptr)) {
                    case GAMEOPTS_DONE:
                        ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP;
                        break;
                    case GAMEOPTS_LOAD:
                        return UI_TURN_ACT_LOAD_GAME;
                    case GAMEOPTS_QUIT:
                        return UI_TURN_ACT_QUIT_GAME;
                }
                break;
            case UI_MAIN_LOOP_DESIGN:
                {
                    struct game_design_s gd;
                    bool ok;
                    int sd_num;
                    sd_num = g->eto[pi].shipdesigns_num;
                    game_design_prepare(g, &gd, pi, &g->current_design[pi]);
                    ok = ui_design(g, &gd, pi);
                    if (ok && (sd_num == NUM_SHIPDESIGNS)) {
                        ui_specs_before(g, pi);
                        ui_data.ui_main_loop_action = UI_MAIN_LOOP_SPECS;
                        ui_data.ui_main_loop_action_next = UI_MAIN_LOOP_SPECS;
                        ui_data.ui_main_loop_action_prev = UI_MAIN_LOOP_DESIGN;
                        ui_data.flag_scrap_for_new_design = true;
                        scrapi = ui_specs(g, pi, false);
                        sd_num = g->eto[pi].shipdesigns_num;
                        ok = (sd_num < NUM_SHIPDESIGNS);
                        if (ok) {
                            game_design_look_fix(g, pi, &gd.sd);
                        }
                    }
                    if (ok) {
                        game_design_add(g, pi, &gd.sd, true);
                    }
                    g->current_design[pi] = gd.sd;
                }
                ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP;
                break;
            case UI_MAIN_LOOP_SPECS:
                scrapi = ui_specs(g, pi, false);
                break;
            case UI_MAIN_LOOP_MUSTSCRAP:
                if (scrapi >= 0) {
                    ui_specs_mustscrap(g, pi, scrapi);
                } else {
                    LOG_DEBUG((3, "%s: invalid scrapi %i on MUSTSCRAP\n", __func__, scrapi));
                    ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP;
                }
                break;
            case UI_MAIN_LOOP_PLANETS:
                ui_planets(g, pi);
                ui_starmap_set_pos_focus(g, pi);
                ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP;
                break;
            case UI_MAIN_LOOP_FLEET:
                scrapi = ui_fleet(g, pi);
                if (ui_data.ui_main_loop_action == UI_MAIN_LOOP_ORBIT_OWN_SEL) {
                    ui_starmap_set_pos_focus(g, pi);
                } else if (ui_data.ui_main_loop_action == UI_MAIN_LOOP_ENROUTE_SEL) {
                    fleet_enroute_t *r;
                    r = &(g->enroute[ui_data.starmap.fleet_selected]);
                    ui_starmap_set_pos(g, r->x, r->y);
                }
                break;
            case UI_MAIN_LOOP_MAP:
                if (ui_gmap(g, pi)) {
                    ui_starmap_set_pos_focus(g, pi);
                }
                ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP;
                break;
            case UI_MAIN_LOOP_RACES:
                opponi = ui_races(g, pi);
                break;
            case UI_MAIN_LOOP_EMPIRESTATUS:
                ui_empirestatus(g, pi);
                ui_data.ui_main_loop_action = UI_MAIN_LOOP_RACES;
                break;
            case UI_MAIN_LOOP_EMPIREREPORT:
                if (IS_PLAYER(g, opponi) && (opponi != pi)) {
                    ui_empirereport(g, pi, opponi);
                } else {
                    LOG_DEBUG((3, "%s: invalid opponi %i for %i on EMPIREREPORT\n", __func__, opponi, pi));
                }
                ui_data.ui_main_loop_action = UI_MAIN_LOOP_RACES;
                break;
            case UI_MAIN_LOOP_AUDIENCE:
                if (IS_PLAYER(g, opponi) && (opponi != pi)) {
                    if (IS_HUMAN(g, opponi)) {
                        ui_mp_diplo_initiate(g, pi, opponi); /* human-to-human: live audience */
                        ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP; /* 1oom-mp: back to the map so the async diplo pump drives the handshake + pulls me into the session */
                        break;
                    } else {
                        game_audience(g, pi, opponi);        /* vs AI: normal audience */
                    }
                } else {
                    LOG_DEBUG((3, "%s: invalid opponi %i for %i on AUDIENCE\n", __func__, opponi, pi));
                }
                ui_data.ui_main_loop_action = UI_MAIN_LOOP_RACES;
                break;
            case UI_MAIN_LOOP_MP_DIPLO:
                ui_mp_diplo_handle(g, pi); /* 1oom-mp: an incoming live audience request */
                ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP;
                break;
            case UI_MAIN_LOOP_STARVIEW:
                ui_starview(g, pi);
                ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP;
                break;
            case UI_MAIN_LOOP_TECH:
                ui_tech(g, pi);
                ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP;
                break;
            case UI_MAIN_LOOP_SCRAP_BASES:
                ui_basescrap(g, pi);
                ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP;
                break;
            case UI_MAIN_LOOP_SPIES_CAUGHT:
                ui_caught(g, pi);
                ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP;
                break;
            case UI_MAIN_LOOP_NEXT_TURN:
                if (ui_mp_turn_active && ui_mp_turn_active()) {
                    /* soft-ready: "Next Turn" locks in (submits) my orders, or unlocks if I'm
                       already ready. Stay on the map; the turn resolves once everyone is ready. */
                    ui_mp_turn_set_ready((ui_mp_turn_is_ready && ui_mp_turn_is_ready()) ? 0 : 1);
                    ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP;
                    break;
                }
                ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP;
                ui_data.news.flag_also = false;
                return UI_TURN_ACT_NEXT_TURN;
            default:
                LOG_DEBUG((0, "BUG: %s: invalid action 0x%x\n", __func__, ui_data.ui_main_loop_action));
                ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP;
                break;
        }
    }
    return UI_TURN_ACT_QUIT_GAME;
}

static bool s_mp_ui_ready = false; /* 1oom-mp: true once the game UI (incl. font color palette) is live */

void ui_game_start(struct game_s *g)
{
    s_mp_ui_ready = true;
    g_mp_team_stance_propose_hook = mp_team_stance_propose; /* 1oom-mp teams: enemy-stance changes route through team consensus */
    ui_mp_tech_notify_reset(); /* MP: re-baseline the completed-tech diff for a fresh game */
    ui_mp_contact_notify_reset(); /* MP: re-baseline first-contact tracking for a fresh game */
    {
        /* 1oom-mp: the small-nebula array (smneb[4*10]) and the bmap viewport graphic (LBXFILE_V11) only
           have art for Small..Huge. Enormous/Galactic have no art of their own, so index the GRAPHICS as
           Huge -- otherwise we read past the array / LBX and draw garbage boxes on the galaxy map. The big
           nebula (line below) is keyed by type only, so it's already fine at any size. */
        int gsz_gfx = (g->galaxy_size > GALAXY_SIZE_HUGE) ? GALAXY_SIZE_HUGE : g->galaxy_size;
        for (int i = 0; i < g->nebula_num; ++i) {
            ui_data.gfx.starmap.nebula[i] = lbxfile_item_get(LBXFILE_STARMAP, 0xf + g->nebula_type[i]);
            ui_data.gfx.starmap.smnebula[i] = ui_data.gfx.starmap.smneb[g->nebula_type[i] + gsz_gfx * 10];
        }
        ui_data.gfx.starmap.bmap = lbxfile_item_get(LBXFILE_V11, 1 + gsz_gfx);
    }

    /* HACK remove visual glitch on load game */
    ui_draw_erase_buf();
    hw_video_draw_buf();
    hw_video_copy_buf();

    lbxpal_select(0, -1, 0);
    lbxpal_build_colortables();
    /* HACK Fix wrong palette after new game via main menu.
       MOO1 goes from orion.exe to starmap.exe in between and the palette is initialized before coming here.
       We only need to set the update flags of this range.
    */
    lbxpal_set_update_range(248, 255);
    ui_palette_set_n();
    ui_draw_finish_mode = 1;
    ui_data.ui_main_loop_action = UI_MAIN_LOOP_STARMAP;
    for (int i = 0; i < g->players; ++i) {
        if (IS_HUMAN(g, i)) {
            ui_starmap_set_pos_focus(g, i);
            break;
        }
    }
    BOOLVEC_CLEAR(ui_data.players_viewing, PLAYER_NUM);
    for (int pli = 0; pli < g->galaxy_stars; ++pli) {
        ui_data.star_frame[pli] = g->planet[pli].frame;
    }
    ui_data.seed = g->seed;
}

void ui_game_end(struct game_s *g)
{
    for (int i = 0; i < NEBULA_MAX; ++i) {
        if (ui_data.gfx.starmap.nebula[i]) {
            lbxfile_item_release(LBXFILE_STARMAP, ui_data.gfx.starmap.nebula[i]);
            ui_data.gfx.starmap.nebula[i] = NULL;
            ui_data.gfx.starmap.smnebula[i] = NULL;
        }
    }
    lbxfile_item_release(LBXFILE_V11, ui_data.gfx.starmap.bmap);
    ui_data.gfx.starmap.bmap = NULL;
}

/* find the next race/banner value (after cur, wrapping) not already taken by another human in the
   lobby. cur==0xff means "none yet" so it starts from 0. count = RACE_NUM or BANNER_NUM. */
static int lobby_next_free(const struct mp_lobby_s *lob, int my_id, int cur, int count, bool is_race)
{
    int base = (cur >= count) ? -1 : cur;
    int n = lob->num_humans + lob->num_ai;
    if (n > MP_MAX_PLAYERS) { n = MP_MAX_PLAYERS; }
    for (int step = 1; step <= count; ++step) {
        int v = (base + step) % count;
        bool taken = false;
        for (int j = 0; j < n; ++j) { /* a race/color taken by any slot (human or chosen-AI) is skipped */
            if (j == my_id) { continue; }
            int other = is_race ? lob->slot[j].race : lob->slot[j].banner;
            if (other == v) { taken = true; break; }
        }
        if (!taken) { return v; }
    }
    return (cur >= count) ? 0 : cur;
}

/* cycle through allowed timer values (0=Off, 30, 45, 60, 90, 120) */
static uint8_t lobby_next_timer(uint8_t cur)
{
    static const uint8_t opts[] = { 0, 30, 45, 60, 90, 120 };
    static const int n = 6;
    for (int i = 0; i < n - 1; ++i) { if (opts[i] == cur) { return opts[i + 1]; } }
    return opts[0]; /* unknown value or last -> wrap to Off */
}

/* 1oom-mp: interactive pre-game lobby. Runs before the game (and its palette) exist, so it sets up
   the palette itself, then loops drawing the shared state and sending the player's edits, pumping
   the network each frame via g_mp_cl_lobby_poll/_set. Returns 0 when the game starts, <0 on quit. */
int ui_mp_lobby_run(int my_id)
{
    int result = 0;
    bool done = false;
    int anim = 0;
    struct mp_lobby_s lob;
    memset(&lob, 0, sizeof(lob));

    /* native MOO1 setup art: the custom-game backdrop, race leader portraits, and the waving banner
       flags (the flags sit at remapped LBX slots, same fixup the single-player setup screen uses). */
    uint8_t *gfx_custom = lbxfile_item_get(LBXFILE_VORTEX, 9);
    uint8_t *gfx_portrait[RACE_NUM];
    uint8_t *gfx_flag[BANNER_NUM];
    for (int i = 0; i < RACE_NUM; ++i) { gfx_portrait[i] = lbxfile_item_get(LBXFILE_VORTEX, 0x10 + i); }
    {
        const banner_t order[BANNER_NUM] = { BANNER_GREEN, BANNER_BLUE, BANNER_RED, BANNER_WHITE, BANNER_YELLOW, BANNER_PURPLE };
        for (int i = 0; i < BANNER_NUM; ++i) { gfx_flag[order[i]] = lbxfile_item_get(LBXFILE_VORTEX, 0xa + i); }
    }
    /* the game's palette isn't up yet in the lobby; use the setup screen's bank so the art renders */
    lbxpal_select(4, -1, 0);
    lbxpal_build_colortables();
    lbxpal_set_update_range(0, 255);
    ui_palette_set_n();
    ui_cursor_setup_area(1, &ui_cursor_area_tbl[0]);
    /* This setup palette's stock font ramps render the small text too dark to read. Find the whitest
       palette entry and force font ramp 0xf to it, so all lobby text (which uses ramp 0xf, with the
       dark ramp-0 outline) renders white on any background. lbxpal_fontcolors = palette + 0x300. */
    {
        const uint8_t *pal = lbxpal_fontcolors - 0x300;
        int best = 0xff, bestsum = -1;
        for (int i = 0; i < 256; ++i) {
            int s = (int)pal[i * 3] + pal[i * 3 + 1] + pal[i * 3 + 2];
            if (s > bestsum) { bestsum = s; best = i; }
        }
        for (int k = 0; k < 16; ++k) { lbxpal_fontcolors[(0xf << 4) + k] = (uint8_t)best; }
    }

    while (!done) {
        int16_t oi;
        int16_t oi_my_race = UIOBJI_INVALID, oi_my_flag = UIOBJI_INVALID, oi_ready = UIOBJI_INVALID, oi_leave = UIOBJI_INVALID, oi_start = UIOBJI_INVALID;
        int16_t oi_galaxy = UIOBJI_INVALID, oi_diff = UIOBJI_INVALID, oi_ai = UIOBJI_INVALID, oi_timer = UIOBJI_INVALID;
        int16_t oi_ai_race[MP_MAX_PLAYERS], oi_team[MP_MAX_PLAYERS];
        for (int k = 0; k < MP_MAX_PLAYERS; ++k) { oi_ai_race[k] = UIOBJI_INVALID; oi_team[k] = UIOBJI_INVALID; }
        char buf[64];
        /* pump the lobby: refresh shared state; leave the loop once the game starts (p>0) or drops (p<0) */
        int p = g_mp_cl_lobby_poll ? g_mp_cl_lobby_poll(&lob) : 1;
        if (p != 0) { result = (p > 0) ? 0 : -1; break; }
        bool is_host = (my_id == 0);
        int total = lob.num_humans + lob.num_ai;
        if (total > MP_MAX_PLAYERS) { total = MP_MAX_PLAYERS; }
        int my_race = lob.slot[my_id].race;
        int my_banner = lob.slot[my_id].banner;
        bool my_ready = (lob.slot[my_id].ready != 0);

        ui_delay_prepare();
        ui_draw_erase_buf();
        lbxgfx_draw_frame(0, 0, gfx_custom, UI_SCREEN_W, ui_scale);
        lbxfont_select(5, 0xf, 0, 0);
        lbxfont_print_str_center(160, 3, "MULTIPLAYER  LOBBY", UI_SCREEN_W, ui_scale);
        uiobj_table_clear();

        /* player-slot grid (same layout as the single-player custom-game screen): portrait | flag | info */
        for (int i = 0; i < total; ++i) {
            int x0 = 4 + (i / 3) * 160;
            int y0 = 20 + (i % 3) * 50;
            bool is_ai = (i >= lob.num_humans);
            bool mine = (!is_ai && (i == my_id));
            int race = lob.slot[i].race; /* AI slots can carry a host-chosen race too */
            int banner = is_ai ? -1 : lob.slot[i].banner;
            int tx = x0 + 86;
            /* portrait */
            ui_draw_box1(x0, y0, x0 + 41, y0 + 35, 0x9b, 0x9b, ui_scale);
            if ((race >= 0) && (race < RACE_NUM)) {
                lbxgfx_draw_frame(x0 + 1, y0 + 1, gfx_portrait[race], UI_SCREEN_W, ui_scale);
            } else {
                ui_draw_filled_rect(x0 + 1, y0 + 1, x0 + 40, y0 + 34, 0, ui_scale);
                lbxfont_select(5, 0xf, 0, 0);
                lbxfont_print_str_center(x0 + 21, y0 + 14, is_ai ? "CPU" : "?", UI_SCREEN_W, ui_scale);
            }
            /* banner flag */
            ui_draw_filled_rect(x0 + 43, y0, x0 + 43 + 41, y0 + 35, 0, ui_scale);
            ui_draw_box1(x0 + 43, y0, x0 + 43 + 41, y0 + 35, 0x9b, 0x9b, ui_scale);
            if ((banner >= 0) && (banner < BANNER_NUM)) {
                lbxgfx_set_new_frame(gfx_flag[banner], anim);
                gfx_aux_draw_frame_to(gfx_flag[banner], &ui_data.aux.screen);
                gfx_aux_draw_frame_from(x0 + 43 + 1, y0 + 1, &ui_data.aux.screen, UI_SCREEN_W, ui_scale);
            }
            /* info text (font 5 like the single-player setup screen; font 2 renders dark in this palette) */
            lbxfont_select(5, 0xf, 0, 0);
            lib_sprintf(buf, sizeof(buf), is_ai ? "Computer" : (mine ? "You (P%d)" : "Player %d"), i + 1);
            lbxfont_print_str_normal(tx, y0 + 2, buf, UI_SCREEN_W, ui_scale);
            {   /* team tag (top-right of the slot); click to cycle -- own slot, or host sets anyone */
                char tbuf[8];
                if (lob.slot[i].team == 0) { lib_sprintf(tbuf, sizeof(tbuf), "%s", "FFA"); }
                else { lib_sprintf(tbuf, sizeof(tbuf), "T%d", lob.slot[i].team); }
                lbxfont_print_str_right(tx + 66, y0 + 2, tbuf, UI_SCREEN_W, ui_scale);
                if (mine || is_host) { oi_team[i] = uiobj_add_mousearea(tx + 42, y0 + 1, tx + 68, y0 + 11, MOO_KEY_UNKNOWN); }
            }
            lbxfont_select(5, 0xf, 0, 0);
            lbxfont_print_str_normal(tx, y0 + 13, ((race >= 0) && (race < RACE_NUM)) ? game_str_tbl_race[race] : (is_ai ? (is_host ? "click to pick" : "random") : "choosing"), UI_SCREEN_W, ui_scale);
            if (!is_ai) {
                lbxfont_select(5, 0xf, 0, 0);
                lbxfont_print_str_normal(tx, y0 + 24, ((lob.open_lobby != 0) && (i == 0)) ? "HOST" : (lob.slot[i].ready ? "READY" : "not ready"), UI_SCREEN_W, ui_scale);
            }
            if (mine) { /* my slot: click portrait to cycle race, flag to cycle color */
                oi_my_race = uiobj_add_mousearea(x0, y0, x0 + 41, y0 + 35, MOO_KEY_UNKNOWN);
                oi_my_flag = uiobj_add_mousearea(x0 + 43, y0, x0 + 43 + 41, y0 + 35, MOO_KEY_UNKNOWN);
            } else if (is_ai && is_host) { /* the host clicks an AI portrait to cycle that AI's race */
                oi_ai_race[i] = uiobj_add_mousearea(x0, y0, x0 + 41, y0 + 35, MOO_KEY_UNKNOWN);
            }
        }

        /* settings strip: galaxy size, AI difficulty, AI count, turn timer.
           Galaxy and Diff keep their original widths (Diff needs 120px for "Diff: Impossible").
           The original AI box (x=232..318) is split into AI (x=232..271) and Timer (x=275..318).
           Host clicks a box to cycle its value; "T:" prefix keeps Timer label short enough. */
        {
            char gv[40], dv[40], av[24], tv[24];
            lib_sprintf(gv, sizeof(gv), "Galaxy: %s", (lob.galaxy_size < GALAXY_SIZE_NUM) ? game_str_tbl_gsize[lob.galaxy_size] : "?");
            lib_sprintf(dv, sizeof(dv), "Diff: %s", (lob.difficulty < DIFFICULTY_NUM) ? game_str_tbl_diffic[lob.difficulty] : "?");
            lib_sprintf(av, sizeof(av), "AI: %d", lob.num_ai);
            if (lob.turn_timer_secs == 0) { lib_sprintf(tv, sizeof(tv), "T: Off"); }
            else { lib_sprintf(tv, sizeof(tv), "T: %ds", lob.turn_timer_secs); }
            ui_draw_filled_rect(  3, 159, 103, 171, 0, ui_scale); ui_draw_box1(  2, 158, 104, 172, 0x9b, 0x9b, ui_scale);
            ui_draw_filled_rect(109, 159, 227, 171, 0, ui_scale); ui_draw_box1(108, 158, 228, 172, 0x9b, 0x9b, ui_scale);
            ui_draw_filled_rect(233, 159, 270, 171, 0, ui_scale); ui_draw_box1(232, 158, 271, 172, 0x9b, 0x9b, ui_scale);
            ui_draw_filled_rect(276, 159, 317, 171, 0, ui_scale); ui_draw_box1(275, 158, 318, 172, 0x9b, 0x9b, ui_scale);
            lbxfont_select(2, 0xf, 0, 0);
            lbxfont_print_str_center( 53, 161, gv, UI_SCREEN_W, ui_scale);
            lbxfont_print_str_center(168, 161, dv, UI_SCREEN_W, ui_scale);
            lbxfont_print_str_center(252, 161, av, UI_SCREEN_W, ui_scale);
            lbxfont_print_str_center(297, 161, tv, UI_SCREEN_W, ui_scale);
            if (is_host) {
                oi_galaxy = uiobj_add_mousearea(  2, 158, 104, 172, MOO_KEY_UNKNOWN);
                oi_diff   = uiobj_add_mousearea(108, 158, 228, 172, MOO_KEY_UNKNOWN);
                oi_ai     = uiobj_add_mousearea(232, 158, 271, 172, MOO_KEY_UNKNOWN);
                oi_timer  = uiobj_add_mousearea(275, 158, 318, 172, MOO_KEY_UNKNOWN);
            }
        }

        /* LEAVE (bottom-left) + READY (bottom-right), over the backdrop's button corners */
        lbxfont_select(5, 0xf, 0, 0);
        lbxfont_print_str_center(40, 181, "LEAVE", UI_SCREEN_W, ui_scale);
        oi_leave = uiobj_add_mousearea(4, 174, 78, 197, MOO_KEY_ESCAPE);
        lbxfont_select(5, 0xf, 0, 0);
        if ((lob.open_lobby != 0) && is_host) {
            /* open lobby: the host begins the game (no Ready toggle of their own); START lights up once
               every other connected human has readied. */
            bool others_ready = true;
            for (int j = 1; j < lob.num_humans; ++j) { if (lob.slot[j].connected && !lob.slot[j].ready) { others_ready = false; break; } }
            bool can_start = (my_race < RACE_NUM) && others_ready;
            lbxfont_print_str_center(280, 181, "START", UI_SCREEN_W, ui_scale);
            if (can_start) { oi_start = uiobj_add_mousearea(242, 174, 316, 197, MOO_KEY_SPACE); }
            lbxfont_print_str_center(160, 189, (my_race >= RACE_NUM) ? "pick your race and color" : (can_start ? "everyone's ready -- click START to begin" : "waiting for players to ready up..."), UI_SCREEN_W, ui_scale);
        } else {
            lbxfont_print_str_center(280, 181, my_ready ? "WAITING" : "READY", UI_SCREEN_W, ui_scale);
            if (my_race < RACE_NUM) { oi_ready = uiobj_add_mousearea(242, 174, 316, 197, MOO_KEY_SPACE); }
            lbxfont_print_str_center(160, 189, my_ready ? ((lob.open_lobby != 0) ? "waiting for the host to start..." : "waiting for all players...") : "pick your race and color, then ready", UI_SCREEN_W, ui_scale);
        }

        uiobj_finish_frame();
        oi = uiobj_handle_input_cond();
        if ((oi == UIOBJI_ESC) || (oi_leave != UIOBJI_INVALID && oi == oi_leave)) { result = -1; done = true; }
        else if (!g_mp_cl_lobby_set) { /* no transport (shouldn't happen) */ }
        else if (oi_my_race != UIOBJI_INVALID && oi == oi_my_race) { g_mp_cl_lobby_set(MP_LOBBY_F_RACE, lobby_next_free(&lob, my_id, my_race, RACE_NUM, true)); }
        else if (oi_my_flag != UIOBJI_INVALID && oi == oi_my_flag) { g_mp_cl_lobby_set(MP_LOBBY_F_BANNER, lobby_next_free(&lob, my_id, my_banner, BANNER_NUM, false)); }
        else if (oi_ready != UIOBJI_INVALID && oi == oi_ready) { g_mp_cl_lobby_set(MP_LOBBY_F_READY, my_ready ? 0 : 1); }
        else if (oi_start != UIOBJI_INVALID && oi == oi_start) { g_mp_cl_lobby_set(MP_LOBBY_F_START, 1); }
        else if (is_host && oi_galaxy != UIOBJI_INVALID && oi == oi_galaxy) { g_mp_cl_lobby_set(MP_LOBBY_F_GALAXY, (lob.galaxy_size + 1) % GALAXY_SIZE_SEL_NUM); } /* 1oom-mp: hide Enormous/Galactic (larger-maps branch) */
        else if (is_host && oi_diff != UIOBJI_INVALID && oi == oi_diff) { g_mp_cl_lobby_set(MP_LOBBY_F_DIFFICULTY, (lob.difficulty + 1) % DIFFICULTY_NUM); }
        else if (is_host && oi_ai != UIOBJI_INVALID && oi == oi_ai) { int mx = MP_MAX_PLAYERS - lob.num_humans; g_mp_cl_lobby_set(MP_LOBBY_F_NUM_AI, (lob.num_ai >= mx) ? 0 : lob.num_ai + 1); }
        else if (is_host && oi_timer != UIOBJI_INVALID && oi == oi_timer) { g_mp_cl_lobby_set(MP_LOBBY_F_TIMER, lobby_next_timer(lob.turn_timer_secs)); }
        else { /* per-slot clicks: AI portrait cycles its race (host); team tag cycles team (own/host) */
            for (int j = 0; j < total; ++j) {
                if (is_host && (oi_ai_race[j] != UIOBJI_INVALID) && (oi == oi_ai_race[j])) {
                    g_mp_cl_lobby_set(MP_LOBBY_F_AI_RACE, (j << 4) | lobby_next_free(&lob, j, lob.slot[j].race, RACE_NUM, true));
                    break;
                }
                if ((oi_team[j] != UIOBJI_INVALID) && (oi == oi_team[j])) {
                    g_mp_cl_lobby_set(MP_LOBBY_F_TEAM, (j << 4) | ((lob.slot[j].team + 1) % (MP_MAX_PLAYERS + 1)));
                    break;
                }
            }
        }
        if (++anim >= 10) { anim = 0; }
        ui_delay_ticks_or_click(2);
    }
    uiobj_table_clear();
    for (int i = 0; i < RACE_NUM; ++i) { lbxfile_item_release(LBXFILE_VORTEX, gfx_portrait[i]); }
    for (int i = 0; i < BANNER_NUM; ++i) { lbxfile_item_release(LBXFILE_VORTEX, gfx_flag[i]); }
    lbxfile_item_release(LBXFILE_VORTEX, gfx_custom);
    return result;
}

/* 1oom-mp: one frame of a "waiting for players" screen. Called repeatedly by the
   MP client while it blocks on the network — pumps SDL events (so the window stays
   alive / movable) and shows a status line instead of a frozen image. */
/* 1oom-mp: one frame of the OPT-IN prompt shown while a teammate is in combat. Mirrors the diplo
   wait's input frame (proven to work during the resolution wait, same as ui_mp_battle_spectate).
   Returns 1 the frame the watch key (W) is pressed, so the caller loads the spectator arena. */
int ui_mp_battle_watch_prompt(void)
{
    int16_t oi_w, oi;
    if (!s_mp_ui_ready) { hw_event_handle(); return 0; }
    ui_delay_prepare();
    ui_draw_erase_buf();
    lbxfont_select(2, 0xd, 0, 0);
    lbxfont_print_str_center(160, 84, "A teammate has entered combat.", UI_SCREEN_W, ui_scale);
    /* 1oom-mp: the call to action was color 6 -- too dim to read on the black prompt, so players saw
       the headline but not the instruction. Use the same bright color as the headline. */
    lbxfont_select(2, 0xd, 0, 0);
    lbxfont_print_str_center(160, 98, "Press  W  to watch the battle", UI_SCREEN_W, ui_scale);
    uiobj_table_clear();
    oi_w = uiobj_add_inputkey(MOO_KEY_w);
    uiobj_finish_frame();
    oi = uiobj_handle_input_cond();
    ui_delay_ticks_or_click(2);
    return (oi == oi_w) ? 1 : 0;
}

void ui_mp_wait(int reason)
{
    hw_event_handle();          /* poll input + ~10ms delay; keeps the window responsive */
    /* Before the game UI is live (lobby/initial sync) the font color palette isn't
       loaded yet, so lbxfont_select would crash — just keep the window alive. */
    if (!s_mp_ui_ready) { return; }
    if (reason == 2 /* MP_WAIT_BATTLE */) { return; } /* in a battle: just pumped events, keep the arena */
    const char *msg;
    if (reason == 4 /* MP_WAIT_COUNCIL */) { msg = "The Galactic Council is in session..."; }
    else if (reason == 3 /* MP_WAIT_COMBAT */) { msg = "Waiting for another player to finish combat..."; }
    else if (reason) { msg = "Waiting for other players to finish their turn..."; }
    else { msg = "Waiting for players..."; }
    ui_draw_erase_buf();
    lbxfont_select(2, 0xd, 0, 0);
    lbxfont_print_str_center(160, 92, msg, UI_SCREEN_W, ui_scale);
    /* Present via ui_draw_finish (not hw_video_draw_buf): the low-level draw skips ui_palette_set_n,
       so the banner was being shown against a stale/unset palette -> invisible (a black screen). The
       working wait screens (council show, diplomacy modal) all present through this path. */
    uiobj_table_clear();
    ui_draw_finish_mode = 0;
    ui_draw_finish();
}
