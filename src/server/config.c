/*
 * config.c - Read sp410.ini.
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "config.h"
#include "../core/log.h"
#include "../platform/compat.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
config_defaults(config_t *c)
{
  memset(c, 0, sizeof(*c));
  strcpy(c->listen, "127.0.0.1");
  c->port = 8631;
  strcpy(c->name, "iDPRT SP410");
  strcpy(c->model, "SP410");
  strcpy(c->log_level, "info");
  c->max_job_mb = 64;
  strcpy(c->device, "usb");
  c->stall_timeout_s = 120;
  c->retry_timeout_s = 300;
  strcpy(c->default_media, "na_index-4x6_4x6in");
}

const char *
config_default_path(void)
{
  static char path[600];

  if (!path[0])
    snprintf(path, sizeof(path), "%s%csp410.ini", default_data_dir(), PATH_SEP);
  return path;
}

static char *
strip(char *s)
{
  char *e;

  while (isspace((unsigned char)*s))
    s ++;
  e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1]))
    *--e = '\0';
  if (e - s >= 2 && ((s[0] == '"' && e[-1] == '"') || (s[0] == '\'' && e[-1] == '\'')))
  {
    e[-1] = '\0';
    s ++;
  }
  return s;
}

static void
copy(char *dst, size_t size, const char *src)
{
  snprintf(dst, size, "%s", src);
}

static int
to_int(const char *key, const char *v, int lo, int hi, int def)
{
  char *end;
  long  n = strtol(v, &end, 10);

  if (end == v || *end || n < lo || n > hi)
  {
    log_msg(LOG_WARN, "config: ignoring %s=\"%s\" (expected %d..%d)", key, v, lo, hi);
    return def;
  }
  return (int)n;
}

int
config_load(config_t *c, const char *path)
{
  FILE *fp;
  char  line[1024], section[32] = "";
  int   lineno = 0;

  copy(c->path, sizeof(c->path), path);
  if ((fp = fopen(path, "r")) == NULL)
    return errno == ENOENT ? 1 : -1;

  while (fgets(line, sizeof(line), fp))
  {
    char *s = line, *eq, *key, *val;

    lineno ++;
    if (lineno == 1 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF)
      s += 3;                                   /* UTF-8 BOM (Notepad) */
    s = strip(s);
    if (!*s || *s == ';' || *s == '#')
      continue;
    if (*s == '[')
    {
      char *e = strchr(s, ']');
      if (e)
      {
        *e = '\0';
        copy(section, sizeof(section), strip(s + 1));
      }
      continue;
    }
    if ((eq = strchr(s, '=')) == NULL)
    {
      log_msg(LOG_WARN, "config %s:%d: expected key = value", path, lineno);
      continue;
    }
    *eq = '\0';
    key = strip(s);
    {
      /* inline comment: whitespace followed by ';' or '#' */
      char *p = eq + 1;
      for (; *p; p ++)
        if ((*p == ';' || *p == '#') && p > eq + 1 && isspace((unsigned char)p[-1]))
        {
          *p = '\0';
          break;
        }
    }
    val = strip(eq + 1);

    if (!strcasecmp(section, "server"))
    {
      if (!strcasecmp(key, "listen"))          copy(c->listen, sizeof(c->listen), val);
      else if (!strcasecmp(key, "port"))       c->port = to_int(key, val, 0, 65535, c->port);
      else if (!strcasecmp(key, "name"))       copy(c->name, sizeof(c->name), val);
      else if (!strcasecmp(key, "location"))   copy(c->location, sizeof(c->location), val);
      else if (!strcasecmp(key, "model"))      copy(c->model, sizeof(c->model), val);
      else if (!strcasecmp(key, "log-level"))  copy(c->log_level, sizeof(c->log_level), val);
      else if (!strcasecmp(key, "log-file"))   copy(c->log_file, sizeof(c->log_file), val);
      else if (!strcasecmp(key, "max-job-mb")) c->max_job_mb = to_int(key, val, 1, 2048, c->max_job_mb);
      else log_msg(LOG_WARN, "config %s:%d: unknown [server] key \"%s\"", path, lineno, key);
    }
    else if (!strcasecmp(section, "device"))
    {
      if (!strcasecmp(key, "uri"))                  copy(c->device, sizeof(c->device), val);
      else if (!strcasecmp(key, "stall-timeout"))   c->stall_timeout_s = to_int(key, val, 1, 86400, c->stall_timeout_s);
      else if (!strcasecmp(key, "retry-timeout"))   c->retry_timeout_s = to_int(key, val, 0, 86400, c->retry_timeout_s);
      else log_msg(LOG_WARN, "config %s:%d: unknown [device] key \"%s\"", path, lineno, key);
    }
    else if (!strcasecmp(section, "media"))
    {
      if (!strcasecmp(key, "default")) copy(c->default_media, sizeof(c->default_media), val);
      else log_msg(LOG_WARN, "config %s:%d: unknown [media] key \"%s\"", path, lineno, key);
    }
    else if (!strcasecmp(section, "print"))
    {
      if (c->num_print < CONFIG_MAX_PRINT)
      {
        copy(c->print_key[c->num_print], sizeof(c->print_key[0]), key);
        copy(c->print_val[c->num_print], sizeof(c->print_val[0]), val);
        c->num_print ++;
      }
    }
    else
      log_msg(LOG_WARN, "config %s:%d: key \"%s\" outside a known section", path, lineno, key);
  }
  fclose(fp);
  return 0;
}

