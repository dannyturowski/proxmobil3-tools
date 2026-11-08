#!/usr/bin/env python3
"""wt32-r3 tools/nori_serial_log.py: sanitizing, partial-line and gap handling, reboot detection, bounded
rotation (unit), and end to end against the r3 simulator over a pty in --host mode (ROM banner text,
HELLO/HB, DIAG pages, a reboot followed by DIAG p=rst / p=prev).  No hardware.  Run via run_tests.sh."""
import os, re, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'tests'))
import nori_serial_log as L  # noqa: E402
import testkit as ti  # noqa: E402

SIM = os.environ.get('NORI_SIM_R3', os.path.join(ti.B, 'nori-esp-sim-r3'))
check = ti.check


class Sink:
    def __init__(self):
        self.lines = []

    def write(self, line):
        self.lines.append(line)


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


def unit():
    t = L.sanitize_frame('NL1 7 TDATA id=1 o=0 d=QUJDREVGRw==')
    check('TDATA payload replaced by its length', t == 'NL1 7 TDATA id=1 o=0 d=<12 b64 chars>', t)
    t = L.sanitize_frame('NL1 8 WIFISET s=U1NJRA== p=c2VjcmV0cHNr')
    check('secret-looking keys redacted', t == 'NL1 8 WIFISET s=<redacted> p=<redacted>', t)
    t = L.sanitize_frame('NL1 9 OK DIAG rx=1 noise=0 crc=0 bad=0 long=0 pg=tof;tof=ranging')
    check('ordinary frames unchanged', t.endswith('pg=tof;tof=ranging'))
    check('escape', L.escape(b'ab\x00\xff\\c') == 'ab\\x00\\xff\\\\c')
    good = L.frame(3, 'HB', 'boot=1234abcd up=5 wifi=up').rstrip(b'\n')
    check('parse a valid frame', L.parse_frame(good)[1] == 'HB')
    check('CRC mismatch is not a frame', L.parse_frame(good[:-1] + (b'0' if good[-1:] != b'0' else b'1')) is None)

    mono = Clock()
    sink = Sink()
    lg = L.Logger(sink, clock=lambda: 1.7e9, mono=mono)
    rb = []
    lg.on_reboot = lambda: rb.append(1)
    lg.feed(L.frame(1, 'HELLO', 'boot=0000aaaa proto=1 fw=x sensor=vl53l5cx btn=2 wifi=up tun=0'))
    lg.feed(L.frame(2, 'HB', 'boot=0000aaaa up=1 wifi=up'))
    lg.feed(b'ets Jun  8 2016 00:22:57\r\n\r\nrst:0x1 (POWERON_RESET)\r\n')
    lg.feed(L.frame(1, 'HELLO', 'boot=0000bbbb proto=1 fw=x sensor=vl53l5cx btn=2 wifi=up tun=0'))
    kinds = [l.split(' ')[2] for l in sink.lines]
    check('frames, ROM text and a reboot', kinds == ['F', 'F', 'T', 'T', 'F', 'R'] and rb == [1], ' '.join(kinds))
    check('R line names both boot ids', '0000aaaa -> 0000bbbb' in sink.lines[-1])
    check('blank lines dropped, ROM text kept', any('rst:0x1 (POWERON_RESET)' in l for l in sink.lines))
    n = len(sink.lines)
    lg.feed(b'Guru Meditation Error: Core  1 panic\'ed (LoadProhibited)')   # no newline: cut by a reset
    lg.idle(5.0)
    check('partial line not written before 0.2 s', len(sink.lines) == n)
    mono.t += 0.25
    lg.idle(5.0)
    check('partial line written after 0.2 s of silence', len(sink.lines) == n + 1 and 'Guru Meditation' in sink.lines[-1])
    mono.t += 5.0
    lg.idle(5.0)
    lg.idle(5.0)
    check('one G line per silence', sum(1 for l in sink.lines if ' G ' in l) == 1)
    lg.feed(b'x' * 1000 + b'\n')
    check('long text line capped', len(sink.lines[-1]) < 520 and '[+600 chars]' in sink.lines[-1])
    lg.feed(b'\x01\x02garbage\x7f\n')
    check('binary escaped', sink.lines[-1].endswith('T \\x01\\x02garbage\\x7f'))

    d = tempfile.mkdtemp(prefix='nori-serlog-rot-')
    open(os.path.join(d, 'keep-me.txt'), 'w').write('x' * 5000)
    clk = Clock()
    rot = L.Rotator(d, 1024, 4096, clock=clk)
    for i in range(2000):
        clk.t += 1
        rot.write('line %05d %s' % (i, 'y' * 40))
    rot.close()
    files = rot.files()
    total = sum(os.path.getsize(p) for p in files)
    check('rotation: total stays within --max-mb', total <= 4096 and len(files) <= 4, f'{len(files)} files, {total} bytes')
    check('rotation: newest data kept', 'line 01999' in open(files[-1]).read())
    check('rotation: unrelated files untouched', os.path.exists(os.path.join(d, 'keep-me.txt')))


