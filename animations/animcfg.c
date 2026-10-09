#define _POSIX_C_SOURCE 200809L
#include "animcfg.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct loader {
    struct animcfg *c;
    animcfg_log_fn log;
    void *log_data;
    int line;
};

static void report(struct loader *l, int level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void report(struct loader *l, int level, const char *fmt, ...)
{
    if (!l->log) {
        return;
    }
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    l->log(level, l->line, msg, l->log_data);
}

static const char *const open_values[] = {"none", "fade", "fade-scale", "slide", NULL};
static const char *const easing_values[] = {"linear", "ease-in", "ease-out", "ease-in-out", NULL};

void animcfg_defaults(struct animcfg *c)
{
    memset(c, 0, sizeof *c);
    c->anim_enabled = true;
    strcpy(c->anim_open, "fade-scale");
    strcpy(c->anim_close, "fade");
    strcpy(c->anim_easing, "ease-out");
    c->anim_move = c->anim_resize = true;
    c->anim_duration_ms = 180;
    c->fire_particles = 400;
    c->fire_size = 14;
    c->fire_color = 0xff7a18;
}

static bool parse_bool(const char *v, bool *out)
{
    if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1")) {
        *out = true;
        return true;
    }
    if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcmp(v, "0")) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_int(const char *v, int min, int max, int *out)
{
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < min || n > max) {
        return false;
    }
    *out = (int)n;
    return true;
}

static bool in_list(const char *v, const char *const *list)
{
    for (; *list; list++) {
        if (!strcmp(v, *list)) {
            return true;
        }
    }
    return false;
}

static void set_bool(struct loader *l, const char *key, const char *v, bool *out)
{
    if (!parse_bool(v, out)) {
        report(l, ANIMCFG_ERROR, "%s: '%s' is not a boolean (use true/false)", key, v);
    }
}

static void set_int(struct loader *l, const char *key, const char *v, int min, int max, int *out)
{
    if (!parse_int(v, min, max, out)) {
        report(l, ANIMCFG_ERROR, "%s: '%s' is not a number between %d and %d", key, v, min, max);
    }
}

static void set_choice(struct loader *l, const char *key, const char *v, const char *const *list,
                       char *out, size_t out_size)
{
    if (!in_list(v, list)) {
        char opts[128] = "";
        for (const char *const *o = list; *o; o++) {
            strncat(opts, *o, sizeof opts - strlen(opts) - 1);
            if (o[1]) {
                strncat(opts, " | ", sizeof opts - strlen(opts) - 1);
            }
        }
        report(l, ANIMCFG_ERROR, "%s: '%s' is not valid (%s)", key, v, opts);
        return;
    }
    snprintf(out, out_size, "%s", v);
}

/* ---------------------------------------------------------- animation rules */

static bool parse_double(const char *v, double min, double max, double *out)
{
    char *end;
    errno = 0;
    double d = strtod(v, &end);
    if (end == v || *end || errno || !isfinite(d) || d < min || d > max) {
        return false;
    }
    *out = d;
    return true;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) {
        s++;
    }
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) {
        *--e = 0;
    }
    return s;
}

bool animcfg_find_curve(const struct animcfg *c, const char *name, struct anim_curve *out)
{
    for (int i = c->n_curves - 1; i >= 0; i--) {
        if (!strcmp(c->curves[i].name, name)) {
            *out = c->curves[i].curve;
            return true;
        }
    }
    return anim_builtin_curve(name, out);
}

bool animcfg_rule(const struct animcfg *c, enum anim_type type, struct anim_rule *out)
{
    if (type < 0 || type >= ANIMT_COUNT) {
        return false;
    }
    if (c->anim[type].set) {
        *out = c->anim[type];
        return true;
    }
    if (c->anim_global.set) {
        *out = c->anim_global;
        out->style = ANIM_STYLE_DEFAULT; /* the global rule has no style */
        return true;
    }
    return false;
}

