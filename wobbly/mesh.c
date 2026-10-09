#define _DEFAULT_SOURCE
#include "mesh.h"

#include <math.h>
#include <stdlib.h>

struct wobbly {
    int g;
    double k, c;
    double rx, ry, rw, rh;
    bool have_rect;
    bool has_grab;
    double gx, gy;
    double *px, *py, *vx, *vy; /* g*g nodes: position and velocity */
};

struct wobbly *wobbly_new(int grid, double spring, double friction)
{
    struct wobbly *w = calloc(1, sizeof *w);
    if (!w) {
        return NULL;
    }
    w->g = grid < 2 ? 2 : grid > 16 ? 16 : grid;
    w->k = spring < 1 ? 1 : spring > 1000 ? 1000 : spring;
    w->c = friction < 0.5 ? 0.5 : friction > 100 ? 100 : friction;
    size_t n = (size_t)w->g * w->g;
    w->px = calloc(n * 4, sizeof(double));
    if (!w->px) {
        free(w);
        return NULL;
    }
    w->py = w->px + n;
    w->vx = w->py + n;
    w->vy = w->vx + n;
    return w;
}

void wobbly_free(struct wobbly *w)
{
    if (w) {
        free(w->px);
        free(w);
    }
}

static void rest_of(const struct wobbly *w, int a, int b, double *x, double *y)
{
    *x = w->rx + w->rw * a / (w->g - 1);
    *y = w->ry + w->rh * b / (w->g - 1);
}

void wobbly_set_rect(struct wobbly *w, double x, double y, double width, double height)
{
    bool reset = !w->have_rect || fabs(width - w->rw) > 0.5 || fabs(height - w->rh) > 0.5;
    w->rx = x;
    w->ry = y;
    w->rw = width;
    w->rh = height;
    w->have_rect = true;
    if (reset) {
        for (int b = 0; b < w->g; b++) {
            for (int a = 0; a < w->g; a++) {
                int i = b * w->g + a;
                rest_of(w, a, b, &w->px[i], &w->py[i]);
                w->vx[i] = w->vy[i] = 0;
            }
        }
    }
}

void wobbly_set_grab(struct wobbly *w, bool has, double gx, double gy)
{
    w->has_grab = has;
    w->gx = gx;
    w->gy = gy;
}

/* how much stiffer than the rest a node near the grabbed point is */
static double stiffness(const struct wobbly *w, double rx, double ry)
{
    if (!w->has_grab) {
        return 1;
    }
    double sigma = 0.3 * (w->rw > w->rh ? w->rw : w->rh);
    if (sigma < 1) {
        sigma = 1;
    }
    double dx = rx - w->gx, dy = ry - w->gy;
    return 1 + 12 * exp(-(dx * dx + dy * dy) / (2 * sigma * sigma));
}

void wobbly_step(struct wobbly *w, double dt)
{
    if (!w->have_rect || dt <= 0) {
        return;
    }
    if (dt > 0.1) {
        dt = 0.1;
    }
    const double h = 1.0 / 240;
    int steps = (int)ceil(dt / h);
    double hs = dt / steps;
    int g = w->g;
    double kn = 0.6 * w->k; /* coupling between neighbors keeps the shape together */
    for (int s = 0; s < steps; s++) {
        for (int b = 0; b < g; b++) {
            for (int a = 0; a < g; a++) {
                int i = b * g + a;
                double rx, ry;
                rest_of(w, a, b, &rx, &ry);
                double m = stiffness(w, rx, ry);
                double fx = -w->k * m * (w->px[i] - rx) - w->c * w->vx[i];
                double fy = -w->k * m * (w->py[i] - ry) - w->c * w->vy[i];
                static const int nb[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (int n = 0; n < 4; n++) {
                    int na = a + nb[n][0], nbb = b + nb[n][1];
                    if (na < 0 || na >= g || nbb < 0 || nbb >= g) {
                        continue;
                    }
                    int j = nbb * g + na;
                    double nrx, nry;
                    rest_of(w, na, nbb, &nrx, &nry);
                    fx += kn * ((w->px[j] - w->px[i]) - (nrx - rx));
                    fy += kn * ((w->py[j] - w->py[i]) - (nry - ry));
                }
                w->vx[i] += fx * hs; /* semi-implicit Euler, mass 1 */
                w->vy[i] += fy * hs;
            }
        }
        for (int i = 0; i < g * g; i++) {
            w->px[i] += w->vx[i] * hs;
            w->py[i] += w->vy[i] * hs;
        }
    }
}

double wobbly_node_offset(const struct wobbly *w, int a, int b)
{
    double rx, ry;
    rest_of(w, a, b, &rx, &ry);
    int i = b * w->g + a;
    return hypot(w->px[i] - rx, w->py[i] - ry);
}

double wobbly_max_offset(const struct wobbly *w)
{
    double m = 0;
    for (int b = 0; b < w->g; b++) {
        for (int a = 0; a < w->g; a++) {
            double d = wobbly_node_offset(w, a, b);
            m = d > m ? d : m;
        }
    }
    return m;
}

bool wobbly_settled(const struct wobbly *w)
{
    if (!w->have_rect) {
        return true;
    }
    for (int i = 0; i < w->g * w->g; i++) {
        if (hypot(w->vx[i], w->vy[i]) > 2.0) {
            return false;
        }
    }
    return wobbly_max_offset(w) < 0.25;
}

void wobbly_point(const struct wobbly *w, double u, double v, double *x, double *y)
{
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    double fu = u * (w->g - 1), fv = v * (w->g - 1);
    int a = (int)fu, b = (int)fv;
    if (a >= w->g - 1) {
        a = w->g - 2;
    }
    if (b >= w->g - 1) {
        b = w->g - 2;
    }
    double tu = fu - a, tv = fv - b;
    int i00 = b * w->g + a, i10 = i00 + 1, i01 = i00 + w->g, i11 = i01 + 1;
    *x = (1 - tv) * ((1 - tu) * w->px[i00] + tu * w->px[i10]) +
         tv * ((1 - tu) * w->px[i01] + tu * w->px[i11]);
    *y = (1 - tv) * ((1 - tu) * w->py[i00] + tu * w->py[i10]) +
         tv * ((1 - tu) * w->py[i01] + tu * w->py[i11]);
}
