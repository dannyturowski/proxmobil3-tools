"""Simulated devices for the end-to-end tests: each one sits on a pty master and the
tool under test opens the pty slave as if it were the real serial port.

FakeReader   PM3 contactless reader module (INIT IPP framing)
FakeScanner  Code CR8200-style barcode engine (illumination commands only)
FakeWt32     ESP32 accessory speaking link protocol 1 ($NL1 ... *CRC16)

None of these model real card data: card frames carry fixed filler bytes.
"""

import os
import pty
import select
import threading
import time
import tty


# ---- IPP ---------------------------------------------------------------------------------
def crc8(data: bytes) -> int:
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0x8C if c & 1 else c >> 1
    return c


def crc32_jam(data: bytes) -> int:
    c = 0xFFFFFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
    return c


def ipp_frame(seq: int, reply_to: int, mtype: int, payload: bytes = b"") -> bytes:
    hdr = bytes([0xBC, seq & 0xFF, reply_to & 0xFF, mtype, len(payload) >> 8, len(payload) & 0xFF])
    out = hdr + bytes([crc8(hdr)]) + payload
    if mtype >= 0x80:
        out += crc32_jam(payload).to_bytes(4, "little")
    return out


class IppParser:
    def __init__(self):
        self.buf = b""

    def feed(self, data: bytes):
        self.buf += data
        frames = []
        while True:
            i = self.buf.find(b"\xBC")
            if i < 0:
                self.buf = b""
                break
            self.buf = self.buf[i:]
            if len(self.buf) < 7:
                break
            if crc8(self.buf[:6]) != self.buf[6]:
                self.buf = self.buf[1:]
                continue
            n = (self.buf[4] << 8) | self.buf[5]
            total = 7 + n + (4 if self.buf[3] >= 0x80 else 0)
            if len(self.buf) < total:
                break
            frames.append({"seq": self.buf[1], "reply_to": self.buf[2], "type": self.buf[3],
                           "payload": self.buf[7:7 + n]})
            self.buf = self.buf[total:]
        return frames


class PtyDevice:
    """A pty whose master side is serviced by a thread."""

    def __init__(self):
        self.master, slave = pty.openpty()
        tty.setraw(self.master)
        self.path = os.ttyname(slave)
        self._slave = slave            # kept open until release_slave(), see FakeReader docs
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self._loop, daemon=True)

    def release_slave(self):
        if self._slave is not None:
            os.close(self._slave)
            self._slave = None

    def start(self):
        self.thread.start()
        return self

    def write(self, data: bytes):
        with self.lock:
            try:
                os.write(self.master, data)
            except OSError:
                pass

    def close(self):
        self.stop.set()
        if self.thread.is_alive():
            self.thread.join(timeout=2)
        self.release_slave()
        os.close(self.master)

    def _loop(self):
        while not self.stop.is_set():
            r, _, _ = select.select([self.master], [], [], 0.02)
            self.tick(time.monotonic())
            if not r:
                continue
            try:
                data = os.read(self.master, 4096)
            except OSError:            # no slave open at the moment
                time.sleep(0.02)
                continue
            if data:
                self.on_bytes(data, time.monotonic())

    def tick(self, now):
        pass

    def on_bytes(self, data, now):
        pass


class FakeReader(PtyDevice):
    """Answers version/status/function-enable, sends heartbeats, records what it got."""

    def __init__(self, answer_status=True):
        super().__init__()
        self.seq = 0
        self.parser = IppParser()
        self.received = []
        self.answer_status = answer_status
        self.next_hb = 0.0

    def _next_seq(self):
        self.seq = self.seq % 255 + 1
        return self.seq

    def send(self, mtype, payload=b"", reply_to=0):
        seq = self._next_seq()
        self.write(ipp_frame(seq, reply_to, mtype, payload))
        return seq

    def tick(self, now):
        if now >= self.next_hb:
            self.next_hb = now + 2.0
            self.send(0x07)

    def on_bytes(self, data, now):
        for f in self.parser.feed(data):
            f["at"] = now
            self.received.append(f)
            t = f["type"]
            if t == 0x02:
                self.send(0x03, b"FAKE-READER-1", f["seq"])
            elif t == 0x04 and self.answer_status:
                self.send(0x05, b"\x00\x00", f["seq"])
            elif t == 0xE4 and len(f["payload"]) == 4:
                self.send(0xE5, bytes([0, f["payload"][1], 0, 0]), f["seq"])
            elif t in (0x32, 0x46):
                self.send(0xB3, b"\x00")

    def types(self):
        return [f["type"] for f in self.received]

    def card(self, mtype=0xB1):
        # Fixed filler, not a real UID.
        return self.send(mtype, bytes([0x00, 0x00, 0xC0, 0xFF, 0xEE, 0xC0, 0xFF, 0xEE, 0x00, 0x00]))

    def phone(self):
        return self.send(0xD1, bytes([0x5A]) * 63)