static const struct {
    const char *name;
    enum anim_type types[4]; /* ANIMT_COUNT ends the list */
} anim_names[] = {
    {"windows", {ANIMT_WINDOWS_IN, ANIMT_WINDOWS_OUT, ANIMT_WINDOWS_MOVE, ANIMT_COUNT}},
    {"windowsIn", {ANIMT_WINDOWS_IN, ANIMT_COUNT}},
    {"windowsOut", {ANIMT_WINDOWS_OUT, ANIMT_COUNT}},
    {"windowsMove", {ANIMT_WINDOWS_MOVE, ANIMT_COUNT}},
    {"fade", {ANIMT_FADE_IN, ANIMT_FADE_OUT, ANIMT_COUNT}},
    {"fadeIn", {ANIMT_FADE_IN, ANIMT_COUNT}},
    {"fadeOut", {ANIMT_FADE_OUT, ANIMT_COUNT}},
    {"border", {ANIMT_BORDER, ANIMT_COUNT}},
    {"workspaces", {ANIMT_WORKSPACES, ANIMT_COUNT}},
    {"layers", {ANIMT_LAYERS_IN, ANIMT_LAYERS_OUT, ANIMT_COUNT}},
    {"layersIn", {ANIMT_LAYERS_IN, ANIMT_COUNT}},
    {"layersOut", {ANIMT_LAYERS_OUT, ANIMT_COUNT}},
};

/* Hyprland animation names that exist there but not here; accepted so that a pasted
 * Hyprland animation block loads, but they do nothing. */
static const char *const anim_ignored[] = {"borderangle", "fadeSwitch", "fadeShadow", "fadeDim",
                                           "fadeLayers", "specialWorkspace", "specialWorkspaceIn",
                                           "specialWorkspaceOut", "workspacesIn", "workspacesOut",
                                           "monitorAdded"};

static bool style_allowed(enum anim_type t, enum anim_style s)
{
    switch (t) {
    case ANIMT_WINDOWS_IN:
    case ANIMT_WINDOWS_OUT:
        return s == ANIM_STYLE_POPIN || s == ANIM_STYLE_SLIDE || s == ANIM_STYLE_SLIDEFADE ||
               s == ANIM_STYLE_ZOOM || s == ANIM_STYLE_SQUEEZE || s == ANIM_STYLE_FIRE;
    case ANIMT_WORKSPACES:
        return s == ANIM_STYLE_SLIDE || s == ANIM_STYLE_SLIDEVERT || s == ANIM_STYLE_SLIDEFADE ||
               s == ANIM_STYLE_SLIDEFADEVERT || s == ANIM_STYLE_FADE;
    case ANIMT_LAYERS_IN:
    case ANIMT_LAYERS_OUT:
        return s == ANIM_STYLE_POPIN || s == ANIM_STYLE_SLIDE || s == ANIM_STYLE_FADE;
    default:
        return false;
    }
}

/* "popin 80%", "slide left", "slidefade 20%", "slidevert", "fade" */
static bool parse_style(const char *text, enum anim_style *style, int *percent, enum anim_dir *dir)
{
    static const struct {
        const char *name;
        enum anim_style style;
    } styles[] = {{"popin", ANIM_STYLE_POPIN},
                  {"slide", ANIM_STYLE_SLIDE},
                  {"slidevert", ANIM_STYLE_SLIDEVERT},
                  {"slidefade", ANIM_STYLE_SLIDEFADE},
                  {"slidefadevert", ANIM_STYLE_SLIDEFADEVERT},
                  {"fade", ANIM_STYLE_FADE},
                  {"zoom", ANIM_STYLE_ZOOM},
                  {"squeeze", ANIM_STYLE_SQUEEZE},
                  {"fire", ANIM_STYLE_FIRE}};
    char buf[64];
    snprintf(buf, sizeof buf, "%s", text);
    char *save = NULL, *word = strtok_r(buf, " \t", &save), *arg = strtok_r(NULL, " \t", &save);
    if (!word || strtok_r(NULL, " \t", &save)) {
        return false;
    }
    bool found = false;
    for (unsigned i = 0; i < sizeof styles / sizeof *styles; i++) {
        if (!strcmp(word, styles[i].name)) {
            *style = styles[i].style;
            found = true;
        }
    }
    if (!found) {
        return false;
    }
    *percent = 0;
    *dir = ANIM_DIR_AUTO;
    if (!arg) {
        return true;
    }
    size_t n = strlen(arg);
    if (n > 1 && arg[n - 1] == '%') {
        char num[16];
        snprintf(num, sizeof num, "%.*s", (int)(n - 1), arg);
        double pct;
        if (!parse_double(num, 1, 300, &pct)) {
            return false;
        }
        *percent = (int)(pct + 0.5);
        /* squeeze and fire take no argument; whether a slide takes a percent or a direction
         * depends on the type and is checked by the caller */
        return *style != ANIM_STYLE_SQUEEZE && *style != ANIM_STYLE_FIRE && *style != ANIM_STYLE_FADE;
    }
    if (*style != ANIM_STYLE_SLIDE) {
        return false;
    }
    if (!strcmp(arg, "left")) {
        *dir = ANIM_DIR_LEFT;
    } else if (!strcmp(arg, "right")) {
        *dir = ANIM_DIR_RIGHT;
    } else if (!strcmp(arg, "top")) {
        *dir = ANIM_DIR_TOP;
    } else if (!strcmp(arg, "bottom")) {
        *dir = ANIM_DIR_BOTTOM;
    } else {
        return false;
    }
    return true;
}

