#include "config.h"

#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>

#include "ui.h"
#include "mp.h"
#include "game.h"
#include "game_save.h"
#include "game_shipdesign.h"
#include "game_turn_start.h"
#include "lib.h"
#include "log.h"
#include "types.h"
#include "uicheat.h"
#include "uicmds.h"
#include "uidefs.h"
#include "uidesign.h"
#include "uiempire.h"
#include "uiempirereport.h"
#include "uiempirestats.h"
#include "uifinished.h"
#include "uifleet.h"
#include "uihelp.h"
#include "uiinput.h"
#include "uiload.h"
#include "uiplanet.h"
#include "uiraces.h"
#include "uispecs.h"
#include "uiswitch.h"
#include "uitech.h"
#include "uiview.h"
#include "util.h"

/* -------------------------------------------------------------------------- */

static int cmd_next(struct game_s *g, int api, struct input_token_s *param, int num_param, void *var)
{
    return 0;
}

static const struct input_cmd_s * const cmdsptr_turn[3];

static const struct input_cmd_s cmds_turn[] = {
    { "?", NULL, "Help", 0, 0, 0, ui_cmd_help, (void *)cmdsptr_turn },
    { "n", NULL, "Next turn", 0, 0, 0, cmd_next, 0 },
    { "v", "[RANGE] [+DIST] [FILTERS]", "View galaxy up to RANGE and or DIST from planet\nFILTERS: Planet, Fleet, Transport,\n         mY, Opponent,\n         Unexplored, eXplored,\n         uNcolonized, Colonized", 0, 3, 0, ui_cmd_view, 0 },
    { "l", "[PLANET|*]", "Look at planet", 0, 1, 0, ui_cmd_planet_look, 0 },
    { "g", "PLANET", "Go to planet", 1, 1, 0, ui_cmd_planet_go, 0 },
    { "s", "SLIDER VALUE", "Set planet slider\nSLIDER is s, d, i, e or t\nVALUE can be +N or -N for relative adjustment", 2, 2, 0, ui_cmd_planet_slider, 0 },
    { "sl", "SLIDER", "Toggle planet slider lock", 1, 1, 0, ui_cmd_planet_slider_lock, 0 },
    { "b", "[SHIP]", "Select ship to build", 0, 1, 0, ui_cmd_planet_build, 0 },
    { "bscrap", "VALUE", "Scrap missile bases", 1, 1, 0, ui_cmd_planet_scrap_bases, 0 },
    { "reloc", "[PLANET]", "Relocate built ships to", 0, 1, 0, ui_cmd_planet_reloc, 0 },
    { "trans", "[PLANET NUM]", "Transport troops to", 0, 2, 0, ui_cmd_planet_trans, 0 },
    { "res", "[BC]", "Transfer reserves", 0, 1, 0, ui_cmd_planet_reserve, 0 },
    { "fs", "PLANET [NUM]*", "Send fleet to\nAll ships are sent if no NUM given", 1, 7, 0, ui_cmd_fleet_send, 0 },
    { "des", NULL, "Ship design", 0, 0, 0, ui_cmd_design, 0 },
    { "fleet", NULL, "Display fleets", 0, 0, 0, ui_cmd_fleet_list, 0 },
    { "scrap", "[SHIP]", "Scrap ship", 0, 1, 0, ui_cmd_fleet_scrap, 0 },
    { "specs", "[SHIP]", "Show fleet specs", 0, 1, 0, ui_cmd_fleet_specs, 0 },
    { "redir", "[#Fn|#Tn PLANET]", "Redirect fleet/transport to planet\nShows redirectable items if no params given", 0, 2, 0, ui_cmd_fleet_redir, 0 },
    { "t", "[FIELD]", "View technology\nFIELD is c, o, f, p, r or w\nShows sliders if no field given", 0, 1, 0, ui_cmd_tech_look, 0 },
    { "ts", "FIELD VALUE", "Set tech slider to value\nVALUE can be +N or -N for relative adjustment", 2, 2, 0, ui_cmd_tech_slider, 0 },
    { "tsl", "FIELD", "Toggle tech slider lock", 1, 1, 0, ui_cmd_tech_slider_lock, 0 },
    { "t=", NULL, "Equalize tech sliders", 0, 0, 0, ui_cmd_tech_equals, 0 },
    { "emp", NULL, "Empire overview", 0, 0, 0, ui_cmd_empire_look, 0 },
    { "tax", "VALUE", "Set tax (tenths of percent)\nVALUE can be +N or -N for relative adjustment", 1, 1, 0, ui_cmd_empire_tax, 0 },
    { "races", NULL, "Show races", 0, 0, 0, ui_cmd_races, 0 },
    { "stats", NULL, "Show stats", 0, 0, 0, ui_cmd_empirestats, 0 },
    { "spy", "RACE VALUE|MODE", "Set spy spending (tenths of percent) or mode (Hide, Esp., Sabotage)\nVALUE can be +N or -N for relative adjustment", 2, 2, 0, ui_cmd_spy, 0 },
    { "sec", "VALUE", "Set security spending (tenths of percent)\nVALUE can be +N or -N for relative adjustment", 1, 1, 0, ui_cmd_empire_security, 0 },
    { "rep", NULL, "Show report", 0, 0, 0, ui_cmd_empirereport, 0 },
    { "aud", NULL, "Audience", 0, 0, 0, ui_cmd_audience, 0 },
    { "galaxy", NULL, NULL, 0, 0, 0, ui_cmd_cheat_galaxy, 0 },
    { "events", NULL, NULL, 0, 0, 0, ui_cmd_cheat_events, 0 },
    { "moola", NULL, NULL, 0, 0, 0, ui_cmd_cheat_moola, 0 },
    { NULL, NULL, NULL, 0, 0, 0, NULL, 0 }
};

