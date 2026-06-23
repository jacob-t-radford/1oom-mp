#include "config.h"

#include <stdio.h>
#include <string.h>

#include "uistarmap_common.h"
#include "comp.h"
#include "hw.h"
#include "game.h"
#include "game_aux.h"
#include "game_fleet.h"
#include "game_misc.h"
#include "game_num.h"
#include "game_str.h"
#include "game_tech.h"
#include "kbd.h"
#include "mouse.h"
#include "lbxgfx.h"
#include "lbxfont.h"
#include "lib.h"
#include "log.h"
#include "rnd.h"
#include "types.h"
#include "ui.h"
#include "uicursor.h"
#include "uidraw.h"
#include "uidefs.h"
#include "uidelay.h"
#include "uiobj.h"
#include "uisound.h"
#include "uistarmap.h"

/* -------------------------------------------------------------------------- */

const uint8_t colortbl_textbox[5] = { 0x18, 0x17, 0x16, 0x15, 0x14 };
const uint8_t colortbl_line_red[5] = { 0x44, 0x43, 0x42, 0x41, 0x40 };
const uint8_t colortbl_line_reloc[5] = { 0x14, 0x15, 0x16, 0x17, 0x18 };
const uint8_t colortbl_line_green[5] = { 0xb0, 0xb1, 0xb2, 0xb3, 0xb4 };

static planet_id_t ui_starmap_cursor_on_star(const struct starmap_data_s *d, int16_t oi)
{
    if (oi == 0) {
        return PLANET_NONE;
    }
    for (int i = 0; i < d->g->galaxy_stars; ++i) {
        if (oi == d->oi_tbl_stars[i]) {
            return i;
        }
    }
    return PLANET_NONE;
}

/* -------------------------------------------------------------------------- */

static void ui_starmap_draw_planetinfo_do(const struct game_s *g, player_id_t api, planet_id_t planet_i, bool explored, bool show_plus, bool draw_name)
{
    const planet_t *p = &g->planet[planet_i];
    if (explored || (ui_extra_enabled && g->gaux->flag_cheat_stars)) {
        if (draw_name) {
            lbxfont_select_set_12_4(4, 0xf, 0, 0);
            lbxfont_print_str_center(269, 10, p->name, UI_SCREEN_W, ui_scale);
        }
        /* stars in nebulas get a purple instead of a red frame */
        if (ui_extra_enabled && p->battlebg==0) {
            ui_draw_box1(228, 25, 309, 52, 0xd3, 0xd3, ui_scale);
        }
        if (p->type == PLANET_TYPE_NOT_HABITABLE) {
            lbxfont_select(0, 0xe, 0, 0);
            lbxfont_print_str_center(269, 32, game_str_sm_nohabit, UI_SCREEN_W, ui_scale);
            lbxfont_print_str_center(269, 41, game_str_sm__planets, UI_SCREEN_W, ui_scale);
        } else {
            const char *str = NULL;
            lbxgfx_draw_frame(229, 27, ui_data.gfx.planets.planet[p->infogfx], UI_SCREEN_W, ui_scale);
            lbxfont_select(0, 0xd, 0, 0);
            lbxfont_print_str_right(305, 28, game_str_tbl_sm_pltype[p->type], UI_SCREEN_W, ui_scale);
            if (g->evn.have_plague && (g->evn.plague_planet_i == planet_i)) {
                str = game_str_sm_plague;
            } else if (g->evn.have_nova && (g->evn.nova_planet_i == planet_i)) {
                str = game_str_sm_nova;
            } else if (g->evn.have_comet && (g->evn.comet_planet_i == planet_i)) {
                str = game_str_sm_comet;
            } else if (g->evn.have_pirates && (g->evn.pirates_planet_i == planet_i)) {
                str = game_str_sm_pirates;
            } else if (p->unrest == PLANET_UNREST_REBELLION) {
                str = game_str_sm_rebellion;
            } else if (p->unrest == PLANET_UNREST_UNREST) {
                str = game_str_sm_unrest;
            } else if (g->evn.have_accident && (g->evn.accident_planet_i == planet_i)) {
                str = game_str_sm_accident;
            }
            if (str) {
                lbxfont_select(5, 5, 0, 0);
                lbxfont_print_str_right(305, 40, str, UI_SCREEN_W, ui_scale);
            } else {
                int x, xp, max_pop = p->max_pop3;
                lbxfont_select(0, 1, 0, 0);
                if (p->special == PLANET_SPECIAL_NORMAL) {
                    str = game_str_tbl_sm_pgrowth[p->growth];
                } else {
                    str = game_str_tbl_sm_pspecial[p->special];
                }
                lbxfont_print_str_right(305, 36, str, UI_SCREEN_W, ui_scale);
                lbxfont_select(2, 0xe, 0, 0);
                if (show_plus && ((p->owner == api) || (ui_extra_enabled && g->gaux->flag_cheat_stars)) && game_planet_can_terraform(g, p, p->owner, ui_extra_enabled)) {
                    lbxfont_print_str_normal(289, 44, "+", UI_SCREEN_W, ui_scale);
                    x = 287;
                    xp = x - 22;
                } else {
                    x = 291;
                    xp = x - 23;
                }
                lbxfont_print_str_normal(xp, 45, game_str_sm_pop, UI_SCREEN_W, ui_scale);
                lbxfont_print_str_normal(295, 45, game_str_sm_max, UI_SCREEN_W, ui_scale);
                if ((!show_plus) || (g->eto[api].race != RACE_SILICOID)) {
                    max_pop -= p->waste;
                }
                SETMAX(max_pop, 10);
                lbxfont_print_num_right(x, 45, max_pop, UI_SCREEN_W, ui_scale);
            }
        }
    } else {
        lbxfont_select(5, 0xe, 0, 0);
        lbxfont_print_str_center(269, show_plus ? 27 : 35, game_str_sm_unexplored, UI_SCREEN_W, ui_scale);
    }
}

static void ui_starmap_draw_range_parsec(struct starmap_data_s *d, int y)
{
    const struct game_s *g = d->g;
    int dist = game_get_min_dist(g, d->api, g->planet_focus_i[d->api]);
    char buf[64];
    lib_sprintf(buf, sizeof(buf), "%s %i %s", game_str_sm_range, dist, (dist == 1) ? game_str_sm_parsec : game_str_sm_parsecs);
    lbxfont_select_set_12_4(0, 4, 0, 0);
    lbxfont_print_str_center(269, y, buf, UI_SCREEN_W, ui_scale);
}

static void ui_starmap_draw_sliders_and_prod(struct starmap_data_s *d)
{
    const struct game_s *g = d->g;
    int focus_i = g->planet_focus_i[d->api];
    const planet_t *p = &g->planet[focus_i];
    int x = 311;
    char buf[64];
    /* 1oom-mp: for a teammate's world, draw their full CURRENT (live-relayed) planet -- sliders AND
       economy -- so the read-only panel's production figures match exactly what they see. */
    if (p->owner != d->api) {
        const planet_t *tp = ui_mp_team_plan_planet(focus_i);
        if (tp) { p = tp; }
    }

    for (planet_slider_i_t i = PLANET_SLIDER_SHIP; i < PLANET_SLIDER_NUM; ++i) {
        ui_draw_filled_rect(227, 81 + 11 * i, 244, 90 + 11 * i, p->slider_lock[i] ? 0x22 : 0, ui_scale);
    }

    lbxgfx_draw_frame(224, 5, ui_data.gfx.starmap.yourplnt, UI_SCREEN_W, ui_scale);
    lbxfont_select(2, 0xd, 0xe, 0);
    lib_sprintf(buf, sizeof(buf), "%i \x02(%i)\x01", p->prod_after_maint, p->total_prod);
    lbxfont_print_str_right(x, 72, buf, UI_SCREEN_W, ui_scale);
    lbxfont_select(2, 0xd, 0, 0);
    lbxfont_print_num_right(265, 61, p->pop, UI_SCREEN_W, ui_scale);
    lbxfont_print_num_right(x, 61, p->missile_bases, UI_SCREEN_W, ui_scale);

    for (planet_slider_i_t i = PLANET_SLIDER_SHIP; i < PLANET_SLIDER_NUM; ++i) {
        ui_draw_filled_rect(253, 84 + 11 * i, 278, 84 + 11 * i + 3, 0x2f, ui_scale);
        if (p->slider[i] != 0) {
            ui_draw_slider(253, 84 + 11 * i + 1, p->slider[i], 4, -1, p->slider_lock[i] ? 0x22 : 0x73, ui_scale);
        }
    }

    lbxfont_select(2, 0xa, 0, 0);
    if (p->buildship == BUILDSHIP_STARGATE) {
        ui_draw_filled_rect(229, 141, 274, 166, 0, ui_scale);
        lbxgfx_draw_frame(229, 141, ui_data.gfx.starmap.stargate, UI_SCREEN_W, ui_scale);
        lbxfont_print_str_center(251, 169, game_str_sm_stargate, UI_SCREEN_W, ui_scale);
    } else if ((p->owner < g->players) && (p->buildship < NUM_SHIPDESIGNS)) { /* 1oom-mp: a teammate's relayed planet snapshot can carry a garbage owner/buildship -> guard the srd[]/ships[] index (mirrors the guard on the sibling path) */
        const shipdesign_t *sd = &g->srd[p->owner].design[p->buildship];
        uint8_t *gfx = ui_data.gfx.ships[sd->look];
        lbxgfx_set_frame_0(gfx);
        lbxgfx_draw_frame(236, 142, gfx, UI_SCREEN_W, ui_scale);
        lbxfont_print_str_center(252, 169, sd->name, UI_SCREEN_W, ui_scale);
    }

    lbxfont_select(2, 6, 0, 0);
    {
        int v;
        v = game_planet_get_slider_text(g, p, PLANET_SLIDER_SHIP, buf, sizeof(buf));
        lbxfont_print_str_right(x, 83, buf, UI_SCREEN_W, ui_scale);
        lbxfont_select(0, 0xd, 0, 0);
        if (v >= 0) {
            lbxfont_print_num_right(271, 160, v, UI_SCREEN_W, ui_scale);
        }
    }
    lbxfont_select(2, 6, 0, 0);
    game_planet_get_slider_text(g, p, PLANET_SLIDER_DEF, buf, sizeof(buf));
    lbxfont_print_str_right(x, 94, buf, UI_SCREEN_W, ui_scale);
    game_planet_get_slider_text(g, p, PLANET_SLIDER_IND, buf, sizeof(buf));
    lbxfont_print_str_right(x, 105, buf, UI_SCREEN_W, ui_scale);
    {
        int v;
        v = game_planet_get_slider_text_eco(g, p, ui_extra_enabled, buf, sizeof(buf));
        lbxfont_print_str_right(x, 116, buf, UI_SCREEN_W, ui_scale);
        if (v >= 0) {
            if (ui_extra_enabled) {
                if (v < 100) {
                    lib_sprintf(buf, sizeof(buf), "%i.%i", v / 10, v % 10); /* "+0.X" does not fit the box */
                } else {
                    lib_sprintf(buf, sizeof(buf), "+%i", v / 10);
                }
            } else {
                lib_sprintf(buf, sizeof(buf), "+%i", v);
            }
            lbxfont_print_str_right(297, 116, buf, UI_SCREEN_W, ui_scale);
        }
    }
    {
        int v;
        v = game_planet_get_slider_text(g, p, PLANET_SLIDER_TECH, buf, sizeof(buf));
        if (v > 9999) {
            ui_draw_filled_rect(288, 127, 312, 132, 7, ui_scale);
        } else {
            x -= 9;
        }
        lbxfont_print_str_right(x, 127, buf, UI_SCREEN_W, ui_scale);
    }
}

