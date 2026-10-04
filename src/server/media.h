/*
 * media.h - Label sizes advertised to Windows (PWG 5101.1 self-describing names).
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SP410_MEDIA_H
#define SP410_MEDIA_H

typedef struct
{
  const char *pwg;          /* e.g. "na_index-4x6_4x6in" */
  int         width;        /* hundredths of a millimetre */
  int         length;
} media_size_t;

extern const media_size_t media_sizes[];
extern const int          media_count;

/* Custom size limits (hundredths of mm): 20-108 mm wide, 10-300 mm long. */
#define MEDIA_MIN_WIDTH   2000
#define MEDIA_MAX_WIDTH   10800
#define MEDIA_MIN_LENGTH  1000
#define MEDIA_MAX_LENGTH  30000

const media_size_t *media_find(const char *pwg);

#endif /* SP410_MEDIA_H */
