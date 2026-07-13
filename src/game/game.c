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
#include "game_str.h"
#include "game_turn.h"
#include "game_tech.h"
#include "game_shiptech.h"
#include "util_math.h"
#include "log.h"
#include "hw.h"
#include "mp.h"
#include "net.h"
#include "options.h"
#include "rnd.h"
#include "ui.h"
#include "util.h"
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

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
static int opt_mp_open = 0;   /* host: >0 = open lobby -- players join freely up to this many and the host clicks Start */
static char *opt_mp_load = NULL; /* host: path to an MP autosave to resume instead of starting a new game */
static char *opt_mp_race = NULL; /* client: race name/id to claim when joining a RESUMED game (-mprace) */
struct ui_mp_setup_s ui_mp_setup; /* 1oom-mp: what the Multiplayer menu collected (see ui.h) */
char *ui_mp_last_addr = NULL;     /* 1oom-mp: last join address, persisted in the config for the Join screen */
static int s_mp_humans = 1;      /* host: human-client count, recorded in the autosave header for -mpload */
static char s_mp_game_id[24] = {0}; /* 1oom-mp: per-game id (timestamp at game start), kept in the save header so each game has its own autosave file (no clobber) and the id is stable across -mpload resumes */
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
    { "-mprace", 1,
      options_set_str_var, (void *)&opt_mp_race,
      "RACE", "Join (resumed game): claim the empire of this race (name e.g. Silicoid, or id 0-9) instead of the next free slot" },
    { "-mphumans", 1,
      options_set_int_var, (void *)&opt_mp_humans,
      "N", "Multiplayer (host): number of human players to wait for (default 1; rest of -new empires are AI)" },
    { "-mpload", 1,
      options_set_str_var, (void *)&opt_mp_load,
      "FILE", "Multiplayer (host): resume instead of starting a new game. FILE may be a game folder (games/<id> -> loads its latest turn), a specific turn save (games/<id>/y<year>.blob), or a legacy autosave (mp_auto_<id>.blob)" },
    { "-mpopen", 1,
      options_set_int_var, (void *)&opt_mp_open,
      "MAXHUMANS", "Multiplayer (host): OPEN lobby -- players join freely (up to MAXHUMANS) and the host clicks Start to begin, instead of -mphumans' fixed count" },
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
    CFG_ITEM_STR("mp_join_addr", &ui_mp_last_addr, NULL), /* 1oom-mp: Join screen remembers the last host address */
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
static void game_mp_client_store_state(struct game_s *g, const uint8_t *blob, int len); /* fwd: save sync (below) */
static int mp_if_deserialize(void *ctx, const uint8_t *buf, int len) {
    int r = game_save_blob_load((struct game_s *)ctx, buf, len);
    /* 1oom-mp save sync: a client stores each authoritative state it receives (the raw bytes, so the
       local files are byte-identical to the host's). The server never deserializes here. */
    if ((r == 0) && !game_mp_is_server && ui_mp_active) {
        game_mp_client_store_state((struct game_s *)ctx, buf, len);
    }
    return r;
}
/* 1oom-mp save-file header magic. Distinguishes the [u32 magic][int humans][char game_id[24]] header
   from the OLD [int humans] one -- humans is 1..6, never equal to this, so the loader can tell them
   apart and still read pre-this-change autosaves. */
#define MP_SAVE_MAGIC 0x31535000u

/* write an MP save (header + the given state bytes) to fname, atomically: write a temp then rename,
   so a crash mid-write can't corrupt it. Used by the server (freshly serialized state) AND by clients
   storing the RAW authoritative blob they received (save sync) -- writing the received bytes verbatim
   guarantees every machine's copy is byte-identical to the host's. */
static bool mp_write_save_bytes(const uint8_t *bytes, int n, const char *fname) {
    char tmp[1040];
    FILE *f;
    uint32_t magic = MP_SAVE_MAGIC;
    int humans = s_mp_humans;
    bool ok;
    if (n <= 0) { return false; }
    /* per-process temp name: on the HOST machine the server AND the host's client (save sync) write
       the same files each turn -- a shared "<fname>.tmp" would race and could corrupt a rename. */
    lib_sprintf(tmp, sizeof(tmp), "%s.%d.tmp", fname, (int)getpid());
    f = fopen(tmp, "wb");
    if (!f) { return false; }
    ok = (fwrite(&magic, sizeof(magic), 1, f) == 1)
      && (fwrite(&humans, sizeof(humans), 1, f) == 1)            /* human-client count, so -mpload waits for the right number */
      && (fwrite(s_mp_game_id, sizeof(s_mp_game_id), 1, f) == 1) /* per-game id (the filename stem), carried across resumes */
      && (fwrite(bytes, 1, (size_t)n, f) == (size_t)n);
    if (fclose(f) != 0) { ok = false; }
    if (ok) {
#ifdef _WIN32
        remove(fname); /* Windows rename() won't overwrite an existing file */
#endif
        if (rename(tmp, fname) != 0) { remove(tmp); ok = false; }
    } else {
        remove(tmp);
    }
    return ok;
}

/* serialize the current state and write it as an MP save (header + blob). */
static bool mp_write_save_blob(struct game_s *g, const char *fname) {
    static uint8_t *buf = NULL;
    static int cap = 0;
    int need = game_save_blob_maxlen(g), n;
    if (need > cap) { uint8_t *nb = (uint8_t *)realloc(buf, (size_t)need); if (!nb) { return false; } buf = nb; cap = need; }
    n = game_save_blob_save(g, buf, cap);
    return mp_write_save_bytes(buf, n, fname);
}

/* 1oom-mp: per-turn autosave so a crash/restart never loses the game. Each game has its OWN file
   (mp_auto_<game_id>.blob) so starting a new game never clobbers an old game's autosave; resume any of
   them with -mpload. Cheap (~one save-sized write per turn). */
static void game_mp_autosave(struct game_s *g) {
    char fname[1024];
    os_make_path_user();
    if (s_mp_game_id[0]) { lib_sprintf(fname, sizeof(fname), "%s/mp_auto_%s.blob", os_get_path_user(), s_mp_game_id); }
    else { lib_sprintf(fname, sizeof(fname), "%s/mp_autosave.blob", os_get_path_user()); } /* fallback if no id was set */
    mp_write_save_blob(g, fname);
}

/* 1oom-mp: every game gets its own folder, <userdir>/games/<game_id>/, that accumulates one save blob
   per turn. Returns the folder path in buf and creates the tree (games/, then games/<id>/). os_make_path
   only makes one level at a time, so we create the parent first. */
static const char *game_mp_game_dir(char *buf, size_t bufsize) {
    char gamesdir[1024];
    os_make_path_user();
    lib_sprintf(gamesdir, sizeof(gamesdir), "%s/games", os_get_path_user());
    os_make_path(gamesdir);
    if (s_mp_game_id[0]) { lib_sprintf(buf, bufsize, "%s/%s", gamesdir, s_mp_game_id); }
    else { lib_sprintf(buf, bufsize, "%s/default", gamesdir); }
    os_make_path(buf);
    return buf;
}

/* 1oom-mp: keep an immutable per-turn save in this game's folder (games/<id>/y<year>.blob). Always on
   -- a save is only ~20 KB, so a whole game is a few MB -- so any game can be resumed at, or rewound to,
   any turn, and the full history can be re-analyzed later. Resuming an earlier turn simply overwrites
   the now-superseded later turns (a clean branch). */
static void game_mp_turnsave(struct game_s *g) {
    char dir[1024], fname[1152];
    int year = (int)g->year + YEAR_BASE;
    game_mp_game_dir(dir, sizeof(dir));
    lib_sprintf(fname, sizeof(fname), "%s/y%d.blob", dir, year);
    mp_write_save_blob(g, fname);
}

/* 1oom-mp: the named-saves folder, <userdir>/saves/. Player-chosen names live here so they're easy to
   find in the Resume list. Creates the directory. */
static const char *game_mp_saves_dir(char *buf, size_t bufsize) {
    os_make_path_user();
    lib_sprintf(buf, bufsize, "%s/saves", os_get_path_user());
    os_make_path(buf);
    return buf;
}

/* 1oom-mp: make a player-typed save name safe as a filename: keep alnum/space/dash/underscore/dot,
   replace everything else with '_', trim to fit. Same function runs on server and clients so every
   machine writes the identical filename (save sync). */
static void game_mp_sanitize_name(const char *in, char *out, size_t outsize) {
    size_t o = 0;
    for (size_t i = 0; in[i] && (o + 1 < outsize); ++i) {
        char c = in[i];
        bool ok = ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9'))
               || (c == ' ') || (c == '-') || (c == '_') || (c == '.');
        out[o++] = ok ? c : '_';
    }
    while ((o > 0) && (out[o - 1] == ' ')) { --o; } /* no trailing spaces (awkward on Windows) */
    out[o] = '\0';
}

/* 1oom-mp: an on-command save (a player chose Esc -> Save). A permanent NAMED snapshot, separate from
   the rolling autosave: saves/<player-chosen name>.blob (empty name -> the legacy y<year> naming). */
static void game_mp_save_named(struct game_s *g, const char *name) {
    char fname[1152];
    char clean[64];
    game_mp_sanitize_name(name ? name : "", clean, sizeof(clean));
    if (clean[0]) {
        char dir[1024];
        game_mp_saves_dir(dir, sizeof(dir));
        lib_sprintf(fname, sizeof(fname), "%s/%s.blob", dir, clean);
    } else {
        int year = (int)g->year + YEAR_BASE;
        os_make_path_user();
        if (s_mp_game_id[0]) { lib_sprintf(fname, sizeof(fname), "%s/mp_save_%s_y%d.blob", os_get_path_user(), s_mp_game_id, year); }
        else { lib_sprintf(fname, sizeof(fname), "%s/mp_save_y%d.blob", os_get_path_user(), year); }
    }
    if (mp_write_save_blob(g, fname)) { log_message("MP: saved game to %s\n", fname); }
    else { log_error("MP: save to %s FAILED\n", fname); }
}

