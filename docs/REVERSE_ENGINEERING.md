# Clean-room record

## Rules this project follows

1. **No vendor code.** iDPRT's Windows driver (listed on its downloads page
   as v2.7.4.23), Linux/Raspberry Pi packages and label software were not
   downloaded, run, disassembled or read. Nothing from them is in this
   repository.
2. **Allowed inputs:** iDPRT's public product specifications (see
   [HARDWARE.md](HARDWARE.md)), the publicly documented TSPL command language,
   public IPP and PWG standards, and this project's own Linux driver.
3. **Record findings in your own words.** If a contributor learns something
   by observing the vendor driver (for example a USB capture of what it
   sends), they document the behaviour in an issue. Code is written from
   that description, not from the observation.
4. Never commit vendor material. `vendor/`, `vendor-inspect/`, captures
   (`*.pcap`, `*.pcapng`) and print streams (`*.prn`, except test fixtures we
   generated ourselves) are git-ignored.

## Sources

| What | Source |
|---|---|
| SP410 resolution, width, length, speed, sensors, fonts, memory, TSPL | iDPRT SP410 product page (public spec sheet) |
| TSPL commands, `BITMAP` encoding (1 = white), status byte | TSC TSPL/TSPL2 programming manuals (public) |
| Behaviour of the conversion on real hardware | sp410-cups-driver, confirmed on an SP410 with a Raspberry Pi 3B+ |
| IPP/2.0 encoding and operations | RFC 8010, RFC 8011 |
| IPP Everywhere attributes, PWG Raster format, media names | PWG 5100.14, PWG 5102.4, PWG 5101.1 |
| Windows printing to IPP printers (`Add-Printer -IppURL`, IPP Class Driver) | Microsoft documentation and Microsoft Q&A |
| Microsoft's third-party driver end-of-servicing timeline | Microsoft announcement (Sept 2023) and its 2026 follow-ups as reported by the Windows press |
| USB printer access from user mode (`GUID_DEVINTERFACE_USBPRINT`) | Microsoft Windows SDK headers and documentation |

## What carried over from the Linux work

The Linux driver established, and real hardware confirmed, that the SP410
prints correctly from:

```text
SIZE w mm,h mm / GAP g mm,0 mm / DIRECTION 0 / REFERENCE 0,0 / SET TEAR ON / CLS
BITMAP x,y,wbytes,h,0,<inverted rows> ...
PRINT 1,copies
```

The Windows driver emits exactly that stream; the shared code and fixtures
guarantee it (see [ARCHITECTURE.md](ARCHITECTURE.md#why-the-output-matches-the-linux-driver-exactly)).

## Open questions

| # | Question | Assumption now | How to settle it |
|---|---|---|---|
| 1 | Does Windows bind `usbprint.sys` to every SP410 revision? | Yes (it is a USB printer-class device) | `sp410-cli list` on real units |
| 2 | Does `usbprint` return the `ESC !?` status byte on this printer? | Probably (bidirectional printer class) | `sp410-cli status` over USB |
| 3 | Product ID on Windows | `20D1:7008` (community tables) | Device Manager → Hardware Ids |
| 4 | Do all Windows 10 builds accept `Add-Printer -IppURL`? | Windows 11 yes; older builds use the fallback that names the IPP Class Driver, or the manual steps | Reports from Windows 10 users |
| 5 | Does Windows' IPP Class Driver honour `copies`? | Older versions ignore it; then copies come out as repeated pages | Print 3 copies, compare labels vs jobs |
| 6 | SP410BT Bluetooth baud rate | 115200 (SPP usually ignores it) | Report from an SP410BT owner |
| 7 | Status bit 6 meaning on SP410 | "cover open / model-specific" | Open the cover, `sp410-cli status` |
