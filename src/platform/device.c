/*
 * device.c - Printer transports (USB, TCP, serial, spooler, file).
 *
 * Windows USB access goes through usbprint.sys, the in-box "USB Printing
 * Support" driver that Windows binds to every USB printer-class device,
 * including the SP410.  Its device interface (GUID_DEVINTERFACE_USBPRINT) is
 * opened with CreateFile and written with overlapped WriteFile, so no
 * kernel-mode code or signed driver package is needed.
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "device.h"
#include "compat.h"
#include "../core/log.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <setupapi.h>
#  include <winspool.h>
#else
#  include <dirent.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <termios.h>
#  include <unistd.h>
#endif

typedef enum { DEV_USB, DEV_TCP, DEV_SERIAL, DEV_SPOOL, DEV_FILE } dev_kind_t;

struct device_s
{
  dev_kind_t kind;
#ifdef _WIN32
  HANDLE     h;            /* USB / serial */
  HANDLE     printer;      /* spool */
#else
  int        fd;           /* USB / serial */
#endif
  sock_t     sock;         /* TCP */
  FILE      *fp;           /* file */
  char       final_path[1024], temp_path[1040];
};

const char *
device_kind(const device_t *d)
{
  static const char *const names[] = { "USB", "TCP", "serial", "spooler", "file" };
  return names[d->kind];
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 4)))
#endif
static void
seterr(char *err, size_t errlen, const char *fmt, ...)
{
  va_list ap;

  if (!err || !errlen)
    return;
  va_start(ap, fmt);
  vsnprintf(err, errlen, fmt, ap);
  va_end(ap);
}

/* ======================================================================== */
#ifdef _WIN32

static int
contains_ci(const char *hay, const char *needle)
{
  size_t n = strlen(needle);

  for (; *hay; hay ++)
    if (!strncasecmp(hay, needle, n))
      return 1;
  return 0;
}

static const GUID GUID_USBPRINT =
  { 0x28d78fad, 0x5a12, 0x11d1, { 0xae, 0x5b, 0x00, 0x00, 0xf8, 0x03, 0xa8, 0xc2 } };

static const char *
win_err(void)
{
  static __thread char buf[256];
  DWORD e = GetLastError();
  int   n;

  n = snprintf(buf, sizeof(buf), "error %lu: ", (unsigned long)e);
  if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                      e, 0, buf + n, (DWORD)(sizeof(buf) - (size_t)n), NULL))
    buf[n] = '\0';
  for (char *p = buf + strlen(buf); p > buf && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' '); )
    *--p = '\0';
  return buf;
}

int
device_list_usb(device_list_fn fn, void *ctx)
{
  HDEVINFO                  set;
  SP_DEVICE_INTERFACE_DATA  ifd;
  int                       count = 0;

  set = SetupDiGetClassDevsA(&GUID_USBPRINT, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE)
    return -1;

  ifd.cbSize = sizeof(ifd);
  for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &GUID_USBPRINT, i, &ifd); i ++)
  {
    DWORD                             need = 0;
    SP_DEVICE_INTERFACE_DETAIL_DATA_A *det;
    SP_DEVINFO_DATA                   dev;
    char                              desc[256] = "";

    SetupDiGetDeviceInterfaceDetailA(set, &ifd, NULL, 0, &need, NULL);
    if (!need || (det = malloc(need)) == NULL)
      continue;
    det->cbSize = sizeof(*det);
    dev.cbSize  = sizeof(dev);
    if (SetupDiGetDeviceInterfaceDetailA(set, &ifd, det, need, NULL, &dev))
    {
      if (!SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_FRIENDLYNAME, NULL,
                                             (PBYTE)desc, sizeof(desc), NULL))
        SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_DEVICEDESC, NULL,
                                          (PBYTE)desc, sizeof(desc), NULL);
      fn(det->DevicePath, desc, contains_ci(det->DevicePath, "vid_20d1"), ctx);
      count ++;
    }
    free(det);
  }
  SetupDiDestroyDeviceInfoList(set);
  return count;
}

