#ifndef INC_1OOM_GAME_TURN_H
#define INC_1OOM_GAME_TURN_H

#include "game_end.h"
#include "types.h"

struct game_s;

extern struct game_end_s game_turn_process(struct game_s *g);

/* The turn's fleet-movement animation phase (steps every fleet toward its
   destination, redrawing the galaxy map). Exposed so the MP client can replay it
   on the server's pre-movement snapshot to show ships moving. */
extern void game_turn_move_ships(struct game_s *g);

/* 1oom-mp: when non-NULL, called at the start of game_turn_move_ships (from the
   null UI's ui_gmap_basic_init) so the headless server can snapshot the
   pre-movement state and stream it to clients for animation. NULL otherwise. */
extern void (*game_mp_premove_hook)(struct game_s *g);

/* 1oom-mp colonization (used by the MP client to offer the colonize prompt and by
   the server to apply it). can_colonize: does pi have a colony ship in orbit at pli
   that could colonize it (and pli is unowned/habitable)? colonize_with_ship: do it
   (set owner, consume the ship); returns false if no longer possible. */
extern bool game_planet_can_colonize_with_ship(const struct game_s *g, player_id_t pi, planet_id_t pli);
extern bool game_planet_colonize_with_ship(struct game_s *g, player_id_t pi, planet_id_t pli);
/* the orbit ship-design slot of a colony ship that could colonize pli for pi, or -1 (the client
   uses this to consume the colony ship on colonize so the player can't move it away). */
extern int mp_colony_ship_for(const struct game_s *g, player_id_t pi, planet_id_t pli);

extern int copyprot_status;

#endif
