/*
 * render.c - PWG raster page -> 1-bit label -> TSPL.
 *
 * This is the image pipeline of the Linux filter (sp410-rastertotspl),
 * re-hosted on the libcups-free PWG reader:
 *   unpack (1/2/4/8/16-bit K, sGray, sRGB/AdobeRGB, CMYK) -> 8-bit ink plane
 *   -> resample to 203 dpi if needed -> halftone -> clip to the 864-dot head
 *   -> apply ShiftX/ShiftY -> banded TSPL BITMAP output.
 *
 * Copyright 2026 The sp410-cups-driver and sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "render.h"
#include "dither.h"
#include "log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- pixel unpacking --------------------------------------------------- */

static inline unsigned
sample1(const uint8_t *line, unsigned bits, unsigned x)
{
  switch (bits)
  {
    case 1:  return ((line[x >> 3] >> (7 - (x & 7))) & 1) ? 255 : 0;
    case 2:  return ((line[x >> 2] >> (6 - 2 * (x & 3))) & 3) * 85;
    case 4:  return ((line[x >> 1] >> ((x & 1) ? 0 : 4)) & 15) * 17;
    case 8:  return line[x];
    case 16: return line[2 * (size_t)x];          /* big-endian: high byte first */
  }
  return 0;
}

static inline unsigned
channel(const uint8_t *line, unsigned bits, unsigned nch, unsigned x, unsigned c)
{
  if (bits == 16)
    return line[2 * ((size_t)x * nch + c)];
  return line[(size_t)x * nch + c];
}

typedef enum { SRC_INK, SRC_LUMA, SRC_RGB, SRC_CMYK } src_kind_t;

static int
classify(const pwg_header_t *h, src_kind_t *kind, unsigned *nch)
{
  unsigned bits = h->bits_per_color;

  switch (h->color_space)
  {
    case PWG_CS_K:        *kind = SRC_INK;  *nch = 1; break;
    case PWG_CS_W:
    case PWG_CS_SW:       *kind = SRC_LUMA; *nch = 1; break;
    case PWG_CS_RGB:
    case PWG_CS_SRGB:
    case PWG_CS_ADOBERGB: *kind = SRC_RGB;  *nch = 3; break;
    case PWG_CS_CMYK:     *kind = SRC_CMYK; *nch = 4; break;
    default: return -1;
  }
  if (h->bits_per_pixel != bits * *nch)
    return -1;
  if (*nch == 1)
    return (bits == 1 || bits == 2 || bits == 4 || bits == 8 || bits == 16) ? 0 : -1;
  if (h->color_order != 0 || (bits != 8 && bits != 16))
    return -1;
  return 0;
}

static void
line_to_ink(const uint8_t *line, unsigned width, unsigned bits,
            src_kind_t kind, unsigned nch, uint8_t *ink)
{
  unsigned x;

  switch (kind)
  {
    case SRC_INK:
      for (x = 0; x < width; x ++)
        ink[x] = (uint8_t)sample1(line, bits, x);
      break;
    case SRC_LUMA:
      for (x = 0; x < width; x ++)
        ink[x] = (uint8_t)(255 - sample1(line, bits, x));
      break;
    case SRC_RGB:
      for (x = 0; x < width; x ++)
      {
        unsigned r = channel(line, bits, nch, x, 0);
        unsigned g = channel(line, bits, nch, x, 1);
        unsigned b = channel(line, bits, nch, x, 2);
        unsigned y = (299 * r + 587 * g + 114 * b + 500) / 1000;
        ink[x] = (uint8_t)(255 - y);
      }
      break;
    case SRC_CMYK:
      for (x = 0; x < width; x ++)
      {
        unsigned c = channel(line, bits, nch, x, 0);
        unsigned m = channel(line, bits, nch, x, 1);
        unsigned y = channel(line, bits, nch, x, 2);
        unsigned k = channel(line, bits, nch, x, 3);
        unsigned v = (300 * c + 590 * m + 110 * y + 500) / 1000 + k;
        ink[x] = (uint8_t)(v > 255 ? 255 : v);
      }
      break;
  }
}

