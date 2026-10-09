/*
 * The animations plugin: Hyprland style open/close (popin, slide, slidefade), window moves, a fade
 * on its own timeline, the border color following the focus, workspace switches and layer
 * surfaces appearing, plus the Wayfire style effects fire, squeeze and zoom.
 * Driven by the frame callback, so skipped frames only make an animation less smooth, never
 * longer.
 *
 * How it works: every animation has two tracks, a geometry track (position and scale) and an
 * opacity track, each with its own duration and Bézier curve, like `windowsIn` and `fadeIn` in
 * Hyprland. wlroots' scene graph cannot scale a tree, so popin scales every buffer of the
 * animated tree around the tree's center (dest size and position), re-applied before every frame
 * because the client may reset it with a commit.
 *
 * `fire` crops the window's buffers along a burn line that climbs from the bottom while a
 * particle simulation (fire.c) draws flames along the line into an overlay; `squeeze` scales the
 * two axes separately, like a switched off TV.
 *
 * Without `animation = ...` rules the simple keys (open, close, move, duration_ms, easing) apply.
 * Settings: the [animations] section, see animcfg.c.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cairo.h>
#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>

#include <sfwc-plugin.h>

#include "anim.h"
#include "animcfg.h"
#include "fire.h"


/* ---------------------------------------------------------------- state */

enum anim_kind { ANIM_OPEN, ANIM_CLOSE, ANIM_MOVE, ANIM_WORKSPACE, ANIM_LAYER };

/* One timeline: progress over `duration_ms` mapped through a Bézier curve. */
struct anim_track {
    bool on;
    uint32_t start_ms, duration_ms;
    struct anim_curve curve;
};

/* The original place and size of one scene buffer of an animated tree, so that scaling can be
 * undone exactly. Dropped when the buffer goes away. */
struct scale_rec {
    struct wl_list link;
    struct wlr_scene_buffer *buffer;
    struct wl_listener destroy;
    int node_x, node_y; /* position inside its parent */
    int sx, sy;         /* position inside the animated tree */
    int w, h;           /* size on screen */
    int dst_w, dst_h;   /* what the buffer's dest size was (0 = not set) */
    struct wlr_fbox src; /* its source box, if it had one */
    bool had_src;
    bool was_enabled;
};

struct animation {
    struct wl_list link;
    enum anim_kind kind;
    struct sfwc_toplevel *toplevel; /* OPEN, MOVE, WORKSPACE; NULL for CLOSE and LAYER */
    bool busy;                      /* counted with toplevel_busy */
    struct wlr_scene_tree *tree;
    struct anim_track geo;  /* position and scale */
    struct anim_track fade; /* opacity */
    double from_x, from_y, to_x, to_y; /* position of `tree` */
    double from_scale, to_scale;       /* popin: around the center of the tree */
    double from_opacity, to_opacity;
    bool destroy_tree; /* CLOSE: the tree is a snapshot that goes away at the end */
    bool hide_at_end;  /* WORKSPACE: show or hide the window as its workspace says, at rest_x/y */
    double rest_x, rest_y;
    struct wl_list scale_recs; /* struct scale_rec */
    bool centered;
    double cx, cy;
    bool scaled;       /* popin / zoom: scale from_scale -> to_scale */
    /* Wayfire style effects (fx is ANIM_STYLE_SQUEEZE or ANIM_STYLE_FIRE, else ANIM_STYLE_DEFAULT) */
    enum anim_style fx;
    bool fx_closing;
    double bx0, by0, bx1, by1; /* the area of the animated tree, in layout coordinates */
    struct fire_sim *fire;
    struct wlr_scene_buffer *fire_buf;
    int fire_ox, fire_oy, fire_w, fire_h; /* the area the flames are drawn into */
    uint32_t last_ms;
};

/* What the plugin keeps per window. */
struct wrec {
    struct wrec *next;
    struct sfwc_toplevel *t;
    struct wlr_scene_tree *last_frame; /* hidden copy of what the window showed, for fade-out */
    uint32_t last_frame_ms;
    struct { /* focus transition of the frame colors (animation type `border`) */
        bool active;
        uint32_t start_ms, duration_ms;
        double from, to;
        struct anim_curve curve;
    } border;
};

static struct {
    struct sfwc_host *host;
    struct animcfg cfg;
    struct wl_list animations; /* struct animation */
    struct wrec *wrecs;
} G;

static uint32_t now_msec(void)
{
    return G.host->now_ms(G.host);
}

static struct wrec *wrec_of(struct sfwc_toplevel *t, bool create)
{
    for (struct wrec *w = G.wrecs; w; w = w->next) {
        if (w->t == t) {
            return w;
        }
    }
    if (!create) {
        return NULL;
    }
    struct wrec *w = calloc(1, sizeof *w);
    if (w) {
        w->t = t;
        w->next = G.wrecs;
        G.wrecs = w;
    }
    return w;
}

static void wrec_free(struct wrec *w)
{
    for (struct wrec **pp = &G.wrecs; *pp; pp = &(*pp)->next) {
        if (*pp == w) {
            *pp = w->next;
            break;
        }
    }
    if (w->last_frame) {
        wlr_scene_node_destroy(&w->last_frame->node);
    }
    free(w);
}

/* ------------------------------------------------------------ cairo image buffers */

/* A wlr_buffer backed by a cairo image surface (the flames are drawn in software). */
struct cairo_buffer {
    struct wlr_buffer base;
    cairo_surface_t *surface;
};

