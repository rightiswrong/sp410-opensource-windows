# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The sp410-opensource-windows contributors.
"""
Small IPP client for the test suite (independent of the server's C code).
"""

from __future__ import annotations

import http.client
import struct

TAG = dict(operation=0x01, job=0x02, end=0x03, printer=0x04,
           integer=0x21, boolean=0x22, enum=0x23, resolution=0x32, range=0x33,
           begcoll=0x34, endcoll=0x37, text=0x41, name=0x42, keyword=0x44,
           uri=0x45, charset=0x47, language=0x48, mimetype=0x49, member=0x4A,
           novalue=0x13)

OP = dict(print_job=0x02, validate_job=0x04, create_job=0x05, send_document=0x06,
          cancel_job=0x08, get_job_attributes=0x09, get_jobs=0x0A,
          get_printer_attributes=0x0B, close_job=0x3B, identify_printer=0x3C)


def _attr(tag, name, value) -> bytes:
    if tag in (TAG["integer"], TAG["enum"]):
        v = struct.pack(">i", value)
    elif tag == TAG["boolean"]:
        v = bytes([1 if value else 0])
    elif isinstance(value, bytes):
        v = value
    else:
        v = value.encode()
    n = name.encode()
    return bytes([tag]) + struct.pack(">H", len(n)) + n + struct.pack(">H", len(v)) + v


class Request:
    def __init__(self, op, request_id=1, version=(2, 0)):
        self.buf = bytearray(bytes(version) + struct.pack(">HI", op, request_id))
        self.group(TAG["operation"])
        self.add("charset", "attributes-charset", "utf-8")
        self.add("language", "attributes-natural-language", "en")

    def group(self, tag):
        self.buf.append(tag)
        return self

    def add(self, tag, name, *values):
        tag = TAG[tag] if isinstance(tag, str) else tag
        for i, v in enumerate(values):
            self.buf += _attr(tag, name if i == 0 else "", v)
        return self

    def raw(self, data: bytes):
        self.buf += data
        return self

    def media_col(self, x, y, media_type=None):
        b = _attr(TAG["begcoll"], "media-col", b"")
        b += _attr(TAG["member"], "", "media-size") + _attr(TAG["begcoll"], "", b"")
        b += _attr(TAG["member"], "", "x-dimension") + _attr(TAG["integer"], "", x)
        b += _attr(TAG["member"], "", "y-dimension") + _attr(TAG["integer"], "", y)
        b += _attr(TAG["endcoll"], "", b"")
        if media_type:
            b += _attr(TAG["member"], "", "media-type") + _attr(TAG["keyword"], "", media_type)
        b += _attr(TAG["endcoll"], "", b"")
        self.buf += b
        return self

    def encode(self, document: bytes = b"") -> bytes:
        return bytes(self.buf) + bytes([TAG["end"]]) + document


class Response:
    def __init__(self, data: bytes):
        self.version = (data[0], data[1])
        self.status, self.request_id = struct.unpack(">HI", data[2:8])
        self.groups = []           # list of (group_tag, {name: [values]})
        pos, cur, last = 8, None, None
        stack = []                 # for collections: (container_dict, member_name)
        while True:
            tag = data[pos]; pos += 1
            if tag == TAG["end"]:
                break
            if tag < 0x10:
                cur = {}
                self.groups.append((tag, cur))
                continue
            nl, = struct.unpack(">H", data[pos:pos + 2]); pos += 2
            name = data[pos:pos + nl].decode(); pos += nl
            vl, = struct.unpack(">H", data[pos:pos + 2]); pos += 2
            raw = data[pos:pos + vl]; pos += vl
            target = stack[-1][0] if stack else cur
            if tag == TAG["member"]:
                stack[-1][1][0] = raw.decode()
                continue
            if tag == TAG["endcoll"]:
                stack.pop()
                continue
            if stack:
                name = stack[-1][1][0]
            elif name:
                last = name
            else:
                name = last
            if tag == TAG["begcoll"]:
                value = {}
                target.setdefault(name, []).append(value)
                stack.append((value, [None]))
                continue
            target.setdefault(name, []).append(self._decode(tag, raw))

    @staticmethod
    def _decode(tag, raw):
        if tag in (TAG["integer"], TAG["enum"]):
            return struct.unpack(">i", raw)[0]
        if tag == TAG["boolean"]:
            return bool(raw[0])
        if tag == TAG["range"]:
            return struct.unpack(">ii", raw)
        if tag == TAG["resolution"]:
            return struct.unpack(">iib", raw)
        if tag == TAG["novalue"]:
            return None
        try:
            return raw.decode()
        except UnicodeDecodeError:
            return raw

    def get(self, name, group=None):
        for g, attrs in self.groups:
            if (group is None or g == group) and name in attrs:
                return attrs[name]
        return None

    def first(self, name, group=None):
        v = self.get(name, group)
        return v[0] if v else None

    def jobs(self):
        return [a for g, a in self.groups if g == TAG["job"]]


def post(host, port, body: bytes, path="/ipp/print", chunked=False, expect=False,
         timeout=30, conn=None):
    own = conn is None
    if own:
        conn = http.client.HTTPConnection(host, port, timeout=timeout)
    headers = {"Content-Type": "application/ipp"}
    if expect:
        headers["Expect"] = "100-continue"
    if chunked:
        def gen():
            for i in range(0, len(body), 7001):
                yield body[i:i + 7001]
        conn.request("POST", path, body=gen(), headers=headers, encode_chunked=True)
    else:
        conn.request("POST", path, body=body, headers=headers)
    r = conn.getresponse()
    data = r.read()
    if own:
        conn.close()
    return r.status, data
