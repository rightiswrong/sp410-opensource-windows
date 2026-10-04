# Manifest

Every file in the repository, what it does, and where the installer puts it.
Keep this in sync when adding, renaming or removing files.

## Installed on Windows

| Source | Purpose | Installed as |
|---|---|---|
| `src/server/*`, `src/core/*`, `src/platform/*` | The IPP printer service (built into one static executable) | `%ProgramFiles%\SP410 Open Driver\sp410-ippd.exe`, service **SP410IPP** ("SP410 Open Driver") |
| `src/cli/main.c` (+ core, platform, `src/server/config.c`) | Printer utility | `%ProgramFiles%\SP410 Open Driver\sp410-cli.exe` |
| `installer/add-printer.ps1` | Creates the "iDPRT SP410" queue on the in-box Microsoft IPP Class Driver | `…\SP410 Open Driver\add-printer.ps1` |
| `installer/remove-printer.ps1` | Removes that queue (only queues pointing at the service) | `…\SP410 Open Driver\remove-printer.ps1` |
| `installer/sp410.ini` | Commented default settings (written by `sp410-cli config-init`) | `%ProgramData%\SP410\sp410.ini` (kept on upgrade) |
| `README.md`, `LICENSE` | Documentation and licence | `…\SP410 Open Driver\README.txt`, `LICENSE.txt` |
| (generated) | Uninstaller | `…\SP410 Open Driver\uninstall.exe`, Apps & features entry |
| (generated) | Start menu: status page, edit settings, test label, calibrate, README, uninstall | `Start → SP410 Open Driver` |
| (runtime) | Service log, reset at 1 MB | `%ProgramData%\SP410\sp410-ippd.log` |

## Source

| Path | Purpose |
|---|---|
| `src/core/pwg.c`, `pwg.h` | PWG Raster (PWG 5102.4) reader; also CUPS v3 rasters; no libcups |
| `src/core/render.c`, `render.h` | Page pipeline: unpack → ink plane → resample → halftone → clip/shift → TSPL; one page buffered at a time |
| `src/core/dither.c`, `dither.h` | Threshold, Atkinson, Floyd–Steinberg, ordered halftoning; Auto photo detection (shared with the Linux driver) |
| `src/core/tspl.c`, `tspl.h` | TSPL writer: per-label setup, band trimming, bit inversion, C-locale numbers (shared with the Linux driver, callback sink) |
| `src/core/settings.c`, `settings.h` | Print options with the Linux PPD's names and ranges |
| `src/core/log.c`, `log.h` | Leveled logging to the console and the service log file |
| `src/platform/compat.c`, `compat.h` | Sockets, threads, condition variables, atomics, time, paths (Win32 + POSIX) |
| `src/platform/device.c`, `device.h` | Transports: USB via `usbprint.sys`/SetupAPI, serial/Bluetooth COM, raw TCP, spooler RAW, file |
| `src/server/http.c`, `http.h` | HTTP/1.1: chunked + Content-Length bodies, `Expect: 100-continue`, keep-alive, lingering close |
| `src/server/ipp.c`, `ipp.h` | IPP/2.0 message parser (with collections) and writer |
| `src/server/server.c`, `server.h` | Printer attributes, job queue, IPP operations, worker thread, status page |
| `src/server/media.c`, `media.h` | 21 label sizes (PWG self-describing names) and custom-size limits |
| `src/server/config.c`, `config.h` | `sp410.ini` reader and commented template |
| `src/server/main.c` | `sp410-ippd` entry point: console mode and Windows service |
| `src/cli/main.c` | `sp410-cli` |

## Build and installer

| Path | Purpose |
|---|---|
| `Makefile` | `make` / `check` (native), `windows` (MinGW-w64 or llvm-mingw), `installer` (NSIS), `dist`, `clean` |
| `VERSION` | Single version number for binaries, resources and installer |
| `installer/sp410.nsi` | NSIS installer: service, printer, shortcuts, uninstaller, silent mode |
| `installer/sp410-ippd.rc`, `sp410-cli.rc` | Windows version resources (built with `windres`) |
| `installer/app.manifest` | Application manifest: `asInvoker`, Windows 10/11, UTF-8 code page |
| `scripts/ci-run.sh` | CI helper: shows the tail of a failing step as an error annotation |
| `.github/workflows/ci.yml` | Linux build + tests + sanitizers + cross build + installer; Windows test suite + install/print/uninstall |

## Tests and tools

| Path | Purpose |
|---|---|
| `tests/run_tests.py` | 21 end-to-end tests against the real `sp410-ippd`/`sp410-cli` binaries (Linux or Windows) |
| `tests/ippclient.py` | Independent IPP client used by the tests |
| `tests/pwgraster.py` | Independent PWG Raster encoder used by the tests |
| `tests/e2e-windows.ps1` | Windows end-to-end: silent install, service, IPP Class Driver queue, print from Windows, uninstall |
| `tests/fixtures/cups-pwg-4x2.ras`, `cups-v3-rgb.ras` | Rasters written by libcups (from sp410-cups-driver's test generator) |
| `tests/fixtures/*.linux.prn` | The Linux filter's TSPL output for those rasters; must match byte for byte |
| `tools/tspl_decode.py` | Parse, render (PBM/PNG) and diff TSPL streams (shared with the Linux driver) |

## Documentation and metadata

| Path | Purpose |
|---|---|
| `README.md` | Overview, install, settings, utilities, build |
| `MANIFEST.md` | This file |
| `docs/ARCHITECTURE.md` | Why IPP instead of a classic driver; components; request flow; safety |
| `docs/INSTALL.md` | Install, upgrade, uninstall, SmartScreen, manual printer setup, Bluetooth/network, troubleshooting |
| `docs/HARDWARE.md` | Spec-sheet values used and where; models and test status; how to report |
| `docs/PROTOCOL.md` | TSPL sent to the printer; IPP operations and attributes offered to Windows |
| `docs/REVERSE_ENGINEERING.md` | Clean-room rules, sources, open questions |
| `CONTRIBUTING.md` | Hardware reports, code rules, checks before a pull request |
| `CHANGELOG.md` | Release notes |
| `LICENSE` | Apache License 2.0 |
| `.gitignore`, `.editorconfig` | Repository hygiene; vendor material and print captures are ignored |
