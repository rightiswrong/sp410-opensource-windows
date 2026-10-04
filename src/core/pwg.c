/*
 * pwg.c - PWG Raster (PWG 5102.4) reader.
 *
 * Stream layout:
 *   "RaS2" sync word, then per page a 1796-byte header followed by
 *   compressed image data.  Header integers are big-endian 32-bit.
 *
 * Line compression (always used by PWG Raster):
 *   <line-repeat byte L>  the decoded line occurs L+1 times
 *   then until the line is full, a run of
 *     n = 0..127   : the next pixel is repeated n+1 times
 *     n = 129..255 : 257-n literal pixels follow
 *     n = 128      : fill the rest of the line with white
 *   where a "pixel" is ceil(bits_per_pixel / 8) bytes.
 *
 * "RaS3" (CUPS v3) carries the same header with uncompressed lines and is
 * accepted too; byte-swapped sync words select little-endian headers.
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pwg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Header field offsets (identical to the CUPS v2/v3 page header). */
#define OFF_MEDIA_TYPE        128
#define OFF_HW_RES            276
#define OFF_NUM_COPIES        340
#define OFF_PAGE_SIZE         352
#define OFF_WIDTH             372
#define OFF_HEIGHT            376
#define OFF_BITS_PER_COLOR    384
#define OFF_BITS_PER_PIXEL    388
#define OFF_BYTES_PER_LINE    392
#define OFF_COLOR_ORDER       396
#define OFF_COLOR_SPACE       400
#define OFF_NUM_COLORS        420
#define OFF_TOTAL_PAGE_COUNT  452
#define OFF_PAGE_SIZE_NAME    1732

/* Sanity limits: generous for a 108 mm x 300 mm label at up to 600 dpi. */
#define MAX_WIDTH             20000u
#define MAX_HEIGHT            100000u
#define MAX_BYTES_PER_LINE    (20000u * 8u)

static unsigned
get_u32(const pwg_reader_t *r, const uint8_t *p)
{
  if (r->big_endian)
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
  return ((unsigned)p[3] << 24) | ((unsigned)p[2] << 16) | ((unsigned)p[1] << 8) | p[0];
}

static void
get_str(char *dst, const uint8_t *src)
{
  memcpy(dst, src, 64);
  dst[64] = '\0';
}

int
pwg_open(pwg_reader_t *r, const uint8_t *data, size_t len)
{
  memset(r, 0, sizeof(*r));
  r->data = data;
  r->len  = len;

  if (len < 4)
  {
    snprintf(r->error, sizeof(r->error), "document too short (%zu bytes)", len);
    return -1;
  }
  if (!memcmp(data, "RaS2", 4))      { r->big_endian = 1; r->compressed = 1; }
  else if (!memcmp(data, "2SaR", 4)) { r->big_endian = 0; r->compressed = 1; }
  else if (!memcmp(data, "RaS3", 4)) { r->big_endian = 1; r->compressed = 0; }
  else if (!memcmp(data, "3SaR", 4)) { r->big_endian = 0; r->compressed = 0; }
  else
  {
    snprintf(r->error, sizeof(r->error), "not a PWG raster stream (bad sync word)");
    return -1;
  }
  r->pos = 4;
  return 0;
}

static int
white_value(unsigned color_space)
{
  switch (color_space)
  {
    case PWG_CS_K:
    case PWG_CS_CMYK:
      return 0x00;
    default:
      return 0xFF;
  }
}

