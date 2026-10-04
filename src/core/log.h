/*
 * log.h - Minimal leveled logging shared by the server, CLI and core.
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SP410_LOG_H
#define SP410_LOG_H

#include <stdio.h>

typedef enum { LOG_DEBUG = 0, LOG_INFO, LOG_WARN, LOG_ERROR } log_level_t;

void log_set_level(log_level_t level);
void log_set_file(FILE *fp);          /* NULL = stderr only */
int  log_parse_level(const char *name, log_level_t *out);

/*
 * printf-style checking.  MinGW builds use MinGW's C99 printf
 * (__USE_MINGW_ANSI_STDIO), so check against GNU rules there (%zu etc.),
 * not the Microsoft CRT's.
 */
#if defined(__MINGW32__) && !defined(__clang__)
#  define SP410_PRINTF(f, a) __attribute__((format(gnu_printf, f, a)))
#elif defined(__GNUC__) || defined(__clang__)
#  define SP410_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#  define SP410_PRINTF(f, a)
#endif

SP410_PRINTF(2, 3)
void log_msg(log_level_t level, const char *fmt, ...);

#endif /* SP410_LOG_H */
