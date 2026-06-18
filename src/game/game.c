#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "cfg.h"
#include "game.h"
#include "gameapi.h"
#include "game_ai.h"
#include "game_aux.h"
#include "game_misc.h"
#include "game_mp_orders.h"
#include "game_new.h"
#include "game_num.h"
#include "game_nump.h"
#include "game_audience.h"
#include "game_battle.h"
#include "game_battle_human.h"
#include "game_election.h"
#include "game_end.h"
#include "game_event.h"
#include "game_news.h"
#include "game_save.h"
#include "game_ground.h"
#include "lib.h"
#include "os.h"
#include "game_save_moo13.h"
#include "game_strp.h"
#include "game_turn.h"
#include "game_tech.h"
#include "log.h"
#include "mp.h"
#include "net.h"
#include "options.h"
#include "rnd.h"
#include "ui.h"
#include "util.h"
#include <stdio.h>

/* -------------------------------------------------------------------------- */

bool game_opt_skip_intro_always = false;
static bool game_opt_skip_intro = false;
static bool game_opt_new_game = false;
static bool game_opt_continue = false;
static int game_opt_load_game = 0;
static const char *game_opt_load_fname = 0;
static bool game_opt_undo_enabled = true;
static bool game_opt_year_save_enabled = false;
static bool game_opt_init_save_enabled = true;
static bool game_opt_next_turn = false;
static bool game_opt_save_quit = false;

static struct game_end_s game_opt_end = { GAME_END_NONE, 0, 0, 0, 0 };
static struct game_new_options_s game_opt_new = GAME_NEW_OPTS_DEFAULT;
struct game_new_options_s game_opt_custom = GAME_NEW_OPTS_DEFAULT;

static int game_opt_new_value = 200;
static int game_opt_custom_race_value = 0xaaaaa0;
static int game_opt_custom_banner_value = 666660;
static int game_opt_custom_isai_value = 111110;

bool game_opt_message_filter[FINISHED_NUM] = { false, false, false, false, false, false };

static struct game_s game;
static struct game_aux_s game_aux;

/* 1oom-mp: multiplayer entry options (set via -mphost PORT / -mpjoin HOST[:PORT]) */
static int opt_mp_host_port = 0;
static char *opt_mp_join = NULL;
static int opt_mp_humans = 1; /* host: number of human clients to wait for (empires 0..N-1) */
static char *opt_mp_load = NULL; /* host: path to an MP autosave to resume instead of starting a new game */
static int s_mp_humans = 1;      /* host: human-client count, recorded in the autosave header for -mpload */
#define MP_DEFAULT_PORT 24444


/* -------------------------------------------------------------------------- */

static void game_start(struct game_s *g)
{
    if (g->seed == 0) {
        g->seed = rnd_get_new_seed();
        log_message("Game: seed was 0, got new seed 0x%0x\n", g->seed);
    }
    if (g->ai_id >= GAME_AI_NUM) {
        log_warning("Game: AI ID was %i >= %i, setting to %i (%s)\n", g->ai_id, GAME_AI_NUM, GAME_AI_CLASSIC, game_ais[GAME_AI_CLASSIC]->name);
        g->ai_id = GAME_AI_CLASSIC;
    }
    game_ai = game_ais[g->ai_id];
    game_update_production(g);
    game_update_tech_util(g);
    if (game_num_update_on_load) {
        for (int i = 0; i < g->players; ++i) {
            game_update_eco_on_waste(g, i, false);
            game_update_seen_by_orbit(g, i);
        }
    }
    game_update_within_range(g);
    game_update_visibility(g);
    game_update_have_reserve_fuel(g);
}

static void game_stop(struct game_s *g)
{
    g->gaux->flag_cheat_galaxy = false;
    g->gaux->flag_cheat_elections = false;
    g->gaux->flag_cheat_events = false;
    g->gaux->flag_cheat_spy_hint = false;
    g->gaux->flag_cheat_stars = false;
    g->gaux->flag_cheat_tech_hint = false;
    g->gaux->flag_cheat_news = false;
}

static void game_set_opts_from_value(struct game_new_options_s *go, int v)
{
    int v2;
    v2 = v % 10;
    v = v / 10;
    go->difficulty = v2;
    v2 = v % 10;
    v = v / 10;
    go->galaxy_size = v2;
    go->players = v;
}

static int game_get_opts_value(const struct game_s *g)
{
    return g->difficulty + g->galaxy_size * 10 + g->players * 100;
}

static void game_set_custom_opts_from_cfg(struct game_new_options_s *go)
{
    uint32_t races = game_opt_custom_race_value;
    uint32_t banners = game_opt_custom_banner_value;
    uint32_t is_ai = game_opt_custom_isai_value;

    for (int i = 0; i < PLAYER_NUM; ++i) {
        go->pdata[i].race = races % 0x10;
        races /= 0x10;
        go->pdata[i].banner = banners % 10;
        banners /= 10;
        go->pdata[i].is_ai = is_ai % 10;
        is_ai /= 10;
    }
}

static void game_save_custom_opts_to_cfg(struct game_new_options_s *go)
{
    game_opt_custom_race_value = 0;
    game_opt_custom_banner_value = 0;
    game_opt_custom_isai_value = 0;
    for (int i = PLAYER_NUM - 1; i >= 0; --i) {
        game_opt_custom_race_value *= 0x10;
        game_opt_custom_race_value += go->pdata[i].race;
        game_opt_custom_banner_value *= 10;
        game_opt_custom_banner_value += go->pdata[i].banner;
        game_opt_custom_isai_value *= 10;
        game_opt_custom_isai_value += go->pdata[i].is_ai;
    }
}

void game_apply_rules(void)
{
    if (game_opt_fix_bugs) {
        game_num_fix_bugs();
    }
    if (game_opt_fix_starting_ships) {
        game_num_fix_starting_ships();
    }
    game_num_fix_bugs_1_3a();
}

/* -------------------------------------------------------------------------- */

static int game_opt_do_new_seed(char **argv, void *var)
{
    uint32_t vo, vr, vb, vs, va;
    char buf[512];
    char *stropt = NULL;
    char *strrace = NULL;
    char *strbanner = NULL;
    char *strseed = NULL;
    char *strhuman = NULL;
    strncpy(buf, argv[1], 511);
    buf[511] = 0;
    {
        char *p = buf;
        stropt = (*p != ':') ? p : NULL;
        p = strchr(p, ':');
        if (p) {
            *p++ = 0;
            strrace = (*p != ':') ? p : NULL;
            p = strchr(p, ':');
            if (p) {
                *p++ = 0;
                strbanner = (*p != ':') ? p : NULL;
                p = strchr(p, ':');
                if (p) {
                    *p++ = 0;
                    strseed = (*p != ':') ? p : NULL;
                    p = strchr(p, ':');
                    if (p) {
                        *p++ = 0;
                        strhuman = (*p != ':') ? p : NULL;
                    }
                }
            }
        }
    }
    {
        uint32_t v = 0, v2;
        if (stropt) {
            if (!util_parse_number(stropt, &v)) {
                log_error("invalid value '%s'\n", stropt);
                return -1;
            }
        } else {
            v = game_opt_new_value;
        }
        vo = v;
        v2 = v % 10;
        v = v / 10;
        if (v2 > DIFFICULTY_NUM) {
            log_error("invalid difficulty num %i\n", v2);
            return -1;
        }
        game_opt_new.difficulty = v2;
        v2 = v % 10;
        v = v / 10;
        if (v2 >= GALAXY_SIZE_NUM) {
            log_error("invalid galaxy size num %i\n", v2);
            return -1;
        }
        game_opt_new.galaxy_size = v2;
        v2 = v % 10;
        if ((v2 < 2) || (v2 > PLAYER_NUM)) {
            log_error("invalid players num %i\n", v2);
            return -1;
        }
        game_opt_new.players = v2;
    }
    {
        uint32_t v = 0, v2;
        if (strrace) {
            if (!util_parse_number(strrace, &v)) {
                log_error("invalid value '%s'\n", strrace);
                return -1;
            }
        } else {
            v = 0;
        }
        vr = v;
        for (int i = 0; i < PLAYER_NUM; ++i) {
            v2 = v & 0xf;
            v = v >> 4;
            if (v2 > RACE_NUM) {
                log_error("invalid race num %i\n", v2);
                return -1;
            }
            v2 = v2 ? (v2 - 1) : RACE_RANDOM;
            game_opt_new.pdata[i].race = v2;
        }
    }
    {
        uint32_t v = 0, v2;
        if (strbanner) {
            if (!util_parse_number(strbanner, &v)) {
                log_error("invalid value '%s'\n", strbanner);
                return -1;
            }
        } else {
            v = 0;
        }
        vb = v;
        for (int i = 0; i < PLAYER_NUM; ++i) {
            v2 = v % 10;
            v = v / 10;
            if (v2 > BANNER_NUM) {
                log_error("invalid banner num %i\n", v2);
                return -1;
            }
            v2 = v2 ? (v2 - 1) : BANNER_RANDOM;
            game_opt_new.pdata[i].banner = v2;
        }
    }
    {
        uint32_t v = 0;
        if (strseed) {
            if (!util_parse_number(strseed, &v)) {
                log_error("invalid value '%s'\n", strseed);
                return -1;
            }
        } else {
            v = 0;
        }
        vs = v;
        game_opt_new.galaxy_seed = v;
    }
    {
        uint32_t v = 0, v2;
        if (strhuman) {
            if (!util_parse_number(strhuman, &v)) {
                log_error("invalid value '%s'\n", strhuman);
                return -1;
            }
        } else {
            v = 1;
        }
        va = v;
        for (int i = 0; i < PLAYER_NUM; ++i) {
            v2 = v % 10;
            v = v / 10;
            game_opt_new.pdata[i].is_ai = !v2;
        }
    }
    game_opt_skip_intro = true;
    game_opt_load_game = 0;
    game_opt_load_fname = 0;
    game_opt_continue = false;
    game_opt_new_game = true;
    log_message("Game: -new %u:0x%x:%u:0x%x:%u\n", vo, vr, vb, vs, va);
    return 0;
}

static int game_opt_set_new_name(char **argv, void *var)
{
    uint32_t v = 0;
    char *buf;
    if (!util_parse_number(argv[1], &v)) {
        log_error("invalid value '%s'\n", argv[1]);
        return -1;
    } else if ((v < 1) || (v > PLAYER_NUM)) {
        log_error("invalid player num %i\n", v);
        return -1;
    }
    buf = game_opt_new.pdata[v - 1].playername;
    strncpy(buf, argv[2], EMPEROR_NAME_LEN);
    buf[EMPEROR_NAME_LEN - 1] = '\0';
    log_message("Game: player %i name '%s'\n", v, buf);
    return 0;
}

static int game_opt_set_new_home(char **argv, void *var)
{
    uint32_t v = 0;
    char *buf;
    if (!util_parse_number(argv[1], &v)) {
        log_error("invalid value '%s'\n", argv[1]);
        return -1;
    } else if ((v < 1) || (v > PLAYER_NUM)) {
        log_error("invalid player num %i\n", v);
        return -1;
    }
    buf = game_opt_new.pdata[v - 1].homename;
    strncpy(buf, argv[2], PLANET_NAME_LEN);
    buf[PLANET_NAME_LEN - 1] = '\0';
    log_message("Game: player %i home '%s'\n", v, buf);
    return 0;
}

static int game_opt_set_new_ai(char **argv, void *var)
{
    uint32_t v = 0;
    if (!util_parse_number(argv[1], &v)) {
        log_error("invalid value '%s'\n", argv[1]);
        return -1;
    } else if (v > GAME_AI_NUM) {
        log_error("invalid AI num %i\n", v);
        return -1;
    }
    game_opt_new.ai_id = v;
    log_message("Game: AI type %i '%s'\n", v, game_ais[v]->name);
    return 0;
}


static int game_opt_do_load(char **argv, void *var)
{
    uint32_t v = 0;
    if (1
      && util_parse_number(argv[1], &v)
      && (((v >= 1) && (v <= NUM_ALL_SAVES)) || ((v >= 2300) && (v <= 9999)))
    ) {
        game_opt_load_game = v;
        game_opt_load_fname = 0;
        log_message("Game: load game %i\n", game_opt_load_game);
    } else {
        game_opt_load_game = 0;
        game_opt_load_fname = argv[1];
        log_message("Game: load game '%s'\n", game_opt_load_fname);
    }
    game_opt_skip_intro = true;
    game_opt_continue = false;
    game_opt_new_game = false;
    return 0;
}

static int game_opt_do_continue(char **argv, void *var)
{
    game_opt_load_game = 0;
    game_opt_load_fname = 0;
    game_opt_skip_intro = true;
    game_opt_continue = true;
    game_opt_new_game = false;
    log_message("Game: continue game\n");
    return 0;
}