void
config_print_settings(const config_t *c, sp410_settings_t *s)
{
  settings_defaults(s);
  for (int i = 0; i < c->num_print; i ++)
    if (settings_apply(s, c->print_key[i], c->print_val[i]) > 0)
      log_msg(LOG_WARN, "config: unknown [print] option \"%s\"", c->print_key[i]);
}

static const char template_text[] =
"; sp410.ini - settings for the SP410 open-source driver (sp410-ippd).\n"
"; Changes to [device], [media] and [print] apply to the next label; changes to\n"
"; [server] need a restart of the \"SP410 Open Driver\" service\n"
"; (Services app, or an admin prompt: sc stop SP410IPP && sc start SP410IPP).\n"
"\n"
"[server]\n"
"; Only this computer can print by default. Use 0.0.0.0 to share on the network.\n"
"listen = 127.0.0.1\n"
"port = 8631\n"
"name = iDPRT SP410\n"
"location =\n"
"; SP410, SP410BT or SP420\n"
"model = SP410\n"
"; debug, info, warn or error\n"
"log-level = info\n"
"max-job-mb = 64\n"
"\n"
"[device]\n"
"; usb                         first iDPRT USB printer (default)\n"
"; usb:any                     first USB printer of any brand\n"
"; serial:COM5?baud=115200     SP410BT over Bluetooth (outgoing COM port)\n"
"; socket://192.168.1.50:9100  printer on a network print server\n"
"; spool:Name of a Windows printer   send RAW data through an existing queue\n"
"uri = usb\n"
"; seconds without progress before a job fails (paper out, cover open)\n"
"stall-timeout = 120\n"
"; seconds to keep retrying when the printer is off or unplugged\n"
"retry-timeout = 300\n"
"\n"
"[media]\n"
"; default label size: na_index-4x6_4x6in, oe_4x2-label_4x2in, om_100x150-label_100x150mm ...\n"
"default = na_index-4x6_4x6in\n"
"\n"
"[print]\n"
"; Same options as the Linux driver. Remove the ';' to change a setting.\n"
"; Darkness = Default        ; Default or 0-15\n"
"; PrintSpeed = Default      ; Default or 2-6 inches per second\n"
"; MediaTracking = Gap       ; Gap, BlackMark or Continuous\n"
"; GapHeight = 3             ; mm\n"
"; GapOffset = 0             ; mm\n"
"; PrintDirection = Normal   ; Normal or Rotate180\n"
"; TearOff = True            ; advance the label to the tear bar\n"
"; TearOffset = 0            ; mm, -10 to 10\n"
"; Dither = Auto             ; Auto, Threshold, Atkinson, FloydSteinberg, Ordered\n"
"; Threshold = 128           ; 1-254, lower is darker\n"
"; ShiftX = 0                ; mm\n"
"; ShiftY = 0                ; mm\n";

int
config_write_template(const char *path)
{
  FILE *fp = fopen(path, "w");

  if (!fp)
    return -1;
  if (fputs(template_text, fp) == EOF)
  {
    fclose(fp);
    return -1;
  }
  return fclose(fp) ? -1 : 0;
}
