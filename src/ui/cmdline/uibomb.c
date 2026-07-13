#include "config.h"

#include <stdio.h>

#include "ui.h"
#include "game.h"
#include "game_aux.h"
#include "game_str.h"
#include "lib.h"
#include "types.h"
#include "uidefs.h"
#include "uiinput.h"
#include "uiswitch.h"

/* -------------------------------------------------------------------------- */

bool ui_bomb_ask_batch(struct game_s *g, const struct ui_bomb_target_s *targets, int n, bool *decided)
{
    (void)g; (void)targets; (void)n; (void)decided;
    return false;
}

bool ui_bomb_ask_took_all(void) { return false; } /* 1oom-mp QoL: classic-UI only */
void ui_mp_tech_note_seen(uint8_t field, uint8_t tech) { (void)field; (void)tech; } /* classic-UI only */

bool ui_bomb_ask(struct game_s *g, int pi, planet_id_t planet_i, int pop_inbound)
{
    const planet_t *p = &(g->planet[planet_i]);
    const empiretechorbit_t *e = &(g->eto[p->owner]);
    char buf[0x80];
    int v;
    ui_switch_1(g, pi);
    printf("%s : %s %s, %s %i, %s %i", p->name, game_str_tbl_race[e->race], game_str_sm_colony, game_str_sb_pop, p->pop, game_str_sb_fact, p->factories);
    if (pop_inbound) {
        printf(", %i %s", pop_inbound, (pop_inbound == 1) ? game_str_sm_trinb1 : game_str_sm_trinb1s);
    }
    putchar('\n');
    lib_sprintf(buf, sizeof(buf), "%s %s", game_str_sm_bomb1, game_str_sm_bomb2);
    v = ui_input_list(buf, "> ", il_yes_no);
    return (v == 1);
}

void ui_bomb_show(struct game_s *g, int pi, int attacker_i, int owner_i, planet_id_t planet_i, int popdmg, int factdmg, bool play_music, bool hide_other)
{
    const planet_t *p = &(g->planet[planet_i]);
    ui_switch_2(g, attacker_i, owner_i);
    printf("%s : %s %s. ", p->name, game_str_sm_obomb1, game_str_sm_obomb2);
    {
        const char *s;
        if ((g->gaux->local_players == 1) && IS_HUMAN(g, owner_i)) {
            s = game_str_sb_your;
        } else {
            s = game_str_tbl_race[g->eto[owner_i].race];
        }
        printf("%s %s", s, game_str_sm_colony);
    }
    if (p->owner == PLAYER_NONE) {
        printf(". %s %s", game_str_sm_cdest1, game_str_sm_cdest2);
    } else if ((popdmg == 0) && (factdmg == 0)) {
        printf(". %s %s", game_str_sm_ineff1, game_str_sm_ineff2);
    } else {
        if (popdmg) {
            printf(", %i %s %s", popdmg, game_str_sm_bkill1, game_str_sm_bkill2);
        }
        if (factdmg) {
            printf(", %i %s %s", factdmg, (factdmg == 1) ? game_str_sm_bfact1 : game_str_sm_bfact1s, game_str_sm_bfact2);
        }
    }
    putchar('\n');
    ui_switch_wait(g);
}