static int dump_strings(char **argv, void *var)
{
    game_str_dump();
    return -1;
}

static int dump_numbers(char **argv, void *var)
{
    game_num_dump();
    return -1;
}

/* -------------------------------------------------------------------------- */

const char *idstr_main = "game";

bool main_use_lbx = true;
bool main_use_cfg = true;

void (*main_usage)(void) = 0;

const struct cmdline_options_s main_cmdline_options_early[] = {
    { "-dumpstr", 0,
      dump_strings, NULL,
      NULL, "Dump strings in PBXIN format" },
    { "-dumpnum", 0,
      dump_numbers, NULL,
      NULL, "Dump numbers in PBXIN format" },
    { 0, 0, 0, 0, 0, 0 }
};

const struct cmdline_options_s main_cmdline_options[] = {
    { "-new", 1,
      game_opt_do_new_seed, 0,
      "GAMESEED", "Start new game using given game seed\n"
                  "GAMESEED is OPT[:RACES[:BANNERS[:GSEED[:HUMANS]]]]\n"
                  "OPT is PLAYERS*100+GALAXYSIZE*10+DIFFICULTY\n  2..6, 0..3 = small..huge, 0..4 = simple..impossible\n  default same as last new game\n"
                  "RACES is PLAYERnRACE*(0x10^n), n=0..5\n  0 = random, 1..0xA = human..darlok\n  default 0 (all random)\n"
                  "BANNERS is PLAYERnBANNER*(10^n), n=0..5\n  0 = random, 1..6 = blue..yellow\n  default 0 (all random)\n"
                  "GSEED is a 32 bit galaxy seed or 0 for random\n  default 0\n"
                  "HUMANS is PLAYERnISHUMAN*(10^n), n=0..5\n  default 1 (player 1 is human, others AI)"
    },
    { "-mphost", 1,
      options_set_int_var, (void *)&opt_mp_host_port,
      "PORT", "Host a multiplayer game (authoritative server) on PORT" },
    { "-mpjoin", 1,
      options_set_str_var, (void *)&opt_mp_join,
      "HOST[:PORT]", "Join a multiplayer game as a client" },
    { "-mphumans", 1,
      options_set_int_var, (void *)&opt_mp_humans,
      "N", "Multiplayer (host): number of human players to wait for (default 1; rest of -new empires are AI)" },
    { "-mpload", 1,
      options_set_str_var, (void *)&opt_mp_load,
      "FILE", "Multiplayer (host): resume a saved game from FILE (e.g. the autosave mp_autosave.blob in the user dir) instead of starting a new one" },
    { "-ngn", 2,
      game_opt_set_new_name, 0,
      "PLAYER NAME", "Set new game emperor name for player 1..6" },
    { "-ngh", 2,
      game_opt_set_new_home, 0,
      "PLAYER NAME", "Set new game home world name for player 1..6" },
    { "-nga", 1,
      game_opt_set_new_ai, 0,
      "AITYPE", "Set new game AI type (0..1)" },
    { "-load", 1,
      game_opt_do_load, 0,
      "SAVE", "Load game (1..8, 2300.. or filename)\n1..6 are regular save slots\n7 is continue game\n8 is undo\n2300 and over are yearly saves" },
    { "-continue", 0,
      game_opt_do_continue, 0,
      NULL, "Continue game" },
    { "-undo", 0,
      options_enable_bool_var, (void *)&game_opt_undo_enabled,
      NULL, "Enable undo saves" },
    { "-noundo", 0,
      options_disable_bool_var, (void *)&game_opt_undo_enabled,
      NULL, "Disable undo saves" },
    { "-yearsave", 0,
      options_enable_bool_var, (void *)&game_opt_year_save_enabled,
      NULL, "Enable yearly saves" },
    { "-noyearsave", 0,
      options_disable_bool_var, (void *)&game_opt_year_save_enabled,
      NULL, "Disable yearly saves" },
    { "-skipintro", 0,
      options_enable_bool_var, (void *)&game_opt_skip_intro_always,
      NULL, "Skip intro" },
    { "-noskipintro", 0,
      options_disable_bool_var, (void *)&game_opt_skip_intro_always,
      NULL, "Do not skip intro" },
    { "-nextturn", 0,
      options_enable_bool_var, (void *)&game_opt_next_turn,
      NULL, "Go directly to next turn (for reproducing bugs)" },
    { "-savequit", 0,
      options_enable_bool_var, (void *)&game_opt_save_quit,
      NULL, "Save and quit (for debugging)" },
    { 0, 0, 0, 0, 0, 0 }
};

/* -------------------------------------------------------------------------- */

static bool game_cfg_check_difficulty_value(void *val)
{
    int v = (int)(intptr_t)val;
    if (v >= DIFFICULTY_NUM) {
        log_error("invalid difficulty num %i\n", v);
        return false;
    }
    return true;
}

static bool game_cfg_check_galaxy_size_value(void *val)
{
    int v = (int)(intptr_t)val;
    if (v >= GALAXY_SIZE_NUM) {
        log_error("invalid galaxy size num %i\n", v);
        return false;
    }
    return true;
}

static bool game_cfg_check_players_value(void *val)
{
    int v = (int)(intptr_t)val;
    if ((v < 2) || (v > PLAYER_NUM)) {
        log_error("invalid players num %i\n", v);
        return false;
    }
    return true;
}

static bool game_cfg_check_custom_game_ai_id(void *val)
{
    int v = (int)(intptr_t)val;
    if (v >= GAME_AI_NUM || v < 0) {
        log_error("invalid ai id %i\n", v);
        return false;
    }
    return true;
}

static bool game_cfg_check_new_game_opts(void *val)
{
    int v2, v = (int)(intptr_t)val;
    v2 = v % 10;
    v = v / 10;
    if (v2 >= DIFFICULTY_NUM) {
        log_error("invalid difficulty num %i\n", v2);
        return false;
    }
    v2 = v % 10;
    v = v / 10;
    if (v2 >= GALAXY_SIZE_NUM) {
        log_error("invalid galaxy size num %i\n", v2);
        return false;
    }
    v2 = v % 10;
    if ((v2 < 2) || (v2 > PLAYER_NUM)) {
        log_error("invalid players num %i\n", v2);
        return false;
    }
    return true;
}

static bool game_cfg_check_race_value(void *val)
{
    int v2, v = (int)(intptr_t)val;
    for (int i = 0; i < PLAYER_NUM; ++i) {
        v2 = v % 0x10;
        v = v / 0x10;
        if (v2 > RACE_RANDOM) {
            log_error("invalid race num %i\n", v2);
            return false;
        }
    }
    return true;
}

static bool game_cfg_check_banner_value(void *val)
{
    int v2, v = (int)(intptr_t)val;
    for (int i = 0; i < PLAYER_NUM; ++i) {
        v2 = v % 10;
        v = v / 10;
        if (v2 > BANNER_RANDOM) {
            log_error("invalid banner num %i\n", v2);
            return false;
        }
    }
    return true;
}

static bool game_cfg_check_isai_value(void *val)
{
    int v2, v = (int)(intptr_t)val;
    for (int i = 0; i < PLAYER_NUM; ++i) {
        v2 = v % 10;
        v = v / 10;
        if (v2 > 1) {
            log_error("invalid isai num %i\n", v2);
            return false;
        }
    }
    return true;
}

const struct cfg_items_s game_cfg_items[] = {
    CFG_ITEM_BOOL("undo", &game_opt_undo_enabled),
    CFG_ITEM_BOOL("yearsave", &game_opt_year_save_enabled),
    CFG_ITEM_BOOL("initsave", &game_opt_init_save_enabled),
    CFG_ITEM_BOOL("skipintro", &game_opt_skip_intro_always),
    CFG_ITEM_BOOL("skiprandomnews", &game_opt_skip_random_news),
    CFG_ITEM_BOOL("news_orion_colonized", &game_num_news_orion),
    CFG_ITEM_COMMENT("PLAYERS*100+GALAXYSIZE*10+DIFFICULTY"),
    CFG_ITEM_COMMENT(" 2..6, 0..3 = small..huge, 0..4 = simple..impossible"),
    CFG_ITEM_INT("new_game_opts", &game_opt_new_value, game_cfg_check_new_game_opts),
    CFG_ITEM_INT("custom_game_ai_id", &game_opt_custom.ai_id, game_cfg_check_custom_game_ai_id),
    CFG_ITEM_INT("custom_game_difficulty", &game_opt_custom.difficulty, game_cfg_check_difficulty_value),
    CFG_ITEM_INT("custom_game_galaxy_size", &game_opt_custom.galaxy_size, game_cfg_check_galaxy_size_value),
    CFG_ITEM_INT("custom_game_players", &game_opt_custom.players, game_cfg_check_players_value),
    CFG_ITEM_INT("custom_game_races", &game_opt_custom_race_value, game_cfg_check_race_value),
    CFG_ITEM_INT("custom_game_banners", &game_opt_custom_banner_value, game_cfg_check_banner_value),
    CFG_ITEM_INT("custom_game_isai", &game_opt_custom_isai_value, game_cfg_check_isai_value),
    CFG_ITEM_INT("custom_game_galaxy_seed", &game_opt_custom.galaxy_seed, NULL),
    CFG_ITEM_BOOL("custom_game_improved_galaxy_generator", &game_opt_custom.improved_galaxy_generator),
    CFG_ITEM_BOOL("custom_game_nebulae", &game_opt_custom.nebulae),
    CFG_ITEM_INT("custom_game_home_max_pop", &game_opt_custom.homeworlds.max_pop, NULL),
    CFG_ITEM_INT("custom_game_home_special", &game_opt_custom.homeworlds.special, NULL),
    CFG_ITEM_INT("custom_game_home_num_dist_checks", &game_opt_custom.homeworlds.num_dist_checks, NULL),
    CFG_ITEM_INT("custom_game_home_num_ok_planet_checks", &game_opt_custom.homeworlds.num_ok_planet_checks, NULL),
    CFG_ITEM_INT("custom_game_home_num_scouts", &game_opt_custom.homeworlds.num_scouts, NULL),
    CFG_ITEM_INT("custom_game_home_num_fighters", &game_opt_custom.homeworlds.num_fighters, NULL),
    CFG_ITEM_INT("custom_game_home_num_colony_ships", &game_opt_custom.homeworlds.num_colony_ships, NULL),
    CFG_ITEM_BOOL("custom_game_home_armed_colony_ships", &game_opt_custom.homeworlds.armed_colony_ships),
    CFG_ITEM_BOOL("msg_filter_fact", &game_opt_message_filter[FINISHED_FACT]),
    CFG_ITEM_BOOL("msg_filter_popmax", &game_opt_message_filter[FINISHED_POPMAX]),
    CFG_ITEM_BOOL("msg_filter_soilatmos", &game_opt_message_filter[FINISHED_SOILATMOS]),
    CFG_ITEM_BOOL("msg_filter_stargate", &game_opt_message_filter[FINISHED_STARGATE]),
    CFG_ITEM_BOOL("msg_filter_shield", &game_opt_message_filter[FINISHED_SHIELD]),
    CFG_ITEM_INT("rules_tech_cost_mul", &game_num_tech_costmul, NULL),
    CFG_ITEM_INT("rules_tech_cost_mul_human", &game_num_tech_costmuld2, NULL),
    CFG_ITEM_INT("rules_tech_cost_mul_ai", &game_num_tech_costmula2, NULL),
    CFG_ITEM_BOOL("rules_ai_transport_range_fix", &game_num_ai_trans_range_fix),
    CFG_ITEM_BOOL("rules_ai_first_tech_cost_fix", &game_num_ai_first_tech_cost_fix),
    CFG_ITEM_BOOL("rules_doom_stack_fix", &game_num_doom_stack_fix),
    CFG_ITEM_BOOL("rules_bt_no_tohit_acc", &game_num_bt_no_tohit_acc),
    CFG_ITEM_BOOL("rules_bt_precap_tohit", &game_num_bt_precap_tohit),
    CFG_ITEM_BOOL("rules_bt_wait_no_reload", &game_num_bt_wait_no_reload),
    CFG_ITEM_BOOL("rules_deterministic_rng", &game_num_deterministic),
    CFG_ITEM_BOOL("rules_hidden_child_labor_fix", &game_num_hidden_child_labor_fix),
    CFG_ITEM_BOOL("rules_leaving_trans_fix", &game_num_leaving_trans_fix),
    CFG_ITEM_BOOL("rules_monster_rest_attack", &game_num_monster_rest_att),
    CFG_ITEM_BOOL("rules_factory_cost_fix", &game_num_factory_cost_fix),
    CFG_ITEM_BOOL("rules_colonized_factories_fix", &game_num_colonized_factories_fix),
    CFG_ITEM_BOOL("rules_newtech_adjust_fix", &game_num_newtech_adjust_fix),
    CFG_ITEM_BOOL("rules_slider_eco_done_fix", &game_num_slider_eco_done_fix),
    CFG_ITEM_BOOL("rules_cond_switch_to_ind_fix", &game_num_cond_switch_to_ind_fix),
    CFG_ITEM_BOOL("rules_waste_adjust_fix", &game_num_waste_adjust_fix),
    CFG_ITEM_BOOL("rules_orbital_bio_fix", &game_num_orbital_bio_fix),
    CFG_ITEM_BOOL("rules_orbital_weap_4", &game_num_orbital_weap_4),
    CFG_ITEM_BOOL("rules_orbital_comp_fix", &game_num_orbital_comp_fix),
    CFG_ITEM_BOOL("rules_retreat_redir_fix", &game_num_retreat_redir_fix),
    CFG_ITEM_BOOL("rules_ship_stargate_redir_fix", &game_num_stargate_redir_fix),
    CFG_ITEM_BOOL("rules_ship_trans_redir_fix", &game_num_trans_redir_fix),
    CFG_ITEM_BOOL("rules_soil_rounding_fix", &game_num_soil_rounding_fix),
    CFG_ITEM_BOOL("rules_waste_calc_fix", &game_num_waste_calc_fix),
    CFG_ITEM_BOOL("rules_ai_fleet_cheating_fix", &game_num_ai_fleet_cheating_fix),
    CFG_ITEM_BOOL("rules_fix_bugs", &game_opt_fix_bugs),
    CFG_ITEM_BOOL("rules_fix_starting_ships", &game_opt_fix_starting_ships),
    CFG_ITEM_END
};

