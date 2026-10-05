# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

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