static void ui_starmap_draw_textbox_finished(const struct game_s *g, player_id_t api, int pi)
{
    const planet_t *p = &g->planet[pi];
    char *buf = ui_data.strbuf;
    planet_finished_t i;
    for (i = 0; i < FINISHED_NUM; ++i) {
        if (BOOLVEC_IS1(p->finished, i)) {
            break;
        }
    }
    game_planet_get_finished_text(g, p, i, buf, UI_STRBUF_SIZE);
    int y = ui_extra_enabled ? 45 : 54;
    ui_draw_textbox_2str("", buf, y, ui_scale);
    ui_draw_textbox_2str("", game_str_sm_planratio, 110, ui_scale);
}

/* 1oom-mp: map an OFFSCREEN render coord (origin 0; the galaxy draws there at "*2", i.e. rc = 2*galaxy)
   to the on-screen window pixel where sm_offscreen_blit() actually places it: winl + (rc/2 - origin)*F,
   origin = starmap.x + frac/16, F = sm_zoom_f16/16. Used to put click areas under the blitted glyphs. */
static int sm_blit_win_x(int rc)
{
    return 6 * ui_scale + (int)((((long)rc * 8 - (long)ui_data.starmap.x * 16 - sm_frac_x16) * sm_zoom_f16) / 256);
}

static int sm_blit_win_y(int rc)
{
    return 6 * ui_scale + (int)((((long)rc * 8 - (long)ui_data.starmap.y * 16 - sm_frac_y16) * sm_zoom_f16) / 256);
}

/* Add a clickable area for a galaxy element whose offscreen glyph box is render-coord
   [2*gx+ox0 .. 2*gx+ox1] x [2*gy+oy0 .. 2*gy+oy1], placed where the downscale-blit draws it on screen.
   Returns UIOBJI_INVALID if the box falls entirely outside the map window. */
int16_t sm_add_galaxy_mousearea(int gx, int gy, int ox0, int oy0, int ox1, int oy1)
{
    int winl = 6 * ui_scale, wint = 6 * ui_scale, winr = 222 * ui_scale - 1, winb = 178 * ui_scale - 1;
    int x0 = sm_blit_win_x(2 * gx + ox0);
    int y0 = sm_blit_win_y(2 * gy + oy0);
    int x1 = sm_blit_win_x(2 * gx + ox1);
    int y1 = sm_blit_win_y(2 * gy + oy1);
    if ((x1 < winl) || (x0 > winr) || (y1 < wint) || (y0 > winb)) {
        return UIOBJI_INVALID;
    }
    SETMAX(x0, winl);
    SETMAX(y0, wint);
    SETMIN(x1, winr);
    SETMIN(y1, winb);
    /* uiobj stores render coords and multiplies by ui_scale at hit-test; pass screen px / ui_scale. */
    return uiobj_add_mousearea(x0 / ui_scale, y0 / ui_scale, x1 / ui_scale, y1 / ui_scale, MOO_KEY_UNKNOWN);
}

/* 1oom-mp: the sub-mode overlays (selector boxes, route lines, fleet sprites) are drawn AFTER the galaxy
   is blitted, straight to the framebuffer, so they must land where the blit drew the galaxy -- not via the
   old sm_span transform. These give the render coord (for a scale=starmap_scale draw) that sits on the
   blitted element at galaxy coord g + render offset off. Drop-in replacement for
   "sm_span_x(g - ui_data.starmap.x) + off"; relative tweaks like (x0 + 6) keep working because +k render
   units == +k*starmap_scale screen px == the blit's +k offset (starmap_scale == F/2). */
int ui_starmap_ovl_x(int galaxy_x, int off)
{
    return sm_blit_win_x(2 * galaxy_x + off) / starmap_scale;
}

int ui_starmap_ovl_y(int galaxy_y, int off)
{
    return sm_blit_win_y(2 * galaxy_y + off) / starmap_scale;
}

/* -------------------------------------------------------------------------- */

static void ui_starmap_add_oi_enroute(struct starmap_data_s *d, bool want_prio)
{
    const struct game_s *g = d->g;
    for (int i = 0; i < g->enroute_num; ++i) {
        const fleet_enroute_t *r = &(g->enroute[i]);
        if (BOOLVEC_IS1(r->visible, d->api) && (BOOLVEC_IS1(ui_data.starmap.select_prio_fleet, i) == want_prio)) {
            d->oi_tbl_enroute[i] = sm_add_galaxy_mousearea(r->x, r->y, 8, 8, 16, 12);
        }
    }
    for (int i = 0; i < g->transport_num; ++i) {
        const transport_t *r = &(g->transport[i]);
        if (BOOLVEC_IS1(r->visible, d->api) && (BOOLVEC_IS1(ui_data.starmap.select_prio_trans, i) == want_prio)) {
            d->oi_tbl_transport[i] = sm_add_galaxy_mousearea(r->x, r->y, 8, 8, 16, 12);
        }
    }
}

static int ui_starmap_scrollkey_accel(int zh)
{
    int v = zh;
    if (zh < 0) {
        v = -v;
    }
    v = 1 + (v / 4);
    if (v > 8) v = 8;
    v *= ui_sm_scroll_speed / 2 + 1;
    if (zh < 0) {
        v = -v;
    }
    return v;
}

/* -------------------------------------------------------------------------- */

void ui_starmap_draw_basic(struct starmap_data_s *d)
{
    const struct game_s *g = d->g;
    const planet_t *p = &g->planet[g->planet_focus_i[d->api]];

    ui_starmap_draw_starmap(d);
    ui_starmap_draw_button_text(d, true);
    ui_draw_filled_rect(224, 5, 314, 178, 0, ui_scale);
    if (BOOLVEC_IS0(p->explored, d->api) && !(ui_extra_enabled && g->gaux->flag_cheat_stars)) {
        lbxgfx_draw_frame(224, 5, ui_data.gfx.starmap.unexplor, UI_SCREEN_W, ui_scale);
        lbxfont_select_set_12_4(5, 1, 0, 0);
        lbxfont_print_str_split(232, 74, 76, game_str_tbl_sm_stinfo[p->star_type], 2, UI_SCREEN_W, UI_SCREEN_H, ui_scale);
        ui_starmap_draw_range_parsec(d, 165);
    } else {
        player_id_t owner = p->owner;
        int pi = g->planet_focus_i[d->api];
        bool teammate_live = (ui_mp_team_plan_planet(pi) != NULL); /* 1oom-mp: a teammate's live plan -> show their read-only sliders, not the enemy-colony picture */
        if (BOOLVEC_IS0(p->within_srange, d->api) && !(ui_extra_enabled && g->gaux->flag_cheat_stars) && ((owner == PLAYER_NONE) || BOOLVEC_IS0(g->eto[d->api].contact, owner))) {
            owner = g->seen[d->api][pi].owner;
        }
        if (owner == PLAYER_NONE) {
            /* 1oom-mp: a teammate that just settled here this turn owns it in their LIVE overlay but
               not yet in my synced state -> adopt their ownership so the panel shows THEIR colony now,
               instead of "No Colony" until the turn resolves. */
            const planet_t *tpc = ui_mp_team_plan_planet(pi);
            if (tpc && (tpc->owner < g->players)) { owner = tpc->owner; }
        }
        if (owner == PLAYER_NONE) {
            lbxgfx_draw_frame(224, 5, ui_data.gfx.starmap.no_colny, UI_SCREEN_W, ui_scale);
            ui_data.gfx.colonies.current = ui_data.gfx.colonies.d[p->type * 2];
            lbxgfx_draw_frame(227, 73, ui_data.gfx.colonies.current, UI_SCREEN_W, ui_scale);
            ui_draw_box1(227, 73, 310, 174, 0, 0, ui_scale);
            ui_starmap_draw_range_parsec(d, 80);
        } else if ((((owner != d->api) && !(ui_extra_enabled && g->gaux->flag_cheat_stars)) && !teammate_live) || (p->unrest == PLANET_UNREST_REBELLION)) {
            char buf[64];
            int pop, bases, range_y;
            lbxgfx_draw_frame(224, 5, ui_data.gfx.starmap.en_colny, UI_SCREEN_W, ui_scale);
            ui_data.gfx.colonies.current = ui_data.gfx.colonies.d[p->type * 2 + 1];
            lbxgfx_draw_frame(227, 73, ui_data.gfx.colonies.current, UI_SCREEN_W, ui_scale);
            ui_draw_box1(227, 73, 310, 174, 0, 0, ui_scale);
            lib_sprintf(buf, sizeof(buf), "%s %s", game_str_tbl_race[g->eto[owner].race], game_str_sm_colony);
            if (BOOLVEC_IS1(p->within_srange, d->api)) {
                lbxfont_select_set_12_4(5, tbl_banner_fontparam[g->eto[owner].banner], 0, 0);
                lbxfont_print_str_center(270, 84, buf, UI_SCREEN_W, ui_scale);
                pop = p->pop;
                bases = p->missile_bases;
                range_y = 95;
            } else {
                lbxfont_select_set_12_4(0, tbl_banner_fontparam[g->eto[owner].banner], 0, 0);
                lbxfont_print_str_center(269, 75, game_str_sm_lastrep, UI_SCREEN_W, ui_scale);
                lbxfont_print_str_center(268, 83, buf, UI_SCREEN_W, ui_scale);    /* TODO combine with above */
                pop = g->seen[d->api][pi].pop;
                bases = g->seen[d->api][pi].bases;
                range_y = 92;
            }
            lbxfont_select(0, 0xd, 0, 0);
            lbxfont_print_num_right(265, 61, pop, UI_SCREEN_W, ui_scale);
            lbxfont_print_num_right(310, 61, bases, UI_SCREEN_W, ui_scale);
            ui_starmap_draw_range_parsec(d, range_y);
        } else {
            ui_starmap_draw_sliders_and_prod(d);
            if (p->owner == d->api) { /* build / reloc / transport buttons are for your OWN world only -- a teammate's view is read-only */
                lbxgfx_set_frame_0(ui_data.gfx.starmap.col_butt_ship);
                lbxgfx_set_frame_0(ui_data.gfx.starmap.col_butt_reloc);
                if (p->buildship == BUILDSHIP_STARGATE) {
                    lbxgfx_set_frame(ui_data.gfx.starmap.col_butt_reloc, 1);
                }
                if ((g->evn.have_plague == 0) || (g->evn.plague_planet_i != g->planet_focus_i[d->api])) {
                    lbxgfx_set_frame_0(ui_data.gfx.starmap.col_butt_trans);
                } else {
                    lbxgfx_set_frame(ui_data.gfx.starmap.col_butt_trans, 1);
                }
                lbxgfx_draw_frame(282, 140, ui_data.gfx.starmap.col_butt_ship, UI_SCREEN_W, ui_scale);
                lbxgfx_draw_frame(282, 152, ui_data.gfx.starmap.col_butt_reloc, UI_SCREEN_W, ui_scale);
                lbxgfx_draw_frame(282, 164, ui_data.gfx.starmap.col_butt_trans, UI_SCREEN_W, ui_scale);
            }
        }
    }
    ui_starmap_draw_planetinfo(g, d->api, g->planet_focus_i[d->api], d->planet_draw_name);
    if (g->evn.build_finished_num[d->api]) {
        ui_starmap_draw_textbox_finished(g, d->api, g->planet_focus_i[d->api]);
    }
}