static void cairo_buffer_destroy(struct wlr_buffer *buffer)
{
    struct cairo_buffer *cb = wl_container_of(buffer, cb, base);
    cairo_surface_destroy(cb->surface);
    free(cb);
}

static bool cairo_buffer_begin_data_ptr_access(struct wlr_buffer *buffer, uint32_t flags, void **data,
                                               uint32_t *format, size_t *stride)
{
    struct cairo_buffer *cb = wl_container_of(buffer, cb, base);
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
        return false;
    }
    *data = cairo_image_surface_get_data(cb->surface);
    *format = DRM_FORMAT_ARGB8888;
    *stride = cairo_image_surface_get_stride(cb->surface);
    return true;
}

static void cairo_buffer_end_data_ptr_access(struct wlr_buffer *buffer) {}

static const struct wlr_buffer_impl cairo_buffer_impl = {
    .destroy = cairo_buffer_destroy,
    .begin_data_ptr_access = cairo_buffer_begin_data_ptr_access,
    .end_data_ptr_access = cairo_buffer_end_data_ptr_access,
};

/* Show a cairo image in a scene buffer (takes ownership of the surface). */
static void scene_buffer_set_cairo(struct wlr_scene_buffer *node, cairo_surface_t *surface)
{
    struct cairo_buffer *cb = calloc(1, sizeof(*cb));
    wlr_buffer_init(&cb->base, &cairo_buffer_impl, cairo_image_surface_get_width(surface),
                    cairo_image_surface_get_height(surface));
    cb->surface = surface;
    wlr_scene_buffer_set_buffer(node, &cb->base);
    wlr_buffer_drop(&cb->base); /* the scene node holds its own lock */
}

static bool scene_buffer_no_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
    return false;
}

/* ------------------------------------------------------------ settings */

/* config `enabled` (reduced motion) and the SFWC_NO_ANIMATIONS=1 environment variable */
static bool animations_enabled(void)
{
    const char *off = getenv("SFWC_NO_ANIMATIONS");
    return G.cfg.anim_enabled && !(off && !strcmp(off, "1"));
}

/* What one animation type is configured to do, from a rule or from the simple keys. */
struct anim_cfg {
    uint32_t duration_ms;
    struct anim_curve curve;
    enum anim_style style;
    int percent;
    enum anim_dir dir;
    bool legacy;  /* from the simple keys */
    int slide_px; /* simple keys: how far the window slides */
};

/* the old `easing` names as Bézier curves (the same shapes: cubic ease in / out / in-out) */
static struct anim_curve legacy_curve(const char *easing)
{
    if (!strcmp(easing, "linear")) {
        return (struct anim_curve){0, 0, 1, 1};
    } else if (!strcmp(easing, "ease-in")) {
        return (struct anim_curve){0.55, 0.055, 0.675, 0.19};
    } else if (!strcmp(easing, "ease-in-out")) {
        return (struct anim_curve){0.645, 0.045, 0.355, 1};
    }
    return (struct anim_curve){0.215, 0.61, 0.355, 1}; /* ease-out, the default */
}

/* false when this type of animation is off */
static bool cfg_get(enum anim_type type, struct anim_cfg *out)
{
    const struct animcfg *c = &G.cfg;
    if (!animations_enabled()) {
        return false;
    }
    memset(out, 0, sizeof *out);
    struct anim_rule r;
    if (animcfg_rule(c, type, &r)) {
        struct anim_curve curve;
        if (!r.on || !animcfg_find_curve(c, r.curve, &curve)) {
            return false;
        }
        out->duration_ms = (uint32_t)(r.speed * 100.0 + 0.5);
        out->curve = curve;
        out->style = r.style;
        out->percent = r.percent;
        out->dir = r.dir;
        return out->duration_ms > 0;
    }
    /* the simple keys */
    out->legacy = true;
    out->duration_ms = (uint32_t)c->anim_duration_ms;
    out->curve = legacy_curve(c->anim_easing);
    if (out->duration_ms == 0) {
        return false;
    }
    const char *style = NULL;
    switch (type) {
    case ANIMT_WINDOWS_IN:
    case ANIMT_FADE_IN:
        style = c->anim_open;
        break;
    case ANIMT_WINDOWS_OUT:
    case ANIMT_FADE_OUT:
        style = c->anim_close;
        break;
    case ANIMT_WINDOWS_MOVE:
        return c->anim_move;
    default:
        return false; /* border, workspaces and layers only exist as rules */
    }
    if (!strcmp(style, "none")) {
        return false;
    }
    if (type == ANIMT_WINDOWS_IN || type == ANIMT_WINDOWS_OUT) {
        out->slide_px = !strcmp(style, "slide") ? 32 : !strcmp(style, "fade-scale") ? 10 : 0;
        return out->slide_px > 0; /* "fade" has no movement; the fade track does it */
    }
    return true;
}

static void cfg_log(int level, int line, const char *text, void *data)
{
    G.host->log(G.host, level == ANIMCFG_ERROR ? 1 : 2, "config line %d: %s", line, text);
}

static void cfg_load(void)
{
    animcfg_defaults(&G.cfg);
    const char *key, *value;
    int line;
    for (size_t i = 0, n = G.host->config_count(G.host); i < n; i++) {
        if (G.host->config_entry(G.host, i, &key, &value, &line)) {
            animcfg_apply(&G.cfg, key, value, line, cfg_log, NULL);
        }
    }
}