/* 1oom-mp: env-gated (MP_AIDUMP) per-turn expansion trace. Appends one section per turn to
   <userdir>/mp_aidump_<game_id>.txt so the WHOLE game's AI-expansion history can be reviewed after
   the fact -- when/why an AI stopped colonizing -- not just the current snapshot. Mirrors
   game_ai_classic_turn_p1_send_colony_ships's target test exactly: can_colonize comes from the
   colony SHIP's module (not empire tech), and the classic AI also needs a colony ship within fuel
   range of the target (line-614 check). Cheap (a few fprintf's per turn). */
static void game_mp_aidump(struct game_s *g) {
    char fname[1024];
    FILE *f;
    if (!getenv("MP_AIDUMP")) { return; }
    game_update_within_range(g);
    os_make_path_user();
    if (s_mp_game_id[0]) { lib_sprintf(fname, sizeof(fname), "%s/mp_aidump_%s.txt", os_get_path_user(), s_mp_game_id); }
    else { lib_sprintf(fname, sizeof(fname), "%s/mp_aidump.txt", os_get_path_user()); }
    f = fopen(fname, "a");
    if (!f) { log_error("MP_AIDUMP: cannot open %s for append\n", fname); return; }
    fprintf(f, "=== year %d | %d empires | %d stars | ai_id=%d (CLASSIC=%d CLASSICPLUS=%d) ===\n",
        (int)g->year + YEAR_BASE, g->players, g->galaxy_stars,
        (int)g->ai_id, (int)GAME_AI_CLASSIC, (int)GAME_AI_CLASSICPLUS);
    for (player_id_t pi = PLAYER_0; pi < g->players; ++pi) {
        empiretechorbit_t *e = &g->eto[pi];
        int shipi = e->shipi_colony;
        int owned = 0, colships = 0, developing = 0;
        int reservefuel = ((shipi >= 0) && g->srd[pi].have_reserve_fuel[shipi]) ? 1 : 0;
        int hcontact = 0, expansionist = (e->trait2 == TRAIT2_EXPANSIONIST), send_colony;
        int range_units = e->fuel_range * 15;   /* line-614 source<->target distance budget */
        /* replicate send_colony_ships's can_colonize (the colony SHIP's module, not empire tech) */
        planet_type_t can_col = PLANET_TYPE_MINIMAL;
        if (shipi >= 0) {
            const shipdesign_t *sd = &g->srd[pi].design[shipi];
            for (int k = 0; k < SPECIAL_SLOT_NUM; ++k) {
                ship_special_t s = sd->special[k];
                if ((s >= SHIP_SPECIAL_STANDARD_COLONY_BASE) && (s <= SHIP_SPECIAL_RADIATED_COLONY_BASE)) {
                    can_col = PLANET_TYPE_MINIMAL - (s - SHIP_SPECIAL_STANDARD_COLONY_BASE);
                }
            }
        }
        if (e->race == RACE_SILICOID) { can_col = PLANET_TYPE_RADIATED; }
        int r1_total = 0, r1_typeok = 0, r1_wrongtype = 0, r1_enroute = 0, r1_noship = 0;
        int r1_toohostile = 0, r1_modulelag = 0;
        int r1_send_classic = 0, r1_send_plus = 0, r2_total = 0, r2_typeok = 0;
        for (int i = 0; i < g->galaxy_stars; ++i) {
            const planet_t *p = &g->planet[i];
            if (p->owner == pi) {
                owned++;
                if (shipi >= 0) { colships += e->orbit[i].ships[shipi]; }
                if ((p->missile_bases < (p->max_pop3 / 20)) && (p->pop < ((p->max_pop3 * 2) / 3))) { developing++; }
                continue;
            }
            if (p->owner != PLAYER_NONE) { continue; }
            int fr = p->within_frange[pi];
            if (fr == 2) { r2_total++; if (p->type >= can_col) { r2_typeok++; } continue; }
            if (fr != 1) { continue; }
            r1_total++;
            if (p->type < can_col) {
                r1_wrongtype++;
                if (p->type < e->have_colony_for) { r1_toohostile++; } else { r1_modulelag++; }
                continue;
            }
            r1_typeok++;
            int enr = 0;
            for (int j = 0; j < g->enroute_num; ++j) {
                if ((g->enroute[j].owner == pi) && (g->enroute[j].dest == i) && (shipi >= 0) && (g->enroute[j].ships[shipi] > 0)) { enr = 1; break; }
            }
            if (enr) { r1_enroute++; continue; }
            int mind = 1000000, found = 0;
            if (shipi >= 0) {
                for (int j = 0; j < g->galaxy_stars; ++j) {
                    if (e->orbit[j].ships[shipi] > 0) {
                        int d = util_math_dist_fast(p->x, p->y, g->planet[j].x, g->planet[j].y);
                        if (d < mind) { mind = d; found = 1; }
                    }
                }
            }
            if (!found) { r1_noship++; continue; }
            r1_send_plus++;                               /* CLASSICPLUS: any orbiting colony ship can be sent */
            if (mind <= range_units) { r1_send_classic++; } /* CLASSIC: source must also be within fuel range */
        }
        for (int i = 0; i < g->enroute_num; ++i) { if ((g->enroute[i].owner == pi) && (shipi >= 0)) { colships += g->enroute[i].ships[shipi]; } }
        for (player_id_t pj = PLAYER_0; pj < g->players; ++pj) { if ((pj != pi) && IS_HUMAN(g, pj) && BOOLVEC_IS1(g->eto[pj].contact, pi)) { hcontact = 1; } }
        send_colony = !((!expansionist) && ((owned / 2) < developing) && hcontact);
        fprintf(f, "  P%d race=%d %s planets=%2d colships=%d range=%d resfuel=%d can_col=%d have_col=%d SEND_COLONY=%d\n",
            pi, (int)e->race, IS_AI(g, pi) ? "AI " : "HUM", owned, colships, (int)e->fuel_range,
            reservefuel, (int)can_col, (int)e->have_colony_for, send_colony);
        fprintf(f, "      range1: unowned=%d type_ok=%d DELIVERABLE[classic=%d plus=%d] (wrongtype=%d[too_hostile=%d module_lag=%d] noship_inrange=%d enroute=%d) | range2: unowned=%d type_ok=%d\n",
            r1_total, r1_typeok, r1_send_classic, r1_send_plus, r1_wrongtype, r1_toohostile, r1_modulelag, r1_noship, r1_enroute, r2_total, r2_typeok);
    }
    fclose(f);
}

/* 1oom-mp: multiplayer runs with the upstream quality-of-life RULES FIXES ON (vanilla MOO1 economy
   quirks like ECO spending silently wasted on clean planets, waste/soil rounding errors, etc). Forced
   identically on the server AND every client (same build both ends), so views always agree; the game
   rules are the server's anyway. Single-player keeps the menu-configurable defaults. */
static void game_mp_force_rules(void) {
    game_num_slider_respects_locks = true;   /* a pinned slider stays pinned (pre-existing MP force) */
    game_num_waste_calc_fix = true;
    game_num_waste_adjust_fix = true;
    game_num_slider_eco_done_fix = true;     /* no ECO BC silently wasted on already-clean planets */
    game_num_soil_rounding_fix = true;
    game_num_pop_tenths_fix = true;
    game_num_factory_cost_fix = true;
    game_num_colonized_factories_fix = true;
    game_num_leaving_trans_fix = true;
    game_num_hidden_child_labor_fix = true;
    game_num_newtech_adjust_fix = true;
    game_num_cond_switch_to_ind_fix = true;
    game_num_orbital_bio_fix = true;         /* bio damage only applies when the attacker actually bombs */
    game_num_orbital_comp_fix = true;        /* no targeting bonus from ships that don't exist */
    game_num_stargate_redir_fix = true;
    game_num_trans_redir_fix = true;
    game_num_retreat_redir_fix = true;
    game_num_ship_scanner_fix = true;
}

/* server: a client requested a save (Esc -> Save) -> write a named snapshot of the authoritative state.
   (mp.c then broadcasts the same snapshot to every client so each machine stores its own copy.) */
static int mp_if_save_request(void *ctx, int player_id, const char *name) {
    (void)player_id;
    game_mp_save_named((struct game_s *)ctx, name);
    return 0;
}

/* server: the save-header identity clients prepend when storing synced state ([game_id 24][u8 humans]). */
static int mp_if_get_game_meta(void *ctx, uint8_t *buf, int buflen) {
    (void)ctx;
    if (buflen < 25) { return 0; }
    memcpy(buf, s_mp_game_id, 24);
    buf[24] = (uint8_t)s_mp_humans;
    return 25;
}

/* ----- 1oom-mp save sync, client side: every machine stores the authoritative state locally ----- */

/* client: GAME_META arrived -- adopt the game's save identity so the local writes use the same
   filenames (and header) as the host's. */
static void mp_cl_game_meta_impl(const char *game_id24, int humans) {
    lib_strcpy(s_mp_game_id, game_id24, sizeof(s_mp_game_id));
    if ((humans >= 1) && (humans <= PLAYER_NUM)) { s_mp_humans = humans; }
}

/* client: a player-named snapshot arrived -- write our own local copy (saves/<name>.blob), raw bytes
   verbatim so it's identical to the host's file. */
