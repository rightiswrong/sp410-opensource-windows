# Hardware

## Specifications used by this driver

Taken from iDPRT's public SP410 product page. These are the only
manufacturer materials used; no vendor driver was downloaded or examined.

| Spec sheet | Value | Where it is used |
|---|---|---|
| Printing method | Direct thermal | No ribbon; status bit 3 ("out of ribbon") is not expected |
| Resolution | 203 dpi (8 dots/mm) | `pwg-raster-document-resolution-supported`; TSPL `mm` → dots at 8/mm |
| Max print width | 108 mm | 864-dot clip; custom size range 20–108 mm wide |
| Max print length | 300 mm | 2400-dot clip; custom size range up to 300 mm |
| Max speed | 150 mm/s | `PrintSpeed` 2–6 in/s (6 in/s ≈ 152 mm/s) |
| Command language | TSPL | Everything in [PROTOCOL.md](PROTOCOL.md) |
| Sensors | gap, black mark, label edge, out of paper, cover open | `MediaTracking` Gap / BlackMark / Continuous; `calibrate`; status bits |
| Resident fonts | 8×16 and 12×24 bitmap, 4 rotations | Used only by `sp410-cli test-label` |
| Memory | 2 MB RAM, 2 MB flash | Each label is sent as trimmed bands (a 4 × 6 in label is about 120 KB at most) |
| Interfaces | USB (SP410), USB + Bluetooth (SP410BT) | `usb`, `serial:COMn` transports |

## Models and test status

| Model | Windows status | Linux status (same conversion code) |
|---|---|---|
| iDPRT SP410 | install and print path verified on Windows in CI (IPP Class Driver → service → TSPL); **awaiting a physical-printer report** | **confirmed working** (Raspberry Pi 3B+, sp410-cups-driver) |
| iDPRT SP410BT | protocol-compatible, awaiting report (USB or Bluetooth COM) | awaiting report |
| iDPRT SP420 | protocol-compatible, awaiting report | other open TSPL drivers report it working |

Set `model = SP410BT` or `SP420` under `[server]` in `sp410.ini` to change the
model name Windows shows. The output is the same for all three.

## Reporting your printer

Open an issue with:

1. Windows version (`winver`) and whether it is x64 or ARM64.
2. `sp410-cli list` output, and the printer's hardware ID from Device Manager
   (Universal Serial Bus devices or Printers → Properties → Details → Hardware Ids).
3. `sp410-cli status` and `sp410-cli info`.
4. Whether **Print a test label** and a real print from an app worked, plus
   the label size and media (gap or black mark).
5. The last 50 lines of `C:\ProgramData\SP410\sp410-ippd.log` if anything failed.
