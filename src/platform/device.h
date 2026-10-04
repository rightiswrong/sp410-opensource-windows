/*
 * device.h - Byte transports to the printer.
 *
 * Device URIs:
 *   usb                 first iDPRT (USB VID 20D1) printer, else the only USB printer
 *   usb:any             first USB printer of any vendor
 *   usb:<path>          a specific device (Windows interface path or /dev/usb/lpN)
 *   socket://host[:port]  raw TCP (port 9100 by default), e.g. a print server
 *   serial:COM5[?baud=115200]  serial / Bluetooth SPP (SP410BT); serial:/dev/rfcomm0 on Linux
 *   spool:<queue name>  Windows only: send RAW data through an existing print queue
 *   file:<path>         write each job to a file; "{job}" in the path is replaced
 *                       by the job number (used by the test suite)
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SP410_DEVICE_H
#define SP410_DEVICE_H

#include <stddef.h>

typedef struct device_s device_t;

/* Returns non-zero to abort a blocking write (job cancelled / shutting down). */
typedef int (*device_cancel_fn)(void *ctx);

device_t *device_open(const char *uri, int job_id, char *err, size_t errlen);

/*
 * Write all bytes.  A write that makes no progress for `stall_ms` (printer out
 * of paper, cover open, unplugged) fails.  Returns 0, -1 on error, 1 if cancelled.
 */
int  device_write(device_t *d, const void *buf, size_t len, int stall_ms,
                  device_cancel_fn cancel, void *ctx, char *err, size_t errlen);

/* Read a reply (status queries).  Returns bytes read, 0 on timeout, -1 if unsupported. */
int  device_read(device_t *d, void *buf, size_t len, int timeout_ms);

/* Close; for file: devices this publishes the finished file. Returns 0 or -1. */
int  device_close(device_t *d);

/* Close after a failed or cancelled job: file: devices delete the partial output. */
void device_discard(device_t *d);

/* Human-readable transport name ("USB", "TCP", ...). */
const char *device_kind(const device_t *d);

/* Enumerate local USB printers: calls fn(path, description, ctx) for each. */
typedef void (*device_list_fn)(const char *path, const char *desc, int is_idprt, void *ctx);
int  device_list_usb(device_list_fn fn, void *ctx);

#endif /* SP410_DEVICE_H */