class FakeSerialError(Exception):
    pass


class FakeSerial:
    """A port whose reads follow a script: bytes, or an exception (the USB fault)."""
    def __init__(self, script, writes):
        self.script, self.writes, self.closed = list(script), writes, False

    def read(self, n):
        time.sleep(0.01)
        if not self.script:
            return b''
        x = self.script.pop(0)
        if isinstance(x, Exception):
            raise x
        return x

    def write(self, b):
        if self.closed:
            raise FakeSerialError('write on closed port')
        self.writes.append(b)

    def close(self):
        self.closed = True


def usb_faults():
    """Review regression: a USB read fault (CP2102 EPIPE on the PM3) must not end the recording."""
    writes = []
    hello1 = L.frame(1, 'HELLO', 'boot=0000aaaa proto=1 fw=x sensor=vl53l5cx btn=2 wifi=up tun=0')
    hello2 = L.frame(1, 'HELLO', 'boot=0000cccc proto=1 fw=x sensor=vl53l5cx btn=2 wifi=up tun=0')
    sessions = [
        [hello1, b'Guru Meditation Error: Core  1 panic', FakeSerialError('[Errno 32] Broken pipe')],
        FakeSerialError('could not open port: device gone'),
        FakeSerialError('could not open port: device gone'),
        [b"ets Jun  8 2016 00:22:57\r\n", hello2, b'', b''],
    ]
    opens = []

    def opener(port, baud):
        opens.append(time.monotonic())
        x = sessions.pop(0) if sessions else []
        if isinstance(x, Exception):
            raise x
        return FakeSerial(x, writes)

    out = tempfile.mkdtemp(prefix='nori-serlog-usb-')
    rc = L.main(['--port', 'FAKE', '--host', '--duration', '4', '--diag-every', '100', '--out', out, '--pages', 'rst'], opener=opener)
    text = ''.join(open(os.path.join(out, f)).read() for f in sorted(os.listdir(out)))
    check('USB fault: logger keeps running to the end of --duration', rc == 0 and ' I stop ' in text)
    check('USB fault: the read failure is logged with its cause', re.search(r' U read failed: FakeSerialError: \[Errno 32\] Broken pipe', text))
    check('USB fault: bytes received just before the fault are kept', ' T Guru Meditation Error: Core  1 panic' in text)
    check('USB fault: failed reopen attempts logged (bounded: first attempt)', text.count('reopen attempt 1 failed') == 1)
    check('USB fault: reopened, with the time it took', re.search(r' U reopened after \d+\.\d s \(3 attempts\)', text))
    check('USB fault: reopen backoff 0.5 s doubling (>= 0.5 + 0.5 + 1.0 s between opens)', len(opens) == 4 and opens[3] - opens[0] >= 1.9,
          f'{[round(o - opens[0], 2) for o in opens]}')
    check('USB fault: re-handshake (HOST) after the reopen', sum(1 for w in writes if b' HOST proto=1' in w) >= 2)
    check('USB fault: the reboot across the outage is detected', ' R reboot: boot 0000aaaa -> 0000cccc' in text)
    check('USB fault: DIAG p=rst asked for after the reopen', ' I sent DIAG p=rst' in text)
    check('USB fault: ROM text after the reopen kept', ' T ets Jun  8 2016' in text)


