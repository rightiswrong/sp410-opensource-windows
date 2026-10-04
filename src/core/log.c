/*
 * log.c - Minimal leveled logging.
 *
 * Messages go to stderr and, if set, a log file.  A single mutex-free
 * fprintf per message is atomic enough for line-oriented logs on both
 * Windows (CRT stream locks) and POSIX (stdio locks).
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "log.h"

#include <stdarg.h>
#include <string.h>
#include <time.h>

static log_level_t g_level = LOG_INFO;
static FILE       *g_file  = NULL;

void log_set_level(log_level_t level) { g_level = level; }
void log_set_file(FILE *fp)           { g_file = fp; }

int
log_parse_level(const char *name, log_level_t *out)
{
  static const char *const names[] = { "debug", "info", "warn", "error" };

  for (int i = 0; i < 4; i ++)
    if (name && !strcmp(name, names[i]))
    {
      *out = (log_level_t)i;
      return 0;
    }
  return -1;
}

void
log_msg(log_level_t level, const char *fmt, ...)
{
  static const char *const tags[] = { "DEBUG", "INFO", "WARN", "ERROR" };
  char       msg[2048], stamp[32];
  va_list    ap;
  time_t     now = time(NULL);
  struct tm  tmbuf;
  int        ok;

  if (level < g_level)
    return;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

#ifdef _WIN32
  ok = localtime_s(&tmbuf, &now) == 0;
#else
  ok = localtime_r(&now, &tmbuf) != NULL;
#endif
  if (!ok || !strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmbuf))
    strcpy(stamp, "-");

  fprintf(stderr, "%s %-5s %s\n", stamp, tags[level], msg);
  if (g_file)
  {
    fprintf(g_file, "%s %-5s %s\n", stamp, tags[level], msg);
    fflush(g_file);
  }
}
