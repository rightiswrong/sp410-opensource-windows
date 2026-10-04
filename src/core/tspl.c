/*
 * tspl.c - TSPL job writer.
 *
 * Wire format notes (see docs/PROTOCOL.md for the full write-up):
 *   - Commands are ASCII, terminated by CR LF.
 *   - BITMAP x,y,width_bytes,height,mode,<raw bytes>
 *       raw bytes are MSB-first rows; a 1 bit leaves the dot WHITE and a
 *       0 bit burns it BLACK (the opposite of most raster formats).
 *   - Numbers are always formatted in the "C" locale.  This filter never
 *     calls setlocale(), so a German/French system locale cannot turn
 *     "101.6 mm" into "101,6 mm" (which the printer would misparse as two
 *     arguments).
 *
 * Copyright 2026 The sp410-cups-driver and sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tspl.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  const tspl_sink_t *sink;
  size_t             bytes;
  int                err;
} writer_t;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
static void
w_printf(writer_t *w, const char *fmt, ...)
{
  va_list ap;
  char    line[256];
  int     n;

  if (w->err)
    return;
  va_start(ap, fmt);
  n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= sizeof(line) ||
      w->sink->write(w->sink->ctx, line, (size_t)n))
    w->err = 1;
  else
    w->bytes += (size_t)n;
}

static void
w_write(writer_t *w, const void *data, size_t len)
{
  if (w->err || !len)
    return;
  if (w->sink->write(w->sink->ctx, data, len))
    w->err = 1;
  else
    w->bytes += len;
}

void
tspl_fmt_mm(char *buf, size_t bufsize, double mm)
{
  double r = round(mm * 10.0) / 10.0;

  if (fabs(r - round(r)) < 1e-9)
    snprintf(buf, bufsize, "%ld", (long)round(r));
  else
    snprintf(buf, bufsize, "%.1f", r);
}

/* ---- band detection --------------------------------------------------- */

typedef struct
{
  int y0, y1;     /* inclusive rows  */
  int b0, b1;     /* inclusive bytes */
} band_t;

static int
row_extent(const uint8_t *row, int wbytes, int *first, int *last)
{
  int f = -1, l = -1, i;

  for (i = 0; i < wbytes; i ++)
    if (row[i])
    {
      if (f < 0)
        f = i;
      l = i;
    }
  *first = f;
  *last  = l;
  return f >= 0;
}

/*
 * Split the image into bands of non-blank rows.  Blank runs shorter than
 * `merge` rows are kept inside a band (cheaper than another BITMAP header).
 * Returns the number of bands written to `out` (allocated by caller with
 * room for hdots entries).
 */
static int
find_bands(const uint8_t *bits, int wbytes, int hdots, size_t stride,
           int merge, band_t *out)
{
  int    n = 0, y, f, l;
  band_t cur;
  int    open = 0, last_ink = -1;

  for (y = 0; y < hdots; y ++)
  {
    if (!row_extent(bits + (size_t)y * stride, wbytes, &f, &l))
      continue;

    if (open && y - last_ink - 1 >= merge)
    {
      cur.y1 = last_ink;
      out[n ++] = cur;
      open = 0;
    }

    if (!open)
    {
      cur.y0 = y;
      cur.b0 = f;
      cur.b1 = l;
      open   = 1;
    }
    else
    {
      if (f < cur.b0) cur.b0 = f;
      if (l > cur.b1) cur.b1 = l;
    }
    last_ink = y;
  }

  if (open)
  {
    cur.y1 = last_ink;
    out[n ++] = cur;
  }
  return n;
}

/* ---- page writer ------------------------------------------------------ */

