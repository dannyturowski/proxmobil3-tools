#!/usr/bin/env python3
"""wt32-r3: timestamped, bounded, sanitized log of everything the WT32 sends on its UART (pyserial;
Windows or Linux).  Its purpose is to keep on the PC what the board can't keep itself: the ROM reset
banner, a panic / watchdog dump, and the frames around a reset.  Nothing is stored on the board.

  nori_serial_log.py --port COM20                      passive: log only (the board says HELLO every 3 s)
  nori_serial_log.py --port COM20 --host               also be the host: HOST + HHB every second, so the
                                                       board sends HB, and DIAG pages every --diag-every s
                                                       and right after every reboot it sees
  nori_serial_log.py --list                            serial ports

Each line:  <UTC ISO time> <seconds since start> <kind> <text>
  F  a valid link frame (CRC checked), sanitized
  T  any other text: ROM banner, panic dump, line noise; non-printable bytes as \\xNN; a partial line
     is written after 0.2 s of silence, so a dump cut off by a reset is kept
  R  the board's boot id changed (HELLO / HB boot=): a reset happened
  G  no byte for --gap seconds (the board said nothing at all)
  I  logger notes (start, rotation, settings, what it sent: only HOST / HHB / DIAG p=...)
  U  the serial port / USB failed (read, write, reopen; e.g. the CP2102 EPIPE seen on the PM3) and when it
     came back.  The logger never exits on such a fault: it keeps the bytes received so far, retries the
     open every 0.5 s doubling to 10 s (within --duration), deasserts DTR/RTS again, re-handshakes in
     --host mode and asks for DIAG pages at once.

Bounded: files of --file-mb MB, at most --max-mb MB in total; the oldest of THIS tool's files
(nori-wt32-serial-*.log in --out) are deleted first.  Nothing else in the directory is touched.

Sanitized: TDATA payloads (SSH tunnel bytes) are replaced by their length; any key=value token whose
key could carry a secret (p s h psk pass password key) is redacted, although the firmware never sends
one (WIFISET / OTAKEY are host-to-board only and never echoed).  The logger itself never sends a
credential or a configuration command.  DTR / RTS are held deasserted: opening the port never resets
the board or pulls GPIO0 low.  The port is exclusive: stop nori-linkd / other tools first.

DIAG over the UART: wt32-r3 answers DIAG and DIAG p=tof|net|rst|ev|prev; wt32-r4 adds p=rt|dhcp, wt32-r5 p=tog (see the firmware README).
At 460800 baud the ROM banner (115200 baud) shows up as garbage; the reset reason is also in DIAG
p=rst (rst=<code>/<name>), which the logger asks for after every reboot in --host mode."""
import argparse
import binascii
import datetime
import glob
import os
import re
import sys
import time

FRAME = re.compile(rb'\$(NL1 [!-~ ]*?)\*([0-9A-Fa-f]{4})\r?$')
SECRET_KEYS = {'p', 's', 'h', 'psk', 'pass', 'password', 'key'}
PREFIX = 'nori-wt32-serial-'


def default_dir():
    base = os.environ.get('LOCALAPPDATA')
    if base:
        return os.path.join(base, 'Nori', 'logs', 'wt32')
    return os.path.join(os.path.expanduser('~'), '.local', 'state', 'nori', 'logs', 'wt32')


def escape(b):
    """bytes -> printable ASCII; everything else as \\xNN."""
    out = []
    for c in b:
        if 0x20 <= c < 0x7f and c != 0x5c:
            out.append(chr(c))
        elif c == 0x5c:
            out.append('\\\\')
        else:
            out.append('\\x%02x' % c)
    return ''.join(out)


