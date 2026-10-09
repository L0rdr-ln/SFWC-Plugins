/*
 * Settings of the animations plugin: the lines of the [animations] section (Hyprland style
 * `animation =` rules, `bezier =` curves, `preset =`, the simple keys and the fire settings).
 * No wlroots dependency, so it is unit-tested on its own (tests/test_animcfg.c).
 */
#ifndef SFWC_ANIMCFG_H
#define SFWC_ANIMCFG_H

#include <stdbool.h>
#include <stdint.h>

#include "anim.h"

/*
 * Hyprland-style animation rules: `animation = <type>, <on>, <speed>, <curve>[, <style>]`.
 * Speed is in units of 100 ms, as in Hyprland.
 */
enum anim_type {
    ANIMT_WINDOWS_IN,
    ANIMT_WINDOWS_OUT,
    ANIMT_WINDOWS_MOVE,
    ANIMT_FADE_IN,
    ANIMT_FADE_OUT,
    ANIMT_BORDER,
    ANIMT_WORKSPACES,
    ANIMT_LAYERS_IN,
    ANIMT_LAYERS_OUT,
    ANIMT_COUNT,
};

enum anim_style {
    ANIM_STYLE_DEFAULT, /* the type's own default */
    ANIM_STYLE_POPIN,   /* grow/shrink around the center, `percent` = size at the start */
    ANIM_STYLE_SLIDE,   /* move in from / out to the nearest screen edge (or `dir`) */
    ANIM_STYLE_SLIDEVERT,     /* workspaces: slide up/down */
    ANIM_STYLE_SLIDEFADE,     /* slide by `percent` of the size while fading */
    ANIM_STYLE_SLIDEFADEVERT, /* workspaces: slidefade up/down */
    ANIM_STYLE_FADE,
    ANIM_STYLE_ZOOM,    /* popin that also fades */
    ANIM_STYLE_SQUEEZE, /* collapse to a line, then to nothing, like a switched off TV */
    ANIM_STYLE_FIRE,    /* burn away from the bottom, with flames */
};

enum anim_dir { ANIM_DIR_AUTO, ANIM_DIR_LEFT, ANIM_DIR_RIGHT, ANIM_DIR_TOP, ANIM_DIR_BOTTOM };

struct anim_rule {
    bool set;  /* given in the config (otherwise the legacy keys / the global rule apply) */
    bool on;
    double speed; /* x 100 ms */
    char curve[24];
    enum anim_style style;
    int percent; /* 0 = the style's default */
    enum anim_dir dir;
};

#define MAX_CURVES 16
struct anim_curve_def {
    char name[24];
    struct anim_curve curve;
};


struct animcfg {
    /* the simple keys */
    bool anim_enabled;
    char anim_open[24], anim_close[24], anim_easing[24];
    bool anim_move, anim_resize;
    int anim_duration_ms;
    /* bezier = ..., animation = ... */
    struct anim_curve_def curves[MAX_CURVES];
    int n_curves;
    struct anim_rule anim[ANIMT_COUNT];
    struct anim_rule anim_global; /* `animation = global, ...`: default for types without a rule */
    /* the `fire` style */
    int fire_particles; /* at most this many flames at a time */
    int fire_size;      /* radius of a flame in px */
    uint32_t fire_color; /* 0xRRGGBB, the main color of the flames */
};

enum { ANIMCFG_WARNING = 1, ANIMCFG_ERROR = 2 };
typedef void (*animcfg_log_fn)(int level, int line, const char *text, void *data);

void animcfg_defaults(struct animcfg *c);
/* One line `key = value` of the section; problems are reported through `log` with `line`. */
void animcfg_apply(struct animcfg *c, const char *key, const char *value, int line, animcfg_log_fn log, void *data);

/* The curve called `name`: defined with `bezier = ...`, else a built-in one. */
bool animcfg_find_curve(const struct animcfg *c, const char *name, struct anim_curve *out);

/* The rule for `type`: its own, else the global one (with the type's style), else false. */
bool animcfg_rule(const struct animcfg *c, enum anim_type type, struct anim_rule *out);

#endif
