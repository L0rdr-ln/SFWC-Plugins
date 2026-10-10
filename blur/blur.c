/*
 * Blur behind windows and panels, as an sfwc plugin.
 *
 * wlroots 0.20 has no blur, and the scene graph cannot read back what is behind a window. So this
 * is the "pseudo transparency" kind of blur: the wallpaper (the layer-shell surface that fills an
 * output at the bottom) is blurred in software whenever it changes, and a piece of the blurred
 * picture is shown right behind every window and panel. A translucent window therefore shows a
 * frosted wallpaper. Other windows behind it are not blurred.
 *
 * It needs a wallpaper in a shared-memory buffer (swaybg, hyprpaper and most others use one); with
 * a GPU buffer there is nothing to read and the plugin does nothing.
 *
 * Settings ([plugin:blur]): radius (2..100, pixels), passes (1..5), shrink (1..16, how much the
 * picture is made smaller before blurring: faster, softer), windows (true/false), layers
 * (true/false: panels, launchers, notifications), frame (true: the whole window with its title
 * bar instead of only the client area).
 */
#define _DEFAULT_SOURCE
#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/interfaces/wlr_buffer.h>
#include <sfwc-plugin.h>
#include "boxblur.h"

struct blur_buffer {
    struct wlr_buffer base;
    uint8_t *data;
    int stride;
};

static void bb_destroy(struct wlr_buffer *buffer)
{
    struct blur_buffer *bb = wl_container_of(buffer, bb, base);
    free(bb->data);
    free(bb);
}

static bool bb_begin(struct wlr_buffer *buffer, uint32_t flags, void **data, uint32_t *format, size_t *stride)
{
    struct blur_buffer *bb = wl_container_of(buffer, bb, base);
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
        return false;
    }
    *data = bb->data;
    *format = DRM_FORMAT_XRGB8888;
    *stride = bb->stride;
    return true;
}

static void bb_end(struct wlr_buffer *buffer) {}

static const struct wlr_buffer_impl bb_impl = {
    .destroy = bb_destroy,
    .begin_data_ptr_access = bb_begin,
    .end_data_ptr_access = bb_end,
};

/* A wallpaper: its scene tree, what it showed when we blurred it, and the blurred picture. */
struct paper {
    struct paper *next;
    struct wlr_scene_tree *tree;
    struct wl_listener destroy;
    const struct wlr_buffer *seen; /* the buffer that was blurred */
    struct wlr_buffer *blurred;    /* NULL if there is nothing (yet) */
    struct wlr_box box;            /* layout coordinates of the wallpaper */
    int bw, bh;                    /* size of the source picture in pixels */
};

/* A window or a panel with the blurred piece behind it. */
struct item {
    struct item *next;
    struct sfwc_toplevel *t;       /* window, or NULL for a panel */
    struct wlr_scene_tree *tree;   /* the panel's tree */
    struct wlr_scene_buffer *node; /* the blurred piece */
    struct wl_listener destroy;
};

static struct {
    struct sfwc_host *host;
    struct paper *papers;
    struct item *items;
    int radius, passes, shrink;
    bool windows, layers, frame;
} G;

/* ----------------------------------------------------------------- helpers */

