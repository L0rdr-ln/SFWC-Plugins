/*
 * Wobbly windows, as an sfwc plugin.
 *
 * wlroots' scene graph cannot warp a picture, so while a window swings it is cut into small
 * tiles: every buffer of the window (the client's surface, the frame) gets a grid of scene
 * buffers that show one piece each of the same wlr_buffer (source box), placed along the deformed
 * mesh of mesh.c. The window's own buffers stay where they are, invisible (opacity 0), so input,
 * frame callbacks and new content keep working; the tiles pick up the current buffer of their
 * original every frame. When the mesh has settled the tiles are removed and the originals are
 * shown again.
 *
 * The mesh is driven by watching the window's position once per frame, so every kind of move
 * wobbles: dragging, maximize, snapping, "next output".
 *
 * Settings ([plugin:wobbly]): grid (3..12), spring (1..1000), friction (0.5..100).
 */
#define _DEFAULT_SOURCE
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <sfwc-plugin.h>
#include "mesh.h"

#define TILE_PX 40 /* about this big, before the mesh stretches them */
#define MAX_TILES_SIDE 20

struct wob;

/* All tiles of one original buffer. */
struct wob_buf {
    struct wob_buf *next;
    struct wlr_scene_buffer *orig;
    struct wl_listener destroy;
    float orig_opacity;
    double u0, v0, u1, v1; /* where the buffer sits in the window's outer box, 0..1 */
    int nx, ny;
    struct wlr_scene_buffer **tiles; /* nx * ny, NULL for tiles that are not needed */
};

/* The state of one window. */
struct wob {
    struct wob *next;
    struct sfwc_toplevel *t;
    struct wobbly *sim;
    bool active;
    struct wlr_scene_tree *tree; /* the tiles, next to the window in the stacking order */
    struct wob_buf *bufs;
    bool have_last;
    int last_x, last_y, last_w, last_h;
    uint32_t last_ms;
};

static struct {
    struct wob *wobs;
    int grid;
    double spring, friction;
} G;

/* ------------------------------------------------------------------ settings */

static void read_settings(struct sfwc_host *host)
{
    G.grid = 6;
    G.spring = 120;
    G.friction = 9;
    const char *v;
    char *end;
    if ((v = host->config_get(host, "grid"))) {
        long n = strtol(v, &end, 10);
        if (end == v || *end || n < 3 || n > 12) {
            host->log(host, 1, "grid: '%s' is not a whole number from 3 to 12", v);
        } else {
            G.grid = (int)n;
        }
    }
    if ((v = host->config_get(host, "spring"))) {
        double d = strtod(v, &end);
        if (end == v || *end || !(d >= 1 && d <= 1000)) {
            host->log(host, 1, "spring: '%s' is not a number from 1 to 1000", v);
        } else {
            G.spring = d;
        }
    }
    if ((v = host->config_get(host, "friction"))) {
        double d = strtod(v, &end);
        if (end == v || *end || !(d >= 0.5 && d <= 100)) {
            host->log(host, 1, "friction: '%s' is not a number from 0.5 to 100", v);
        } else {
            G.friction = d;
        }
    }
}

/* ------------------------------------------------------------------- tiles */

static bool no_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
    return false;
}

static void buf_unlink(struct wob *w, struct wob_buf *b)
{
    for (struct wob_buf **pp = &w->bufs; *pp; pp = &(*pp)->next) {
        if (*pp == b) {
            *pp = b->next;
            break;
        }
    }
    wl_list_remove(&b->destroy.link);
    free(b->tiles);
    free(b);
}

static void buf_destroyed(struct wl_listener *listener, void *data)
{
    struct wob_buf *b = wl_container_of(listener, b, destroy);
    struct wob *w = NULL;
    for (struct wob *it = G.wobs; it && !w; it = it->next) {
        for (struct wob_buf *x = it->bufs; x; x = x->next) {
            if (x == b) {
                w = it;
            }
        }
    }
    if (!w) {
        return;
    }
    for (int i = 0; i < b->nx * b->ny; i++) {
        if (b->tiles[i]) {
            wlr_scene_node_destroy(&b->tiles[i]->node);
        }
    }
    buf_unlink(w, b); /* the original is gone, nothing to restore */
}

