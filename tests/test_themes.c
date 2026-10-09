#define _DEFAULT_SOURCE
/*
 * Checks every .theme file in themes/ of the theme pack (uses the compositor's theme parser):
 *  - loads without a single warning or error and says "format = 1"
 *  - has a name that no other theme has, and a lower-case file name
 *  - is readable: title text 4.5:1 against both titlebars (WCAG AA), the focused border is
 *    told apart from the unfocused one (1.5:1), the close/maximize/minimize buttons stand out
 *    against both titlebars (1.8:1) and are different colors
 *  - is installed by meson.build, has a preview image and is listed in the README
 */
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "theme.h"

static int failures;
static int checked;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                   \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fprintf(stderr, "\n");                                                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

static double channel(float c)
{
    return c <= 0.03928f ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double luminance(struct color c)
{
    return 0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b);
}

static double contrast(struct color a, struct color b)
{
    double la = luminance(a), lb = luminance(b);
    if (la < lb) {
        double t = la;
        la = lb;
        lb = t;
    }
    return (la + 0.05) / (lb + 0.05);
}

static int same_color(struct color a, struct color b)
{
    return fabsf(a.r - b.r) < 0.01f && fabsf(a.g - b.g) < 0.01f && fabsf(a.b - b.b) < 0.01f;
}

struct log {
    int n;
    char first[160];
};

static void collect(int level, int line, const char *text, void *data)
{
    (void)level;
    struct log *l = data;
    if (l->n++ == 0) {
        snprintf(l->first, sizeof l->first, "line %d: %s", line, text);
    }
}

static void check_theme(const char *stem, char names[][64], int *n_names, const char *meson,
                        const char *readme)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/themes/%s.theme", SRC_ROOT, stem);

    for (const char *p = stem; *p; p++) {
        CHECK(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '-',
              "%s: file names are lower-case letters, digits and '-'", stem);
    }

    char *text = slurp(path);
    CHECK(text && strstr(text, "\nformat = 1\n"), "%s: missing 'format = 1'", stem);
    free(text);

    struct theme t;
    struct log l = {0};
    theme_init_default(&t);
    CHECK(theme_load_file(&t, path, collect, &l), "%s: cannot read the file", stem);
    CHECK(l.n == 0, "%s: %d message(s) while loading, first: %s", stem, l.n, l.first);

    for (int i = 0; i < *n_names; i++) {
        CHECK(strcmp(names[i], t.name) != 0, "%s: the name '%s' is used by another theme", stem, t.name);
    }
    snprintf(names[(*n_names)++], 64, "%s", t.name);

    double tf = contrast(t.title_text, t.titlebar_focused);
    double tu = contrast(t.title_text, t.titlebar_unfocused);
    CHECK(tf >= 4.5 && tu >= 4.5, "%s: title text contrast %.1f / %.1f is below 4.5", stem, tf, tu);
    double b = contrast(t.border_focused, t.border_unfocused);
    CHECK(b >= 1.5, "%s: focused and unfocused border look alike (%.1f:1)", stem, b);
    const struct color btn[3] = {t.close_button, t.maximize_button, t.minimize_button};
    for (int i = 0; i < 3; i++) {
        double cf = contrast(btn[i], t.titlebar_focused), cu = contrast(btn[i], t.titlebar_unfocused);
        CHECK(cf >= 1.8 && cu >= 1.8, "%s: button %d is hard to see (%.1f / %.1f)", stem, i, cf, cu);
        for (int j = i + 1; j < 3; j++) {
            CHECK(!same_color(btn[i], btn[j]), "%s: buttons %d and %d have the same color", stem, i, j);
        }
    }
    CHECK(t.titlebar_height >= t.button_size + 6, "%s: the buttons do not fit in the titlebar", stem);

    char needle[128];
    snprintf(needle, sizeof needle, "%s.theme", stem);
    CHECK(strstr(meson, needle), "%s: not installed by meson.build", stem);
    snprintf(needle, sizeof needle, "themes/previews/%s.svg", stem);
    CHECK(strstr(readme, needle), "%s: no preview in the README gallery", stem);
    snprintf(path, sizeof path, "%s/themes/previews/%s.svg", SRC_ROOT, stem);
    FILE *f = fopen(path, "rb");
    CHECK(f, "%s: %s does not exist (run tools/theme-preview.py)", stem, path);
    if (f) {
        fclose(f);
    }
    theme_finish(&t);
    checked++;
}

int main(void)
{
    char dir[1024], path[1024];
    snprintf(dir, sizeof dir, "%s/themes", SRC_ROOT);
    snprintf(path, sizeof path, "%s/meson.build", SRC_ROOT);
    char *meson = slurp(path);
    snprintf(path, sizeof path, "%s/README.md", SRC_ROOT);
    char *readme = slurp(path);
    if (!meson || !readme) {
        fprintf(stderr, "FAIL: cannot read meson.build or README.md\n");
        return 1;
    }
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "FAIL: cannot open %s\n", dir);
        return 1;
    }
    static char names[64][64];
    int n_names = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len > 6 && !strcmp(e->d_name + len - 6, ".theme") && n_names < 64) {
            char stem[128];
            snprintf(stem, sizeof stem, "%.*s", (int)(len - 6), e->d_name);
            check_theme(stem, names, &n_names, meson, readme);
        }
    }
    closedir(d);
    free(meson);
    free(readme);
    CHECK(checked >= 8, "only %d themes found", checked);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("shipped themes: %d themes OK\n", checked);
    return 0;
}