def e2e():
    if not os.access(SIM, os.X_OK):
        print('SKIP e2e (no ' + SIM + ')')
        return
    try:
        import serial  # noqa: F401
    except ImportError:
        print('SKIP e2e (no pyserial)')
        return
    esp, out = os.path.join(ti.T, 'esp-log'), os.path.join(ti.T, 'serlog')
    sim = ti.Proc('log-sim', [SIM, '--pty-link', esp, '--baud', '460800', '--sensor', 'vl53l5cx'], stdin=True)
    time.sleep(0.3)
    p = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools', 'nori_serial_log.py'), '--port', esp, '--host',
                          '--diag-every', '2', '--duration', '9', '--out', out, '--file-mb', '1', '--max-mb', '2'],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(1.5)
    sim.cmd('tof near 600')
    time.sleep(1.5)
    sim.cmd('noise')
    time.sleep(1.0)
    sim.cmd('reboot')
    try:
        so, se = p.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        p.kill()
        so, se = p.communicate()
    # tools/nori_wt32_provision.py wt32-r3 commands against the same simulator
    prov = lambda *a: subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'nori_wt32_provision.py'), '--port', esp] + list(a),  # noqa: E731
                                     capture_output=True, text=True, timeout=30)
    r = prov('diag', '--page', 'tof')
    check('provision diag --page tof', r.returncode == 0 and re.search(r'^  pg=tof$', r.stdout, re.M) and
          re.search(r'^  tof=sim$', r.stdout, re.M), (r.stdout + r.stderr)[-300:])
    r = prov('diag', '--page', 'zzz')
    check('provision diag unknown page: rc 1', r.returncode == 1 and 'ERR DIAG' in r.stdout, (r.stdout + r.stderr)[-200:])
    r = prov('prescfg')
    check('provision prescfg', r.returncode == 0 and 'OK PRESCFG near=1000 far=1300 min=60 zones=2 on=3 off=8 stale=1500' in r.stdout,
          (r.stdout + r.stderr)[-200:])
    r = prov('prescfg', '--set', 'near=99')
    check('provision prescfg out of range: ERR, rc 1', r.returncode == 1 and 'ERR PRESCFG' in r.stdout, (r.stdout + r.stderr)[-200:])
    r = prov('sensor', '--restart')
    check('provision sensor --restart', r.returncode == 0 and 'OK SENSOR en=1' in r.stdout, (r.stdout + r.stderr)[-200:])
    sim.cmd('quit'); sim.stop()
    files = sorted(os.listdir(out)) if os.path.isdir(out) else []
    text = ''.join(open(os.path.join(out, f)).read() for f in files)
    check('logger exited cleanly', p.returncode == 0, se.strip()[-200:])
    check('one log file, named by the tool', len(files) == 1 and files[0].startswith(L.PREFIX))
    check('HELLO and HB frames logged', re.search(r' F NL1 \d+ HELLO boot=', text) and re.search(r' F NL1 \d+ HB boot=', text))
    check('DIAG pages answered (host mode)', re.search(r' F NL1 \d+ OK DIAG rx=\d+ .*pg=tof;', text) is not None)
    check('presence event logged', re.search(r' F NL1 \d+ EV pon t=\d+', text) is not None)
    check('ROM boot text kept as T lines', re.search(r' T ets Jun  8 2016', text) is not None and ' T rst:0x1 (POWERON_RESET)' in text)
    check('reboot detected (R) and followed by DIAG p=rst and p=prev', re.search(r' R reboot: boot \w+ -> \w+', text) is not None and
          re.search(r'R reboot[\s\S]*I sent DIAG p=rst[\s\S]*I sent DIAG p=prev', text) is not None)
    check('timestamps: UTC ISO + monotonic offset', re.match(r'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}\+00:00 \+\d+\.\d{3} I start ', text) is not None)
    check('logger sent nothing but HOST / HHB / DIAG', all(re.match(r'sent (HOST proto=1 role=app tun=0|DIAG( p=[a-z0-9]+)?)$', l.split(' I ', 1)[1])
                                                           for l in text.splitlines() if ' I sent ' in l))


def main():
    unit()
    usb_faults()
    e2e()
    bad = [n for n, ok in ti.RESULTS if not ok]
    print(f'serial-log: {len(ti.RESULTS)} checks, {len(bad)} failures (logs in {ti.T})')
    ti.cleanup()
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