/* -------------------------------------------------------------- tracks */

static void track_start(struct anim_track *t, const struct anim_cfg *c, uint32_t now)
{
    t->on = true;
    t->start_ms = now;
    t->duration_ms = c->duration_ms;
    t->curve = c->curve;
}

/* curve value at `now`; *done tells whether the track has run its course */
static double track_value(const struct anim_track *t, uint32_t now, bool *done)
{
    if (!t->on) {
        *done = true;
        return 1;
    }
    double p = anim_progress(t->start_ms, now, t->duration_ms);
    *done = p >= 1;
    return anim_curve_eval(&t->curve, p);
}

static void set_opacity_iter(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
    wlr_scene_buffer_set_opacity(buffer, *(float *)data);
}

static void tree_set_opacity(struct wlr_scene_tree *tree, double opacity)
{
    float o = opacity < 0 ? 0 : opacity > 1 ? 1 : (float)opacity;
    wlr_scene_node_for_each_buffer(&tree->node, set_opacity_iter, &o);
}

static void scale_rec_destroy(struct wl_listener *listener, void *data)
{
    struct scale_rec *rec = wl_container_of(listener, rec, destroy);
    wl_list_remove(&rec->destroy.link);
    wl_list_remove(&rec->link);
    free(rec);
}

static void scale_recs_free(struct animation *a)
{
    struct scale_rec *rec, *tmp;
    wl_list_for_each_safe(rec, tmp, &a->scale_recs, link) {
        wl_list_remove(&rec->destroy.link);
        wl_list_remove(&rec->link);
        free(rec);
    }
}

static void scale_collect_iter(struct wlr_scene_buffer *sb, int sx, int sy, void *data)
{
    struct animation *a = data;
    struct scale_rec *rec;
    wl_list_for_each(rec, &a->scale_recs, link) {
        if (rec->buffer == sb) {
            return;
        }
    }
    int w = sb->dst_width ? sb->dst_width : (sb->buffer ? sb->buffer->width : 0);
    int h = sb->dst_height ? sb->dst_height : (sb->buffer ? sb->buffer->height : 0);
    if (w <= 0 || h <= 0) {
        return;
    }
    rec = calloc(1, sizeof(*rec));
    if (!rec) {
        return;
    }
    rec->buffer = sb;
    rec->node_x = sb->node.x;
    rec->node_y = sb->node.y;
    rec->sx = sx;
    rec->sy = sy;
    rec->w = w;
    rec->h = h;
    rec->dst_w = sb->dst_width;
    rec->dst_h = sb->dst_height;
    rec->had_src = sb->src_box.width > 0 && sb->src_box.height > 0;
    rec->src = sb->src_box;
    rec->was_enabled = sb->node.enabled;
    rec->destroy.notify = scale_rec_destroy;
    wl_signal_add(&sb->node.events.destroy, &rec->destroy);
    wl_list_insert(&a->scale_recs, &rec->link);
}

/* Find the buffers of the animated tree (new ones that appeared are added) and, the first time,
 * the area they cover. False while there is nothing yet. */
static bool recs_prepare(struct animation *a)
{
    wlr_scene_node_for_each_buffer(&a->tree->node, scale_collect_iter, a);
    if (a->centered) {
        return true;
    }
    int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    struct scale_rec *rec;
    wl_list_for_each(rec, &a->scale_recs, link) {
        x0 = rec->sx < x0 ? rec->sx : x0;
        y0 = rec->sy < y0 ? rec->sy : y0;
        x1 = rec->sx + rec->w > x1 ? rec->sx + rec->w : x1;
        y1 = rec->sy + rec->h > y1 ? rec->sy + rec->h : y1;
    }
    if (x0 > x1) {
        return false;
    }
    a->bx0 = x0;
    a->by0 = y0;
    a->bx1 = x1;
    a->by1 = y1;
    a->cx = (x0 + x1) / 2.0;
    a->cy = (y0 + y1) / 2.0;
    a->centered = true;
    return true;
}

/* Put one buffer back as it was. */
static void rec_restore(struct scale_rec *rec)
{
    wlr_scene_node_set_position(&rec->buffer->node, rec->node_x, rec->node_y);
    wlr_scene_buffer_set_dest_size(rec->buffer, rec->dst_w, rec->dst_h);
    wlr_scene_buffer_set_source_box(rec->buffer, rec->had_src ? &rec->src : NULL);
    wlr_scene_node_set_enabled(&rec->buffer->node, rec->was_enabled);
}

/* Scale every buffer of the tree by sx and sy around the center of the tree (1, 1 restores). */
static void scale_apply(struct animation *a, double sx, double sy)
{
    if (!recs_prepare(a)) {
        return;
    }
    struct scale_rec *rec;
    wl_list_for_each(rec, &a->scale_recs, link) {
        if (sx == 1.0 && sy == 1.0) {
            rec_restore(rec);
            continue;
        }
        int dx = (int)lround((rec->sx - a->cx) * (sx - 1));
        int dy = (int)lround((rec->sy - a->cy) * (sy - 1));
        int w = (int)lround(rec->w * sx), h = (int)lround(rec->h * sy);
        wlr_scene_node_set_position(&rec->buffer->node, rec->node_x + dx, rec->node_y + dy);
        wlr_scene_buffer_set_dest_size(rec->buffer, w < 1 ? 1 : w, h < 1 ? 1 : h);
    }
}