class FakeScanner(PtyDevice):
    def __init__(self, level=25, mute=False):
        super().__init__()
        self.level = level
        self.mute = mute
        self.lines = []
        self.buf = b""

    def on_bytes(self, data, now):
        self.buf += data
        while b"\r\n" in self.buf:
            line, self.buf = self.buf.split(b"\r\n", 1)
            self.lines.append(line.decode("ascii", "replace"))
            if self.mute:
                continue
            if line.startswith(b"RDILPMB") and line[7:].isdigit():
                self.level = int(line[7:])
                self.write(b"\x06")
            elif line == b"RDILG":
                self.write(b'<RD><IL LO="0" MB="%d" AC="0" /></RD>\r\n' % self.level)


# ---- link protocol 1 ---------------------------------------------------------------------
def crc16_ccitt_false(data: bytes) -> int:
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def nl_line(seq: int, kind: str, args: str = "") -> bytes:
    body = f"NL1 {seq} {kind}" + (f" {args}" if args else "")
    return f"${body}*{crc16_ccitt_false(body.encode()):04X}\n".encode()


def nl_parse(line: str):
    if not line.startswith("$") or "*" not in line:
        return None
    body, crc = line[1:].rsplit("*", 1)
    if f"{crc16_ccitt_false(body.encode()):04X}" != crc.strip().upper():
        return None
    parts = body.split(" ")
    return {"seq": int(parts[1]), "type": parts[2], "args": parts[3:]}


class FakeWt32(PtyDevice):
    def __init__(self, boot="1a2b3c4d"):
        super().__init__()
        self.boot = boot
        self.seq = 0
        self.t0 = time.monotonic()
        self.host = False
        self.next_hb = 0.0
        self.buf = b""
        self.frames = []
        self.pending_events = []

    def _send(self, kind, args=""):
        self.seq += 1
        self.write(nl_line(self.seq, kind, args))

    def ms(self):
        return int((time.monotonic() - self.t0) * 1000)

    def event(self, name):
        self.pending_events.append(name)

    def tick(self, now):
        if self.host and now >= self.next_hb:
            self.next_hb = now + 1.0
            self._send("HB", f"boot={self.boot} up={self.ms()} wifi=off tun=idle evdrop=0")
        while self.host and self.pending_events:
            self._send("EV", f"{self.pending_events.pop(0)} t={self.ms()}")

    def on_bytes(self, data, now):
        self.buf += data
        while b"\n" in self.buf:
            raw, self.buf = self.buf.split(b"\n", 1)
            f = nl_parse(raw.decode("ascii", "replace").strip())
            if not f:
                continue
            self.frames.append(f)
            if f["type"] == "HOST":
                self.host = True
                self._send("HELLO", f"boot={self.boot} proto=1 fw=sim-1.0 sensor=none btn=2 wifi=off tun=0")
            elif f["type"] == "DIAG":
                page = next((a[2:] for a in f["args"] if a.startswith("p=")), None)
                if page and page != "net":
                    self._send("ERR", "DIAG page")
                else:
                    self._send("OK", "DIAG rx=%d noise=0 crc=0 bad=0 long=0" % len(self.frames))
            elif f["type"] == "CFG":
                self._send("OK", "CFG ths=8 wait=200 refr=500")
