/*
 * main.c - sp410-ippd: run the SP410 IPP printer in a console or as a
 * Windows service.
 *
 *   sp410-ippd                    run in the foreground (Ctrl+C to stop)
 *   sp410-ippd --service          entry point used by the Windows service
 *   sp410-ippd --config FILE --port N --listen ADDR --device URI --log-level L
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "config.h"
#include "server.h"
#include "../core/log.h"
#include "../platform/compat.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SP410_VERSION
#  define SP410_VERSION "0.1.0"
#endif

#define SERVICE_NAME "SP410IPP"

static server_t *g_server;
static config_t  g_cfg;

static void
usage(void)
{
  puts("Usage: sp410-ippd [options]\n"
       "\n"
       "Runs an IPP Everywhere printer that Windows (or any IPP client) prints to,\n"
       "converting each page to TSPL for the iDPRT SP410.\n"
       "\n"
       "  --config FILE     settings file (default: %ProgramData%\\SP410\\sp410.ini)\n"
       "  --listen ADDR     address to listen on (default 127.0.0.1)\n"
       "  --port N          TCP port (default 8631, 0 = pick a free port)\n"
       "  --device URI      printer device (usb, serial:COM5, socket://host, file:path)\n"
       "  --log-level L     debug, info, warn, error\n"
       "  --port-file FILE  write the bound port number to FILE once listening\n"
       "  --service         run as the Windows service (used by the installer)\n"
       "  --version         print the version and exit");
}

static int
run(const char *port_file)
{
  int rc;

  if ((g_server = server_create(&g_cfg)) == NULL)
    return 1;
  if (port_file)
  {
    FILE *fp = fopen(port_file, "w");
    if (fp)
    {
      fprintf(fp, "%d\n", server_port(g_server));
      fclose(fp);
    }
  }
  rc = server_run(g_server);
  server_destroy(g_server);
  g_server = NULL;
  return rc ? 1 : 0;
}

#ifdef _WIN32
static void
open_log_file(void)
{
  char        path[700];
  const char *p = g_cfg.log_file;
  FILE       *fp;

  if (!*p)
  {
    make_dir(default_data_dir());
    snprintf(path, sizeof(path), "%s%csp410-ippd.log", default_data_dir(), PATH_SEP);
    p = path;
  }
  /* Keep the log bounded: start over when it passes 1 MB. */
  fp = fopen(p, "a");
  if (fp && ftell(fp) > 1024 * 1024)
  {
    fclose(fp);
    fp = fopen(p, "w");
  }
  if (fp)
    log_set_file(fp);
}
#endif

/* ---- console -------------------------------------------------------------- */

#ifdef _WIN32
static BOOL WINAPI
console_handler(DWORD type)
{
  (void)type;
  if (g_server)
    server_stop(g_server);
  return TRUE;
}
#else
static void
on_signal(int sig)
{
  (void)sig;
  if (g_server)
    server_stop(g_server);
}
#endif

/* ---- Windows service --------------------------------------------------------- */

#ifdef _WIN32
static SERVICE_STATUS_HANDLE g_status_handle;
static SERVICE_STATUS        g_status;

static void
report(DWORD state, DWORD exit_code, DWORD wait_hint)
{
  static DWORD checkpoint = 1;

  g_status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwCurrentState            = state;
  g_status.dwWin32ExitCode           = exit_code;
  g_status.dwWaitHint                = wait_hint;
  g_status.dwControlsAccepted        = state == SERVICE_START_PENDING ? 0 :
                                       SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
  g_status.dwCheckPoint              = (state == SERVICE_RUNNING || state == SERVICE_STOPPED)
                                       ? 0 : checkpoint ++;
  SetServiceStatus(g_status_handle, &g_status);
}

static DWORD WINAPI
service_ctrl(DWORD ctrl, DWORD type, LPVOID data, LPVOID ctx)
{
  (void)type; (void)data; (void)ctx;
  if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN)
  {
    report(SERVICE_STOP_PENDING, NO_ERROR, 10000);
    if (g_server)
      server_stop(g_server);
    return NO_ERROR;
  }
  if (ctrl == SERVICE_CONTROL_INTERROGATE)
    return NO_ERROR;
  return ERROR_CALL_NOT_IMPLEMENTED;
}