/* Show only what is above the line `line_y` (layout coordinates): buffers below it are hidden,
 * the one it cuts is cropped (source box and size in proportion). */
static void crop_apply(struct animation *a, double line_y)
{
    if (!recs_prepare(a)) {
        return;
    }
    struct scale_rec *rec;
    wl_list_for_each(rec, &a->scale_recs, link) {
        double vis = line_y - rec->sy;
        if (vis >= rec->h) {
            rec_restore(rec);
        } else if (vis <= 0) {
            wlr_scene_node_set_enabled(&rec->buffer->node, false);
        } else {
            struct wlr_fbox src = rec->src;
            if (!rec->had_src) {
                src = (struct wlr_fbox){0, 0, rec->buffer->buffer ? rec->buffer->buffer->width : rec->w,
                                        rec->buffer->buffer ? rec->buffer->buffer->height : rec->h};
            }
            src.height *= vis / rec->h;
            wlr_scene_node_set_enabled(&rec->buffer->node, rec->was_enabled);
            wlr_scene_buffer_set_source_box(rec->buffer, &src);
            wlr_scene_buffer_set_dest_size(rec->buffer, rec->w, (int)lround(vis) < 1 ? 1 : (int)lround(vis));
        }
    }
}

/* squeeze: q runs 0 (whole window) to 1 (gone): the height collapses to a line, then the width */
static void squeeze_scales(double q, double *sx, double *sy)
{
    q = q < 0 ? 0 : q > 1 ? 1 : q;
    if (q < 0.7) {
        *sy = 1 - 0.98 * (q / 0.7);
        *sx = 1;
    } else {
        *sy = 0.02;
        *sx = 1 - 0.98 * ((q - 0.7) / 0.3);
    }
}

/* fire: flames along the burn line, drawn at half resolution into an overlay above the window */
static void fire_draw(struct animation *a, double line_y, bool emitting, uint32_t now)
{
    if (!a->fire) {
        const struct animcfg *c = &G.cfg;
        int margin = c->fire_size * 3 > 40 ? c->fire_size * 3 : 40;
        a->fire = fire_sim_new(c->fire_particles, c->fire_size, c->fire_color, now | 1);
        a->fire_buf = wlr_scene_buffer_create(G.host->windows_tree, NULL);
        if (!a->fire || !a->fire_buf) {
            return;
        }
        a->fire_buf->point_accepts_input = scene_buffer_no_input;
        a->fire_ox = (int)a->bx0 - margin;
        a->fire_oy = (int)a->by0 - margin;
        a->fire_w = (int)(a->bx1 - a->bx0) + 2 * margin;
        a->fire_h = (int)(a->by1 - a->by0) + 2 * margin;
        a->last_ms = now;
    }
    double dt = (now - a->last_ms) / 1000.0;
    a->last_ms = now;
    fire_sim_step(a->fire, dt, a->bx0, a->bx1, line_y, emitting);
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (a->fire_w + 1) / 2, (a->fire_h + 1) / 2);
    fire_sim_draw(a->fire, surf, a->fire_ox, a->fire_oy, 0.5);
    scene_buffer_set_cairo(a->fire_buf, surf);
    wlr_scene_buffer_set_dest_size(a->fire_buf, a->fire_w, a->fire_h);
    wlr_scene_node_set_position(&a->fire_buf->node, a->fire_ox, a->fire_oy);
    wlr_scene_node_raise_to_top(&a->fire_buf->node);
}

/* ------------------------------------------------------- the animations */

static void busy_add(struct animation *a, int delta)
{
    if (a->toplevel && a->busy == (delta < 0)) {
        G.host->toplevel_busy(G.host, a->toplevel, delta);
        a->busy = delta > 0;
    }
}

static struct animation *anim_new(enum anim_kind kind, struct sfwc_toplevel *t, struct wlr_scene_tree *tree)
{
    struct animation *a = calloc(1, sizeof(*a));
    if (!a) {
        return NULL;
    }
    a->kind = kind;
    a->toplevel = t;
    a->tree = tree;
    a->from_x = a->to_x = tree->node.x;
    a->from_y = a->to_y = tree->node.y;
    a->from_scale = a->to_scale = 1;
    a->from_opacity = a->to_opacity = 1;
    wl_list_init(&a->scale_recs);
    wl_list_insert(&G.animations, &a->link);
    /* windows that fade or grow in: other plugins keep out of the way */
    if (kind == ANIM_OPEN || kind == ANIM_WORKSPACE) {
        busy_add(a, +1);
    }
    return a;
}

/* Apply the state at time `now`; true when the animation has run its course. */
static bool animation_apply(struct animation *a, uint32_t now)
{
    bool geo_done, fade_done;
    double eg = track_value(&a->geo, now, &geo_done);
    double ef = track_value(&a->fade, now, &fade_done);
    wlr_scene_node_set_position(&a->tree->node, (int)lround(anim_lerp(a->from_x, a->to_x, eg)),
                                (int)lround(anim_lerp(a->from_y, a->to_y, eg)));
    if (a->fx == ANIM_STYLE_SQUEEZE) {
        double sx, sy;
        squeeze_scales(a->fx_closing ? eg : 1 - eg, &sx, &sy);
        scale_apply(a, sx, sy);
    } else if (a->fx == ANIM_STYLE_FIRE) {
        /* the burn takes the first 80% of the time, the rest is the last flames dying down */
        double c = eg / 0.8;
        c = c < 0 ? 0 : c > 1 ? 1 : c;
        if (!a->fx_closing) {
            c = 1 - c;
        }
        if (recs_prepare(a)) {
            double line_y = a->by1 - c * (a->by1 - a->by0);
            crop_apply(a, line_y);
            fire_draw(a, line_y, c > 0 && c < 1, now);
        }
    } else if (a->scaled) {
        double s = anim_lerp(a->from_scale, a->to_scale, eg);
        scale_apply(a, s, s);
    }
    if (a->from_opacity != a->to_opacity) {
        tree_set_opacity(a->tree, anim_lerp(a->from_opacity, a->to_opacity, ef));
    }
    return geo_done && fade_done;
}