/* bezier = name, x0, y0, x1, y1 */
static void handle_bezier(struct loader *l, const char *value)
{
    struct animcfg *c = l->c;
    char buf[160];
    snprintf(buf, sizeof buf, "%s", value);
    char *fields[5];
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok && n < 6; tok = strtok_r(NULL, ",", &save)) {
        if (n < 5) {
            fields[n] = trim(tok);
        }
        n++;
    }
    if (n != 5) {
        report(l, ANIMCFG_ERROR, "bezier: use 'name, x0, y0, x1, y1' (got %d value%s)", n, n == 1 ? "" : "s");
        return;
    }
    const char *name = fields[0];
    size_t nl = strlen(name);
    bool ok = nl > 0 && nl < sizeof c->curves[0].name;
    for (const char *p = name; ok && *p; p++) {
        ok = isalnum((unsigned char)*p) || *p == '_' || *p == '-';
    }
    if (!ok) {
        report(l, ANIMCFG_ERROR, "bezier: '%s' is not a curve name (letters, digits, - and _, up to 23)", name);
        return;
    }
    struct anim_curve cv;
    double *p[4] = {&cv.x0, &cv.y0, &cv.x1, &cv.y1};
    for (int i = 0; i < 4; i++) {
        if (!parse_double(fields[i + 1], -10, 10, p[i])) {
            report(l, ANIMCFG_ERROR, "bezier %s: '%s' is not a number", name, fields[i + 1]);
            return;
        }
    }
    if (!anim_curve_valid(&cv)) {
        report(l, ANIMCFG_ERROR, "bezier %s: x0 and x1 must be between 0 and 1", name);
        return;
    }
    for (int i = 0; i < c->n_curves; i++) {
        if (!strcmp(c->curves[i].name, name)) { /* last definition wins */
            c->curves[i].curve = cv;
            return;
        }
    }
    if (c->n_curves >= MAX_CURVES) {
        report(l, ANIMCFG_ERROR, "bezier: at most %d curves can be defined", MAX_CURVES);
        return;
    }
    snprintf(c->curves[c->n_curves].name, sizeof c->curves[0].name, "%s", name);
    c->curves[c->n_curves++].curve = cv;
}