/* Remove the tiles and show the window's own buffers again. */
static void deactivate(struct wob *w)
{
    while (w->bufs) {
        struct wob_buf *b = w->bufs;
        wlr_scene_buffer_set_opacity(b->orig, b->orig_opacity);
        buf_unlink(w, b);
    }
    if (w->tree) {
        wlr_scene_node_destroy(&w->tree->node); /* and with it all tiles */
        w->tree = NULL;
    }
    w->active = false;
}

struct collect {
    struct wob *w;
    struct sfwc_toplevel_info info;
};

static void collect_iter(struct wlr_scene_buffer *sb, int sx, int sy, void *data)
{
    struct collect *c = data;
    struct wob *w = c->w;
    const struct wlr_box *box = &c->info.outer;
    if (sb == c->info.shadow || sb->transform != WL_OUTPUT_TRANSFORM_NORMAL) {
        return; /* the shadow stays where it is; rotated buffers are not tiled */
    }
    int bw = sb->dst_width ? sb->dst_width : (sb->buffer ? sb->buffer->width : 0);
    int bh = sb->dst_height ? sb->dst_height : (sb->buffer ? sb->buffer->height : 0);
    if (bw <= 0 || bh <= 0 || box->width <= 0 || box->height <= 0) {
        return;
    }
    struct wob_buf *b = calloc(1, sizeof *b);
    if (!b) {
        return;
    }
    b->orig = sb;
    b->orig_opacity = sb->opacity;
    b->u0 = (double)(sx - box->x) / box->width;
    b->v0 = (double)(sy - box->y) / box->height;
    b->u1 = (double)(sx + bw - box->x) / box->width;
    b->v1 = (double)(sy + bh - box->y) / box->height;
    b->nx = (bw + TILE_PX - 1) / TILE_PX;
    b->ny = (bh + TILE_PX - 1) / TILE_PX;
    b->nx = b->nx < 1 ? 1 : b->nx > MAX_TILES_SIDE ? MAX_TILES_SIDE : b->nx;
    b->ny = b->ny < 1 ? 1 : b->ny > MAX_TILES_SIDE ? MAX_TILES_SIDE : b->ny;
    b->tiles = calloc((size_t)b->nx * b->ny, sizeof *b->tiles);
    if (!b->tiles) {
        free(b);
        return;
    }
    /* The frame is transparent in the middle (the client shows through): no tiles there. */
    bool is_frame = sb == c->info.frame;
    const struct wlr_box *in = &c->info.inner;
    for (int j = 0; j < b->ny; j++) {
        for (int i = 0; i < b->nx; i++) {
            if (is_frame) {
                double x0 = sx + (double)bw * i / b->nx, x1 = sx + (double)bw * (i + 1) / b->nx;
                double y0 = sy + (double)bh * j / b->ny, y1 = sy + (double)bh * (j + 1) / b->ny;
                if (x0 >= in->x && x1 <= in->x + in->width && y0 >= in->y && y1 <= in->y + in->height) {
                    continue;
                }
            }
            struct wlr_scene_buffer *tile = wlr_scene_buffer_create(w->tree, NULL);
            if (!tile) {
                continue;
            }
            tile->point_accepts_input = no_input;
            b->tiles[j * b->nx + i] = tile;
        }
    }
    b->destroy.notify = buf_destroyed;
    wl_signal_add(&sb->node.events.destroy, &b->destroy);
    b->next = w->bufs;
    w->bufs = b;
    wlr_scene_buffer_set_opacity(sb, 0);
}