static void animation_free(struct animation *a)
{
    busy_add(a, -1);
    if (a->fire_buf) {
        wlr_scene_node_destroy(&a->fire_buf->node);
    }
    fire_sim_free(a->fire);
    scale_recs_free(a);
    wl_list_remove(&a->link);
    free(a);
}

/* Jump to the end state and drop the animation. */
static void animation_finish(struct animation *a)
{
    if (a->destroy_tree) {
        busy_add(a, -1);
        if (a->fire_buf) {
            wlr_scene_node_destroy(&a->fire_buf->node);
        }
        fire_sim_free(a->fire);
        scale_recs_free(a); /* the buffers go with the tree */
        wlr_scene_node_destroy(&a->tree->node);
        wl_list_remove(&a->link);
        free(a);
        return;
    }
    if (a->scaled || a->fx == ANIM_STYLE_SQUEEZE) {
        scale_apply(a, 1, 1);
    } else if (a->fx == ANIM_STYLE_FIRE) {
        struct scale_rec *rec;
        wl_list_for_each(rec, &a->scale_recs, link) {
            rec_restore(rec);
        }
    }
    tree_set_opacity(a->tree, 1);
    if (a->hide_at_end) {
        wlr_scene_node_set_position(&a->tree->node, (int)lround(a->rest_x), (int)lround(a->rest_y));
        struct sfwc_toplevel_info info;
        if (a->toplevel && G.host->toplevel_info(G.host, a->toplevel, &info)) {
            wlr_scene_node_set_enabled(&a->tree->node, info.shown);
        }
    } else {
        wlr_scene_node_set_position(&a->tree->node, (int)lround(a->to_x), (int)lround(a->to_y));
    }
    animation_free(a);
}

/* Show the first frame's state right away (no flash of the end state), and ask for frames. */
static void anim_begin(struct animation *a)
{
    animation_apply(a, a->geo.on ? a->geo.start_ms : a->fade.start_ms);
    G.host->request_frame(G.host);
}

static void border_finish(struct wrec *w)
{
    if (w->border.active) {
        w->border.active = false;
        G.host->toplevel_set_focus_mix(G.host, w->t, w->border.to);
    }
}

static void border_tick(uint32_t now)
{
    for (struct wrec *w = G.wrecs; w; w = w->next) {
        if (!w->border.active) {
            continue;
        }
        double p = anim_progress(w->border.start_ms, now, w->border.duration_ms);
        if (p >= 1) {
            border_finish(w);
        } else {
            G.host->toplevel_set_focus_mix(G.host, w->t,
                                           anim_lerp(w->border.from, w->border.to, anim_curve_eval(&w->border.curve, p)));
        }
    }
}

/* Called for every output frame, before the scene is committed. */
static void on_frame(struct sfwc_host *host, uint32_t now)
{
    struct animation *a, *tmp;
    wl_list_for_each_safe(a, tmp, &G.animations, link) {
        if (animation_apply(a, now)) {
            animation_finish(a);
        }
    }
    border_tick(now);
}

/* Finish (or drop) what runs on one window. */
static void cancel_window(struct sfwc_toplevel *t, bool finish)
{
    struct animation *a, *tmp;
    wl_list_for_each_safe(a, tmp, &G.animations, link) {
        if (a->toplevel == t) {
            if (finish) {
                animation_finish(a);
            } else {
                animation_free(a);
            }
        }
    }
    struct wrec *w = wrec_of(t, false);
    if (w && finish) {
        border_finish(w); /* the frame colors arrive at the look they were going to */
    } else if (w) {
        w->border.active = false;
    }
}

/* ------------------------------------------------------ geometry styles */

/* How far the window slides to leave through (or arrive from) the nearest screen edge; `fraction`
 * is the part of the way (1 = completely off the screen). */
static void slide_offset(const struct wlr_box *box, const struct wlr_box *out, enum anim_dir dir,
                         double fraction, double *dx, double *dy)
{
    *dx = *dy = 0;
    if (out->width <= 0 || out->height <= 0) {
        return;
    }
    if (dir == ANIM_DIR_AUTO) {
        double cx = box->x + box->width / 2.0, cy = box->y + box->height / 2.0;
        double left = cx - out->x, right = out->x + out->width - cx;
        double top = cy - out->y, bottom = out->y + out->height - cy;
        double best = left;
        dir = ANIM_DIR_LEFT;
        if (right < best) {
            best = right;
            dir = ANIM_DIR_RIGHT;
        }
        if (top < best) {
            best = top;
            dir = ANIM_DIR_TOP;
        }
        if (bottom < best) {
            dir = ANIM_DIR_BOTTOM;
        }
    }
    switch (dir) {
    case ANIM_DIR_LEFT:
        *dx = -(box->x + box->width - out->x) * fraction;
        break;
    case ANIM_DIR_RIGHT:
        *dx = (out->x + out->width - box->x) * fraction;
        break;
    case ANIM_DIR_TOP:
        *dy = -(box->y + box->height - out->y) * fraction;
        break;
    default:
        *dy = (out->y + out->height - box->y) * fraction;
        break;
    }
}