def sanitize_frame(text):
    """'NL1 <seq> <TYPE> args...' -> the same with tunnel payloads and secret-looking values removed."""
    parts = text.split(' ')
    if len(parts) < 3:
        return text
    kind = parts[2]
    out = parts[:3]
    for tok in parts[3:]:
        k, eq, v = tok.partition('=')
        if eq and kind == 'TDATA' and k == 'd':
            out.append('d=<%d b64 chars>' % len(v))
        elif eq and k.lower() in SECRET_KEYS:
            out.append('%s=<redacted>' % k)
        else:
            out.append(tok)
    return ' '.join(out)


def parse_frame(line):
    """bytes (no newline) -> (text, kind, args dict) for a valid frame, else None."""
    m = FRAME.fullmatch(line.rstrip(b'\r'))
    if not m or binascii.crc_hqx(m.group(1), 0xFFFF) != int(m.group(2), 16):
        return None
    text = m.group(1).decode('ascii')
    parts = text.split(' ')
    if len(parts) < 3:
        return None
    args = {}
    for tok in parts[3:]:
        k, eq, v = tok.partition('=')
        if eq:
            args[k] = v
    return text, parts[2], args


class Rotator:
    def __init__(self, out, file_bytes, max_bytes, clock=time.time):
        self.out, self.file_bytes, self.max_bytes, self.clock = out, file_bytes, max_bytes, clock
        os.makedirs(out, exist_ok=True)
        self.f, self.size, self.n = None, 0, 0
        self.open_new()

    def files(self):
        return sorted(glob.glob(os.path.join(self.out, PREFIX + '*.log')))

    def open_new(self):
        if self.f:
            self.f.close()
        stamp = datetime.datetime.fromtimestamp(self.clock(), datetime.timezone.utc).strftime('%Y%m%d-%H%M%S')
        self.n += 1
        path = os.path.join(self.out, '%s%s-%03d.log' % (PREFIX, stamp, self.n))
        self.f = open(path, 'a', encoding='ascii', errors='replace', newline='\n')
        self.path, self.size = path, 0
        self.prune()

    def prune(self):
        files = self.files()
        total = sum(os.path.getsize(p) for p in files)
        for p in files:
            if total <= self.max_bytes - self.file_bytes or p == self.path:
                break
            total -= os.path.getsize(p)
            os.remove(p)

    def write(self, line):
        data = line + '\n'
        if self.size + len(data) > self.file_bytes and self.size:
            self.open_new()
        self.f.write(data)
        self.f.flush()
        self.size += len(data)

    def close(self):
        if self.f:
            self.f.close()
            self.f = None


class Logger:
    LINE_MAX = 400

    def __init__(self, sink, t0=None, clock=time.time, mono=time.monotonic):
        self.sink, self.clock, self.mono = sink, clock, mono
        self.t0 = mono() if t0 is None else t0
        self.buf = b''
        self.last_rx = mono()
        self.gap_logged = False
        self.boot = None
        self.reboots = 0
        self.frames = self.text_lines = 0
        self.on_reboot = None

    def emit(self, kind, text):
        now = self.clock()
        iso = datetime.datetime.fromtimestamp(now, datetime.timezone.utc).isoformat(timespec='milliseconds')
        self.sink.write('%s %+.3f %s %s' % (iso, self.mono() - self.t0, kind, text))

    def line(self, raw):
        fr = parse_frame(raw)
        if fr:
            text, kind, args = fr
            self.frames += 1
            self.emit('F', sanitize_frame(text))
            boot = args.get('boot') if kind in ('HELLO', 'HB') else None
            if boot:
                if self.boot and boot != self.boot:
                    self.reboots += 1
                    self.emit('R', 'reboot: boot %s -> %s (#%d this run)' % (self.boot, boot, self.reboots))
                    if self.on_reboot:
                        self.on_reboot()
                self.boot = boot
            return
        if not raw.strip(b'\r'):
            return
        self.text_lines += 1
        s = escape(raw.rstrip(b'\r'))
        if len(s) > self.LINE_MAX:
            s = s[:self.LINE_MAX] + ' ...[+%d chars]' % (len(s) - self.LINE_MAX)
        self.emit('T', s)

    def feed(self, data):
        if data:
            self.last_rx = self.mono()
            self.gap_logged = False
            self.buf += data
            while b'\n' in self.buf:
                raw, self.buf = self.buf.split(b'\n', 1)
                self.line(raw)
            if len(self.buf) > 4096:                       # no newline for a long time: keep it anyway
                self.line(self.buf)
                self.buf = b''

    def idle(self, gap_s):
        quiet = self.mono() - self.last_rx
        if self.buf and quiet >= 0.2:                      # a partial line (e.g. a dump cut by a reset)
            self.line(self.buf)
            self.buf = b''
        if gap_s and quiet >= gap_s and not self.gap_logged:
            self.gap_logged = True
            self.emit('G', 'no byte for %.0f s' % quiet)


