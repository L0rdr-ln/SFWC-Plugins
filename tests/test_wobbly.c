/* Unit tests for wobbly/mesh.c (no compositor needed). */
#include <math.h>
#include <stdio.h>

#include "mesh.h"

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define NEAR(a, b, tol) (fabs((a) - (b)) < (tol))

int main(void)
{
    /* at rest: the corners and the middle are where the window is */
    struct wobbly *w = wobbly_new(6, 120, 9);
    double x, y;
    CHECK(w && wobbly_settled(w)); /* nothing yet */
    wobbly_set_rect(w, 100, 50, 200, 100);
    CHECK(wobbly_settled(w) && wobbly_max_offset(w) == 0);
    wobbly_point(w, 0, 0, &x, &y);
    CHECK(NEAR(x, 100, 1e-9) && NEAR(y, 50, 1e-9));
    wobbly_point(w, 1, 1, &x, &y);
    CHECK(NEAR(x, 300, 1e-9) && NEAR(y, 150, 1e-9));
    wobbly_point(w, 0.5, 0.5, &x, &y);
    CHECK(NEAR(x, 200, 1e-9) && NEAR(y, 100, 1e-9));
    wobbly_point(w, 2, -1, &x, &y); /* outside is clamped */
    CHECK(NEAR(x, 300, 1e-9) && NEAR(y, 50, 1e-9));
    for (int i = 0; i < 100; i++) {
        wobbly_step(w, 1 / 60.0);
    }
    CHECK(wobbly_settled(w)); /* stays at rest */

    /* the window jumps 300 px to the right: the nodes lag, swing and settle on the new place */
    wobbly_set_rect(w, 400, 50, 200, 100);
    CHECK(!wobbly_settled(w) && NEAR(wobbly_max_offset(w), 300, 1e-6));
    wobbly_step(w, 0.1);
    CHECK(wobbly_max_offset(w) > 20 && wobbly_max_offset(w) < 400);
    wobbly_point(w, 0, 0.5, &x, &y);
    CHECK(x < 400 + 1e-6 || x > 400); /* finite, whatever side it is on */
    CHECK(isfinite(x) && isfinite(y));
    for (int i = 0; i < 60 * 15; i++) {
        wobbly_step(w, 1 / 60.0);
    }
    CHECK(wobbly_settled(w));
    wobbly_point(w, 0, 0, &x, &y);
    CHECK(NEAR(x, 400, 0.3) && NEAR(y, 50, 0.3));
    wobbly_point(w, 1, 1, &x, &y);
    CHECK(NEAR(x, 600, 0.3) && NEAR(y, 150, 0.3));

    /* a grab: nodes near the held point follow, the far ones lag more */
    wobbly_free(w);
    w = wobbly_new(6, 120, 9);
    wobbly_set_rect(w, 0, 0, 300, 200);
    wobbly_set_grab(w, true, 0, 100); /* held at the left edge */
    wobbly_set_rect(w, 200, 0, 300, 200);
    wobbly_set_grab(w, true, 200, 100);
    wobbly_step(w, 0.08);
    CHECK(wobbly_node_offset(w, 0, 2) < wobbly_node_offset(w, 5, 2)); /* left (held) < right (far) */
    CHECK(wobbly_node_offset(w, 5, 2) > 20);
    wobbly_free(w);

    /* without a grab every node lags the same way: the window moves as a whole, no deformation */
    w = wobbly_new(5, 120, 9);
    wobbly_set_rect(w, 0, 0, 300, 200);
    wobbly_set_rect(w, 100, 0, 300, 200);
    wobbly_step(w, 0.05);
    CHECK(NEAR(wobbly_node_offset(w, 0, 0), wobbly_node_offset(w, 4, 4), 0.5));
    CHECK(NEAR(wobbly_node_offset(w, 2, 2), wobbly_node_offset(w, 0, 3), 0.5));

    /* a new size puts the nodes at rest at once (the shape would be meaningless) */
    wobbly_set_rect(w, 100, 0, 360, 240);
    CHECK(wobbly_settled(w) && wobbly_max_offset(w) == 0);
    wobbly_free(w);

    /* a slow, sluggish setting lags for a long time but never overshoots */
    w = wobbly_new(6, 6, 5);
    wobbly_set_rect(w, 0, 0, 200, 100);
    wobbly_set_rect(w, 300, 0, 200, 100);
    double prev = wobbly_max_offset(w);
    for (int i = 0; i < 90; i++) { /* 1.5 s */
        wobbly_step(w, 1 / 60.0);
        double o = wobbly_max_offset(w);
        CHECK(o <= prev + 1e-6); /* only ever approaches */
        prev = o;
    }
    CHECK(prev > 10 && prev < 300); /* after 1.5 s it is still on its way */
    for (int i = 0; i < 60 * 12; i++) {
        wobbly_step(w, 1 / 60.0);
    }
    CHECK(wobbly_settled(w));
    wobbly_free(w);

    /* extreme settings and huge steps stay finite and settle */
    w = wobbly_new(100, 100000, 0); /* grid clamped to 16, spring to 1000, friction to 0.5 */
    wobbly_set_rect(w, 0, 0, 800, 600);
    wobbly_set_grab(w, true, 10, 10);
    wobbly_set_rect(w, 1000, 800, 800, 600);
    for (int i = 0; i < 400; i++) {
        wobbly_step(w, 1.0); /* clamped to 0.1 s */
        wobbly_point(w, 0.5, 0.5, &x, &y);
        CHECK(isfinite(x) && isfinite(y) && fabs(x) < 1e6 && fabs(y) < 1e6);
    }
    for (int i = 0; i < 6000 && !wobbly_settled(w); i++) {
        wobbly_step(w, 0.1);
    }
    CHECK(wobbly_settled(w));
    wobbly_free(w);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_wobbly: all checks passed\n");
    return 0;
}
