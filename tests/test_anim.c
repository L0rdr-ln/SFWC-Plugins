/* Unit tests for src/anim.c. */
#include <math.h>
#include <stdio.h>

#include "anim.h"

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define NEAR(a, b) (fabs((a) - (b)) < 1e-9)

int main(void)
{
    enum anim_easing e;
    CHECK(anim_easing_from_name("linear", &e) && e == EASE_LINEAR);
    CHECK(anim_easing_from_name("ease-in", &e) && e == EASE_IN);
    CHECK(anim_easing_from_name("ease-out", &e) && e == EASE_OUT);
    CHECK(anim_easing_from_name("ease-in-out", &e) && e == EASE_IN_OUT);
    CHECK(!anim_easing_from_name("bounce", &e));

    /* every curve is anchored at both ends and clamps its input */
    for (int c = EASE_LINEAR; c <= EASE_IN_OUT; c++) {
        CHECK(NEAR(anim_ease(c, 0), 0) && NEAR(anim_ease(c, 1), 1));
        CHECK(NEAR(anim_ease(c, -3), 0) && NEAR(anim_ease(c, 7), 1));
        double prev = -1; /* monotonic */
        for (int i = 0; i <= 100; i++) {
            double v = anim_ease(c, i / 100.0);
            CHECK(v >= prev);
            prev = v;
        }
    }
    CHECK(NEAR(anim_ease(EASE_LINEAR, 0.3), 0.3));
    CHECK(NEAR(anim_ease(EASE_IN, 0.5), 0.125));
    CHECK(NEAR(anim_ease(EASE_OUT, 0.5), 0.875));
    CHECK(NEAR(anim_ease(EASE_IN_OUT, 0.5), 0.5));
    CHECK(NEAR(anim_ease(EASE_IN_OUT, 0.25), 0.0625));
    CHECK(NEAR(anim_ease(EASE_IN_OUT, 0.75), 0.9375));
    CHECK(anim_ease(EASE_IN, 0.3) < 0.3 && anim_ease(EASE_OUT, 0.3) > 0.3); /* slow start / fast start */

    CHECK(NEAR(anim_progress(1000, 1000, 200), 0));
    CHECK(NEAR(anim_progress(1000, 1050, 200), 0.25));
    CHECK(NEAR(anim_progress(1000, 1200, 200), 1));
    CHECK(NEAR(anim_progress(1000, 5000, 200), 1));
    CHECK(NEAR(anim_progress(1000, 1000, 0), 1)); /* no duration: already finished */
    /* the 32-bit millisecond clock wraps around after ~49 days */
    CHECK(NEAR(anim_progress(0xfffffff0u, 0x00000010u, 64), 0.5));

    CHECK(NEAR(anim_lerp(10, 20, 0.5), 15));
    CHECK(NEAR(anim_lerp(20, 10, 0.25), 17.5));

    /* cubic Bézier curves */
    struct anim_curve lin = {0, 0, 1, 1};
    for (int i = 0; i <= 20; i++) {
        double t = i / 20.0;
        CHECK(fabs(anim_curve_eval(&lin, t) - t) < 1e-5); /* (0,0,1,1) is the identity */
    }
    struct anim_curve ease = {0.25, 0.1, 0.25, 1.0}, def;
    CHECK(anim_builtin_curve("ease", &def) && def.x0 == ease.x0 && def.y0 == ease.y0);
    CHECK(anim_builtin_curve("default", &def) && def.y0 == 0.75);
    CHECK(!anim_builtin_curve("nope", &def));
    /* CSS reference values: ease(0.5) ~ 0.8024, ease-in-out(0.25) ~ 0.1288 (to 3 digits) */
    CHECK(fabs(anim_curve_eval(&ease, 0.5) - 0.8024) < 0.002);
    struct anim_curve io;
    anim_builtin_curve("ease-in-out", &io);
    CHECK(fabs(anim_curve_eval(&io, 0.25) - 0.1288) < 0.002);
    CHECK(fabs(anim_curve_eval(&io, 0.5) - 0.5) < 1e-4);
    for (const char *const *n = (const char *const[]){"linear", "default", "ease", "ease-in", "ease-out",
                                                    "ease-in-out", NULL};
         *n; n++) {
        struct anim_curve c;
        CHECK(anim_builtin_curve(*n, &c) && anim_curve_valid(&c));
        CHECK(NEAR(anim_curve_eval(&c, 0), 0) && NEAR(anim_curve_eval(&c, 1), 1));
        CHECK(NEAR(anim_curve_eval(&c, -1), 0) && NEAR(anim_curve_eval(&c, 2), 1));
        double prev = -1; /* these are monotonic */
        for (int i = 0; i <= 200; i++) {
            double v = anim_curve_eval(&c, i / 200.0);
            CHECK(v >= prev - 1e-9);
            prev = v;
        }
    }
    /* an overshooting curve leaves [0,1] but still ends at 1; extreme control points stay stable */
    struct anim_curve over = {0.05, 0.9, 0.1, 1.05};
    CHECK(anim_curve_valid(&over));
    double peak = 0;
    for (int i = 0; i <= 200; i++) {
        double v = anim_curve_eval(&over, i / 200.0);
        CHECK(isfinite(v));
        if (v > peak) {
            peak = v;
        }
    }
    CHECK(peak > 1.0 && peak < 1.1 && NEAR(anim_curve_eval(&over, 1), 1));
    struct anim_curve flat = {0, 0, 0, 0}, steep = {1, 1, 1, 1};
    for (int i = 0; i <= 50; i++) {
        CHECK(isfinite(anim_curve_eval(&flat, i / 50.0)) && isfinite(anim_curve_eval(&steep, i / 50.0)));
    }
    struct anim_curve bad = {1.5, 0, 0.5, 1}, bad2 = {0.5, 0, -0.1, 1}, nan = {0.5, NAN, 0.5, 1};
    CHECK(!anim_curve_valid(&bad) && !anim_curve_valid(&bad2) && !anim_curve_valid(&nan));

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_anim: all checks passed\n");
    return 0;
}