/* `info` is the window where it is NOW; the buffers are found at that place. */
static void activate(struct sfwc_host *host, struct wob *w, const struct sfwc_toplevel_info *info)
{
    w->tree = wlr_scene_tree_create(host->windows_tree);
    if (!w->tree) {
        return;
    }
    struct collect c = {.w = w, .info = *info};
    wlr_scene_node_for_each_buffer(&info->tree->node, collect_iter, &c);
    if (!w->bufs) {
        wlr_scene_node_destroy(&w->tree->node);
        w->tree = NULL;
        return;
    }
    w->active = true;
}

/* Put every tile of one original where the mesh says. */
static void tiles_update(struct wob *w, struct wob_buf *b)
{
    struct wlr_scene_buffer *orig = b->orig;
    struct wlr_buffer *buffer = orig->buffer;
    struct wlr_fbox base = orig->src_box;
    if (buffer && (base.width <= 0 || base.height <= 0)) {
        base = (struct wlr_fbox){0, 0, buffer->width, buffer->height};
    }
    for (int j = 0; j < b->ny; j++) {
        for (int i = 0; i < b->nx; i++) {
            struct wlr_scene_buffer *tile = b->tiles[j * b->nx + i];
            if (!tile) {
                continue;
            }
            if (!buffer) {
                wlr_scene_node_set_enabled(&tile->node, false);
                continue;
            }
            wlr_scene_node_set_enabled(&tile->node, orig->node.enabled);
            if (tile->buffer != buffer) {
                wlr_scene_buffer_set_buffer(tile, buffer);
            }
            /* the four corners of this tile on the mesh; the tile covers their bounding box */
            double u[2] = {b->u0 + (b->u1 - b->u0) * i / b->nx, b->u0 + (b->u1 - b->u0) * (i + 1) / b->nx};
            double v[2] = {b->v0 + (b->v1 - b->v0) * j / b->ny, b->v0 + (b->v1 - b->v0) * (j + 1) / b->ny};
            double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
            for (int cv = 0; cv < 2; cv++) {
                for (int cu = 0; cu < 2; cu++) {
                    double x, y;
                    wobbly_point(w->sim, u[cu], v[cv], &x, &y);
                    x0 = x < x0 ? x : x0;
                    y0 = y < y0 ? y : y0;
                    x1 = x > x1 ? x : x1;
                    y1 = y > y1 ? y : y1;
                }
            }
            int tw = (int)ceil(x1 - x0) + 1, th = (int)ceil(y1 - y0) + 1; /* +1: no gaps between tiles */
            wlr_scene_node_set_position(&tile->node, (int)floor(x0), (int)floor(y0));
            wlr_scene_buffer_set_dest_size(tile, tw, th);
            struct wlr_fbox src = {base.x + base.width * i / b->nx, base.y + base.height * j / b->ny,
                                   base.width / b->nx, base.height / b->ny};
            wlr_scene_buffer_set_source_box(tile, &src);
            wlr_scene_buffer_set_opacity(tile, b->orig_opacity);
        }
    }
}

/* ------------------------------------------------------------------ windows */

static struct wob *wob_of(struct sfwc_toplevel *t, bool create)
{
    for (struct wob *w = G.wobs; w; w = w->next) {
        if (w->t == t) {
            return w;
        }
    }
    if (!create) {
        return NULL;
    }
    struct wob *w = calloc(1, sizeof *w);
    if (w) {
        w->t = t;
        w->next = G.wobs;
        G.wobs = w;
    }
    return w;
}

static void wob_free(struct wob *w)
{
    if (w->active) {
        deactivate(w);
    }
    for (struct wob **pp = &G.wobs; *pp; pp = &(*pp)->next) {
        if (*pp == w) {
            *pp = w->next;
            break;
        }
    }
    wobbly_free(w->sim);
    free(w);
}

static void on_unmap(struct sfwc_host *host, struct sfwc_toplevel *t)
{
    struct wob *w = wob_of(t, false);
    if (w) {
        wob_free(w); /* shows the window's own buffers again */
    }
}

