"""End-to-end tests of the built tools against simulated devices on ptys.

Run from the repository root after `make`:  python3 -m unittest discover -s tests/e2e -v
The binaries are taken from $PM3_TOOLS_BIN (default build/native).  No hardware is used.
"""

import json
import os
import select
import stat
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import fakes  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BIN = os.environ.get("PM3_TOOLS_BIN", os.path.join(ROOT, "build", "native"))


def tool(name):
    return os.path.join(BIN, name)


def events(text):
    out = []
    for line in text.splitlines():
        if line.strip():
            out.append(json.loads(line))       # every stdout line must be valid JSON
    return out


def run_until(proc, predicate, timeout):
    """Read JSON lines from a running tool until predicate(events) or timeout."""
    seen = []
    end = time.monotonic() + timeout
    fd = proc.stdout.fileno()
    buf = b""
    while time.monotonic() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if not r:
            if proc.poll() is not None:
                break
            continue
        chunk = os.read(fd, 65536)
        if not chunk:
            break
        buf += chunk
        while b"\n" in buf:                 # stop at a line boundary; the rest stays for communicate()
            line, buf = buf.split(b"\n", 1)
            if line.strip():
                seen.append(json.loads(line))
            if predicate(seen):
                while buf and not buf.endswith(b"\n"):     # finish a partial line before handing over
                    more = os.read(fd, 65536)
                    if not more:
                        break
                    buf += more
                seen.extend(events(buf.decode()))
                return seen
    if buf.strip():
        seen.extend(events(buf.decode()))
    return seen


