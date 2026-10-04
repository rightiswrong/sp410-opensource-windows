# Architecture

## The decision: an IPP printer, not a classic print driver

Three ways to make an unsupported printer work on Windows were considered.

| Option | What it takes | Why it was not chosen / chosen |
|---|---|---|
| **Classic v3 driver** (Unidrv GPD minidriver + rendering plug-in DLL) | A GPD file plus a C++ COM plug-in to invert bits and wrap TSPL `BITMAP` headers; an INF and a **signed catalog** (`.cat`) | 64-bit Windows refuses to install an unsigned driver package. Signing needs Microsoft's Inf2Cat/SignTool and either a paid certificate or asking every user to trust a self-made one. Microsoft stopped publishing new third-party v3/v4 drivers through Windows Update on 15 January 2026, and Windows Protected Print Mode removes such queues. |
| **v4 driver** (XPS-based, GPD/PDC + JavaScript constraints) | Same signing problem; v4 is also covered by the third-party driver end-of-servicing plan | Not chosen for the same reasons. |
| **IPP Everywhere printer + Windows' in-box IPP Class Driver** | A user-mode service that speaks IPP and accepts PWG Raster | **Chosen.** Microsoft's own signed driver does all the Windows-side work; our code never enters the spooler or kernel; works with Protected Print Mode; the same direction Microsoft is pushing every printer. |

The trade-off: options that IPP has no standard attribute for (darkness,
speed, gap/black mark, halftoning) are not in the print dialog. They are set
in `sp410.ini` instead, which is also how the Linux driver's queue defaults work.

## Components

```text
src/core/        portable conversion library (no OS calls)
  pwg.c            PWG Raster reader: 1796-byte header, line-repeat + PackBits-style runs
  render.c         page -> 8-bit ink plane -> resample -> halftone -> clip/shift -> TSPL
  dither.c         threshold, Atkinson, Floyd-Steinberg, ordered (shared with Linux)
  tspl.c           banded, trimmed BITMAP writer (shared with Linux)
  settings.c       option parsing (same names/ranges as the Linux PPD)
src/server/      the IPP printer
  http.c           HTTP/1.1: Content-Length + chunked bodies, Expect: 100-continue, keep-alive
  ipp.c            RFC 8010 encoding: parser (incl. collections) and response writer
  server.c         printer attributes, job queue, operations, worker thread, status page
  config.c         sp410.ini
  media.c          21 label sizes with PWG self-describing names + custom-size ranges
  main.c           console mode and Windows service (Service Control Manager)
src/platform/    OS layer
  compat.c         sockets, threads, condition variables, time, paths (Win32 + POSIX)
  device.c         printer transports: usbprint, COM, TCP, spooler RAW, file
src/cli/main.c   sp410-cli
```

## Request flow

1. Windows sends `Get-Printer-Attributes`. We answer as an IPP Everywhere
   printer: `document-format-supported = image/pwg-raster`,
   `pwg-raster-document-resolution-supported = 203dpi`,
   `pwg-raster-document-type-supported = black_1, sgray_8`, zero margins,
   21 label sizes plus a 20–108 × 10–300 mm custom range, `color-supported = false`.
2. On print, Windows renders each page to PWG Raster at that size and
   resolution and sends `Print-Job`, or `Create-Job` + `Send-Document`,
   often with chunked transfer encoding.
3. The connection thread stores the document and queues the job; the
   response returns at once.
4. The single worker thread re-reads `sp410.ini`, opens the printer
   (retrying for `retry-timeout` seconds if it is off or unplugged), converts
   page by page, and writes the TSPL. Each page is converted completely
   before any of it is sent, so a corrupt page never leaves a half-printed label.
5. Windows polls `Get-Job-Attributes` and shows the job as printed, failed
   (with the reason in `job-state-message`) or cancelled.

## Talking to the USB printer without a driver

Every USB printer-class device, the SP410 included, is bound by Windows to
`usbprint.sys` ("USB Printing Support"), which ships with Windows. It exposes a
device interface (`GUID_DEVINTERFACE_USBPRINT`). `device.c` finds it with
SetupAPI, preferring USB vendor ID `20D1`, opens it with `CreateFile`, and
writes with overlapped I/O. That lets it notice a cancelled job or a printer
that stops accepting data (out of paper, cover open) without hanging.

Other transports: `serial:COM5` (SP410BT over Bluetooth SPP), `socket://host:9100`
(network print servers), `spool:<queue>` (RAW through an existing Windows
queue, a fallback if direct USB access is blocked), `file:` (tests).

## Threads and safety

- One thread per HTTP connection, one worker thread, the accept loop.
  All job and printer state is behind one mutex. The stop and cancel flags
  are atomics. CI runs the full test suite under ThreadSanitizer.
- Listens on `127.0.0.1` only by default, so other computers cannot print
  unless `listen = 0.0.0.0` is set on purpose.
- Bounded input: request bodies are capped (`max-job-mb`, 413 response),
  headers are capped, IPP collections are depth-limited, PWG geometry is
  validated before any allocation, and every run length is bounds-checked.
  CI also runs the tests under AddressSanitizer and UBSan.
- The service runs as LocalSystem (the Windows default for services) because
  it must open the USB printer. It writes only to `%ProgramData%\SP410`.

## Why the output matches the Linux driver exactly

`dither.c` and `tspl.c` are the Linux driver's files with only the output
sink changed (a callback instead of `FILE *`). `render.c` is the Linux filter's
page pipeline on top of the new PWG reader. During development, 36
format × option combinations of libcups-written rasters were converted by
both and compared byte for byte. Two of them are kept as test fixtures
(`tests/fixtures/*.linux.prn`) so any drift fails the build.