/* Set up the geometry track of an opening or closing window from its rule. Returns true when
 * the style fades the window by itself (slidefade), so the caller makes sure a fade runs. */
static bool style_geometry(struct animation *a, const struct anim_cfg *g, const struct wlr_box *box,
                           const struct wlr_box *out, bool closing, uint32_t now)
{
    double rest_x = a->tree->node.x, rest_y = a->tree->node.y;
    double dx = 0, dy = 0, scale = 1;
    bool fades = false;
    if (g->legacy) {
        dy = g->slide_px;
    } else {
        enum anim_style style = g->style == ANIM_STYLE_DEFAULT ? ANIM_STYLE_SLIDE : g->style;
        switch (style) {
        case ANIM_STYLE_POPIN:
            scale = (g->percent ? g->percent : 80) / 100.0;
            break;
        case ANIM_STYLE_ZOOM: /* popin that fades, like Wayfire's zoom */
            scale = (g->percent ? g->percent : 75) / 100.0;
            fades = true;
            break;
        case ANIM_STYLE_SQUEEZE:
        case ANIM_STYLE_FIRE:
            a->fx = style;
            a->fx_closing = closing;
            break;
        case ANIM_STYLE_SLIDEFADE:
            slide_offset(box, out, g->dir, (g->percent ? g->percent : 20) / 100.0, &dx, &dy);
            /* a slidefade moves by a part of the window's size, not of the way off screen */
            if (dx != 0) {
                dx = (dx < 0 ? -1 : 1) * box->width * (g->percent ? g->percent : 20) / 100.0;
            }
            if (dy != 0) {
                dy = (dy < 0 ? -1 : 1) * box->height * (g->percent ? g->percent : 20) / 100.0;
            }
            fades = true;
            break;
        default:
            slide_offset(box, out, g->dir, 1.0, &dx, &dy);
            break;
        }
    }
    a->scaled = scale != 1;
    track_start(&a->geo, g, now);
    if (closing) {
        a->from_x = rest_x;
        a->from_y = rest_y;
        a->to_x = rest_x + dx;
        a->to_y = rest_y + dy;
        a->from_scale = 1;
        a->to_scale = scale;
    } else {
        a->from_x = rest_x + dx;
        a->from_y = rest_y + dy;
        a->to_x = rest_x;
        a->to_y = rest_y;
        a->from_scale = scale;
        a->to_scale = 1;
    }
    return fades;
}

/* The window's whole look (frame included) and the output it is on, in layout coordinates. */
static bool window_boxes(struct sfwc_toplevel *t, struct sfwc_toplevel_info *info, struct wlr_box *box,
                         struct wlr_box *out)
{
    if (!G.host->toplevel_info(G.host, t, info)) {
        return false;
    }
    *box = info->outer;
    G.host->output_box_at(G.host, box->x + box->width / 2.0, box->y + box->height / 2.0, out);
    return true;
}

/* Both tracks of an opening or closing animation. */
static struct animation *open_close(struct sfwc_toplevel *t, struct wlr_scene_tree *tree, enum anim_kind kind,
                                    const struct anim_cfg *g, bool have_g, const struct anim_cfg *f, bool have_f)
{
    bool closing = kind == ANIM_CLOSE;
    struct sfwc_toplevel_info info;
    struct wlr_box box, out;
    if (!window_boxes(t, &info, &box, &out)) {
        return NULL;
    }
    struct animation *a = anim_new(kind, closing ? NULL : t, tree);
    if (!a) {
        return NULL;
    }
    uint32_t now = now_msec();
    bool forces_fade = false;
    if (have_g) {
        forces_fade = style_geometry(a, g, &box, &out, closing, now);
    }
    if (have_f) {
        track_start(&a->fade, f, now);
    } else if (forces_fade) {
        track_start(&a->fade, g, now); /* slidefade fades with the movement's timing */
    }
    if (a->fade.on) {
        a->from_opacity = closing ? 1 : 0;
        a->to_opacity = closing ? 0 : 1;
    }
    a->destroy_tree = closing;
    anim_begin(a);
    return a;
}

/* ------------------------------------------------------------ open/close */

static void on_map(struct sfwc_host *host, struct sfwc_toplevel *t)
{
    struct sfwc_toplevel_info info;
    if (!host->toplevel_info(host, t, &info)) {
        return;
    }
    struct anim_cfg g, f;
    bool have_g = cfg_get(ANIMT_WINDOWS_IN, &g);
    bool have_f = cfg_get(ANIMT_FADE_IN, &f);
    if (have_g || have_f) {
        open_close(t, info.tree, ANIM_OPEN, &g, have_g, &f, have_f);
    }
}

struct snapshot_ctx {
    struct wlr_scene_tree *snap;
    int ox, oy; /* position of the window's tree: the iterator's coordinates include it */
    int count;
};