/* -------------------------------------------------------------------------- */

/* 1oom-mp: smooth continuous starmap zoom via an offscreen render.
   The galaxy is drawn once into s_sm_off at a FIXED reference scale (REF_SCALE == the maximum on-screen
   zoom, so the window blit only ever DOWNSCALES -> always crisp, never blocky), then sm_offscreen_blit()
   nearest-neighbour downscales it into the map window at the real continuous zoom (f16) + fractional pan
   origin. Drawing everything at one uniform scale is what removes the per-element "shift" at zoom steps:
   stars, nebulae, names, grid and lines all scale together because they are a single image. */
#define REF_SCALE (2 * ui_scale)        /* offscreen render scale == max on-screen zoom (downscale-only) */

static uint8_t *s_sm_off = NULL;        /* offscreen 8-bit galaxy buffer (whole galaxy at REF_SCALE) */
static int s_sm_off_w = 0;
static int s_sm_off_h = 0;

static void sm_offscreen_ensure(const struct game_s *g)
{
    int w = (g->galaxy_maxx + 4) * 2 * REF_SCALE;   /* galaxy G -> 2*G*REF_SCALE px, +4 units glyph margin */
    int h = (g->galaxy_maxy + 4) * 2 * REF_SCALE;
    if (s_sm_off && (s_sm_off_w == w) && (s_sm_off_h == h)) { return; }
    if (s_sm_off) { lib_free(s_sm_off); s_sm_off = NULL; }
    s_sm_off_w = w;
    s_sm_off_h = h;
    s_sm_off = lib_malloc((size_t)w * h);
}

/* Downscale-blit the visible region of the offscreen into the map window at origin (ox + ofx/16,
   oy + ofy/16) galaxy units and zoom f16 (screen px per galaxy unit, 4-bit fixed point, F = f16/16). */
static void sm_offscreen_blit(int ox, int oy, int ofx, int ofy, int f16)
{
    int SCL = 2 * REF_SCALE;                              /* offscreen px per galaxy unit */
    uint8_t *fb = hw_video_get_buf();                    /* real framebuffer (override is NULL again here) */
    int winl = 6 * ui_scale, wint = 6 * ui_scale;
    int winr = 222 * ui_scale, winb = 178 * ui_scale;    /* exclusive */
    /* source position in 1/16 offscreen px, computed PER PIXEL (no truncated-step accumulation, which
       drifts a few px across the window and jitters as the zoom changes). src16 = real_origin*SCL*16 +
       wo*SCL*256/f16 (wo = window offset; SCL*256/f16 = (SCL/F)*16 offscreen px per window px). */
    long ox_off = (long)(ox * 16 + ofx) * SCL;
    long oy_off = (long)(oy * 16 + ofy) * SCL;
    if (f16 < 1) { f16 = 1; }
    for (int wy = wint; wy < winb; ++wy) {
        long sy16 = oy_off + ((long)(wy - wint) * SCL * 256) / f16;
        int sy = (int)(sy16 / 16);
        uint8_t *srow, *drow;
        if ((sy < 0) || (sy >= s_sm_off_h)) { continue; }
        srow = s_sm_off + (size_t)sy * s_sm_off_w;
        drow = fb + (size_t)wy * ui_screen_w;
        for (int wx = winl; wx < winr; ++wx) {
            long sx16 = ox_off + ((long)(wx - winl) * SCL * 256) / f16;
            int sx = (int)(sx16 / 16);
            uint8_t c;
            if ((sx < 0) || (sx >= s_sm_off_w)) { continue; }
            c = srow[sx];
            if (c) { drow[wx] = c; }    /* 0 = space-black; skip so the framebuffer clear shows through */
        }
    }
}

/* -------------------------------------------------------------------------- */

/* 1oom-mp: draw the chat overlay (last few lines, "Race: message" in the sender's banner colour) top-left
   over the map, on the framebuffer after the galaxy blit. Click the [-]/[+] (oi_chat_min) to minimize. */
static void ui_starmap_draw_chat(const struct game_s *g)
{
    int n = ui_mp_chat_count();
    int y;
    return;   /* 1oom-mp: chat overlay DISABLED for now (remove this line to re-enable) */
    if (n <= 0) { return; }
    if (ui_mp_chat_minimized()) {
        ui_draw_filled_rect(6, 6, 22, 15, 0, ui_scale);
        lbxfont_select(2, 0xf, 0, 0);
        lbxfont_print_str_normal(9, 8, "[+]", UI_SCREEN_W, ui_scale);
        return;
    }
    ui_draw_filled_rect(6, 6, 158, 9 + 8 * (n + 1), 0, ui_scale);
    lbxfont_select(2, 0xf, 0, 0);
    lbxfont_print_str_normal(9, 8, "[-]", UI_SCREEN_W, ui_scale);
    y = 8 + 8;
    for (int i = 0; i < n; ++i) {
        int sender = 0;
        const char *text = NULL;
        char line[160];
        if (!ui_mp_chat_get(i, &sender, &text)) { break; }
        if ((sender < 0) || (sender >= g->players)) { y += 8; continue; }
        lib_sprintf(line, sizeof(line), "%s: %s", game_str_tbl_race[g->eto[sender].race], text ? text : "");
        lbxfont_select(2, tbl_banner_fontparam[g->eto[sender].banner], 0, 0);
        lbxfont_print_str_normal(9, y, line, UI_SCREEN_W, ui_scale);
        y += 8;
    }
}

