# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Security
- Fixed a heap buffer overflow in the HTTP server's chunked-request handling:
  a crafted Transfer-Encoding: chunked body could overflow the body-length
  accumulator and trigger an out-of-bounds write in the LocalSystem service
  (reachable by any local user on 127.0.0.1, or remotely if shared). Chunk
  sizes are now bounded before allocation. (audit V1)
- Added a configurable concurrent-connection cap ([server] max-connections,
  default 64) so a connection flood or Slowloris cannot exhaust the service.
  Excess connections get an immediate 503. (audit V2)
- The status page now HTML-escapes the client Host header, and all responses
  carry Content-Security-Policy and X-Content-Type-Options: nosniff, removing
  a reflected-XSS vector. (audit V3)
- The file: device scheme is now disabled in the service unless
  [device] allow-file-device = true is set, so a tampered config cannot make
  the SYSTEM service write to an arbitrary path. The CLI still allows file:.
  (audit V4)
- Installer: %ProgramData%\SP410 is now ACL-restricted so only SYSTEM and
  Administrators can modify sp410.ini (Users get read-only); editing settings
  now requires elevation. (audit V4)
- The service logs a clear warning when bound to a non-loopback address
  (unauthenticated network exposure). (audit V6)
- Tightened PWG raster geometry limits (<=10000x40000, <=64 Mpx/page) to bound
  per-page allocations. (audit V11)
- Builds now use -fstack-protector-strong (ASLR/high-entropy/DEP already on;
  Control Flow Guard still depends on the release toolchain). (audit V12)

## [1.0.0] - 2026-10-04

First stable release.

### Verified
- Printing to a physical iDPRT SP410 over USB from a Windows 11 laptop:
  installer, service, the in-box Microsoft IPP Class Driver queue, and
  label output all confirmed working.
- CI installs the release on Windows, prints through the IPP Class Driver,
  decodes the resulting TSPL and uninstalls, on every commit.

### Added
- Release automation: pushing a `v*` tag, or running the CI workflow by hand
  with "publish_release", rebuilds, retests and publishes the installer, a
  portable zip and SHA-256 checksums as a GitHub release.

## [0.1.0] - 2026-10-04 (pre-release, CI only)

### Fixed
- MinGW-w64 GCC builds: printf format checking now matches MinGW's C99
  printf (`%zu`), so the Windows build passes with `-Werror`.
- Log lines are flushed immediately, so none are lost when the service's
  output is a pipe and the process is stopped.

### Added
- `sp410-ippd`: IPP Everywhere printer service for the iDPRT SP410 family.
  Windows prints to it through its in-box Microsoft IPP Class Driver, so no
  third-party print or kernel driver is installed.
  - IPP/2.0 operations: Print-Job, Validate-Job, Create-Job, Send-Document,
    Close-Job, Cancel-Job, Cancel-My-Jobs, Get-Job-Attributes, Get-Jobs,
    Get-Printer-Attributes; HTTP/1.1 with chunked bodies, `Expect: 100-continue`
    and keep-alive.
  - PWG Raster reader (no libcups) for black_1, sgray_8, srgb and CMYK, 1–16 bits.
  - The Linux driver's image pipeline and TSPL writer; output matches it byte
    for byte.
  - Job queue with a worker thread, device retry, stall detection,
    cancellation, and no partial labels on errors.
  - Settings in `%ProgramData%\SP410\sp410.ini`, re-read for every job.
  - Status page at `http://127.0.0.1:8631/`.
  - Windows service (Service Control Manager) and console mode.
- Transports: USB through `usbprint.sys` (SetupAPI), serial/Bluetooth COM,
  raw TCP, RAW through an existing Windows queue, and file.
- `sp410-cli`: list, status, info, test-label, calibrate, self-test,
  feed, home, reset, raw, offline convert, config-init.
- NSIS installer built with open-source tools: service registration with
  restart-on-failure, printer creation through `Add-Printer -IppURL` (with
  fallbacks), Start menu shortcuts, settings kept on upgrade, clean uninstall,
  silent mode.
- 21-case end-to-end test suite with an independent Python IPP client and
  PWG encoder, plus libcups-generated fixtures checked against the Linux filter.
- CI: native, AddressSanitizer/UBSan and ThreadSanitizer test runs,
  MinGW-w64 cross build, NSIS installer, the test suite on Windows, and an
  install → print through Windows → uninstall check on a Windows runner.
