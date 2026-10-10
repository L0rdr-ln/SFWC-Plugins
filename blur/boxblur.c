#include "boxblur.h"

#include <stdlib.h>
#include <string.h>

/* One box filter pass along rows (stride 4) or columns (stride row bytes); edge pixels repeat. */
static void box_line(const uint8_t *in, uint8_t *out, int n, size_t step, int radius)
{
    for (int c = 0; c < 4; c++) {
        long sum = 0;
        int win = 2 * radius + 1;
        for (int k = -radius; k <= radius; k++) {
            int i = k < 0 ? 0 : (k >= n ? n - 1 : k);
            sum += in[(size_t)i * step + c];
        }
        for (int i = 0; i < n; i++) {
            out[(size_t)i * step + c] = (uint8_t)((sum + win / 2) / win);
            int add = i + radius + 1, sub = i - radius;
            add = add >= n ? n - 1 : add;
            sub = sub < 0 ? 0 : sub;
            sum += in[(size_t)add * step + c] - in[(size_t)sub * step + c];
        }
    }
}

uint8_t *boxblur_shrink(const uint8_t *src, int w, int h, size_t stride, int shrink, int radius,
                        int passes, int *out_w, int *out_h)
{
    if (!src || w <= 0 || h <= 0 || shrink < 1 || radius < 0 || passes < 0) {
        return NULL;
    }
    int ow = (w + shrink - 1) / shrink, oh = (h + shrink - 1) / shrink;
    size_t row = (size_t)ow * 4;
    uint8_t *a = malloc(row * oh), *b = malloc(row * oh);
    uint8_t *line = malloc((size_t)(ow > oh ? ow : oh) * 4);
    if (!a || !b || !line) {
        free(a);
        free(b);
        free(line);
        return NULL;
    }
    /* shrink: the average of each block */
    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) {
            unsigned sum[4] = {0}, n = 0;
            for (int yy = y * shrink; yy < (y + 1) * shrink && yy < h; yy++) {
                for (int xx = x * shrink; xx < (x + 1) * shrink && xx < w; xx++) {
                    const uint8_t *p = src + (size_t)yy * stride + (size_t)xx * 4;
                    for (int c = 0; c < 4; c++) {
                        sum[c] += p[c];
                    }
                    n++;
                }
            }
            for (int c = 0; c < 4; c++) {
                a[y * row + (size_t)x * 4 + c] = (uint8_t)((sum[c] + n / 2) / n);
            }
        }
    }
    if (radius > 0) {
        uint8_t *col = malloc((size_t)oh * 4);
        if (!col) {
            free(a);
            free(b);
            free(line);
            return NULL;
        }
        for (int p = 0; p < passes; p++) {
            for (int y = 0; y < oh; y++) {
                box_line(a + y * row, b + y * row, ow, 4, radius);
            }
            for (int x = 0; x < ow; x++) { /* columns: gather, filter, scatter */
                for (int y = 0; y < oh; y++) {
                    memcpy(line + (size_t)y * 4, b + y * row + (size_t)x * 4, 4);
                }
                box_line(line, col, oh, 4, radius);
                for (int y = 0; y < oh; y++) {
                    memcpy(a + y * row + (size_t)x * 4, col + (size_t)y * 4, 4);
                }
            }
        }
        free(col);
    }
    free(b);
    free(line);
    *out_w = ow;
    *out_h = oh;
    return a;
}