void ui_starmap_draw_starmap(struct starmap_data_s *d)
{
    const struct game_s *g = d->g;
    int x, y, tx, ty;
    char str[16];
    int stl_x0, stl_y0, stl_x1, stl_y1;     /* text/name clip = offscreen bounds during the offscreen render */
    STARMAP_LIM_INIT();
    {
        int v, step;
        step = STARMAP_SCROLLSTEP;
        v = ui_data.starmap.x2;
        x = ui_data.starmap.x;
        if (v != x) {
            if (step == 0) {
                x = v;
            } else {
                if (v < x) {
                    x -= STARMAP_SCROLLSTEP;
                    SETMAX(x, v);
                } else {
                    x += STARMAP_SCROLLSTEP;
                    SETMIN(x, v);
                }
            }
            ui_data.starmap.x = x;
        }
        v = ui_data.starmap.y2;
        y = ui_data.starmap.y;
        if (v != y) {
            if (step == 0) {
                y = v;
            } else {
                if (v < y) {
                    y -= STARMAP_SCROLLSTEP;
                    SETMAX(y, v);
                } else {
                    y += STARMAP_SCROLLSTEP;
                    SETMIN(y, v);
                }
            }
            ui_data.starmap.y = y;
        }
    }
    if (++d->anim_delay >= STARMAP_ANIM_DELAY) {
        d->anim_delay = 0;
    }
    ui_draw_filled_rect(6, 6, 222 - 1, 178 - 1, 0, ui_scale);
    lbxgfx_draw_frame(0, 0, ui_data.gfx.starmap.mainview, UI_SCREEN_W, ui_scale);
    uiobj_set_limits(STARMAP_LIMITS);
    sm_offscreen_ensure(g);
    if (s_sm_off) {
        /* save real view params */
        int rx = ui_data.starmap.x, ry = ui_data.starmap.y;
        int rf16 = sm_zoom_f16, rsc = starmap_scale, rfx = sm_frac_x16, rfy = sm_frac_y16, rsw = ui_screen_w;
        /* save the clip limits so they can be restored after the offscreen render */
        int rlx0 = slx0, rly0 = sly0, rlx1 = slx1, rly1 = sly1;
        /* render the galaxy at the fixed reference into the offscreen, origin 0 */
        memset(s_sm_off, 0, (size_t)s_sm_off_w * s_sm_off_h);
        hw_video_set_draw_buf(s_sm_off);
        ui_screen_w = s_sm_off_w;               /* pitch for the offscreen */
        ui_data.starmap.x = 0; ui_data.starmap.y = 0;
        sm_frac_x16 = 0; sm_frac_y16 = 0;
        starmap_scale = REF_SCALE;
        sm_zoom_f16 = 32 * REF_SCALE;           /* keeps sm_span()==classic "*2", so G -> 2*G*REF_SCALE px */
        /* clip the galaxy-content block to the whole offscreen. STARMAP_LIMITS (slx0..sly1) are render
           coords (the draw primitives multiply them by starmap_scale), so divide the offscreen px size
           by starmap_scale to get the render-coord bound. */
        slx0 = 0; sly0 = 0;
        slx1 = s_sm_off_w / starmap_scale - 1;
        sly1 = s_sm_off_h / starmap_scale - 1;
        x = 0; y = 0;                           /* the galaxy-content block draws relative to x,y */
        /* text/name clip: lbxfont_*_limit takes the limits in SCREEN coords (it divides by scale
           internally), so use the offscreen px bounds. STARMAP_TEXT_LIMITS (window coords) would clip
           most names away during the offscreen render -- the name-print calls below use these instead. */
        stl_x0 = 0; stl_y0 = 0; stl_x1 = s_sm_off_w - 1; stl_y1 = s_sm_off_h - 1;
    {
        uint8_t *gfx1, *gfx2;
        int x0, y0, x1, y1, lx, ly;
        /* 1oom-mp: rendered into the offscreen at the fixed REF_SCALE with origin 0, so the parallax is
           baked into the buffer; it pans/zooms with everything else via the downscale-blit. Original
           1oom origin-based parallax (layer 1 = -origin/4, layer 2 = -origin/2), tiled to cover the
           whole offscreen. (origin x,y are 0 here, so the offsets are just +6.) */
        gfx1 = ui_fixbugs_enabled ? ui_data.gfx.starmap.starbak2 : ui_data.gfx.starmap.starback;
        gfx2 = ui_fixbugs_enabled ? ui_data.gfx.starmap.starback : ui_data.gfx.starmap.starbak2;
        x0 = (-x / 4) + 6;
        y0 = (-y / 4) + 6;
        x1 = ((-x + 1) / 2) + 6;
        y1 = ((-y + 1) / 2) + 6;
        lx = slx1;
        ly = sly1;
        for (int yb = y0; yb < ly; yb += 200) {
            for (int xb = x0; xb < lx; xb += 320) {
                lbxgfx_draw_frame_offs(xb, yb, gfx1, STARMAP_LIMITS, UI_SCREEN_W, starmap_scale);
            }
        }
        for (int yb = y1; yb < ly; yb += 200) {
            for (int xb = x1; xb < lx; xb += 320) {
                lbxgfx_draw_frame_offs(xb, yb, gfx2, STARMAP_LIMITS, UI_SCREEN_W, starmap_scale);
            }
        }
    }
    for (int i = 0; i < g->nebula_num; ++i) {
        int tx, ty;
        tx = sm_span_x(g->nebula_x[i] - x) + 7;
        ty = sm_span_y(g->nebula_y[i] - y) + 7;
        lbxgfx_draw_frame_offs(tx, ty, ui_data.gfx.starmap.nebula[i], STARMAP_LIMITS, UI_SCREEN_W, starmap_scale);
    }
    if (ui_data.starmap.flag_show_grid) {
        int x0, y0, x1, y1;
        for (y0 = 10; y0 < g->galaxy_maxy; y0 += 50) {
            int ty;
            x0 = sm_span_x(-x) + 6;
            x1 = sm_span_x(g->galaxy_maxx - x) + 6;
            ty = sm_span_y(y0 - y) + 6;
            ui_draw_line_limit(x0, ty, x1, ty, 4, starmap_scale);
        }
        for (x0 = 10; x0 < g->galaxy_maxx; x0 += 50) {
            int tx;
            y0 = sm_span_y(-y) + 6;
            y1 = sm_span_y(g->galaxy_maxy - y) + 6;
            tx = sm_span_x(x0 - x) + 6;
            ui_draw_line_limit(tx, y0, tx, y1, 4, starmap_scale);
        }
    }
    for (int pi = 0; pi < g->galaxy_stars; ++pi) {
        const planet_t *p = &g->planet[pi];
        if ((p->owner == d->api) && (p->reloc != pi)) {
            const planet_t *p2 = &g->planet[p->reloc];
            int x0, y0, x1, y1;
            x0 = sm_span_x(p->x - x) + 14;
            x1 = sm_span_x(p2->x - x) + 14;
            y0 = sm_span_y(p->y - y) + 14;
            y1 = sm_span_y(p2->y - y) + 14;
            ui_draw_line_limit_ctbl(x0, y0, x1, y1, colortbl_line_reloc, 5, ui_data.starmap.line_anim_phase, starmap_scale);
        }
    }
    for (int pi = 0; pi < g->galaxy_stars; ++pi) {
        const planet_t *p = &g->planet[pi];
        uint8_t *gfx = ui_data.gfx.starmap.stars[p->star_type + p->look];
        uint8_t anim_frame = ui_data.star_frame[pi];
        bool explored = BOOLVEC_IS1(p->explored, d->api);
        bool visible = BOOLVEC_IS1(p->within_srange, d->api);
        bool done = false;
        lbxgfx_set_new_frame(gfx, (anim_frame < 4) ? anim_frame : 0);
        gfx_aux_draw_frame_to(gfx, &ui_data.starmap.star_aux);
        if (p->look > 0) {
            tx = sm_span_x(p->x - x) + 8;
            ty = sm_span_y(p->y - y) + 9;
        } else {
            tx = sm_span_x(p->x - x) + 11;
            ty = sm_span_y(p->y - y) + 11;
        }
        gfx_aux_draw_frame_from_limit(tx, ty, &ui_data.starmap.star_aux, STARMAP_LIMITS, UI_SCREEN_W, starmap_scale);
        { /* 1oom-mp live teammate visibility: a teammate is about to colonize this world this turn --
             mark it in their banner colour so you see their settling plans before the turn resolves. */
            int cz = ui_mp_team_plan_colonizer(pi);
            if (cz >= 0) {
                ui_draw_filled_rect(tx - 1, ty - 6, tx + 3, ty - 2, tbl_banner_color[g->eto[cz].banner], starmap_scale);
                ui_draw_box1(tx - 2, ty - 7, tx + 4, ty - 1, 0, 0, starmap_scale);
            }
        }
        { /* 1oom-mp teams: a teammate flagged this world for the team ("look here") -> ring the star in their colour. */
            int pgr = ui_mp_team_plan_ping_at(pi);
            if (pgr >= 0) {
                uint8_t bc = tbl_banner_color[g->eto[pgr].banner];
                ui_draw_box1(tx - 4, ty - 4, tx + 6, ty + 6, 0, 0, starmap_scale);
                ui_draw_box1(tx - 3, ty - 3, tx + 5, ty + 5, bc, bc, starmap_scale);
            }
        }
        if (d->anim_delay == 0) {
            if (anim_frame == 4) {
                anim_frame = rnd_0_nm1(50, &ui_data.seed);
            } else {
                anim_frame = (anim_frame + 1) % 50;
            }
            ui_data.star_frame[pi] = anim_frame;
        }
        tx = sm_span_x(p->x - x) + 14;
        ty = sm_span_y(p->y - y) + 22;
        /* 1oom-mp: a teammate settling this world this turn owns it in the live overlay but not yet in
           my synced state -> render its name as THEIR colony (banner colour, and actually shown) now,
           instead of a bare unowned star until the turn resolves. */
        player_id_t eff_owner = p->owner;
        if (eff_owner == PLAYER_NONE) { int cz = ui_mp_team_plan_colonizer(pi); if (cz >= 0) { eff_owner = (player_id_t)cz; } }
        if (eff_owner == PLAYER_NONE) {
            lbxfont_select(2, 7, 0, 0);
        } else if (visible || (pi == g->evn.planet_orion_i)) {
            lbxfont_select(2, tbl_banner_fontparam[g->eto[eff_owner].banner], 0, 0);
        } else if (BOOLVEC_IS1(g->eto[d->api].contact, eff_owner) || (p->within_frange[d->api] == 1)) {
            lbxfont_select(2, 0, 0, 0);
            lbxfont_set_color0(tbl_banner_color[g->eto[eff_owner].banner]);
        } else {
            lbxfont_select(2, 7, 0, 0);
        }
        if (ui_extra_enabled && explored && ui_data.starmap.star_text_type != UI_SM_STAR_TEXT_NAME) {
            if (p->type == PLANET_TYPE_NOT_HABITABLE) {
                lbxfont_print_str_center_limit(tx, ty, "0/0", stl_x0, stl_y0, stl_x1, stl_y1, UI_SCREEN_W, starmap_scale);
                done = true;
            } else if (visible && ui_data.starmap.star_text_type == UI_SM_STAR_TEXT_POPULATION) {
                int max_pop = p->max_pop3;
                if (g->eto[d->api].race != RACE_SILICOID) {
                    max_pop -= p->waste;
                }
                if (game_planet_can_terraform(g, p, d->api, true)) {
                    lib_sprintf(str, sizeof(str), "%i/%i+", p->pop, max_pop);
                } else {
                    lib_sprintf(str, sizeof(str), "%i/%i", p->pop, max_pop);
                }
                lbxfont_print_str_center_limit(tx, ty, str, stl_x0, stl_y0, stl_x1, stl_y1, UI_SCREEN_W, starmap_scale);
                done = true;
            } else if (ui_data.starmap.star_text_type == UI_SM_STAR_TEXT_ENVIRONMENT) {
                lib_sprintf(str, sizeof(str), "%s", game_str_tbl_sm_pltype[p->type]);
                lbxfont_print_str_center_limit(tx, ty, str, stl_x0, stl_y0, stl_x1, stl_y1, UI_SCREEN_W, starmap_scale);
                done = true;
            } else if (ui_data.starmap.star_text_type == UI_SM_STAR_TEXT_SPECIAL) {
                if (p->special == PLANET_SPECIAL_NORMAL) {
                    lib_sprintf(str, sizeof(str), "%s", "Normal");
                } else {
                    lib_sprintf(str, sizeof(str), "%s", game_str_tbl_sm_pspecial[p->special]);
                }
                lbxfont_print_str_center_limit(tx, ty, str, stl_x0, stl_y0, stl_x1, stl_y1, UI_SCREEN_W, starmap_scale);
                done = true;
            }
        }
        if (ui_data.starmap.star_text_type == UI_SM_STAR_TEXT_DISTANCE) {
            if (p->owner != d->api) {
                lib_sprintf(str, sizeof(str), "%d parsecs", game_get_min_dist(g, d->api, pi));
                lbxfont_print_str_center_limit(tx, ty, str, stl_x0, stl_y0, stl_x1, stl_y1, UI_SCREEN_W, starmap_scale);
                done = true;
            }
        }
        if (!done) {
            bool do_print = visible
                        || (pi == g->evn.planet_orion_i)
                        || (BOOLVEC_IS1(g->eto[d->api].contact, p->owner))
                        || (p->within_frange[d->api] == 1)
                        || (eff_owner != p->owner); /* 1oom-mp: a teammate's just-settled colony (overlay owner, synced still NONE) -> show its name via shared team vision even out of my scanner range */
            if (eff_owner != PLAYER_NONE && do_print) {
                lbxfont_print_str_center_limit(tx, ty, p->name, stl_x0, stl_y0, stl_x1, stl_y1, UI_SCREEN_W, starmap_scale);
            }
        }
    }
    if (1
      && (ui_data.ui_main_loop_action != UI_MAIN_LOOP_RELOC)
      && (ui_data.ui_main_loop_action != UI_MAIN_LOOP_TRANS)
      && (ui_data.ui_main_loop_action != UI_MAIN_LOOP_ORBIT_OWN_SEL)
      && (ui_data.ui_main_loop_action != UI_MAIN_LOOP_TRANSPORT_SEL)
      && (ui_data.ui_main_loop_action != UI_MAIN_LOOP_ENROUTE_SEL)
      && (ui_data.ui_main_loop_action != UI_MAIN_LOOP_ORBIT_EN_SEL)
    ) {
        const planet_t *p = &g->planet[g->planet_focus_i[d->api]];
        tx = sm_span_x(p->x - x) + 8;
        ty = sm_span_y(p->y - y) + 8;
        lbxgfx_draw_frame_offs_delay(tx, ty, !d->anim_delay, ui_data.gfx.starmap.planbord, STARMAP_LIMITS, UI_SCREEN_W, starmap_scale);
    }
    if (d->anim_delay == 0) {
        if (--ui_data.starmap.line_anim_phase < 0) {
            ui_data.starmap.line_anim_phase = 4;
        }
    }
    for (int i = 0; i < g->enroute_num; ++i) {
        const fleet_enroute_t *r = &g->enroute[i];
        if (ui_mp_team_plan_active(r->owner)) { continue; } /* teammate's live overlay replaces their stale committed fleets */
        if (BOOLVEC_IS1(r->visible, d->api)) {
            uint8_t *gfx = ui_data.gfx.starmap.smalship[g->eto[r->owner].banner];
            const planet_t *p = &g->planet[r->dest];
            tx = sm_span_x(r->x - x) + 8;
            ty = sm_span_y(r->y - y) + 8;
            if (p->x < r->x) {
                lbxgfx_set_new_frame(gfx, 1);
            } else {
                lbxgfx_set_frame_0(gfx);
            }
            p = &g->planet[g->planet_focus_i[d->api]];
            if (g->eto[d->api].have_ia_scanner && (p->owner == d->api) && (r->owner != d->api) && (r->dest == g->planet_focus_i[d->api])) {
                ui_draw_line_limit_ctbl(tx + 5, ty + 2, sm_span_x(p->x - x) + 14, sm_span_y(p->y - y) + 14, colortbl_line_red, 5, ui_data.starmap.line_anim_phase, starmap_scale);
            }
            if (ui_extra_enabled && (r->owner == d->api) && (r->dest < g->galaxy_stars)) {
                /* 1oom-mp: show MY OWN fleets'/transports' destinations too (like teammates'), always
                   (not just the focused planet), each line drawn in MY banner colour. */
                const planet_t *pdst = &g->planet[r->dest];
                ui_draw_line_limit(tx + 5, ty + 2, sm_span_x(pdst->x - x) + 14, sm_span_y(pdst->y - y) + 14, tbl_banner_color[g->eto[r->owner].banner], starmap_scale);
            }
            lbxgfx_draw_frame_offs(tx, ty, gfx, STARMAP_LIMITS, UI_SCREEN_W, starmap_scale);
        }
    }
    /* 1oom-mp live teammate visibility: overlay teammates' in-progress planned fleets (streamed each
       frame during planning) so you see where they're sending ships before the turn resolves. Drawn
       in the owner's banner colour with a green planned-route line to the destination. */
    {
        int tpn = ui_mp_team_plan_fleet_total();
        for (int k = 0; k < tpn; ++k) {
            int fo = 0, fx = 0, fy = 0, fdest = 0;
            const planet_t *pd; uint8_t *gfx;
            if (!ui_mp_team_plan_fleet_get(k, &fo, &fx, &fy, &fdest)) { break; }
            if ((fdest < 0) || (fdest >= g->galaxy_stars)) { continue; }
            pd = &g->planet[fdest];
            gfx = ui_data.gfx.starmap.smalship[g->eto[fo].banner];
            tx = sm_span_x(fx - x) + 8;
            ty = sm_span_y(fy - y) + 8;
            if (pd->x < fx) { lbxgfx_set_new_frame(gfx, 1); } else { lbxgfx_set_frame_0(gfx); }
            ui_draw_line_limit(tx + 5, ty + 2, sm_span_x(pd->x - x) + 14, sm_span_y(pd->y - y) + 14, tbl_banner_color[g->eto[fo].banner], starmap_scale); /* 1oom-mp: teammate fleet route in THEIR banner colour */
            lbxgfx_draw_frame_offs(tx, ty, gfx, STARMAP_LIMITS, UI_SCREEN_W, starmap_scale);
        }
    }
    for (int i = 0; i < g->transport_num; ++i) {
        const transport_t *r = &g->transport[i];
        if (BOOLVEC_IS1(r->visible, d->api)) {
            uint8_t *gfx = ui_data.gfx.starmap.smaltran[g->eto[r->owner].banner];
            const planet_t *p = &g->planet[r->dest];
            tx = sm_span_x(r->x - x) + 8;
            ty = sm_span_y(r->y - y) + 8;
            if (p->x < r->x) {
                lbxgfx_set_new_frame(gfx, 1);
            } else {
                lbxgfx_set_frame_0(gfx);
            }
            p = &g->planet[g->planet_focus_i[d->api]];
            if (g->eto[d->api].have_ia_scanner && (p->owner == d->api) && (r->owner != d->api) && (r->dest == g->planet_focus_i[d->api])) {
                ui_draw_line_limit_ctbl(tx + 5, ty + 2, sm_span_x(p->x - x) + 14, sm_span_y(p->y - y) + 14, colortbl_line_red, 5, ui_data.starmap.line_anim_phase, starmap_scale);
            }
            if (ui_extra_enabled && (r->owner == d->api) && (r->dest < g->galaxy_stars)) {
                /* 1oom-mp: show MY OWN fleets'/transports' destinations too (like teammates'), always
                   (not just the focused planet), each line drawn in MY banner colour. */
                const planet_t *pdst = &g->planet[r->dest];
                ui_draw_line_limit(tx + 5, ty + 2, sm_span_x(pdst->x - x) + 14, sm_span_y(pdst->y - y) + 14, tbl_banner_color[g->eto[r->owner].banner], starmap_scale);
            }
            lbxgfx_draw_frame_offs(tx, ty, gfx, STARMAP_LIMITS, UI_SCREEN_W, starmap_scale);
        }
    }
    if (g->evn.crystal.exists && (g->evn.crystal.killer == PLAYER_NONE)) {
        tx = sm_span_x(g->evn.crystal.x - x) + 8;
        ty = sm_span_y(g->evn.crystal.y - y) + 8;
        lbxgfx_draw_frame_offs(tx, ty, ui_data.gfx.planets.smonster, STARMAP_LIMITS, UI_SCREEN_W, starmap_scale);
        lbxfont_select(2, 8, 0, 0);
        lbxfont_print_str_center_limit(tx + 2, ty + 5, game_str_sm_crystal, stl_x0, stl_y0, stl_x1, stl_y1, UI_SCREEN_W, starmap_scale);
    }
    if ((g->evn.amoeba.exists != 0) && (g->evn.amoeba.killer == PLAYER_NONE)) {
        tx = sm_span_x(g->evn.amoeba.x - x) + 8;
        ty = sm_span_y(g->evn.amoeba.y - y) + 8;
        lbxgfx_draw_frame_offs(tx, ty, ui_data.gfx.planets.smonster, STARMAP_LIMITS, UI_SCREEN_W, starmap_scale);
        lbxfont_select(2, 8, 0, 0);
        lbxfont_print_str_center_limit(tx + 2, ty + 5, game_str_sm_amoeba, stl_x0, stl_y0, stl_x1, stl_y1, UI_SCREEN_W, starmap_scale);
    }
    for (int pi = 0; pi < g->galaxy_stars; ++pi) {
        const planet_t *p = &g->planet[pi];
        if (BOOLVEC_IS1(p->within_srange, d->api) || BOOLVEC_IS1(g->eto[d->api].orbit[pi].visible, d->api)) {
            player_id_t tblorbit[PLAYER_NUM];
            player_id_t num;
            num = 0;
            for (player_id_t i = PLAYER_0; i < g->players; ++i) {
                const empiretechorbit_t *e = &g->eto[i];
                if (BOOLVEC_IS0(p->within_srange, d->api) && (i != d->api)) {
                    continue;
                }
                if (ui_mp_team_plan_active(i)) {
                    /* teammate's overlay is live: use their relayed orbit so a ship they just
                       sent en-route this turn stops showing in orbit too (no phantom duplicate). */
                    if (ui_mp_team_plan_orbit_has(i, pi)) { tblorbit[num++] = i; }
                } else {
                    for (int j = 0; j < e->shipdesigns_num; ++j) {
                        if (e->orbit[pi].ships[j]) {
                            tblorbit[num++] = i;
                            break;
                        }
                    }
                }
            }
            tx = sm_span_x(p->x - x) + 25;
            if (p->have_stargate) {
                lbxgfx_draw_frame_offs(tx, sm_span_y(p->y - y) + 7, ui_data.gfx.starmap.stargate2, STARMAP_LIMITS, UI_SCREEN_W, starmap_scale);
            }
            ++tx;
            for (player_id_t i = PLAYER_0; i < num; ++i) {
                player_id_t i2 = tblorbit[i];
                uint8_t *gfx = ui_data.gfx.starmap.smalship[g->eto[i2].banner];
                lbxgfx_set_frame_0(gfx);
                ty = sm_span_y(p->y - y) + i * 6 + 8;
                lbxgfx_draw_frame_offs(tx, ty, gfx, STARMAP_LIMITS, UI_SCREEN_W, starmap_scale);
            }
            /* 1oom-mp teams: stacked-fleet indicator. When more than one fleet (typically your own +
               a teammate's, or two teammates') sits at the same star, the small badges overlap and
               are easy to miss -- draw a little count chit to the right of the stack, in the top
               owner's banner colour, so it's visibly stacked and you know to click each row. */
            if (num > 1) {
                uint8_t bc = tbl_banner_color[g->eto[tblorbit[0]].banner];
                int bx = tx + 7;
                int by = sm_span_y(p->y - y) + 8;
                char nb[8];
                ui_draw_box1(bx - 1, by - 1, bx + 5, by + 5, 0, 0, starmap_scale);
                ui_draw_filled_rect(bx, by, bx + 4, by + 4, bc, starmap_scale);
                lib_sprintf(nb, sizeof(nb), "%d", num);
                lbxfont_select(2, 0, 0, 0);
                lbxfont_set_color0(0);
                lbxfont_print_str_center_limit(bx + 2, by, nb, stl_x0, stl_y0, stl_x1, stl_y1, UI_SCREEN_W, starmap_scale);
            }
        }
    }
        /* === end of the galaxy-content draw; it ran into the offscreen at the reference scale === */
        hw_video_set_draw_buf(NULL);
        /* restore real view params + clip limits */
        ui_data.starmap.x = rx; ui_data.starmap.y = ry;
        sm_zoom_f16 = rf16; starmap_scale = rsc; sm_frac_x16 = rfx; sm_frac_y16 = rfy; ui_screen_w = rsw;
        slx0 = rlx0; sly0 = rly0; slx1 = rlx1; sly1 = rly1;
        /* blit offscreen -> framebuffer map window at the real origin + zoom */
        sm_offscreen_blit(rx, ry, rfx, rfy, rf16);
    }
    ui_starmap_draw_chat(g);   /* 1oom-mp: chat overlay on top of the galaxy (all starmap modes) */
}

