#!/usr/bin/env python3
"""wt32-r5.1: tools/nori_wifi_profiles.py against the firmware's own code.  build/tests/wn_dev is the real link core and the
real shared/nori_wifi behind a pty; the HTTP side is a fake Digest server (arduino-esp32's checks, as test_http_ota.py)
that hands every request to that same C code.  So the Python sealing is checked against the C opening, not against
itself.  Test values only; no hardware, no network beyond 127.0.0.1, and the private catalog is never read."""
import contextlib, hashlib, io, json, os, pty, re, secrets, socketserver, subprocess, sys, tempfile, threading, time, tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import nori_wifi_profiles as wp  # noqa: E402

DEV = os.path.join(ROOT, 'build', 'tests', 'wn_dev')
T = tempfile.mkdtemp(prefix='nori-wifi-test-')
RESULTS = []
PASSWORD = b'unit-test-ota-password'
SECRET = hashlib.md5(PASSWORD).hexdigest()
PW_FILE = os.path.join(T, 'ota.txt')
SENT = ['SENTINEL-HOME-SSID', 'sentinel-home-password', 'SENTINEL-SPOT-SSID', 'sentinel-spot-password', 'legacy-home-password', PASSWORD.decode(), SECRET]


def check(name, ok, detail=''):
    RESULTS.append(bool(ok))
    print(('PASS ' if ok else 'FAIL ') + name + (f'  [{detail}]' if detail else ''), flush=True)


def md5(b):
    return hashlib.md5(b).hexdigest()


class Device:
    def __init__(self, secret=SECRET):
        self.master, slave = pty.openpty()
        tty.setraw(self.master)
        tty.setraw(slave)
        self.slave_name = os.ttyname(slave)
        # the tool is the pty's far side: wn_dev gets a second pty pair bridged byte for byte
        self.m2, s2 = pty.openpty()
        tty.setraw(self.m2)
        tty.setraw(s2)
        d = tempfile.mkdtemp(dir=T)
        self.ci, self.co = os.path.join(d, 'in'), os.path.join(d, 'out')
        os.mkfifo(self.ci)
        os.mkfifo(self.co)
        self.p = subprocess.Popen([DEV, os.ttyname(s2), secret, self.ci, self.co])
        self.cw = open(self.ci, 'w', buffering=1)
        self.cr = open(self.co, 'r')
        self.stop = False
        self.wire = bytearray()                    # everything that crossed the UART, both ways
        threading.Thread(target=self.bridge, daemon=True).start()
        self.keep = slave
        self.lock = threading.Lock()

    def bridge(self):
        import select
        while not self.stop:
            r, _, _ = select.select([self.master, self.m2], [], [], 0.05)
            for a, b in ((self.master, self.m2), (self.m2, self.master)):
                if a in r:
                    try:
                        data = os.read(a, 4096)
                    except OSError:
                        return
                    self.wire += data
                    os.write(b, data)

    def ctl(self, line):
        with self.lock:
            self.cw.write(line + '\n')
            self.cw.flush()
            return self.cr.readline().strip()

    def table(self):
        parts = self.ctl('D').split(' ')
        nets = []
        for e in parts[2:]:
            if e:
                s, p, f = e.split('-')
                nets.append((bytes.fromhex(s), bytes.fromhex(p), int(f)))
        return int(parts[0]), int(parts[1]), nets

    def close(self):
        self.stop = True
        self.p.terminate()
        self.p.wait()


def param(auth, name):
    i = auth.find(name)
    if i < 0:
        return ''
    j = auth.find(name[-1] == '"' and '"' or ',', i + len(name))
    return auth[i + len(name):j if j >= 0 else len(auth)]