/* -------------------------------------------------------------------------- */

int main_handle_option(const char *argv)
{
    if (game_opt_end.type == GAME_END_NONE) {
        if ((argv[1] == '\0') && (argv[0] >= '0') && (argv[0] <= '3')) {
            switch (argv[0]) {
                case '0':
                    game_opt_end.type = GAME_END_WON_GOOD;
                    break;
                case '1':
                    game_opt_end.type = GAME_END_LOST_FUNERAL;
                    break;
                case '2':
                    game_opt_end.type = GAME_END_LOST_EXILE;
                    break;
                case '3':
                    game_opt_end.type = GAME_END_WON_TYRANT;
                    break;
            }
            game_opt_load_game = 0;
            game_opt_load_fname = 0;
            game_opt_new_game = false;
            game_opt_skip_intro = true;
            game_opt_continue = false;
            return 0;
        } else if (strcmp(argv, "YOMAMA") == 0) {
            log_message("Game: skip intro for YOMAMA\n");
            game_opt_skip_intro = true;
            return 0;
        } else if (strcmp(argv, "s") == 0) {
            log_message("Game: direct continue\n");
            game_opt_load_game = 0;
            game_opt_load_fname = 0;
            game_opt_new_game = false;
            game_opt_skip_intro = true;
            game_opt_continue = true;
            return 0;
        }
    } else {
        if (game_opt_end.varnum == 0) {
            switch (game_opt_end.type) {
                case GAME_END_LOST_EXILE:
                    game_opt_end.name = argv;
                    break;
                default:
                    game_opt_end.race = atoi(argv);
                    break;
            }
        } else if (game_opt_end.varnum == 1) {
            switch (game_opt_end.type) {
                case GAME_END_WON_GOOD:
                case GAME_END_WON_TYRANT:
                    game_opt_end.name = argv;
                    log_message("Game: ending %s %i '%s'\n", (game_opt_end.type == GAME_END_WON_GOOD) ? "good" : "tyrant", game_opt_end.race, game_opt_end.name);
                    break;
                case GAME_END_LOST_FUNERAL:
                    game_opt_end.banner_dead = atoi(argv);
                    log_message("Game: ending funeral %i %i\n", game_opt_end.race, game_opt_end.banner_dead);
                    break;
                case GAME_END_LOST_EXILE:
                    log_message("Game: ending exile '%s'\n", game_opt_end.name);
                    break;
                default:
                    break;
            }
        } else {
            return -1;
        }
        ++game_opt_end.varnum;
        return 0;
    }
    return -1;
}

/* ----- 1oom-mp: server/client entry points (Phase A) ----- */
/* The game is accessed by mp.c through these engine-adapter callbacks. GAME_DATA
   over the wire is a save blob; advancing a turn is game_turn_process. */
static int mp_if_serialize(void *ctx, uint8_t *buf, int buflen) { return game_save_blob_save((struct game_s *)ctx, buf, buflen); }
static int mp_if_deserialize(void *ctx, const uint8_t *buf, int len) { return game_save_blob_load((struct game_s *)ctx, buf, len); }
/* 1oom-mp: autosave the just-resolved game so a server crash or restart never loses it. Written every
   turn in the same blob format the GAME_DATA sync uses, so the server can resume from it (-mpload).
   Path: <user dir>/mp_autosave.blob. Cheap (~one save-sized write per turn). */
static void game_mp_autosave(struct game_s *g) {
    static uint8_t *buf = NULL;
    static int cap = 0;
    int need, n;
    char fname[1024];
    FILE *f;
    need = game_save_blob_maxlen(g);
    if (need > cap) {
        uint8_t *nb = (uint8_t *)realloc(buf, (size_t)need);
        if (!nb) { return; }
        buf = nb; cap = need;
    }
    n = game_save_blob_save(g, buf, cap);
    if (n <= 0) { return; }
    os_make_path_user();
    lib_sprintf(fname, sizeof(fname), "%s/mp_autosave.blob", os_get_path_user());
    f = fopen(fname, "wb");
    if (f) {
        int humans = s_mp_humans;
        fwrite(&humans, sizeof(humans), 1, f); /* header: human-client count, so -mpload knows how many to wait for */
        fwrite(buf, 1, (size_t)n, f);
        fclose(f);
    }
}

static int mp_if_advance(void *ctx) {
    struct game_s *g = (struct game_s *)ctx;
    if (getenv("MP_PINGTEST") && g_mp_decision_hook) {
        static bool pinged = false;
        if (!pinged) { /* MP-DBG: prove the decision channel round-trips (client returns req+1) */
            uint8_t rq = 7, rsp = 0;
            int r = g_mp_decision_hook(0, MP_DEC_PING, &rq, 1, &rsp, 1);
            log_message("MP-DBG ping: sent 7 -> got %d (rlen=%d, expect 8)\n", rsp, r);
            pinged = true;
        }
    }
    /* apply colonize requests BEFORE the turn resolves so the new colony plays this turn */
    game_mp_colonize_apply_pending(g);
    struct game_end_s ge = game_turn_process(g);
    if (getenv("MP_BIGRANGE")) { /* test: make every empire's ships reach anywhere, for quick combat */
        for (player_id_t i = PLAYER_0; i < g->players; ++i) { g->eto[i].fuel_range = 30; }
    }
    if (getenv("MP_CONTACT")) { /* test: bump fuel range so everyone is in contact. The Races
        screen recomputes contact by range (game_update_empire_contact), so forcing the contact
        bit doesn't stick -- making empires genuinely in-range does. */
        for (player_id_t i = PLAYER_0; i < g->players; ++i) { if (g->eto[i].fuel_range < 200) { g->eto[i].fuel_range = 200; } }
    }
    if (getenv("MP_AITALK")) { /* test: once, drive an audience with the AI for each human so the
        relay is exercised deterministically (each human sees the AI leader + treaty options). */
        static bool aitalk_done = false;
        if (!aitalk_done) {
            aitalk_done = true;
            player_id_t ai = PLAYER_NONE;
            for (player_id_t i = PLAYER_0; i < g->players; ++i) { if (IS_AI(g, i)) { ai = i; break; } }
            if (ai != PLAYER_NONE) {
                for (player_id_t h = PLAYER_0; h < g->players; ++h) {
                    if (IS_HUMAN(g, h)) { game_audience(g, h, ai); }
                }
            }
        }
    }
    /* apply diplomacy actions AFTER turn processing (it clears queued proposals) */
    game_mp_diplo_apply_pending(g);
    game_mp_autosave(g); /* persist the resolved turn so a crash/restart can resume it */
    /* return >0 to tell mp_server_run the game has ended (someone won/lost) */
    return ((ge.type == GAME_END_NONE) || (ge.type == GAME_END_FINAL_WAR)) ? 0 : 1;
}
static int mp_if_turn(void *ctx) { return ((struct game_s *)ctx)->year; }
static int mp_if_maxblob(void *ctx) { return game_save_blob_maxlen((struct game_s *)ctx); }

/* --- turn-movement playback: the server snapshots state at movement-start (via
   game_mp_premove_hook), then streams it so clients replay game_turn_move_ships. --- */
static uint8_t *s_mp_premove_buf = NULL;
static int s_mp_premove_cap = 0;
static int s_mp_premove_len = 0;

/* server: called mid-resolution (from the null UI) when fleet movement begins */
static void mp_premove_capture(struct game_s *g) {
    if (s_mp_premove_buf) {
        int n = game_save_blob_save(g, s_mp_premove_buf, s_mp_premove_cap);
        s_mp_premove_len = (n > 0) ? n : 0;
        /* 1oom-mp: stream the movement replay NOW (at movement-start), BEFORE this turn's combat /
           bomb / ground result screens fire later in resolution, so the client animates fleet
           movement first and the results follow in logical order. (Falls back to the post-advance
           get_movement send if no broadcast hook is set, e.g. standalone tests.) */
        if ((s_mp_premove_len > 0) && g_mp_movement_hook) {
            g_mp_movement_hook(s_mp_premove_buf, s_mp_premove_len);
            s_mp_premove_len = 0; /* sent; stop get_movement from re-sending it after advance_turn */
        }
    }
}

/* server: hand the captured pre-movement snapshot to mp_server_run for streaming */
static int mp_if_get_movement(void *ctx, uint8_t *buf, int buflen) {
    (void)ctx;
    int n = s_mp_premove_len;
    s_mp_premove_len = 0; /* consume */
    if (n <= 0 || n > buflen || !s_mp_premove_buf) { return 0; }
    memcpy(buf, s_mp_premove_buf, n);
    return n;
}

/* client: load the pre-movement state and replay the movement animation. The
   authoritative GAME_DATA follows immediately and overwrites this transient state. */
static void mp_if_play_movement(void *ctx, int player_id, const uint8_t *buf, int len) {
    struct game_s *g = (struct game_s *)ctx;
    if (game_save_blob_load(g, buf, len) != 0) { return; }
    /* Animate from THIS client's perspective only: game_turn_move_ships runs one
       animation pass per non-AI empire, so treat every other empire as AI for the
       replay (the authoritative GAME_DATA load right after restores the real flags). */
    for (player_id_t i = PLAYER_0; i < g->players; ++i) {
        if (i == (player_id_t)player_id) { BOOLVEC_SET0(g->is_ai, i); } else { BOOLVEC_SET1(g->is_ai, i); }
    }
    game_turn_move_ships(g);
}

/* set by the combat dispatch while a battle UI is live on this client */
static void *s_mp_battle_uictx = NULL;
static bool s_mp_battle_auto = false; /* this client auto-resolved/retreated its battle: ctx is half-built (no gfx_bg loaded), so it must NOT be drawn as a spectate arena */
/* set while a relayed AI<->human audience UI is live on this client (so on_wait keeps its screen) */
static void *s_mp_audience_uictx = NULL;
static struct audience_s s_mp_audience; /* client's working copy of the relayed audience */
/* the client's working copy of the streamed battle (declared here so mp_if_on_wait, below,
   can render the spectate/examine view from it). battle_s isn't fully self-contained, so we
   re-establish g + sprite pointers (read directly each draw) after every memcpy. */
static struct battle_s s_mp_battle;

/* 1oom-mp: turn-summary news relayed from the server (already filtered to this player). Buffered as
   it streams in during the server's turn resolution, then replayed as one GNN broadcast when the
   new turn's state loads (mp_if_on_state_loaded). */
struct mp_news_item_s { int32_t type, subtype, num1, num2, race, planet_i; };
static struct mp_news_item_s s_mp_news[64];
static int s_mp_news_n = 0;

/* 1oom-mp: ground invasions this client took part in, buffered as they stream in during the server's
   turn resolution, then replayed at the next state load. Replaying client-side (instead of blocking
   the server on each) lets every player's independent invasions play at the same wall-clock time
   rather than queuing behind each other. ui_ground renders purely from the ground_s + static data, so
   replaying against post-invasion state is safe. */
static struct ground_s s_mp_ground[16];
static int s_mp_ground_n = 0;

/* 1oom-mp: orbital bombings, same buffer+replay treatment as ground (concurrent across players). The
   planet snapshot is patched in at replay so ui_bomb_show sees the bomb-time owner, not a later one. */
