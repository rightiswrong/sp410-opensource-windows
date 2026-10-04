/*
 * media.c - Label size table.
 *
 * The same sizes as the Linux PPDs.  Inch sizes use 2540 hundredths of a
 * millimetre per inch, exactly as PWG 5101.1 requires.
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "media.h"

#include <string.h>

const media_size_t media_sizes[] =
{
  { "na_index-4x6_4x6in",             10160, 15240 },
  { "oe_4x4-label_4x4in",             10160, 10160 },
  { "oe_4x3-label_4x3in",             10160,  7620 },
  { "oe_4x2-label_4x2in",             10160,  5080 },
  { "oe_4x1-label_4x1in",             10160,  2540 },
  { "oe_3x2-label_3x2in",              7620,  5080 },
  { "oe_3x1-label_3x1in",              7620,  2540 },
  { "oe_2x1-label_2x1in",              5080,  2540 },
  { "oe_2.25x1.25-label_2.25x1.25in",  5715,  3175 },
  { "oe_2.25x0.75-label_2.25x0.75in",  5715,  1905 },
  { "om_100x150-label_100x150mm",     10000, 15000 },
  { "om_100x100-label_100x100mm",     10000, 10000 },
  { "om_100x70-label_100x70mm",       10000,  7000 },
  { "om_100x50-label_100x50mm",       10000,  5000 },
  { "om_80x50-label_80x50mm",          8000,  5000 },
  { "om_70x40-label_70x40mm",          7000,  4000 },
  { "om_60x40-label_60x40mm",          6000,  4000 },
  { "om_57x32-label_57x32mm",          5700,  3200 },
  { "om_50x30-label_50x30mm",          5000,  3000 },
  { "om_40x30-label_40x30mm",          4000,  3000 },
  { "om_38x25-label_38x25mm",          3800,  2500 },
};

const int media_count = (int)(sizeof(media_sizes) / sizeof(media_sizes[0]));

const media_size_t *
media_find(const char *pwg)
{
  for (int i = 0; pwg && i < media_count; i ++)
    if (!strcmp(media_sizes[i].pwg, pwg))
      return &media_sizes[i];
  return NULL;
}