typedef struct { char idprt[1024], first[1024]; int n; } pick_t;

static void
pick_cb(const char *path, const char *desc, int is_idprt, void *ctx)
{
  pick_t *p = ctx;
  (void)desc;
  if (is_idprt && !p->idprt[0])
    snprintf(p->idprt, sizeof(p->idprt), "%s", path);
  if (!p->first[0])
    snprintf(p->first, sizeof(p->first), "%s", path);
  p->n ++;
}

static int
open_usb(device_t *d, const char *spec, char *err, size_t errlen)
{
  char   path[1024];
  pick_t p;

  memset(&p, 0, sizeof(p));
  if (!*spec || !strcmp(spec, "auto") || !strcmp(spec, "any"))
  {
    if (device_list_usb(pick_cb, &p) < 0)
    {
      seterr(err, errlen, "cannot enumerate USB printers (%s)", win_err());
      return -1;
    }
    if (p.idprt[0])
      snprintf(path, sizeof(path), "%s", p.idprt);
    else if (p.n == 1 || (p.n > 0 && !strcmp(spec, "any")))
      snprintf(path, sizeof(path), "%s", p.first);
    else
    {
      if (p.n)
        seterr(err, errlen, "several USB printers found and none is an iDPRT (VID 20D1); "
                            "set device = usb:<path> (see sp410-cli list)");
      else
        seterr(err, errlen, "no USB printer connected (is the SP410 plugged in and switched on?)");
      return -1;
    }
  }
  else
    snprintf(path, sizeof(path), "%s", spec);

  d->h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
  if (d->h == INVALID_HANDLE_VALUE)
  {
    seterr(err, errlen, "cannot open USB printer (%s)", win_err());
    return -1;
  }
  log_msg(LOG_DEBUG, "opened USB printer %s", path);
  return 0;
}

static int
open_serial(device_t *d, const char *spec, char *err, size_t errlen)
{
  char         port[256], path[300];
  const char  *q = strchr(spec, '?');
  long         baud = 115200;
  DCB          dcb;
  COMMTIMEOUTS to;

  snprintf(port, sizeof(port), "%.*s", q ? (int)(q - spec) : (int)strlen(spec), spec);
  if (q && !strncmp(q + 1, "baud=", 5))
    baud = strtol(q + 6, NULL, 10);
  snprintf(path, sizeof(path), "\\\\.\\%s", port);

  d->h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                     FILE_FLAG_OVERLAPPED, NULL);
  if (d->h == INVALID_HANDLE_VALUE)
  {
    seterr(err, errlen, "cannot open serial port (%s)", win_err());
    return -1;
  }
  memset(&dcb, 0, sizeof(dcb));
  dcb.DCBlength = sizeof(dcb);
  GetCommState(d->h, &dcb);
  dcb.BaudRate = (DWORD)baud;
  dcb.ByteSize = 8;
  dcb.Parity   = NOPARITY;
  dcb.StopBits = ONESTOPBIT;
  dcb.fBinary  = TRUE;
  SetCommState(d->h, &dcb);
  memset(&to, 0, sizeof(to));
  to.ReadIntervalTimeout = MAXDWORD;
  SetCommTimeouts(d->h, &to);
  return 0;
}

static int
open_spool(device_t *d, const char *name, char *err, size_t errlen)
{
  DOC_INFO_1A di;

  if (!OpenPrinterA((LPSTR)name, &d->printer, NULL))
  {
    seterr(err, errlen, "cannot open print queue (%s)", win_err());
    return -1;
  }
  di.pDocName    = (LPSTR)"SP410 label";
  di.pOutputFile = NULL;
  di.pDatatype   = (LPSTR)"RAW";
  if (!StartDocPrinterA(d->printer, 1, (LPBYTE)&di))
  {
    seterr(err, errlen, "cannot start RAW job (%s)", win_err());
    ClosePrinter(d->printer);
    return -1;
  }
  return 0;
}

