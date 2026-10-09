#define _POSIX_C_SOURCE 200809L
/* Unit tests for animations/animcfg.c (no compositor needed). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "animcfg.h"

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

struct msg {
    int level, line;
    char text[200];
};
struct log {
    struct msg m[32];
    int n;
};

static void collect(int level, int line, const char *text, void *data)
{
    struct log *l = data;
    if (l->n < 32) {
        l->m[l->n].level = level;
        l->m[l->n].line = line;
        snprintf(l->m[l->n].text, sizeof l->m[l->n].text, "%s", text);
        l->n++;
    }
}

static int has_msg(const struct log *l, int level, int line, const char *needle)
{
    for (int i = 0; i < l->n; i++) {
        if (l->m[i].level == level && l->m[i].line == line && strstr(l->m[i].text, needle)) {
            return 1;
        }
    }
    return 0;
}

/* The lines of an [animations] section as the compositor hands them over: `key = value` with the
 * line number of the config file (here: of the text, whose first line is the section header). */
static int load(struct animcfg *c, const char *text, animcfg_log_fn log, void *data)
{
    char *copy = strdup(text), *save = NULL;
    int line = 0;
    for (char *l = strtok_r(copy, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        line++; /* the text has no empty lines */
        char *eq = strstr(l, " = ");
        if (l[0] == '[' || !eq) {
            continue;
        }
        *eq = 0;
        animcfg_apply(c, l, eq + 3, line, log, data);
    }
    free(copy);
    return 1;
}

static void test_animation_rules(void)
{
    struct animcfg c;
    struct log l = {0};
    struct anim_rule r;
    struct anim_curve cv;

    /* nothing configured: no rule, the legacy keys apply */
    animcfg_defaults(&c);
    CHECK(!animcfg_rule(&c, ANIMT_WINDOWS_IN, &r));
    CHECK(animcfg_find_curve(&c, "linear", &cv) && !animcfg_find_curve(&c, "myBezier", &cv));

    /* a Hyprland style block */
    const char *text =
        "[animations]\n"
        "bezier = myBezier, 0.05, 0.9, 0.1, 1.05\n"         /* 2 */
        "animation = windows, 1, 7, myBezier\n"             /* 3 */
        "animation = windowsOut, 1, 5, default, popin 80%\n" /* 4 */
        "animation = border, 1, 10, default\n"              /* 5 */
        "animation = fade, 0\n"                             /* 6 */
        "animation = workspaces, 1, 6, default, slidefade 20%\n" /* 7 */
        "animation = windowsIn, 1, 4, myBezier, slide left\n"    /* 8 */
        "animation = borderangle, 1, 8, default, loop\n";   /* 9 */
    CHECK(load(&c, text, collect, &l));
    CHECK(l.n == 1 && has_msg(&l, ANIMCFG_WARNING, 9, "not supported"));
    CHECK(animcfg_find_curve(&c, "myBezier", &cv) && cv.y1 == 1.05);
    /* `windows` set all three, windowsIn/windowsOut then replaced their own */
    CHECK(animcfg_rule(&c, ANIMT_WINDOWS_MOVE, &r) && r.on && r.speed == 7 && !strcmp(r.curve, "myBezier"));
    CHECK(animcfg_rule(&c, ANIMT_WINDOWS_OUT, &r) && r.speed == 5 && r.style == ANIM_STYLE_POPIN && r.percent == 80);
    CHECK(animcfg_rule(&c, ANIMT_WINDOWS_IN, &r) && r.speed == 4 && r.style == ANIM_STYLE_SLIDE && r.dir == ANIM_DIR_LEFT);
    CHECK(animcfg_rule(&c, ANIMT_BORDER, &r) && r.on && r.speed == 10);
    CHECK(animcfg_rule(&c, ANIMT_FADE_IN, &r) && !r.on && animcfg_rule(&c, ANIMT_FADE_OUT, &r) && !r.on);
    CHECK(animcfg_rule(&c, ANIMT_WORKSPACES, &r) && r.style == ANIM_STYLE_SLIDEFADE && r.percent == 20);
    CHECK(!animcfg_rule(&c, ANIMT_LAYERS_IN, &r));

    /* the global rule is the default for types without their own */
    l.n = 0;
    CHECK(load(&c, "[animations]\nanimation = global, 1, 3, linear\n", collect, &l));
    CHECK(l.n == 0);
    CHECK(animcfg_rule(&c, ANIMT_LAYERS_IN, &r) && r.speed == 3 && r.style == ANIM_STYLE_DEFAULT);
    CHECK(animcfg_rule(&c, ANIMT_BORDER, &r) && r.speed == 10); /* its own rule wins */
    

    /* a later bezier with the same name replaces the earlier one */
    animcfg_defaults(&c);
    CHECK(load(&c, "[animations]\nbezier = a, 0, 0, 1, 1\nbezier = a, 0.1, 0.2, 0.3, 0.4\n", NULL, NULL));
    CHECK(c.n_curves == 1 && animcfg_find_curve(&c, "a", &cv) && cv.y0 == 0.2);
    

    /* mistakes: each is reported on its line and changes nothing */
    animcfg_defaults(&c);
    memset(&l, 0, sizeof l);
    text = "[animations]\n"
           "bezier = x, 0, 0, 1\n"                      /* 2 too few values */
           "bezier = x, 1.5, 0, 0.5, 1\n"               /* 3 x out of range */
           "bezier = x, 0, 0, a, 1\n"                   /* 4 not a number */
           "bezier = bad name!, 0, 0, 1, 1\n"           /* 5 bad name */
           "animation = windows, 1, 4, nosuchcurve\n"   /* 6 unknown curve */
           "animation = windows, 1, fast, linear\n"     /* 7 bad speed */
           "animation = windows, 1, 0, linear\n"        /* 8 speed too small */
           "animation = windows, maybe, 4, linear\n"    /* 9 bad on/off */
           "animation = teleport, 1, 4, linear\n"       /* 10 unknown type */
           "animation = windows, 1, 4, linear, spin\n"  /* 11 unknown style */
           "animation = border, 1, 4, linear, popin\n"  /* 12 style not allowed */
           "animation = windowsIn, 1, 4, linear, popin 80\n" /* 13 percent needs % */
           "animation = windowsIn, 1, 4, linear, slide 50%\n" /* 14 slide takes a direction */
           "animation = workspaces, 1, 4, linear, popin\n"   /* 15 not for workspaces */
           "animation = global, 1, 4, linear, fade\n"        /* 16 global takes no style */
           "animation = windows, 1\n"                        /* 17 speed and curve missing */
           "preset = flashy\n";                              /* 18 */
    CHECK(load(&c, text, collect, &l));
    CHECK(c.n_curves == 0 && !animcfg_rule(&c, ANIMT_WINDOWS_IN, &r) && !c.anim_global.set);
    CHECK(has_msg(&l, ANIMCFG_ERROR, 2, "name, x0"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 3, "between 0 and 1"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 4, "not a number"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 5, "curve name"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 6, "unknown curve"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 7, "speed"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 8, "speed"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 9, "0/1"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 10, "unknown type"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 11, "bad style"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 12, "does not apply"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 13, "bad style"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 14, "bad style"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 15, "does not apply"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 16, "no style"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 17, "needs speed"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 18, "unknown preset"));
    

    /* `animation = windows, 1, 4, linear, popin 80%` is fine: the style only applies where it can */
    animcfg_defaults(&c);
    CHECK(load(&c, "[animations]\nanimation = windows, 1, 4, linear, popin 80%\n", collect, &(struct log){0}));
    CHECK(animcfg_rule(&c, ANIMT_WINDOWS_IN, &r) && r.style == ANIM_STYLE_POPIN && r.percent == 80);
    CHECK(animcfg_rule(&c, ANIMT_WINDOWS_MOVE, &r) && r.style == ANIM_STYLE_DEFAULT && r.percent == 0);
    

    /* presets */
    animcfg_defaults(&c);
    memset(&l, 0, sizeof l);
    CHECK(load(&c, "[animations]\npreset = hyprland\nanimation = border, 0\n", collect, &l));
    CHECK(l.n == 0);
    CHECK(animcfg_rule(&c, ANIMT_WINDOWS_IN, &r) && r.on && r.style == ANIM_STYLE_POPIN && r.percent == 87);
    CHECK(animcfg_rule(&c, ANIMT_WORKSPACES, &r) && r.style == ANIM_STYLE_SLIDE);
    CHECK(animcfg_rule(&c, ANIMT_BORDER, &r) && !r.on); /* a line after the preset wins */
    CHECK(animcfg_find_curve(&c, "easeOutQuint", &cv) && cv.x0 == 0.23);
    CHECK(c.anim_enabled);
    
    animcfg_defaults(&c);
    CHECK(load(&c, "[animations]\npreset = none\n", NULL, NULL));
    for (int i = 0; i < ANIMT_COUNT; i++) {
        CHECK(animcfg_rule(&c, i, &r) && !r.on);
    }
    
    animcfg_defaults(&c);
    CHECK(load(&c, "[animations]\npreset = minimal\n", NULL, NULL));
    CHECK(animcfg_rule(&c, ANIMT_WINDOWS_IN, &r) && r.percent == 95);
    

    /* Wayfire style effects, and which style takes which argument */
    animcfg_defaults(&c);
    CHECK(c.fire_particles == 400 && c.fire_size == 14 && c.fire_color == 0xff7a18);
    memset(&l, 0, sizeof l);
    text = "[animations]\n"
           "animation = windowsOut, 1, 6, ease, fire\n"            /* 2 */
           "animation = windowsIn, 1, 4, ease, squeeze\n"          /* 3 */
           "animation = windowsMove, 1, 3, ease\n"                 /* 4 */
           "animation = windowsOut, 1, 6, ease, fire 50%\n"        /* 5 fire takes no argument */
           "animation = windowsIn, 1, 6, ease, squeeze left\n"     /* 6 squeeze takes none either */
           "animation = windowsIn, 1, 6, ease, zoom 70%\n"         /* 7 ok */
           "animation = windowsIn, 1, 6, ease, slide 50%\n"        /* 8 a window slide has a direction */
           "animation = workspaces, 1, 6, ease, slide 50%\n"       /* 9 a workspace slide a percentage */
           "animation = workspaces, 1, 6, ease, slide left\n"      /* 10 not a direction */
           "animation = border, 1, 6, ease, fire\n"                /* 11 not for borders */
           "fire_particles = 900\nfire_size = 20\nfire_color = #3060ff\n"
           "fire_particles = 5\nfire_size = 500\nfire_color = red\n"; /* 15-17 out of range / bad */
    CHECK(load(&c, text, collect, &l));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 5, "bad style"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 6, "bad style"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 8, "left, right"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 10, "percentage"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 11, "does not apply"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 15, "between"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 16, "between"));
    CHECK(has_msg(&l, ANIMCFG_ERROR, 17, "#rrggbb"));
    CHECK(l.n == 8);
    CHECK(c.fire_particles == 900 && c.fire_size == 20 && c.fire_color == 0x3060ff); /* bad ones changed nothing */
    CHECK(animcfg_rule(&c, ANIMT_WINDOWS_OUT, &r) && r.style == ANIM_STYLE_FIRE && r.speed == 6);
    CHECK(animcfg_rule(&c, ANIMT_WORKSPACES, &r) && r.style == ANIM_STYLE_SLIDE && r.percent == 50);
    
    animcfg_defaults(&c);
    CHECK(load(&c, "[animations]\nanimation = windowsIn, 1, 6, ease, zoom 70%\nanimation = windowsOut, 1, 6, ease, squeeze\n", NULL, NULL));
    CHECK(animcfg_rule(&c, ANIMT_WINDOWS_IN, &r) && r.style == ANIM_STYLE_ZOOM && r.percent == 70);
    CHECK(animcfg_rule(&c, ANIMT_WINDOWS_OUT, &r) && r.style == ANIM_STYLE_SQUEEZE);
    

    /* the legacy keys still work next to the new ones */
    animcfg_defaults(&c);
    CHECK(load(&c, "[animations]\nopen = slide\nduration_ms = 400\nanimation = border, 1, 5, ease\n", NULL, NULL));
    CHECK(!strcmp(c.anim_open, "slide") && c.anim_duration_ms == 400 && c.anim[ANIMT_BORDER].on);
    
}


int main(void)
{
    test_animation_rules();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_animcfg: all checks passed\n");
    return 0;
}
