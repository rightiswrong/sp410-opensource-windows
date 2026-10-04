#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The sp410-cups-driver contributors.
"""
tspl-decode - parse and render a TSPL print stream without a printer.

Used three ways:
  * test oracle:      the test suite renders our filter's output and compares
                      it dot-for-dot with the expected image;
  * debugging:        see exactly which commands a job sends;
  * reverse-engineering: capture the vendor filter's output (see
                      docs/REVERSE_ENGINEERING.md) and diff it against ours.

Examples
  tspl-decode job.prn                     # command listing + label summary
  tspl-decode job.prn --pbm-dir out/      # one P4 PBM per printed label
  tspl-decode job.prn --png-dir out/      # PNG (needs Pillow)
  tspl-decode vendor.prn --diff ours.prn  # setup + pixel diff, exit 1 if different
  tspl-decode job.prn --json              # machine-readable

Only rendering of BITMAP is implemented; TEXT/BARCODE/etc. are listed as
"unrendered" so you can still see that a stream used printer-resident fonts.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from dataclasses import dataclass, field

DEFAULT_DPI = 203
_INVERT = bytes(255 - i for i in range(256))

_BITMAP_RE = re.compile(rb"BITMAP\s+(-?\d+)\s*,\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,")
_SIZE_RE = re.compile(r"^SIZE\s+([\d.]+)\s*(mm|dot)?\s*,\s*([\d.]+)\s*(mm|dot)?$", re.I)
_PRINT_RE = re.compile(r"^PRINT\s+(\d+)(?:\s*,\s*(\d+))?$", re.I)
_RENDERED = {"BITMAP", "CLS"}
_SETUP = {"SIZE", "GAP", "BLINE", "DIRECTION", "REFERENCE", "OFFSET", "SPEED",
          "DENSITY", "SET", "SHIFT", "CODEPAGE", "LIMITFEED"}
_DRAWING = {"TEXT", "BARCODE", "QRCODE", "BAR", "BOX", "CIRCLE", "ELLIPSE",
            "PUTBMP", "PUTPCX", "BLOCK", "DMATRIX", "PDF417", "AZTEC",
            "ERASE", "REVERSE", "DIAGONAL"}


@dataclass
class Command:
    offset: int
    text: str
    payload: int = 0          # raw byte count following the command (BITMAP)

    def keyword(self) -> str:
        return self.text.split(None, 1)[0].upper() if self.text else ""


@dataclass
class Label:
    index: int
    width: int
    height: int
    bits: bytearray            # packed rows, MSB first, 1 = black
    copies: int
    setup: dict = field(default_factory=dict)
    bitmaps: int = 0
    unrendered: list = field(default_factory=list)
    warnings: list = field(default_factory=list)

    @property
    def stride(self) -> int:
        return (self.width + 7) // 8

    def black_dots(self) -> int:
        return sum(bin(b).count("1") for b in self.bits)

    def get(self, x: int, y: int) -> int:
        return (self.bits[y * self.stride + (x >> 3)] >> (7 - (x & 7))) & 1

    def to_pbm(self) -> bytes:
        return f"P4\n{self.width} {self.height}\n".encode() + bytes(self.bits)


@dataclass
class Stream:
    commands: list
    labels: list
    warnings: list


def _dots_per_mm(dpi: int) -> float:
    # TSPL convention: 203 dpi heads are treated as exactly 8 dots/mm and
    # 300 dpi heads as 12 dots/mm when converting "mm" arguments.
    return {203: 8.0, 300: 12.0}.get(dpi, dpi / 25.4)


def _to_dots(value: float, unit: str | None, dpi: int) -> int:
    if unit and unit.lower() == "mm":
        return int(value * _dots_per_mm(dpi) + 1e-6)
    if unit and unit.lower() == "dot":
        return int(round(value))
    return int(round(value * dpi))          # inches (TSPL default unit)


class _Canvas:
    def __init__(self, w: int, h: int):
        self.w, self.h = w, h
        self.stride = (w + 7) // 8
        self.bits = bytearray(self.stride * h)

    def put(self, x: int, y: int, black: bool, mode: int) -> bool:
        if not (0 <= x < self.w and 0 <= y < self.h):
            return False
        i, m = y * self.stride + (x >> 3), 0x80 >> (x & 7)
        if mode == 0:                       # OVERWRITE
            self.bits[i] = (self.bits[i] | m) if black else (self.bits[i] & ~m)
        elif mode == 1:                     # OR
            if black:
                self.bits[i] |= m
        elif mode == 2:                     # XOR
            if black:
                self.bits[i] ^= m
        return True


def parse(data: bytes, dpi: int = DEFAULT_DPI) -> Stream:
    pos, n = 0, len(data)
    commands: list[Command] = []
    labels: list[Label] = []
    warnings: list[str] = []
    setup: dict[str, str] = {}
    canvas: _Canvas | None = None
    bitmaps, unrendered, lwarn = 0, [], []

    def new_canvas() -> _Canvas | None:
        if "SIZE" not in setup:
            return None
        m = _SIZE_RE.match("SIZE " + setup["SIZE"])
        if not m:
            warnings.append(f"unparseable SIZE {setup['SIZE']!r}")
            return None
        w = _to_dots(float(m.group(1)), m.group(2), dpi)
        h = _to_dots(float(m.group(3)), m.group(4) or m.group(2), dpi)
        return _Canvas(w, h)

    while pos < n:
        c = data[pos]
        if c in b"\r\n \t\x00":
            pos += 1
            continue

        # ESC-prefixed immediate commands, e.g. ESC !? (status), ESC !R (reset)
        if c == 0x1B:
            seq = data[pos:pos + 3]
            commands.append(Command(pos, "ESC " + seq[1:].decode("latin-1", "replace")))
            pos += 3
            continue

        if data.startswith(b"BITMAP", pos):
            m = _BITMAP_RE.match(data, pos)
            if not m:
                warnings.append(f"malformed BITMAP header at offset {pos}")
                end = data.find(b"\n", pos)
                pos = n if end < 0 else end + 1
                continue
            x, y, wb, h, mode = (int(g) for g in m.groups())
            size = wb * h
            payload = data[m.end():m.end() + size]
            commands.append(Command(pos, m.group(0).decode().rstrip(","), size))
            if len(payload) < size:
                warnings.append(f"BITMAP at offset {pos} truncated "
                                f"({len(payload)} of {size} bytes)")
            if canvas is None:
                canvas = new_canvas()
            if canvas is None:
                warnings.append(f"BITMAP at offset {pos} before SIZE/CLS")
            elif (mode == 0 and x >= 0 and y >= 0 and x % 8 == 0 and
                  x // 8 + wb <= canvas.stride and y + h <= canvas.h and
                  len(payload) == size):
                # Fast path: byte-aligned OVERWRITE fully inside the label.
                bx, tail = x // 8, canvas.w & 7
                clipped = 0
                for row in range(h):
                    src = payload[row * wb:(row + 1) * wb].translate(_INVERT)
                    if tail and bx + wb == canvas.stride:
                        mask = (0xFF << (8 - tail)) & 0xFF
                        clipped += bin(src[-1] & ~mask & 0xFF).count("1")
                        src = src[:-1] + bytes([src[-1] & mask])
                    off = (y + row) * canvas.stride + bx
                    canvas.bits[off:off + wb] = src
                if clipped:
                    lwarn.append(f"BITMAP at offset {pos}: {clipped} black dots off-label")
                bitmaps += 1
            else:
                clipped = 0
                for row in range(h):
                    base = row * wb
                    for b in range(wb):
                        if base + b >= len(payload):
                            break
                        v = payload[base + b]
                        if v == 0xFF and mode != 0:
                            continue
                        for bit in range(8):
                            black = not (v & (0x80 >> bit))       # TSPL: 0 = black
                            if not canvas.put(x + b * 8 + bit, y + row, black, mode):
                                if black:
                                    clipped += 1
                if clipped:
                    lwarn.append(f"BITMAP at offset {pos}: {clipped} black dots off-label")
                bitmaps += 1
            pos = m.end() + size
            continue

        start = pos
        end = data.find(b"\n", pos)
        end = n if end < 0 else end
        line = data[pos:end].rstrip(b"\r").decode("latin-1", "replace").strip()
        pos = end + 1
        if not line:
            continue
        cmd = Command(start, line)
        commands.append(cmd)
        kw = cmd.keyword()
        arg = line[len(kw):].strip()

        if kw in _SETUP:
            key = "SET " + arg.split(None, 1)[0].upper() if kw == "SET" and arg else kw
            setup[key] = arg.split(None, 1)[1] if kw == "SET" and " " in arg else arg
            if kw == "SIZE":
                canvas = None
        elif kw == "CLS":
            canvas = new_canvas()
            if canvas is None:
                warnings.append(f"CLS at offset {cmd.offset} without a valid SIZE")
        elif kw == "PRINT":
            m = _PRINT_RE.match(line)
            sets, copies = (int(m.group(1)), int(m.group(2) or 1)) if m else (1, 1)
            if canvas is None:
                canvas = new_canvas() or _Canvas(0, 0)
            labels.append(Label(len(labels) + 1, canvas.w, canvas.h,
                                bytearray(canvas.bits), sets * copies,
                                dict(setup), bitmaps, unrendered, lwarn))
            bitmaps, unrendered, lwarn = 0, [], []
        elif kw in _DRAWING:
            unrendered.append(line[:80])

    return Stream(commands, labels, warnings)


# ---- presentation --------------------------------------------------------

def summarize(s: Stream, out=sys.stdout) -> None:
    for c in s.commands:
        extra = f"  <{c.payload} bytes>" if c.payload else ""
        print(f"{c.offset:8d}  {c.text}{extra}", file=out)
    print(file=out)
    for lb in s.labels:
        area = max(1, lb.width * lb.height)
        print(f"label {lb.index}: {lb.width}x{lb.height} dots, x{lb.copies}, "
              f"{lb.bitmaps} bitmap(s), {100.0 * lb.black_dots() / area:.2f}% black", file=out)
        for u in lb.unrendered:
            print(f"    unrendered: {u}", file=out)
        for w in lb.warnings:
            print(f"    warning: {w}", file=out)
    for w in s.warnings:
        print(f"warning: {w}", file=out)


def as_json(s: Stream) -> dict:
    return {
        "commands": [{"offset": c.offset, "text": c.text, "payload": c.payload}
                     for c in s.commands],
        "labels": [{"index": l.index, "width": l.width, "height": l.height,
                    "copies": l.copies, "bitmaps": l.bitmaps,
                    "black_dots": l.black_dots(), "setup": l.setup,
                    "unrendered": l.unrendered, "warnings": l.warnings}
                   for l in s.labels],
        "warnings": s.warnings,
    }


def diff(a: Stream, b: Stream, out=sys.stdout) -> int:
    """Compare two streams label by label.  Returns number of differences."""
    ndiff = 0
    if len(a.labels) != len(b.labels):
        print(f"label count: {len(a.labels)} vs {len(b.labels)}", file=out)
        ndiff += 1
    for la, lb in zip(a.labels, b.labels):
        keys = sorted(set(la.setup) | set(lb.setup))
        for k in keys:
            va, vb = la.setup.get(k), lb.setup.get(k)
            if va != vb:
                print(f"label {la.index}: {k}: {va!r} vs {vb!r}", file=out)
                ndiff += 1
        if la.copies != lb.copies:
            print(f"label {la.index}: copies {la.copies} vs {lb.copies}", file=out)
            ndiff += 1
        if (la.width, la.height) != (lb.width, lb.height):
            print(f"label {la.index}: size {la.width}x{la.height} vs "
                  f"{lb.width}x{lb.height}", file=out)
            ndiff += 1
            continue
        dots = sum(bin(x ^ y).count("1") for x, y in zip(la.bits, lb.bits))
        if dots:
            print(f"label {la.index}: {dots} dots differ "
                  f"({100.0 * dots / max(1, la.width * la.height):.3f}%)", file=out)
            ndiff += 1
    if not ndiff:
        print("streams are equivalent (same setup, same dots)", file=out)
    return ndiff


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="tspl-decode", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", type=pathlib.Path, help="TSPL stream ('-' for stdin)")
    ap.add_argument("--dpi", type=int, default=DEFAULT_DPI)
    ap.add_argument("--json", action="store_true", help="print JSON instead of text")
    ap.add_argument("--pbm-dir", type=pathlib.Path, help="write label-N.pbm files")
    ap.add_argument("--png-dir", type=pathlib.Path, help="write label-N.png (Pillow)")
    ap.add_argument("--diff", type=pathlib.Path, metavar="OTHER",
                    help="compare against another TSPL stream")
    args = ap.parse_args(argv)

    data = sys.stdin.buffer.read() if str(args.file) == "-" else args.file.read_bytes()
    s = parse(data, args.dpi)

    if args.diff:
        return 1 if diff(s, parse(args.diff.read_bytes(), args.dpi)) else 0

    if args.json:
        json.dump(as_json(s), sys.stdout, indent=2)
        print()
    else:
        summarize(s)

    if args.pbm_dir:
        args.pbm_dir.mkdir(parents=True, exist_ok=True)
        for lb in s.labels:
            (args.pbm_dir / f"label-{lb.index}.pbm").write_bytes(lb.to_pbm())
    if args.png_dir:
        try:
            from PIL import Image
        except ImportError:
            print("--png-dir needs Pillow (pip install pillow); use --pbm-dir", file=sys.stderr)
            return 2
        args.png_dir.mkdir(parents=True, exist_ok=True)
        for lb in s.labels:
            img = Image.frombytes("1", (lb.width, lb.height), bytes(lb.bits))
            # PIL mode "1": 1 = white, so invert our 1 = black packing.
            img = Image.eval(img.convert("L"), lambda v: 255 - v)
            img.save(args.png_dir / f"label-{lb.index}.png")
    return 1 if s.warnings else 0


if __name__ == "__main__":
    sys.exit(main())