int
tspl_write_page(const tspl_sink_t *sink, const sp410_settings_t *s,
                double width_mm, double height_mm,
                const uint8_t *bits, int wdots, int hdots, size_t stride,
                int copies, tspl_page_stats_t *stats)
{
  writer_t  w = { sink, 0, 0 };
  char      a[32], b[32];
  int       wbytes = (wdots + 7) / 8;
  band_t   *bands = NULL;
  int       nbands = 0, i, merge;
  uint8_t  *rowbuf = NULL;
  size_t    payload = 0;

  if (copies < 1)
    copies = 1;

  /* --- label setup --- */
  tspl_fmt_mm(a, sizeof(a), width_mm);
  tspl_fmt_mm(b, sizeof(b), height_mm);
  w_printf(&w, "SIZE %s mm,%s mm\r\n", a, b);

  tspl_fmt_mm(a, sizeof(a), s->gap_mm);
  tspl_fmt_mm(b, sizeof(b), s->gap_offset_mm);
  switch (s->media)
  {
    case MEDIA_GAP:
      w_printf(&w, "GAP %s mm,%s mm\r\n", a, b);
      break;
    case MEDIA_BLACKMARK:
      w_printf(&w, "BLINE %s mm,%s mm\r\n", a, b);
      break;
    case MEDIA_CONTINUOUS:
      w_printf(&w, "GAP 0,0\r\n");
      break;
  }

  w_printf(&w, "DIRECTION %d\r\n", s->direction ? 1 : 0);
  w_printf(&w, "REFERENCE 0,0\r\n");
  if (s->tear_offset_mm != 0.0)
  {
    tspl_fmt_mm(a, sizeof(a), s->tear_offset_mm);
    w_printf(&w, "OFFSET %s mm\r\n", a);
  }
  if (s->speed >= 0)
    w_printf(&w, "SPEED %d\r\n", s->speed);
  if (s->darkness >= 0)
    w_printf(&w, "DENSITY %d\r\n", s->darkness);
  w_printf(&w, "SET TEAR %s\r\n", s->tear ? "ON" : "OFF");
  w_printf(&w, "CLS\r\n");

  /* --- image bands --- */
  if (wdots > 0 && hdots > 0 && bits)
  {
    if ((bands = calloc((size_t)hdots, sizeof(band_t))) == NULL ||
        (rowbuf = malloc((size_t)wbytes)) == NULL)
    {
      free(bands);
      return -1;
    }

    /* Keep the BITMAP count bounded on pathological (striped) pages. */
    for (merge = s->band_merge_rows > 0 ? s->band_merge_rows : 1; ; merge *= 2)
    {
      nbands = find_bands(bits, wbytes, hdots, stride, merge, bands);
      if (nbands <= 64 || merge >= hdots)
        break;
    }

    for (i = 0; i < nbands && !w.err; i ++)
    {
      const band_t *bd = bands + i;
      int           bw = bd->b1 - bd->b0 + 1;
      int           bh = bd->y1 - bd->y0 + 1;
      int           y, k;

      w_printf(&w, "BITMAP %d,%d,%d,%d,0,", bd->b0 * 8, bd->y0, bw, bh);
      for (y = bd->y0; y <= bd->y1 && !w.err; y ++)
      {
        const uint8_t *src = bits + (size_t)y * stride + bd->b0;

        for (k = 0; k < bw; k ++)
          rowbuf[k] = (uint8_t)~src[k];          /* TSPL: 0 = black */

        /* Bits past the right edge of the image must stay white (1). */
        if (bd->b0 + bw == wbytes && (wdots & 7))
          rowbuf[bw - 1] |= (uint8_t)(0xFF >> (wdots & 7));

        w_write(&w, rowbuf, (size_t)bw);
      }
      w_printf(&w, "\r\n");
      payload += (size_t)bw * (size_t)bh;
    }

    free(rowbuf);
    free(bands);
  }

  w_printf(&w, "PRINT 1,%d\r\n", copies);


  if (stats)
  {
    stats->bands        = nbands;
    stats->bitmap_bytes = payload;
    stats->total_bytes  = w.bytes;
  }

  return w.err ? -1 : 0;
}
