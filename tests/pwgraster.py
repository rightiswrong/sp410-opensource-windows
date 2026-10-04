# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The sp410-opensource-windows contributors.
"""
PWG Raster (PWG 5102.4) writer for tests - an independent encoder, so the
C reader is checked against a second implementation, not against itself.

Pages are given as an 8-bit "ink" plane (0 = white paper, 255 = black) and
encoded as black_1, sgray_8 or srgb_8 with the standard line compression.
"""

from __future__ import annotations

import struct

CS_BLACK, CS_SGRAY, CS_SRGB = 3, 18, 19


class Page:
    def __init__(self, width: int, height: int, dpi: int = 203):
        self.w, self.h, self.dpi = width, height, dpi
        self.ink = bytearray(width * height)
        self.copies = 1

    def fill(self, x0, y0, x1, y1, value=255):
        x0, y0 = max(0, x0), max(0, y0)
        x1, y1 = min(self.w, x1), min(self.h, y1)
        for y in range(y0, y1):
            self.ink[y * self.w + x0:y * self.w + x1] = bytes([value]) * (x1 - x0)

    def dots(self, threshold=128):
        """Expected 1-bit image (list of row bytearrays, MSB first, 1 = black)."""
        stride = (self.w + 7) // 8
        rows = []
        for y in range(self.h):
            row = bytearray(stride)
            base = y * self.w
            for x in range(self.w):
                if self.ink[base + x] >= threshold:
                    row[x >> 3] |= 0x80 >> (x & 7)
            rows.append(row)
        return rows


def _header(p: Page, kind: str, page_size_pt=None) -> bytes:
    bpc, bpp, cs, ncolors = {"black_1": (1, 1, CS_BLACK, 1),
                             "sgray_8": (8, 8, CS_SGRAY, 1),
                             "srgb_8": (8, 24, CS_SRGB, 3)}[kind]
    bpl = (p.w * bpp + 7) // 8
    if page_size_pt is None:
        page_size_pt = (round(p.w * 72 / p.dpi), round(p.h * 72 / p.dpi))
    h = bytearray(1796)
    h[0:9] = b"PwgRaster"
    u32 = lambda off, v: struct.pack_into(">I", h, off, v)
    u32(276, p.dpi); u32(280, p.dpi)
    u32(340, p.copies)
    u32(352, page_size_pt[0]); u32(356, page_size_pt[1])
    u32(372, p.w); u32(376, p.h)
    u32(384, bpc); u32(388, bpp); u32(392, bpl)
    u32(396, 0); u32(400, cs); u32(420, ncolors)
    u32(452, 1)  # TotalPageCount
    return bytes(h)


def _line(p: Page, y: int, kind: str) -> bytes:
    ink = p.ink[y * p.w:(y + 1) * p.w]
    if kind == "black_1":
        out = bytearray((p.w + 7) // 8)
        for x, v in enumerate(ink):
            if v >= 128:
                out[x >> 3] |= 0x80 >> (x & 7)
        return bytes(out)
    if kind == "sgray_8":
        return bytes(255 - v for v in ink)
    return b"".join(bytes([255 - v] * 3) for v in ink)


def _compress(line: bytes, bpp: int, white: int) -> bytes:
    """One line, excluding the line-repeat byte."""
    px = [line[i:i + bpp] for i in range(0, len(line), bpp)]
    out = bytearray()
    i, n = 0, len(px)
    # Trailing white can be sent with the 128 "fill rest" code.
    end = n
    while end > 0 and px[end - 1] == bytes([white]) * bpp:
        end -= 1
    while i < end:
        j = i + 1
        while j < end and px[j] == px[i] and j - i < 128:
            j += 1
        if j - i > 1:
            out.append(j - i - 1)
            out += px[i]
            i = j
            continue
        j = i + 1
        while j < end and j - i < 128 and (j + 1 >= end or px[j] != px[j + 1]):
            j += 1
        if j - i == 1:                 # a single pixel is a run of length 1
            out.append(0)
            out += px[i]
            i = j
            continue
        out.append(257 - (j - i))
        out += b"".join(px[i:j])
        i = j
    if end < n:
        out.append(128)
    return bytes(out)


def encode(pages, kind: str = "sgray_8", page_size_pt=None) -> bytes:
    bpp = {"black_1": 1, "sgray_8": 1, "srgb_8": 3}[kind]
    white = 0x00 if kind == "black_1" else 0xFF
    out = bytearray(b"RaS2")
    for p in pages:
        out += _header(p, kind, page_size_pt)
        y = 0
        while y < p.h:
            line = _line(p, y, kind)
            rep = 0
            while y + rep + 1 < p.h and rep < 255 and _line(p, y + rep + 1, kind) == line:
                rep += 1
            out.append(rep)
            out += _compress(line, bpp, white)
            y += rep + 1
    return bytes(out)


def shipping_label(w=812, h=1218) -> Page:
    """Border, text-like blocks, a non-byte-aligned barcode, a corner block."""
    p = Page(w, h)
    p.fill(0, 0, w, 4); p.fill(0, h - 4, w, h); p.fill(0, 0, 4, h); p.fill(w - 4, 0, w, h)
    for i in range(6):
        for x in range(40, w // 2, 23):
            p.fill(x, 60 + i * 30, x + 15 + (i * 7 + x) % 9, 60 + i * 30 + 18)
    x, i = 37, 0
    while x < w - 40:
        bw, sp = 2 + (i * 7) % 4, 2 + (i * 5) % 3
        p.fill(x, h // 2, x + bw, h // 2 + 160)
        x += bw + sp
        i += 1
    p.fill(w - 101, h - 101, w - 5, h - 5)
    return p
