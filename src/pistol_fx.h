#ifndef PISTOL_FX_H
#define PISTOL_FX_H

#include "types.h"

/* Queue a pistol tracer from the muzzle to the impact point. */
void pistol_fx_add(const vector_t *from, const vector_t *to);

/* Draw and age active tracers. Call once per frame before the page flip. */
void pistol_fx_draw(void);

#endif /* PISTOL_FX_H */