void ui_starmap_draw_button_text(struct starmap_data_s *d, bool highlight)
{
    lbxfont_select_set_12_4(5, (highlight && (d->bottom_highlight == 0)) ? 0 : 2, 0, 0);
    lbxfont_print_str_normal(10, 184, game_str_sm_game, UI_SCREEN_W, ui_scale);
    lbxfont_select_set_12_4(5, (highlight && (d->bottom_highlight == 1)) ? 0 : 2, 0, 0);
    lbxfont_print_str_normal(44, 184, game_str_sm_design, UI_SCREEN_W, ui_scale);
    lbxfont_select_set_12_4(5, (highlight && (d->bottom_highlight == 2)) ? 0 : 2, 0, 0);
    lbxfont_print_str_normal(83, 184, game_str_sm_fleet, UI_SCREEN_W, ui_scale);
    lbxfont_select_set_12_4(5, (highlight && (d->bottom_highlight == 3)) ? 0 : 2, 0, 0);
    lbxfont_print_str_normal(119, 184, game_str_sm_map, UI_SCREEN_W, ui_scale);
    lbxfont_select_set_12_4(5, (highlight && (d->bottom_highlight == 4)) ? 0 : 2, 0, 0);
    lbxfont_print_str_normal(147, 184, game_str_sm_races, UI_SCREEN_W, ui_scale);
    lbxfont_select_set_12_4(5, (highlight && (d->bottom_highlight == 5)) ? 0 : 2, 0, 0);
    lbxfont_print_str_normal(184, 184, game_str_sm_planets, UI_SCREEN_W, ui_scale);
    lbxfont_select_set_12_4(5, (highlight && (d->bottom_highlight == 6)) ? 0 : 2, 0, 0);
    lbxfont_print_str_normal(230, 184, game_str_sm_tech, UI_SCREEN_W, ui_scale);
    lbxfont_select_set_12_4(5, (highlight && (d->bottom_highlight == 7)) ? 0 : 2, 0, 0);
    lbxfont_print_str_normal(263, 184, game_str_sm_next_turn, UI_SCREEN_W, ui_scale);
}