/* Overlapped WriteFile in slices so cancellation and stalls are noticed. */
static int
handle_write(device_t *d, const uint8_t *p, size_t len, int stall_ms,
             device_cancel_fn cancel, void *ctx, char *err, size_t errlen)
{
  OVERLAPPED ov;
  HANDLE     ev = CreateEventA(NULL, TRUE, FALSE, NULL);
  int        rc = 0;

  if (!ev)
    return -1;
  while (len && rc == 0)
  {
    DWORD   chunk = len > 65536 ? 65536 : (DWORD)len, done = 0;
    int64_t start = time_ms();

    memset(&ov, 0, sizeof(ov));
    ov.hEvent = ev;
    ResetEvent(ev);
    if (!WriteFile(d->h, p, chunk, NULL, &ov) && GetLastError() != ERROR_IO_PENDING)
    {
      seterr(err, errlen, "write failed (%s)", win_err());
      rc = -1;
      break;
    }
    for (;;)
    {
      DWORD w = WaitForSingleObject(ev, 250);

      if (w == WAIT_OBJECT_0)
      {
        if (!GetOverlappedResult(d->h, &ov, &done, FALSE))
        {
          seterr(err, errlen, "write failed (%s)", win_err());
          rc = -1;
        }
        break;
      }
      if ((cancel && cancel(ctx)) || time_ms() - start > stall_ms)
      {
        CancelIoEx(d->h, &ov);
        GetOverlappedResult(d->h, &ov, &done, TRUE);
        if (cancel && cancel(ctx))
          rc = 1;
        else
        {
          seterr(err, errlen, "printer not accepting data (out of paper, cover open "
                              "or disconnected?)");
          rc = -1;
        }
        break;
      }
    }
    p   += done;
    len -= done;
  }
  CloseHandle(ev);
  return rc;
}

static int
handle_read(device_t *d, void *buf, size_t len, int timeout_ms)
{
  OVERLAPPED ov;
  DWORD      got = 0;
  HANDLE     ev = CreateEventA(NULL, TRUE, FALSE, NULL);

  if (!ev)
    return -1;
  memset(&ov, 0, sizeof(ov));
  ov.hEvent = ev;
  if (!ReadFile(d->h, buf, (DWORD)len, NULL, &ov) && GetLastError() != ERROR_IO_PENDING)
  {
    CloseHandle(ev);
    return -1;
  }
  if (WaitForSingleObject(ev, (DWORD)timeout_ms) != WAIT_OBJECT_0)
    CancelIoEx(d->h, &ov);
  GetOverlappedResult(d->h, &ov, &got, TRUE);
  CloseHandle(ev);
  return (int)got;
}

/* ======================================================================== */
#else  /* POSIX (used for Linux builds and the test suite) */

int
device_list_usb(device_list_fn fn, void *ctx)
{
  DIR           *dir = opendir("/dev/usb");
  struct dirent *de;
  int            n = 0;

  if (!dir)
    return 0;
  while ((de = readdir(dir)) != NULL)
    if (!strncmp(de->d_name, "lp", 2) || !strncmp(de->d_name, "idprt", 5))
    {
      char path[300];
      snprintf(path, sizeof(path), "/dev/usb/%s", de->d_name);
      fn(path, de->d_name, !strncmp(de->d_name, "idprt", 5), ctx);
      n ++;
    }
  closedir(dir);
  return n;
}

static int
open_fd(device_t *d, const char *path, char *err, size_t errlen)
{
  if ((d->fd = open(path, O_RDWR | O_NOCTTY)) < 0 &&
      (d->fd = open(path, O_WRONLY | O_NOCTTY)) < 0)
  {
    seterr(err, errlen, "cannot open device: %s", strerror(errno));
    return -1;
  }
  return 0;
}

