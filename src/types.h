#ifndef INC_1OOM_TYPES_H
#define INC_1OOM_TYPES_H

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

/* 1oom-mp: star/planet index. 16-bit so galaxies can exceed 254 stars.
   PLANET_NONE (game_planet.h) is the 0xffff sentinel. */
typedef uint16_t planet_id_t;

#endif