/* Box-filter resample of an ink plane (only for rasters not at 203 dpi). */
static uint8_t *
resample(const uint8_t *src, int sw, int sh, int dw, int dh)
{
  uint8_t *dst;
  double   fx = (double)sw / dw, fy = (double)sh / dh;

  if ((dst = malloc((size_t)dw * (size_t)dh)) == NULL)
    return NULL;

  for (int y = 0; y < dh; y ++)
  {
    int y0 = (int)floor(y * fy), y1 = (int)ceil((y + 1) * fy);

    if (y1 <= y0) y1 = y0 + 1;
    if (y1 > sh)  y1 = sh;
    for (int x = 0; x < dw; x ++)
    {
      int      x0 = (int)floor(x * fx), x1 = (int)ceil((x + 1) * fx);
      unsigned sum = 0, n = 0;

      if (x1 <= x0) x1 = x0 + 1;
      if (x1 > sw)  x1 = sw;
      for (int yy = y0; yy < y1; yy ++)
        for (int xx = x0; xx < x1; xx ++)
        {
          sum += src[(size_t)yy * sw + xx];
          n ++;
        }
      dst[(size_t)y * dw + x] = (uint8_t)(n ? (sum + n / 2) / n : 0);
    }
  }
  return dst;
}

/* ---- buffered sink: one page is converted fully before it is emitted ---- */

typedef struct
{
  uint8_t *data;
  size_t   len, cap;
} membuf_t;

static int
membuf_write(void *ctx, const void *data, size_t len)
{
  membuf_t *m = ctx;

  if (m->len + len > m->cap)
  {
    size_t   ncap = m->cap ? m->cap * 2 : 65536;
    uint8_t *nd;

    while (ncap < m->len + len)
      ncap *= 2;
    if ((nd = realloc(m->data, ncap)) == NULL)
      return -1;
    m->data = nd;
    m->cap  = ncap;
  }
  memcpy(m->data + m->len, data, len);
  m->len += len;
  return 0;
}

/* ---- one page ----------------------------------------------------------- */