int
pwg_next_page(pwg_reader_t *r, pwg_header_t *h)
{
  const uint8_t *p;

  /* Skip any lines the caller did not consume. */
  if (r->hdr && r->lines_left)
  {
    uint8_t *tmp = malloc(r->hdr->bytes_per_line ? r->hdr->bytes_per_line : 1);

    if (!tmp)
      return -1;
    while (r->lines_left)
      if (pwg_read_line(r, tmp))
      {
        free(tmp);
        return -1;
      }
    free(tmp);
  }

  if (r->pos == r->len)
    return 0;
  if (r->len - r->pos < PWG_HEADER_SIZE)
  {
    snprintf(r->error, sizeof(r->error), "truncated page header at offset %zu", r->pos);
    return -1;
  }

  p = r->data + r->pos;
  memset(h, 0, sizeof(*h));
  get_str(h->media_type, p + OFF_MEDIA_TYPE);
  get_str(h->page_size_name, p + OFF_PAGE_SIZE_NAME);
  h->hw_res[0]         = get_u32(r, p + OFF_HW_RES);
  h->hw_res[1]         = get_u32(r, p + OFF_HW_RES + 4);
  h->num_copies        = get_u32(r, p + OFF_NUM_COPIES);
  h->page_size_pt[0]   = get_u32(r, p + OFF_PAGE_SIZE);
  h->page_size_pt[1]   = get_u32(r, p + OFF_PAGE_SIZE + 4);
  h->width             = get_u32(r, p + OFF_WIDTH);
  h->height            = get_u32(r, p + OFF_HEIGHT);
  h->bits_per_color    = get_u32(r, p + OFF_BITS_PER_COLOR);
  h->bits_per_pixel    = get_u32(r, p + OFF_BITS_PER_PIXEL);
  h->bytes_per_line    = get_u32(r, p + OFF_BYTES_PER_LINE);
  h->color_order       = get_u32(r, p + OFF_COLOR_ORDER);
  h->color_space       = get_u32(r, p + OFF_COLOR_SPACE);
  h->num_colors        = get_u32(r, p + OFF_NUM_COLORS);
  h->total_page_count  = get_u32(r, p + OFF_TOTAL_PAGE_COUNT);
  r->pos += PWG_HEADER_SIZE;

  if (!h->width || !h->height || h->width > MAX_WIDTH || h->height > MAX_HEIGHT ||
      !h->bits_per_pixel || h->bits_per_pixel > 64 ||
      h->bytes_per_line != (h->width * h->bits_per_pixel + 7) / 8 ||
      h->bytes_per_line > MAX_BYTES_PER_LINE)
  {
    snprintf(r->error, sizeof(r->error),
             "invalid page geometry (%ux%u, %u bpp, %u bytes/line)",
             h->width, h->height, h->bits_per_pixel, h->bytes_per_line);
    return -1;
  }

  if (r->line_cap < h->bytes_per_line)
  {
    uint8_t *nl = realloc(r->line, h->bytes_per_line);

    if (!nl)
      return -1;
    r->line     = nl;
    r->line_cap = h->bytes_per_line;
  }

  r->hdr         = h;
  r->lines_left  = h->height;
  r->repeat_left = 0;
  return 1;
}

static int
decode_line(pwg_reader_t *r)
{
  const pwg_header_t *h = r->hdr;
  unsigned bpp = (h->bits_per_pixel + 7) / 8;
  size_t   bpl = h->bytes_per_line, out = 0;
  int      white = white_value(h->color_space);

  if (r->pos >= r->len)
    goto truncated;
  r->repeat_left = r->data[r->pos ++];          /* extra repetitions */

  while (out < bpl)
  {
    unsigned n;

    if (r->pos >= r->len)
      goto truncated;
    n = r->data[r->pos ++];

    if (n == 128)
    {
      memset(r->line + out, white, bpl - out);
      out = bpl;
    }
    else if (n < 128)
    {
      size_t count = (size_t)(n + 1) * bpp;

      if (r->len - r->pos < bpp)
        goto truncated;
      if (count > bpl - out)
        goto overflow;
      for (size_t i = 0; i < count; i += bpp)
        memcpy(r->line + out + i, r->data + r->pos, bpp);
      r->pos += bpp;
      out    += count;
    }
    else
    {
      size_t count = (size_t)(257 - n) * bpp;

      if (r->len - r->pos < count)
        goto truncated;
      if (count > bpl - out)
        goto overflow;
      memcpy(r->line + out, r->data + r->pos, count);
      r->pos += count;
      out    += count;
    }
  }
  return 0;

truncated:
  snprintf(r->error, sizeof(r->error), "truncated image data (%u lines missing)",
           r->lines_left);
  return -1;
overflow:
  snprintf(r->error, sizeof(r->error), "corrupt image data: run overflows line");
  return -1;
}

int
pwg_read_line(pwg_reader_t *r, uint8_t *out)
{
  const pwg_header_t *h = r->hdr;

  if (!h || !r->lines_left)
  {
    snprintf(r->error, sizeof(r->error), "read past end of page");
    return -1;
  }

  if (!r->compressed)
  {
    if (r->len - r->pos < h->bytes_per_line)
    {
      snprintf(r->error, sizeof(r->error), "truncated image data");
      return -1;
    }
    memcpy(out, r->data + r->pos, h->bytes_per_line);
    r->pos += h->bytes_per_line;
    r->lines_left --;
    return 0;
  }

  if (r->repeat_left)
    r->repeat_left --;
  else if (decode_line(r))
    return -1;

  memcpy(out, r->line, h->bytes_per_line);
  r->lines_left --;

  /* A repeat count may not run past the end of the page. */
  if (!r->lines_left && r->repeat_left)
  {
    snprintf(r->error, sizeof(r->error), "line repeat runs past end of page");
    return -1;
  }
  return 0;
}

void
pwg_close(pwg_reader_t *r)
{
  free(r->line);
  r->line     = NULL;
  r->line_cap = 0;
}
