/*
 * tspl.h - Minimal TSPL/TSPL2 job writer for 203-dpi label printers.
 *
 * Copyright 2026 The sp410-cups-driver and sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SP410_TSPL_H
#define SP410_TSPL_H

#include <stddef.h>
#include <stdint.h>

#include "settings.h"

/* Output sink: write() returns 0 on success, -1 on failure. */
typedef struct
{
  int  (*write)(void *ctx, const void *data, size_t len);
  void  *ctx;
} tspl_sink_t;

typedef struct
{
  int    bands;          /* BITMAP commands emitted               */
  size_t bitmap_bytes;   /* raster payload bytes (excl. headers)  */
  size_t total_bytes;    /* everything written for this page      */
} tspl_page_stats_t;

/*
 * Emit one complete label:
 *   setup (SIZE/GAP/DIRECTION/...), CLS, one BITMAP per non-blank band, PRINT.
 *
 * `bits` is a packed 1-bpp image (MSB first, 1 = black) of wdots x hdots with
 * `stride` bytes per row, already positioned in label coordinates.
 * Returns 0 on success, -1 on write error.
 */
int tspl_write_page(const tspl_sink_t *sink, const sp410_settings_t *s,
                    double width_mm, double height_mm,
                    const uint8_t *bits, int wdots, int hdots, size_t stride,
                    int copies, tspl_page_stats_t *stats);

/* Format a millimetre value the way TSPL expects ("2", "101.6"). */
void tspl_fmt_mm(char *buf, size_t bufsize, double mm);

#endif /* SP410_TSPL_H */