static int
render_page(pwg_reader_t *r, const pwg_header_t *h, const sp410_settings_t *s,
            int copies, const tspl_sink_t *out, int page, render_stats_t *st,
            render_cancel_fn cancel, void *cancel_ctx)
{
  src_kind_t        kind;
  unsigned          nch;
  unsigned          sw = h->width, sh = h->height;
  unsigned          xdpi = h->hw_res[0] ? h->hw_res[0] : SP410_DPI;
  unsigned          ydpi = h->hw_res[1] ? h->hw_res[1] : SP410_DPI;
  uint8_t          *line = NULL, *ink = NULL, *bits = NULL, *label = NULL;
  int               w, hgt, lw, lh, rc = -1;
  size_t            stride, lstride;
  double            wmm, hmm;
  membuf_t          mb = { NULL, 0, 0 };
  tspl_sink_t       msink = { membuf_write, &mb };
  tspl_page_stats_t ps;

  if (classify(h, &kind, &nch))
  {
    snprintf(st->error, sizeof(st->error),
             "page %d: unsupported raster format (color space %u, %u bits, order %u)",
             page, h->color_space, h->bits_per_color, h->color_order);
    return -1;
  }

  wmm = h->page_size_pt[0] ? h->page_size_pt[0] * 25.4 / 72.0 : sw * 25.4 / xdpi;
  hmm = h->page_size_pt[1] ? h->page_size_pt[1] * 25.4 / 72.0 : sh * 25.4 / ydpi;

  log_msg(LOG_DEBUG, "page %d: %ux%u px @ %ux%u dpi, %u bpc, cs %u, %.1fx%.1f mm",
          page, sw, sh, xdpi, ydpi, h->bits_per_color, h->color_space, wmm, hmm);

  if ((line = malloc(h->bytes_per_line)) == NULL ||
      (ink = malloc((size_t)sw * sh)) == NULL)
  {
    snprintf(st->error, sizeof(st->error), "page %d: out of memory", page);
    goto done;
  }

  for (unsigned y = 0; y < sh; y ++)
  {
    if (cancel && (y & 255) == 0 && cancel(cancel_ctx))
    {
      rc = 1;
      goto done;
    }
    if (pwg_read_line(r, line))
    {
      snprintf(st->error, sizeof(st->error), "page %d: %s", page, r->error);
      goto done;
    }
    line_to_ink(line, sw, h->bits_per_color, kind, nch, ink + (size_t)y * sw);
  }

  w   = (int)sw;
  hgt = (int)sh;

  if (abs((int)xdpi - SP410_DPI) > 1 || abs((int)ydpi - SP410_DPI) > 1)
  {
    int      nw = (int)lround((double)sw * SP410_DPI / xdpi);
    int      nh = (int)lround((double)sh * SP410_DPI / ydpi);
    uint8_t *rs;

    log_msg(LOG_INFO, "page %d: resampling from %ux%u dpi to %d dpi",
            page, xdpi, ydpi, SP410_DPI);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if ((rs = resample(ink, w, hgt, nw, nh)) == NULL)
    {
      snprintf(st->error, sizeof(st->error), "page %d: out of memory", page);
      goto done;
    }
    free(ink);
    ink = rs;
    w   = nw;
    hgt = nh;
  }

  stride = ((size_t)w + 7) / 8;
  if ((bits = malloc(stride * (size_t)hgt)) == NULL ||
      dither_page(s->dither, s->threshold, ink, w, hgt, bits, stride))
  {
    snprintf(st->error, sizeof(st->error), "page %d: out of memory", page);
    goto done;
  }

  lw = w   < s->max_width_dots  ? w   : s->max_width_dots;
  lh = hgt < s->max_length_dots ? hgt : s->max_length_dots;
  if (lw < w)
    log_msg(LOG_WARN, "page %d is %d dots wide; the print head is %d dots (%d mm), "
            "the right edge will be clipped", page, w, s->max_width_dots,
            s->max_width_dots / SP410_DOTS_PER_MM);
  if (lh < hgt)
    log_msg(LOG_WARN, "page %d is %d dots long; clipping to %d dots",
            page, hgt, s->max_length_dots);
  if (wmm > s->max_width_dots / (double)SP410_DOTS_PER_MM)
    wmm = s->max_width_dots / (double)SP410_DOTS_PER_MM;
  if (hmm > s->max_length_dots / (double)SP410_DOTS_PER_MM)
    hmm = s->max_length_dots / (double)SP410_DOTS_PER_MM;

  lstride = ((size_t)lw + 7) / 8;
  if ((label = calloc(lstride, (size_t)lh)) == NULL)
  {
    snprintf(st->error, sizeof(st->error), "page %d: out of memory", page);
    goto done;
  }

  for (int ly = 0; ly < lh; ly ++)
  {
    int sy = ly - s->shift_y_dots;

    if (sy < 0 || sy >= hgt)
      continue;
    for (int lx = 0; lx < lw; lx ++)
    {
      int sx = lx - s->shift_x_dots;

      if (sx < 0 || sx >= w)
        continue;
      if (bits[(size_t)sy * stride + (sx >> 3)] & (0x80 >> (sx & 7)))
        label[(size_t)ly * lstride + (lx >> 3)] |= (uint8_t)(0x80 >> (lx & 7));
    }
  }

  if (tspl_write_page(&msink, s, wmm, hmm, label, lw, lh, lstride, copies, &ps))
  {
    snprintf(st->error, sizeof(st->error), "page %d: out of memory", page);
    goto done;
  }
  if (out->write(out->ctx, mb.data, mb.len))
  {
    snprintf(st->error, sizeof(st->error), "page %d: unable to send data to the printer", page);
    goto done;
  }

  log_msg(LOG_DEBUG, "page %d: %d band(s), %zu bitmap bytes, %zu bytes total",
          page, ps.bands, ps.bitmap_bytes, ps.total_bytes);
  st->bytes  += mb.len;
  st->labels += copies;
  rc = 0;

done:
  free(mb.data);
  free(label);
  free(bits);
  free(ink);
  free(line);
  return rc;
}

int
render_document(const uint8_t *data, size_t len, const sp410_settings_t *s,
                int copies, const tspl_sink_t *sink,
                render_cancel_fn cancel, void *cancel_ctx, render_stats_t *st)
{
  pwg_reader_t r;
  pwg_header_t h;
  int          rc = 0, more;

  memset(st, 0, sizeof(*st));
  if (pwg_open(&r, data, len))
  {
    snprintf(st->error, sizeof(st->error), "%s", r.error);
    return -1;
  }

  while ((more = pwg_next_page(&r, &h)) == 1)
  {
    int page_copies = h.num_copies > 1 ? (int)h.num_copies : (copies > 1 ? copies : 1);

    if (page_copies > 9999)
      page_copies = 9999;
    st->pages ++;
    if ((rc = render_page(&r, &h, s, page_copies, sink, st->pages, st, cancel, cancel_ctx)) != 0)
      break;
  }

  if (more < 0 && rc == 0)
  {
    snprintf(st->error, sizeof(st->error), "%s", r.error[0] ? r.error : "corrupt document");
    rc = -1;
  }
  if (rc == 0 && st->pages == 0)
  {
    snprintf(st->error, sizeof(st->error), "document contains no pages");
    rc = -1;
  }
  pwg_close(&r);
  return rc;
}