static void bounds_cb(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
    struct wlr_box *b = data;
    if (!buffer->node.enabled) {
        return;
    }
    int w = buffer->dst_width, h = buffer->dst_height;
    if (w <= 0 && buffer->buffer) {
        w = buffer->buffer->width;
        h = buffer->buffer->height;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    if (b->width == 0) {
        *b = (struct wlr_box){sx, sy, w, h};
        return;
    }
    int x1 = b->x + b->width > sx + w ? b->x + b->width : sx + w;
    int y1 = b->y + b->height > sy + h ? b->y + b->height : sy + h;
    b->x = b->x < sx ? b->x : sx;
    b->y = b->y < sy ? b->y : sy;
    b->width = x1 - b->x;
    b->height = y1 - b->y;
}

/* The box a tree covers in layout coordinates (all its enabled buffers). */
static struct wlr_box tree_bounds(struct wlr_scene_tree *tree)
{
    struct wlr_box b = {0};
    wlr_scene_node_for_each_buffer(&tree->node, bounds_cb, &b);
    return b;
}

static void first_buffer_cb(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
    struct wlr_scene_buffer **out = data;
    if (!*out && buffer->buffer && buffer->node.enabled) {
        *out = buffer;
    }
}

static bool covers(const struct wlr_box *outer, const struct wlr_box *inner)
{
    return inner->width >= outer->width * 9 / 10 && inner->height >= outer->height * 9 / 10;
}

/* ----------------------------------------------------------------- wallpapers */

static void paper_free(struct paper *p)
{
    wl_list_remove(&p->destroy.link);
    if (p->blurred) {
        wlr_buffer_drop(p->blurred);
    }
    for (struct paper **pp = &G.papers; *pp; pp = &(*pp)->next) {
        if (*pp == p) {
            *pp = p->next;
            break;
        }
    }
    free(p);
}

static void paper_destroyed(struct wl_listener *listener, void *data)
{
    struct paper *p = wl_container_of(listener, p, destroy);
    paper_free(p);
}

static void paper_blur(struct paper *p)
{
    struct wlr_scene_buffer *sb = NULL;
    wlr_scene_node_for_each_buffer(&p->tree->node, first_buffer_cb, &sb);
    if (!sb) {
        return;
    }
    if (sb->buffer == p->seen) {
        return;
    }
    p->seen = sb->buffer;
    if (p->blurred) {
        wlr_buffer_drop(p->blurred);
        p->blurred = NULL;
    }
    void *data;
    uint32_t format;
    size_t stride;
    if (!wlr_buffer_begin_data_ptr_access(sb->buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
        G.host->log(G.host, 3, "the wallpaper is not in shared memory: no blur");
        return;
    }
    if (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888) {
        wlr_buffer_end_data_ptr_access(sb->buffer);
        G.host->log(G.host, 3, "the wallpaper has an unsupported pixel format: no blur");
        return;
    }
    int w = sb->buffer->width, h = sb->buffer->height, ow, oh;
    uint8_t *out = boxblur_shrink(data, w, h, stride, G.shrink, G.radius / G.shrink > 0 ? G.radius / G.shrink : 1,
                                  G.passes, &ow, &oh);
    wlr_buffer_end_data_ptr_access(sb->buffer);
    if (!out) {
        return;
    }
    struct blur_buffer *bb = calloc(1, sizeof *bb);
    if (!bb) {
        free(out);
        return;
    }
    wlr_buffer_init(&bb->base, &bb_impl, ow, oh);
    bb->data = out;
    bb->stride = ow * 4;
    p->blurred = &bb->base;
    p->bw = w;
    p->bh = h;
    G.host->log(G.host, 2, "wallpaper %dx%d blurred (radius %d)", w, h, G.radius);
}

static void papers_refresh(void)
{
    for (struct paper *p = G.papers; p; p = p->next) {
        p->box = tree_bounds(p->tree);
        paper_blur(p);
    }
}

static struct paper *paper_at(const struct wlr_box *box)
{
    double cx = box->x + box->width / 2.0, cy = box->y + box->height / 2.0;
    for (struct paper *p = G.papers; p; p = p->next) {
        if (p->blurred && cx >= p->box.x && cx < p->box.x + p->box.width && cy >= p->box.y &&
            cy < p->box.y + p->box.height) {
            return p;
        }
    }
    return NULL;
}

/* ----------------------------------------------------------------- items */

static void item_free(struct item *it)
{
    if (it->tree) {
        wl_list_remove(&it->destroy.link);
    }
    if (it->node) {
        wlr_scene_node_destroy(&it->node->node);
    }
    for (struct item **pp = &G.items; *pp; pp = &(*pp)->next) {
        if (*pp == it) {
            *pp = it->next;
            break;
        }
    }
    free(it);
}

static void item_tree_destroyed(struct wl_listener *listener, void *data)
{
    struct item *it = wl_container_of(listener, it, destroy);
    it->tree = NULL; /* the node goes with the parent; do not touch the listener again */
    wl_list_remove(&it->destroy.link);
    wl_list_init(&it->destroy.link);
    it->node = NULL;
    item_free(it);
}

static struct item *item_new(struct sfwc_toplevel *t, struct wlr_scene_tree *tree)
{
    struct item *it = calloc(1, sizeof *it);
    if (!it) {
        return NULL;
    }
    it->t = t;
    it->tree = tree;
    if (tree) {
        it->destroy.notify = item_tree_destroyed;
        wl_signal_add(&tree->node.events.destroy, &it->destroy);
    }
    it->next = G.items;
    G.items = it;
    return it;
}

/* Put the blurred piece behind `below` (a node), covering `box` (layout coordinates). */
static void item_place(struct item *it, struct wlr_scene_node *below, const struct wlr_box *box, bool shown)
{
    struct paper *p = shown && box->width > 0 ? paper_at(box) : NULL;
    if (!p) {
        if (it->node) {
            wlr_scene_node_set_enabled(&it->node->node, false);
        }
        return;
    }
    if (!it->node) {
        it->node = wlr_scene_buffer_create(below->parent, NULL);
        if (!it->node) {
            return;
        }
    } else if (it->node->node.parent != below->parent) {
        wlr_scene_node_reparent(&it->node->node, below->parent);
    }
    if (it->node->buffer != p->blurred) {
        wlr_scene_buffer_set_buffer(it->node, p->blurred);
    }
    /* the piece of the blurred picture that lies behind the box */
    struct wlr_box in = *box;
    int x0 = in.x < p->box.x ? p->box.x : in.x, y0 = in.y < p->box.y ? p->box.y : in.y;
    int x1 = in.x + in.width > p->box.x + p->box.width ? p->box.x + p->box.width : in.x + in.width;
    int y1 = in.y + in.height > p->box.y + p->box.height ? p->box.y + p->box.height : in.y + in.height;
    if (x1 <= x0 || y1 <= y0) {
        wlr_scene_node_set_enabled(&it->node->node, false);
        return;
    }
    double sx = (double)p->blurred->width / p->box.width, sy = (double)p->blurred->height / p->box.height;
    struct wlr_fbox src = {(x0 - p->box.x) * sx, (y0 - p->box.y) * sy, (x1 - x0) * sx, (y1 - y0) * sy};
    wlr_scene_buffer_set_source_box(it->node, &src);
    wlr_scene_buffer_set_dest_size(it->node, x1 - x0, y1 - y0);
    int px = 0, py = 0;
    wlr_scene_node_coords(&below->parent->node, &px, &py);
    wlr_scene_node_set_position(&it->node->node, x0 - px, y0 - py);
    wlr_scene_node_place_below(&it->node->node, below);
    wlr_scene_node_set_enabled(&it->node->node, true);
}

static void items_update(void)
{
    for (struct item *it = G.items; it; it = it->next) {
        if (it->t) {
            struct sfwc_toplevel_info info;
            if (!G.host->toplevel_info(G.host, it->t, &info) || !info.tree) {
                continue;
            }
            const struct wlr_box *box = G.frame ? &info.outer : &info.inner;
            item_place(it, &info.tree->node, box, info.visible && !info.fullscreen);
        } else if (it->tree) {
            struct wlr_box box = tree_bounds(it->tree);
            item_place(it, &it->tree->node, &box, it->tree->node.enabled);
        }
    }
}

/* ----------------------------------------------------------------- callbacks */

static void settings_read(void)
{
    G.radius = 24;
    G.passes = 3;
    G.shrink = 4;
    G.windows = G.layers = true;
    G.frame = false;
    const char *v;
    if ((v = G.host->config_get(G.host, "radius"))) {
        G.radius = atoi(v) < 2 ? 2 : (atoi(v) > 100 ? 100 : atoi(v));
    }
    if ((v = G.host->config_get(G.host, "passes"))) {
        G.passes = atoi(v) < 1 ? 1 : (atoi(v) > 5 ? 5 : atoi(v));
    }
    if ((v = G.host->config_get(G.host, "shrink"))) {
        G.shrink = atoi(v) < 1 ? 1 : (atoi(v) > 16 ? 16 : atoi(v));
    }
    if ((v = G.host->config_get(G.host, "windows"))) {
        G.windows = strcmp(v, "false") != 0 && strcmp(v, "no") != 0 && strcmp(v, "off") != 0 && strcmp(v, "0") != 0;
    }
    if ((v = G.host->config_get(G.host, "layers"))) {
        G.layers = strcmp(v, "false") != 0 && strcmp(v, "no") != 0 && strcmp(v, "off") != 0 && strcmp(v, "0") != 0;
    }
    if ((v = G.host->config_get(G.host, "frame"))) {
        G.frame = !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "on") || !strcmp(v, "1");
    }
}

static void flush_papers_blurred(void)
{
    for (struct paper *p = G.papers; p; p = p->next) {
        p->seen = NULL; /* blur again with the new settings */
    }
}

static void item_window(struct sfwc_toplevel *t)
{
    if (G.windows) {
        item_new(t, NULL);
    }
}

static void on_map(struct sfwc_host *host, struct sfwc_toplevel *t)
{
    item_window(t);
}

static void on_unmap(struct sfwc_host *host, struct sfwc_toplevel *t)
{
    for (struct item *it = G.items; it; it = it->next) {
        if (it->t == t) {
            item_free(it);
            return;
        }
    }
}

static void on_layer_map(struct sfwc_host *host, struct wlr_scene_tree *tree)
{
    /* a surface that fills an output is a wallpaper, anything else is a panel */
    struct wlr_box b = tree_bounds(tree);
    struct wlr_box out;
    host->output_box_at(host, b.x + b.width / 2.0, b.y + b.height / 2.0, &out);
    if (b.width > 0 && out.width > 0 && covers(&out, &b)) {
        struct paper *p = calloc(1, sizeof *p);
        if (!p) {
            return;
        }
        p->tree = tree;
        p->destroy.notify = paper_destroyed;
        wl_signal_add(&tree->node.events.destroy, &p->destroy);
        p->next = G.papers;
        G.papers = p;
        return;
    }
    if (G.layers) {
        item_new(NULL, tree);
    }
}

static void on_layer_unmap(struct sfwc_host *host, struct wlr_scene_tree *tree)
{
    for (struct paper *p = G.papers; p; p = p->next) {
        if (p->tree == tree) {
            paper_free(p);
            return;
        }
    }
    for (struct item *it = G.items; it; it = it->next) {
        if (it->tree == tree) {
            item_free(it);
            return;
        }
    }
}

static void on_frame(struct sfwc_host *host, uint32_t now_ms)
{
    papers_refresh();
    items_update();
}

static void on_reconfigure(struct sfwc_host *host)
{
    settings_read();
    flush_papers_blurred();
    /* windows are found again; panels stay (they are announced only once, when they map) */
    for (struct item *it = G.items, *next; it; it = next) {
        next = it->next;
        if (it->t || !G.layers) {
            item_free(it);
        }
    }
    for (struct sfwc_toplevel *t = host->toplevel_next(host, NULL); t; t = host->toplevel_next(host, t)) {
        item_window(t);
    }
}

static bool on_init(struct sfwc_host *host)
{
    G.host = host;
    settings_read();
    for (struct sfwc_toplevel *t = host->toplevel_next(host, NULL); t; t = host->toplevel_next(host, t)) {
        item_window(t);
    }
    return true;
}

static void on_fini(struct sfwc_host *host)
{
    while (G.items) {
        item_free(G.items);
    }
    while (G.papers) {
        paper_free(G.papers);
    }
}

static const struct sfwc_plugin plugin = {
    .api_version = SFWC_PLUGIN_API_VERSION,
    .struct_size = sizeof(struct sfwc_plugin),
    .name = "blur",
    .wlroots_version = WLR_VERSION_STR,
    .init = on_init,
    .fini = on_fini,
    .reconfigure = on_reconfigure,
    .frame = on_frame,
    .toplevel_unmap = on_unmap,
    .toplevel_map = on_map,
    .layer_map = on_layer_map,
    .layer_unmap = on_layer_unmap,
};

SFWC_PLUGIN_EXPORT const struct sfwc_plugin *sfwc_plugin_entry(void)
{
    return &plugin;
}
