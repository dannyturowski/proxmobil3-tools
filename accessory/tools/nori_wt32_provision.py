#!/usr/bin/env python3
"""wt32-r1/r3: provision / inspect a Nori WT32-ETH01 over its UART0 link (pyserial; Windows or Linux).

The WT32-ETH01 has no USB: any 3.3 V USB-UART adapter on TX0/RX0 (GPIO1/GPIO3) works, so the
port is never guessed and the adapter type is not checked.  DTR/RTS are held deasserted so an
auto-reset adapter does not reset the board or pull GPIO0 low.  Same CRC framing as
tools/nori_frame.py and shared/nori_link (CRC-16/CCITT-FALSE).

Secrets: the OTA password is read from a user-only file (the same file nori_ota.py /
nori_http_ota.py use); only its MD5 is sent (OTAKEY).  Neither the password, the hash nor a Wi-Fi
PSK is ever printed: OK/ERR replies are reduced to their command word, except DIAG.

  nori_wt32_provision.py --list                                    serial ports
  nori_wt32_provision.py --port COM7 status                        HELLO + HB + DIAG for a few seconds
  nori_wt32_provision.py --port COM7 otakey --password-file F      store the OTA key (enables OTA)
  nori_wt32_provision.py --port COM7 tunnel --on 1                 SSH tunnel opt-in (WIFIEN on=1)
  nori_wt32_provision.py --port COM7 wifi --wifi-file W.json       make {"ssid","psk"} the primary; other saved networks stay
  nori_wt32_provision.py --port COM7 wificlr                       forget every saved network
                                                                   (several networks: nori_wifi_profiles.py)
  nori_wt32_provision.py newpass F                                 new random password -> F (user-only ACL)
 wt32-r3:
  nori_wt32_provision.py --port COM7 diag --page tof               a DIAG page: tof net rt dhcp rst ev prev
  nori_wt32_provision.py --port COM7 prescfg                       presence thresholds in use
  nori_wt32_provision.py --port COM7 prescfg --set "near=900 far=1200"   change + save (validated together)
  nori_wt32_provision.py --port COM7 sensor [--on 0|1] [--restart] sensor on/off (saved) / re-initialise
"""
import argparse
import base64
import binascii
import hashlib
import json
import os
import re
import secrets
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nori_ota  # noqa: E402  (password file checks: user-only ACL / mode)

FRAME = re.compile(rb'\$(NL1 [!-~ ]*?)\*([0-9A-Fa-f]{4})\r?$')


def die(msg):
    sys.stderr.write('nori_wt32_provision: %s\n' % msg)
    sys.exit(2)


class Link:
    def __init__(self, port, baud):
        import serial
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, baud, 0.1
        self.s.dtr = False             # set before open: pyserial applies them when opening
        self.s.rts = False
        try:
            self.s.open()
        except serial.SerialException as e:
            die('cannot open %s: %s (see --list)' % (port, e))
        self.seq = 0
        self.hb_due = 0.0

    def send(self, kind, args=''):
        self.seq += 1
        body = ('NL1 %d %s%s' % (self.seq, kind, ' ' + args if args else '')).encode('ascii')
        self.s.write(b'$' + body + b'*%04X\n' % binascii.crc_hqx(body, 0xFFFF))

    def frames(self, seconds):
        """Yield (kind, args) of valid device frames for `seconds`, sending HHB every second."""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if time.monotonic() >= self.hb_due:
                self.send('HHB')
                self.hb_due = time.monotonic() + 1.0
            line = self.s.readline().strip()
            m = FRAME.fullmatch(line)
            if not m or binascii.crc_hqx(m.group(1), 0xFFFF) != int(m.group(2), 16):
                continue                   # boot text, noise, partial lines
            parts = m.group(1).decode('ascii').split(' ', 3)
            if len(parts) >= 3:
                yield parts[2], parts[3] if len(parts) > 3 else ''

    def show(self, kind, args):
        if kind in ('HELLO', 'HB'):
            print('%s %s' % (kind, args), flush=True)
        elif kind == 'OK' and args.startswith('DIAG '):
            tok = args.split(' ')
            print('DIAG %s' % ' '.join(tok[1:6]), flush=True)
            if len(tok) > 6:
                print('  ' + ' '.join(tok[6:]).replace(';', '\n  '), flush=True)
        elif kind == 'OK' and args.split(' ', 1)[0] in ('PRESCFG', 'SENSOR'):
            print('OK %s' % args, flush=True)            # thresholds / sensor state: nothing secret
        elif kind in ('OK', 'ERR'):
            print('%s %s' % (kind, args.split(' ', 2)[0] if kind == 'OK' else ' '.join(args.split(' ', 2)[:2])), flush=True)
        elif kind == 'EV':
            print('EV %s' % args, flush=True)

    def run(self, seconds, expect=None):
        """Show frames; with expect, stop at its OK/ERR reply and return True for OK."""
        for kind, args in self.frames(seconds):
            self.show(kind, args)
            if expect and kind in ('OK', 'ERR') and args.split(' ', 1)[0] == expect:
                return kind == 'OK'
        if expect:
            die('no reply to %s (wrong port or baud, device not running, or TX/RX swapped?)' % expect)
        return True

    def handshake(self):
        self.send('HOST', 'proto=1 role=app tun=0')
        for kind, args in self.frames(3):
            self.show(kind, args)
            if kind == 'HELLO':
                return
        die('no HELLO within 3 s (check port, 460800 baud, TX0/RX0 wiring and that the board is running)')


