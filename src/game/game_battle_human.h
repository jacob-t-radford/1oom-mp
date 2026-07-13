#ifndef INC_1OOM_GAME_BATTLE_HUMAN_H
#define INC_1OOM_GAME_BATTLE_HUMAN_H

#include "types.h"

#define BATTLE_XY_SET(_x_, _y_) (((_x_) << 3) + (_y_))
#define BATTLE_XY_GET_X(_v_)    (((_v_) >> 3) & 0xf)
#define BATTLE_XY_GET_Y(_v_)    ((_v_) & 7)
#define BATTLE_XY_INVALID       0xff

struct battle_s;
struct battle_item_s;
struct shiptech_weap_s;

extern void game_battle_area_setup(struct battle_s *bt);
/* 1oom-mp teams: mark one owner's ships as AI-piloted for the current battle (the pre-battle "Auto"
   choice in a combined fleet -- same path as the mid-battle Auto button). Call AFTER battle init. */
extern void game_battle_owner_auto_set(int owner);
extern int game_battle_area_check_line_ok(struct battle_s *bt, int *tblx, int *tbly, int len);
extern void game_battle_item_move(struct battle_s *bt, int itemi, int sx, int sy);
/* 1oom-mp: set on the headless server only — fired at the start of a ship move so the server can
   relay the destination to spectating clients (which glide the ship). NULL in single-player/client. */
extern void (*g_mp_battle_move_hook)(struct battle_s *bt, int itemi, int sx, int sy);
extern int game_battle_get_xy_notsame(const struct battle_s *bt, int item1, int item2, int *x_notsame);
extern bool game_battle_attack(struct battle_s *bt, int attacker_i, int target_i, bool retaliate);
extern int game_battle_get_absorbdiv(const struct battle_item_s *b, const struct shiptech_weap_s *w, bool force_oracle_check);

extern bool game_battle_with_human(struct battle_s *bt); /* true if right side won */

#endif
