/*
 * render.h - Convert PWG raster pages into TSPL labels.
 *
 * Copyright 2026 The sp410-cups-driver and sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SP410_RENDER_H
#define SP410_RENDER_H

#include "pwg.h"
#include "settings.h"
#include "tspl.h"

typedef struct
{
  int    pages;
  int    labels;          /* pages x copies */
  size_t bytes;           /* TSPL bytes produced */
  char   error[160];
} render_stats_t;

/* Returns non-zero to abort (e.g. the job was cancelled). */
typedef int (*render_cancel_fn)(void *ctx);

/*
 * Convert every page of a PWG raster document to TSPL.
 *   copies   - IPP "copies" (used when the raster header does not set copies)
 * Returns 0 on success, -1 on error (stats->error explains), 1 if cancelled.
 * Nothing is written to `sink` for a page that fails to decode, so a
 * corrupt page never reaches the printer half-finished.
 */
int render_document(const uint8_t *data, size_t len, const sp410_settings_t *s,
                    int copies, const tspl_sink_t *sink,
                    render_cancel_fn cancel, void *cancel_ctx,
                    render_stats_t *stats);

#endif /* SP410_RENDER_H */
