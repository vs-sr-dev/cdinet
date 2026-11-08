#!/usr/bin/env python3
"""A CD-i stand-in for testing the proxy: speaks docs/protocol.md and prints the screens.

    python fake_cdi.py [--port 6400] [--cols 64 --rows 24] STEP...
    STEP: go TEXT | link N | page N | back | noise

Links print as [N:text]; bold as *text*; heading colour as =text=."""
import argparse
import os
import socket
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "proxy"))
from cdinet_proxy import Deframer, frame  # noqa: E402


class FakeCdi:
    def __init__(self, port, cols, rows):
        self.s = socket.create_connection(("127.0.0.1", port))
        self.s.settimeout(0.2)
        self.d = Deframer()
        self.seq = 0
        self.cols, self.rows = cols, rows
        self.bytes_in = self.bytes_out = 0
        self.frames = 0

    def request(self, ftype, args=b""):
        self.seq = self.seq % 255 + 1
        f = frame(ftype, bytes([self.seq]) + args)
        for attempt in range(5):
            self.s.sendall(f)
            self.bytes_out += len(f)
            self.frames += 1
            end = time.time() + 2
            while time.time() < end:
                try:
                    data = self.s.recv(4096)
                except socket.timeout:
                    continue
                self.bytes_in += len(data)
                for t, p in self.d.feed(data):
                    if p and p[0] == self.seq:
                        return t, p[1:]
            print("(no reply to %s, resending)" % ftype)
        raise SystemExit("the proxy does not answer")

    def screen(self, t, p):
        """After a navigation request: wait through W, then collect the chunks."""
        while t == "W":
            time.sleep(0.5)
            t, p = self.request("N", b"\0\0")
        if t == "E":
            print("ERROR:", p.decode("latin-1"))
            return None
        assert t == "C", t
        sid, total = p[0], (p[3] << 8) | p[4]
        blob = bytearray(total)
        got = 0
        while True:
            off = (p[1] << 8) | p[2]
            data = p[5:]
            if p[0] == sid:
                blob[off:off + len(data)] = data
                got = max(got, off + len(data))
            if got >= total:
                break
            t, p = self.request("N", bytes([got >> 8, got & 0xFF]))
            assert t == "C", t
        return bytes(blob)

    def show(self, blob):
        if blob is None:
            return
        i = 0
        page, pages = (blob[0] << 8) | blob[1], (blob[2] << 8) | blob[3]
        i = 4
        n = blob[i]; title = blob[i + 1:i + 1 + n].decode("latin-1"); i += 1 + n
        n = blob[i]; url = blob[i + 1:i + 1 + n].decode("latin-1"); i += 1 + n
        nrows = blob[i]; i += 1
        if nrows == 0xFF:
            self.picture(blob[i:], title)
            return
        print("+" + "-" * self.cols + "+  %s" % title)
        print("|%-*s|  %s  screen %d/%d" % (self.cols, "", url, page + 1, pages))
        for _ in range(nrows):
            n = blob[i]; row = blob[i + 1:i + 1 + n]; i += 1 + n
            out, j, style, width = "", 0, 0, 0
            while j < len(row):
                b = row[j]
                if b == 1:
                    out += "[%d:" % ((row[j + 1] << 8) | row[j + 2]); j += 3
                elif b == 2:
                    out += "]"; j += 1
                elif b == 3:
                    new = row[j + 1]; j += 2
                    if style & 1 and not new & 1:
                        out += "*"
                    if new & 1 and not style & 1:
                        out += "*"
                    style = new
                else:
                    out += chr(b); j += 1; width += 1
            assert width <= self.cols, (width, out)
            print("|" + out)
        print("+" + "-" * self.cols + "+  %d bytes" % len(blob))


    def picture(self, b, title):
        """decode as the CD-i does: LZSS through a 4 KB ring, palette entries from 16"""
        w, h, n = (b[0] << 8) | b[1], (b[2] << 8) | b[3], b[4]
        pal = b[5:5 + 3 * n]
        data = b[5 + 3 * n:]
        out, ring, rp = bytearray(), bytearray(4096), 0
        i = 0
        while i < len(data) and len(out) < w * h:
            flags = data[i]; i += 1
            for bit in range(8):
                if i >= len(data) or len(out) >= w * h:
                    break
                if flags & (1 << bit):
                    d = ((data[i] << 4) | (data[i + 1] >> 4)) + 1
                    ln = (data[i + 1] & 15) + 3
                    i += 2
                    for _ in range(ln):
                        c = ring[(rp - d) & 4095]
                        ring[rp & 4095] = c; rp += 1; out.append(c)
                else:
                    c = data[i]; i += 1
                    ring[rp & 4095] = c; rp += 1; out.append(c)
        print("picture %s: %dx%d, %d colours, %d bytes of LZSS, %d of %d pixels"
              % (title, w, h, n, len(data), len(out), w * h))
        try:
            from PIL import Image
            im = Image.frombytes("P", (w, h), bytes(out[:w * h]).ljust(w * h, b"\0"))
            im.putpalette(pal)
            name = "fake_cdi_picture.png"
            im.convert("RGB").resize((w * 2, h * 2), Image.NEAREST).save(name)
            print("saved", name)
        except ImportError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=6400)
    ap.add_argument("--cols", type=int, default=64)
    ap.add_argument("--rows", type=int, default=24)
    ap.add_argument("steps", nargs="*")
    a = ap.parse_args()
    c = FakeCdi(a.port, a.cols, a.rows)
    c.s.sendall(b"\x01junk before hello")      # what the player shell leaves on the line
    t, p = c.request("H", bytes([1, a.cols, a.rows]))
    print("hello:", t, p[1:].decode("latin-1"))
    c.show(c.screen(*c.request("G", b"")))
    steps = list(a.steps)
    while steps:
        cmd = steps.pop(0)
        t0 = time.time()
        if cmd == "go":
            r = c.request("G", steps.pop(0).encode("latin-1"))
        elif cmd == "link":
            n = int(steps.pop(0)); r = c.request("L", bytes([n >> 8, n & 0xFF]))
        elif cmd == "page":
            n = int(steps.pop(0)); r = c.request("P", bytes([n >> 8, n & 0xFF]))
        elif cmd == "back":
            r = c.request("B")
        else:
            raise SystemExit("unknown step " + cmd)
        print("\n>>> %s  (%.1f s)" % (cmd, time.time() - t0))
        c.show(c.screen(*r))
    print("line totals: %d bytes in, %d bytes out, %d requests; at 9600 baud %.1f s of line time"
          % (c.bytes_in, c.bytes_out, c.frames, (c.bytes_in + c.bytes_out) / 960))


if __name__ == "__main__":
    main()