struct mp_bomb_item_s { int32_t attacker, owner, planet, popdmg, factdmg; uint8_t play_music, hide_other; planet_t psnap; };
/* 1oom-mp: orbital bombings buffered as they stream in during resolution, replayed concurrently at the
   next state load -- so the bomb phase never blocks one player on another's bombardment screens. */
static struct mp_bomb_item_s s_mp_bomb[16];
static int s_mp_bomb_n = 0;

/* 1oom-mp: espionage result streams, buffered as they stream in during resolution and replayed
   concurrently at the next state load -- so neither the tech-theft victim notice nor the saboteur's
   result screen blocks one player on the other's espionage. ui_spy_stolen needs no planet snapshot
   (it shows only race + tech); the sabotage result swaps in a post-sabotage planet snapshot at replay
   like ui_bomb_show does. The blocking framing choice (other2 != PLAYER_NONE) is NOT buffered. */
struct mp_stolen_item_s { int32_t spy, field; uint8_t tech; };
static struct mp_stolen_item_s s_mp_stolen[16];
static int s_mp_stolen_n = 0;
struct mp_sabres_item_s { int32_t spy, target, act, other1, other2, snum, planet; planet_t psnap; };
static struct mp_sabres_item_s s_mp_sabres[16];
static int s_mp_sabres_n = 0;

/* 1oom-mp: hysteresis for the "waiting for other players" banner. Counts consecutive on_wait ticks
   since the last relayed event; the banner only appears once this exceeds a threshold, so the brief
   gaps between events don't flash it on and off. Reset to 0 on every relayed decision. */
static int s_mp_wait_quiet = 0;

/* 1oom-mp: shared synchronous council view. The server streams the council chamber (election_s) to
   ALL humans (UI_MP_SPEC_COUNCIL), so everyone watches the votes instead of a black/banner screen.
   The council artwork is a shared resource (LBXFILE_COUNCIL): load it ONCE here and free it ONCE
   (council-end / turn-advance); the voting client reuses this ctx rather than load/free its own,
   which would release the shared file out from under the spectate render. */
static struct election_s s_mp_election;
static bool s_mp_council_active = false;
static void *s_mp_election_ctx = NULL;
static char s_mp_election_str[256]; /* 1oom-mp: client copy of the streamed council announcement text (el->str is a server pointer; the bytes ride after the struct -- see mp_if_on_spectate) */

static void mp_council_ctx_ensure(struct game_s *g) {
    if (!s_mp_election_ctx) {
        s_mp_election.g = g;
        ui_election_ctx_load(&s_mp_election); /* loads council gfx + palette, sets uictx */
        s_mp_election_ctx = s_mp_election.uictx;
    }
}
static void mp_council_ctx_free(void) {
    if (s_mp_election_ctx) { ui_election_ctx_free(&s_mp_election); s_mp_election_ctx = NULL; }
    s_mp_council_active = false;
}

/* client: pump events while blocked. During a battle, keep the arena on screen
   instead of the black "waiting" frame (the other player's turn would otherwise
   blank our battle view, then flip back). */
/* keep the "another player is in combat" notice up for a short while even after the battle ends, so
   a quick / auto-resolved fight isn't an imperceptible on/off flash. Counted in on_wait iterations
   (~1ms each), so ~1500 ≈ 1.5s. */
static int s_mp_combat_hold = 0;
static void mp_if_on_wait(void *ctx, int reason) {
    (void)ctx;
    if (s_mp_battle_uictx && !s_mp_battle_auto) {
        /* I'm an interactive participant waiting on the other side's move: render one examine frame
           (banner + examine-only cursor) from the last streamed battle state, instead of
           a passive black "waiting" screen. on_spectate keeps s_mp_battle fresh. NOTE: only for
           interactively-fought battles -- an auto-resolved/retreated battle leaves the ctx half-built
           (no gfx_bg), so spectating it would draw a NULL background and segfault; those fall through
           to the combat "waiting" banner below. */
        s_mp_battle.uictx = s_mp_battle_uictx;
        ui_mp_battle_spectate(&s_mp_battle);
        return;
    }
    if (s_mp_council_active && s_mp_election_ctx) {
        /* a council is in session and we have the streamed chamber state: render it (the shared
           synchronous council view) rather than a waiting banner. */
        s_mp_election.uictx = s_mp_election_ctx;
        ui_election_spectate(&s_mp_election);
        return;
    }
    if (reason == MP_WAIT_COUNCIL) { s_mp_combat_hold = 0; } /* the council banner (fallback if no chamber stream) takes priority over a stale combat notice */
    if (reason == MP_WAIT_COMBAT) { s_mp_combat_hold = 1500; } /* another player just started/continued a battle */
    if (s_mp_combat_hold > 0) {
        --s_mp_combat_hold;
        ui_mp_wait(MP_WAIT_COMBAT); /* show the combat notice (sticky; also takes priority over a stale audience screen) */
        return;
    }
    if (s_mp_audience_uictx) { ui_mp_wait(MP_WAIT_BATTLE); return; } /* mid AI-audience: keep its screen, just pump */
    if (++s_mp_wait_quiet < 700) {
        ui_mp_wait(MP_WAIT_BATTLE); /* brief gap between relayed events: just pump, keep the screen up (no banner flash) */
    } else {
        ui_mp_wait(reason); /* sustained wait -> show the banner */
    }
}

/* interactive combat: the client renders a battle from the server's streamed
   battle_s and drives the real battle UI. battle_s isn't fully self-contained, so we
   re-establish g + sprite pointers (read directly each draw) after every memcpy. */

static void mp_battle_fixup(struct game_s *g, struct battle_s *bt) {
    bt->g = g;
    for (int i = 0; (i <= bt->items_num) && (i < BATTLE_ITEM_MAX); ++i) {
        struct battle_item_s *b = &bt->item[i];
        /* the planet (a defending owned colony) is always item[0]; identify it by bt->planet_side,
           NOT b->side -- the planet item's side is left at 0 (SIDE_L) by memset (game_battle_item_add
           never sets it for the SIDE_NONE/planet case), so "b->side == SIDE_NONE" is never true for it
           and the planet would otherwise be re-gfx'd as a ship (the colony-ship sprite). */
        bool is_planet = (i == 0) && (bt->planet_side != SIDE_NONE);
        b->gfx = is_planet ? ui_gfx_get_planet(b->look) : ui_gfx_get_ship(b->look);
    }
    for (int i = 0; (i < bt->num_rocks) && (i < BATTLE_ROCK_MAX); ++i) {
        bt->rock[i].gfx = ui_gfx_get_rock((bt->rock[i].sx + bt->rock[i].sy) & 3); /* cosmetic */
    }
}

/* client: answer a mid-resolution interactive decision the server is blocking on.
   Dispatches by type to the matching real UI; returns the response byte length. */