void ui_starmap_clamp_xy(const struct game_s *g, int *x, int *y)
{
    ui_starmap_zoom_sync_scale();   /* 1oom-mp: ensure sm_zoom_f16 init'd & starmap_scale derived */
    if (!ui_sm_expanded_scroll) {
        SETRANGE(*x, 0, g->galaxy_maxx - ((108 * 32 * ui_scale) / sm_zoom_f16));
        SETRANGE(*y, 0, g->galaxy_maxy - ((86 * 32 * ui_scale) / sm_zoom_f16));
    } else {
        SETRANGE(*x, -((54 * 32 * ui_scale) / sm_zoom_f16), g->galaxy_maxx - ((54 * 32 * ui_scale) / sm_zoom_f16));
        SETRANGE(*y, -((43 * 32 * ui_scale) / sm_zoom_f16), g->galaxy_maxy - ((43 * 32 * ui_scale) / sm_zoom_f16));
    }
}

/* 1oom-mp: continuous map zoom. sm_span() converts a galaxy-coord delta to render-coord spacing using
   the fractional sm_zoom_f16 (screen px per galaxy unit, 4-bit fixed pt); replaces the classic "* 2".
   The draw primitives still multiply the result by starmap_scale (the integer ICON scale), so star icons
   step while spacing is smooth. sm_zoom_f16 == 32*starmap_scale reproduces the classic "* 2" exactly. */
static int sm_span_axis(int d, int frac16)
{
    int den = 256 * starmap_scale;
    int num = (d * 16 - frac16) * sm_zoom_f16;   /* (d - frac) * F, in render-coord, *16*16 fixed */
    if (den <= 0) { return d * 2; }
    return (num >= 0) ? ((num + den / 2) / den) : -(((-num) + den / 2) / den);
}
int sm_span_x(int d) { return sm_span_axis(d, sm_frac_x16); }
int sm_span_y(int d) { return sm_span_axis(d, sm_frac_y16); }
int sm_span(int d)   { return sm_span_axis(d, 0); }   /* fallback: no sub-unit offset (any unconverted site) */

/* 1oom-mp: derive the integer ICON scale (starmap_scale) from the fractional zoom (sm_zoom_f16). Call
   after setting either. Self-initialises sm_zoom_f16 from starmap_scale when it is still 0/uninit. */
void ui_starmap_zoom_sync_scale(void)
{
    int s;
    if (sm_zoom_f16 < 32) { sm_zoom_f16 = 32 * (starmap_scale > 0 ? starmap_scale : 1); } /* init from scale */
    s = (sm_zoom_f16 + 16) / 32;         /* round(F/2) */
    if (s < 1) { s = 1; }
    if (s > ui_scale) { s = ui_scale; }
    starmap_scale = s;
}

void ui_starmap_set_pos_focus(const struct game_s *g, player_id_t active_player)
{
    const planet_t *p = &g->planet[g->planet_focus_i[active_player]];
    ui_starmap_set_pos(g, p->x, p->y);
}

void ui_starmap_set_pos(const struct game_s *g, int x, int y)
{
    ui_starmap_zoom_sync_scale();   /* 1oom-mp: ensure sm_zoom_f16 init'd before extent math */
    x -= (54 * 32 * ui_scale) / sm_zoom_f16;
    y -= (43 * 32 * ui_scale) / sm_zoom_f16;
    ui_starmap_clamp_xy(g, &x, &y);
    ui_data.starmap.x = x;
    ui_data.starmap.x2 = x;
    ui_data.starmap.y = y;
    ui_data.starmap.y2 = y;
    sm_frac_x16 = 0; sm_frac_y16 = 0;
}

/* 1oom-mp: set the pan origin from a galaxy*16 fixed-point position. Splits into the integer origin
   (clamped, used by logic/save) plus the 1/16-unit fractional remainder (used by sm_span_x/y for a
   smooth sub-pixel render offset). */
void ui_starmap_set_origin16(const struct game_s *g, int ox16, int oy16)
{
    int xi = ox16 >> 4, yi = oy16 >> 4;
    int fx = ox16 - (xi << 4), fy = oy16 - (yi << 4);   /* in [0,16) */
    ui_starmap_clamp_xy(g, &xi, &yi);
    ui_data.starmap.x = xi; ui_data.starmap.x2 = xi;
    ui_data.starmap.y = yi; ui_data.starmap.y2 = yi;
    sm_frac_x16 = fx; sm_frac_y16 = fy;
}

/* 1oom-mp: continuous map zoom to an absolute fixed-point level (sm_zoom_f16 = screen px per galaxy
   unit * 16), holding the galaxy point under the cursor (mx,my) fixed when the cursor is over the map,
   else holding the view centre. NOTE: mx,my are SCALED game coords (moouse_x/y, i.e. *ui_scale), so the
   map window and the (mouse-left)/F anchor are all in *ui_scale units. Range: F in [1, 4*ui_scale] px
   per galaxy unit; the icon scale (starmap_scale) caps at ui_scale, so beyond that spacing keeps growing
   with icons held at max size (deeper zoom-in on a big galaxy). */
void ui_starmap_zoom_to_f16(const struct game_s *g, int new_f16, int mx, int my)
{
    int lo = 16, hi = 32 * ui_scale * 2;
    int old_f16 = sm_zoom_f16;
    int ax16, ay16;     /* galaxy*16 anchor point held fixed on screen across the zoom */
    int ox, oy;         /* anchor's offset from the map top-left, in screen px */
    if (old_f16 < 16) { old_f16 = 32 * (starmap_scale > 0 ? starmap_scale : 1); }
    if (new_f16 < lo) { new_f16 = lo; }
    if (new_f16 > hi) { new_f16 = hi; }
    if (new_f16 == old_f16) { return; }
    /* anchor = the galaxy point under the cursor when it's over the map (zoom-to-cursor); otherwise the
       view centre (slider/keyboard zoom). map window = [6,222)x[6,178) * ui_scale; offset*256/f16 converts
       a screen-px offset to a galaxy*16 delta (256/f16 = 16/F, F = f16/16). */
    if ((mx >= 6 * ui_scale) && (mx < 222 * ui_scale) && (my >= 6 * ui_scale) && (my < 178 * ui_scale)) {
        ox = mx - 6 * ui_scale;
        oy = my - 6 * ui_scale;
    } else {
        ox = 108 * ui_scale;    /* map centre: half of the 216*ui_scale-wide window */
        oy = 86 * ui_scale;     /* half of the 172*ui_scale-tall window */
    }
    ax16 = (ui_data.starmap.x << 4) + sm_frac_x16 + (ox * 256) / old_f16;
    ay16 = (ui_data.starmap.y << 4) + sm_frac_y16 + (oy * 256) / old_f16;
    sm_zoom_f16 = new_f16; ui_starmap_zoom_sync_scale();
    ui_starmap_set_origin16(g, ax16 - (ox * 256) / sm_zoom_f16,
                               ay16 - (oy * 256) / sm_zoom_f16);
}

/* 1oom-mp: integer-scale entry point (zoom slider / legacy callers). Maps scale -> f16 (32*scale). */
void ui_starmap_set_zoom(const struct game_s *g, int new_scale)
{
    ui_starmap_zoom_to_f16(g, 32 * new_scale, moouse_x, moouse_y);
}

static void ui_starmap_select_target(struct starmap_data_s *d, planet_id_t planet_i)
{
    if (!d->controllable || (planet_i == PLANET_NONE)) {
        return;
    }
    d->g->planet_focus_i[d->api] = planet_i;
    if (ui_data.ui_main_loop_action == UI_MAIN_LOOP_TRANS) {
        d->tr.other = true;
    }
}