class Http:
    """WebServer::authenticate(): one server nonce, replaced by every challenge."""

    def __init__(self, dev, passhash=SECRET):
        self.dev, self.passhash = dev, passhash
        self.nonce = self.opaque = ''
        self.bodies = []
        me = self

        class H(socketserver.BaseRequestHandler):
            def handle(self):
                me.serve(self.request)
        self.srv = socketserver.ThreadingTCPServer(('127.0.0.1', 0), H)
        self.srv.daemon_threads = True
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def ok(self, method, auth):
        if not auth.startswith('Digest'):
            return False
        a = auth[7:]
        n, u, resp, op = param(a, 'nonce="'), param(a, 'uri="'), param(a, 'response="'), param(a, 'opaque="')
        if n != self.nonce or op != self.opaque or not self.nonce:
            return False
        h1, h2 = md5(f'nori:nori-ota:{self.passhash}'.encode()), md5(f'{method}:{u}'.encode())
        return resp == md5(f'{h1}:{n}:{param(a, "nc=")}:{param(a, "cnonce=" + chr(34))}:auth:{h2}'.encode())

    def serve(self, s):
        s.settimeout(5)
        buf = b''
        while b'\r\n\r\n' not in buf:
            c = s.recv(4096)
            if not c:
                return
            buf += c
        head, _, rest = buf.partition(b'\r\n\r\n')
        lines = head.decode('latin-1').split('\r\n')
        method, path, _ = lines[0].split(' ')
        hdr = {k.strip().lower(): v.strip() for k, v in (x.split(':', 1) for x in lines[1:])}

        def out(code, body, extra=''):
            s.sendall(f'HTTP/1.1 {code} X\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\n{extra}Connection: close\r\n\r\n'.encode() + body.encode())
        if not self.ok(method, hdr.get('authorization', '')):
            self.nonce, self.opaque = secrets.token_hex(16), secrets.token_hex(16)
            return out(401, '', f'WWW-Authenticate: Digest realm="nori-ota", qop="auth", nonce="{self.nonce}", opaque="{self.opaque}"\r\n')
        if method == 'GET' and path == '/wifi/nonce':
            n = self.dev.ctl('N')
            return out(403, '{"ok":0,"err":"nokey"}') if n == 'nokey' else out(200, json.dumps({'nonce': n}))
        if method == 'POST' and path == '/wifi':
            want = int(hdr['content-length'])
            while len(rest) < want:
                rest += s.recv(4096)
            self.bodies.append(rest)
            code, js = self.dev.ctl('P ' + rest.hex()).split(' ', 1)
            return out(int(code), js)
        if method == 'GET' and path == '/status':
            return out(200, json.dumps({'fw': 'wt32-test', 'part': 'app0', 'img': 'valid', 'wifi': 'up', 'wifi_n': 2, 'wifi_i': 0, 'wifi_gen': 3}))
        out(404, '')

    def close(self):
        self.srv.shutdown()
        self.srv.server_close()


def catalog(name, nets, **extra):
    p = os.path.join(T, name)
    with open(p, 'w', encoding='utf-8') as f:
        json.dump({'_comment': 'test', 'networks': nets, **extra}, f)
    return p


def tool(*args):
    """In process, so what it prints can be inspected.  (rc, stdout + stderr)"""
    o = io.StringIO()
    with contextlib.redirect_stdout(o), contextlib.redirect_stderr(o):
        try:
            rc = wp.main(list(args))
        except SystemExit as e:
            rc = e.code
    return rc, o.getvalue()


def clean(text):
    return not any(s in text for s in SENT)


HOME = {'ssid': 'SENTINEL-HOME-SSID', 'password': 'sentinel-home-password', 'type': 'home', 'band': '2.4GHz', 'subnet': '192.168.0.0/24'}
SPOT = {'ssid': 'SENTINEL-SPOT-SSID', 'password': 'sentinel-spot-password', 'type': 'hotspot', 'band': '2.4GHz', 'subnet': '172.20.10.0/28'}
LEGACY = {'ssid': 'legacy-home', 'password': 'legacy-home-password'}


