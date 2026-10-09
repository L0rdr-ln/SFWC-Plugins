/*
 * Fire for the `fire` window animation: a small particle simulation and its software drawing.
 * No wlroots dependency (unit-tested in tests/test_fire.c). The compositor drives it from the
 * output frame callbacks and shows the result in a scene buffer next to the burning window.
 */
#ifndef SFWC_FX_FIRE_H
#define SFWC_FX_FIRE_H

#include <stdbool.h>
#include <stdint.h>

#include <cairo.h>

struct fire_sim;

/* `max_particles` 1..5000, `size` = radius of a flame in px, `rgb` = the main flame color
 * (0xRRGGBB). The same seed gives the same flames. */
struct fire_sim *fire_sim_new(int max_particles, int size, uint32_t rgb, uint32_t seed);
void fire_sim_free(struct fire_sim *f);

/* Advance by `dt` seconds (clamped to 0.1). While `emit` is set, new flames start along the
 * horizontal line from x0 to x1 at height y (flames rise: y decreases). Coordinates are in
 * pixels of the caller's choosing. */
void fire_sim_step(struct fire_sim *f, double dt, double x0, double x1, double y, bool emit);

/* Draw the flames into a cleared ARGB32 surface. A flame at (x, y) lands at
 * ((x - ox) * scale, (y - oy) * scale), so the surface can be a smaller picture of an area that
 * starts at (ox, oy). Additive blending, premultiplied alpha. */
void fire_sim_draw(const struct fire_sim *f, cairo_surface_t *surface, double ox, double oy,
                   double scale);

int fire_sim_count(const struct fire_sim *f);
/* The highest point (smallest y) of any flame, for tests; false when there are none. */
bool fire_sim_top(const struct fire_sim *f, double *y);

#endif
