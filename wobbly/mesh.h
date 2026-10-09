/*
 * Wobbly windows: a grid of masses on springs under the window. When the window moves, the grid
 * lags behind and then swings back, so the window looks like jelly. No wlroots dependency
 * (unit-tested in tests/test_wobbly.c); wobbly.c turns the grid into scene buffers.
 */
#ifndef SFWC_FX_WOBBLY_H
#define SFWC_FX_WOBBLY_H

#include <stdbool.h>

struct wobbly;

/* `grid` = nodes per side (2..16); `spring`: how hard a node is pulled to its place (1..1000);
 * `friction`: damping (0.5..100). A damping of about 2*sqrt(spring) is critical (no overshoot);
 * less swings, more is sluggish. */
struct wobbly *wobbly_new(int grid, double spring, double friction);
void wobbly_free(struct wobbly *w);

/* Where the window is now (its rest place). A change of position moves the springs' rest points
 * and the nodes follow with a delay; a change of size, or the first call, puts the nodes at rest. */
void wobbly_set_rect(struct wobbly *w, double x, double y, double width, double height);

/* The point of the window that is held (the pointer while dragging): nodes near it follow the
 * window closely, the others lag. Without a grab all nodes lag the same. */
void wobbly_set_grab(struct wobbly *w, bool has, double gx, double gy);

void wobbly_step(struct wobbly *w, double dt);

/* All nodes at rest and not moving (within a fraction of a pixel). */
bool wobbly_settled(const struct wobbly *w);

/* Where the point (u, v), both 0..1 across the window, is now. */
void wobbly_point(const struct wobbly *w, double u, double v, double *x, double *y);

/* Largest distance of a node from its rest place, for tests and for deciding what to draw. */
double wobbly_max_offset(const struct wobbly *w);
/* The same for one node (a, b), 0 <= a, b < grid. */
double wobbly_node_offset(const struct wobbly *w, int a, int b);

#endif
