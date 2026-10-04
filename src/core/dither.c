/*
 * dither.c - Thresholding, ordered and error-diffusion halftoning.
 *
 * Copyright 2026 The sp410-cups-driver and sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "dither.h"

#include <stdlib.h>
#include <string.h>

#define MID_LO  48
#define MID_HI  207

static inline int
is_mid(uint8_t v)
{
  return v >= MID_LO && v <= MID_HI;
}

int
dither_page_needs_halftone(const uint8_t *ink, int width, int height)
{
  long long flat = 0, limit;
  int       x, y;

  if (width < 2 || height < 2)
    return 0;

  /* 0.5% of the page area in flat mid-tones => treat as continuous tone. */
  limit = ((long long)width * height) / 200;
  if (limit < 16)
    limit = 16;

  for (y = 0; y < height - 1; y ++)
  {
    const uint8_t *row  = ink + (size_t)y * width;
    const uint8_t *next = row + width;

    for (x = 0; x < width - 1; x ++)
      if (is_mid(row[x]) && is_mid(row[x + 1]) && is_mid(next[x]))
        if (++flat > limit)
          return 1;
  }
  return 0;
}

static inline void
set_dot(uint8_t *row, int x)
{
  row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
}

static void
do_threshold(int threshold, const uint8_t *ink, int w, int h,
             uint8_t *out, size_t stride)
{
  int x, y;

  for (y = 0; y < h; y ++)
  {
    const uint8_t *src = ink + (size_t)y * w;
    uint8_t       *dst = out + (size_t)y * stride;

    for (x = 0; x < w; x ++)
      if (src[x] >= threshold)
        set_dot(dst, x);
  }
}

static const uint8_t bayer8[8][8] =
{
  {  0, 32,  8, 40,  2, 34, 10, 42 },
  { 48, 16, 56, 24, 50, 18, 58, 26 },
  { 12, 44,  4, 36, 14, 46,  6, 38 },
  { 60, 28, 52, 20, 62, 30, 54, 22 },
  {  3, 35, 11, 43,  1, 33,  9, 41 },
  { 51, 19, 59, 27, 49, 17, 57, 25 },
  { 15, 47,  7, 39, 13, 45,  5, 37 },
  { 63, 31, 55, 23, 61, 29, 53, 21 }
};

static void
do_ordered(const uint8_t *ink, int w, int h, uint8_t *out, size_t stride)
{
  int x, y;

  for (y = 0; y < h; y ++)
  {
    const uint8_t *src = ink + (size_t)y * w;
    uint8_t       *dst = out + (size_t)y * stride;

    for (x = 0; x < w; x ++)
      if ((int)src[x] * 64 > bayer8[y & 7][x & 7] * 256 + 128)
        set_dot(dst, x);
  }
}

/*
 * Floyd-Steinberg, serpentine scan.  Error rows are padded by one cell on
 * each side so neighbours never need bounds checks.
 */
static int
do_floyd(int threshold, const uint8_t *ink, int w, int h,
         uint8_t *out, size_t stride)
{
  int *cur, *nxt, *tmp;
  int  x, y;

  cur = calloc((size_t)w + 2, sizeof(int));
  nxt = calloc((size_t)w + 2, sizeof(int));
  if (!cur || !nxt)
  {
    free(cur);
    free(nxt);
    return -1;
  }

  for (y = 0; y < h; y ++)
  {
    const uint8_t *src = ink + (size_t)y * w;
    uint8_t       *dst = out + (size_t)y * stride;
    int            ltr = !(y & 1);

    memset(nxt, 0, ((size_t)w + 2) * sizeof(int));

    for (int i = 0; i < w; i ++)
    {
      int v, err, d;

      x   = ltr ? i : w - 1 - i;
      d   = ltr ? 1 : -1;
      v   = src[x] + cur[x + 1] / 16;
      if (v >= threshold)
      {
        set_dot(dst, x);
        err = v - 255;
      }
      else
        err = v;

      cur[x + 1 + d] += err * 7;
      nxt[x + 1 - d] += err * 3;
      nxt[x + 1]     += err * 5;
      nxt[x + 1 + d] += err * 1;
    }

    tmp = cur; cur = nxt; nxt = tmp;
  }

  free(cur);
  free(nxt);
  return 0;
}

/*
 * Atkinson: diffuses only 6/8 of the error, which keeps highlights clean and
 * blacks solid - a good match for direct-thermal media.
 */
static int
do_atkinson(int threshold, const uint8_t *ink, int w, int h,
            uint8_t *out, size_t stride)
{
  int  *rows[3];
  int  *buf;
  int   x, y;
  size_t rw = (size_t)w + 4;          /* 2 cells of padding on each side */

  if ((buf = calloc(rw * 3, sizeof(int))) == NULL)
    return -1;
  rows[0] = buf;
  rows[1] = buf + rw;
  rows[2] = buf + 2 * rw;

  for (y = 0; y < h; y ++)
  {
    const uint8_t *src = ink + (size_t)y * w;
    uint8_t       *dst = out + (size_t)y * stride;
    int           *r0 = rows[0] + 2, *r1 = rows[1] + 2, *r2 = rows[2] + 2;

    for (x = 0; x < w; x ++)
    {
      int v = src[x] + r0[x] / 8, err;

      if (v >= threshold)
      {
        set_dot(dst, x);
        err = v - 255;
      }
      else
        err = v;

      r0[x + 1] += err;
      r0[x + 2] += err;
      r1[x - 1] += err;
      r1[x]     += err;
      r1[x + 1] += err;
      r2[x]     += err;
    }

    /* rotate rows: 1 -> 0, 2 -> 1, cleared 0 -> 2 */
    {
      int *t = rows[0];
      rows[0] = rows[1];
      rows[1] = rows[2];
      rows[2] = t;
      memset(rows[2], 0, rw * sizeof(int));
    }
  }

  free(buf);
  return 0;
}

int
dither_page(dither_mode_t mode, int threshold, const uint8_t *ink,
            int width, int height, uint8_t *out, size_t out_stride)
{
  memset(out, 0, out_stride * (size_t)height);

  if (width <= 0 || height <= 0)
    return 0;

  if (mode == DITHER_AUTO)
    mode = dither_page_needs_halftone(ink, width, height) ? DITHER_ATKINSON
                                                          : DITHER_THRESHOLD;

  switch (mode)
  {
    case DITHER_ORDERED:
      do_ordered(ink, width, height, out, out_stride);
      return 0;
    case DITHER_FLOYD:
      return do_floyd(threshold, ink, width, height, out, out_stride);
    case DITHER_ATKINSON:
      return do_atkinson(threshold, ink, width, height, out, out_stride);
    case DITHER_THRESHOLD:
    case DITHER_AUTO:
    default:
      do_threshold(threshold, ink, width, height, out, out_stride);
      return 0;
  }
}