static int
open_usb(device_t *d, const char *spec, char *err, size_t errlen)
{
  const char *path = spec;

  if (!*spec || !strcmp(spec, "auto") || !strcmp(spec, "any"))
    path = access("/dev/usb/idprt-sp410-0", F_OK) == 0 ? "/dev/usb/idprt-sp410-0" : "/dev/usb/lp0";
  return open_fd(d, path, err, errlen);
}

static int
open_serial(device_t *d, const char *spec, char *err, size_t errlen)
{
  char           path[256];
  const char    *q = strchr(spec, '?');
  struct termios tio;

  snprintf(path, sizeof(path), "%.*s", q ? (int)(q - spec) : (int)strlen(spec), spec);
  if (open_fd(d, path, err, errlen))
    return -1;
  if (tcgetattr(d->fd, &tio) == 0)
  {
    long baud = q && !strncmp(q + 1, "baud=", 5) ? strtol(q + 6, NULL, 10) : 115200;
    cfmakeraw(&tio);
    cfsetspeed(&tio, baud == 9600 ? B9600 : baud == 19200 ? B19200 :
                     baud == 38400 ? B38400 : baud == 57600 ? B57600 : B115200);
    tcsetattr(d->fd, TCSANOW, &tio);
  }
  return 0;
}

static int
handle_write(device_t *d, const uint8_t *p, size_t len, int stall_ms,
             device_cancel_fn cancel, void *ctx, char *err, size_t errlen)
{
  int64_t last = time_ms();

  while (len)
  {
    struct pollfd pfd = { d->fd, POLLOUT, 0 };
    ssize_t       n;

    if (cancel && cancel(ctx))
      return 1;
    if (poll(&pfd, 1, 250) <= 0)
    {
      if (time_ms() - last > stall_ms)
      {
        seterr(err, errlen, "printer not accepting data");
        return -1;
      }
      continue;
    }
    n = write(d->fd, p, len > 65536 ? 65536 : len);
    if (n < 0)
    {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      seterr(err, errlen, "write failed: %s", strerror(errno));
      return -1;
    }
    p    += n;
    len  -= (size_t)n;
    last  = time_ms();
  }
  return 0;
}

static int
handle_read(device_t *d, void *buf, size_t len, int timeout_ms)
{
  struct pollfd pfd = { d->fd, POLLIN, 0 };
  ssize_t       n;

  if (poll(&pfd, 1, timeout_ms) <= 0)
    return 0;
  n = read(d->fd, buf, len);
  return n < 0 ? -1 : (int)n;
}

#endif /* _WIN32 */
/* ======================================================================== */

device_t *
device_open(const char *uri, int job_id, char *err, size_t errlen)
{
  device_t *d = calloc(1, sizeof(*d));
  int       rc;

  if (!d)
    return NULL;
  d->sock = SOCK_INVALID;
#ifdef _WIN32
  d->h = INVALID_HANDLE_VALUE;
#else
  d->fd = -1;
#endif

  if (!uri || !*uri || !strcmp(uri, "usb") || !strncmp(uri, "usb:", 4))
  {
    const char *spec = (!uri || strlen(uri) <= 4) ? "" : uri + 4;
    if (!strncmp(spec, "//", 2))
      spec += 2;
    d->kind = DEV_USB;
    rc = open_usb(d, spec, err, errlen);
  }
  else if (!strncmp(uri, "socket://", 9))
  {
    char host[256], *colon;
    int  port = 9100;

    snprintf(host, sizeof(host), "%s", uri + 9);
    host[strcspn(host, "/")] = '\0';
    if ((colon = strrchr(host, ':')) != NULL && !strchr(colon, ']'))
    {
      port   = atoi(colon + 1);
      *colon = '\0';
    }
    d->kind = DEV_TCP;
    d->sock = net_connect(host, port, 5000);
    if (d->sock == SOCK_INVALID)
    {
      seterr(err, errlen, "cannot connect to %s", uri);
      rc = -1;
    }
    else
      rc = 0;
  }
  else if (!strncmp(uri, "serial:", 7))
  {
    d->kind = DEV_SERIAL;
    rc = open_serial(d, uri + 7, err, errlen);
  }
#ifdef _WIN32
  else if (!strncmp(uri, "spool:", 6))
  {
    d->kind = DEV_SPOOL;
    rc = open_spool(d, uri + 6, err, errlen);
  }
#endif
  else if (!strncmp(uri, "file:", 5))
  {
    const char *tmpl = uri + 5, *mark = strstr(tmpl, "{job}");

    d->kind = DEV_FILE;
    if (mark)
      snprintf(d->final_path, sizeof(d->final_path), "%.*s%d%s",
               (int)(mark - tmpl), tmpl, job_id, mark + 5);
    else
      snprintf(d->final_path, sizeof(d->final_path), "%s", tmpl);
    snprintf(d->temp_path, sizeof(d->temp_path), "%s.part", d->final_path);
    d->fp = fopen(d->temp_path, "wb");
    if (!d->fp)
    {
      seterr(err, errlen, "cannot create output file: %s", strerror(errno));
      rc = -1;
    }
    else
      rc = 0;
  }
  else
  {
    seterr(err, errlen, "unsupported device URI \"%s\"", uri);
    rc = -1;
  }

  if (rc)
  {
    free(d);
    return NULL;
  }
  return d;
}