/* animation = type, on, speed, curve[, style] */
static void handle_animation_rule(struct loader *l, const char *value)
{
    struct animcfg *c = l->c;
    char buf[200];
    snprintf(buf, sizeof buf, "%s", value);
    char *fields[5];
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok && n < 6; tok = strtok_r(NULL, ",", &save)) {
        if (n < 5) {
            fields[n] = trim(tok);
        }
        n++;
    }
    if (n < 2 || n > 5) {
        report(l, ANIMCFG_ERROR, "animation: use 'type, on, speed, curve[, style]'");
        return;
    }
    const char *name = fields[0];
    bool global = !strcmp(name, "global");
    const enum anim_type *types = NULL;
    for (unsigned i = 0; i < sizeof anim_names / sizeof *anim_names; i++) {
        if (!strcmp(name, anim_names[i].name)) {
            types = anim_names[i].types;
        }
    }
    if (!global && !types) {
        for (unsigned i = 0; i < sizeof anim_ignored / sizeof *anim_ignored; i++) {
            if (!strcmp(name, anim_ignored[i])) {
                report(l, ANIMCFG_WARNING, "animation %s is not supported and is ignored", name);
                return;
            }
        }
        report(l, ANIMCFG_ERROR, "animation: unknown type '%s'", name);
        return;
    }
    struct anim_rule r = {.set = true, .on = true, .speed = 5, .style = ANIM_STYLE_DEFAULT};
    snprintf(r.curve, sizeof r.curve, "%s", "default");
    if (!parse_bool(fields[1], &r.on)) {
        report(l, ANIMCFG_ERROR, "animation %s: '%s' is not 0/1", name, fields[1]);
        return;
    }
    if (r.on) {
        if (n < 4) {
            report(l, ANIMCFG_ERROR, "animation %s: needs speed and curve (or 0 to turn it off)", name);
            return;
        }
        if (!parse_double(fields[2], 0.01, 100, &r.speed)) {
            report(l, ANIMCFG_ERROR, "animation %s: speed '%s' is not a number between 0.01 and 100", name, fields[2]);
            return;
        }
        struct anim_curve probe;
        if (strlen(fields[3]) >= sizeof r.curve || !animcfg_find_curve(c, fields[3], &probe)) {
            report(l, ANIMCFG_ERROR, "animation %s: unknown curve '%s' (define it with bezier = ... first)", name, fields[3]);
            return;
        }
        snprintf(r.curve, sizeof r.curve, "%s", fields[3]);
    }
    if (n == 5 && r.on) {
        enum anim_style style;
        if (global) {
            report(l, ANIMCFG_ERROR, "animation global: takes no style");
            return;
        }
        if (!parse_style(fields[4], &style, &r.percent, &r.dir)) {
            report(l, ANIMCFG_ERROR, "animation %s: bad style '%s' (popin 80%%, slide [left|right|top|bottom], slidefade 20%%, slidevert, fade)", name, fields[4]);
            return;
        }
        bool applies = false; /* at least one of the types must be able to use the style */
        for (const enum anim_type *t = types; *t != ANIMT_COUNT; t++) {
            applies = applies || style_allowed(*t, style);
        }
        if (!applies) {
            report(l, ANIMCFG_ERROR, "animation %s: style '%s' does not apply here", name, fields[4]);
            return;
        }
        r.style = style;
        /* windows slide towards an edge (a direction), workspaces slide a part of the screen (a percent) */
        bool windows = types[0] == ANIMT_WINDOWS_IN || types[0] == ANIMT_WINDOWS_OUT;
        if (style == ANIM_STYLE_SLIDE && ((windows && r.percent) || (!windows && r.dir != ANIM_DIR_AUTO))) {
            report(l, ANIMCFG_ERROR, "animation %s: bad style '%s' (%s)", name, fields[4],
                   windows ? "slide takes left, right, top or bottom" : "slide takes a percentage");
            return;
        }
    }
    if (global) {
        c->anim_global = r;
        return;
    }
    for (const enum anim_type *t = types; *t != ANIMT_COUNT; t++) {
        c->anim[*t] = r;
        if (r.style != ANIM_STYLE_DEFAULT && !style_allowed(*t, r.style)) {
            c->anim[*t].style = ANIM_STYLE_DEFAULT; /* `windows` carries a style only windowsIn/Out take */
            c->anim[*t].percent = 0;
        }
    }
}