static void snapshot_iter(struct wlr_scene_buffer *sb, int sx, int sy, void *data)
{
    struct snapshot_ctx *ctx = data;
    if (!sb->buffer) {
        return;
    }
    struct wlr_scene_buffer *copy = wlr_scene_buffer_create(ctx->snap, sb->buffer);
    wlr_scene_node_set_position(&copy->node, sx - ctx->ox, sy - ctx->oy);
    wlr_scene_buffer_set_dest_size(copy, sb->dst_width, sb->dst_height);
    wlr_scene_buffer_set_source_box(copy, &sb->src_box);
    wlr_scene_buffer_set_transform(copy, sb->transform);
    ctx->count++;
}

/* Keep a hidden, reference-only copy of the window's current buffers (no pixels are copied).
 * By the time a window unmaps, the scene has already dropped them, so the copy is made while
 * the window is still showing. Refreshed on commits, at most every 50 ms. */
static void on_commit(struct sfwc_host *host, struct sfwc_toplevel *t)
{
    struct anim_cfg g, f;
    struct wrec *w = wrec_of(t, true);
    if (!w) {
        return;
    }
    if (!cfg_get(ANIMT_WINDOWS_OUT, &g) && !cfg_get(ANIMT_FADE_OUT, &f)) {
        if (w->last_frame) {
            wlr_scene_node_destroy(&w->last_frame->node);
            w->last_frame = NULL;
        }
        return;
    }
    uint32_t now = now_msec();
    if (w->last_frame && now - w->last_frame_ms < 50) {
        return;
    }
    struct sfwc_toplevel_info info;
    if (!host->toplevel_info(host, t, &info)) {
        return;
    }
    struct wlr_scene_tree *copy = wlr_scene_tree_create(host->windows_tree);
    wlr_scene_node_set_enabled(&copy->node, false);
    struct snapshot_ctx ctx = {copy, info.tree->node.x, info.tree->node.y, 0};
    wlr_scene_node_for_each_buffer(&info.tree->node, snapshot_iter, &ctx);
    if (ctx.count == 0) {
        wlr_scene_node_destroy(&copy->node);
        return;
    }
    if (w->last_frame) {
        wlr_scene_node_destroy(&w->last_frame->node);
    }
    w->last_frame = copy;
    w->last_frame_ms = now;
}

/* The window is going away: animate the copy of what it showed. */
static void on_unmap(struct sfwc_host *host, struct sfwc_toplevel *t)
{
    cancel_window(t, false);
    struct wrec *w = wrec_of(t, false);
    if (!w) {
        return;
    }
    struct wlr_scene_tree *snap = w->last_frame;
    w->last_frame = NULL;
    wrec_free(w);
    if (!snap) {
        return;
    }
    struct anim_cfg g, f;
    bool have_g = cfg_get(ANIMT_WINDOWS_OUT, &g);
    bool have_f = cfg_get(ANIMT_FADE_OUT, &f);
    struct sfwc_toplevel_info info;
    if ((!have_g && !have_f) || !host->toplevel_info(host, t, &info)) {
        wlr_scene_node_destroy(&snap->node);
        return;
    }
    int lx, ly;
    wlr_scene_node_coords(&info.tree->node, &lx, &ly);
    wlr_scene_node_set_position(&snap->node, lx, ly);
    wlr_scene_node_set_enabled(&snap->node, true);
    wlr_scene_node_raise_to_top(&snap->node);
    if (!open_close(t, snap, ANIM_CLOSE, &g, have_g, &f, have_f)) {
        wlr_scene_node_destroy(&snap->node);
    }
}

/* ------------------------------------------------------------------ move */

/* Slide a window from one position to another (maximize, restore, to the next output). */
static bool on_move(struct sfwc_host *host, struct sfwc_toplevel *t, double from_x, double from_y, double to_x, double to_y)
{
    struct anim_cfg g;
    struct sfwc_toplevel_info info;
    if (!cfg_get(ANIMT_WINDOWS_MOVE, &g) || !host->toplevel_info(host, t, &info)) {
        return false;
    }
    struct animation *a = anim_new(ANIM_MOVE, t, info.tree);
    if (!a) {
        return false;
    }
    track_start(&a->geo, &g, now_msec());
    a->from_x = from_x;
    a->from_y = from_y;
    a->to_x = to_x;
    a->to_y = to_y;
    anim_begin(a);
    return true;
}

static void on_cancel(struct sfwc_host *host, struct sfwc_toplevel *t)
{
    cancel_window(t, true);
}

/* ---------------------------------------------------------------- border */

/* The frame colors move from the look at `from` to the look at `to` (0 unfocused, 1 focused). */
static bool on_focus(struct sfwc_host *host, struct sfwc_toplevel *t, double from, double to)
{
    struct anim_cfg b;
    if (!cfg_get(ANIMT_BORDER, &b)) {
        return false;
    }
    struct wrec *w = wrec_of(t, true);
    if (!w) {
        return false;
    }
    w->border.active = true;
    w->border.start_ms = now_msec();
    w->border.duration_ms = b.duration_ms;
    w->border.from = from;
    w->border.to = to;
    w->border.curve = b.curve;
    host->request_frame(host);
    return true;
}

/* ------------------------------------------------------------ workspaces */

/* A new workspace switch while the last one is still sliding: jump to its end first. */
static void on_ws_leaving(struct sfwc_host *host, int old_ws, int new_ws)
{
    struct animation *a, *tmp;
    wl_list_for_each_safe(a, tmp, &G.animations, link) {
        if (a->kind == ANIM_WORKSPACE) {
            animation_finish(a);
        }
    }
}