void ui_starmap_handle_oi_ctrl(struct starmap_data_s *d, int16_t oi)
{
#define XSTEP   0x1b
#define YSTEP   0x15
    const struct game_s *g = d->g;
    bool changed = false;
    int x, y;
    if (g->evn.build_finished_num[d->api]) {
        return;
    }
    x = ui_data.starmap.x;
    y = ui_data.starmap.y;
    if (oi == d->oi_scroll) {
        if (d->scrollx >= 0) {
            x += d->scrollx - 54;
            y += d->scrolly - 43;
            changed = true;
        } else {
            ui_starmap_set_zoom(g, d->scrollz);   /* 1oom-mp: zoom on the view centre, not the homeworld */
        }
    } else if (oi == d->oi_ctrl_ul) {
        x -= XSTEP;
        y -= YSTEP;
        changed = true;
    } else if ((oi == d->oi_ctrl_up) || (oi == d->oi_ctrl_u2)) {
        y -= YSTEP;
        changed = true;
    } else if (oi == d->oi_ctrl_ur) {
        x += XSTEP;
        y -= YSTEP;
        changed = true;
    } else if ((oi == d->oi_ctrl_left) || (oi == d->oi_ctrl_l2)) {
        x -= XSTEP;
        changed = true;
    } else if ((oi == d->oi_ctrl_right) || (oi == d->oi_ctrl_r2)) {
        x += XSTEP;
        changed = true;
    } else if (oi == d->oi_ctrl_dl) {
        x -= XSTEP;
        y += YSTEP;
        changed = true;
    } else if ((oi == d->oi_ctrl_down) || (oi == d->oi_ctrl_d2)) {
        y += YSTEP;
        changed = true;
    } else if (oi == d->oi_ctrl_dr) {
        x += XSTEP;
        y += YSTEP;
        changed = true;
    } else if (oi == d->oi_pgdown) {
        /* 1oom-mp: fine continuous zoom-out (~12.5%/press, min step 2 f16); keep scrollz = derived icon scale */
        ui_starmap_zoom_to_f16(g, sm_zoom_f16 - (sm_zoom_f16 / 8 > 2 ? sm_zoom_f16 / 8 : 2), moouse_x, moouse_y);
        d->scrollz = starmap_scale;
    } else if (oi == d->oi_pgup) {
        /* 1oom-mp: fine continuous zoom-in (~12.5%/press, min step 2 f16); keep scrollz = derived icon scale */
        ui_starmap_zoom_to_f16(g, sm_zoom_f16 + (sm_zoom_f16 / 8 > 2 ? sm_zoom_f16 / 8 : 2), moouse_x, moouse_y);
        d->scrollz = starmap_scale;
    }
    if (changed) {
        ui_starmap_clamp_xy(g, &x, &y);
        ui_data.starmap.x2 = x;
        ui_data.starmap.y2 = y;
        sm_frac_x16 = 0; sm_frac_y16 = 0;
    }
#undef XSTEP
#undef YSTEP
}

void ui_starmap_handle_scrollkeys(struct starmap_data_s *d, int16_t oi)
{
    const struct game_s *g = d->g;
    int x, y, xh, yh;
    if (oi != 0) {
        ui_data.starmap.xhold = 0;
        ui_data.starmap.yhold = 0;
        return;
    }
    if (g->evn.build_finished_num[d->api]) {
        return;
    }
    x = ui_data.starmap.x;
    y = ui_data.starmap.y;
    xh = ui_data.starmap.xhold;
    yh = ui_data.starmap.yhold;
    if (0
      || (ui_sm_uhjk_scroll && kbd_is_pressed(MOO_KEY_u, 0, MOO_MOD_SHIFT | MOO_MOD_ALT | MOO_MOD_CTRL))
      || (ui_sm_mouse_scroll && (moouse_y <= 0))) {
        if (yh > 0) {
            yh = 0;
        }
        --yh;
    } else if (0
      || (ui_sm_uhjk_scroll && kbd_is_pressed(MOO_KEY_j, 0, MOO_MOD_SHIFT | MOO_MOD_ALT | MOO_MOD_CTRL))
      || (ui_sm_mouse_scroll && (moouse_y >= UI_SCREEN_H - 1))) {
        if (yh < 0) {
            yh = 0;
        }
        ++yh;
    } else {
        yh = 0;
    }
    if (yh) {
        y += ui_starmap_scrollkey_accel(yh);
    }
    if (0
      || (ui_sm_uhjk_scroll && kbd_is_pressed(MOO_KEY_h, 0, MOO_MOD_SHIFT | MOO_MOD_ALT | MOO_MOD_CTRL))
      || (ui_sm_mouse_scroll && (moouse_x <= 0))) {
        if (xh > 0) {
            xh = 0;
        }
        --xh;
    } else if (0
      || (ui_sm_uhjk_scroll && kbd_is_pressed(MOO_KEY_k, 0, MOO_MOD_SHIFT | MOO_MOD_ALT | MOO_MOD_CTRL))
      || (ui_sm_mouse_scroll && (moouse_x >= UI_SCREEN_W - 1))) {
        if (xh < 0) {
            xh = 0;
        }
        ++xh;
    } else {
        xh = 0;
    }
    if (xh) {
        x += ui_starmap_scrollkey_accel(xh);
    }
    if (xh || yh) {
        ui_starmap_clamp_xy(g, &x, &y);
        ui_data.starmap.x2 = x;
        ui_data.starmap.y2 = y;
        ui_data.starmap.x = x;
        ui_data.starmap.y = y;
    }
    ui_data.starmap.xhold = xh;
    ui_data.starmap.yhold = yh;
}

void ui_starmap_add_oi_bottom_buttons(struct starmap_data_s *d)
{
    d->oi_gameopts = uiobj_add_mousearea(5, 181, 36, 194, MOO_KEY_g);
    d->oi_design = uiobj_add_mousearea(40, 181, 75, 194, MOO_KEY_d);
    d->oi_fleet = uiobj_add_mousearea(79, 181, 111, 194, MOO_KEY_f);
    d->oi_map = uiobj_add_mousearea(115, 181, 139, 194, MOO_KEY_m);
    d->oi_races = uiobj_add_mousearea(143, 181, 176, 194, (ui_illogical_hotkey_fix ? MOO_KEY_a : MOO_KEY_r));
    d->oi_planets = uiobj_add_mousearea(180, 181, 221, 194, MOO_KEY_p);
    d->oi_tech = uiobj_add_mousearea(225, 181, 254, 194, MOO_KEY_t);
    d->oi_next_turn = uiobj_add_mousearea(258, 181, 314, 194, MOO_KEY_n);
}

bool ui_starmap_handle_oi_bottom_buttons(struct starmap_data_s *d, int16_t oi)
{
    ui_main_loop_action_t action = UI_MAIN_LOOP_NUM;
    if (oi == d->oi_gameopts) {
        action = UI_MAIN_LOOP_GAMEOPTS;
    } else if (oi == d->oi_design) {
        action = UI_MAIN_LOOP_DESIGN;
    } else if (oi == d->oi_fleet) {
        action = UI_MAIN_LOOP_FLEET;
    } else if (oi == d->oi_map) {
        action = UI_MAIN_LOOP_MAP;
    } else if (oi == d->oi_races) {
        action = UI_MAIN_LOOP_RACES;
    } else if (oi == d->oi_planets) {
        action = UI_MAIN_LOOP_PLANETS;
    } else if (oi == d->oi_tech) {
        action = UI_MAIN_LOOP_TECH;
    } else if (oi == d->oi_next_turn) {
        action = UI_MAIN_LOOP_NEXT_TURN;
    }
    if (action != UI_MAIN_LOOP_NUM) {
        ui_data.ui_main_loop_action = action;
        return true;
    }
    return false;
}

void ui_starmap_add_oi_misc(struct starmap_data_s *d)
{
    d->oi_alt_c = uiobj_add_inputkey(MOO_KEY_c | MOO_MOD_ALT);
    d->oi_alt_m = uiobj_add_inputkey(MOO_KEY_m | MOO_MOD_ALT);
    if (d->show_planet_focus) {
        d->oi_alt_r = uiobj_add_inputkey(MOO_KEY_r | MOO_MOD_ALT);
        d->oi_ctrl_r = uiobj_add_inputkey(MOO_KEY_r | MOO_MOD_CTRL);
    }
    if (ui_extra_enabled) {
        d->oi_alt_f = uiobj_add_inputkey(MOO_KEY_f | MOO_MOD_ALT);
        d->oi_alt_o = uiobj_add_inputkey(MOO_KEY_o | MOO_MOD_ALT);
    }
}

bool ui_starmap_handle_oi_misc(struct starmap_data_s *d, int16_t oi)
{
    bool match = false;
    struct game_s *g = d->g;
    uint8_t planet_focus_i = g->planet_focus_i[d->api];
    if (oi == d->oi_alt_m) {
        ui_data.starmap.flag_show_grid = !ui_data.starmap.flag_show_grid;
        match = true;
    } else if (oi == d->oi_alt_c) {
        d->set_pos_focus(d->g, d->api);
        match = true;
    } else if (oi == d->oi_alt_f) {
        ui_data.starmap.flag_show_own_routes = !ui_data.starmap.flag_show_own_routes;
        match = true;
    } else if (oi == d->oi_alt_o) {
        ui_data.starmap.star_text_type = (ui_data.starmap.star_text_type + 1) % UI_SM_STAR_TEXT_NUM;
        match = true;
    } else if ((oi == d->oi_alt_r) && game_reloc_dest_ok(g, planet_focus_i, d->api)) {
        for (int i = 0; i < g->galaxy_stars; ++i) {
            planet_t *p = &(g->planet[i]);
            if ((p->owner == d->api) && (p->reloc != i)) {
                p->reloc = planet_focus_i;
            }
        }
        match = true;
    } else if ((oi == d->oi_ctrl_r) && game_reloc_dest_ok(g, planet_focus_i, d->api)) {
        int count = 0;
        for (int i = 0; i < g->galaxy_stars; ++i) {
            planet_t *p = &(g->planet[i]);
            if ((p->owner == d->api) && (p->reloc != planet_focus_i)) {
                p->reloc = planet_focus_i;
                ++count;
            }
        }
        if (count == 0) {
            for (int i = 0; i < g->galaxy_stars; ++i) {
                planet_t *p = &(g->planet[i]);
                if (p->owner == d->api) {
                    p->reloc = i;
                }
            }
        }
        match = true;
    }
    return match;
}