static void WINAPI
service_main(DWORD argc, LPSTR *argv)
{
  (void)argc; (void)argv;
  g_status_handle = RegisterServiceCtrlHandlerExA(SERVICE_NAME, service_ctrl, NULL);
  if (!g_status_handle)
    return;
  report(SERVICE_START_PENDING, NO_ERROR, 5000);

  open_log_file();
  if ((g_server = server_create(&g_cfg)) == NULL)
  {
    report(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
    return;
  }
  report(SERVICE_RUNNING, NO_ERROR, 0);
  server_run(g_server);
  server_destroy(g_server);
  g_server = NULL;
  report(SERVICE_STOPPED, NO_ERROR, 0);
}
#endif

/* ---- main -------------------------------------------------------------------- */

int
main(int argc, char *argv[])
{
  const char *config_path = NULL, *port_file = NULL;
  const char *o_listen = NULL, *o_device = NULL, *o_level = NULL;
  int         o_port = -1, service = 0, rc;
  log_level_t level;

  for (int i = 1; i < argc; i ++)
  {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : NULL;

    if (!strcmp(a, "--version"))
    {
      puts("sp410-ippd " SP410_VERSION);
      return 0;
    }
    else if (!strcmp(a, "--help") || !strcmp(a, "-h") || !strcmp(a, "/?"))
    {
      usage();
      return 0;
    }
    else if (!strcmp(a, "--service"))
      service = 1;
    else if (!strcmp(a, "--console"))
      service = 0;
    else if (v && !strcmp(a, "--config"))    { config_path = v; i ++; }
    else if (v && !strcmp(a, "--listen"))    { o_listen = v; i ++; }
    else if (v && !strcmp(a, "--device"))    { o_device = v; i ++; }
    else if (v && !strcmp(a, "--log-level")) { o_level = v; i ++; }
    else if (v && !strcmp(a, "--port-file")) { port_file = v; i ++; }
    else if (v && !strcmp(a, "--port"))      { o_port = atoi(v); i ++; }
    else
    {
      fprintf(stderr, "sp410-ippd: unknown or incomplete option \"%s\"\n", a);
      usage();
      return 2;
    }
  }

  config_defaults(&g_cfg);
  if (!config_path)
    config_path = config_default_path();
  rc = config_load(&g_cfg, config_path);
  if (rc < 0)
    fprintf(stderr, "sp410-ippd: cannot read %s; using defaults\n", config_path);
  else if (rc > 0)
    g_cfg.path[0] = '\0';          /* no file: nothing to re-read per job */

  if (o_listen) snprintf(g_cfg.listen, sizeof(g_cfg.listen), "%s", o_listen);
  if (o_device) snprintf(g_cfg.device, sizeof(g_cfg.device), "%s", o_device);
  if (o_level)  snprintf(g_cfg.log_level, sizeof(g_cfg.log_level), "%s", o_level);
  if (o_port >= 0) g_cfg.port = o_port;
  /* A device given on the command line wins over the file for every job. */
  g_cfg.device_locked = o_device != NULL;

  if (log_parse_level(g_cfg.log_level, &level) == 0)
    log_set_level(level);

  if (net_init())
  {
    fprintf(stderr, "sp410-ippd: cannot initialise networking\n");
    return 1;
  }

#ifdef _WIN32
  if (service)
  {
    SERVICE_TABLE_ENTRYA table[] = { { (LPSTR)SERVICE_NAME, service_main }, { NULL, NULL } };
    if (!StartServiceCtrlDispatcherA(table))
    {
      fprintf(stderr, "sp410-ippd: --service is only for the Windows service manager "
                      "(error %lu)\n", (unsigned long)GetLastError());
      return 1;
    }
    return 0;
  }
  SetConsoleCtrlHandler(console_handler, TRUE);
#else
  if (service)
  {
    fprintf(stderr, "sp410-ippd: --service is only available on Windows\n");
    return 2;
  }
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
#endif

  return run(port_file);
}
