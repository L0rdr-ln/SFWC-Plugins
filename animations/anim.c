#include "anim.h"

#include <math.h>
#include <string.h>

bool anim_easing_from_name(const char *name, enum anim_easing *out)
{
    static const struct {
        const char *name;
        enum anim_easing easing;
    } names[] = {
        {"linear", EASE_LINEAR},
        {"ease-in", EASE_IN},
        {"ease-out", EASE_OUT},
        {"ease-in-out", EASE_IN_OUT},
    };
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
        if (!strcmp(name, names[i].name)) {
            *out = names[i].easing;
            return true;
        }
    }
    return false;
}

double anim_ease(enum anim_easing e, double t)
{
    if (t <= 0) {
        return 0;
    }
    if (t >= 1) {
        return 1;
    }
    switch (e) {
    case EASE_IN:
        return t * t * t;
    case EASE_OUT: {
        double u = 1 - t;
        return 1 - u * u * u;
    }
    case EASE_IN_OUT:
        if (t < 0.5) {
            return 4 * t * t * t;
        } else {
            double u = -2 * t + 2;
            return 1 - u * u * u / 2;
        }
    case EASE_LINEAR:
    default:
        return t;
    }
}

double anim_progress(uint32_t start_ms, uint32_t now_ms, uint32_t duration_ms)
{
    if (duration_ms == 0) {
        return 1;
    }
    uint32_t elapsed = now_ms - start_ms; /* wraps correctly */
    if (elapsed >= duration_ms) {
        return 1;
    }
    return (double)elapsed / duration_ms;
}

double anim_lerp(double from, double to, double t)
{
    return from + (to - from) * t;
}

/* ---------------------------------------------------------------- Bézier curves */

static double bez(double a, double b, double s)
{
    double u = 1 - s;
    return 3 * u * u * s * a + 3 * u * s * s * b + s * s * s;
}

static double bez_slope(double a, double b, double s)
{
    double u = 1 - s;
    return 3 * u * u * a + 6 * u * s * (b - a) + 3 * s * s * (1 - b);
}

double anim_curve_eval(const struct anim_curve *c, double t)
{
    if (t <= 0) {
        return 0;
    }
    if (t >= 1) {
        return 1;
    }
    /* find s with x(s) = t: Newton's method, bisection when the slope is too flat */
    double s = t;
    for (int i = 0; i < 8; i++) {
        double err = bez(c->x0, c->x1, s) - t;
        if (fabs(err) < 1e-7) {
            return bez(c->y0, c->y1, s);
        }
        double d = bez_slope(c->x0, c->x1, s);
        if (fabs(d) < 1e-6) {
            break;
        }
        s -= err / d;
        if (s < 0 || s > 1) {
            break;
        }
    }
    double lo = 0, hi = 1;
    s = t;
    for (int i = 0; i < 48; i++) {
        double x = bez(c->x0, c->x1, s);
        if (fabs(x - t) < 1e-7) {
            break;
        }
        if (x < t) {
            lo = s;
        } else {
            hi = s;
        }
        s = (lo + hi) / 2;
    }
    return bez(c->y0, c->y1, s);
}

bool anim_builtin_curve(const char *name, struct anim_curve *out)
{
    static const struct {
        const char *name;
        struct anim_curve c;
    } curves[] = {
        {"linear", {0, 0, 1, 1}},
        {"default", {0.0, 0.75, 0.15, 1.0}},
        {"ease", {0.25, 0.1, 0.25, 1.0}},
        {"ease-in", {0.42, 0, 1, 1}},
        {"ease-out", {0, 0, 0.58, 1}},
        {"ease-in-out", {0.42, 0, 0.58, 1}},
    };
    for (unsigned i = 0; i < sizeof curves / sizeof *curves; i++) {
        if (!strcmp(name, curves[i].name)) {
            *out = curves[i].c;
            return true;
        }
    }
    return false;
}

bool anim_curve_valid(const struct anim_curve *c)
{
    return c->x0 >= 0 && c->x0 <= 1 && c->x1 >= 0 && c->x1 <= 1 && isfinite(c->y0) &&
           isfinite(c->y1) && fabs(c->y0) <= 10 && fabs(c->y1) <= 10;
}