def new_password_file(path):
    if os.path.exists(path):
        die('%s already exists; not overwriting' % path)
    pw = secrets.token_urlsafe(24)     # 32 chars
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as f:
        f.write(pw + '\n')
    del pw
    if os.name == 'nt':                # user-only NTFS ACL, as nori_ota.read_password_file requires
        sid = subprocess.run(['whoami', '/user', '/fo', 'csv', '/nh'], capture_output=True, text=True, check=True).stdout
        sid = sid.strip().split(',')[-1].strip('"')
        r = subprocess.run(['icacls', path, '/inheritance:r', '/grant:r', '*%s:F' % sid], capture_output=True, text=True)
        if r.returncode:
            os.remove(path)
            die('icacls failed; file removed: %s' % r.stderr.strip())
    nori_ota.read_password_file(path)  # verifies the ACL / mode exactly as the OTA tools will
    print('new OTA password written to %s (user-only); not shown' % path)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--list', action='store_true', help='list serial ports and exit')
    p.add_argument('--port', help='serial port, e.g. COM7 or /dev/ttyUSB0 (required for device commands)')
    p.add_argument('--baud', type=int, default=460800)
    p.add_argument('--seconds', type=float, default=6, help='how long to show HELLO/HB after the command')
    p.add_argument('cmd', nargs='?', choices=['status', 'otakey', 'tunnel', 'wifi', 'wificlr', 'newpass', 'diag', 'prescfg',
                                              'sensor'])
    p.add_argument('path', nargs='?', help='newpass: the password file to create')
    p.add_argument('--password-file')
    p.add_argument('--wifi-file')
    p.add_argument('--on', type=int, choices=[0, 1])
    p.add_argument('--page', help='diag: tof net wifi wifip rt dhcp rst ev prev tog (wt32-r3; rt dhcp: wt32-r4; tog: wt32-r5; wifi wifip: wt32-r5.1)')
    p.add_argument('--set', help='prescfg: "near=MM far=MM min=MM zones=N on=N off=N stale=MS" (any subset)')
    p.add_argument('--restart', action='store_true', help='sensor: re-initialise the VL53L5CX')
    a = p.parse_args()

    if a.list:
        from serial.tools import list_ports
        for x in list_ports.comports():
            print('%s  %s  %s' % (x.device, '%04x:%04x' % (x.vid, x.pid) if x.vid else '-', x.description))
        return 0
    if a.cmd == 'newpass':
        if not a.path:
            die('newpass needs a file path')
        new_password_file(a.path)
        return 0
    if not a.cmd or not a.port:
        p.print_usage()
        return 2

    link = Link(a.port, a.baud)
    try:
        link.handshake()
        if a.cmd == 'otakey':
            if not a.password_file:
                die('otakey needs --password-file')
            pw = nori_ota.read_password_file(a.password_file)
            if len(pw) < 12:
                die('password too short (use newpass for a random one)')
            link.send('OTAKEY', 'h=' + hashlib.md5(pw).hexdigest())
            del pw
            if not link.run(5, 'OTAKEY'):
                return 1
        elif a.cmd == 'tunnel':
            if a.on is None:
                die('tunnel needs --on 0|1')
            link.send('WIFIEN', 'on=%d' % a.on)
            if not link.run(5, 'WIFIEN'):
                return 1
        elif a.cmd == 'wifi':
            if not a.wifi_file:
                die('wifi needs --wifi-file')
            with open(a.wifi_file, encoding='utf-8') as f:
                w = json.load(f)
            enc = lambda s: base64.b64encode(s.encode()).decode()  # noqa: E731
            link.send('WIFISET', 's=%s p=%s' % (enc(w['ssid']), enc(w.get('psk', ''))))
            del w
            if not link.run(8, 'WIFISET'):
                return 1
            print('note: the station runs only with the opt-in (tunnel --on 1)', flush=True)
        elif a.cmd == 'wificlr':
            link.send('WIFICLR')
            if not link.run(5, 'WIFICLR'):
                return 1
        elif a.cmd == 'diag':                      # wt32-r3
            if a.page and not re.fullmatch(r'[a-z0-9]{1,8}', a.page):
                die('--page: tof net rt dhcp rst ev prev')
            link.send('DIAG', 'p=' + a.page if a.page else '')
            return 0 if link.run(4, 'DIAG') else 1
        elif a.cmd == 'prescfg':                   # wt32-r3
            args = ' '.join((a.set or '').split())
            if args and not re.fullmatch(r'([a-z]+=\d+ ?)+', args):
                die('--set takes key=number tokens')
            link.send('PRESCFG', args)
            return 0 if link.run(5, 'PRESCFG') else 1
        elif a.cmd == 'sensor':                    # wt32-r3
            args = ' '.join(x for x in ('en=%d' % a.on if a.on is not None else '', 're=1' if a.restart else '') if x)
            link.send('SENSOR', args)
            return 0 if link.run(5, 'SENSOR') else 1
        link.run(a.seconds)
        link.send('DIAG')
        link.run(3, 'DIAG')
    finally:
        link.s.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