void ui_starmap_fill_oi_tbls(struct starmap_data_s *d)
{
    const struct game_s *g = d->g;
    STARMAP_LIM_INIT();
    uiobj_set_limits(STARMAP_LIMITS);
    UIOBJI_SET_TBL_INVALID(d->oi_tbl_enroute);
    UIOBJI_SET_TBL_INVALID(d->oi_tbl_transport);
    ui_starmap_add_oi_enroute(d, false);
    for (int i = 0; i < g->galaxy_stars; ++i) {
        for (int j = 0; j < g->players; ++j) {
            d->oi_tbl_pl_stars[j][i] = UIOBJI_INVALID;
        }
    }
    for (int i = 0; i < g->galaxy_stars; ++i) {
        const planet_t *p = &(g->planet[i]);
        if (BOOLVEC_IS1(p->within_srange, d->api) || BOOLVEC_IS1(g->eto[d->api].orbit[i].visible, d->api)) {
            int numorbits;
            player_id_t tblpl[PLAYER_NUM];
            numorbits = 0;
            for (int j = 0; j < g->players; ++j) {
                const fleet_orbit_t *r = &(g->eto[j].orbit[i]);
                if (BOOLVEC_IS0(p->within_srange, d->api) && (j != d->api)) {
                    continue;
                }
                /* 1oom-mp teams: the clickareas MUST line up row-for-row with the badges drawn in
                   ui_starmap_draw_starmap (which is overlay-aware). For a teammate whose live plan is
                   streaming, use their relayed orbit -- otherwise a fleet they sent en-route this turn
                   still occupies an (invisible) orbit row here, shifting your own badge's clickarea down
                   so clicking YOUR badge actually selects THEIR co-located fleet (wrong owner's ships). */
                if (ui_mp_team_plan_active(j)) {
                    if (ui_mp_team_plan_orbit_has(j, i)) { tblpl[numorbits++] = j; }
                    continue;
                }
                for (int k = 0; k < g->eto[j].shipdesigns_num; ++k) {
                    if (r->ships[k]) {
                        tblpl[numorbits++] = j;
                        break;
                    }
                }
            }
            /* badges stack vertically (offscreen render-coord 2*p + offset; +6 per badge), placed where
               the downscale-blit draws them so clicking your badge selects YOUR co-located ships. */
            for (int j = 0; j < numorbits; ++j) {
                d->oi_tbl_pl_stars[tblpl[j]][i] = sm_add_galaxy_mousearea(p->x, p->y, 26, 8 + j * 6, 34, 12 + j * 6);
            }
        }
    }
    ui_starmap_add_oi_enroute(d, true);
}

void ui_starmap_fill_oi_tbl_stars(struct starmap_data_s *d)
{
    const struct game_s *g = d->g;
    for (int i = 0; i < g->galaxy_stars; ++i) {
        const planet_t *p = &(g->planet[i]);
        d->oi_tbl_stars[i] = sm_add_galaxy_mousearea(p->x, p->y, 8, 8, 21, 21);
    }
}

void ui_starmap_fill_oi_tbl_stars_own(struct starmap_data_s *d, player_id_t owner)
{
    const struct game_s *g = d->g;
    for (int i = 0; i < g->galaxy_stars; ++i) {
        const planet_t *p = &(g->planet[i]);
        if (p->owner == owner) {
            d->oi_tbl_stars[i] = sm_add_galaxy_mousearea(p->x, p->y, 8, 8, 21, 21);
        }
    }
}

void ui_starmap_clear_oi_ctrl(struct starmap_data_s *d)
{
    d->oi_scroll = UIOBJI_INVALID;
    d->oi_ctrl_left = UIOBJI_INVALID;
    d->oi_ctrl_l2 = UIOBJI_INVALID;
    d->oi_ctrl_right = UIOBJI_INVALID;
    d->oi_ctrl_r2 = UIOBJI_INVALID;
    d->oi_ctrl_ul = UIOBJI_INVALID;
    d->oi_ctrl_ur = UIOBJI_INVALID;
    d->oi_ctrl_up = UIOBJI_INVALID;
    d->oi_ctrl_u2 = UIOBJI_INVALID;
    d->oi_ctrl_dl = UIOBJI_INVALID;
    d->oi_ctrl_down = UIOBJI_INVALID;
    d->oi_ctrl_d2 = UIOBJI_INVALID;
    d->oi_ctrl_dr = UIOBJI_INVALID;
    d->oi_pgup = UIOBJI_INVALID;
    d->oi_pgdown = UIOBJI_INVALID;
}

void ui_starmap_fill_oi_ctrl(struct starmap_data_s *d)
{
    d->oi_scroll = uiobj_add_tb(6, 6, 2, 2, 108, 86, &d->scrollx, &d->scrolly, &d->scrollz, ui_scale);
    d->oi_ctrl_left = uiobj_add_inputkey(MOO_KEY_LEFT | MOO_MOD_CTRL);
    if (MOO_KEY_LEFT != MOO_KEY_KP4) {
        d->oi_ctrl_l2 = uiobj_add_inputkey(MOO_KEY_KP4 | MOO_MOD_CTRL);
    }
    d->oi_ctrl_right = uiobj_add_inputkey(MOO_KEY_RIGHT | MOO_MOD_CTRL);
    if (MOO_KEY_RIGHT != MOO_KEY_KP6) {
        d->oi_ctrl_r2 = uiobj_add_inputkey(MOO_KEY_KP6 | MOO_MOD_CTRL);
    }
    d->oi_ctrl_ul = uiobj_add_inputkey(MOO_KEY_KP7 | MOO_MOD_CTRL);
    d->oi_ctrl_ur = uiobj_add_inputkey(MOO_KEY_KP9 | MOO_MOD_CTRL);
    d->oi_ctrl_up = uiobj_add_inputkey(MOO_KEY_UP | MOO_MOD_CTRL);
    if (MOO_KEY_UP != MOO_KEY_KP8) {
        d->oi_ctrl_u2 = uiobj_add_inputkey(MOO_KEY_KP8 | MOO_MOD_CTRL);
    }
    d->oi_ctrl_dl = uiobj_add_inputkey(MOO_KEY_KP1 | MOO_MOD_CTRL);
    d->oi_ctrl_down = uiobj_add_inputkey(MOO_KEY_DOWN | MOO_MOD_CTRL);
    if (MOO_KEY_DOWN != MOO_KEY_KP2) {
        d->oi_ctrl_d2 = uiobj_add_inputkey(MOO_KEY_KP2 | MOO_MOD_CTRL);
    }
    d->oi_ctrl_dr = uiobj_add_inputkey(MOO_KEY_KP3 | MOO_MOD_CTRL);
    d->oi_pgup = uiobj_add_inputkey(MOO_KEY_PAGEUP);
    d->oi_pgdown = uiobj_add_inputkey(MOO_KEY_PAGEDOWN);
}

void ui_starmap_sn0_setup(struct shipnon0_s *sn0, int sd_num, const shipcount_t *ships)
{
    int num = 0;
    for (int i = 0; i < sd_num; ++i) {
        shipcount_t n;
        n = ships[i];
        sn0->ships[num] = n;
        if (n) {
            sn0->type[num++] = i;
        }
    }
    sn0->num = num;
}

void ui_starmap_update_reserve_fuel(struct game_s *g, struct shipnon0_s *sn0, const shipcount_t *ships, player_id_t pi)
{
    const bool *hrf = &(g->srd[pi].have_reserve_fuel[0]);
    for (int i = 0; i < sn0->num; ++i) {
        int st;
        st = sn0->type[i];
        if ((!hrf[st]) && (ships[st] != 0)) {
            sn0->have_reserve_fuel = false;
            return;
        }
    }
    sn0->have_reserve_fuel = true;
}

void ui_starmap_draw_planetinfo(const struct game_s *g, player_id_t api, int planet_i, bool draw_name)
{
    const planet_t *p = &(g->planet[planet_i]);
    ui_starmap_draw_planetinfo_do(g, api, planet_i, BOOLVEC_IS1(p->explored, api), true, draw_name);
}

void ui_starmap_draw_planetinfo_2(const struct game_s *g, int p1, int p2, int planet_i)
{
    const planet_t *p = &(g->planet[planet_i]);
    player_id_t api = (p1 < PLAYER_NUM) ? p1 : p2;
    bool explored = true;
    if (0
      || (IS_HUMAN(g, p1) && BOOLVEC_IS0(p->explored, p1))
      || (IS_HUMAN(g, p2) && BOOLVEC_IS0(p->explored, p2))
    ) {
        explored = false;
    }
    ui_starmap_draw_planetinfo_do(g, api, planet_i, explored, false, true);
}

int ui_starmap_newship_next(const struct game_s *g, player_id_t pi, int i)
{
    int t = i;
    const planet_t *p;
    do {
        i = (i + 1) % g->galaxy_stars;
        p = &(g->planet[i]);
    } while ((!((p->owner == pi) && BOOLVEC_IS1(p->finished, FINISHED_SHIP))) && (i != t));
    return i;
}

int ui_starmap_newship_prev(const struct game_s *g, player_id_t pi, int i)
{
    int t = i;
    const planet_t *p;
    do {
        if (--i < 0) { i = g->galaxy_stars - 1; }
        p = &(g->planet[i]);
    } while ((!((p->owner == pi) && BOOLVEC_IS1(p->finished, FINISHED_SHIP))) && (i != t));
    return i;
}

int ui_starmap_enemy_incoming(const struct game_s *g, player_id_t pi, int i, bool next)
{
    int t = i;
    do {
        if (next) {
            i = (i + 1) % g->galaxy_stars;
        } else {
            if (--i < 0) { i = g->galaxy_stars - 1; }
        }
        if (g->planet[i].owner == pi) {
           for (int j = 0; j < g->enroute_num; ++j) {
               const fleet_enroute_t *r = &(g->enroute[j]);
               if (BOOLVEC_IS1(r->visible, pi) && (r->owner != pi) && (r->dest == i)) {
                    return i;
                }
            }
            for (int j = 0; j < g->transport_num; ++j) {
                const transport_t *r = &(g->transport[j]);
                if (BOOLVEC_IS1(r->visible, pi) && (r->owner != pi) && (r->dest == i)) {
                    return i;
                }
            }
        }
    } while (i != t);
    return i;
}

void ui_starmap_common_init(struct game_s *g, struct starmap_data_s *d, player_id_t active_player)
{
    d->set_pos_focus = ui_starmap_set_pos_focus;
    d->g = g;
    d->api = active_player;
    d->controllable = false;
    d->show_planet_focus = true;
    d->anim_delay = 0;
    d->planet_draw_name = true;
    d->scrollx = 0;
    d->scrolly = 0;
    /* 1oom-mp: do NOT reset the zoom on every sub-mode entry -- that snapped a fractional continuous zoom
       to the nearest integer icon level when you e.g. clicked a ship. Preserve the current sm_zoom_f16
       (zoom_sync_scale only inits it from starmap_scale the very first time, when it's still unset). */
    ui_starmap_zoom_sync_scale();
    d->scrollz = starmap_scale;
}

void ui_starmap_common_update_mouse_hover(struct starmap_data_s *d, int16_t oi)
{
    d->bottom_highlight = -1;
    if (oi == d->oi_gameopts) {
        d->bottom_highlight = 0;
    } else if (oi == d->oi_design) {
        d->bottom_highlight = 1;
    } else if (oi == d->oi_fleet) {
        d->bottom_highlight = 2;
    } else if (oi == d->oi_map) {
        d->bottom_highlight = 3;
    } else if (oi == d->oi_races) {
        d->bottom_highlight = 4;
    } else if (oi == d->oi_planets) {
        d->bottom_highlight = 5;
    } else if (oi == d->oi_tech) {
        d->bottom_highlight = 6;
    } else if (oi == d->oi_next_turn) {
        d->bottom_highlight = 7;
    }
    if (ui_sm_mouseover_focus && d->controllable) {
        ui_starmap_select_target(d, ui_starmap_cursor_on_star(d, oi));
    }
}