static int mp_if_handle_decision(void *ctx, int dtype, const uint8_t *req, int req_len, uint8_t *resp, int resp_buflen) {
    struct game_s *g = (struct game_s *)ctx;
    s_mp_wait_quiet = 0; /* a relayed event just arrived -> hold off the "waiting" banner briefly (hysteresis) */
    switch (dtype) {
        case MP_DEC_PING: /* synthetic round-trip self-test: return req[0]+1 */
            if (resp_buflen >= 1) { resp[0] = (req_len > 0) ? (uint8_t)(req[0] + 1) : 0x42; return 1; }
            return 0;
        case MP_DEC_BOMB: { /* "bomb this planet?" -> the real bomb prompt */
            if ((req_len < 8) || (resp_buflen < 1)) { return 0; }
            int bpi = (req[0] << 8) | req[1];
            planet_id_t bplanet = (planet_id_t)((req[2] << 8) | req[3]);
            int pop_inbound = (int)(((uint32_t)req[4] << 24) | ((uint32_t)req[5] << 16) | ((uint32_t)req[6] << 8) | req[7]);
            resp[0] = ui_bomb_ask(g, bpi, bplanet, pop_inbound) ? 1 : 0;
            return 1;
        }
        case MP_DEC_BOMB_SHOW: { /* buffer the bombing result (instant ack, never blocks the server);
                                    replayed concurrently at state load so both players' bombings play at
                                    the same wall-clock time instead of one waiting on the other's screens */
            if (req_len < (int)sizeof(struct mp_bomb_item_s)) { return 0; }
            if (s_mp_bomb_n < (int)(sizeof(s_mp_bomb) / sizeof(s_mp_bomb[0]))) {
                memcpy(&s_mp_bomb[s_mp_bomb_n++], req, sizeof(struct mp_bomb_item_s));
            }
            if (resp_buflen >= 1) { resp[0] = 1; }
            return 1;
        }
        case MP_DEC_BOMB_BATCH: { /* parallel batched bombing: prompt for MY targets in this turn's list
                                     and return a yes/no bitmask. Both players answer at the same time. */
            int32_t n;
            uint64_t mask = 0;
            int me = mp_cl_player_id();
            if (req_len < 4) { return 0; }
            memcpy(&n, req, 4);
            if ((n < 0) || (n > 64)) { return 0; }
            if (req_len < (int)(4 + n * (int)sizeof(struct ui_bomb_target_s))) { return 0; }
            {
                const struct ui_bomb_target_s *tgt = (const struct ui_bomb_target_s *)(req + 4);
                for (int k = 0; k < n; ++k) {
                    if ((int)tgt[k].attacker == me) {
                        if (ui_bomb_ask(g, me, (planet_id_t)tgt[k].planet_i, tgt[k].pop_inbound)) { mask |= ((uint64_t)1 << k); }
                    }
                }
            }
            if (resp_buflen >= 8) { memcpy(resp, &mask, 8); return 8; }
            return 0;
        }
        case MP_DEC_SPY_SABOTAGE_BATCH: { /* parallel batched sabotage: prompt ui_spy_sabotage_ask for MY
                                             opportunities in this turn's list and return ui_spy_sab_dec_s[n]
                                             (only my own slots filled). Both players answer at once. */
            int32_t n;
            int me = mp_cl_player_id();
            if (req_len < 4) { return 0; }
            memcpy(&n, req, 4);
            if ((n < 0) || (n > 32)) { return 0; }
            if (req_len < (int)(4 + n * (int)sizeof(struct ui_spy_sab_target_s))) { return 0; }
            {
                const struct ui_spy_sab_target_s *tgt = (const struct ui_spy_sab_target_s *)(req + 4);
                int rlen = n * (int)sizeof(struct ui_spy_sab_dec_s);
                if (resp_buflen < rlen) { return 0; }
                for (int k = 0; k < n; ++k) {
                    struct ui_spy_sab_dec_s d;
                    d.act = (int16_t)UI_SABOTAGE_NONE;
                    d.planet = PLANET_NONE;
                    if ((int)tgt[k].player == me) {
                        planet_id_t planet = PLANET_NONE;
                        ui_sabotage_t act = ui_spy_sabotage_ask(g, me, tgt[k].target, &planet);
                        d.act = (int16_t)act;
                        d.planet = (uint16_t)planet;
                    }
                    memcpy(resp + k * (int)sizeof(struct ui_spy_sab_dec_s), &d, sizeof(d));
                }
                return rlen;
            }
        }
        case MP_DEC_SPY_STEAL_BATCH: { /* parallel batched tech-steal: prompt ui_spy_steal for MY
                                          opportunities in this turn's list and return int16_t field[n]
                                          (only my own slots filled). Both players answer at once. */
            int32_t n;
            int me = mp_cl_player_id();
            if (req_len < 4) { return 0; }
            memcpy(&n, req, 4);
            if ((n < 0) || (n > 32)) { return 0; }
            if (req_len < (int)(4 + n * (int)sizeof(struct ui_spy_steal_target_s))) { return 0; }
            {
                const struct ui_spy_steal_target_s *tgt = (const struct ui_spy_steal_target_s *)(req + 4);
                int rlen = n * (int)sizeof(int16_t);
                if (resp_buflen < rlen) { return 0; }
                for (int k = 0; k < n; ++k) {
                    int16_t field = -1;
                    if ((int)tgt[k].spy == me) {
                        field = (int16_t)ui_spy_steal(g, me, tgt[k].target, tgt[k].flags);
                    }
                    memcpy(resp + k * (int)sizeof(int16_t), &field, sizeof(field));
                }
                return rlen;
            }
        }
        case MP_DEC_GROUND: { /* buffer a ground-invasion result; replayed at state load so independent invasions play concurrently, not in series */
            if (req_len < (int)sizeof(struct ground_s)) { return 0; }
            if (s_mp_ground_n < (int)(sizeof(s_mp_ground) / sizeof(s_mp_ground[0]))) {
                memcpy(&s_mp_ground[s_mp_ground_n++], req, sizeof(struct ground_s));
            }
            if (resp_buflen >= 1) { resp[0] = 1; } /* instant ack -- the screen plays later, so the server never blocks on it */
            return 1;
        }
        case MP_DEC_SPY_STEAL: { /* which tech field to steal */
            struct { int32_t spy, target; uint8_t flags; } q;
            if ((req_len < (int)sizeof(q)) || (resp_buflen < (int)sizeof(int32_t))) { return 0; }
            memcpy(&q, req, sizeof(q));
            int32_t field = ui_spy_steal(g, q.spy, q.target, (uint8_t)q.flags);
            memcpy(resp, &field, sizeof(field));
            return (int)sizeof(field);
        }
        case MP_DEC_SPY_SABOTAGE: { /* what to sabotage + which planet */
            struct { int32_t spy, target; } q;
            struct { int32_t act; uint16_t planet; } rs;
            if ((req_len < (int)sizeof(q)) || (resp_buflen < (int)sizeof(rs))) { return 0; }
            memcpy(&q, req, sizeof(q));
            planet_id_t planet = PLANET_NONE;
            rs.act = (int32_t)ui_spy_sabotage_ask(g, q.spy, q.target, &planet);
            rs.planet = (uint16_t)planet;
            memcpy(resp, &rs, sizeof(rs));
            return (int)sizeof(rs);
        }
        case MP_DEC_SPY_RESULT: { /* show the sabotage result; patch in the post-sabotage planet snapshot */
            struct { int32_t spy, target, act, other1, other2, snum, planet; planet_t psnap; } q;
            if ((req_len < (int)sizeof(q)) || (resp_buflen < (int)sizeof(int32_t))) { return 0; }
            memcpy(&q, req, sizeof(q));
            planet_id_t pl = (planet_id_t)q.planet;
            if (pl >= g->galaxy_stars) { return 0; }
            planet_t saved = g->planet[pl];
            g->planet[pl] = q.psnap; /* the next GAME_DATA re-syncs this; we restore below regardless */
            int32_t other = (int32_t)ui_spy_sabotage_done(g, mp_cl_player_id(), q.spy, q.target, (ui_sabotage_t)q.act, q.other1, q.other2, pl, q.snum);
            g->planet[pl] = saved;
            memcpy(resp, &other, sizeof(other));
            return (int)sizeof(other);
        }
        case MP_DEC_SPY_STOLEN: { /* buffer the "tech stolen from you" notice; replayed at state load so the
                                     victim's screen plays concurrently and never blocks resolution */
            struct mp_stolen_item_s it;
            if (req_len < (int)sizeof(it)) { return 0; }
            memcpy(&it, req, sizeof(it));
            if (s_mp_stolen_n < (int)(sizeof(s_mp_stolen) / sizeof(s_mp_stolen[0]))) {
                s_mp_stolen[s_mp_stolen_n++] = it;
            }
            if (resp_buflen >= 1) { resp[0] = 1; } /* instant ack -- the screen plays later */
            return 1;
        }
        case MP_DEC_SPY_RESULT_SHOW: { /* buffer a sabotage result (no framing choice); replayed at state
                                          load with the snapshot swapped in, so it never blocks the other player */
            struct mp_sabres_item_s it;
            if (req_len < (int)sizeof(it)) { return 0; }
            memcpy(&it, req, sizeof(it));
            if (s_mp_sabres_n < (int)(sizeof(s_mp_sabres) / sizeof(s_mp_sabres[0]))) {
                s_mp_sabres[s_mp_sabres_n++] = it;
            }
            if (resp_buflen >= 1) { resp[0] = 1; } /* instant ack -- the screen plays later */
            return 1;
        }
        case MP_DEC_COMBAT_REPORT: { /* show this auto-resolved battle's result inline, right after the
                                        battle, instead of holding it to end-of-turn (so you don't wait
                                        on the other player to see your own outcome) */
            struct ui_combat_report_s rep;
            if (req_len < (int)sizeof(rep)) { return 0; }
            memcpy(&rep, req, sizeof(rep));
            ui_combat_report(g, mp_cl_player_id(), &rep, 1);
            if (resp_buflen >= 1) { resp[0] = 1; }
            return 1;
        }
        case MP_DEC_NEWS_ITEM: { /* buffer one turn-summary news record; replayed at state load */
            struct mp_news_item_s it;
            if (req_len < (int)sizeof(it)) { return 0; }
            memcpy(&it, req, sizeof(it));
            if (s_mp_news_n < (int)(sizeof(s_mp_news) / sizeof(s_mp_news[0]))) {
                s_mp_news[s_mp_news_n++] = it;
            }
            if (resp_buflen >= 1) { resp[0] = 1; }
            return 1;
        }
        case MP_DEC_ELECTION_VOTE: { /* galactic council: who to vote for */
            static char ebuf[256];
            struct election_s el;
            int32_t pi32, vote;
            if (req_len < (int)(sizeof(struct election_s) + 4)) { return 0; }
            memcpy(&el, req, sizeof(el));
            memcpy(&pi32, req + sizeof(el), 4);
            el.g = g; el.uictx = NULL; el.buf = ebuf; el.str = NULL;
            if (s_mp_council_active && s_mp_election_ctx) {
                /* shared council: reuse the spectate's loaded chamber ctx (which owns the gfx) */
                s_mp_election = el; s_mp_election.g = g; s_mp_election.uictx = s_mp_election_ctx;
                vote = ui_election_vote(&s_mp_election, pi32);
            } else { /* fallback (no council stream reached us): build + free our own ctx */
                ui_election_ctx_load(&el);
                vote = ui_election_vote(&el, pi32);
                ui_election_ctx_free(&el);
            }
            if (resp_buflen >= (int)sizeof(vote)) { memcpy(resp, &vote, sizeof(vote)); return (int)sizeof(vote); }
            return 0;
        }
        case MP_DEC_ELECTION_ACCEPT: { /* galactic council: accept the elected leader? */
            static char ebuf[256];
            struct election_s el;
            int32_t pi32;
            if ((req_len < (int)(sizeof(struct election_s) + 4)) || (resp_buflen < 1)) { return 0; }
            memcpy(&el, req, sizeof(el));
            memcpy(&pi32, req + sizeof(el), 4);
            el.g = g; el.uictx = NULL; el.buf = ebuf; el.str = NULL;
            if (s_mp_council_active && s_mp_election_ctx) {
                s_mp_election = el; s_mp_election.g = g; s_mp_election.uictx = s_mp_election_ctx;
                resp[0] = ui_election_accept(&s_mp_election, pi32) ? 1 : 0;
            } else {
                ui_election_ctx_load(&el);
                resp[0] = ui_election_accept(&el, pi32) ? 1 : 0;
                ui_election_ctx_free(&el);
            }
            return 1;
        }
        case MP_DEC_BATTLE_INIT: { /* set up the battle UI + run the autoresolve prompt */
            ui_battle_autoresolve_t ar;
            if ((req_len < (int)sizeof(struct battle_s)) || (resp_buflen < 1)) { return 0; }
            memcpy(&s_mp_battle, req, sizeof(s_mp_battle));
            s_mp_battle.uictx = NULL;
            mp_battle_fixup(g, &s_mp_battle);
            ar = ui_battle_init(&s_mp_battle);
            resp[0] = (uint8_t)ar;
            s_mp_battle_uictx = s_mp_battle.uictx; /* capture the UI ctx it allocated */
            s_mp_battle_auto = (ar != UI_BATTLE_AUTORESOLVE_OFF); /* auto/retreat -> no interactive arena */
            return 1;
        }
        case MP_DEC_BATTLE_TURN: { /* render + poll locally until the human picks one action */
            ui_battle_action_t act;
            if ((req_len < (int)sizeof(struct battle_s)) || (resp_buflen < 1)) { return 0; }
            if (!s_mp_battle_uictx) { resp[0] = UI_BATTLE_ACT_AUTO; return 1; } /* no UI -> auto, don't crash */
            memcpy(&s_mp_battle, req, sizeof(s_mp_battle));
            mp_battle_fixup(g, &s_mp_battle);
            s_mp_battle.uictx = s_mp_battle_uictx;
            ui_battle_area_setup(&s_mp_battle); /* per-hex cursor (move/attack arrows) from bt->area */
            ui_battle_draw_basic(&s_mp_battle); /* draw the current battle state */
            do { /* poll until a real action. We MUST redraw every frame: uiobj_finish_frame() is
                    the only thing that recomputes the hover cursor (the move/attack arrow) from
                    bt->area for the current mouse position. Without a per-frame redraw the icon
                    sticks to whatever hex was under the cursor on the first frame — ui_delay only
                    repositions the same sprite, it never re-picks it. Mirrors the SP loop. */
                ui_battle_turn_pre(&s_mp_battle);
                act = ui_battle_turn(&s_mp_battle);
                ui_battle_draw_basic_copy(&s_mp_battle);
                ui_battle_turn_post(&s_mp_battle);
            } while (act == UI_BATTLE_ACT_NONE);
            resp[0] = (uint8_t)act;
            return 1;
        }
        case MP_DEC_BATTLE_END: { /* tear down the battle UI */
            uint8_t colony; int32_t winner;
            if (req_len < (int)(sizeof(struct battle_s) + 5)) { return 0; }
            memcpy(&s_mp_battle, req, sizeof(s_mp_battle));
            colony = req[sizeof(struct battle_s)];
            memcpy(&winner, req + sizeof(struct battle_s) + 1, 4);
            mp_battle_fixup(g, &s_mp_battle);
            s_mp_battle.uictx = s_mp_battle_uictx;
            if (s_mp_battle_uictx) { ui_battle_shutdown(&s_mp_battle, colony != 0, winner); }
            s_mp_battle_uictx = NULL; /* battle over; require a fresh INIT next time */
            return 0;
        }
        case MP_DEC_AUDIENCE: { /* relayed AI<->human audience: run one ui_audience_* call locally */
            if (req_len < (int)(2 + sizeof(struct audience_s))) { return 0; }
            const uint8_t *p = req;
            uint8_t subtype = *p++;
            uint8_t ntpi = *p++;
            memcpy(&s_mp_audience, p, sizeof(s_mp_audience)); p += sizeof(s_mp_audience);
            s_mp_audience.g = g;
            /* message text */
            uint16_t blen; memcpy(&blen, p, 2); p += 2;
            static char s_aud_buf[AUDIENCE_DIPLO_MSG_SIZE + 1];
            if (blen) {
                int n = (blen > AUDIENCE_DIPLO_MSG_SIZE) ? AUDIENCE_DIPLO_MSG_SIZE : blen;
                memcpy(s_aud_buf, p, n); s_aud_buf[n] = '\0'; p += blen;
                s_mp_audience.buf = s_aud_buf;
            } else { s_mp_audience.buf = NULL; }
            /* option strings */
            uint8_t nstr = *p++;
            static char s_aud_str[AUDIENCE_STRTBL_BUFSIZE + AUDIENCE_STR_MAX + 16];
            int realn = (nstr > AUDIENCE_STR_MAX) ? AUDIENCE_STR_MAX : nstr;
            int sp = 0;
            for (int i = 0; i < nstr; ++i) {
                uint16_t l; memcpy(&l, p, 2); p += 2;
                if (i < AUDIENCE_STR_MAX) {
                    int n = l; if (sp + n + 1 > (int)sizeof(s_aud_str)) { n = (int)sizeof(s_aud_str) - sp - 1; if (n < 0) { n = 0; } }
                    memcpy(s_aud_str + sp, p, n); s_aud_str[sp + n] = '\0';
                    s_mp_audience.strtbl[i] = &s_aud_str[sp];
                    sp += n + 1;
                }
                p += l;
            }
            s_mp_audience.strtbl[realn] = NULL;
            /* condition table (which options are enabled) */
            uint8_t has_cond = *p++;
            static bool s_aud_cond[AUDIENCE_STR_MAX];
            if (has_cond) {
                for (int i = 0; i < nstr; ++i) { bool c = (*p++) != 0; if (i < AUDIENCE_STR_MAX) { s_aud_cond[i] = c; } }
                s_mp_audience.condtbl = s_aud_cond;
            } else { s_mp_audience.condtbl = NULL; }
            /* the uictx is a static set up by ui_audience_start; preserve it across the calls */
            if (subtype != MP_AUD_START) { s_mp_audience.uictx = s_mp_audience_uictx; }
            int16_t result = 0; bool is_ask = false;
            switch (subtype) {
                case MP_AUD_START:   ui_audience_start(&s_mp_audience); s_mp_audience_uictx = s_mp_audience.uictx; break;
                case MP_AUD_SHOW1:   ui_audience_show1(&s_mp_audience); break;
                case MP_AUD_SHOW2:   ui_audience_show2(&s_mp_audience); break;
                case MP_AUD_SHOW3:   ui_audience_show3(&s_mp_audience); break;
                case MP_AUD_ASK2A:   result = ui_audience_ask2a(&s_mp_audience); is_ask = true; break;
                case MP_AUD_ASK2B:   result = ui_audience_ask2b(&s_mp_audience); is_ask = true; break;
                case MP_AUD_ASK3:    result = ui_audience_ask3(&s_mp_audience); is_ask = true; break;
                case MP_AUD_ASK4:    result = ui_audience_ask4(&s_mp_audience); is_ask = true; break;
                case MP_AUD_NEWTECH: ui_audience_newtech(&s_mp_audience, ntpi); break;
                case MP_AUD_END:     ui_audience_end(&s_mp_audience); s_mp_audience_uictx = NULL; break;
                default: break;
            }
            if (is_ask && (resp_buflen >= 2)) { memcpy(resp, &result, 2); return 2; }
            return 0;
        }
        default:
            return 0; /* unknown -> no answer; server falls back to its default */
    }
}

