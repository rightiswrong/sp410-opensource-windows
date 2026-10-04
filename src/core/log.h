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

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void log_msg(log_level_t level, const char *fmt, ...);

#endif /* SP410_LOG_H */
