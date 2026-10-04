/*
 * sp410-cli - talk to an iDPRT SP410 directly, without the print service.
 *
 *   sp410-cli list                       USB printers Windows can see
 *   sp410-cli status                     ESC !? status byte, decoded
 *   sp410-cli info                       model, code page, mileage
 *   sp410-cli test-label [--size 4x6]    label drawn with printer-resident fonts
 *   sp410-cli calibrate [--blackmark]    measure label + gap (GAPDETECT/BLINEDETECT)
 *   sp410-cli selftest | feed | home | reset
 *   sp410-cli raw FILE                   send a file verbatim
 *   sp410-cli convert IN.pwg OUT.prn [Key=Value ...] [--copies N]
 *   sp410-cli config-init [FILE]         write a commented sp410.ini
 *
 *   --device URI   override [device] uri from sp410.ini
 *   --config FILE  settings file (default %ProgramData%\SP410\sp410.ini)
 *   --dry-run      write printer bytes to stdout instead of the device
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../core/log.h"
#include "../core/render.h"
#include "../platform/compat.h"
#include "../platform/device.h"
#include "../server/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#endif

#ifndef SP410_VERSION
#  define SP410_VERSION "0.1.0"
#endif

static const char *const status_bits[] =
{
  "print head open", "paper jam", "out of paper", "out of ribbon",
  "paused", "printing", "cover open / model-specific", "other error"
};

static void
usage(void)
{
  puts("Usage: sp410-cli [--device URI] [--config FILE] [--dry-run] COMMAND\n"
       "\n"
       "Commands:\n"
       "  list                     list USB printers\n"
       "  status                   query the printer status byte\n"
       "  info                     model name, code page and print-head mileage\n"
       "  test-label [--size 4x6|100x150mm] [--gap MM]\n"
       "                           print a test label with built-in fonts\n"
       "  calibrate [--blackmark]  measure label and gap length after loading media\n"
       "  selftest                 print the printer's configuration label\n"
       "  feed | home | reset      feed one label / to next label / reset printer\n"
       "  raw FILE                 send FILE to the printer unchanged\n"
       "  convert IN.pwg OUT.prn [Key=Value ...] [--copies N]\n"
       "                           convert PWG raster to TSPL without printing\n"
       "  config-init [FILE]       write a commented settings file\n"
       "  version                  print the version\n"
       "\n"
       "Device URIs: usb, usb:any, serial:COM5?baud=115200, socket://HOST[:9100], spool:QUEUE");
}

typedef struct
{
  const char *device;
  int         dry_run;
} opts_t;

static device_t *
open_dev(const opts_t *o)
{
  char      err[256];
  device_t *d;

  if (o->dry_run)
    return NULL;
  if ((d = device_open(o->device, 0, err, sizeof(err))) == NULL)
    fprintf(stderr, "sp410-cli: %s\n", err);
  return d;
}

static int
send_bytes(const opts_t *o, const void *buf, size_t len)
{
  device_t *d;
  char      err[256] = "";
  int       rc;

  if (o->dry_run)
  {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
    return 0;
  }
  if ((d = open_dev(o)) == NULL)
    return 1;
  rc = device_write(d, buf, len, 30000, NULL, NULL, err, sizeof(err));
  if (rc)
  {
    fprintf(stderr, "sp410-cli: %s\n", err);
    device_discard(d);
    return 1;
  }
  return device_close(d) ? 1 : 0;
}

static int
query(const opts_t *o, const char *cmd, size_t cmdlen, char *reply, size_t size, int timeout_ms)
{
  device_t *d;
  char      err[256] = "";
  int       n;

  if (o->dry_run)
  {
    fwrite(cmd, 1, cmdlen, stdout);
    return 0;
  }
  if ((d = open_dev(o)) == NULL)
    return -1;
  if (device_write(d, cmd, cmdlen, 5000, NULL, NULL, err, sizeof(err)))
  {
    fprintf(stderr, "sp410-cli: %s\n", err);
    device_close(d);
    return -1;
  }
  n = device_read(d, reply, size - 1, timeout_ms);
  device_close(d);
  if (n < 0)
  {
    fprintf(stderr, "sp410-cli: this connection cannot read replies from the printer\n");
    return -1;
  }
  reply[n] = '\0';
  return n;
}

static void
list_cb(const char *path, const char *desc, int is_idprt, void *ctx)
{
  int *n = ctx;
  printf("%s%s\n    %s\n", desc && *desc ? desc : "USB printer",
         is_idprt ? "  (iDPRT)" : "", path);
  (*n) ++;
}

static int
parse_size(const char *s, double *w, double *h)
{
  char   unit[8] = "";
  double f;

  if (sscanf(s, "%lfx%lf%7s", w, h, unit) < 2 || *w <= 0 || *h <= 0)
    return -1;
  f = !strcmp(unit, "mm") ? 1.0 : (!unit[0] || !strcmp(unit, "in")) ? 25.4 : 0;
  if (!f)
    return -1;
  *w *= f;
  *h *= f;
  return 0;
}

static int
cmd_test_label(const opts_t *o, int argc, char **argv)
{
  double w = 101.6, h = 152.4, gap = 3.0;
  char   buf[2048];
  int    n = 0, wd, hd, m = 16;

  for (int i = 0; i < argc; i ++)
  {
    if (!strcmp(argv[i], "--size") && i + 1 < argc)
    {
      if (parse_size(argv[++ i], &w, &h))
      {
        fprintf(stderr, "sp410-cli: --size expects e.g. 4x6 or 100x150mm\n");
        return 2;
      }
    }
    else if (!strcmp(argv[i], "--gap") && i + 1 < argc)
      gap = atof(argv[++ i]);
  }
  wd = (int)(w * 8);
  hd = (int)(h * 8);

#define ADD(...) n += snprintf(buf + n, sizeof(buf) - (size_t)n, __VA_ARGS__)
  ADD("SIZE %.1f mm,%.1f mm\r\nGAP %.1f mm,0 mm\r\nDIRECTION 0\r\nREFERENCE 0,0\r\nCLS\r\n", w, h, gap);
  ADD("BOX %d,%d,%d,%d,4\r\n", m, m, wd - m, hd - m);
  ADD("TEXT %d,%d,\"3\",0,1,1,\"sp410-opensource-windows\"\r\n", m + 16, m + 16);
  ADD("TEXT %d,%d,\"2\",0,1,1,\"TSPL test label %.0fx%.0f mm\"\r\n", m + 16, m + 56, w, h);
  if (hd > 240)
    ADD("BARCODE %d,%d,\"128\",80,1,0,2,2,\"SP410-OK-1234\"\r\n", m + 16, m + 100);
  if (hd > 420 && wd > 300)
    ADD("QRCODE %d,%d,L,6,A,0,\"https://github.com/rightiswrong/sp410-opensource-windows\"\r\n",
        m + 16, m + 230);
  ADD("PRINT 1,1\r\n");
#undef ADD
  return send_bytes(o, buf, (size_t)n);
}

static int
file_write(void *ctx, const void *d, size_t n)
{
  return fwrite(d, 1, n, (FILE *)ctx) == n ? 0 : -1;
}

static int
cmd_convert(int argc, char **argv)
{
  sp410_settings_t s;
  uint8_t         *data;
  size_t           len;
  FILE            *out;
  int              copies = 1, rc;
  render_stats_t   st;
  tspl_sink_t      sink;

  if (argc < 2)
  {
    fprintf(stderr, "sp410-cli: convert needs IN.pwg and OUT.prn\n");
    return 2;
  }
  settings_defaults(&s);
  for (int i = 2; i < argc; i ++)
  {
    char key[64], *eq;

    if (!strcmp(argv[i], "--copies") && i + 1 < argc)
    {
      copies = atoi(argv[++ i]);
      continue;
    }
    snprintf(key, sizeof(key), "%s", argv[i]);
    if ((eq = strchr(key, '=')) == NULL)
    {
      fprintf(stderr, "sp410-cli: expected Key=Value, got \"%s\"\n", argv[i]);
      return 2;
    }
    *eq = '\0';
    if (settings_apply(&s, key, eq + 1) > 0)
    {
      fprintf(stderr, "sp410-cli: unknown option \"%s\"\n", key);
      return 2;
    }
  }

  if (read_file(argv[0], &data, &len))
  {
    fprintf(stderr, "sp410-cli: cannot read %s\n", argv[0]);
    return 1;
  }
  if (!strcmp(argv[1], "-"))
  {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    out = stdout;
  }
  else if ((out = fopen(argv[1], "wb")) == NULL)
  {
    fprintf(stderr, "sp410-cli: cannot create %s\n", argv[1]);
    free(data);
    return 1;
  }

  sink.ctx   = out;
  sink.write = file_write;

  rc = render_document(data, len, &s, copies, &sink, NULL, NULL, &st);
  free(data);
  if (out != stdout && fclose(out))
    rc = -1;
  if (rc)
  {
    fprintf(stderr, "sp410-cli: conversion failed: %s\n", st.error);
    if (out != stdout)
      remove(argv[1]);
    return 1;
  }
  fprintf(stderr, "converted %d page(s), %d label(s), %zu bytes\n", st.pages, st.labels, st.bytes);
  return 0;
}

int
main(int argc, char *argv[])
{
  config_t    cfg;
  opts_t      o = { NULL, 0 };
  const char *config_path = NULL, *cmd;
  int         i;

  log_set_level(LOG_WARN);

  for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i ++)
  {
    if (!strcmp(argv[i], "--device") && i + 1 < argc)
      o.device = argv[++ i];
    else if (!strcmp(argv[i], "--config") && i + 1 < argc)
      config_path = argv[++ i];
    else if (!strcmp(argv[i], "--dry-run"))
      o.dry_run = 1;
    else if (!strcmp(argv[i], "--help"))
    {
      usage();
      return 0;
    }
    else
    {
      fprintf(stderr, "sp410-cli: unknown option \"%s\"\n", argv[i]);
      return 2;
    }
  }
  if (i >= argc)
  {
    usage();
    return 2;
  }
  cmd = argv[i ++];

  config_defaults(&cfg);
  config_load(&cfg, config_path ? config_path : config_default_path());
  if (!o.device)
    o.device = cfg.device;

  if (net_init())
    return 1;

  if (!strcmp(cmd, "version"))
  {
    puts("sp410-cli " SP410_VERSION);
    return 0;
  }
  if (!strcmp(cmd, "list"))
  {
    int n = 0;
    if (device_list_usb(list_cb, &n) < 0)
    {
      fprintf(stderr, "sp410-cli: cannot enumerate USB printers\n");
      return 1;
    }
    if (!n)
      puts("No USB printers found. Is the SP410 connected and switched on?");
    return 0;
  }
  if (!strcmp(cmd, "status"))
  {
    char reply[64];
    int  n = query(&o, "\x1b!?", 3, reply, sizeof(reply), 1000);
    unsigned b;

    if (n < 0)
      return 1;
    if (o.dry_run)
      return 0;
    if (n == 0)
    {
      puts("No reply (printer busy, off, or this connection is one-way).");
      return 2;
    }
    b = (unsigned char)reply[0];
    printf("status 0x%02X: ", b);
    if (!b)
      puts("ready");
    else
    {
      int first = 1;
      for (int k = 0; k < 8; k ++)
        if (b & (1u << k))
        {
          printf("%s%s", first ? "" : ", ", status_bits[k]);
          first = 0;
        }
      putchar('\n');
    }
    return (b == 0 || b == 0x20) ? 0 : 1;
  }
  if (!strcmp(cmd, "info"))
  {
    static const char *const labels[] = { "model", "code page", "mileage" };
    static const char *const cmds[]   = { "~!T\r\n", "~!I\r\n", "~!@\r\n" };
    for (int k = 0; k < 3; k ++)
    {
      char reply[256];
      int  n = query(&o, cmds[k], strlen(cmds[k]), reply, sizeof(reply), 1500);
      if (n < 0)
        return 1;
      if (!o.dry_run)
      {
        reply[strcspn(reply, "\r\n")] = '\0';
        printf("%10s: %s\n", labels[k], n ? reply : "(no reply)");
      }
    }
    return 0;
  }
  if (!strcmp(cmd, "test-label"))
    return cmd_test_label(&o, argc - i, argv + i);
  if (!strcmp(cmd, "calibrate"))
  {
    int bm = i < argc && !strcmp(argv[i], "--blackmark");
    const char *c = bm ? "BLINEDETECT\r\n" : "GAPDETECT\r\n";
    return send_bytes(&o, c, strlen(c));
  }
  if (!strcmp(cmd, "selftest")) return send_bytes(&o, "SELFTEST\r\n", 10);
  if (!strcmp(cmd, "feed"))     return send_bytes(&o, "FORMFEED\r\n", 10);
  if (!strcmp(cmd, "home"))     return send_bytes(&o, "HOME\r\n", 6);
  if (!strcmp(cmd, "reset"))    return send_bytes(&o, "\x1b!R", 3);
  if (!strcmp(cmd, "raw"))
  {
    uint8_t *data;
    size_t   len;
    int      rc;

    if (i >= argc || read_file(argv[i], &data, &len))
    {
      fprintf(stderr, "sp410-cli: raw needs a readable FILE\n");
      return 2;
    }
    rc = send_bytes(&o, data, len);
    free(data);
    return rc;
  }
  if (!strcmp(cmd, "convert"))
    return cmd_convert(argc - i, argv + i);
  if (!strcmp(cmd, "config-init"))
  {
    const char *path = i < argc ? argv[i] : config_default_path();
    FILE       *fp = fopen(path, "r");

    if (fp)
    {
      fclose(fp);
      fprintf(stderr, "sp410-cli: %s already exists; not overwriting\n", path);
      return 1;
    }
    if (i >= argc)
      make_dir(default_data_dir());
    if (config_write_template(path))
    {
      fprintf(stderr, "sp410-cli: cannot write %s\n", path);
      return 1;
    }
    printf("wrote %s\n", path);
    return 0;
  }

  fprintf(stderr, "sp410-cli: unknown command \"%s\"\n", cmd);
  usage();
  return 2;
}