/* preset = hyprland | minimal | none: a ready-made set of rules, later lines can change them */
static void apply_preset(struct loader *l, const char *name)
{
    static const char *const hyprland[] = {
        "bezier = easeOutQuint, 0.23, 1, 0.32, 1",
        "bezier = easeInOutCubic, 0.65, 0.05, 0.36, 1",
        "bezier = almostLinear, 0.5, 0.5, 0.75, 1",
        "bezier = quick, 0.15, 0, 0.1, 1",
        "windows, 1, 4.79, easeOutQuint",
        "windowsIn, 1, 4.1, easeOutQuint, popin 87%",
        "windowsOut, 1, 1.49, linear, popin 87%",
        "fadeIn, 1, 1.73, almostLinear",
        "fadeOut, 1, 1.46, almostLinear",
        "border, 1, 5.39, easeOutQuint",
        "layers, 1, 3.81, easeOutQuint",
        "workspaces, 1, 1.94, easeOutQuint, slide",
        NULL};
    static const char *const minimal[] = {
        "bezier = quick, 0.15, 0, 0.1, 1",
        "windowsIn, 1, 2, quick, popin 95%",
        "windowsOut, 1, 2, quick, popin 95%",
        "windowsMove, 1, 2, quick",
        "fade, 1, 2, quick",
        "workspaces, 1, 2, quick, fade",
        NULL};
    const char *const *lines = NULL;
    if (!strcmp(name, "hyprland")) {
        lines = hyprland;
    } else if (!strcmp(name, "minimal")) {
        lines = minimal;
    } else if (!strcmp(name, "none")) {
        for (int i = 0; i < ANIMT_COUNT; i++) {
            l->c->anim[i] = (struct anim_rule){.set = true, .on = false};
        }
        return;
    } else {
        report(l, ANIMCFG_ERROR, "preset: unknown preset '%s' (hyprland, minimal, none)", name);
        return;
    }
    for (; *lines; lines++) {
        const char *line = *lines;
        if (!strncmp(line, "bezier = ", 9)) {
            handle_bezier(l, line + 9);
        } else {
            handle_animation_rule(l, line);
        }
    }
}

static void handle_animations(struct loader *l, const char *key, const char *v)
{
    struct animcfg *c = l->c;
    if (!strcmp(key, "enabled")) {
        set_bool(l, key, v, &c->anim_enabled);
    } else if (!strcmp(key, "open")) {
        set_choice(l, key, v, open_values, c->anim_open, sizeof c->anim_open);
    } else if (!strcmp(key, "close")) {
        set_choice(l, key, v, open_values, c->anim_close, sizeof c->anim_close);
    } else if (!strcmp(key, "move")) {
        set_bool(l, key, v, &c->anim_move);
    } else if (!strcmp(key, "resize")) {
        set_bool(l, key, v, &c->anim_resize);
    } else if (!strcmp(key, "duration_ms")) {
        set_int(l, key, v, 0, 5000, &c->anim_duration_ms);
    } else if (!strcmp(key, "easing")) {
        set_choice(l, key, v, easing_values, c->anim_easing, sizeof c->anim_easing);
    } else if (!strcmp(key, "bezier")) {
        handle_bezier(l, v);
    } else if (!strcmp(key, "animation")) {
        handle_animation_rule(l, v);
    } else if (!strcmp(key, "preset")) {
        apply_preset(l, v);
    } else if (!strcmp(key, "fire_particles")) {
        set_int(l, key, v, 20, 2000, &c->fire_particles);
    } else if (!strcmp(key, "fire_size")) {
        set_int(l, key, v, 4, 60, &c->fire_size);
    } else if (!strcmp(key, "fire_color")) {
        const char *h = v + (*v == '#');
        char *end;
        unsigned long rgb = strtoul(h, &end, 16);
        if (strlen(h) != 6 || *end) {
            report(l, ANIMCFG_ERROR, "fire_color: '%s' is not a color (use #rrggbb)", v);
        } else {
            c->fire_color = (uint32_t)rgb;
        }
    } else {
        report(l, ANIMCFG_WARNING, "unknown key '%s' in [animations]", key);
    }
}


void animcfg_apply(struct animcfg *c, const char *key, const char *value, int line, animcfg_log_fn log, void *data)
{
    struct loader l = {.c = c, .log = log, .log_data = data, .line = line};
    handle_animations(&l, key, value);
}