/* client: a battle update to watch (the other side is acting) — refresh the cached battle
   state; the interactive render happens in mp_if_on_wait between messages, so a burst of
   updates just lands the latest frame instead of blocking the message pump on each one. */
/* server: g_mp_battle_move_hook impl — relay a ship's move destination to every human in the battle
   (they glide the ship). Reuses the spectate channel; a 7-byte MOVE event, not a battle_s snapshot. */
static void mp_battle_move_relay(struct battle_s *bt, int itemi, int sx, int sy) {
    if (!g_mp_spectate_hook) { return; }
    uint8_t b[7];
    b[0] = UI_MP_SPEC_MOVE;
    b[1] = (uint8_t)(itemi & 0xff); b[2] = (uint8_t)((itemi >> 8) & 0xff);
    b[3] = (uint8_t)(sx & 0xff);    b[4] = (uint8_t)((sx >> 8) & 0xff);
    b[5] = (uint8_t)(sy & 0xff);    b[6] = (uint8_t)((sy >> 8) & 0xff);
    for (battle_side_i_t side = SIDE_L; side <= SIDE_R; ++side) {
        if (bt->s[side].flag_human) { g_mp_spectate_hook(bt->s[side].party, b, 7); }
    }
}

static void mp_if_on_spectate(void *ctx, const uint8_t *data, int len) {
    struct game_s *g = (struct game_s *)ctx;
    /* 1oom-mp: shared council frames (relayed to ALL humans, even non-combatants) -- handle before
       the battle gate, since a council watcher isn't in a battle. */
    if ((len >= 1) && (data[0] == UI_MP_SPEC_COUNCIL_END)) { mp_council_ctx_free(); return; }
    if ((len >= 1 + (int)sizeof(struct election_s)) && (data[0] == UI_MP_SPEC_COUNCIL)) {
        memcpy(&s_mp_election, data + 1, sizeof(s_mp_election));
        s_mp_election.g = g; s_mp_election.buf = NULL;
        /* 1oom-mp: the announcement text rides after the struct (el->str was a server pointer, zeroed
           in transport). Copy it into our own buffer and re-point str, so the council banner -- incl.
           the "X is now emperor" / "no 2/3 majority" result -- actually renders. */
        {
            int textlen = len - 1 - (int)sizeof(struct election_s);
            if (textlen > 0) {
                if (textlen > (int)sizeof(s_mp_election_str) - 1) { textlen = (int)sizeof(s_mp_election_str) - 1; }
                memcpy(s_mp_election_str, data + 1 + sizeof(struct election_s), (size_t)textlen);
                s_mp_election_str[textlen] = 0;
                s_mp_election.str = (s_mp_election_str[0] != 0) ? s_mp_election_str : NULL;
            } else {
                s_mp_election.str = NULL;
            }
        }
        mp_council_ctx_ensure(g);
        s_mp_election.uictx = s_mp_election_ctx; /* re-point: the memcpy above zeroed it */
        s_mp_council_active = true;
        return;
    }
    if (!s_mp_battle_uictx || s_mp_battle_auto) { return; } /* not in a battle, or auto-resolved it (ctx half-built, no gfx_bg): can't render the streamed animations */
    if (len == (int)sizeof(struct battle_s)) {
        /* a full snapshot: refresh the spectator's battle (the wait loop redraws it) */
        memcpy(&s_mp_battle, data, sizeof(s_mp_battle));
        mp_battle_fixup(g, &s_mp_battle);
        s_mp_battle.uictx = s_mp_battle_uictx;
        return;
    }
    if (data[0] == UI_MP_SPEC_MISSILE) { /* 1oom-mp: missile flight. The struct rides along (the missile
                                            launched after the last full snapshot). Populate it and redraw
                                            the arena -- ui_battle_draw_arena already draws every live
                                            missile[] traveling toward its target. Drawing the sprite
                                            directly (the old code) never flipped a frame, so missiles
                                            were invisible; it also double-drew what the arena does. */
        int need = 1 + 10 + (int)sizeof(struct battle_missile_s);
        if (len < need) { return; }
        {
            int16_t mi = (int16_t)(data[1] | (data[2] << 8));
            int16_t mx = (int16_t)(data[3] | (data[4] << 8));
            int16_t my = (int16_t)(data[5] | (data[6] << 8));
            if ((mi >= 0) && (mi < BATTLE_MISSILE_MAX)) {
                memcpy(&s_mp_battle.missile[mi], data + 11, sizeof(struct battle_missile_s));
                s_mp_battle.missile[mi].x = mx; /* the relayed current flight position */
                s_mp_battle.missile[mi].y = my;
                if (mi >= (int)s_mp_battle.num_missile) { s_mp_battle.num_missile = (uint8_t)(mi + 1); }
                s_mp_battle.uictx = s_mp_battle_uictx;
                ui_battle_draw_basic(&s_mp_battle); /* arena draws missile[mi] at (x,y) toward its target + flips */
            }
        }
        return;
    }
    /* otherwise a small animation event: replay the attack on the current battle so the spectator
       sees the beam/bomb/etc. fire, not just the before/after states ([u8 kind] + int16 args). */
    if (len < 1) { return; }
    s_mp_battle.uictx = s_mp_battle_uictx;
    {
        int a[8];
        int n = (len - 1) / 2;
        if (n > 8) { n = 8; }
        for (int i = 0; i < n; ++i) { a[i] = (int16_t)(data[1 + i * 2] | (data[2 + i * 2] << 8)); }
        switch (data[0]) {
            case UI_MP_SPEC_BEAM:      if (n >= 3) { ui_battle_draw_beam_attack(&s_mp_battle, a[0], a[1], a[2]); } break;
            case UI_MP_SPEC_BOMB:      if (n >= 3) { ui_battle_draw_bomb_attack(&s_mp_battle, a[0], a[1], (ui_battle_bomb_t)a[2]); } break;
            case UI_MP_SPEC_STASIS:    if (n >= 2) { ui_battle_draw_stasis(&s_mp_battle, a[0], a[1]); } break;
            case UI_MP_SPEC_STREAM1:   if (n >= 2) { ui_battle_draw_stream1(&s_mp_battle, a[0], a[1]); } break;
            case UI_MP_SPEC_STREAM2:   if (n >= 2) { ui_battle_draw_stream2(&s_mp_battle, a[0], a[1]); } break;
            case UI_MP_SPEC_BLACKHOLE: if (n >= 2) { ui_battle_draw_blackhole(&s_mp_battle, a[0], a[1]); } break;
            case UI_MP_SPEC_TECHNULL:  if (n >= 2) { ui_battle_draw_technull(&s_mp_battle, a[0], a[1]); } break;
            case UI_MP_SPEC_REPULSE:   if (n >= 4) { ui_battle_draw_repulse(&s_mp_battle, a[0], a[1], a[2], a[3]); } break;
            case UI_MP_SPEC_RETREAT:   if (n >= 1) { s_mp_battle.cur_item = a[0]; ui_battle_draw_retreat(&s_mp_battle); } break;
            case UI_MP_SPEC_MOVE:      if (n >= 3) { ui_mp_battle_glide(&s_mp_battle, a[0], a[1], a[2]); } break;
            case UI_MP_SPEC_DAMAGE:    if (n >= 5) { ui_battle_draw_damage(&s_mp_battle, a[0], a[1], a[2], (uint32_t)(uint16_t)a[3] | ((uint32_t)(uint16_t)a[4] << 16)); } break;
            default: break;
        }
    }
}

/* server: apply a client's submitted production orders to its empire */
static int mp_if_apply_orders(void *ctx, int player_id, const uint8_t *buf, int len) {
    return game_mp_apply_orders((struct game_s *)ctx, (player_id_t)player_id, buf, len);
}

/* client: prepare a freshly-received state for display/play (recompute derived
   values; one-time UI setup on the first load). */
static void mp_if_on_state_loaded(void *ctx, int first) {
    struct game_s *g = (struct game_s *)ctx;
    mp_council_ctx_free(); /* a turn resolved: tear down any council view (safety net if COUNCIL_END was dropped) */
    if (first) {
        /* The client never ran game_aux_start, so the galaxy distance table is empty.
           Build it from the just-loaded star positions BEFORE game_start recomputes
           visibility — otherwise star_dist=0 makes every star read as in scanner range
           (revealing all empires' colonies). */
        game_aux_init_star_dist(g->gaux, g);
        game_start(g);
        ui_game_start(g);
    } else {
        /* Per-turn display refresh: recompute derived values from the server's
           authoritative state, but do NOT re-run the load-time slider auto-adjust
           (game_update_eco_on_waste) — the server already resolved the sliders, so
           we show them exactly as broadcast instead of re-adjusting them here. */
        game_update_production(g);
        game_update_tech_util(g);
        for (player_id_t i = PLAYER_0; i < g->players; ++i) { game_update_seen_by_orbit(g, i); }
        game_update_within_range(g);
        game_update_visibility(g);
        game_update_have_reserve_fuel(g);
        /* auto-resolved combat results are now shown inline (MP_DEC_COMBAT_REPORT), not consolidated here */
        if (s_mp_stolen_n > 0) { /* replay tech-theft victim notices (espionage resolves before bombing) */
            for (int i = 0; i < s_mp_stolen_n; ++i) {
                ui_spy_stolen(g, mp_cl_player_id(), s_mp_stolen[i].spy, s_mp_stolen[i].field, (uint8_t)s_mp_stolen[i].tech);
            }
            s_mp_stolen_n = 0;
        }
        if (s_mp_sabres_n > 0) { /* replay saboteurs' result screens concurrently, snapshot swapped in */
            for (int i = 0; i < s_mp_sabres_n; ++i) {
                struct mp_sabres_item_s *r = &s_mp_sabres[i];
                planet_id_t pl = (planet_id_t)r->planet;
                if (pl >= g->galaxy_stars) { continue; }
                planet_t saved = g->planet[pl];
                g->planet[pl] = r->psnap; /* sabotage-time planet state; restore after rendering */
                ui_spy_sabotage_done(g, mp_cl_player_id(), r->spy, r->target, (ui_sabotage_t)r->act, r->other1, r->other2, pl, r->snum);
                g->planet[pl] = saved;
            }
            s_mp_sabres_n = 0;
        }
        if (s_mp_bomb_n > 0) { /* replay this client's orbital bombings concurrently with the other player's */
            for (int i = 0; i < s_mp_bomb_n; ++i) {
                struct mp_bomb_item_s *b = &s_mp_bomb[i];
                planet_id_t pl = (planet_id_t)b->planet;
                if (pl >= g->galaxy_stars) { continue; }
                planet_t saved = g->planet[pl];
                g->planet[pl] = b->psnap; /* bomb-time planet state; restore after rendering */
                ui_bomb_show(g, mp_cl_player_id(), b->attacker, b->owner, pl, b->popdmg, b->factdmg, b->play_music != 0, b->hide_other != 0);
                g->planet[pl] = saved;
            }
            s_mp_bomb_n = 0;
        }
        if (s_mp_ground_n > 0) { /* replay this client's ground invasions; every player's client does its own at the same time, so they no longer block each other */
            for (int i = 0; i < s_mp_ground_n; ++i) { ui_ground(g, &s_mp_ground[i]); }
            s_mp_ground_n = 0;
        }
        if (s_mp_news_n > 0) { /* replay the turn's news as one GNN broadcast, now that fresh state is loaded */
            ui_news_start();
            for (int i = 0; i < s_mp_news_n; ++i) {
                struct news_s ns;
                memset(&ns, 0, sizeof(ns));
                ns.type = (news_type_t)s_mp_news[i].type;
                ns.subtype = s_mp_news[i].subtype;
                ns.num1 = s_mp_news[i].num1;
                ns.num2 = s_mp_news[i].num2;
                ns.race = (race_t)s_mp_news[i].race;
                ns.planet_i = (planet_id_t)s_mp_news[i].planet_i;
                ui_news(g, &ns);
            }
            ui_news_end();
            s_mp_news_n = 0;
        }
    }
    if (getenv("MP_CONTACT")) { /* test: keep everyone in contact. The per-load range re-derive
        (game_update_tech_util) resets fuel_range, which clears contact in the Races screen; bump
        it back up and recompute contact so the other empires stay on the diplomacy page. */
        for (player_id_t i = PLAYER_0; i < g->players; ++i) { g->eto[i].fuel_range = 200; }
        game_update_empire_contact(g);
    }
}

