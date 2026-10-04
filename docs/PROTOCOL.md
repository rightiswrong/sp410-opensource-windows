# SP410 wire protocol (TSPL subset)

Part 1 is the subset of TSPL/TSPL2 sent to the printer. It is identical to the
Linux driver's output, and the CLI's status and utility commands are covered here
too. Part 2 is the IPP Everywhere surface that Windows talks to.

# Part 1: TSPL (to the printer)

This is the subset of TSPL/TSPL2 that `sp410-ippd` emits, plus the
utility commands `sp410-cli` uses. It is written from the public TSPL
programming references and checked against our own decoder; items not yet
confirmed on real SP410 hardware are marked **unverified** and tracked in
[REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md#open-questions).

## Transport

TSPL is transport-agnostic: the same bytes work over every link.

| Link | Windows | `[device] uri` |
|---|---|---|
| USB (printer class) | `usbprint.sys` device interface `{28D78FAD-5A12-11D1-AE5B-0000F803A8C2}` | `usb` |
| Bluetooth SPP (SP410BT) | outgoing virtual COM port | `serial:COM5?baud=115200` |
| Network print server | raw TCP | `socket://host:9100` |

USB ID reported by community tables: `20d1:7008` (vendor 20D1 is used to
pick the printer automatically; the product ID is **unverified** on Windows).

## Units and geometry

* Head: 203 dpi; TSPL treats this as exactly **8 dots/mm** when an argument
  is given in `mm`. Without a unit, sizes are inches.
* Printable width: 108 mm = **864 dots** (spec sheet). Max length: 300 mm =
  2400 dots (spec sheet; firmware limit **unverified**).
* Origin (0,0) is the top-left of the label as it leaves the printer with
  `DIRECTION 0`.

## Line format

ASCII commands, one per line, terminated by `CR LF`. Arguments are
comma-separated. Decimal numbers always use `.` — the converter never calls
`setlocale()`, so a system locale such as `de_DE` cannot turn `101.6` into
`101,6` (which the printer would parse as two arguments).

## Job emitted per label

```text
SIZE 101.6 mm,152.4 mm          label width, label length
GAP 3 mm,0 mm                   or  BLINE 3 mm,0 mm  (black mark)  or  GAP 0,0  (continuous)
DIRECTION 0                     0 = normal, 1 = rotated 180°
REFERENCE 0,0                   image origin; positioning is done in the bitmap
OFFSET -2 mm                    only when TearOffset ≠ 0
SPEED 4                         only when PrintSpeed ≠ Printer Default (ips)
DENSITY 12                      only when Darkness ≠ Printer Default (0–15)
SET TEAR ON                     advance the label to the tear bar after printing
CLS                             clear the image buffer
BITMAP 8,10,37,90,0,<3330 raw bytes>      one band per inked region,
BITMAP 8,120,49,5,0,<245 raw bytes>       trimmed left/right/top/bottom
PRINT 1,1                       1 set × N copies (N from the raster header)
```

Setup is re-sent for every label so that mixed page sizes in one job work
and so that a job never depends on state left by a previous one. Darkness
and speed are deliberately *not* sent when set to "Printer Default", so a
value configured with the vendor utility or the printer's own menu wins.

## BITMAP

```text
BITMAP x,y,width_bytes,height,mode,<width_bytes × height raw bytes>
```

| Field | Meaning |
|---|---|
| `x`, `y` | top-left position in dots (`x` is always a multiple of 8 in our output) |
| `width_bytes` | bytes per row (8 dots per byte) |
| `height` | rows |
| `mode` | `0` overwrite, `1` OR, `2` XOR — we always use `0` |
| data | rows top-to-bottom, MSB = leftmost dot, **bit 1 = white, bit 0 = black** |

The polarity is the opposite of PBM and of PWG `black_1` rasters, so the converter
inverts every byte on output. Padding bits past the right edge of the image
are sent as `1` (white). Data follows the 5th comma immediately — there is no
length prefix and no line terminator inside it; the trailing `CR LF` after the
data is ignored as whitespace.

Worked example — a 10-dot row, █ = black:

```text
dots          █ █ ░ ░ █ █ █ █ █ █ | (6 padding dots)
black=1       1 1 0 0 1 1 1 1   1 1 0 0 0 0 0 0   -> CF C0   (PBM / CUPS K order)
TSPL bytes    0 0 1 1 0 0 0 0   0 0 1 1 1 1 1 1   -> 30 3F   (inverted, padding white)
```

### Band splitting

A 4×6 in label is 102 × 1218 = 124 236 bytes if sent whole. Most shipping
labels are mostly white, so the converter:

1. finds rows containing ink,
2. groups them into bands, keeping blank runs shorter than 16 rows inside a
   band (a new `BITMAP` header costs ~25 bytes, a blank 4-inch row 102 bytes),
3. trims each band to its leftmost/rightmost inked byte,
4. doubles the merge distance if more than 64 bands result (bounded command
   count for striped pages).

The test suite checks that band output is dot-identical to the full image.

## Status and utility commands (sp410-cli)

| Bytes | Purpose | Reply |
|---|---|---|
| `ESC ! ?` | status, processed immediately | 1 byte, see below |
| `ESC ! R` | reset printer | none |
| `~!T CR LF` | model name | text + `CR` |
| `~!I CR LF` | code page | text + `CR` |
| `~!@ CR LF` | mileage (head use) | text + `CR` |
| `SELFTEST` | print configuration label | — |
| `GAPDETECT` | measure label + gap length | — (**unverified** on SP410; some firmwares use `AUTODETECT`) |
| `BLINEDETECT` | measure black-mark media | — |
| `FORMFEED` / `HOME` | feed one label / feed to next label start | — |

Status byte bits (TSPL standard; bit 6 **unverified** on SP410):

| Bit | Value | Meaning |
|---|---|---|
| 0 | 0x01 | head open |
| 1 | 0x02 | paper jam |
| 2 | 0x04 | out of paper |
| 3 | 0x08 | out of ribbon (n/a, direct thermal) |
| 4 | 0x10 | paused |
| 5 | 0x20 | printing |
| 6 | 0x40 | cover open / model-specific |
| 7 | 0x80 | other error |

`0x00` = ready. The service does not poll status during jobs yet. A stalled
write (`stall-timeout`) is reported as "printer not accepting data".

# Part 2: IPP (from Windows)

`sp410-ippd` is an IPP/2.0 printer at `http://127.0.0.1:8631/ipp/print`.

## Operations

| Operation | Notes |
|---|---|
| Get-Printer-Attributes | honours `requested-attributes` (names, `all`, `printer-description`, `job-template`) |
| Print-Job | `document-format` must be `image/pwg-raster`, or `application/octet-stream` holding PWG raster |
| Validate-Job | checks the document format |
| Create-Job + Send-Document | several documents are joined into one job; `last-document=false` keeps it open |
| Close-Job | finishes a Create-Job job whose last document never came |
| Cancel-Job, Cancel-My-Jobs | a pending job is cancelled at once; a printing job stops between write slices |
| Get-Job-Attributes, Get-Jobs | `which-jobs` = `not-completed` (default), `completed`, `all`; `limit` |

Other operations return `server-error-operation-not-supported`.

## Key printer attributes

| Attribute | Value |
|---|---|
| `document-format-supported` | `image/pwg-raster` |
| `pwg-raster-document-resolution-supported` | 203 × 203 dpi |
| `pwg-raster-document-type-supported` | `black_1`, `sgray_8` |
| `color-supported` | false |
| `media-supported` | 21 sizes, e.g. `na_index-4x6_4x6in`, `oe_4x2-label_4x2in`, `om_100x150-label_100x150mm`, plus `custom_min_20x10mm` … `custom_max_108x300mm` |
| `media-*-margin-supported` | 0 (labels are printed edge to edge) |
| `media-type-supported` | `labels`, `continuous` |
| `copies-supported` | 1–999 (sent to the printer as `PRINT 1,n`) |
| `print-darkness-supported` | 16 levels; job `print-darkness` −100…100 shifts `Darkness` by up to ±8 |
| `printer-device-id` | `MFG:iDPRT;MDL:SP410;CMD:TSPL,PWGRaster;CLS:PRINTER;` |
| `ipp-features-supported` | `ipp-everywhere` |

## Job attributes that change the output

| Job attribute | Effect |
|---|---|
| `copies` | `PRINT 1,n` (unless the raster header already carries copies) |
| `media-type` / `media-col.media-type` = `continuous` | `GAP 0,0` |
| `print-darkness` | adjusts `DENSITY` around the configured darkness (8 if "Default") |
| page size in the raster header | `SIZE w mm,h mm` for each label |
