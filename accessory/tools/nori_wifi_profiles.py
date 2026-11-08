#!/usr/bin/env python3
"""wt32-r5.1: load the saved Wi-Fi networks of a Nori WT32-ETH01 from a private catalog, over the UART link or HTTP.

  nori_wifi_profiles.py status --host H [--password-file F]         what the device reports (indices and counters)
  nori_wifi_profiles.py status --port COM20
  nori_wifi_profiles.py check  --catalog C                          validate the catalog; nothing is sent
  nori_wifi_profiles.py apply  --catalog C (--host H | --port P) --password-file F [--mode replace|add] [--update] [--force]
  nori_wifi_profiles.py delete --catalog C --index I (--host H | --port P) --password-file F [--force]

Catalog: JSON, {"networks": [{"ssid": ..., "password": ..., "hidden": false}, ...]}.  Order is priority: first entry first.
Other keys (type, band, subnet, _comment) are ignored, except that the band and subnet can raise a warning.

Secrets: the catalog and password file are read here, at run time.  SSIDs and passwords are never printed, logged, put
on a command line or left in a file: networks are named by catalog index, and errors by word.  Neither transport sees
them in the clear; they travel in an envelope sealed with the OTA secret, see shared/nori_wifi/nori_wnprov.h.

apply --mode replace (default) makes the device's list exactly the catalog.  --mode add adds what the device does not
have; a saved SSID is refused unless --update.  The device refuses to drop or change the login of the network in use or
of its primary unless --force.  Nothing is applied in part, and a refused change leaves the device as it was.
"""
import argparse
import base64
import hashlib
import hmac
import ipaddress
import json
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nori_ota  # noqa: E402  (password file checks: user-only ACL / mode)

MAX_NETS, SSID_MAX, PSK_MAX = 8, 32, 64
OP_ADD, OP_REPLACE, OP_DEL = 1, 2, 3
FL_FORCE, FL_UPDATE = 1, 2
NET_HIDDEN = 1
LABEL = b'nori-wifi-v1'
CHUNK = 180
PM3_LAN = ipaddress.ip_network('192.168.77.0/24')


def die(msg):
    sys.stderr.write('nori_wifi_profiles: %s\n' % msg)
    sys.exit(2)


def _hm(key, *parts):
    return hmac.new(key, b''.join(parts), hashlib.sha256).digest()


