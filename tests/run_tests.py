#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The sp410-opensource-windows contributors.
"""
End-to-end tests for sp410-ippd and sp410-cli.

Starts the real service binary (Linux or Windows build) on a free port with a
file: output device, talks IPP to it exactly like a client would, then decodes
the TSPL that reached the "printer" and compares it dot for dot.

    python3 tests/run_tests.py --ippd build/native/sp410-ippd --cli build/native/sp410-cli
"""

from __future__ import annotations

import argparse
import http.client
import io
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import tspl_decode            # noqa: E402
import pwgraster as pwg       # noqa: E402
from ippclient import OP, TAG, Request, Response, post   # noqa: E402

FIXTURES = ROOT / "tests" / "fixtures"


class Server:
    """A running sp410-ippd with its own config and output directory."""

    def __init__(self, ippd, tmp, ini_extra="", device=None, max_job_mb=64):
        self.tmp = pathlib.Path(tempfile.mkdtemp(dir=tmp))
        self.out = self.tmp / "out"
        self.out.mkdir()
        self.device = device or f"file:{self.out / 'job-{job}.prn'}"
        self.ini = self.tmp / "sp410.ini"
        self.ini.write_text(
            "[server]\nlisten = 127.0.0.1\nport = 0\nname = Test SP410\nlog-level = debug\n"
            f"max-job-mb = {max_job_mb}\n"
            f"[device]\nuri = {self.device}\nretry-timeout = 2\nstall-timeout = 5\n"
            + ini_extra)
        portfile = self.tmp / "port"
        self.log = open(self.tmp / "ippd.log", "wb")
        self.proc = subprocess.Popen([str(ippd), "--config", str(self.ini),
                                      "--port-file", str(portfile)],
                                     stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 15
        while time.time() < deadline:
            if portfile.exists() and portfile.read_text().strip():
                self.port = int(portfile.read_text())
                break
            if self.proc.poll() is not None:
                raise RuntimeError("sp410-ippd exited early:\n" + self.logtext())
            time.sleep(0.05)
        else:
            raise RuntimeError("sp410-ippd did not start:\n" + self.logtext())

    def logtext(self):
        if not self.log.closed:
            self.log.flush()
        return (self.tmp / "ippd.log").read_text(errors="replace")

    def ipp(self, req: Request, doc=b"", **kw):
        status, data = post("127.0.0.1", self.port, req.encode(doc), **kw)
        assert status == 200, f"HTTP {status}: {data[:200]!r}"
        return Response(data)

    def wait_job(self, job_id, states=(7, 8, 9), timeout=20):
        deadline = time.time() + timeout
        while time.time() < deadline:
            r = self.ipp(Request(OP["get_job_attributes"])
                         .add("uri", "printer-uri", f"ipp://127.0.0.1:{self.port}/ipp/print")
                         .add("integer", "job-id", job_id))
            st = r.first("job-state")
            if st in states:
                return r
            time.sleep(0.05)
        raise AssertionError(f"job {job_id} did not finish; log:\n{self.logtext()[-3000:]}")

    def output(self, job_id):
        return (self.out / f"job-{job_id}.prn").read_bytes()

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.log.close()


def print_req(port, **job):
    r = (Request(OP["print_job"])
         .add("uri", "printer-uri", f"ipp://127.0.0.1:{port}/ipp/print")
         .add("name", "requesting-user-name", "tester")
         .add("name", "job-name", job.pop("name", "test label"))
         .add("mimetype", "document-format", job.pop("format", "image/pwg-raster")))
    if job:
        r.group(TAG["job"])
        for k, (tag, v) in job.items():
            r.add(tag, k.replace("_", "-"), v)
    return r


def assert_dots(label, rows, dx=0, dy=0):
    bad = 0
    for y in range(label.height):
        for x in range(label.width):
            sx, sy = x - dx, y - dy
            want = 0
            if 0 <= sy < len(rows) and 0 <= sx < len(rows[0]) * 8:
                want = (rows[sy][sx >> 3] >> (7 - (sx & 7))) & 1
            if label.get(x, y) != want:
                bad += 1
    assert not bad, f"{bad} dots differ from the expected image"


def cmds(stream):
    return [c.text for c in stream.commands]


# ---------------------------------------------------------------------------

def t_printer_attributes(c):
    s = c.server()
    r = s.ipp(Request(OP["get_printer_attributes"])
              .add("uri", "printer-uri", f"ipp://127.0.0.1:{s.port}/ipp/print"))
    assert r.status == 0, hex(r.status)
    assert r.first("printer-name") == "Test SP410"
    assert r.get("document-format-supported") == ["image/pwg-raster"]
    assert r.first("pwg-raster-document-resolution-supported") == (203, 203, 3)
    assert set(r.get("pwg-raster-document-type-supported")) == {"black_1", "sgray_8"}
    assert r.get("ipp-features-supported") == ["ipp-everywhere"]
    assert r.first("printer-state") == 3 and r.get("printer-state-reasons") == ["none"]
    assert r.first("printer-uri-supported") == f"ipp://127.0.0.1:{s.port}/ipp/print"
    assert r.first("printer-make-and-model") == "iDPRT SP410"
    assert "na_index-4x6_4x6in" in r.get("media-supported")
    assert r.first("media-default") == "na_index-4x6_4x6in"
    db = r.get("media-col-database")
    assert len(db) == 22 and db[0]["media-size"][0]["x-dimension"] == [10160]
    assert db[-1]["media-size"][0]["x-dimension"] == [(2000, 10800)]
    assert r.first("color-supported") is False
    for op in (2, 4, 5, 6, 8, 9, 10, 11):
        assert op in r.get("operations-supported"), op
    assert r.first("printer-uuid").startswith("urn:uuid:")


def t_requested_attributes_filter(c):
    s = c.server()
    r = s.ipp(Request(OP["get_printer_attributes"])
              .add("uri", "printer-uri", "ipp://localhost/ipp/print")
              .add("keyword", "requested-attributes", "printer-state", "media-ready"))
    names = set(r.groups[1][1])
    assert names == {"printer-state", "media-ready"}, names


def t_print_job_sgray_exact(c):
    s = c.server()
    page = pwg.shipping_label()
    doc = pwg.encode([page], "sgray_8")
    r = s.ipp(print_req(s.port), doc)
    assert r.status == 0, hex(r.status)
    jid = r.first("job-id", TAG["job"])
    assert r.first("job-uri").endswith(f"/ipp/print/{jid}")
    done = s.wait_job(jid)
    assert done.first("job-state") == 9, done.first("job-state-message")
    assert done.first("job-impressions-completed") == 1
    st = tspl_decode.parse(s.output(jid))
    assert not st.warnings and len(st.labels) == 1
    assert cmds(st)[0] == "SIZE 101.6 mm,152.4 mm"
    assert "GAP 3 mm,0 mm" in cmds(st)
    assert_dots(st.labels[0], page.dots())


def t_black1_and_srgb(c):
    s = c.server()
    page = pwg.shipping_label(406, 203)
    for kind in ("black_1", "srgb_8"):
        r = s.ipp(print_req(s.port), pwg.encode([page], kind))
        jid = r.first("job-id", TAG["job"])
        assert s.wait_job(jid).first("job-state") == 9
        assert_dots(tspl_decode.parse(s.output(jid)).labels[0], page.dots())


def t_create_job_send_document_chunked(c):
    s = c.server()
    pages = [pwg.shipping_label(), pwg.shipping_label(406, 203), pwg.Page(812, 406)]
    doc = pwg.encode(pages, "black_1")
    r = s.ipp(Request(OP["create_job"]).add("uri", "printer-uri", "ipp://x/ipp/print")
              .add("name", "job-name", "multi"))
    jid = r.first("job-id", TAG["job"])
    assert r.first("job-state") == 3
    r = s.ipp(Request(OP["send_document"]).add("uri", "printer-uri", "ipp://x/ipp/print")
              .add("integer", "job-id", jid).add("boolean", "last-document", True)
              .add("mimetype", "document-format", "image/pwg-raster"), doc,
              chunked=True, expect=True)
    assert r.status == 0, hex(r.status)
    done = s.wait_job(jid)
    assert done.first("job-state") == 9 and done.first("job-impressions-completed") == 3
    st = tspl_decode.parse(s.output(jid))
    sizes = [x for x in cmds(st) if x.startswith("SIZE")]
    assert sizes == ["SIZE 101.6 mm,152.4 mm", "SIZE 50.8 mm,25.4 mm", "SIZE 101.6 mm,50.8 mm"]
    assert_dots(st.labels[0], pages[0].dots())
    assert st.labels[2].black_dots() == 0


def t_send_document_in_two_parts(c):
    s = c.server()
    a, b = pwg.shipping_label(406, 203), pwg.Page(406, 203)
    b.fill(10, 10, 100, 100)
    r = s.ipp(Request(OP["create_job"]).add("uri", "printer-uri", "ipp://x/ipp/print"))
    jid = r.first("job-id", TAG["job"])
    for doc, last in ((pwg.encode([a], "sgray_8"), False), (pwg.encode([b], "sgray_8"), True)):
        r = s.ipp(Request(OP["send_document"]).add("uri", "printer-uri", "ipp://x/ipp/print")
                  .add("integer", "job-id", jid).add("boolean", "last-document", last), doc)
        assert r.status == 0, hex(r.status)
    assert s.wait_job(jid).first("job-impressions-completed") == 2


def t_copies_and_job_template(c):
    s = c.server(ini_extra="[print]\nDarkness = 10 ; inline comment\nPrintSpeed = 4\n")
    page = pwg.shipping_label(406, 203)
    r = s.ipp(print_req(s.port, copies=("integer", 3), print_darkness=("integer", 50),
                        media_type=("keyword", "continuous")), pwg.encode([page], "sgray_8"))
    jid = r.first("job-id", TAG["job"])
    done = s.wait_job(jid)
    assert done.first("job-impressions-completed") == 3
    c_ = cmds(tspl_decode.parse(s.output(jid)))
    assert "PRINT 1,3" in c_ and "GAP 0,0" in c_ and "SPEED 4" in c_
    assert "DENSITY 14" in c_, c_            # 10 + 50% of 8


def t_media_col_continuous(c):
    s = c.server()
    req = print_req(s.port)
    req.group(TAG["job"]).media_col(5080, 2540, "continuous")
    r = s.ipp(req, pwg.encode([pwg.shipping_label(406, 203)], "sgray_8"))
    jid = r.first("job-id", TAG["job"])
    s.wait_job(jid)
    assert "GAP 0,0" in cmds(tspl_decode.parse(s.output(jid)))


def t_config_reloaded_per_job(c):
    s = c.server()
    doc = pwg.encode([pwg.shipping_label(406, 203)], "sgray_8")
    j1 = s.ipp(print_req(s.port), doc).first("job-id", TAG["job"])
    s.wait_job(j1)
    with open(s.ini, "a") as f:
        f.write("[print]\nMediaTracking = BlackMark\nGapHeight = 4\nShiftX = 1\n")
    j2 = s.ipp(print_req(s.port), doc).first("job-id", TAG["job"])
    s.wait_job(j2)
    assert "GAP 3 mm,0 mm" in cmds(tspl_decode.parse(s.output(j1)))
    st = tspl_decode.parse(s.output(j2))
    assert "BLINE 4 mm,0 mm" in cmds(st)
    assert_dots(st.labels[0], pwg.shipping_label(406, 203).dots(), dx=8)


def t_validate_and_bad_formats(c):
    s = c.server()
    r = s.ipp(Request(OP["validate_job"]).add("uri", "printer-uri", "ipp://x/ipp/print")
              .add("mimetype", "document-format", "application/pdf"))
    assert r.status == 0x040A, hex(r.status)
    r = s.ipp(Request(OP["validate_job"]).add("uri", "printer-uri", "ipp://x/ipp/print")
              .add("mimetype", "document-format", "image/pwg-raster"))
    assert r.status == 0
    r = s.ipp(print_req(s.port, format="application/pdf"), b"%PDF-1.7 ...")
    assert r.status == 0x040A
    r = s.ipp(print_req(s.port), b"not a raster")
    assert r.status == 0x040A
    r = s.ipp(print_req(s.port), b"")
    assert r.status == 0x0400
    # octet-stream is accepted when the data is PWG raster
    r = s.ipp(print_req(s.port, format="application/octet-stream"),
              pwg.encode([pwg.Page(64, 64)], "sgray_8"))
    assert r.status == 0


def t_corrupt_raster_aborts_without_output(c):
    s = c.server()
    good = pwg.encode([pwg.shipping_label(406, 203)], "sgray_8")
    r = s.ipp(print_req(s.port), good[:len(good) // 2])
    jid = r.first("job-id", TAG["job"])
    done = s.wait_job(jid)
    assert done.first("job-state") == 8
    assert "truncated" in done.first("job-state-message")
    assert not list(s.out.iterdir()), "no partial output may reach the printer"


def t_cancel_and_get_jobs(c):
    s = c.server(device="file:/nonexistent-dir/x/job-{job}.prn")
    doc = pwg.encode([pwg.Page(64, 64)], "sgray_8")
    j1 = s.ipp(print_req(s.port), doc).first("job-id", TAG["job"])
    j2 = s.ipp(print_req(s.port), doc).first("job-id", TAG["job"])
    r = s.ipp(Request(OP["cancel_job"]).add("uri", "printer-uri", "ipp://x/ipp/print")
              .add("integer", "job-id", j2))
    assert r.status == 0
    assert s.wait_job(j2).first("job-state") == 7
    # j1 cannot reach the device: retried, then aborted with a reason
    attrs = s.ipp(Request(OP["get_printer_attributes"])
                  .add("uri", "printer-uri", "ipp://x/ipp/print"))
    done = s.wait_job(j1, timeout=15)
    assert done.first("job-state") == 8
    assert "cannot create output file" in done.first("job-state-message")
    r = s.ipp(Request(OP["get_jobs"]).add("uri", "printer-uri", "ipp://x/ipp/print")
              .add("keyword", "which-jobs", "completed")
              .add("keyword", "requested-attributes", "job-id", "job-state"))
    ids = [(j["job-id"][0], j["job-state"][0]) for j in r.jobs()]
    assert ids == [(j2, 7), (j1, 8)], ids
    r = s.ipp(Request(OP["cancel_job"]).add("uri", "printer-uri", "ipp://x/ipp/print")
              .add("integer", "job-id", j1))
    assert r.status == 0x0404
    r = s.ipp(Request(OP["get_job_attributes"]).add("uri", "printer-uri", "ipp://x/ipp/print")
              .add("integer", "job-id", 999))
    assert r.status == 0x0406
    assert attrs.status == 0


def t_protocol_errors(c):
    s = c.server()
    r = s.ipp(Request(0x4001).add("uri", "printer-uri", "ipp://x/ipp/print"))
    assert r.status == 0x0501
    r = Response(post("127.0.0.1", s.port, Request(OP["get_jobs"], version=(9, 0)).encode())[1])
    assert r.status == 0x0503
    st, data = post("127.0.0.1", s.port, b"\x02\x00\x00\x0b\x00\x00\x00\x07\x01\x47\xff\xff")
    assert st == 200 and Response(data).status == 0x0400 and Response(data).request_id == 7
    conn = http.client.HTTPConnection("127.0.0.1", s.port, timeout=10)
    conn.request("POST", "/ipp/print", body=b"x", headers={"Content-Type": "text/plain"})
    assert conn.getresponse().status == 415
    conn.close()
    with socket.create_connection(("127.0.0.1", s.port), timeout=5) as sk:
        sk.sendall(b"GARBAGE\r\n\r\n")
        assert sk.recv(100).startswith(b"HTTP/1.1 400")


def t_keep_alive_and_concurrency(c):
    s = c.server()
    conn = http.client.HTTPConnection("127.0.0.1", s.port, timeout=10)
    for i in range(20):
        st, data = post(None, None, Request(OP["get_printer_attributes"], request_id=i + 1)
                        .add("uri", "printer-uri", "ipp://x/ipp/print").encode(), conn=conn)
        assert st == 200 and Response(data).request_id == i + 1
    conn.close()
    errors = []
    big = pwg.encode([pwg.shipping_label()], "sgray_8")

    def worker(n):
        try:
            for _ in range(5):
                r = s.ipp(print_req(s.port), big) if n == 0 else \
                    s.ipp(Request(OP["get_printer_attributes"]).add("uri", "printer-uri", "ipp://x/ipp/print"))
                assert r.status == 0
        except Exception as e:      # noqa: BLE001
            errors.append(e)
    ts = [threading.Thread(target=worker, args=(n,)) for n in range(6)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    assert not errors, errors
    r = s.ipp(Request(OP["get_jobs"]).add("uri", "printer-uri", "ipp://x/ipp/print")
              .add("keyword", "which-jobs", "all"))
    assert len(r.jobs()) == 5
    for j in r.jobs():
        assert s.wait_job(j["job-id"][0]).first("job-state") == 9


def t_job_too_large(c):
    s = c.server(max_job_mb=1)
    doc = os.urandom(3 * 1024 * 1024)
    st, _ = post("127.0.0.1", s.port, print_req(s.port).encode(b"RaS2" + doc))
    assert st == 413
    st, _ = post("127.0.0.1", s.port, print_req(s.port).encode(b"RaS2" + doc), chunked=True)
    assert st == 413
    assert s.ipp(Request(OP["get_printer_attributes"])).status == 0      # still serving


def t_status_page(c):
    s = c.server()
    conn = http.client.HTTPConnection("127.0.0.1", s.port, timeout=10)
    conn.request("GET", "/")
    r = conn.getresponse()
    body = r.read().decode()
    assert r.status == 200 and "Test SP410" in body and "/ipp/print" in body
    conn.request("GET", "/nothing")
    assert conn.getresponse().status == 404
    conn.close()


def t_libcups_fixture(c):
    """PWG raster written by libcups (Linux), converted by the server, must equal
    the Linux CUPS filter's output byte for byte."""
    s = c.server()
    for name in ("cups-pwg-4x2", "cups-v3-rgb"):
        doc = (FIXTURES / f"{name}.ras").read_bytes()
        fmt = "image/pwg-raster" if doc[:4] == b"RaS2" else None
        if fmt is None:
            # CUPS v3 rasters are not PWG; check them through the CLI instead.
            out = c.tmp / f"{name}.prn"
            subprocess.run([str(c.cli), "convert", str(FIXTURES / f"{name}.ras"), str(out),
                            "GapHeight=3"], check=True, capture_output=True)
            got = out.read_bytes()
        else:
            jid = s.ipp(print_req(s.port), doc).first("job-id", TAG["job"])
            s.wait_job(jid)
            got = s.output(jid)
        want = (FIXTURES / f"{name}.linux.prn").read_bytes()
        assert got == want, f"{name}: output differs from the Linux filter"


def t_cli_convert_and_test_label(c):
    src = c.tmp / "in.pwg"
    src.write_bytes(pwg.encode([pwg.shipping_label(406, 203)], "sgray_8"))
    out = c.tmp / "out.prn"
    p = subprocess.run([str(c.cli), "convert", str(src), str(out), "Darkness=12",
                        "Dither=Threshold", "--copies", "2"], capture_output=True, text=True)
    assert p.returncode == 0, p.stderr
    st = tspl_decode.parse(out.read_bytes())
    assert "DENSITY 12" in cmds(st) and "PRINT 1,2" in cmds(st)
    p = subprocess.run([str(c.cli), "convert", str(src), str(out), "Bogus=1"], capture_output=True)
    assert p.returncode == 2
    p = subprocess.run([str(c.cli), "--dry-run", "test-label", "--size", "100x150mm"],
                       capture_output=True)
    assert p.returncode == 0
    st = tspl_decode.parse(p.stdout)
    assert st.labels[0].width == 800 and any(u.startswith("BARCODE") for u in st.labels[0].unrendered)
    lab = c.tmp / "label.prn"
    p = subprocess.run([str(c.cli), "--device", f"file:{lab}", "calibrate"], capture_output=True)
    assert p.returncode == 0 and lab.read_bytes() == b"GAPDETECT\r\n"
    ini = c.tmp / "new.ini"
    p = subprocess.run([str(c.cli), "config-init", str(ini)], capture_output=True)
    assert p.returncode == 0 and "[print]" in ini.read_text()


def t_cli_status_over_tcp(c):
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]
    got = bytearray()

    def fake():
        conn, _ = srv.accept()
        got.extend(conn.recv(16))
        conn.sendall(b"\x04")
        time.sleep(0.3)
        conn.close()
        srv.close()
    t = threading.Thread(target=fake)
    t.start()
    p = subprocess.run([str(c.cli), "--device", f"socket://127.0.0.1:{port}", "status"],
                       capture_output=True, text=True, timeout=20)
    t.join()
    assert bytes(got) == b"\x1b!?"
    assert p.returncode == 1 and "out of paper" in p.stdout, p.stdout


def t_sample_config_parses(c):
    """The installer's sample config must load without warnings."""
    s = c.server()
    s.stop()
    tmp = c.tmp / "sample.ini"
    shutil.copy(ROOT / "installer" / "sp410.ini", tmp)
    text = tmp.read_text().replace("port = 8631", "port = 0")
    tmp.write_text(text)
    pf = c.tmp / "sample.port"
    p = subprocess.Popen([str(c.ippd), "--config", str(tmp), "--port-file", str(pf)],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    deadline = time.time() + 10
    while not (pf.exists() and pf.read_text().strip()) and time.time() < deadline:
        time.sleep(0.05)
    p.terminate()
    out = p.communicate(timeout=10)[0].decode(errors="replace")
    assert "WARN" not in out, out
    assert "ready" in out, out


def t_clean_shutdown(c):
    s = c.server()
    conn = http.client.HTTPConnection("127.0.0.1", s.port, timeout=10)
    conn.request("GET", "/")
    conn.getresponse().read()          # leave the keep-alive connection open
    t0 = time.time()
    s.proc.terminate()
    rc = s.proc.wait(15)
    conn.close()
    assert time.time() - t0 < 8, "shutdown should not wait for idle connections"
    if os.name != "nt":
        # On Windows terminate() is TerminateProcess (no graceful path to test);
        # the service's graceful stop is exercised by the installer e2e test.
        assert rc == 0, rc
        assert "stopped" in s.logtext()


TESTS = [v for k, v in sorted(globals().items()) if k.startswith("t_")]


class Ctx:
    def __init__(self, ippd, cli, tmp):
        self.ippd, self.cli, self.tmp = ippd, cli, tmp
        self.servers = []

    def server(self, **kw):
        s = Server(self.ippd, self.tmp, **kw)
        self.servers.append(s)
        return s

    def cleanup(self):
        for s in self.servers:
            s.stop()
        self.servers = []


def main() -> int:
    ap = argparse.ArgumentParser()
    exe = ".exe" if os.name == "nt" else ""
    ap.add_argument("--ippd", type=pathlib.Path, default=ROOT / f"build/native/sp410-ippd{exe}")
    ap.add_argument("--cli", type=pathlib.Path, default=ROOT / f"build/native/sp410-cli{exe}")
    ap.add_argument("-k", help="only tests whose name contains this")
    ap.add_argument("-v", action="store_true", help="print server logs of failing tests")
    args = ap.parse_args()

    fails = 0
    with tempfile.TemporaryDirectory(prefix="sp410win-") as tmp:
        tests = [t for t in TESTS if not args.k or args.k in t.__name__]
        print(f"1..{len(tests)}")
        for i, t in enumerate(tests, 1):
            ctx = Ctx(args.ippd.resolve(), args.cli.resolve(), pathlib.Path(tmp))
            t0 = time.time()
            try:
                t(ctx)
                print(f"ok {i} - {t.__name__[2:]} ({time.time() - t0:.1f}s)")
            except Exception as e:  # noqa: BLE001
                fails += 1
                print(f"not ok {i} - {t.__name__[2:]}\n  # {type(e).__name__}: {e}")
                if args.v:
                    for s in ctx.servers:
                        print(s.logtext()[-4000:])
            finally:
                ctx.cleanup()
            sys.stdout.flush()
    print(f"# {len(tests) - fails}/{len(tests)} passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
