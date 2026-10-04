/*
 * pwg.h - Reader for PWG Raster (PWG 5102.4) documents held in memory.
 *
 * PWG Raster is the image format Windows' built-in IPP Class Driver sends to
 * IPP Everywhere printers.  This reader has no dependency on libcups.
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SP410_PWG_H
#define SP410_PWG_H

#include <stddef.h>
#include <stdint.h>

#define PWG_HEADER_SIZE 1796

/* Colour spaces (cupsColorSpace values used by PWG Raster). */
enum
{
  PWG_CS_W        = 0,     /* not in PWG 5102.4, accepted for CUPS rasters */
  PWG_CS_RGB      = 1,
  PWG_CS_K        = 3,     /* "black"  */
  PWG_CS_CMYK     = 6,
  PWG_CS_SW       = 18,    /* "sgray"  */
  PWG_CS_SRGB     = 19,    /* "srgb"   */
  PWG_CS_ADOBERGB = 20
};

typedef struct
{
  char      media_type[65];
  char      page_size_name[65];
  unsigned  hw_res[2];          /* dpi */
  unsigned  page_size_pt[2];    /* points */
  unsigned  num_copies;
  unsigned  width, height;      /* pixels */
  unsigned  bits_per_color, bits_per_pixel, bytes_per_line;
  unsigned  color_order, color_space, num_colors;
  unsigned  total_page_count;
} pwg_header_t;

typedef struct
{
  const uint8_t *data;
  size_t         len, pos;
  int            big_endian;
  int            compressed;    /* RaS2 = compressed, RaS3 = raw lines */
  /* per-page line state */
  const pwg_header_t *hdr;
  unsigned       lines_left;
  unsigned       repeat_left;
  uint8_t       *line;          /* last decoded line (for repeats) */
  size_t         line_cap;
  char           error[128];
} pwg_reader_t;

/* Returns 0 on success, -1 if the data is not a PWG/CUPS v2/v3 raster. */
int  pwg_open(pwg_reader_t *r, const uint8_t *data, size_t len);

/* Returns 1 and fills *h for the next page, 0 at end of document, -1 on error. */
int  pwg_next_page(pwg_reader_t *r, pwg_header_t *h);

/*
 * Decode the next line of the current page into `out` (h->bytes_per_line
 * bytes).  Returns 0 on success, -1 on corrupt or truncated data.
 */
int  pwg_read_line(pwg_reader_t *r, uint8_t *out);

void pwg_close(pwg_reader_t *r);

#endif /* SP410_PWG_H */
