/*
 * settings.c - Parse and apply SP410 print options.
 *
 * Precedence is decided by the caller, which applies sources in order:
 *   built-in defaults  ->  [print] section of sp410.ini  ->  IPP job attributes
 * A malformed value is logged and ignored, so the previous layer's value
 * stays in force.
 *
 * Copyright 2026 The sp410-cups-driver and sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "settings.h"
#include "log.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define strcasecmp _stricmp
#else
#  include <strings.h>
#endif

const char *const settings_keys[] =
{
  "Darkness", "PrintSpeed", "MediaTracking", "GapHeight", "GapOffset",
  "PrintDirection", "TearOff", "TearOffset", "Dither", "Threshold",
  "ShiftX", "ShiftY", NULL
};

void
settings_defaults(sp410_settings_t *s)
{
  memset(s, 0, sizeof(*s));
  s->darkness        = -1;
  s->speed           = -1;
  s->media           = MEDIA_GAP;
  s->gap_mm          = 3.0;
  s->gap_offset_mm   = 0.0;
  s->direction       = 0;
  s->tear            = 1;
  s->tear_offset_mm  = 0.0;
  s->dither          = DITHER_AUTO;
  s->threshold       = 128;
  s->max_width_dots  = SP410_MAX_WIDTH_DOTS;
  s->max_length_dots = SP410_MAX_LENGTH_DOTS;
  s->band_merge_rows = 16;
}

const char *
dither_name(dither_mode_t d)
{
  switch (d)
  {
    case DITHER_AUTO:      return "Auto";
    case DITHER_THRESHOLD: return "Threshold";
    case DITHER_ATKINSON:  return "Atkinson";
    case DITHER_FLOYD:     return "FloydSteinberg";
    case DITHER_ORDERED:   return "Ordered";
  }
  return "?";
}

const char *
media_name(media_tracking_t m)
{
  switch (m)
  {
    case MEDIA_GAP:        return "Gap";
    case MEDIA_BLACKMARK:  return "BlackMark";
    case MEDIA_CONTINUOUS: return "Continuous";
  }
  return "?";
}

static int
is_default_word(const char *v)
{
  return !strcasecmp(v, "Default") || !strcasecmp(v, "PrinterDefault") ||
         !strcasecmp(v, "None") || !*v;
}

static int
parse_int(const char *name, const char *v, int lo, int hi, int *out)
{
  char *end;
  long  n;

  errno = 0;
  n = strtol(v, &end, 10);
  if (errno || end == v || *end || n < lo || n > hi)
  {
    log_msg(LOG_WARN, "Ignoring %s=\"%s\" (expected integer %d..%d)", name, v, lo, hi);
    return -1;
  }
  *out = (int)n;
  return 0;
}

static int
parse_mm(const char *name, const char *v, double lo, double hi, double *out)
{
  char  *end;
  double d;

  errno = 0;
  d = strtod(v, &end);
  if (end != v && (!strcmp(end, "mm") || !strcmp(end, "MM")))
    end += 2;
  if (errno || end == v || *end || !isfinite(d) || d < lo || d > hi)
  {
    log_msg(LOG_WARN, "Ignoring %s=\"%s\" (expected %g..%g mm)", name, v, lo, hi);
    return -1;
  }
  *out = d;
  return 0;
}

static int
parse_bool(const char *v)
{
  if (!strcasecmp(v, "true") || !strcasecmp(v, "on") || !strcasecmp(v, "yes") ||
      !strcmp(v, "1"))
    return 1;
  if (!strcasecmp(v, "false") || !strcasecmp(v, "off") || !strcasecmp(v, "no") ||
      !strcmp(v, "0"))
    return 0;
  return -1;
}

static int
bad(const char *key, const char *v)
{
  log_msg(LOG_WARN, "Ignoring %s=\"%s\"", key, v);
  return -1;
}

int
settings_apply(sp410_settings_t *s, const char *key, const char *v)
{
  int    i;
  double d;

  if (!key || !v)
    return -1;

  if (!strcasecmp(key, "Darkness"))
  {
    if (is_default_word(v)) { s->darkness = -1; return 0; }
    if (parse_int(key, v, 0, 15, &i)) return -1;
    s->darkness = i;
  }
  else if (!strcasecmp(key, "PrintSpeed"))
  {
    if (is_default_word(v)) { s->speed = -1; return 0; }
    if (parse_int(key, v, 1, 6, &i)) return -1;
    s->speed = i;
  }
  else if (!strcasecmp(key, "MediaTracking"))
  {
    if (!strcasecmp(v, "Gap") || !strcasecmp(v, "NonContinuous") ||
        !strcasecmp(v, "Web") || !strcasecmp(v, "labels"))
      s->media = MEDIA_GAP;
    else if (!strcasecmp(v, "BlackMark") || !strcasecmp(v, "Mark"))
      s->media = MEDIA_BLACKMARK;
    else if (!strcasecmp(v, "Continuous"))
      s->media = MEDIA_CONTINUOUS;
    else
      return bad(key, v);
  }
  else if (!strcasecmp(key, "GapHeight"))
  {
    if (parse_mm(key, v, 0, 25, &d)) return -1;
    s->gap_mm = d;
  }
  else if (!strcasecmp(key, "GapOffset"))
  {
    if (parse_mm(key, v, -25, 25, &d)) return -1;
    s->gap_offset_mm = d;
  }
  else if (!strcasecmp(key, "PrintDirection"))
  {
    if (!strcasecmp(v, "Normal") || !strcmp(v, "0"))
      s->direction = 0;
    else if (!strcasecmp(v, "Rotate180") || !strcasecmp(v, "Reverse") || !strcmp(v, "1"))
      s->direction = 1;
    else
      return bad(key, v);
  }
  else if (!strcasecmp(key, "TearOff"))
  {
    int b = parse_bool(v);
    if (b < 0) return bad(key, v);
    s->tear = b;
  }
  else if (!strcasecmp(key, "TearOffset"))
  {
    if (parse_mm(key, v, -10, 10, &d)) return -1;
    s->tear_offset_mm = d;
  }
  else if (!strcasecmp(key, "ShiftX"))
  {
    if (parse_mm(key, v, -20, 20, &d)) return -1;
    s->shift_x_dots = (int)lround(d * SP410_DOTS_PER_MM);
  }
  else if (!strcasecmp(key, "ShiftY"))
  {
    if (parse_mm(key, v, -20, 20, &d)) return -1;
    s->shift_y_dots = (int)lround(d * SP410_DOTS_PER_MM);
  }
  else if (!strcasecmp(key, "Dither"))
  {
    if (!strcasecmp(v, "Auto"))
      s->dither = DITHER_AUTO;
    else if (!strcasecmp(v, "Threshold") || !strcasecmp(v, "None"))
      s->dither = DITHER_THRESHOLD;
    else if (!strcasecmp(v, "Atkinson"))
      s->dither = DITHER_ATKINSON;
    else if (!strcasecmp(v, "FloydSteinberg") || !strcasecmp(v, "ErrorDiffusion"))
      s->dither = DITHER_FLOYD;
    else if (!strcasecmp(v, "Ordered") || !strcasecmp(v, "Bayer"))
      s->dither = DITHER_ORDERED;
    else
      return bad(key, v);
  }
  else if (!strcasecmp(key, "Threshold"))
  {
    if (parse_int(key, v, 1, 254, &i)) return -1;
    s->threshold = i;
  }
  else
    return 1;

  return 0;
}
