#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boxblur.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void)
{
    /* a uniform picture stays uniform, and the size follows the shrink factor */
    enum { W = 37, H = 21 };
    uint8_t img[W * H * 4];
    for (int i = 0; i < W * H; i++) {
        img[i * 4 + 0] = 200; img[i * 4 + 1] = 100; img[i * 4 + 2] = 50; img[i * 4 + 3] = 255;
    }
    int ow, oh;
    uint8_t *o = boxblur_shrink(img, W, H, W * 4, 4, 3, 3, &ow, &oh);
    CHECK(o && ow == 10 && oh == 6);
    for (int i = 0; i < ow * oh; i++) {
        CHECK(o[i * 4] == 200 && o[i * 4 + 1] == 100 && o[i * 4 + 2] == 50 && o[i * 4 + 3] == 255);
    }
    free(o);

    /* a hard edge (black left, white right) becomes a monotonic ramp that is grey in the middle */
    enum { W2 = 64, H2 = 8 };
    static uint8_t edge[W2 * H2 * 4];
    for (int y = 0; y < H2; y++) {
        for (int x = 0; x < W2; x++) {
            memset(edge + (y * W2 + x) * 4, x < W2 / 2 ? 0 : 255, 4);
        }
    }
    o = boxblur_shrink(edge, W2, H2, W2 * 4, 2, 3, 3, &ow, &oh);
    CHECK(o && ow == 32 && oh == 4);
    int prev = -1;
    for (int x = 0; x < ow; x++) {
        int v = o[x * 4];
        CHECK(v >= prev);
        prev = v;
    }
    CHECK(o[0] < 5 && o[(ow - 1) * 4] > 250);
    int mid = o[(ow / 2) * 4];
    CHECK(mid > 60 && mid < 200);
    CHECK(o[(ow / 2 - 1) * 4] > 5 && o[(ow / 2 + 1) * 4] < 250); /* smooth, not a step */
    free(o);

    /* no blur radius: only the shrink */
    o = boxblur_shrink(edge, W2, H2, W2 * 4, 2, 0, 3, &ow, &oh);
    CHECK(o && o[(ow / 2 - 1) * 4] == 0 && o[(ow / 2) * 4] == 255);
    free(o);

    /* nonsense */
    CHECK(!boxblur_shrink(NULL, 4, 4, 16, 1, 1, 1, &ow, &oh));
    CHECK(!boxblur_shrink(img, 0, 4, 16, 1, 1, 1, &ow, &oh));
    CHECK(!boxblur_shrink(img, 4, 4, 16, 0, 1, 1, &ow, &oh));
    printf("test_boxblur: all checks passed\n");
    return 0;
}
