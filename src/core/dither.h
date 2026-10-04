/*
 * dither.h - 8-bit "ink" plane to 1-bit thermal dots.
 *
 * Ink convention used throughout the filter: 0 = no ink (white paper),
 * 255 = full ink (black dot).  Packed output is MSB-first, 1 = black dot.
 *
 * Copyright 2026 The sp410-cups-driver and sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SP410_DITHER_H
#define SP410_DITHER_H

#include <stddef.h>
#include <stdint.h>

#include "settings.h"

/*
 * Returns non-zero when the page contains continuous-tone areas (photos,
 * gray fills) that need halftoning.  Anti-aliased edges of text and barcodes
 * are one pixel wide and are deliberately NOT counted, so ordinary shipping
 * labels stay on crisp thresholding.
 */
int  dither_page_needs_halftone(const uint8_t *ink, int width, int height);

/*
 * Convert the ink plane (width x height, stride == width) into a packed
 * 1-bpp bitmap.  `out` must hold out_stride * height bytes and is fully
 * overwritten.  Returns 0 on success, -1 on allocation failure.
 */
int  dither_page(dither_mode_t mode, int threshold, const uint8_t *ink,
                 int width, int height, uint8_t *out, size_t out_stride);

#endif /* SP410_DITHER_H */