/* ---- soft-ready turn UI hooks (declared in ui.h, called by the starmap). Active only while a
   networked planning phase runs, i.e. while mp_client_run has wired g_mp_cl_poll.

   Model: "Next Turn" locks me in -- it submits my current orders and marks me ready. I keep
   playing; any change to my orders is re-submitted automatically, so the state at resolution
   time is what counts. The turn resolves once everyone is ready. To take more time on a bigger
   change, I un-ready myself by hitting Next Turn again (toggle). ---- */
static int s_mp_my_pid = -1;            /* my empire id this turn (for serializing orders) */
static bool s_mp_ready = false;         /* am I locked in this turn? */
static uint8_t *s_mp_orders_buf = NULL; /* the orders I last submitted (baseline for change detect) */
static uint8_t *s_mp_orders_tmp = NULL; /* scratch: current orders, to compare against the baseline */
static int s_mp_orders_cap = 0;
static int s_mp_orders_len = 0;

/* serialize my current orders, send them with the given ready flag, and remember them. */
static void mp_submit_orders(bool ready) {
    if (!s_mp_orders_buf) {
        s_mp_orders_cap = game_save_blob_maxlen(&game);
        s_mp_orders_buf = (uint8_t *)malloc(s_mp_orders_cap);
    }
    int olen = 0;
    if (s_mp_orders_buf && (s_mp_my_pid >= 0)) {
        olen = game_mp_write_orders(&game, (player_id_t)s_mp_my_pid, s_mp_orders_buf, s_mp_orders_cap);
        if (olen < 0) { olen = 0; }
    }
    s_mp_orders_len = olen;
    s_mp_ready = ready;
    if (g_mp_cl_send_ready) { g_mp_cl_send_ready(ready ? 1 : 0, s_mp_orders_buf, olen); }
}

/* true if my orders now differ from the snapshot I last submitted */
static bool mp_orders_changed(void) {
    if (!s_mp_orders_buf || (s_mp_orders_cap <= 0) || (s_mp_my_pid < 0)) { return false; }
    if (!s_mp_orders_tmp) { s_mp_orders_tmp = (uint8_t *)malloc(s_mp_orders_cap); }
    if (!s_mp_orders_tmp) { return false; }
    int n = game_mp_write_orders(&game, (player_id_t)s_mp_my_pid, s_mp_orders_tmp, s_mp_orders_cap);
    if (n < 0) { n = 0; }
    if (n != s_mp_orders_len) { return true; }
    return memcmp(s_mp_orders_tmp, s_mp_orders_buf, (size_t)n) != 0;
}

static bool mp_turn_active_impl(void) { return g_mp_cl_poll != NULL; }
static bool mp_turn_is_ready_impl(void) { return s_mp_ready; }
/* "Next Turn": toggle my ready lock (submitting my current orders when I lock in). */
static void mp_turn_set_ready_impl(int ready) { mp_submit_orders(ready != 0); }
/* pump the socket once; while locked in, re-submit my orders if they changed (so the latest
   state resolves). Returns true once the server signalled resolution (planning is over). */
static bool mp_turn_poll_impl(void) {
    if (s_mp_ready && mp_orders_changed()) { mp_submit_orders(true); }
    return g_mp_cl_poll ? (g_mp_cl_poll() != 0) : false;
}

bool (*ui_mp_turn_active)(void) = mp_turn_active_impl;
void (*ui_mp_turn_set_ready)(int ready) = mp_turn_set_ready_impl;
bool (*ui_mp_turn_is_ready)(void) = mp_turn_is_ready_impl;
bool (*ui_mp_turn_poll)(void) = mp_turn_poll_impl;
bool ui_mp_active = false; /* true while this process is a networked client (set in game_mp_join) */
bool game_mp_is_server = false; /* true while this process is the headless MP server (set in game_mp_host) */

/* client: the human plays their empire's turn through the real game UI, then we
   serialize their orders. Combat/resolution happen on the server, not here. */
static int mp_if_write_orders(void *ctx, int player_id, uint8_t *buf, int buflen) {
    struct game_s *g = (struct game_s *)ctx;
    s_mp_my_pid = player_id;   /* soft-ready: who I am, for the Ready-submit hook */
    s_mp_ready = false;        /* a fresh turn starts un-readied */
    game_mp_orders_reset();    /* clear last turn's queued diplo/colonize actions before this turn re-queues */
    if (getenv("MP_AUTOTECH")) {
        /* MP-DBG: pour everything into TECH (unlocked) to reproduce the slider issue */
        for (int i = 0; i < g->galaxy_stars; ++i) {
            planet_t *p = &g->planet[i];
            if (p->owner != (player_id_t)player_id) { continue; }
            for (int j = 0; j < PLANET_SLIDER_NUM; ++j) { p->slider[j] = 0; p->slider_lock[j] = 0; }
            p->slider[PLANET_SLIDER_TECH] = 100;
        }
    } else {
        int load_game_i = -1;
        ui_game_turn(g, &load_game_i, player_id);
    }
    /* MP-DBG diplomacy test hooks (env-gated): exercise the action path + round-trip. */
    if (getenv("MP_AUTOWAR") && (player_id == 0)) {
        static bool warred = false;
        if (!warred && g->players > 1) { game_mp_diplo_record(PLAYER_0, (player_id_t)1, MP_DIPLO_DECLARE_WAR, 0); warred = true; }
    }
    if (getenv("MP_AUTONAP")) {
        if (player_id == 0) {
            static bool proposed = false;
            if (!proposed && g->players > 1) { game_mp_diplo_record(PLAYER_0, (player_id_t)1, MP_DIPLO_PROPOSE_NAP, 0); proposed = true; }
        }
        /* any player auto-accepts a pending human-to-human proposal (simulates the UI) */
        for (player_id_t j = PLAYER_0; j < g->players; ++j) {
            if ((j != (player_id_t)player_id) && (g->eto[player_id].diplo_type[j] == MP_DIPLO_PROPOSAL_MARK)) {
                game_mp_diplo_record((player_id_t)player_id, j, MP_DIPLO_ACCEPT, (uint8_t)g->eto[player_id].diplo_val[j]);
            }
        }
    }
    if (getenv("MP_AUTOWAR") || getenv("MP_AUTONAP")) {
        player_id_t other = (player_id == 0) ? 1 : 0;
        if (other < g->players) { log_message("MP-DBG p%d: treaty[p%d]=%d diplo_type=%d\n", player_id, other, g->eto[player_id].treaty[other], g->eto[player_id].diplo_type[other]); }
    }
    return game_mp_write_orders(g, (player_id_t)player_id, buf, buflen);
}

static int mp_if_setup_game(void *ctx, const struct mp_lobby_s *lobby);
static int mp_if_run_lobby(void *ctx, int my_id);

static mp_game_iface_t mp_make_iface(void) {
    mp_game_iface_t gi;
    gi.ctx = &game;
    gi.serialize = mp_if_serialize;
    gi.deserialize = mp_if_deserialize;
    gi.advance_turn = mp_if_advance;
    gi.turn_number = mp_if_turn;
    gi.max_blob_len = mp_if_maxblob;
    gi.write_orders = mp_if_write_orders;
    gi.apply_orders = mp_if_apply_orders;
    gi.on_state_loaded = mp_if_on_state_loaded;
    gi.get_movement = mp_if_get_movement;
    gi.play_movement = mp_if_play_movement;
    gi.on_wait = mp_if_on_wait;
    gi.handle_decision = mp_if_handle_decision;
    gi.on_spectate = mp_if_on_spectate;
    gi.run_lobby = mp_if_run_lobby;   /* client: interactive pre-game lobby */
    gi.setup_game = mp_if_setup_game; /* server: build the game from the final lobby */
    return gi;
}

/* new-game options captured at host start, so the game can be (re)created from the lobby picks */
static struct game_new_options_s s_mp_opts;

/* server: once the lobby is settled, (re)create the game from it -- player count (humans + AI),
   galaxy size, and each human's chosen race + banner. */
static int mp_if_setup_game(void *ctx, const struct mp_lobby_s *lobby) {
    (void)ctx;
    int humans = lobby->num_humans;
    int total = humans + lobby->num_ai;
    if (total < 1) { total = 1; }
    if (total > PLAYER_NUM) { total = PLAYER_NUM; }
    s_mp_opts.players = (uint32_t)total;
    if (lobby->galaxy_size < GALAXY_SIZE_NUM) { s_mp_opts.galaxy_size = (galaxy_size_t)lobby->galaxy_size; }
    if (lobby->difficulty < DIFFICULTY_NUM) { s_mp_opts.difficulty = (difficulty_t)lobby->difficulty; }
    /* humans fill empires 0..humans-1 (mp_server_run maps client i -> empire i) with their picks;
       the remaining empires are AI keeping the host defaults. */
    for (int i = 0; i < total; ++i) {
        bool is_ai = (i >= humans);
        s_mp_opts.pdata[i].is_ai = is_ai;
        /* race applies to humans and to any AI slot the host chose (unset AI slots keep the default = random) */
        if (lobby->slot[i].race < RACE_NUM) { s_mp_opts.pdata[i].race = (race_t)lobby->slot[i].race; }
        if (!is_ai && (lobby->slot[i].banner < BANNER_NUM)) { s_mp_opts.pdata[i].banner = (banner_t)lobby->slot[i].banner; }
    }
    /* give every empire a distinct banner (a human's pick may collide with an AI default). */
    {
        bool used[BANNER_NUM]; for (int k = 0; k < BANNER_NUM; ++k) { used[k] = false; }
        for (int i = 0; i < total; ++i) {
            int b = s_mp_opts.pdata[i].banner;
            if (b < 0 || b >= BANNER_NUM || used[b]) {
                for (int k = 0; k < BANNER_NUM; ++k) { if (!used[k]) { b = k; break; } }
            }
            s_mp_opts.pdata[i].banner = (banner_t)b;
            if (b >= 0 && b < BANNER_NUM) { used[b] = true; }
        }
    }
    game_new(&game, &game_aux, &s_mp_opts);
    game_aux_start(&game_aux, &game);
    game_start(&game);
    /* teams: record each empire's team, and start same-team empires as met + allied (have_met=1 keeps
       game_turn_update_have_met from resetting the treaty). The team-aware win check reads mp_team. */
    for (int i = 0; i < total; ++i) { game.mp_team[i] = lobby->slot[i].team; }
    for (int i = 0; i < total; ++i) {
        for (int j = i + 1; j < total; ++j) {
            if ((lobby->slot[i].team != 0) && (lobby->slot[i].team == lobby->slot[j].team)) {
                game.eto[i].treaty[j] = TREATY_ALLIANCE; game.eto[j].treaty[i] = TREATY_ALLIANCE;
                game.eto[i].have_met[j] = 1; game.eto[j].have_met[i] = 1;
                BOOLVEC_SET1(game.eto[i].contact, j); BOOLVEC_SET1(game.eto[j].contact, i);
            }
        }
    }
    log_message("MP: game ready -- %d empires (%d human, %d AI), %d stars\n", game.players, humans, total - humans, game.galaxy_stars);
    return 0;
}

/* client: run the interactive pre-game lobby (loops until the game starts). */
static int mp_if_run_lobby(void *ctx, int my_id) {
    (void)ctx;
    return ui_mp_lobby_run(my_id);
}

/* 1oom-mp: load an MP autosave written by game_mp_autosave ([int humans][blob]) into `game`.
   Caller must have game_aux_init'd; game_aux_start/game_start follow (as for a freshly-created game).
   Reports the recorded human-client count via *humans_out. */
static int game_mp_load_autosave(const char *fname, int *humans_out) {
    FILE *f = fopen(fname, "rb");
    long fsize;
    int humans = 0, rc;
    size_t blen, got;
    uint8_t *buf;
    if (!f) { log_error("MP: cannot open save '%s'\n", fname); return 1; }
    fseek(f, 0, SEEK_END); fsize = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsize <= (long)sizeof(int)) { fclose(f); log_error("MP: save '%s' too small\n", fname); return 1; }
    if (fread(&humans, sizeof(int), 1, f) != 1) { fclose(f); log_error("MP: save '%s' header read failed\n", fname); return 1; }
    blen = (size_t)fsize - sizeof(int);
    buf = (uint8_t *)malloc(blen);
    if (!buf) { fclose(f); return 1; }
    got = fread(buf, 1, blen, f);
    fclose(f);
    if (got != blen) { free(buf); log_error("MP: save '%s' body truncated\n", fname); return 1; }
    rc = game_save_blob_load(&game, buf, (int)blen);
    free(buf);
    if (rc != 0) { log_error("MP: save '%s' is corrupt or incompatible\n", fname); return 1; }
    if (humans < 1) { humans = 1; }
    if (humans > PLAYER_NUM) { humans = PLAYER_NUM; }
    *humans_out = humans;
    return 0;
}

