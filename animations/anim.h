/*
 * Animation maths: easing curves and time-based progress. No wlroots dependency
 * (unit-tested in tests/test_anim.c). The compositor drives the animations from its
 * output frame callbacks; progress is computed from timestamps, so skipped frames only
 * make the animation less smooth, never longer.
 */
#ifndef SFWC_ANIM_H
#define SFWC_ANIM_H

#include <stdbool.h>
#include <stdint.h>

enum anim_easing { EASE_LINEAR, EASE_IN, EASE_OUT, EASE_IN_OUT };

/* "linear", "ease-in", "ease-out", "ease-in-out" */
bool anim_easing_from_name(const char *name, enum anim_easing *out);

/* Maps t in [0,1] (clamped) through the curve; 0 -> 0 and 1 -> 1 for every curve. */
double anim_ease(enum anim_easing e, double t);

/* Fraction of the animation that has elapsed, in [0,1]; a zero duration is finished. */
double anim_progress(uint32_t start_ms, uint32_t now_ms, uint32_t duration_ms);

double anim_lerp(double from, double to, double t);

/*
 * Cubic Bézier timing curves as in CSS and Hyprland: the curve runs from (0,0) to (1,1) with the
 * two control points (x0,y0) and (x1,y1). x must be in [0,1]; y may leave [0,1] to overshoot
 * (bezier = bounce, 0.05, 0.9, 0.1, 1.05).
 */
struct anim_curve {
    double x0, y0, x1, y1;
};

/* Maps t in [0,1] (clamped) through the curve; 0 -> 0 and 1 -> 1 for every curve. */
double anim_curve_eval(const struct anim_curve *c, double t);

/* Built-in names: linear, default, ease, ease-in, ease-out, ease-in-out. */
bool anim_builtin_curve(const char *name, struct anim_curve *out);

/* x control points inside [0,1] (otherwise the curve could run backwards in time) */
bool anim_curve_valid(const struct anim_curve *c);

#endif