def seal(secret, nonce, payload):
    """(ciphertext, tag), see nori_wnprov.h.  secret: the 32 hex characters of md5(password), as bytes."""
    k = _hm(secret, LABEL, nonce)
    ke, km = _hm(k, b'enc'), _hm(k, b'mac')
    stream = b''.join(_hm(ke, struct.pack('>I', i)) for i in range((len(payload) + 31) // 32))
    ct = bytes(a ^ b for a, b in zip(payload, stream))
    return ct, _hm(km, nonce, ct)


def build_payload(op, nets, force=False, update=False, pos=255):
    flags = (FL_FORCE if force else 0) | (FL_UPDATE if update else 0)
    out = bytearray([1, op, flags, pos, len(nets)])
    for ssid, psk, hidden in nets:
        out += bytes([len(ssid)]) + ssid + bytes([len(psk)]) + psk + bytes([NET_HIDDEN if hidden else 0])
    return bytes(out)


def check_net(i, n):
    """(ssid, psk, hidden) as bytes; the same limits as nw_net_set().  Messages name the index, never a value."""
    if not isinstance(n, dict) or not isinstance(n.get('ssid'), str):
        die('catalog network %d: no ssid' % i)
    pw = n.get('password', n.get('psk', ''))
    if pw is None:
        pw = ''
    if not isinstance(pw, str):
        die('catalog network %d: password is not text' % i)
    ssid, psk = n['ssid'].encode('utf-8'), pw.encode('utf-8')
    if not 1 <= len(ssid) <= SSID_MAX or b'\0' in ssid:
        die('catalog network %d: SSID must be 1..%d bytes' % (i, SSID_MAX))
    if b'\0' in psk or (psk and not 8 <= len(psk) <= PSK_MAX):
        die('catalog network %d: password must be empty (open), 8..63 characters or 64 hex digits' % i)
    if len(psk) == PSK_MAX and not re.fullmatch(rb'[0-9a-fA-F]{64}', psk):
        die('catalog network %d: a 64 character password must be hex' % i)
    return ssid, psk, bool(n.get('hidden', False))


def warnings_for(i, n):
    band = str(n.get('band', ''))
    if band and '2' not in band:
        yield 'catalog network %d: the band field does not mention 2.4 GHz; the ESP32 has no 5 GHz radio' % i
    sub = n.get('subnet')
    if isinstance(sub, str):
        try:
            if ipaddress.ip_network(sub, strict=False).overlaps(PM3_LAN):
                yield 'catalog network %d: its subnet overlaps the PM3 LAN 192.168.77.0/24' % i
        except ValueError:
            pass


def load_catalog(path):
    try:
        with open(path, encoding='utf-8') as f:
            doc = json.load(f)
    except OSError as e:
        die('cannot read the catalog (%s)' % type(e).__name__)
    except ValueError:
        die('the catalog is not valid JSON')          # the exception text can quote the file: not shown
    raw = doc.get('networks') if isinstance(doc, dict) else doc
    if not isinstance(raw, list) or not raw:
        die('the catalog has no networks')
    if len(raw) > MAX_NETS:
        die('the catalog has %d networks; the device keeps %d' % (len(raw), MAX_NETS))
    nets = [check_net(i, n) for i, n in enumerate(raw)]
    if len({n[0] for n in nets}) != len(nets):
        die('the catalog repeats an SSID')
    for i, n in enumerate(raw):
        for w in warnings_for(i, n):
            print('warning: ' + w, flush=True)
    return nets


class Uart:
    def __init__(self, port, baud):
        import nori_wt32_provision as prov
        self.link = prov.Link(port, baud)
        self.link.show = lambda kind, args: None      # this tool prints only what it means to
        self.link.handshake()

    def _ask(self, kind, args, seconds=5):
        self.link.send(kind, args)
        for k, a in self.link.frames(seconds):
            if k in ('OK', 'ERR') and a.split(' ', 1)[0] == kind:
                return k, a
        die('no reply to %s' % kind)

    def apply(self, secret, payload):
        k, a = self._ask('WNBEGIN', '')
        if k != 'OK':
            return {'ok': 0, 'err': a.split(' ')[-1]}
        m = re.fullmatch(r'WNBEGIN n=([0-9a-f]{32})', a)
        if not m:
            die('unexpected WNBEGIN reply')
        ct, tag = seal(secret, bytes.fromhex(m.group(1)), payload)
        for o in range(0, len(ct), CHUNK):
            k, a = self._ask('WNDATA', 'o=%d d=%s' % (o, base64.b64encode(ct[o:o + CHUNK]).decode()))
            if k != 'OK':
                return {'ok': 0, 'err': a.split(' ')[-1]}
        k, a = self._ask('WNEND', 't=' + tag.hex())
        if k != 'OK':
            return {'ok': 0, 'err': a.split(' ')[-1]}
        m = re.fullmatch(r'WNEND n=(\d+) g=(\d+)', a)
        return {'ok': 1, 'n': int(m.group(1)), 'g': int(m.group(2))} if m else {'ok': 0, 'err': 'reply'}

    def status(self):
        k, a = self._ask('DIAG', 'p=wifi')
        return a.split('pg=wifi;', 1)[-1].replace(';', ' ') if k == 'OK' else 'unavailable'

    def close(self):
        self.link.s.close()


class Http:
    def __init__(self, host, port, password, timeout):
        import nori_http_ota as ho
        if not re.fullmatch(r'[A-Za-z0-9.-]+', host):
            die('--host must be a plain IPv4 address or host name')
        self.ho, self.timeout = ho, timeout
        self.base = 'http://%s:%d' % (host, port)
        self.sess = ho.session()
        self.user = ho.digest_auth(password)

    def _nonce(self):
        """A fresh Digest exchange, insisting on the challenge first (as nori_http_ota.get_status_auth); the POST is
        signed with that same exchange, so it is never sent unauthenticated."""
        a = type(self.user)(self.user.username, self.user.password)
        r = self.sess.get(self.base + '/wifi/nonce', auth=a, timeout=self.timeout, allow_redirects=False)
        if r.status_code == 401:
            return None, a
        if not (r.history and self.ho._digest_challenge(r.history[0])):
            die('device answered without a Digest challenge; refusing')
        if r.status_code == 403:
            die('the device has no OTA key (nokey)')
        if r.status_code != 200:
            die('/wifi/nonce: HTTP %d (firmware older than wt32-r5.1?)' % r.status_code)
        m = re.fullmatch(r'[0-9a-f]{32}', str(r.json().get('nonce', '')))
        if not m:
            die('unexpected /wifi/nonce reply')
        return bytes.fromhex(m.group(0)), a

    def apply(self, secret, payload):
        for attempt in range(3):                      # a 401 means another client took the Digest nonce: nothing was applied
            nonce, a = self._nonce()
            if nonce is None:
                time.sleep(0.15 * (attempt + 1))
                continue
            ct, tag = seal(secret, nonce, payload)
            r = self.sess.post(self.base + '/wifi', auth=a, data=nonce + ct + tag, timeout=self.timeout, allow_redirects=False,
                               headers={'Content-Type': 'application/octet-stream'})
            if r.status_code == 401:
                time.sleep(0.15 * (attempt + 1))
                continue
            try:
                return r.json()
            except ValueError:
                die('/wifi: HTTP %d' % r.status_code)
        die('authentication refused (wrong password, or the device has another OTA key)')

    def status(self):
        st = self.ho.get_status(self.sess, self.base, self.user, self.timeout)
        return ' '.join('%s=%s' % (k, st.get(k)) for k in ('fw', 'wifi', 'wifi_n', 'wifi_i', 'wifi_gen', 'wifi_st', 'wifi_why', 'wifi_fail', 'wip'))

    def close(self):
        self.sess.close()


def connect(a, password):
    if bool(a.host) == bool(a.port):
        die('give exactly one of --host and --port')
    if a.port:
        return Uart(a.port, a.baud)
    return Http(a.host, a.http_port, password, a.timeout)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['status', 'check', 'apply', 'delete'])
    ap.add_argument('--catalog')
    ap.add_argument('--host')
    ap.add_argument('--http-port', type=int, default=80)
    ap.add_argument('--port', help='serial port of the UART link')
    ap.add_argument('--baud', type=int, default=460800)
    ap.add_argument('--password-file')
    ap.add_argument('--timeout', type=float, default=10.0)
    ap.add_argument('--mode', choices=['replace', 'add'], default='replace')
    ap.add_argument('--update', action='store_true', help='add: also change the password of an SSID already saved')
    ap.add_argument('--force', action='store_true', help='allow dropping or changing the network in use or the primary')
    ap.add_argument('--index', type=int, help='delete: index in the catalog')
    a = ap.parse_args(argv)

    nets = None
    if a.cmd != 'status':
        if not a.catalog:
            die('%s needs --catalog' % a.cmd)
        nets = load_catalog(a.catalog)
    if a.cmd == 'check':
        for i, n in enumerate(nets):
            print('network %d: %s%s' % (i, 'open' if not n[1] else 'password', ', hidden' if n[2] else ''), flush=True)
        print('%d networks, within the device limits; nothing was sent' % len(nets))
        return 0

    if a.cmd == 'status' and a.port:
        password = b''
    else:
        password = nori_ota.get_password(a.password_file)
    dev = connect(a, password)
    try:
        if a.cmd == 'status':
            print(dev.status(), flush=True)
            return 0
        if a.cmd == 'delete':
            if a.index is None or not 0 <= a.index < len(nets):
                die('--index must name a catalog entry')
            payload = build_payload(OP_DEL, [nets[a.index]], a.force)
            want = None
        else:
            payload = build_payload(OP_REPLACE if a.mode == 'replace' else OP_ADD, nets, a.force, a.update)
            want = len(nets) if a.mode == 'replace' else None
        r = dev.apply(hashlib.md5(password).hexdigest().encode(), payload)
        del payload
        if not r.get('ok'):
            print('refused: %s; the device is unchanged' % re.sub(r'[^a-z]', '', str(r.get('err', 'unknown'))), flush=True)
            return 1
        if want is not None and r.get('n') != want:
            print('device reports %s networks, expected %d' % (r.get('n'), want), flush=True)
            return 1
        print('stored: %s networks, generation %s' % (r.get('n'), r.get('g')), flush=True)
        return 0
    finally:
        dev.close()
        del password


if __name__ == '__main__':
    sys.exit(main())