static const struct input_cmd_s * const cmdsptr_turn[3] = {
    cmds_turn,
    ui_cmds_opts,
    NULL
};

/* -------------------------------------------------------------------------- */

ui_turn_action_t ui_game_turn(struct game_s *g, int *load_game_i_ptr, int pi)
{
    ui_switch_1_opts(g, pi);
    game_turn_start_messages(g, pi);
    ui_finished_print_all(g, pi);
    while (1) {
        char *input;
        char prompt[80], buf_planet_name[PLANET_NAME_LEN];
        lib_sprintf(prompt, sizeof(prompt), "%s | %i | %s > ", g->emperor_names[pi], g->year + YEAR_BASE, ui_planet_str(g, pi, g->planet_focus_i[pi], buf_planet_name, sizeof(buf_planet_name)));
        input = ui_input_line(prompt);
        if ((ui_input_tokenize(input, cmdsptr_turn) == 0) && (ui_data.input.num > 0)) {
            if (ui_data.input.tok[0].type == INPUT_TOKEN_COMMAND) {
                const struct input_cmd_s *cmd;
                int v;
                cmd = ui_data.input.tok[0].data.cmd;
                v = cmd->handle(g, pi, &ui_data.input.tok[1], ui_data.input.num - 1, cmd->var);
                if (v >= 0) {
                    if (cmd->handle == cmd_next) {
                        return UI_TURN_ACT_NEXT_TURN;
                    }
                    if (cmd->handle == ui_cmd_load) {
                        *load_game_i_ptr = v;
                        return UI_TURN_ACT_LOAD_GAME;
                    }
                    if (cmd->handle == ui_cmd_quit) {
                        return UI_TURN_ACT_QUIT_GAME;
                    }
                }
            }
        }
    }
}

void ui_game_start(struct game_s *g)
{
    BOOLVEC_CLEAR(ui_data.players_viewing, PLAYER_NUM);
    fputs("Welcome! Type ? to get the list of commands.\n", stdout);
}

void ui_game_end(struct game_s *g)
{
}

void ui_mp_connect_failed(const char *addr)
{
    printf("MP: could not connect to %s\n", addr);
}

void ui_mp_wait(int reason)
{
    (void)reason; /* headless: no waiting screen (mp.c paces the wait) */
}

void ui_mp_battle_spectate(const struct battle_s *bt)
{
    (void)bt; /* headless: no battle render */
}

void ui_mp_battle_glide(struct battle_s *bt, int itemi, int sx, int sy)
{
    (void)bt; (void)itemi; (void)sx; (void)sy; /* cmdline: no battle render */
}

void ui_battle_init_spectate(struct battle_s *bt)
{
    (void)bt; /* cmdline: no battle render */
}

int ui_mp_battle_watch_prompt(void)
{
    return 0; /* cmdline: no battle render */
}

int ui_mp_lobby_run(int my_id)
{
    /* cmdline client: no interactive lobby UI -- auto-pick a race by slot, ready up, and wait.
       Resume lobby: readying needs a seat, so claim one first (-mprace's if given, else the
       first open one; a claim lost to a race just retries on the next poll). */
    struct mp_lobby_s lob;
    bool done_fresh = false, done_ready = false;
    while (g_mp_cl_lobby_poll) {
        int p = g_mp_cl_lobby_poll(&lob);
        if (p != 0) { return (p > 0) ? 0 : -1; }
        if (g_mp_cl_lobby_set && lob.resume && !done_ready) {
            bool seated = false;
            for (int j = 0; (j < lob.cap) && (j < MP_MAX_PLAYERS); ++j) { if (lob.seat_owner[j] == my_id) { seated = true; break; } }
            if (seated) {
                g_mp_cl_lobby_set(MP_LOBBY_F_READY, 1);
                done_ready = true;
            } else {
                int pick = -1;
                if (g_mp_cl_req_race >= 0) {
                    for (int j = 0; (j < lob.cap) && (j < MP_MAX_PLAYERS); ++j) { if ((lob.seat_owner[j] == 0xff) && (lob.seat_race[j] == g_mp_cl_req_race)) { pick = j; break; } }
                }
                if (pick < 0) { for (int j = 0; (j < lob.cap) && (j < MP_MAX_PLAYERS); ++j) { if (lob.seat_owner[j] == 0xff) { pick = j; break; } } }
                if (pick >= 0) { g_mp_cl_lobby_set(MP_LOBBY_F_SEAT, pick); }
            }
        } else if (g_mp_cl_lobby_set && !lob.resume && !done_fresh) {
            g_mp_cl_lobby_set(MP_LOBBY_F_RACE, my_id % RACE_NUM);
            g_mp_cl_lobby_set(MP_LOBBY_F_READY, 1);
            done_fresh = true;
        }
        usleep(20000);
    }
    return 0;
}
