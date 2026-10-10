/* Software blur for the blur plugin: shrink the picture, blur it with box filters, done. */
#ifndef BOXBLUR_H
#define BOXBLUR_H

#include <stddef.h>
#include <stdint.h>

/* src: 32 bit pixels (any channel order, 4 bytes each), `stride` in bytes. The result has
 * ceil(w / shrink) x ceil(h / shrink) pixels, tightly packed (4 bytes each, same channel order),
 * blurred with `passes` box filters of `radius` (in pixels of the shrunken picture) on each axis.
 * Returns NULL if memory runs out or the arguments are nonsense. Free with free(). */
uint8_t *boxblur_shrink(const uint8_t *src, int w, int h, size_t stride, int shrink, int radius,
                        int passes, int *out_w, int *out_h);

#endif