static void mp_cl_save_named_impl(const char *name, const uint8_t *blob, int len) {
    char dir[1024], fname[1152], clean[64];
    game_mp_sanitize_name(name, clean, sizeof(clean));
    if (!clean[0]) { lib_strcpy(clean, "unnamed", sizeof(clean)); }
    game_mp_saves_dir(dir, sizeof(dir));
    lib_sprintf(fname, sizeof(fname), "%s/%s.blob", dir, clean);
    if (mp_write_save_bytes(blob, len, fname)) { log_message("MP: stored synced save '%s'\n", clean); }
}

/* client: each authoritative GAME_DATA just loaded -- store it locally (rolling autosave + this turn's
   games/<id>/y<year>.blob), so ANY participant can host a resume without copying files around. */
static void game_mp_client_store_state(struct game_s *g, const uint8_t *blob, int len) {
    char fname[1152], dir[1024];
    if (!s_mp_game_id[0]) { return; } /* no GAME_META yet (pre-v4 server) -> nothing to file it under */
    os_make_path_user();
    lib_sprintf(fname, sizeof(fname), "%s/mp_auto_%s.blob", os_get_path_user(), s_mp_game_id);
    mp_write_save_bytes(blob, len, fname);
    game_mp_game_dir(dir, sizeof(dir));
    lib_sprintf(fname, sizeof(fname), "%s/y%d.blob", dir, (int)g->year + YEAR_BASE);
    mp_write_save_bytes(blob, len, fname);
}

/* 1oom-mp: the natural end-of-game result, stashed when advance_turn reports game-over so it can be
   shipped to clients (mp_if_get_game_over) for them to play the ending sequence. ge.name is a pointer
   into game state, copied into our own buffer for safe transport. */
static struct game_end_s s_mp_game_end;
static char s_mp_game_end_name[64];
static bool s_mp_game_over = false;

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
    game_mp_turnsave(g); /* keep this turn forever in the game's folder (games/<id>/y<year>.blob) */
    /* return >0 to tell mp_server_run the game has ended (someone won/lost) */
    {
        int over = ((ge.type == GAME_END_NONE) || (ge.type == GAME_END_FINAL_WAR)) ? 0 : 1;
        if (over) { /* 1oom-mp: stash the ending so the server can ship it to clients to play the sequence */
            s_mp_game_end = ge;
            if (ge.name) { lib_strcpy(s_mp_game_end_name, ge.name, sizeof(s_mp_game_end_name)); s_mp_game_end.name = s_mp_game_end_name; }
            else { s_mp_game_end_name[0] = 0; s_mp_game_end.name = NULL; }
            s_mp_game_over = true;
        }
        return over;
    }
}

/* 1oom-mp: serialize PLAYER player_id's OWN end-of-game result for its GAME_OVER. The game's outcome is
   one global value, but with multiple competing human teams one team wins and the rest lose -- so derive
   each viewer's ending here: a player on the winning team sees the victory; everyone else sees a defeat
   (a funeral after a conquest, exile after a council vote). For the common single-human-team game this
   also means the humans see their OWN funeral when an AI conquers them, instead of the AI's victory.
   POD fields + the winner name as a length-prefixed string (the struct's name pointer can't cross the wire). */
static int mp_if_get_game_over(void *ctx, int player_id, uint8_t *buf, int buflen) {
    struct game_s *g = (struct game_s *)ctx;
    struct game_end_s ge;
    const char *name;
    int pos = 0, namelen, win_emp = -1, win_team = -1;
    bool won;
    int32_t v; uint8_t nlen;
    if (!s_mp_game_over) { return 0; }
    if ((player_id < 0) || (player_id >= (int)g->players)) { return 0; }
    ge = s_mp_game_end; /* the global result (winner's race/name) is the base */
    /* find the winning empire -> its team */
    if ((ge.type == GAME_END_WON_GOOD) && (g->winner < g->players)) {
        win_emp = g->winner;                                                            /* the elected emperor */
    } else if (ge.type == GAME_END_WON_TYRANT) {
        for (player_id_t i = PLAYER_0; i < g->players; ++i) { if (IS_ALIVE(g, i)) { win_emp = (int)i; break; } } /* last team standing */
    }
    win_team = (win_emp >= 0) ? g->mp_team[win_emp] : -1;
    won = (win_emp >= 0) && ((player_id == win_emp) || ((win_team != 0) && (win_team == g->mp_team[player_id])));
    if (!won) {
        /* this viewer's team did not win -> a defeat ending instead of the global victory */
        if (ge.type == GAME_END_WON_GOOD) {
            ge.type = GAME_END_LOST_EXILE;                  /* the council elected someone else -> exile (name = the new emperor) */
        } else {
            ge.type = GAME_END_LOST_FUNERAL;
            /* funeral: banner_live = the victor's banner. When no winner empire resolved here (e.g. an
               AI conquered all humans -> the base ge already carries the killer's banner from
               game_turn_check_end), KEEP it instead of overwriting with banner 0. */
            if (win_emp >= 0) { ge.race = g->eto[win_emp].banner; }
            ge.banner_dead = g->eto[player_id].banner;             /* banner_dead = this viewer's own banner */
        }
    }
    name = (ge.type == GAME_END_LOST_FUNERAL) ? "" : s_mp_game_end_name; /* the funeral uses banners, not a name */
    namelen = name[0] ? (int)strlen(name) : 0;
    if (namelen > 63) { namelen = 63; }
    if (buflen < 4 * 4 + 1 + namelen) { return 0; }
    v = (int32_t)ge.type;        memcpy(buf + pos, &v, 4); pos += 4;
    v = (int32_t)ge.race;        memcpy(buf + pos, &v, 4); pos += 4;
    v = (int32_t)ge.banner_dead; memcpy(buf + pos, &v, 4); pos += 4;
    v = (int32_t)ge.varnum;      memcpy(buf + pos, &v, 4); pos += 4;
    nlen = (uint8_t)namelen; buf[pos++] = nlen;
    if (namelen) { memcpy(buf + pos, name, namelen); pos += namelen; }
    return pos;
}

/* client: a GAME_OVER with end-of-game info arrived -> play the matching ending sequence (mirrors the
   single-player switch in main_do). Shows the same authoritative outcome to everyone. */
