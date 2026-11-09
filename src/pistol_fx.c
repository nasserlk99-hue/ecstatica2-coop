/**
 * pistol_fx.c
 *
 * Co-op pistol visuals: a short-lived tracer line and muzzle flash drawn
 * directly onto the framebuffer before the page flip.
 *
 * E1's thing table has no bullet/tracer actor (thing_tab[4] is the "warrior"
 * model), so spawning a thing for the pistol used to summon a warrior statue.
 * Drawing the tracer ourselves avoids touching game data and looks the same
 * in E1 and E2.
 */

#include "pistol_fx.h"
#include "display.h"
#include "init.h"

#define PISTOL_FX_MAX   8
#define PISTOL_FX_LIFE  6   /* frames the tracer stays visible (~100 ms) */

/* Palette slots 244-246 are outside both the game's own reserved entries and
 * the debug overlay's 247-255 range. */
#define FX_COL_CORE   244
#define FX_COL_TRACER 245

typedef struct {
    vector_t from;
    vector_t to;
    int life;
} pistol_tracer_t;

static pistol_tracer_t tracers[PISTOL_FX_MAX];
static int next_tracer;

void pistol_fx_add(const vector_t *from, const vector_t *to) {
    if (!from || !to) return;
    pistol_tracer_t *t = &tracers[next_tracer];
    next_tracer = (next_tracer + 1) % PISTOL_FX_MAX;
    t->from = *from;
    t->to = *to;
    t->life = PISTOL_FX_LIFE;
}

static void fx_pixel(int x, int y, uint8_t color) {
    if (!bitmap[db]) return;
    if (x < 0 || x >= screen_width || y < 0 || y >= screen_height) return;
    bitmap[db][y * hires_width + x] = (char)color;
}

static void fx_line(int x0, int y0, int x1, int y1, uint8_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        fx_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

/* Same projection the debug overlay uses: world -> view space -> screen. */
static int fx_project(const vector_t *world, int *sx, int *sy) {
    vector_t out;
    view_transform(&out, (vector_t *)world);
    if (out.Z == 0) return 0;
    *sx = screen_centre_x + (out.X >> 4);
    *sy = screen_centre_y + (out.Y >> 4);
    return 1;
}

static void inject_fx_palette(void) {
    /* 6-bit VGA components, matching the rest of the palette tables. */
    colour_map[FX_COL_CORE].R   = 63; colour_map[FX_COL_CORE].G   = 63; colour_map[FX_COL_CORE].B   = 45;
    colour_map[FX_COL_TRACER].R = 63; colour_map[FX_COL_TRACER].G = 55; colour_map[FX_COL_TRACER].B = 0;
    view_cmap[FX_COL_CORE]   = colour_map[FX_COL_CORE];
    view_cmap[FX_COL_TRACER] = colour_map[FX_COL_TRACER];
}

void pistol_fx_draw(void) {
    if (!bitmap[db]) return;
    inject_fx_palette();

    for (int i = 0; i < PISTOL_FX_MAX; i++) {
        pistol_tracer_t *t = &tracers[i];
        if (t->life <= 0) continue;

        int x0, y0, x1, y1;
        if (fx_project(&t->from, &x0, &y0) && fx_project(&t->to, &x1, &y1)) {
            fx_line(x0, y0, x1, y1, FX_COL_TRACER);

            /* Bright 3x3 impact dot. */
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    fx_pixel(x1 + dx, y1 + dy, FX_COL_CORE);
        }

        /* Muzzle flash for the first two frames. */
        if (t->life >= PISTOL_FX_LIFE - 1) {
            int mx, my;
            if (fx_project(&t->from, &mx, &my)) {
                for (int dy = -2; dy <= 2; dy++)
                    for (int dx = -2; dx <= 2; dx++)
                        if (dx * dx + dy * dy <= 5)
                            fx_pixel(mx + dx, my + dy, FX_COL_CORE);
            }
        }

        t->life--;
    }
}