static int game_mp_host(void) {
    int humans, rc;
    bool resume = (opt_mp_load != NULL);
    mp_game_iface_t gi;
    game_mp_is_server = true; /* keep human newtech lists in the synced state (see game_turn.c) */
    if (game_aux_init(&game_aux, &game)) { return 1; }
    /* 1oom-mp: make slider locks actually hold (default-off in vanilla), so a player
       can pin a slider and the per-turn ECO/waste auto-balance won't override it. */
    game_num_slider_respects_locks = true;
    if (resume) {
        /* resume a crashed/saved game: the autosave blob takes the place of game_new. */
        if (game_mp_load_autosave(opt_mp_load, &humans)) { game_aux_shutdown(&game_aux); return 1; }
    } else {
        /* honor -new (galaxy size, races, empire count, difficulty); GAME_NEW_OPTS_DEFAULT otherwise */
        struct game_new_options_s opts = game_opt_new;
        humans = opt_mp_humans;
        if (humans < 1) { humans = 1; }
        if (humans > PLAYER_NUM) { humans = PLAYER_NUM; }
        if (opts.players < (uint32_t)humans) { opts.players = (uint32_t)humans; }
        /* the connecting clients fill the first `humans` empires; the rest are AI.
           (mp_server_run assigns client i -> empire i, so humans must be contiguous 0..N-1.) */
        for (int i = 0; i < (int)opts.players; ++i) { opts.pdata[i].is_ai = (i >= humans); }
        if (getenv("MP_BIGRANGE")) { opts.homeworlds.num_fighters = 4; } /* test: start with warships for quick combat */
        s_mp_opts = opts; /* keep a copy so the lobby can re-create the game with the chosen races */
        game_new(&game, &game_aux, &opts);
    }
    s_mp_humans = humans; /* recorded in each autosave header so a later -mpload waits for the right count */
    /* Empires 0..humans-1 are the human clients; the rest run as AI on the server.
       At least one human must stay live or game_turn_check_end ends the game. */
    game_aux_start(&game_aux, &game);
    game_start(&game);      /* sets game_ai dispatch + per-empire derived values (eco/tech/production) */
    ui_game_start(&game);
    /* arm turn-movement playback: snapshot the pre-movement state each turn so the
       client can animate fleets moving (see mp_premove_capture / mp_if_get_movement). */
    s_mp_premove_cap = game_save_blob_maxlen(&game);
    s_mp_premove_buf = (uint8_t *)malloc(s_mp_premove_cap);
    game_mp_premove_hook = s_mp_premove_buf ? mp_premove_capture : NULL;
    g_mp_battle_move_hook = mp_battle_move_relay; /* server: relay in-combat ship moves to spectators */
    log_message("MP: %s on port %d, %d empires (%d human, %d AI), %d stars\n", resume ? "resumed" : "hosting", opt_mp_host_port, game.players, humans, (int)game.players - humans, game.galaxy_stars);
    gi = mp_make_iface();
    if (resume) { gi.setup_game = NULL; } /* skip the lobby; the loaded game IS the initial state sent to clients */
    rc = mp_server_run((uint16_t)opt_mp_host_port, humans, 0 /*until disconnect*/, &gi);
    game_mp_premove_hook = NULL;
    g_mp_battle_move_hook = NULL;
    free(s_mp_premove_buf); s_mp_premove_buf = NULL;
    game_aux_shutdown(&game_aux);
    return rc ? 1 : 0;
}

static int game_mp_join(void) {
    char host[128];
    uint16_t port = MP_DEFAULT_PORT;
    ui_mp_active = true; /* enable client-side turn-start prompts (e.g. planet discovery) */
    const char *colon = strrchr(opt_mp_join, ':');
    if (colon) {
        size_t n = (size_t)(colon - opt_mp_join);
        if (n >= sizeof(host)) { n = sizeof(host) - 1; }
        memcpy(host, opt_mp_join, n);
        host[n] = '\0';
        port = (uint16_t)atoi(colon + 1);
    } else {
        size_t n = strlen(opt_mp_join);
        if (n >= sizeof(host)) { n = sizeof(host) - 1; }
        memcpy(host, opt_mp_join, n);
        host[n] = '\0';
    }
    if (game_aux_init(&game_aux, &game)) { return 1; }
    /* 1oom-mp: make slider locks actually hold (default-off in vanilla), so a player
       can pin a slider and the per-turn ECO/waste auto-balance won't override it. */
    game_num_slider_respects_locks = true;
    log_message("MP: joining %s:%u\n", host, (unsigned)port);
    mp_game_iface_t gi = mp_make_iface();
    gi.advance_turn = NULL; /* the client does not resolve turns */
    int rc = mp_client_run(host, port, 0 /*until game over*/, &gi);
    game_aux_shutdown(&game_aux);
    return rc ? 1 : 0;
}

int main_do(void)
{
    struct game_end_s game_end = game_opt_end;
    if (ui_late_init()) {
        return 1;
    }
    if (opt_mp_host_port > 0) { return game_mp_host(); }
    if (opt_mp_join) { return game_mp_join(); }
    game_aux_init(&game_aux, &game);
    game_save_check_saves(game_aux.savenamebuf, game_aux.savenamebuflen);
    if ((game_opt_end.type != GAME_END_NONE) && (game_opt_end.varnum == 2)) {
        goto do_ending;
    }
    if (!(game_opt_skip_intro || game_opt_skip_intro_always)) {
        ui_play_intro();
    }
    while (1) {
        struct game_new_options_s game_new_opts = GAME_NEW_OPTS_DEFAULT;
        struct game_new_options_s game_challenge_opts = GAME_NEW_OPTS_DEFAULT;
        main_menu_action_t main_menu_action;
        int load_game_i = 0;

        if (game_opt_new_game) {
            game_opt_new_game = false;
            game_new_opts = game_opt_new;
            goto main_menu_new_game;
        } else if (game_opt_load_fname) {
            if (game_save_do_load_fname(game_opt_load_fname, 0, &game)) {
                log_fatal_and_die("Game: could not load save '%s'\n", game_opt_load_fname);
            }
            game_opt_load_fname = 0;
            goto main_menu_start_game;
        } else if (game_opt_load_game) {
            load_game_i = game_opt_load_game;
            if (load_game_i < 2300) {
                --load_game_i;
            }
            game_opt_load_game = 0;
            if ((load_game_i >= 2300) || game_save_tbl_have_save[load_game_i]) {
                goto main_menu_load_game;
            } else {
                log_warning("Game: direct load game %i failed due to missing savegame\n", load_game_i + 1);
                /* try again, now with game_opt_load_game set to 0 */
                continue;
            }
        } else if (game_opt_continue) {
            game_opt_continue = false;
            if (game_save_tbl_have_save[GAME_SAVE_I_CONTINUE]) {
                goto main_menu_continue_game;
            } else {
                log_warning("Game: direct continue failed due to missing savegame\n");
                /* try again, now with game_opt_continue set to false */
                continue;
            }
        } else {
            game_set_opts_from_value(&game_new_opts, game_opt_new_value);
            game_set_custom_opts_from_cfg(&game_opt_custom);
            main_menu_action = ui_main_menu(&game_new_opts, &game_opt_custom, &game_challenge_opts, &load_game_i);
        }
        switch (main_menu_action) {
            case MAIN_MENU_ACT_NEW_GAME:
                main_menu_new_game:
                game_new(&game, &game_aux, &game_new_opts);
                game_opt_new_value = game_get_opts_value(&game);
                if (game_opt_init_save_enabled) {
                    game_save_do_save_i(GAME_SAVE_I_INIT, "Init", &game);
                }
                break;
            case MAIN_MENU_ACT_CUSTOM_GAME:
                game_new(&game, &game_aux, &game_opt_custom);
                game_save_custom_opts_to_cfg(&game_opt_custom);
                if (game_opt_init_save_enabled) {
                    game_save_do_save_i(GAME_SAVE_I_INIT, "Init", &game);
                }
                break;
            case MAIN_MENU_ACT_CHALLENGE_GAME:
                game_new(&game, &game_aux, &game_challenge_opts);
                if (game_opt_init_save_enabled) {
                    game_save_do_save_i(GAME_SAVE_I_INIT, "Init", &game);
                }
                break;
            case MAIN_MENU_ACT_TUTOR:
                game_new_tutor(&game, &game_aux);
                break;
            case MAIN_MENU_ACT_LOAD_GAME:
                main_menu_load_game:
                if (0
                  || ((load_game_i < NUM_ALL_SAVES) && game_save_do_load_i(load_game_i, &game))
                  || ((load_game_i >= 2300) && game_save_do_load_year(load_game_i, 0, &game))
                ) {
                    log_fatal_and_die("Game: could not load save %i\n", load_game_i);
                }
                break;
            case MAIN_MENU_ACT_LOAD_GAME_MOO13:
                {
                    char fname[12] = "SAVEX.GAM";
                    fname[4] = load_game_i + '0' + 1;   /* FIXME */
                    if (game_save_de_moo13(&game, fname) < 0) {
                        continue;
                    }
                }
                break;
            case MAIN_MENU_ACT_CONTINUE_GAME:
                main_menu_continue_game:
                if (game_save_do_load_i(GAME_SAVE_I_CONTINUE, &game)) {
                    log_fatal_and_die("Game: could not start or continue from save 7\n");
                }
                break;
            case MAIN_MENU_ACT_QUIT_GAME:
                log_message("Game: quit (main)\n");
                goto done;
        }
        main_menu_start_game:
        game_aux_start(&game_aux, &game);
        game_start(&game);
        ui_game_start(&game);
        game_end.type = GAME_END_NONE;
        while ((game_end.type == GAME_END_NONE) || (game_end.type == GAME_END_FINAL_WAR)) {
            for (; ((game_end.type == GAME_END_NONE) || (game_end.type == GAME_END_FINAL_WAR)) && (game.active_player < game.players); ++game.active_player) {
                if (!IS_HUMAN(&game, game.active_player) || (!IS_ALIVE(&game, game.active_player))) {
                    continue;
                }
                if (game_opt_next_turn) {
                    game_opt_next_turn = false;
                    break;
                }
                if (game_opt_save_quit) {
                    goto turn_act_quit;
                }
                switch (ui_game_turn(&game, &load_game_i, game.active_player)) {
                    case UI_TURN_ACT_LOAD_GAME:
                        main_menu_action = MAIN_MENU_ACT_LOAD_GAME;
                        ui_game_end(&game);
                        goto main_menu_load_game;
                    case UI_TURN_ACT_QUIT_GAME:
                        turn_act_quit:
                        if (game_save_do_save_i(GAME_SAVE_I_CONTINUE, "Continue", &game)) {
                            log_error("Game: could create continue save\n");
                        }
                        game_end.type = GAME_END_QUIT;
                        break;
                    case UI_TURN_ACT_NEXT_TURN:
                        if (game_opt_undo_enabled && game_save_do_save_i(GAME_SAVE_I_UNDO, "Undo", &game)) {
                            log_error("Game: could create undo save\n");
                        }
                        if (game_opt_year_save_enabled && game_save_do_save_year(NULL, &game)) {
                            log_error("Game: could create year save\n");
                        }
                        break;
                }
            }
            if (game_end.type != GAME_END_QUIT) {
                game_end = game_turn_process(&game);
            }
            game.active_player = PLAYER_0;
        }
        ui_game_end(&game);
        do_ending:
        switch (game_end.type) {
            case GAME_END_QUIT:
                log_message("Game: quit (ingame)\n");
                if (game_opt_save_quit) {
                    game_opt_save_quit = false;
                    goto done;
                }
                break;
            case GAME_END_NONE:
            case GAME_END_FINAL_WAR:
                break;
            case GAME_END_WON_GOOD:
                ui_play_ending_good(game_end.race, game_end.name);
                break;
            case GAME_END_WON_TYRANT:
                ui_play_ending_tyrant(game_end.race, game_end.name);
                break;
            case GAME_END_LOST_FUNERAL:
                ui_play_ending_funeral(game_end.race, game_end.banner_dead);
                break;
            case GAME_END_LOST_EXILE:
                ui_play_ending_exile(game_end.name);
                break;
        }
        game_end.type = GAME_END_NONE;
        game_stop(&game);
    }

done:
    return 0;
}

void main_do_shutdown(void)
{
    /* TODO save game if in progress */
    game_aux_shutdown(&game_aux);
    game_str_shutdown();
}