def frame(seq, kind, args=''):
    body = ('NL1 %d %s%s' % (seq, kind, ' ' + args if args else '')).encode('ascii')
    return b'$' + body + b'*%04X\n' % binascii.crc_hqx(body, 0xFFFF)


def open_port(port, baud):
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, baud, 0.05
    s.dtr = False                                       # before open: no reset, GPIO0 not pulled low
    s.rts = False
    s.open()
    return s


RECONNECT_MIN_S, RECONNECT_MAX_S = 0.5, 10.0


def main(argv=None, opener=None, sleep=time.sleep):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--port')
    ap.add_argument('--baud', type=int, default=460800)
    ap.add_argument('--out', default=default_dir())
    ap.add_argument('--file-mb', type=float, default=8.0)
    ap.add_argument('--max-mb', type=float, default=64.0)
    ap.add_argument('--host', action='store_true', help='act as the link host (HOST/HHB) and ask for DIAG pages')
    ap.add_argument('--diag-every', type=float, default=60.0)
    ap.add_argument('--pages', default='tof,net,wifi,wifip,rt,dhcp,rst,tog,ph', help='DIAG pages asked for besides plain DIAG (wt32-r3; rt,dhcp: wt32-r4; tog: wt32-r5; wifi,wifip: wt32-r5.1; ph: wt32-r5.1.3, older firmware answers ERR)')
    ap.add_argument('--gap', type=float, default=5.0, help='log a G line after this many silent seconds')
    ap.add_argument('--duration', type=float, default=0.0, help='stop after this many seconds (0: until Ctrl-C)')
    a = ap.parse_args(argv)
    if a.list:
        from serial.tools import list_ports
        for x in list_ports.comports():
            print('%s  %s  %s' % (x.device, '%04x:%04x' % (x.vid, x.pid) if x.vid else '-', x.description))
        return 0
    if not a.port:
        ap.error('--port is required')
    pages = [p for p in a.pages.split(',') if p]
    if any(not re.fullmatch(r'[a-z0-9]{1,8}', p) for p in pages):
        ap.error('--pages: names of 1..8 lower-case letters / digits')
    if not (0 < a.file_mb <= a.max_mb):
        ap.error('need 0 < --file-mb <= --max-mb')
    rot = Rotator(a.out, int(a.file_mb * 1024 * 1024), int(a.max_mb * 1024 * 1024))
    lg = Logger(rot)
    opener = opener or open_port
    try:
        port = opener(a.port, a.baud)
    except Exception as e:                              # pyserial's SerialException carries the OS reason
        rot.close()
        sys.stderr.write('nori_serial_log: cannot open %s: %s\n' % (a.port, e))
        return 2
    lg.emit('I', 'start port=%s baud=%d host=%d diag_every=%.0f pages=%s file_mb=%g max_mb=%g' %
            (a.port, a.baud, a.host, a.diag_every, ','.join(pages), a.file_mb, a.max_mb))
    sys.stderr.write('logging %s to %s (Ctrl-C to stop)\n' % (a.port, rot.path))
    st = {'s': port, 'seq': 0, 'lost_at': 0.0, 'tries': 0, 'next_try': 0.0, 'backoff': RECONNECT_MIN_S, 'opens': 1}
    want_diag = [False]
    lg.on_reboot = lambda: want_diag.__setitem__(0, True)

    def detail(e):
        return ('%s: %s' % (type(e).__name__, e))[:200]

    def lost(what, e):
        """USB / port failure: keep what was received, close, and start reconnecting (never fatal)."""
        lg.emit('U', '%s failed: %s' % (what, detail(e)))
        if lg.buf:                                      # the bytes just before the fault are the interesting ones
            lg.line(lg.buf)
            lg.buf = b''
        try:
            st['s'].close()
        except Exception:
            pass
        st.update(s=None, lost_at=time.monotonic(), tries=0, next_try=time.monotonic() + RECONNECT_MIN_S, backoff=RECONNECT_MIN_S)

    def send(kind, args=''):
        if st['s'] is None:
            return False
        st['seq'] += 1
        try:
            st['s'].write(frame(st['seq'], kind, args))
            return True
        except Exception as e:                          # SerialException / OSError: USB gone (EPIPE, unplug)
            lost('write', e)
            return False

    def handshake():
        if a.host and send('HOST', 'proto=1 role=app tun=0'):
            lg.emit('I', 'sent HOST proto=1 role=app tun=0')

    t_end = time.monotonic() + a.duration if a.duration else None
    next_hb = next_diag = time.monotonic()
    diag_q = []
    try:
        handshake()
        while t_end is None or time.monotonic() < t_end:
            now = time.monotonic()
            if st['s'] is None:                         # reconnecting: bounded backoff, inside --duration
                lg.idle(a.gap)
                if now < st['next_try']:
                    sleep(min(0.05, st['next_try'] - now))
                    continue
                st['tries'] += 1
                try:
                    st['s'] = opener(a.port, a.baud)    # DTR/RTS deasserted again by open_port
                except Exception as e:
                    if st['tries'] == 1 or st['tries'] % 10 == 0:
                        lg.emit('U', 'reopen attempt %d failed: %s' % (st['tries'], detail(e)))
                    st['next_try'] = now + st['backoff']
                    st['backoff'] = min(st['backoff'] * 2, RECONNECT_MAX_S)
                    continue
                st['opens'] += 1
                lg.emit('U', 'reopened after %.1f s (%d attempts)' % (now - st['lost_at'], st['tries']))
                handshake()                             # a new host session; reboot detection keeps the old boot id
                want_diag[0] = True
                continue
            try:
                data = st['s'].read(4096)
            except Exception as e:
                lost('read', e)
                continue
            lg.feed(data)
            lg.idle(a.gap)
            if a.host:
                if now >= next_hb:
                    next_hb = now + 1.0
                    send('HHB')
                    if lg.boot is None and int(now) % 3 == 0:
                        send('HOST', 'proto=1 role=app tun=0')
                if want_diag[0] or (a.diag_every and now >= next_diag):
                    want_diag[0] = False
                    next_diag = now + a.diag_every
                    diag_q = [''] + ['p=' + p for p in pages] + (['p=rst', 'p=prev', 'p=ph'] if lg.reboots else [])
                    diag_q = list(dict.fromkeys(diag_q))
                if diag_q and not data:                     # one request at a time, when the line is quiet
                    q = diag_q.pop(0)
                    if send('DIAG', q):
                        lg.emit('I', 'sent DIAG %s' % q if q else 'sent DIAG')
    except KeyboardInterrupt:
        pass
    finally:
        if lg.buf:
            lg.line(lg.buf)
        lg.emit('I', 'stop frames=%d text_lines=%d reboots=%d opens=%d' % (lg.frames, lg.text_lines, lg.reboots, st['opens']))
        if st['s'] is not None:
            try:
                st['s'].close()
            except Exception:
                pass
        rot.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