/* Slide or fade the windows of the workspace we leave out and those of the one we enter in. */
static void on_ws_entered(struct sfwc_host *host, int old_ws, int new_ws)
{
    struct anim_cfg g;
    if (!cfg_get(ANIMT_WORKSPACES, &g)) {
        return;
    }
    enum anim_style style = g.style == ANIM_STYLE_DEFAULT ? ANIM_STYLE_SLIDE : g.style;
    bool vertical = style == ANIM_STYLE_SLIDEVERT || style == ANIM_STYLE_SLIDEFADEVERT;
    bool fades = style == ANIM_STYLE_FADE || style == ANIM_STYLE_SLIDEFADE || style == ANIM_STYLE_SLIDEFADEVERT;
    bool moves = style != ANIM_STYLE_FADE;
    double part = style == ANIM_STYLE_SLIDEFADE || style == ANIM_STYLE_SLIDEFADEVERT
                      ? (g.percent ? g.percent : 20) / 100.0
                      : (g.percent ? g.percent : 100) / 100.0;
    double forward = new_ws > old_ws ? 1 : -1; /* the content moves against the direction of travel */
    uint32_t now = now_msec();

    for (struct sfwc_toplevel *t = host->toplevel_next(host, NULL); t; t = host->toplevel_next(host, t)) {
        struct sfwc_toplevel_info info;
        struct wlr_box box, out;
        if (!window_boxes(t, &info, &box, &out)) {
            continue;
        }
        bool leaving = info.workspace == old_ws, entering = info.workspace == new_ws;
        if (info.minimized || (!leaving && !entering)) {
            continue;
        }
        struct animation *a = anim_new(ANIM_WORKSPACE, t, info.tree);
        if (!a) {
            continue;
        }
        double dist = out.width > 0 ? (vertical ? out.height : out.width) * part : 0;
        a->rest_x = a->from_x = a->to_x = info.tree->node.x;
        a->rest_y = a->from_y = a->to_y = info.tree->node.y;
        a->hide_at_end = true;
        if (moves) {
            double *from = vertical ? &a->from_y : &a->from_x, *to = vertical ? &a->to_y : &a->to_x;
            if (leaving) {
                *to += -forward * dist;
            } else {
                *from += forward * dist;
            }
        }
        track_start(&a->geo, &g, now);
        if (fades) {
            track_start(&a->fade, &g, now);
            a->from_opacity = leaving ? 1 : 0;
            a->to_opacity = leaving ? 0 : 1;
        }
        wlr_scene_node_set_enabled(&info.tree->node, true); /* the old workspace stays until it is out */
        anim_begin(a);
    }
}

/* ---------------------------------------------------------------- layers */

/* A panel, launcher or notification appears: fade it in (popin also grows it). */
static void on_layer_map(struct sfwc_host *host, struct wlr_scene_tree *tree)
{
    struct anim_cfg g;
    if (!cfg_get(ANIMT_LAYERS_IN, &g)) {
        return;
    }
    struct animation *a = anim_new(ANIM_LAYER, NULL, tree);
    if (!a) {
        return;
    }
    uint32_t now = now_msec();
    track_start(&a->geo, &g, now);
    track_start(&a->fade, &g, now);
    a->from_opacity = 0;
    a->to_opacity = 1;
    if (g.style == ANIM_STYLE_POPIN) {
        a->from_scale = (g.percent ? g.percent : 80) / 100.0;
    }
    anim_begin(a);
}

/* The tree (a layer surface) is about to go away. */
static void on_layer_unmap(struct sfwc_host *host, struct wlr_scene_tree *tree)
{
    struct animation *a, *tmp;
    wl_list_for_each_safe(a, tmp, &G.animations, link) {
        if (a->tree == tree && !a->destroy_tree) {
            animation_free(a);
        }
    }
}

/* ---------------------------------------------------------------- plugin */

static bool on_init(struct sfwc_host *host)
{
    memset(&G, 0, sizeof G);
    G.host = host;
    wl_list_init(&G.animations);
    cfg_load();
    return true;
}

static void on_fini(struct sfwc_host *host)
{
    struct animation *a, *tmp;
    wl_list_for_each_safe(a, tmp, &G.animations, link) {
        animation_finish(a); /* windows are left in their normal state */
    }
    for (struct wrec *w = G.wrecs; w;) {
        struct wrec *next = w->next;
        border_finish(w);
        wrec_free(w);
        w = next;
    }
}

static void on_reconfigure(struct sfwc_host *host)
{
    cfg_load();
}

static const struct sfwc_plugin plugin = {
    .api_version = SFWC_PLUGIN_API_VERSION,
    .struct_size = sizeof(struct sfwc_plugin),
    .name = "animations",
    .wlroots_version = WLR_VERSION_STR,
    .init = on_init,
    .fini = on_fini,
    .reconfigure = on_reconfigure,
    .frame = on_frame,
    .toplevel_map = on_map,
    .toplevel_commit = on_commit,
    .toplevel_unmap = on_unmap,
    .toplevel_move = on_move,
    .toplevel_cancel = on_cancel,
    .toplevel_focus = on_focus,
    .workspace_leaving = on_ws_leaving,
    .workspace_entered = on_ws_entered,
    .layer_map = on_layer_map,
    .layer_unmap = on_layer_unmap,
};

SFWC_PLUGIN_EXPORT const struct sfwc_plugin *sfwc_plugin_entry(void)
{
    return &plugin;
}