static void mp_if_on_game_over(void *ctx, const uint8_t *buf, int len) {
    struct game_s *g = (struct game_s *)ctx;
    struct game_end_s ge;
    static char namebuf[64];
    int pos = 0; int32_t v; uint8_t nlen;
    if (len < 4 * 4 + 1) { return; } /* empty / link-drop GAME_OVER -> no ending */
    memset(&ge, 0, sizeof(ge));
    memcpy(&v, buf + pos, 4); pos += 4; ge.type = (game_end_type_t)v;
    memcpy(&v, buf + pos, 4); pos += 4; ge.race = (int)v;
    memcpy(&v, buf + pos, 4); pos += 4; ge.banner_dead = (int)v;
    memcpy(&v, buf + pos, 4); pos += 4; ge.varnum = (int)v;
    nlen = buf[pos++];
    if (nlen > 63) { nlen = 63; }
    if (len < pos + nlen) { return; }
    memcpy(namebuf, buf + pos, nlen); namebuf[nlen] = 0;
    ge.name = namebuf;
    ui_game_end(g);
    switch (ge.type) {
        case GAME_END_WON_GOOD:    ui_play_ending_good(ge.race, ge.name); break;
        case GAME_END_WON_TYRANT:  ui_play_ending_tyrant(ge.race, ge.name); break;
        case GAME_END_LOST_FUNERAL: ui_play_ending_funeral(ge.race, ge.banner_dead); break;
        case GAME_END_LOST_EXILE:  ui_play_ending_exile(ge.name); break;
        default: break;
    }
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
    /* 1oom-mp polish: nothing in flight -> nothing to animate; skip the stream so clients don't sit
       through an empty movement replay every quiet turn. (Monsters mid-flight ride in enroute-like
       state rarely enough that a missed monster-only frame is acceptable.) */
    if ((g->enroute_num == 0) && (g->transport_num == 0)) { return; }
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
static bool s_mp_battle_avail = false; /* 1oom-mp: a teammate's interactive battle is available to WATCH (opt-in); set on BATTLE_INIT, cleared on BATTLE_END. The observer loads the arena only if it presses the watch key. */

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
static struct ground_s s_mp_ground[32]; /* 1oom-mp: raised from 16 so a big multi-front turn doesn't drop invasion replays */
static int s_mp_ground_n = 0;

/* 1oom-mp: orbital bombings, same buffer+replay treatment as ground (concurrent across players). The
   planet snapshot is patched in at replay so ui_bomb_show sees the bomb-time owner, not a later one. */
struct mp_bomb_item_s { int32_t attacker, owner, planet, popdmg, factdmg; uint8_t play_music, hide_other; planet_t psnap; };
/* 1oom-mp: orbital bombings buffered as they stream in during resolution, replayed concurrently at the
   next state load -- so the bomb phase never blocks one player on another's bombardment screens. */
static struct mp_bomb_item_s s_mp_bomb[32]; /* 1oom-mp: raised from 16 (busy bombardment turn) */
static int s_mp_bomb_n = 0;
/* 1oom-mp: combat reports + turn messages are BUFFERED (instant ack) and replayed at state load --
   a blocking modal here would freeze every other player until this one clicked it away. */
static struct ui_combat_report_s s_mp_reports[12];
static int s_mp_reports_n = 0;
static char s_mp_turnmsgs[6][160];
static int s_mp_turnmsgs_n = 0;

/* 1oom-mp: espionage result streams, buffered as they stream in during resolution and replayed
   concurrently at the next state load -- so neither the tech-theft victim notice nor the saboteur's
   result screen blocks one player on the other's espionage. ui_spy_stolen needs no planet snapshot
   (it shows only race + tech); the sabotage result swaps in a post-sabotage planet snapshot at replay
   like ui_bomb_show does. The blocking framing choice (other2 != PLAYER_NONE) is NOT buffered. */
struct mp_stolen_item_s { int32_t spy, field; uint8_t tech; };
static struct mp_stolen_item_s s_mp_stolen[32]; /* 1oom-mp: raised from 16 */
static int s_mp_stolen_n = 0;
struct mp_sabres_item_s { int32_t spy, target, act, other1, other2, snum, planet; planet_t psnap; };
static struct mp_sabres_item_s s_mp_sabres[32]; /* 1oom-mp: raised from 16 */
static int s_mp_sabres_n = 0;

/* 1oom-mp: hysteresis for the "waiting for other players" banner. Counts consecutive on_wait ticks
   since the last relayed event; the banner only appears once this exceeds a threshold, so the brief
   gaps between events don't flash it on and off. Reset on every relayed decision. Wall-clock
   deadlines (each idle wait iteration is ~11ms, so iteration counts ran ~10x too long). */
static int64_t s_mp_wait_quiet_since_us = 0;

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
static int64_t s_mp_combat_hold_until_us = 0;
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
    if (s_mp_battle_avail) {
        /* 1oom-mp: a teammate is in combat -> OPT-IN. Show the "press W to watch" prompt instead of
           forcing the arena; load it (and switch to the spectate render next tick) only if pressed. */
        if (ui_mp_battle_watch_prompt()) {
            ui_battle_init_spectate(&s_mp_battle); /* s_mp_battle.g already set by BATTLE_INIT fixup */
            s_mp_battle_uictx = s_mp_battle.uictx;
            s_mp_battle_auto = false;
        }
        return;
    }
    if (reason == MP_WAIT_COUNCIL) { s_mp_combat_hold_until_us = 0; } /* the council banner (fallback if no chamber stream) takes priority over a stale combat notice */
    if (reason == MP_WAIT_COMBAT) { s_mp_combat_hold_until_us = hw_get_time_us() + 1500000; } /* another player just started/continued a battle: notice lingers ~1.5s */
    if (hw_get_time_us() < s_mp_combat_hold_until_us) {
        ui_mp_wait(MP_WAIT_COMBAT); /* show the combat notice (sticky; also takes priority over a stale audience screen) */
        return;
    }
    if (s_mp_audience_uictx) { ui_mp_wait(MP_WAIT_BATTLE); return; } /* mid AI-audience: keep its screen, just pump */
    if (s_mp_wait_quiet_since_us == 0) { s_mp_wait_quiet_since_us = hw_get_time_us(); }
    if ((hw_get_time_us() - s_mp_wait_quiet_since_us) < 700000) {
        ui_mp_wait(MP_WAIT_BATTLE); /* brief (~0.7s) gap between relayed events: just pump, no banner flash */
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
    s_mp_wait_quiet_since_us = 0; /* a relayed event just arrived -> hold off the "waiting" banner briefly (hysteresis) */
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
            } else { log_warning("MP: bombing-replay buffer full (%d) -- a bombing result won't be shown this turn\n", (int)(sizeof(s_mp_bomb) / sizeof(s_mp_bomb[0]))); }
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
                bool bomb_all = false; /* 1oom-mp QoL: "A" in the prompt = yes for the rest of my targets */
                for (int k = 0; k < n; ++k) {
                    if ((int)tgt[k].attacker == me) {
                        if (bomb_all || ui_bomb_ask(g, me, (planet_id_t)tgt[k].planet_i, tgt[k].pop_inbound)) { mask |= ((uint64_t)1 << k); }
                        if (!bomb_all && ui_bomb_ask_took_all()) { bomb_all = true; }
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
            } else { log_warning("MP: ground-invasion replay buffer full (%d) -- an invasion won't be shown this turn\n", (int)(sizeof(s_mp_ground) / sizeof(s_mp_ground[0]))); }
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
            } else { log_warning("MP: stolen-tech notice buffer full (%d) -- a 'tech stolen from you' notice won't show this turn\n", (int)(sizeof(s_mp_stolen) / sizeof(s_mp_stolen[0]))); }
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
            } else { log_warning("MP: sabotage-result buffer full (%d) -- a sabotage result won't show this turn\n", (int)(sizeof(s_mp_sabres) / sizeof(s_mp_sabres[0]))); }
            if (resp_buflen >= 1) { resp[0] = 1; } /* instant ack -- the screen plays later */
            return 1;
        }
        case MP_DEC_TURN_MSG: { /* a resolution-time message ("your transports were destroyed") --
                                   BUFFER it (instant ack, replayed at state load) so it never blocks
                                   the server / the other players on this one's click */
            if ((req_len > 0) && (s_mp_turnmsgs_n < (int)(sizeof(s_mp_turnmsgs) / sizeof(s_mp_turnmsgs[0])))) {
                int n = (req_len < (int)sizeof(s_mp_turnmsgs[0])) ? req_len : (int)(sizeof(s_mp_turnmsgs[0]) - 1);
                memcpy(s_mp_turnmsgs[s_mp_turnmsgs_n], req, n);
                s_mp_turnmsgs[s_mp_turnmsgs_n][n] = '\0';
                ++s_mp_turnmsgs_n;
            }
            if (resp_buflen >= 1) { resp[0] = 1; }
            return 1;
        }
        case MP_DEC_COMBAT_REPORT: { /* show this auto-resolved battle's result inline, right after the
                                        battle, instead of holding it to end-of-turn (so you don't wait
                                        on the other player to see your own outcome) */
            if (req_len < (int)sizeof(struct ui_combat_report_s)) { return 0; }
            /* BUFFER + instant ack (a blocking modal would hold the whole resolution on this player's
               click); all reports replay as one paged screen at state load */
            if (s_mp_reports_n < (int)(sizeof(s_mp_reports) / sizeof(s_mp_reports[0]))) {
                memcpy(&s_mp_reports[s_mp_reports_n++], req, sizeof(struct ui_combat_report_s));
            }
            if (resp_buflen >= 1) { resp[0] = 1; }
            return 1;
        }
        case MP_DEC_NEWS_ITEM: { /* buffer one turn-summary news record; replayed at state load */
            struct mp_news_item_s it;
            if (req_len < (int)sizeof(it)) { return 0; }
            memcpy(&it, req, sizeof(it));
            if (s_mp_news_n < (int)(sizeof(s_mp_news) / sizeof(s_mp_news[0]))) {
                s_mp_news[s_mp_news_n++] = it;
            } else { log_warning("MP: turn-news buffer full (%d) -- a GNN report won't show this turn\n", (int)(sizeof(s_mp_news) / sizeof(s_mp_news[0]))); }
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
        if (bt->s[side].flag_human) { g_mp_spectate_hook(bt->s[side].party, b, 7, 1/*reliable: ship move*/); }
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
    if ((data[0] == UI_MP_SPEC_BATTLE_INIT) && (len >= 1 + (int)sizeof(struct battle_s))) {
        /* 1oom-mp: a teammate's fight is starting -> OFFER it (opt-in). Stash the battle but DON'T
           load the arena; the observer chooses to watch from the wait screen (mp_if_on_wait). The
           fixup sets s_mp_battle.g so a later ui_battle_init_spectate works without g in hand. */
        memcpy(&s_mp_battle, data + 1, sizeof(s_mp_battle));
        s_mp_battle.uictx = NULL;
        mp_battle_fixup(g, &s_mp_battle);
        s_mp_battle_avail = true;
        return;
    }
    if ((data[0] == UI_MP_SPEC_BATTLE_END) && (len >= 1 + (int)sizeof(struct battle_s) + 5)) {
        /* 1oom-mp: the teammate's fight ended -> tear down our observer arena. */
        if (s_mp_battle_uictx) {
            uint8_t colony = data[1 + sizeof(struct battle_s)];
            int32_t winner; memcpy(&winner, data + 1 + sizeof(struct battle_s) + 1, 4);
            memcpy(&s_mp_battle, data + 1, sizeof(s_mp_battle));
            mp_battle_fixup(g, &s_mp_battle);
            s_mp_battle.uictx = s_mp_battle_uictx;
            ui_battle_shutdown(&s_mp_battle, colony != 0, winner);
            s_mp_battle_uictx = NULL;
        }
        s_mp_battle_avail = false; /* the fight is over: drop the watch offer / tear-down done */
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
    ui_mp_team_plan_reset(); /* a turn resolved: drop last turn's teammate overlay so stale fleets don't linger */
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
        if (s_mp_reports_n > 0) { /* this turn's combat reports, one paged screen (buffered, never blocked others) */
            ui_combat_report(g, mp_cl_player_id(), s_mp_reports, s_mp_reports_n);
            s_mp_reports_n = 0;
        }
        if (s_mp_turnmsgs_n > 0) { /* transport-loss and similar one-line results */
            for (int i = 0; i < s_mp_turnmsgs_n; ++i) { ui_turn_msg(g, mp_cl_player_id(), s_mp_turnmsgs[i]); }
            s_mp_turnmsgs_n = 0;
        }
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
static bool s_mp_dead = false;          /* eliminated -> spectator: auto-ready every turn */
static uint8_t *s_mp_orders_buf = NULL; /* the orders I last submitted (baseline for change detect) */
static uint8_t *s_mp_orders_tmp = NULL; /* scratch: current orders, to compare against the baseline */
static int s_mp_orders_cap = 0;
static int s_mp_orders_len = 0;
static bool s_mp_timer_active = false;      /* is a countdown currently running? */
static int64_t s_mp_timer_deadline_us = 0;  /* hw_get_time_us() value at which we auto-submit */

/* ---- 1oom-mp live teammate visibility: a teammate's relayed in-progress plan, for the overlay ---- */
struct mp_team_plan_s {
    bool active;
    int fleet_num;
    struct { uint16_t x, y, dest; } fleet[FLEET_ENROUTE_MAX];
    int col_num;
    uint16_t col[16];
    int sl_num;
    struct { uint16_t planet; planet_t p; } sl[PLANETS_MAX]; /* a teammate's owned worlds, full live state */
    int orbit_num;
    uint16_t orbit[PLANETS_MAX]; /* planets where this teammate has ships orbiting live this turn */
    uint16_t ping[3];            /* 1oom-mp teams: up to 3 planets this teammate flagged for the team, PLANET_NONE = empty */
};
static struct mp_team_plan_s s_team_plan[MP_MAX_PLAYERS];
static uint8_t *s_team_plan_buf = NULL;
static int s_team_plan_cap = 0;
static int s_mp_my_ping[3] = { PLANET_NONE, PLANET_NONE, PLANET_NONE }; /* 1oom-mp teams: up to 3 planets (beacons) I've flagged for my team this turn */

/* client: a teammate's plan snapshot arrived -> store it for the starmap overlay. */
static void mp_team_plan_recv(const void *data, int len) {
    const uint8_t *buf = (const uint8_t *)data;
    int pos = 0;
    uint16_t sender = 0, n = 0;
#define TPGET(dst, nn) do { if (pos + (int)(nn) > len) { return; } memcpy((dst), buf + pos, (nn)); pos += (int)(nn); } while (0)
    TPGET(&sender, 2);
    if ((sender >= MP_MAX_PLAYERS) || (s_mp_my_pid < 0)) { return; }
    if (!((game.mp_team[s_mp_my_pid] != 0) && (game.mp_team[s_mp_my_pid] == game.mp_team[sender]))) { return; }
    struct mp_team_plan_s *tp = &s_team_plan[sender];
    TPGET(&n, 2); tp->fleet_num = 0;
    for (int i = 0; i < (int)n; ++i) {
        uint16_t fx, fy, fd; TPGET(&fx, 2); TPGET(&fy, 2); TPGET(&fd, 2);
        if (tp->fleet_num < FLEET_ENROUTE_MAX) { tp->fleet[tp->fleet_num].x = fx; tp->fleet[tp->fleet_num].y = fy; tp->fleet[tp->fleet_num].dest = fd; ++tp->fleet_num; }
    }
    TPGET(&n, 2); tp->col_num = 0;
    for (int i = 0; i < (int)n; ++i) { uint16_t pli; TPGET(&pli, 2); if (tp->col_num < 16) { tp->col[tp->col_num++] = pli; } }
    TPGET(&n, 2); tp->sl_num = 0;
    for (int i = 0; i < (int)n; ++i) {
        uint16_t pli; TPGET(&pli, 2);
        if (tp->sl_num < PLANETS_MAX) {
            tp->sl[tp->sl_num].planet = pli;
            TPGET(&tp->sl[tp->sl_num].p, (int)sizeof(planet_t));
            ++tp->sl_num;
        } else {
            if (pos + (int)sizeof(planet_t) > len) { return; }
            pos += (int)sizeof(planet_t);
        }
    }
    tp->orbit_num = 0;
    TPGET(&n, 2);
    for (int i = 0; i < (int)n; ++i) { uint16_t pli; TPGET(&pli, 2); if (tp->orbit_num < PLANETS_MAX) { tp->orbit[tp->orbit_num++] = pli; } }
    for (int i = 0; i < 3; ++i) { tp->ping[i] = (uint16_t)PLANET_NONE; } /* 1oom-mp teams: up to 3 trailing map-beacons (back-compatible: absent -> none) */
    for (int i = 0; i < 3; ++i) { uint16_t pg; if (pos + 2 <= len) { memcpy(&pg, buf + pos, 2); pos += 2; tp->ping[i] = pg; } }
    tp->active = true;
#undef TPGET
    { static bool logged = false; if (!logged) { logged = true; log_message("MP: live team-plan relay active (first snapshot from P%d: %d fleets)\n", sender, tp->fleet_num); } }
}

/* client: called each planning frame -> stream my current plan to teammates (throttled). */
/* 1oom-mp: global chat -- a ring of the last few lines (best-effort overlay, NOT game state). Persists
   across turns (no per-turn reset), so recent messages stay visible. */
#define MP_CHAT_HIST 5
struct mp_chat_line_s { int sender; char text[MP_CHAT_MAX + 1]; };
static struct mp_chat_line_s s_chat[MP_CHAT_HIST];
static int s_chat_num = 0;       /* lines held (0..MP_CHAT_HIST) */
static int s_chat_head = 0;      /* index of the oldest held line */
static bool s_chat_min = false;  /* overlay minimized? */

/* client: a chat line arrived ([u16 sender][text]) -> push it into the ring (overwriting the oldest). */
static void mp_chat_recv(const void *data, int len) {
    const uint8_t *b = (const uint8_t *)data;
    int sender, tlen, slot;
    if (len < 2) { return; }
    sender = (b[0] << 8) | b[1];
    tlen = len - 2;
    if (tlen > MP_CHAT_MAX) { tlen = MP_CHAT_MAX; }
    slot = (s_chat_head + s_chat_num) % MP_CHAT_HIST;
    if (s_chat_num == MP_CHAT_HIST) { s_chat_head = (s_chat_head + 1) % MP_CHAT_HIST; }
    else { ++s_chat_num; }
    s_chat[slot].sender = sender;
    if (tlen > 0) { memcpy(s_chat[slot].text, b + 2, (size_t)tlen); }
    s_chat[slot].text[tlen] = '\0';
}

/* client: send a typed line to everyone (the server stamps the sender). */
void ui_mp_chat_send(const char *text) {
    int n;
    if (!g_mp_cl_chat_send || !text) { return; }
    n = (int)strlen(text);
    if (n <= 0) { return; }
    if (n > MP_CHAT_MAX) { n = MP_CHAT_MAX; }
    g_mp_cl_chat_send(text, n);
}

/* overlay accessors. i in [0, ui_mp_chat_count): 0 = oldest shown, last = newest. */
int ui_mp_chat_count(void) { return s_chat_num; }
bool ui_mp_chat_get(int i, int *sender, const char **text) {
    int idx;
    if ((i < 0) || (i >= s_chat_num)) { return false; }
    idx = (s_chat_head + i) % MP_CHAT_HIST;
    if (sender) { *sender = s_chat[idx].sender; }
    if (text) { *text = s_chat[idx].text; }
    return true;
}
bool ui_mp_chat_minimized(void) { return s_chat_min; }
void ui_mp_chat_toggle_min(void) { s_chat_min = !s_chat_min; }

void ui_mp_team_plan_tick(void) {
    static int ctr = 0;
    if (g_mp_team_plan_recv != mp_team_plan_recv) { g_mp_team_plan_recv = mp_team_plan_recv; }
    if (g_mp_chat_recv != mp_chat_recv) { g_mp_chat_recv = mp_chat_recv; } /* wire chat recv for ALL MP games (before the team gate) */
    if (!g_mp_cl_team_plan_send || (s_mp_my_pid < 0) || (game.mp_team[s_mp_my_pid] == 0)) { return; }
    if (++ctr < 8) { return; }
    ctr = 0;
    if (!s_team_plan_buf) { s_team_plan_cap = game_save_blob_maxlen(&game); s_team_plan_buf = (uint8_t *)malloc(s_team_plan_cap); if (!s_team_plan_buf) { return; } }
    {
        int len = game_mp_write_team_plan(&game, (player_id_t)s_mp_my_pid, s_team_plan_buf, s_team_plan_cap);
        if (len > 0) {
            for (int i = 0; (i < 3) && (len + 2 <= s_team_plan_cap); ++i) { uint16_t pg = (uint16_t)s_mp_my_ping[i]; memcpy(s_team_plan_buf + len, &pg, 2); len += 2; } /* 1oom-mp teams: append my up-to-3 map-beacons */
            g_mp_cl_team_plan_send(s_team_plan_buf, len);
        }
    }
}

/* starmap overlay accessors. */
/* 1oom-mp: clear the per-turn teammate-plan snapshots. Beacons (s_mp_my_ping) are NOT cleared here --
   they persist across turns until the player toggles them off, and are re-streamed each planning frame. */
void ui_mp_team_plan_reset(void) { for (int p = 0; p < MP_MAX_PLAYERS; ++p) { s_team_plan[p].active = false; } }
/* 1oom-mp teams: toggle a beacon on a planet. If it's already one of my up-to-3 beacons, remove it; else
   add it to a free slot; if all 3 slots are in use, do nothing (the player removes one first). */
void ui_mp_set_ping(int planet_i) {
    for (int i = 0; i < 3; ++i) { if (s_mp_my_ping[i] == planet_i) { s_mp_my_ping[i] = PLANET_NONE; return; } }
    for (int i = 0; i < 3; ++i) { if (s_mp_my_ping[i] == PLANET_NONE) { s_mp_my_ping[i] = planet_i; return; } }
}
/* the teammate (banner) who has a beacon on planet_i this turn, else -1. */
int ui_mp_team_plan_ping_at(int planet_i) {
    if (planet_i == PLANET_NONE) { return -1; }
    if (s_mp_my_pid >= 0) { for (int i = 0; i < 3; ++i) { if (s_mp_my_ping[i] == planet_i) { return s_mp_my_pid; } } } /* my own beacons, so I see what I flagged */
    for (int p = 0; p < MP_MAX_PLAYERS; ++p) { if (s_team_plan[p].active) { for (int i = 0; i < 3; ++i) { if (s_team_plan[p].ping[i] == (uint16_t)planet_i) { return p; } } } }
    return -1;
}
bool ui_mp_team_plan_active(int player) {
    return (player >= 0) && (player < MP_MAX_PLAYERS) && s_team_plan[player].active;
}
/* true if this teammate's overlay is live AND they have ships orbiting planet_i right now. */
bool ui_mp_team_plan_orbit_has(int player, int planet_i) {
    if ((player < 0) || (player >= MP_MAX_PLAYERS) || !s_team_plan[player].active) { return false; }
    for (int i = 0; i < s_team_plan[player].orbit_num; ++i) { if (s_team_plan[player].orbit[i] == planet_i) { return true; } }
    return false;
}
int ui_mp_team_plan_fleet_total(void) {
    int n = 0;
    for (int p = 0; p < MP_MAX_PLAYERS; ++p) { if (s_team_plan[p].active) { n += s_team_plan[p].fleet_num; } }
    return n;
}
bool ui_mp_team_plan_fleet_get(int idx, int *owner, int *x, int *y, int *dest) {
    for (int p = 0; p < MP_MAX_PLAYERS; ++p) {
        if (!s_team_plan[p].active) { continue; }
        if (idx < s_team_plan[p].fleet_num) {
            if (owner) { *owner = p; }
            if (x) { *x = s_team_plan[p].fleet[idx].x; }
            if (y) { *y = s_team_plan[p].fleet[idx].y; }
            if (dest) { *dest = s_team_plan[p].fleet[idx].dest; }
            return true;
        }
        idx -= s_team_plan[p].fleet_num;
    }
    return false;
}
/* the teammate about to colonize planet_i this turn (their banner for the marker), else -1. */
int ui_mp_team_plan_colonizer(int planet_i) {
    for (int p = 0; p < MP_MAX_PLAYERS; ++p) {
        if (!s_team_plan[p].active) { continue; }
        for (int i = 0; i < s_team_plan[p].col_num; ++i) { if (s_team_plan[p].col[i] == planet_i) { return p; } }
    }
    return -1;
}
/* a teammate's live planet snapshot (full state, for the read-only panel); NULL if none. */
const planet_t *ui_mp_team_plan_planet(int planet_i) {
    for (int p = 0; p < MP_MAX_PLAYERS; ++p) {
        if (!s_team_plan[p].active) { continue; }
        for (int i = 0; i < s_team_plan[p].sl_num; ++i) {
            if (s_team_plan[p].sl[i].planet == planet_i) { return &s_team_plan[p].sl[i].p; }
        }
    }
    return NULL;
}

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

/* called by mp.c when TIMER_START (seconds >= 0) or TIMER_CANCEL (seconds < 0) arrives */
static void mp_timer_notify_impl(int seconds) {
    if (seconds < 0) {
        s_mp_timer_active = false;
    } else {
        s_mp_timer_active = true;
        s_mp_timer_deadline_us = hw_get_time_us() + (int64_t)seconds * 1000000LL;
    }
}

static bool mp_turn_active_impl(void) { return g_mp_cl_poll != NULL; }
static bool mp_turn_is_ready_impl(void) { return s_mp_ready; }
/* returns remaining countdown seconds, or -1 when no timer is active */
static int mp_turn_timer_remaining_s_impl(void) {
    if (!s_mp_timer_active) { return -1; }
    int64_t rem = s_mp_timer_deadline_us - hw_get_time_us();
    if (rem <= 0) { return 0; }
    return (int)(rem / 1000000LL);
}
/* "Next Turn": toggle my ready lock (submitting my current orders when I lock in). */
static void mp_turn_set_ready_impl(int ready) { mp_submit_orders(ready != 0); }
/* pump the socket once; if the countdown expired, auto-submit our orders; returns true once
   the server signalled resolution (planning is over). */
/* 1oom-mp: transient one-line banner note ("ORDERS UPDATED", "TIME'S UP"), so silent automatic
   actions give visible feedback on the starmap for a couple of seconds. */
static char s_mp_note[48];
static int64_t s_mp_note_until_us = 0;
static void mp_set_note(const char *txt, int tenths) {
    lib_strcpy(s_mp_note, txt, sizeof(s_mp_note));
    s_mp_note_until_us = hw_get_time_us() + (int64_t)tenths * 100000;
}
static const char *mp_turn_note_impl(void) {
    return (s_mp_note[0] && (hw_get_time_us() < s_mp_note_until_us)) ? s_mp_note : NULL;
}

static bool mp_turn_poll_impl(void) {
    if (s_mp_dead && !s_mp_ready) { mp_submit_orders(true); } /* eliminated spectator never blocks the turn */
    if (s_mp_ready && mp_orders_changed()) {
        mp_submit_orders(true);
        mp_set_note("ORDERS UPDATED", 15); /* feedback: the post-Ready edit was re-sent */
    }
    if (s_mp_timer_active && !s_mp_ready && hw_get_time_us() >= s_mp_timer_deadline_us) {
        mp_submit_orders(true); /* timer expired: submit whatever we have */
        mp_set_note("TIME'S UP - TURN SUBMITTED", 25);
    }
    return g_mp_cl_poll ? (g_mp_cl_poll() != 0) : false;
}

/* 1oom-mp: name of a (human) player everyone is still waiting on, for the READY banner -- from the
   server's READY_STATUS broadcast. NULL when unknown or nobody is blocking. */
static const char *mp_turn_waiting_race_impl(void) {
    if (g_mp_cl_ready_nplayers == 0) { return NULL; }
    for (int i = 0; (i < g_mp_cl_ready_nplayers) && (i < (int)game.players) && (i < 8); ++i) {
        if ((i != s_mp_my_pid) && !(g_mp_cl_ready_mask & (1u << i)) && IS_HUMAN(&game, i)) {
            return game_str_tbl_races[game.eto[i].race];
        }
    }
    return NULL;
}

/* 1oom-mp: race name of a DISCONNECTED human (empire on autopilot until they rejoin), or NULL. */
static const char *mp_turn_disconnected_race_impl(void) {
    if (g_mp_cl_ready_nplayers == 0) { return NULL; }
    for (int i = 0; (i < g_mp_cl_ready_nplayers) && (i < (int)game.players) && (i < 8); ++i) {
        if ((i != s_mp_my_pid) && !(g_mp_cl_conn_mask & (1u << i)) && IS_HUMAN(&game, i)) {
            return game_str_tbl_races[game.eto[i].race];
        }
    }
    return NULL;
}

/* 1oom-mp: true once the planning timer has run out but we haven't submitted yet. Nested UI screens
   (ship design, planet transfer, research, ...) run their own input loops that never pump the timer,
   so the auto-submit can't fire there and the turn stalls for everyone. Those loops poll this and bail
   out (the shared input handler returns ESC) so the player unwinds back to the starmap, where
   mp_turn_poll_impl auto-submits and the turn resolves. Clears the instant mp_submit_orders sets ready. */
static bool mp_turn_force_unwind_impl(void) {
    return s_mp_timer_active && !s_mp_ready && (hw_get_time_us() >= s_mp_timer_deadline_us);
}

bool (*ui_mp_turn_active)(void) = mp_turn_active_impl;
void (*ui_mp_turn_set_ready)(int ready) = mp_turn_set_ready_impl;
bool (*ui_mp_turn_is_ready)(void) = mp_turn_is_ready_impl;
bool (*ui_mp_turn_poll)(void) = mp_turn_poll_impl;
int (*ui_mp_turn_timer_remaining_s)(void) = mp_turn_timer_remaining_s_impl;
const char *(*ui_mp_turn_waiting_race)(void) = mp_turn_waiting_race_impl;
const char *(*ui_mp_turn_note)(void) = mp_turn_note_impl;
const char *(*ui_mp_turn_disconnected_race)(void) = mp_turn_disconnected_race_impl;
bool (*ui_mp_turn_force_unwind)(void) = mp_turn_force_unwind_impl;
bool ui_mp_active = false; /* true while this process is a networked client (set in game_mp_join) */
bool game_mp_is_server = false; /* true while this process is the headless MP server (set in game_mp_host) */

/* client: the human plays their empire's turn through the real game UI, then we
   serialize their orders. Combat/resolution happen on the server, not here. */
static int mp_if_write_orders(void *ctx, int player_id, uint8_t *buf, int buflen) {
    struct game_s *g = (struct game_s *)ctx;
    s_mp_my_pid = player_id;   /* soft-ready: who I am, for the Ready-submit hook */
    s_mp_ready = false;        /* a fresh turn starts un-readied */
    s_mp_timer_active = false; /* any pending countdown from the previous turn is gone */
    g_mp_cl_ready_nplayers = 0; /* last turn's ready-status is stale */
    game_mp_orders_reset();    /* clear last turn's queued diplo/colonize actions before this turn re-queues */
    s_mp_dead = !IS_ALIVE(g, player_id); /* eliminated -> spectate: the poll auto-readies so we never block the turn */
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

static int mp_if_get_team(void *ctx, int player) {
    (void)ctx;
    return ((player >= 0) && (player < (int)game.players)) ? game.mp_team[player] : 0;
}

/* 1oom-mp: a player's race id, so the server can hand a -mprace joiner the empire it asked for. */
static int mp_if_player_race(void *ctx, int player) {
    (void)ctx;
    return ((player >= 0) && (player < (int)game.players)) ? (int)game.eto[player].race : -1;
}

/* 1oom-mp: fingerprint the raw-struct wire layout. The relays memcpy whole structs (battle/election/
   audience/ground) and the orders/team-plan ship per-planet state, and GAME_DATA is the field-by-field
   save blob keyed to the build's save version -- all of which assume the SAME build on both ends. Fold
   the sizes of the big relayed structs + the whole game state into one id; the server compares it at
   HELLO and refuses a client whose build differs, instead of letting a layout skew silently corrupt the
   wire. sizeof(struct game_s) is the broad catch-all (it contains eto/srd/enroute/transport/mp_team, so
   any save-format-relevant change moves it). FNV-1a mix. */
static int mp_if_wire_id(void *ctx) {
    (void)ctx;
    uint32_t v = 0x811c9dc5u;
    v = (v ^ (uint32_t)sizeof(struct game_s))     * 16777619u;
    v = (v ^ (uint32_t)sizeof(struct battle_s))   * 16777619u;
    v = (v ^ (uint32_t)sizeof(planet_t))          * 16777619u;
    v = (v ^ (uint32_t)sizeof(struct election_s)) * 16777619u;
    v = (v ^ (uint32_t)sizeof(struct audience_s)) * 16777619u;
    v = (v ^ (uint32_t)sizeof(struct ground_s))   * 16777619u;
    v = (v ^ (uint32_t)MP_PROTO_VERSION)          * 16777619u;
    return (int)v;
}

static mp_game_iface_t mp_make_iface(void) {
    mp_game_iface_t gi;
    gi.ctx = &game;
    gi.get_team = mp_if_get_team;
    gi.player_race = mp_if_player_race;
    gi.wire_id = mp_if_wire_id;
    gi.get_game_over = mp_if_get_game_over;
    gi.on_game_over = mp_if_on_game_over;
    gi.save_request = mp_if_save_request;
    gi.get_game_meta = mp_if_get_game_meta; /* save sync: identity header for client-side stores */
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

/* 1oom-mp: load an MP save (autosave or named) into `game`. New header is [u32 magic][int humans]
   [char game_id[24]]; pre-this-change autosaves are just [int humans] (no magic) and still load (game_id
   then comes up empty -> the caller stamps a fresh one). Sets s_mp_game_id so a resumed game keeps
   autosaving to the same file. Caller must have game_aux_init'd; game_aux_start/game_start follow.
   Reports the recorded human-client count via *humans_out. */
static int game_mp_load_autosave(const char *fname, int *humans_out) {
    FILE *f = fopen(fname, "rb");
    long fsize;
    int humans = 0, rc, hdr;
    uint32_t magic = 0;
    size_t blen, got;
    uint8_t *buf;
    s_mp_game_id[0] = 0;
    if (!f) { log_error("MP: cannot open save '%s'\n", fname); return 1; }
    fseek(f, 0, SEEK_END); fsize = ftell(f); fseek(f, 0, SEEK_SET);
    if (fread(&magic, sizeof(magic), 1, f) != 1) { fclose(f); log_error("MP: save '%s' header read failed\n", fname); return 1; }
    if (magic == MP_SAVE_MAGIC) { /* new format: [magic][humans][game_id] */
        if ((fread(&humans, sizeof(int), 1, f) != 1) || (fread(s_mp_game_id, sizeof(s_mp_game_id), 1, f) != 1)) {
            fclose(f); log_error("MP: save '%s' header truncated\n", fname); return 1;
        }
        s_mp_game_id[sizeof(s_mp_game_id) - 1] = 0;
        hdr = (int)sizeof(magic) + (int)sizeof(int) + (int)sizeof(s_mp_game_id);
    } else { /* old format: those first 4 bytes were the humans count */
        humans = (int)magic;
        hdr = (int)sizeof(int);
        fseek(f, hdr, SEEK_SET);
    }
    if (fsize <= hdr) { fclose(f); log_error("MP: save '%s' too small\n", fname); return 1; }
    blen = (size_t)fsize - (size_t)hdr;
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

/* 1oom-mp: stamp a fresh per-game id (a timestamp) used as the autosave/named-save filename stem. */
static void game_mp_set_game_id(void) {
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    if (!lt || (strftime(s_mp_game_id, sizeof(s_mp_game_id), "%Y%m%d_%H%M%S", lt) == 0)) {
        lib_sprintf(s_mp_game_id, sizeof(s_mp_game_id), "%lu", (unsigned long)t); /* fallback */
    }
}

/* 1oom-mp: resolve a -mpload argument. If it names a directory (a game folder under games/), return a
   freshly-allocated path to its latest-turn save (highest y<year>.blob) so "resume" picks up the last
   turn by default. To resume a SPECIFIC turn instead, point -mpload straight at games/<id>/y<year>.blob.
   A plain file (or a folder with no turn saves) is returned unchanged. Caller frees the result. */
static char *game_mp_resolve_load_path(const char *path) {
    struct stat st;
    if ((stat(path, &st) == 0) && S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *de;
            int best = -1;
            char bestname[64] = {0};
            while ((de = readdir(d)) != NULL) {
                int y = 0;
                if ((sscanf(de->d_name, "y%d.blob", &y) == 1) && (y > best)) {
                    best = y;
                    lib_strcpy(bestname, de->d_name, sizeof(bestname));
                }
            }
            closedir(d);
            if (best >= 0) {
                size_t n = strlen(path) + 1 + strlen(bestname) + 1;
                char *full = lib_malloc(n);
                lib_sprintf(full, n, "%s/%s", path, bestname);
                log_message("MP: resuming game folder %s at latest turn (%s)\n", path, bestname);
                return full;
            }
            log_error("MP: -mpload folder '%s' has no y<year>.blob turn saves\n", path);
        }
    }
    return lib_stralloc(path);
}

static int game_mp_host(void) {
    int humans, rc;
    bool resume = (opt_mp_load != NULL);
    mp_game_iface_t gi;
    game_mp_is_server = true; /* keep human newtech lists in the synced state (see game_turn.c) */
    if (game_aux_init(&game_aux, &game)) { return 1; }
    game_mp_force_rules(); /* 1oom-mp: MP always runs with the QoL rules fixes on (same on all ends) */
    if (resume) {
        /* resume a crashed/saved game: the save blob takes the place of game_new (it also restores the
           per-game id, so we keep autosaving to the same mp_auto_<id>.blob). */
        {   /* a -mpload folder resumes its latest turn; a y<year>.blob resumes that exact turn */
            char *loadpath = game_mp_resolve_load_path(opt_mp_load);
            int lr = game_mp_load_autosave(loadpath, &humans);
            lib_free(loadpath);
            if (lr) { game_aux_shutdown(&game_aux); return 1; }
        }
        if (s_mp_game_id[0] == 0) { game_mp_set_game_id(); } /* an old-format save carried no id -> stamp a fresh one */
    } else {
        /* honor -new (galaxy size, races, empire count, difficulty); GAME_NEW_OPTS_DEFAULT otherwise */
        struct game_new_options_s opts = game_opt_new;
        game_mp_set_game_id(); /* fresh game -> a new per-game autosave id */
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
    game_mp_aidump(&game); /* MP_AIDUMP: write the loaded/initial state as the first trace section */
    /* arm turn-movement playback: snapshot the pre-movement state each turn so the
       client can animate fleets moving (see mp_premove_capture / mp_if_get_movement). */
    s_mp_premove_cap = game_save_blob_maxlen(&game);
    s_mp_premove_buf = (uint8_t *)malloc(s_mp_premove_cap);
    game_mp_premove_hook = s_mp_premove_buf ? mp_premove_capture : NULL;
    g_mp_battle_move_hook = mp_battle_move_relay; /* server: relay in-combat ship moves to spectators */
    log_message("MP: %s on port %d, %d empires (%d human, %d AI), %d stars\n", resume ? "resumed" : "hosting", opt_mp_host_port, game.players, humans, (int)game.players - humans, game.galaxy_stars);
    gi = mp_make_iface();
    if (resume) { gi.setup_game = NULL; } /* skip the lobby; the loaded game IS the initial state sent to clients */
    int mp_open = (opt_mp_open > 0) && !resume; /* open lobby is a fresh-game thing, not for resume */
    int mp_cap = mp_open ? opt_mp_open : humans;
    if (mp_cap > MP_MAX_PLAYERS) { mp_cap = MP_MAX_PLAYERS; }
    if (mp_cap < 1) { mp_cap = 1; }
    rc = mp_server_run((uint16_t)opt_mp_host_port, mp_cap, 0 /*until disconnect*/, &gi, mp_open);
    game_mp_premove_hook = NULL;
    g_mp_battle_move_hook = NULL;
    free(s_mp_premove_buf); s_mp_premove_buf = NULL;
    game_aux_shutdown(&game_aux);
    return rc ? 1 : 0;
}

/* 1oom-mp: map a -mprace argument (race name -- singular or plural, case-insensitive -- or a 0-9 id) to a
   race id, or -1 if unrecognized. */
static int mp_race_id_from_arg(const char *s) {
    if (!s || !s[0]) { return -1; }
    {   /* a bare number 0..RACE_NUM-1 */
        char *end; long v = strtol(s, &end, 10);
        if ((*end == '\0') && (v >= 0) && (v < RACE_NUM)) { return (int)v; }
    }
    for (int r = 0; r < RACE_NUM; ++r) {
        const char *names[2]; names[0] = game_str_tbl_race[r]; names[1] = game_str_tbl_races[r];
        for (int k = 0; k < 2; ++k) {
            const char *a = s, *b = names[k];
            while (*a && *b) {
                char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
                char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
                if (ca != cb) { break; }
                ++a; ++b;
            }
            if (!*a && !*b) { return r; }
        }
    }
    return -1;
}

/* join addr ("host[:port]") as a client. The core of -mpjoin, also reached from the Multiplayer menu. */
static int game_mp_join_addr(const char *addr) {
    char host[128];
    uint16_t port = MP_DEFAULT_PORT;
    ui_mp_active = true; /* enable client-side turn-start prompts (e.g. planet discovery) */
    const char *colon = strrchr(addr, ':');
    if (colon) {
        size_t n = (size_t)(colon - addr);
        if (n >= sizeof(host)) { n = sizeof(host) - 1; }
        memcpy(host, addr, n);
        host[n] = '\0';
        port = (uint16_t)atoi(colon + 1);
    } else {
        size_t n = strlen(addr);
        if (n >= sizeof(host)) { n = sizeof(host) - 1; }
        memcpy(host, addr, n);
        host[n] = '\0';
    }
    if (game_aux_init(&game_aux, &game)) { return 1; }
    game_mp_force_rules(); /* 1oom-mp: MP always runs with the QoL rules fixes on (same on all ends) */
    log_message("MP: joining %s:%u\n", host, (unsigned)port);
    mp_game_iface_t gi = mp_make_iface();
    gi.advance_turn = NULL; /* the client does not resolve turns */
    g_mp_cl_timer_notify = mp_timer_notify_impl;
    g_mp_cl_game_meta = mp_cl_game_meta_impl;   /* save sync: adopt the game's save identity */
    g_mp_cl_save_named = mp_cl_save_named_impl; /* save sync: store player-named snapshots locally */
    int rc = mp_client_run(host, port, 0 /*until game over*/, &gi);
    g_mp_cl_timer_notify = NULL;
    g_mp_cl_game_meta = NULL;
    g_mp_cl_save_named = NULL;
    game_aux_shutdown(&game_aux);
    return rc ? 1 : 0;
}

static int game_mp_join(void) {
    if (opt_mp_race) { /* -mprace: ask the server (resumed game) for this race's empire instead of next free slot */
        g_mp_cl_req_race = mp_race_id_from_arg(opt_mp_race);
        if (g_mp_cl_req_race < 0) { log_warning("MP: -mprace '%s' not recognized -- joining by connection order\n", opt_mp_race); }
        else { log_message("MP: will request race '%s' (id %d) on join\n", opt_mp_race, g_mp_cl_req_race); }
    }
    return game_mp_join_addr(opt_mp_join);
}

/* 1oom-mp: describe an MP save for the Multiplayer menu's Resume list (see ui.h). Loads the state
   into the (pre-game) game struct just to read who's in it -- harmless before a session starts. */
int game_mp_peek_save(const char *path, int *humans_out, uint8_t *races_out, int *year_out) {
    char *resolved = game_mp_resolve_load_path(path);
    int humans = 0, r;
    r = game_mp_load_autosave(resolved, &humans);
    lib_free(resolved);
    if (r) { return -1; }
    if (humans_out) { *humans_out = humans; }
    if (races_out) {
        for (int i = 0; i < PLAYER_NUM; ++i) { races_out[i] = (i < (int)game.players) ? (uint8_t)game.eto[i].race : 0xff; }
    }
    if (year_out) { *year_out = (int)game.year + YEAR_BASE; }
    return 0;
}

/* 1oom-mp: find the server binary that ships next to this client (mac/linux build: 1oom_server;
   the Windows bundle renames it Server.exe). */
static bool game_mp_find_server_bin(char *buf, size_t bufsize) {
    static const char *cand[] = { "1oom_server", "Server.exe", "1oom_server.exe", NULL };
    char dir[1024];
    os_get_path_exe_dir(dir, sizeof(dir));
    for (int i = 0; cand[i]; ++i) {
        FILE *f;
        if (dir[0]) { lib_sprintf(buf, bufsize, "%s/%s", dir, cand[i]); }
        else { lib_sprintf(buf, bufsize, "%s", cand[i]); }
        f = fopen(buf, "rb");
        if (f) { fclose(f); return true; }
    }
    return false;
}

/* 1oom-mp: launch a session from the Multiplayer menu (ui_mp_setup filled by the menu screens).
   HOST/RESUME spawn the sibling server binary locally, then join it as a normal client; JOIN just
   connects. Returns the exit code, or -1 to go back to the menu (setup failed). */
static int game_mp_menu_launch(int action) {
    char addr[144];
    int spawned = -1, rc;
    game_aux_shutdown(&game_aux); /* the menu path inited it; game_mp_join_addr re-inits */
    g_mp_cl_req_race = ui_mp_setup.req_race;
    if (((action == MAIN_MENU_ACT_MP_HOST) || (action == MAIN_MENU_ACT_MP_RESUME)) && net_probe_local_port(24695)) {
        /* a server from a previous session (e.g. a crashed client) is still running on this machine --
           rejoin it instead of spawning a second one that couldn't bind the port anyway */
        log_message("MP: a game server is already running on this machine -- rejoining it\n");
        lib_strcpy(addr, "127.0.0.1:24695", sizeof(addr));
        rc = game_mp_join_addr(addr);
        if (rc != 0) { ui_mp_active = false; }
        game_aux_init(&game_aux, &game);
        ui_mp_active = false;
        g_mp_cl_req_race = -1;
        return -1; /* back to the menu after the session (or failed rejoin) */
    }
    if ((action == MAIN_MENU_ACT_MP_HOST) || (action == MAIN_MENU_ACT_MP_RESUME)) {
        char bin[1152], humans[8], logf[1088];
        const char *argv[12];
        int n = 0;
        if (!game_mp_find_server_bin(bin, sizeof(bin))) {
            log_error("MP: server binary not found next to this program -- cannot host\n");
            game_aux_init(&game_aux, &game);
            return -1;
        }
        lib_sprintf(humans, sizeof(humans), "%d", ui_mp_setup.humans);
        lib_sprintf(logf, sizeof(logf), "%s/mp_server_log.txt", os_get_path_user());
        argv[n++] = bin;
        argv[n++] = "-mphost"; argv[n++] = "24695";
        /* OPEN lobby for a new game: the lobby appears immediately, players join as they arrive (up
           to the cap) or leave freely, and the host clicks Start -- no all-or-nothing blocking. A
           RESUME keeps the fixed count (it's baked into the save). */
        if (action == MAIN_MENU_ACT_MP_HOST) { argv[n++] = "-mpopen"; argv[n++] = humans; }
        else { argv[n++] = "-mpload"; argv[n++] = ui_mp_setup.load_path; }
        if (os_get_path_data() && os_get_path_data()[0]) { argv[n++] = "-data"; argv[n++] = os_get_path_data(); }
        argv[n++] = "-log"; argv[n++] = logf;
        argv[n] = NULL;
        spawned = os_spawn_bg(argv);
        if (spawned < 0) {
            log_error("MP: could not start the server (%s)\n", bin);
            game_aux_init(&game_aux, &game);
            return -1;
        }
        log_message("MP: local server started (%s)\n", bin);
        lib_strcpy(addr, "127.0.0.1:24695", sizeof(addr));
        /* keep a frame on screen during the spawn+connect gap (the menu just faded to black) */
        for (int i = 0; i < 5; ++i) { ui_mp_wait(0); usleep(100000); }
    } else {
        lib_strcpy(addr, ui_mp_setup.join_addr, sizeof(addr));
        /* remember the address for next time (persisted via the config file) */
        lib_free(ui_mp_last_addr);
        ui_mp_last_addr = lib_stralloc(addr);
    }
    if (spawned >= 0) {
        /* our own just-spawned server may still be loading -- retry FAST failures (connection refused)
           for up to ~10 attempts instead of racing a fixed sleep. A long-lived session that later ends
           with an error isn't retried (elapsed >= 10s). */
        for (int tries = 0; ; ++tries) {
            time_t t0 = time(NULL);
            rc = game_mp_join_addr(addr);
            if ((rc == 0) || (tries >= 10) || ((time(NULL) - t0) >= 10)) { break; }
            for (int i = 0; i < 9; ++i) { ui_mp_wait(0); usleep(100000); } /* keep drawing between attempts */
        }
    } else {
        rc = game_mp_join_addr(addr);
        if (rc != 0) {
            /* JOIN failed (bad address / host down / version mismatch) -> back to the menu, not exit */
            log_warning("MP: join '%s' failed -- returning to the menu\n", addr);
            ui_mp_active = false;
            game_aux_init(&game_aux, &game);
            return -1;
        }
    }
    /* do NOT kill the spawned server after a REAL session: if the host steps away, the others keep
       playing and the server self-exits once everyone has left. But a server whose game NEVER STARTED
       (join failed, or the host backed out of the pre-game wait) must be reaped -- a leftover would
       squat on the port and wedge the next hosting attempt with its dead half-connection. */
    if ((spawned >= 0) && ((rc != 0) || !g_mp_cl_game_started)) { os_spawn_kill(spawned); }
    /* session over (game finished, quit to menu, or link lost) -> back to the MAIN MENU, not the
       desktop, so a rematch doesn't mean relaunching the app. A window-close still exits via the
       hw layer's quit path before we get here. */
    ui_mp_active = false;
    g_mp_cl_req_race = -1;
    game_aux_init(&game_aux, &game);
    return -1;
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
            case MAIN_MENU_ACT_MP_HOST:
            case MAIN_MENU_ACT_MP_RESUME:
            case MAIN_MENU_ACT_MP_JOIN:
                {   /* 1oom-mp: launch the session the Multiplayer menu set up; -1 = back to the menu */
                    int r = game_mp_menu_launch(main_menu_action);
                    if (r >= 0) { return r; }
                }
                continue;
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
