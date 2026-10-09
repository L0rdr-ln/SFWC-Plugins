/* Unit tests for src/fx_fire.c (no compositor needed). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fire.h"

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void px(cairo_surface_t *s, int x, int y, int *r, int *g, int *b, int *a)
{
    unsigned char *d = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    uint32_t v = *(uint32_t *)(d + (size_t)y * stride + (size_t)x * 4);
    *a = v >> 24;
    *r = (v >> 16) & 0xff;
    *g = (v >> 8) & 0xff;
    *b = v & 0xff;
}

/* number of pixels in rows [y0, y1) that are clearly flame colored (orange / yellow) */
static int fiery(cairo_surface_t *s, int y0, int y1)
{
    int n = 0, w = cairo_image_surface_get_width(s);
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < w; x++) {
            int r, g, b, a;
            px(s, x, y, &r, &g, &b, &a);
            if (a > 40 && r >= 150 && r > b + 50) {
                n++;
            }
        }
    }
    return n;
}

static int lit(cairo_surface_t *s, int y0, int y1)
{
    int n = 0, w = cairo_image_surface_get_width(s);
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < w; x++) {
            int r, g, b, a;
            px(s, x, y, &r, &g, &b, &a);
            n += a > 0;
        }
    }
    return n;
}

static cairo_surface_t *frame(struct fire_sim *f, double ox, double oy, double scale, int w, int h)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    fire_sim_draw(f, s, ox, oy, scale);
    return s;
}

int main(void)
{
    /* limits */
    struct fire_sim *f = fire_sim_new(0, 1, 0xff7a18, 1);
    CHECK(f && fire_sim_count(f) == 0);
    for (int i = 0; i < 200; i++) {
        fire_sim_step(f, 1 / 60.0, 0, 200, 100, true);
    }
    CHECK(fire_sim_count(f) >= 1 && fire_sim_count(f) <= 1); /* clamped to one particle */
    fire_sim_free(f);
    f = fire_sim_new(100000, 14, 0xff7a18, 1);
    for (int i = 0; i < 400; i++) {
        fire_sim_step(f, 1 / 60.0, 0, 800, 100, true);
        CHECK(fire_sim_count(f) <= 5000);
    }
    CHECK(fire_sim_count(f) > 100);
    fire_sim_free(f);

    /* flames start on the line, rise, and die out when nothing feeds them */
    f = fire_sim_new(400, 14, 0xff7a18, 7);
    double top;
    CHECK(!fire_sim_top(f, &top));
    for (int i = 0; i < 40; i++) {
        fire_sim_step(f, 1 / 60.0, 20, 180, 150, true);
    }
    CHECK(fire_sim_count(f) > 20 && fire_sim_count(f) <= 400);
    CHECK(fire_sim_top(f, &top) && top < 150 - 20 && top > 150 - 400);
    cairo_surface_t *s = frame(f, 0, 0, 1.0, 200, 200);
    CHECK(fiery(s, 40, 170) > 50);   /* flames around and above the line */
    CHECK(lit(s, 185, 200) == 0);    /* nothing far below it */
    cairo_surface_destroy(s);
    for (int i = 0; i < 120; i++) {
        fire_sim_step(f, 1 / 60.0, 20, 180, 150, false);
    }
    CHECK(fire_sim_count(f) == 0);
    fire_sim_free(f);

    /* a huge time step is clamped, not exploded */
    struct fire_sim *a = fire_sim_new(400, 14, 0xff7a18, 3), *b = fire_sim_new(400, 14, 0xff7a18, 3);
    fire_sim_step(a, 10.0, 0, 100, 50, true);
    fire_sim_step(b, 0.1, 0, 100, 50, true);
    CHECK(fire_sim_count(a) == fire_sim_count(b));
    fire_sim_step(a, -1, 0, 100, 50, true); /* negative: ignored */
    CHECK(fire_sim_count(a) == fire_sim_count(b));

    /* the same seed gives the same flames, another seed does not */
    struct fire_sim *c = fire_sim_new(400, 14, 0xff7a18, 4);
    for (int i = 0; i < 50; i++) {
        fire_sim_step(a, 1 / 60.0, 10, 190, 150, true);
        fire_sim_step(b, 1 / 60.0, 10, 190, 150, true);
        fire_sim_step(c, 1 / 60.0, 10, 190, 150, true);
    }
    cairo_surface_t *sa = frame(a, 0, 0, 1, 200, 200), *sb = frame(b, 0, 0, 1, 200, 200),
                    *sc = frame(c, 0, 0, 1, 200, 200);
    size_t bytes = (size_t)cairo_image_surface_get_stride(sa) * 200;
    CHECK(!memcmp(cairo_image_surface_get_data(sa), cairo_image_surface_get_data(sb), bytes));
    CHECK(memcmp(cairo_image_surface_get_data(sa), cairo_image_surface_get_data(sc), bytes) != 0);
    cairo_surface_destroy(sa);
    cairo_surface_destroy(sb);
    cairo_surface_destroy(sc);

    /* drawing into a smaller picture of an area that starts at (ox, oy): the same flames at half
     * the size, shifted */
    s = frame(a, 0, 0, 1.0, 200, 200);
    cairo_surface_t *half = frame(a, 0, 0, 0.5, 100, 100);
    int full_n = lit(s, 0, 200), half_n = lit(half, 0, 100);
    CHECK(half_n > 0 && half_n < full_n); /* a quarter of the area, give or take */
    cairo_surface_destroy(half);
    cairo_surface_t *shifted = frame(a, 0, 100, 1.0, 200, 100); /* only the lower half of the area */
    CHECK(lit(shifted, 0, 100) > 0 && lit(shifted, 0, 100) < full_n);
    cairo_surface_destroy(shifted);
    cairo_surface_destroy(s);

    /* the main color is respected: a blue fire is blue in the middle of its life */
    struct fire_sim *blue = fire_sim_new(400, 14, 0x2060ff, 9);
    for (int i = 0; i < 20; i++) {
        fire_sim_step(blue, 1 / 60.0, 20, 180, 150, true);
    }
    s = frame(blue, 0, 0, 1, 200, 200);
    int bluish = 0;
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 200; x++) {
            int r, g, bl, al;
            px(s, x, y, &r, &g, &bl, &al);
            bluish += al > 40 && bl > r + 40;
        }
    }
    CHECK(bluish > 20);
    cairo_surface_destroy(s);
    fire_sim_free(blue);

    fire_sim_free(a);
    fire_sim_free(b);
    fire_sim_free(c);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_fire: all checks passed\n");
    return 0;
}