static void on_frame(struct sfwc_host *host, uint32_t now)
{
    bool keep_going = false;
    for (struct sfwc_toplevel *t = host->toplevel_next(host, NULL); t; t = host->toplevel_next(host, t)) {
        struct sfwc_toplevel_info info;
        if (!host->toplevel_info(host, t, &info)) {
            continue;
        }
        struct wob *w = wob_of(t, false);
        if (!info.visible || info.busy) {
            if (w) {
                if (w->active) {
                    deactivate(w);
                }
                w->have_last = false;
            }
            continue;
        }
        if (!w && !(w = wob_of(t, true))) {
            continue;
        }
        const struct wlr_box *box = &info.outer;
        bool resized = w->have_last && (box->width != w->last_w || box->height != w->last_h);
        bool moved = w->have_last && !resized && (box->x != w->last_x || box->y != w->last_y);
        int old_x = w->last_x, old_y = w->last_y;
        w->have_last = true;
        w->last_x = box->x;
        w->last_y = box->y;
        w->last_w = box->width;
        w->last_h = box->height;
        if (resized && w->active) {
            deactivate(w); /* the mesh would not fit the new size */
            continue;
        }
        if (!w->active) {
            if (!moved || box->width < 16 || box->height < 16) {
                continue;
            }
            if (!w->sim) {
                w->sim = wobbly_new(G.grid, G.spring, G.friction);
                if (!w->sim) {
                    continue;
                }
            }
            /* start the mesh at the old place; it follows to the new one */
            wobbly_set_rect(w->sim, old_x, old_y, box->width, box->height);
            wobbly_set_grab(w->sim, false, 0, 0);
            activate(host, w, &info);
            if (!w->active) {
                continue;
            }
            host->log(host, 2, "swinging from %d,%d to %d,%d", old_x, old_y, box->x, box->y);
            w->last_ms = now - 1; /* the first step is a millisecond */
        }
        double dt = (now - w->last_ms) / 1000.0;
        if (dt < 0.0005) {
            continue; /* another output's frame in the same instant */
        }
        w->last_ms = now;
        wobbly_set_rect(w->sim, box->x, box->y, box->width, box->height);
        if (info.moving) {
            double cx, cy;
            host->cursor_position(host, &cx, &cy);
            wobbly_set_grab(w->sim, true, cx, cy);
        } else {
            wobbly_set_grab(w->sim, true, box->x + box->width / 2.0, box->y + box->height / 2.0);
        }
        wobbly_step(w->sim, dt);
        if (wobbly_settled(w->sim) && !moved) {
            deactivate(w);
            continue;
        }
        for (struct wob_buf *b = w->bufs; b; b = b->next) {
            tiles_update(w, b);
        }
        wlr_scene_node_set_enabled(&w->tree->node, true);
        wlr_scene_node_place_above(&w->tree->node, &info.tree->node);
        keep_going = true;
    }
    if (keep_going) {
        host->request_frame(host); /* the mesh moves even when no client draws */
    }
}

/* ------------------------------------------------------------------ plugin */

static bool on_init(struct sfwc_host *host)
{
    G.wobs = NULL;
    read_settings(host);
    return true;
}

static void on_fini(struct sfwc_host *host)
{
    while (G.wobs) {
        wob_free(G.wobs);
    }
}

static void on_reconfigure(struct sfwc_host *host)
{
    read_settings(host);
    /* running meshes keep their old stiffness; the next swing uses the new one */
    for (struct wob *w = G.wobs; w; w = w->next) {
        if (!w->active) {
            wobbly_free(w->sim);
            w->sim = NULL;
        }
    }
}

static const struct sfwc_plugin plugin = {
    .api_version = SFWC_PLUGIN_API_VERSION,
    .struct_size = sizeof(struct sfwc_plugin),
    .name = "wobbly",
    .wlroots_version = WLR_VERSION_STR,
    .init = on_init,
    .fini = on_fini,
    .reconfigure = on_reconfigure,
    .frame = on_frame,
    .toplevel_unmap = on_unmap,
};

SFWC_PLUGIN_EXPORT const struct sfwc_plugin *sfwc_plugin_entry(void)
{
    return &plugin;
}
