/*
 * config.h - sp410.ini configuration.
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SP410_CONFIG_H
#define SP410_CONFIG_H

#include "../core/settings.h"

#define CONFIG_MAX_PRINT 32

typedef struct
{
  char path[512];
  /* [server] */
  char listen[64];
  int  port;
  char name[128];
  char location[128];
  char model[32];             /* SP410, SP410BT, SP420 */
  char log_level[16];
  char log_file[512];
  int  max_job_mb;
  /* [device] */
  char device[512];
  int  device_locked;         /* set by --device: ignore [device] uri on reload */
  int  stall_timeout_s;       /* give up when the printer accepts no data this long */
  int  retry_timeout_s;       /* keep retrying an unreachable printer this long */
  /* [media] */
  char default_media[96];
  /* [print]: passed to settings_apply() in file order */
  int  num_print;
  char print_key[CONFIG_MAX_PRINT][32];
  char print_val[CONFIG_MAX_PRINT][64];
} config_t;

void config_defaults(config_t *c);
/* Returns 0 if read, 1 if the file does not exist (defaults kept), -1 on error. */
int  config_load(config_t *c, const char *path);
/* Apply [print] settings on top of built-in defaults. */
void config_print_settings(const config_t *c, sp410_settings_t *s);
/* Write a commented template; returns 0 on success. */
int  config_write_template(const char *path);
const char *config_default_path(void);

#endif /* SP410_CONFIG_H */