def main():
    with open(PW_FILE, 'w') as f:
        f.write(PASSWORD.decode() + '\n')
    os.chmod(PW_FILE, 0o600)
    cat = catalog('cat.json', [HOME, SPOT])
    keep = catalog('keep.json', [LEGACY, HOME, SPOT])
    out_all = ''

    # ---- catalog handling, nothing sent
    rc, out = tool('check', '--catalog', cat)
    out_all += out
    check('w1 check accepts the catalog schema', rc == 0 and '2 networks' in out, out.strip())
    check('w1b check prints no value', clean(out))
    for name, nets, word in (('short password', [{'ssid': 'a', 'password': 'seven77'}], 'password'),
                             ('long ssid', [{'ssid': 'x' * 33, 'password': 'password'}], 'SSID'),
                             ('64 non-hex', [{'ssid': 'a', 'password': 'g' * 64}], 'hex'),
                             ('repeat', [HOME, HOME], 'repeats'),
                             ('nine', [{'ssid': 's%d' % i, 'password': 'password'} for i in range(9)], 'keeps 8'),
                             ('no ssid', [{'password': 'sentinel-home-password'}], 'no ssid')):
        rc, out = tool('check', '--catalog', catalog('bad.json', nets))
        out_all += out
        check(f'w2 {name} refused before anything is sent', rc == 2 and word in out and clean(out), out.strip())
    with open(os.path.join(T, 'broken.json'), 'w') as f:
        f.write('{"networks": [{"ssid": "SENTINEL-HOME-SSID", "password": "sentinel-home-password",,]}')
    rc, out = tool('check', '--catalog', os.path.join(T, 'broken.json'))
    check('w3 a broken file is not quoted back', rc == 2 and clean(out), out.strip())
    rc, out = tool('check', '--catalog', catalog('w.json', [{**HOME, 'band': '5GHz'}, {**SPOT, 'subnet': '192.168.77.0/24'}]))
    check('w4 band and subnet warnings by index', rc == 0 and 'network 0' in out and 'network 1' in out and clean(out), out.strip())

    # ---- UART, through the real link core
    d = Device()
    n, g, nets = d.table()
    check('u1 device starts migrated from the legacy keys', n == 1 and nets[0][0] == b'legacy-home', str(n))
    port = ['--port', d.slave_name, '--password-file', PW_FILE]
    rc, out = tool('apply', '--catalog', cat, *port)
    out_all += out
    check('u2 replace that drops the primary is refused', rc == 1 and 'active' in out and d.table()[0] == 1, out.strip())
    rc, out = tool('apply', '--catalog', keep, *port)
    out_all += out
    n, g, nets = d.table()
    check('u3 replace keeping the primary lands in catalog order', rc == 0 and n == 3 and [x[0] for x in nets] == [b'legacy-home', b'SENTINEL-HOME-SSID', b'SENTINEL-SPOT-SSID'], out.strip())
    check('u3b passwords intact', nets[1][1] == b'sentinel-home-password' and nets[2][1] == b'sentinel-spot-password')
    rc, out = tool('apply', '--catalog', cat, '--mode', 'add', *port)
    check('u4 add of a saved SSID is refused', rc == 1 and 'exists' in out and d.table()[0] == 3, out.strip())
    rc, out = tool('apply', '--catalog', cat, '--force', *port)
    out_all += out
    n, g, nets = d.table()
    check('u5 --force replaces, home first and hotspot second', rc == 0 and n == 2 and nets[0][0] == b'SENTINEL-HOME-SSID' and nets[1][0] == b'SENTINEL-SPOT-SSID', out.strip())
    rc, out = tool('delete', '--catalog', cat, '--index', '1', *port)
    out_all += out
    check('u6 delete by catalog index', rc == 0 and d.table()[0] == 1, out.strip())
    rc, out = tool('delete', '--catalog', cat, '--index', '0', '--force', *port)
    check('u7 the last network cannot be deleted', rc == 1 and 'empty' in out and d.table()[0] == 1, out.strip())
    with open(os.path.join(T, 'wrong.txt'), 'w') as f:
        f.write('a-different-ota-password\n')
    os.chmod(os.path.join(T, 'wrong.txt'), 0o600)
    rc, out = tool('apply', '--catalog', cat, '--mode', 'add', '--port', d.slave_name, '--password-file', os.path.join(T, 'wrong.txt'))
    check('u8 wrong OTA password', rc == 1 and 'auth' in out and d.table()[0] == 1, out.strip())
    rc, out = tool('status', '--port', d.slave_name)
    out_all += out
    check('u9 status reports index and count only', rc == 0 and re.search(r'wn=-?\d+/1', out) and clean(out), out.strip())
    wire = bytes(d.wire)
    for s in SENT[:5]:
        check(f'u10 {s[:14]}... never crossed the UART in clear', s.encode() not in wire)
    import base64
    check('u11 nor as base64', all(base64.b64encode(s.encode()).rstrip(b'=') not in wire for s in SENT[:4]))

    # WIFISET compatibility through the real core: fallback kept, then WIFICLR leaves nothing
    from nori_wt32_provision import Link
    tool('apply', '--catalog', keep, '--force', *port)
    link = Link(d.slave_name, 460800)
    link.show = lambda *a: None
    link.handshake()
    enc = lambda s: base64.b64encode(s.encode()).decode()  # noqa: E731

    def ask(kind, args):
        link.send(kind, args)
        for k, a in link.frames(4):
            if k in ('OK', 'ERR') and a.split(' ')[0] == kind:
                return k + ' ' + a
        return 'none'
    r = ask('WIFISET', f's={enc("new-primary")} p={enc("new-primary-password")}')
    n, g, nets = d.table()
    check('u12 WIFISET: new primary, everything kept', r == 'OK WIFISET' and n == 4 and nets[0][0] == b'new-primary' and nets[1][0] == b'legacy-home', r)
    for i in range(5):                             # 4 saved: the fifth filler is the ninth network
        r = ask('WIFISET', f's={enc("filler%d" % i)} p={enc("filler-password")}')
    check('u13 WIFISET on a full table: its own word, nothing lost', r == 'ERR WIFISET full' and d.table()[0] == 8, r)
    r = ask('WIFISET', f's={enc("x")} p={enc("short")}')
    check('u14 WIFISET length rule unchanged', r == 'ERR WIFISET psklen', r)
    r = ask('WIFICLR', '')
    check('u15 WIFICLR forgets every network', r == 'OK WIFICLR' and d.table()[0] == 0, r)
    link.s.close()
    d.close()

    # ---- HTTP
    d = Device()
    h = Http(d)
    host = ['--host', '127.0.0.1', '--http-port', str(h.port), '--password-file', PW_FILE]
    rc, out = tool('apply', '--catalog', keep, *host)
    out_all += out
    n, g, nets = d.table()
    check('h1 replace over HTTP', rc == 0 and n == 3 and nets[2][1] == b'sentinel-spot-password', out.strip())
    check('h2 the body is sealed', len(h.bodies) == 1 and not any(s.encode() in h.bodies[0] for s in SENT))
    d.ctl('A 1')
    rc, out = tool('apply', '--catalog', catalog('nohome.json', [LEGACY, SPOT]), *host)
    check('h3 the network in use cannot be dropped', rc == 1 and 'active' in out and d.table()[0] == 3, out.strip())
    d.ctl('A -1')
    code = d.ctl('P ' + h.bodies[0].hex())
    check('h4 a captured body cannot be replayed', code.startswith('400') and 'expired' in code and d.table()[1] == g, code)
    d.ctl('N')
    code = d.ctl('P ' + h.bodies[0].hex())
    check('h5 nor against a new nonce', code.startswith('400') and d.table()[1] == g, code)
    n0 = d.ctl('N')
    body = bytearray(bytes.fromhex(n0) + h.bodies[0][16:])
    code = d.ctl('P ' + bytes(body).hex())
    check('h6 nor with the new nonce pasted in', code.startswith('403') and 'auth' in code and d.table()[1] == g, code)
    rc, out = tool('apply', '--catalog', keep, '--host', '127.0.0.1', '--http-port', str(h.port), '--password-file', os.path.join(T, 'wrong.txt'))
    check('h7 wrong password stops at Digest', rc == 2 and 'refused' in out and clean(out), out.strip())
    rc, out = tool('status', *host)
    out_all += out
    check('h8 status', rc == 0 and 'wifi_n=2' in out and clean(out), out.strip())
    h.close()
    d.close()

    d = Device('')                                 # no OTA key on the device
    h = Http(d)
    rc, out = tool('apply', '--catalog', keep, '--host', '127.0.0.1', '--http-port', str(h.port), '--password-file', PW_FILE)
    check('h9 no OTA key on the device: no provisioning', rc == 2 and 'nokey' in out, out.strip())
    h.close()
    d.close()

    check('r1 nothing the tool printed carries a value', clean(out_all))
    src = open(os.path.join(ROOT, 'tools', 'nori_wifi_profiles.py'), encoding='utf-8').read()
    check('r2 the tool starts no subprocess', 'subprocess' not in src and 'os.system' not in src)
    fails = RESULTS.count(False)
    print(f'test_wifi_provision: {len(RESULTS)} checks, {fails} failures')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
