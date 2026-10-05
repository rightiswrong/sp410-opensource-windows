# sp410-opensource-windows

An open-source Windows driver for the **iDPRT SP410** 4-inch direct-thermal
label printer (and the SP410BT / SP420), with an installer built entirely
from open-source tools.

It is the Windows sibling of
[sp410-cups-driver](https://github.com/rightiswrong/sp410-cups-driver) and
uses the same clean-room conversion code. That code has been confirmed on a
real SP410 under Linux, and it produces byte-identical TSPL output here.
No vendor code, binaries or installers were used or consulted.

> **Status: 1.0.0, confirmed on hardware.** Printing to a physical SP410 over
> USB from Windows 11 works through the installer, the service and Windows'
> own IPP Class Driver. CI also installs, prints and uninstalls on a real
> Windows machine for every commit.

## How it works

Windows no longer wants new third-party printer drivers: since January 2026
Microsoft has stopped publishing new v3/v4 drivers through Windows Update and
steers printers to its in-box **IPP Class Driver**. So instead of a signed
kernel- or user-mode print driver, this project ships a small service that
*is* an IPP Everywhere printer, and lets Windows' own driver print to it:

```text
 Word / browser / shipping app
        │  GDI / XPS
        ▼
 Microsoft IPP Class Driver  (built into Windows, already signed)
        │  PWG Raster pages, 203 dpi, label sizes we advertise
        ▼  HTTP POST http://127.0.0.1:8631/ipp/print
 sp410-ippd.exe  (Windows service "SP410 Open Driver")
        ├─ PWG raster reader (own implementation, no libcups)
        ├─ halftoning, clipping, shift   ┐ same code as the Linux driver
        └─ banded TSPL BITMAP writer     ┘
        │  TSPL
        ▼  usbprint.sys (in-box "USB Printing Support"), or COM / TCP
 iDPRT SP410
```

Nothing is installed in the kernel or the print spooler, so there is no
driver signing, no test mode, and it keeps working with Windows Protected
Print Mode. Details and the alternatives considered:
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Install

1. Plug in the SP410 by USB and switch it on.
2. Download `sp410-opensource-windows-<version>-setup.exe` from
   [Releases](https://github.com/rightiswrong/sp410-opensource-windows/releases/latest).
   Optionally check it against `SHA256SUMS.txt` there
   (`Get-FileHash <file>` in PowerShell).
3. Run it and accept the Windows administrator prompt.
   Windows SmartScreen may warn because the installer is not code-signed;
   choose **More info → Run anyway** (see [docs/INSTALL.md](docs/INSTALL.md#smartscreen)).
4. Print anything to **iDPRT SP410**. The default label size is 4 × 6 in.

Requirements: 64-bit Windows 10 or Windows 11 (Windows 11 recommended, since
Microsoft's IPP support improves with each release). Windows on ARM runs the
x64 build through emulation.

Silent install for fleets: `setup.exe /S`. Full guide, upgrading, Bluetooth
and network printers: [docs/INSTALL.md](docs/INSTALL.md).

## Settings

Windows' print dialog shows the label size, copies and orientation. Printer
options that IPP has no standard name for (darkness, speed, gap or black-mark
media, halftoning, tear-off, offsets) live in one file:

`C:\ProgramData\SP410\sp410.ini` (Start menu → **SP410 Open Driver → Edit printer settings**)

```ini
[device]
uri = usb                     ; or serial:COM5, socket://192.168.1.50:9100

[media]
default = na_index-4x6_4x6in  ; default label size in the print dialog

[print]
Darkness = 10                 ; Default or 0-15
MediaTracking = Gap           ; Gap, BlackMark or Continuous
Dither = Auto                 ; Auto, Threshold, Atkinson, FloydSteinberg, Ordered
```

`[device]`, `[media]` and `[print]` apply to the next label; there is no need
to restart anything. The option names and ranges are the same as in the Linux
driver's PPD. The sample file lists every option.

## Utilities

`sp410-cli.exe` (in `C:\Program Files\SP410 Open Driver`) talks to the
printer without the service:

```bat
sp410-cli list                    :: USB printers Windows can see
sp410-cli status                  :: status 0x00: ready
sp410-cli calibrate               :: measure label + gap after loading a new roll
sp410-cli test-label --size 4x6   :: label drawn with the printer's own fonts
sp410-cli --device serial:COM5 info
sp410-cli convert page.pwg out.prn Darkness=12   :: offline conversion, for debugging
```

The service also has a status page at <http://127.0.0.1:8631/> showing the
printer state, device and recent jobs.

## Build from source

Everything builds on Linux with open-source tools (GCC, MinGW-w64, NSIS):

```sh
sudo apt install build-essential gcc-mingw-w64-x86-64 nsis python3
make check          # native build + 21 end-to-end tests (no printer needed)
make windows        # build/windows-x86_64/sp410-ippd.exe, sp410-cli.exe
make installer      # build/sp410-opensource-windows-1.0.0-setup.exe
```

[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) also works
(`make windows` with its `bin/` on `PATH`; `ARCH=aarch64` for native ARM64).
CI builds, tests under AddressSanitizer and ThreadSanitizer, produces the
installer, then installs it on a real Windows machine and prints through
Windows' IPP Class Driver: [.github/workflows/ci.yml](.github/workflows/ci.yml).

## Documentation

| Document | What's in it |
|---|---|
| [MANIFEST.md](MANIFEST.md) | Every file and where it installs |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Why IPP instead of a classic driver; components; threading |
| [docs/INSTALL.md](docs/INSTALL.md) | Install, upgrade, uninstall, SmartScreen, Bluetooth, network, troubleshooting |
| [docs/HARDWARE.md](docs/HARDWARE.md) | Printer specifications used, models, test status |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | The TSPL commands sent and the IPP attributes advertised |
| [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md) | Clean-room rules, sources, open questions |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Hardware reports, code and test conventions |

## License and trademarks

Apache License 2.0, see [LICENSE](LICENSE). iDPRT is a trademark of its
owner. This project is independent and is not affiliated with, endorsed by,
or supported by iDPRT, HPRT or Microsoft.
