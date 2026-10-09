#define _DEFAULT_SOURCE
#include "fire.h"

#include <math.h>
#include <stdlib.h>

struct particle {
    double x, y, vx, vy;
    double age, life; /* seconds */
    double size;
};

struct fire_sim {
    struct particle *p;
    int n, max;
    int size;
    double base[3]; /* main flame color */
    uint32_t rng;
    double carry; /* emission that did not make a whole particle yet */
    double time;
};

static double rnd(struct fire_sim *f) /* 0..1, xorshift32 */
{
    f->rng ^= f->rng << 13;
    f->rng ^= f->rng >> 17;
    f->rng ^= f->rng << 5;
    return (f->rng & 0xffffff) / (double)0x1000000;
}

struct fire_sim *fire_sim_new(int max_particles, int size, uint32_t rgb, uint32_t seed)
{
    if (max_particles < 1) {
        max_particles = 1;
    } else if (max_particles > 5000) {
        max_particles = 5000;
    }
    struct fire_sim *f = calloc(1, sizeof *f);
    if (!f) {
        return NULL;
    }
    f->p = calloc((size_t)max_particles, sizeof *f->p);
    if (!f->p) {
        free(f);
        return NULL;
    }
    f->max = max_particles;
    f->size = size < 2 ? 2 : size > 100 ? 100 : size;
    f->base[0] = ((rgb >> 16) & 0xff) / 255.0;
    f->base[1] = ((rgb >> 8) & 0xff) / 255.0;
    f->base[2] = (rgb & 0xff) / 255.0;
    f->rng = seed ? seed : 0x9e3779b9u;
    return f;
}

void fire_sim_free(struct fire_sim *f)
{
    if (f) {
        free(f->p);
        free(f);
    }
}

int fire_sim_count(const struct fire_sim *f)
{
    return f->n;
}

bool fire_sim_top(const struct fire_sim *f, double *y)
{
    if (f->n == 0) {
        return false;
    }
    double top = f->p[0].y;
    for (int i = 1; i < f->n; i++) {
        if (f->p[i].y < top) {
            top = f->p[i].y;
        }
    }
    *y = top;
    return true;
}

void fire_sim_step(struct fire_sim *f, double dt, double x0, double x1, double y, bool emit)
{
    if (dt <= 0) {
        return;
    }
    if (dt > 0.1) {
        dt = 0.1;
    }
    f->time += dt;
    /* move and age; drop the dead by swapping in the last one */
    for (int i = 0; i < f->n;) {
        struct particle *p = &f->p[i];
        p->age += dt;
        if (p->age >= p->life) {
            *p = f->p[--f->n];
            continue;
        }
        double u = p->age / p->life;
        /* hot air accelerates upward, a little sideways turbulence that grows with age */
        p->vy -= (30 + 50 * (1 - u)) * dt;
        p->vx += sin(f->time * 9 + p->y * 0.05 + i) * 140 * u * dt;
        p->vx *= 1 - 1.5 * dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        i++;
    }
    if (!emit || x1 < x0) {
        return;
    }
    /* a few new flames per second for every pixel of line, scaled to the cap: thin enough to
     * see the window through them */
    double rate = (x1 - x0 + 1) * 1.8 * (f->max / 400.0 > 4 ? 4 : f->max / 400.0 < 0.25 ? 0.25 : f->max / 400.0);
    f->carry += rate * dt;
    while (f->carry >= 1 && f->n < f->max) {
        f->carry -= 1;
        struct particle *p = &f->p[f->n++];
        p->x = x0 + rnd(f) * (x1 - x0);
        p->y = y + (rnd(f) - 0.5) * f->size * 0.6;
        p->vx = (rnd(f) - 0.5) * 50;
        p->vy = -(30 + rnd(f) * 50);
        p->life = 0.3 + rnd(f) * 0.5;
        p->age = 0;
        p->size = f->size * (0.55 + rnd(f) * 0.6);
    }
    if (f->carry > 50) {
        f->carry = 50; /* the cap was reached: do not store up a burst */
    }
}

static double lerp(double a, double b, double t)
{
    return a + (b - a) * t;
}

/* white-yellow core -> the main color -> deep red -> grey smoke */
static void flame_color(const struct fire_sim *f, double u, double rgb[3])
{
    static const double core[3] = {1.0, 0.95, 0.65}, deep[3] = {0.55, 0.07, 0.02},
                        smoke[3] = {0.18, 0.16, 0.15};
    const double *a, *b;
    double t;
    double mid[3] = {f->base[0], f->base[1], f->base[2]};
    if (u < 0.3) {
        a = core;
        b = mid;
        t = u / 0.3;
    } else if (u < 0.7) {
        a = mid;
        b = deep;
        t = (u - 0.3) / 0.4;
    } else {
        a = deep;
        b = smoke;
        t = (u - 0.7) / 0.3;
    }
    for (int i = 0; i < 3; i++) {
        rgb[i] = lerp(a[i], b[i], t);
    }
}

void fire_sim_draw(const struct fire_sim *f, cairo_surface_t *surface, double ox, double oy,
                   double scale)
{
    cairo_t *cr = cairo_create(surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
    for (int i = 0; i < f->n; i++) {
        const struct particle *p = &f->p[i];
        double u = p->age / p->life;
        double rgb[3];
        flame_color(f, u, rgb);
        double alpha = pow(1 - u, 0.8) * 0.7;
        double r = p->size * (1 - 0.45 * u) * scale;
        double x = (p->x - ox) * scale, y = (p->y - oy) * scale;
        cairo_pattern_t *g = cairo_pattern_create_radial(x, y, 0, x, y, r);
        cairo_pattern_add_color_stop_rgba(g, 0, rgb[0], rgb[1], rgb[2], alpha);
        cairo_pattern_add_color_stop_rgba(g, 0.5, rgb[0], rgb[1], rgb[2], alpha * 0.55);
        cairo_pattern_add_color_stop_rgba(g, 1, rgb[0], rgb[1], rgb[2], 0);
        cairo_set_source(cr, g);
        cairo_arc(cr, x, y, r, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }
    cairo_destroy(cr);
    cairo_surface_flush(surface);
}