int
device_write(device_t *d, const void *buf, size_t len, int stall_ms,
             device_cancel_fn cancel, void *ctx, char *err, size_t errlen)
{
  switch (d->kind)
  {
    case DEV_FILE:
      if (fwrite(buf, 1, len, d->fp) != len)
      {
        seterr(err, errlen, "write failed: %s", strerror(errno));
        return -1;
      }
      return 0;
    case DEV_TCP:
      if (cancel && cancel(ctx))
        return 1;
      if (net_send_all(d->sock, buf, len))
      {
        seterr(err, errlen, "network write failed (%s)", net_strerror());
        return -1;
      }
      return 0;
#ifdef _WIN32
    case DEV_SPOOL:
    {
      const uint8_t *p = buf;
      while (len)
      {
        DWORD n = 0;
        if (cancel && cancel(ctx))
          return 1;
        if (!WritePrinter(d->printer, (LPVOID)p, len > 65536 ? 65536 : (DWORD)len, &n) || !n)
        {
          seterr(err, errlen, "spooler write failed (%s)", win_err());
          return -1;
        }
        p   += n;
        len -= n;
      }
      return 0;
    }
#endif
    default:
      return handle_write(d, buf, len, stall_ms, cancel, ctx, err, errlen);
  }
}

int
device_read(device_t *d, void *buf, size_t len, int timeout_ms)
{
  switch (d->kind)
  {
    case DEV_USB:
    case DEV_SERIAL:
      return handle_read(d, buf, len, timeout_ms);
    case DEV_TCP:
    {
      int n = net_recv(d->sock, buf, len, timeout_ms);
      return n < 0 ? 0 : n;
    }
    default:
      return -1;
  }
}

void
device_discard(device_t *d)
{
  if (d && d->kind == DEV_FILE)
  {
    fclose(d->fp);
    remove(d->temp_path);
    free(d);
    return;
  }
  device_close(d);
}

int
device_close(device_t *d)
{
  int rc = 0;

  if (!d)
    return 0;
  switch (d->kind)
  {
    case DEV_FILE:
      if (fclose(d->fp))
        rc = -1;
      remove(d->final_path);
      if (rc == 0 && rename(d->temp_path, d->final_path))
        rc = -1;
      break;
    case DEV_TCP:
      net_close(d->sock);
      break;
#ifdef _WIN32
    case DEV_SPOOL:
      EndDocPrinter(d->printer);
      ClosePrinter(d->printer);
      break;
    default:
      if (d->h != INVALID_HANDLE_VALUE)
        CloseHandle(d->h);
      break;
#else
    default:
      if (d->fd >= 0)
        close(d->fd);
      break;
#endif
  }
  free(d);
  return rc;
}