class NfcPresenceTest(unittest.TestCase):
    def test_refuses_port_open_elsewhere(self):
        dev = fakes.FakeReader()           # the test process still holds the slave open
        try:
            p = subprocess.run([tool("pm3-nfc-presence"), "--device", dev.path, "--duration", "1"],
                               capture_output=True, text=True, timeout=10)
            self.assertEqual(p.returncode, 3)
            self.assertIn("already open by pid", p.stderr)
            self.assertEqual(p.stdout, "")
        finally:
            dev.close()

    def test_card_and_phone_presence(self):
        dev = fakes.FakeReader()
        dev.release_slave()
        dev.start()
        proc = subprocess.Popen([tool("pm3-nfc-presence"), "--device", dev.path, "--duration", "14",
                                 "--health-every", "5"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            seen = run_until(proc, lambda ev: any(e["event"] == "ready" for e in ev), 8)
            self.assertTrue(any(e["event"] == "ready" for e in seen), seen)
            card_seq = dev.card()
            for _ in range(5):              # held card re-reports
                time.sleep(0.2)
                dev.card()
            seen += run_until(proc, lambda ev: any(e["event"] == "absent" for e in ev), 6)
            time.sleep(1.8)                 # past the 1.5 s grace after the tool's rearm abort
            dev.phone()
            seen += run_until(proc, lambda ev: any(e.get("kind") == "phone_or_payment" for e in ev), 4)
            out, err = proc.communicate(timeout=20)
            seen += events(out)
        finally:
            if proc.poll() is None:
                proc.kill()
            dev.close()
        self.assertEqual(proc.returncode, 0, err)
        kinds = [e["event"] for e in seen]
        self.assertEqual(kinds[0], "start")
        self.assertEqual(kinds[-1], "exit")
        cards = [e for e in seen if e["event"] == "presence" and e["kind"] == "card"]
        self.assertEqual(len(cards), 1, cards)
        self.assertEqual(cards[0]["outcome"], "accepted")
        self.assertEqual(cards[0]["frame"], "iso-read")
        absent = [e for e in seen if e["event"] == "absent"]
        self.assertEqual(absent[0]["reason"], "timeout")
        phones = [e for e in seen if e["event"] == "presence" and e["kind"] == "phone_or_payment"]
        self.assertEqual(len(phones), 1, seen)
        health = [e for e in seen if e["event"] == "health"]
        self.assertTrue(health and health[-1]["cmd"] == "verified" and health[-1]["configured"] is True, health)
        self.assertGreaterEqual(health[-1]["held"], 5)
        # What the reader received: setup order, one release answering the card, a rearm abort, clean exit.
        rx = dev.received
        types = [f["type"] for f in rx]
        self.assertEqual(types[:4], [0x46, 0x32, 0x02, 0x04])
        enables = [f["payload"][1] for f in rx if f["type"] == 0xE4]
        self.assertEqual(sorted(set(enables)), [6, 7, 10])
        releases = [f for f in rx if f["type"] == 0x32 and f["reply_to"] == card_seq]
        self.assertEqual(len(releases), 1)
        self.assertGreaterEqual(types.count(0x46), 3)          # setup, rearm, exit
        self.assertEqual(types[-2:], [0x46, 0x32])
        self.assertTrue(set(types) <= {0x02, 0x04, 0x32, 0x46, 0xE4}, set(types))
        # No card bytes in the output.
        text = json.dumps(seen).lower()
        for needle in ("c0ffee", "c0 ff", "5a5a", "fake-reader"):
            self.assertNotIn(needle, text)

    def test_usage(self):
        p = subprocess.run([tool("pm3-nfc-presence"), "--funcs", "x"], capture_output=True, text=True)
        self.assertEqual(p.returncode, 2)
        p = subprocess.run([tool("pm3-nfc-presence"), "--device", "/nonexistent/tty"], capture_output=True, text=True)
        self.assertEqual(p.returncode, 3)


class Pic32Test(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def fake(self, body):
        path = os.path.join(self.dir, "NxExe")
        with open(path, "w") as f:
            f.write("#!/bin/sh\n" + textwrap.dedent(body))
        os.chmod(path, stat.S_IRWXU)
        return path

    def run_tool(self, *args, nxexe):
        return subprocess.run([tool("pm3-pic32-read"), "--nxexe", nxexe, "--workdir", self.dir, *args],
                              capture_output=True, text=True, timeout=20)

    def test_reads_and_args(self):
        nx = self.fake("""\
            echo "$@" >> args.log
            echo "PIC32: com/usb/available 8/0/1"
            case "$3" in
              0x500b) printf 'Addr 20491:\\t 48\\r\\n' ;;
              0x5040) echo 'Addr 20544: 12087' ;;
              *) echo bad; exit 1 ;;
            esac
            """)
        p = self.run_tool("ambient", "input-voltage", nxexe=nx)
        self.assertEqual(p.returncode, 0, p.stderr)
        ev = events(p.stdout)
        self.assertEqual([(e["register"], e["value"]) for e in ev], [("ambient", 48), ("input-voltage", 12087)])
        with open(os.path.join(self.dir, "args.log")) as f:
            self.assertEqual(f.read().split("\n")[:2], ["prox2 read 0x500b", "prox2 read 0x5040"])

    def test_failures(self):
        cases = [("exit 1\n", "exit=1"), ("echo 'Addr 20491: error'\n", "parse"),
                 ("kill -9 $$\n", "signal=9"), ("sleep 20\n", "timeout")]
        for body, reason in cases:
            with self.subTest(reason=reason):
                t = time.monotonic()
                p = self.run_tool("--timeout", "1", "ambient", nxexe=self.fake(body))
                self.assertEqual(p.returncode, 1)
                ev = events(p.stdout)
                self.assertEqual(ev[0]["event"], "pic32_error")
                self.assertEqual(ev[0]["reason"], reason)
                self.assertLess(time.monotonic() - t, 5)
        p = self.run_tool("ambient", nxexe=os.path.join(self.dir, "missing"))
        self.assertEqual(events(p.stdout)[0]["reason"], "exit=127")

    def test_rounds_and_usage(self):
        nx = self.fake("echo 'Addr 20491: 7'\n")
        p = self.run_tool("--count", "2", "--interval", "1", "ambient", nxexe=nx)
        self.assertEqual(len(events(p.stdout)), 2)
        self.assertEqual(subprocess.run([tool("pm3-pic32-read"), "backlight"], capture_output=True).returncode, 2)
        listed = events(subprocess.run([tool("pm3-pic32-read"), "--list"], capture_output=True, text=True).stdout)
        self.assertEqual([e["addr"] for e in listed], ["0x500b", "0x5040"])


class ScannerLightTest(unittest.TestCase):
    def run_tool(self, dev, *args, wait="1"):
        return subprocess.run([tool("pm3-scanner-light"), "--device", dev.path, "--any-usb-id", "--wait", wait, *args],
                              capture_output=True, text=True, timeout=15)

    def test_off_level_get(self):
        dev = fakes.FakeScanner(level=25)
        dev.release_slave()
        dev.start()
        try:
            p = self.run_tool(dev, "get")
            self.assertEqual((p.returncode, events(p.stdout)[0]["level"]), (0, 25), p.stderr)
            p = self.run_tool(dev, "off")
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(events(p.stdout)[0], {"event": "scanner_light", "action": "set", "requested": 0,
                                                   "readback": 0, "confirmed": True})
            p = self.run_tool(dev, "level", "60")
            self.assertEqual((p.returncode, events(p.stdout)[0]["readback"]), (0, 60))
            self.assertEqual(dev.lines, ["RDILG", "RDILPMB0", "RDILG", "RDILPMB60", "RDILG"])
        finally:
            dev.close()

    def test_no_reply_and_refusals(self):
        dev = fakes.FakeScanner(mute=True)
        dev.release_slave()
        dev.start()
        try:
            p = self.run_tool(dev, "off", wait="0.5")
            self.assertEqual(p.returncode, 1)
            self.assertEqual(events(p.stdout)[0]["readback"], "none")
            p = self.run_tool(dev, "--no-readback", "off")
            self.assertEqual((p.returncode, events(p.stdout)[0]["readback"]), (0, "skipped"))
            p = subprocess.run([tool("pm3-scanner-light"), "--device", dev.path, "off"], capture_output=True, text=True)
            self.assertEqual(p.returncode, 3)
            self.assertIn("not a USB serial device", p.stderr)
            for bad in (["level", "101"], ["level"], ["blink"], ["--no-readback", "get"]):
                self.assertEqual(self.run_tool(dev, *bad).returncode, 2, bad)
        finally:
            dev.close()
        busy = fakes.FakeScanner()        # slave still held by this process
        try:
            p = self.run_tool(busy, "off")
            self.assertEqual(p.returncode, 3)
            self.assertIn("already open by pid", p.stderr)
        finally:
            busy.close()


class Wt32MonitorTest(unittest.TestCase):
    def test_handshake_events_and_reply(self):
        dev = fakes.FakeWt32()
        dev.release_slave()
        dev.start()
        proc = subprocess.Popen([tool("wt32-monitor"), "--device", dev.path, "--allow-pty", "--cmd", "DIAG",
                                 "--duration", "6", "--stdin", "--heartbeats"],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            seen = run_until(proc, lambda ev: any(e["event"] == "reply" for e in ev), 5)
            dev.event("b1d")
            dev.event("b1u")
            dev.event("trk5")
            proc.stdin.write("CFG ths=3\nWIFISET s=AAAA p=BBBB\nDIAG p=bogus\n")
            proc.stdin.flush()
            out, err = proc.communicate(timeout=15)
            seen += events(out)
        finally:
            if proc.poll() is None:
                proc.kill()
            dev.close()
        self.assertEqual(proc.returncode, 0, err)
        hello = next(e for e in seen if e["event"] == "hello")
        self.assertEqual((hello["boot"], hello["fw"], hello["tun_cap"]), ("1a2b3c4d", "sim-1.0", False))
        replies = [e for e in seen if e["event"] == "reply"]
        self.assertEqual(replies[0]["status"], "OK")
        self.assertEqual(replies[0]["what"], "DIAG")
        self.assertTrue(any(r["status"] == "ERR" and r["args"] == ["page"] for r in replies), replies)
        self.assertEqual([e["name"] for e in seen if e["event"] == "input"], ["b1d", "b1u", "trk5"])
        refused = [e["what"] for e in seen if e["event"] == "command_refused"]
        self.assertEqual(refused, ["CFG", "WIFISET"])
        self.assertTrue(any(e["event"] == "heartbeat" for e in seen))
        sent = [f["type"] for f in dev.frames]
        self.assertIn("HOST", sent)
        self.assertNotIn("CFG", sent)
        self.assertNotIn("WIFISET", sent)
        host = next(f for f in dev.frames if f["type"] == "HOST")
        self.assertIn("proto=1", host["args"])

    def test_until_reply_and_refusals(self):
        dev = fakes.FakeWt32()
        dev.release_slave()
        dev.start()
        try:
            p = subprocess.run([tool("wt32-monitor"), "--device", dev.path, "--allow-pty", "--cmd", "DIAG", "--until-reply"],
                               capture_output=True, text=True, timeout=20)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(events(p.stdout)[-1]["event"], "exit")
            p = subprocess.run([tool("wt32-monitor"), "--device", dev.path, "--allow-pty", "--cmd", "WIFISET s=x p=y"],
                               capture_output=True, text=True, timeout=5)
            self.assertEqual(p.returncode, 2)
            p = subprocess.run([tool("wt32-monitor"), "--device", dev.path, "--cmd", "DIAG"], capture_output=True, text=True, timeout=5)
            self.assertEqual(p.returncode, 3)                  # a pty without --allow-pty
            self.assertIn("refusing", p.stderr)
        finally:
            dev.close()
        silent = fakes.PtyDevice()         # nothing answers
        silent.release_slave()
        silent.start()
        try:
            p = subprocess.run([tool("wt32-monitor"), "--device", silent.path, "--allow-pty", "--cmd", "DIAG",
                                "--until-reply", "--duration", "2"], capture_output=True, text=True, timeout=10)
            self.assertEqual(p.returncode, 5)
        finally:
            silent.close()


if __name__ == "__main__":
    unittest.main()
