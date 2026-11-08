#!/usr/bin/env python3
"""Host-only tests for tools/nori_http_ota.py against a fake r2.0.1 device on
localhost.  The fake checks Digest auth exactly like arduino-esp32 2.0.17
WebServer::authenticate() (plus the firmware's uri binding and burnt nonces) and
hands every POST body to build/tests/test_http --request, i.e. the firmware's
own nori_http.cpp parser.  No hardware, no network beyond 127.0.0.1, no real
password.  Run via tests/run_tests.sh (which builds test_http first)."""
import hashlib, io, json, os, re, secrets, socket, socketserver, struct, subprocess, sys, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'tests'))
import nori_http_ota  # noqa: E402
from test_ota_updater import fake_app_image  # noqa: E402

DRIVER = os.path.join(ROOT, 'build', 'tests', 'test_http')
T = tempfile.mkdtemp(prefix='nori-http-ota-test-')
RESULTS = []


def check(name, ok, detail=''):
    RESULTS.append(bool(ok))
    print(('PASS ' if ok else 'FAIL ') + name + (f'  [{detail}]' if detail else ''), flush=True)


def md5(b):
    return hashlib.md5(b).hexdigest()


def param(auth, name):
    """WebServer::_extractParam: first occurrence of name=" (so it is order sensitive, like the device)."""
    i = auth.find(name)
    if i < 0:
        return ''
    j = auth.find(name[-1] == '"' and '"' or ',', i + len(name))
    return auth[i + len(name):j if j >= 0 else len(auth)]


class Device:
    """Fake accessory.  mode: digest | noauth | basic | refuse | noreboot | reset (wt32-r3: RST mid-upload)."""

    def __init__(self, passhash, mode='digest', fw='r2.0.0'):
        self.passhash, self.mode = passhash, mode
        self.fw, self.part, self.img, self.boot = fw, 'app0', 'valid', time.monotonic()
        self.nonce = self.opaque = self.realm = ''
        self.down_until = 0.0
        self.bodies, self.images, self.auth_seen, self.posts = [], [], [], 0
        self.steal = 0            # wt32-r4: next N signed requests lose to a "concurrent client" (nonce rotated)
        self.steal_after_reboot = 0
        self.challenges = 0
        self.lock = threading.Lock()
        dev = self

        class H(socketserver.BaseRequestHandler):
            def handle(self):
                dev.serve(self.request)

        self.srv = socketserver.ThreadingTCPServer(('127.0.0.1', 0), H)
        self.srv.daemon_threads = True
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def close(self):
        self.srv.shutdown()
        self.srv.server_close()

    def authenticate(self, method, auth):           # WebServer::authenticate(), digest branch
        if not auth.startswith('Digest'):
            return False
        a = auth[7:]
        user, realm, nonce = param(a, 'username="'), param(a, 'realm="'), param(a, 'nonce="')
        uri, resp, opaque = param(a, 'uri="'), param(a, 'response="'), param(a, 'opaque="')
        if user != 'nori' or not (realm and nonce and uri and resp and opaque):
            return False
        if opaque != self.opaque or nonce != self.nonce or realm != self.realm:
            return False
        h1 = md5(f'nori:{realm}:{self.passhash}'.encode())
        h2 = md5(f'{method}:{uri}'.encode())
        if 'qop=auth' in a or 'qop="auth"' in a:
            nc, cnonce = param(a, 'nc='), param(a, 'cnonce="')
            want = md5(f'{h1}:{nonce}:{nc}:{cnonce}:auth:{h2}'.encode())
        else:
            want = md5(f'{h1}:{nonce}:{h2}'.encode())
        return resp == want

    def serve(self, s):
        s.settimeout(5)
        if time.monotonic() < self.down_until:
            return                                    # "rebooting": connection closed at once
        buf = b''
        while b'\r\n\r\n' not in buf and len(buf) < 2048:
            chunk = s.recv(2048)
            if not chunk:
                return
            buf += chunk
        head, _, rest = buf.partition(b'\r\n\r\n')
        lines = head.decode('latin-1').split('\r\n')
        method, path, _ = lines[0].split(' ')
        hdr = {k.strip().lower(): v.strip() for k, v in (l.split(':', 1) for l in lines[1:])}
        auth = hdr.get('authorization', '')

        def reply(code, body, extra=''):
            s.sendall(f'HTTP/1.1 {code} X\r\n{extra}Content-Type: text/plain\r\nContent-Length: {len(body)}\r\n'
                      f'Connection: close\r\n\r\n'.encode() + body)

        with self.lock:
            if self.mode == 'noauth':
                ok = True
            elif self.steal and auth.startswith("Digest"):
                self.steal -= 1           # another client fetched a challenge in between: our nonce is gone
                ok = False
            elif self.mode == 'basic':
                return reply(401, b'', 'WWW-Authenticate: Basic realm="nori-ota"\r\n')
            else:
                ok = self.authenticate(method, auth) and param(auth[7:], 'uri="') == path
            if not ok:
                self.nonce, self.opaque = secrets.token_hex(16), secrets.token_hex(16)
                self.realm = 'other' if self.mode == 'otherrealm' else 'nori-ota'
                self.challenges += 1
                return reply(401, b'', f'WWW-Authenticate: Digest realm="{self.realm}", qop="auth", '
                                       f'nonce="{self.nonce}", opaque="{self.opaque}"\r\n')
            self.auth_seen.append(auth)
            if method == 'GET' and path == '/status':
                st = dict(fw=self.fw, part=self.part, img=self.img, ota='ready', http='ready',
                          uptime_s=int(time.monotonic() - self.boot))
                return reply(200, json.dumps(st).encode())
            if method == 'GET' and path == '/diag':      # wt32-r3
                st = dict(fw=self.fw, part=self.part, img=self.img, tof=dict(stage='ranging'), events=[])
                return reply(200, json.dumps(st).encode())
            if method != 'POST' or path != '/update':
                return reply(404, b'not found\n')
            self.nonce = secrets.token_hex(16)        # burnt, as NoriHttp::update()
            self.posts += 1
        if self.mode == 'reset':                      # the device dies mid-transfer: TCP RST, no reply
            try:
                s.recv(20000)
            except OSError:
                pass
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            s.close()
            return
        cl = int(hdr['content-length'])
        body = rest
        while len(body) < cl:
            chunk = s.recv(65536)
            if not chunk:
                return
            body += chunk
        self.bodies.append(body)
        out = os.path.join(T, 'driver-%d.bin' % len(self.bodies))
        verdict = subprocess.run([DRIVER, '--request', out], input=head + b'\r\n\r\n' + body,
                                 capture_output=True).stdout.decode().strip().splitlines()
        self.verdict = verdict
        if not verdict or not verdict[-1].startswith('OK'):
            return reply(400, ('ERR %s\n' % (verdict[-1] if verdict else 'driver')).encode())
        img = open(out, 'rb').read()
        if hdr.get('x-nori-image-md5') and hdr['x-nori-image-md5'] != md5(img):
            return reply(500, b'ERR end MD5 Check Failed\n')
        if self.mode == 'refuse':
            return reply(500, b'ERR end Could Not Activate The Firmware\n')
        self.images.append(img)
        reply(200, b'OK rebooting\n')
        with self.lock:                               # reboot into the other slot
            self.down_until = time.monotonic() + 1.0
            self.boot = time.monotonic()
            self.nonce = self.opaque = self.realm = ''
            self.steal = self.steal_after_reboot
            if self.mode != 'noreboot':
                self.part = 'app1' if self.part == 'app0' else 'app0'
                self.fw = nori_http_ota.image_fw(img) or 'unknown'
                self.img = 'pending'


def tagged_image(fw=b'r2.0.1', size=60000):
    img = bytearray(fake_app_image(size))
    tag = b'@?NORI_FW=' + fw + b'\0'
    img[3000:3000 + len(tag)] = tag
    return bytes(img)


def cli(*args):
    return subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'nori_http_ota.py')] + list(args),
                          stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=120)


def main():
    if not os.path.exists(DRIVER):
        print('FAIL build/tests/test_http missing (run tests/run_tests.sh)')
        return 1
    pwfile = os.path.join(T, 'ota.pass')
    pw = secrets.token_urlsafe(32).encode()
    with open(os.open(pwfile, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'wb') as f:
        f.write(pw + b'\n')
    ph = md5(pw)
    secrets_in = lambda text: pw.decode() in text or ph in text   # noqa: E731
    img_path = os.path.join(T, 'app.bin')
    open(img_path, 'wb').write(tagged_image())
    before = set(os.listdir(T))

    # image tag
    check('fw tag read from image', nori_http_ota.image_fw(tagged_image()) == 'r2.0.1')
    check('untagged image has no fw', nori_http_ota.image_fw(fake_app_image()) is None)
    real = os.path.join(ROOT, 'release', 'wt32', 'firmware.bin')   # wt32-r3: the released application image
    if os.path.exists(real):
        rfw = nori_http_ota.image_fw(open(real, 'rb').read())
        check('released wt32 firmware.bin carries NORI_FW tag', rfw == 'wt32-r5.1.3', rfw)

    # status
    dev = Device(ph)
    r = cli('status', '--host', '127.0.0.1', '--port', str(dev.port), '--password-file', pwfile)
    check('status with the right password', r.returncode == 0 and 'fw=r2.0.0 part=app0 img=valid' in r.stdout, r.stdout.strip() + r.stderr.strip())
    check('status output has no password or hash', not secrets_in(r.stdout + r.stderr))
    wrong = os.path.join(T, 'wrong.pass')
    with open(os.open(wrong, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'wb') as f:
        f.write(b'not-the-password\n')
    r = cli('diag', '--host', '127.0.0.1', '--port', str(dev.port), '--password-file', pwfile)
    check('wt32-r3 diag: authenticated /diag printed as JSON', r.returncode == 0 and json.loads(r.stdout).get('tof') == {'stage': 'ranging'},
          r.stdout.strip()[:120] + r.stderr.strip())
    r = cli('status', '--json', '--host', '127.0.0.1', '--port', str(dev.port), '--password-file', pwfile)
    check('wt32-r3 status --json', r.returncode == 0 and json.loads(r.stdout).get('fw') == 'r2.0.0', r.stdout.strip()[:120])
    check('diag/status --json output has no password or hash', not secrets_in(r.stdout + r.stderr))
    r = cli('status', '--host', '127.0.0.1', '--port', str(dev.port), '--password-file', wrong)
    check('status with a wrong password refused', r.returncode == 1 and 'authentication refused' in r.stderr, r.stderr.strip())
    dev.close()

    # wt32-r4 host-tool fix: one server nonce on the device + concurrent clients -> transient 401s
    dev = Device(ph)
    base = 'http://127.0.0.1:%d' % dev.port
    a = nori_http_ota.digest_auth(pw)
    ok = True
    for i in range(12):                                # repeated polls with ONE auth object (diag sampling)
        try:
            ok = ok and nori_http_ota.get_status(nori_http_ota.session(), base, a, 3,
                                                 '/diag' if i % 2 else '/status').get('fw') == 'r2.0.0'
        except nori_http_ota.Refused as e:
            ok = False
            print('   refused:', e)
    check('12 repeated status/diag polls with one auth object: all succeed, each after a fresh challenge',
          ok and dev.challenges == 12, f'challenges={dev.challenges}')
    dev.challenges, dev.steal = 0, 1
    r = cli('diag', '--host', '127.0.0.1', '--port', str(dev.port), '--password-file', pwfile)
    check('transient concurrent 401 (nonce rotated by another client) -> diag retries and succeeds', r.returncode == 0 and
          json.loads(r.stdout).get('tof') == {'stage': 'ranging'} and dev.challenges == 3, f'challenges={dev.challenges}')   # 2 per refused attempt + 1
    dev.steal = 2
    r = cli('status', '--host', '127.0.0.1', '--port', str(dev.port), '--password-file', pwfile)
    check('two transient 401s in a row -> still succeeds within the bound', r.returncode == 0 and 'fw=r2.0.0' in r.stdout, r.stderr.strip())
    dev.steal, dev.challenges = 5, 0
    r = cli('status', '--host', '127.0.0.1', '--port', str(dev.port), '--password-file', pwfile)
    check('persistent 401s -> refused after exactly 3 fresh Digest exchanges (6 challenges, bounded)', r.returncode == 1 and
          'authentication refused' in r.stderr and dev.challenges == 6, f'challenges={dev.challenges}')
    dev.steal, dev.challenges = 0, 0
    r = cli('status', '--host', '127.0.0.1', '--port', str(dev.port), '--password-file', wrong)
    check('wrong password still refused (3 bounded attempts), no secret printed', r.returncode == 1 and
          'authentication refused' in r.stderr and dev.challenges == 6 and not secrets_in(r.stdout + r.stderr), f'challenges={dev.challenges}')
    dev.close()
    dev = Device(ph, mode='otherrealm')
    r = cli('status', '--host', '127.0.0.1', '--port', str(dev.port), '--password-file', pwfile)
    check('Digest challenge for another realm -> refused at once, not retried', r.returncode == 1 and
          'realm nori-ota; refusing' in r.stderr and dev.challenges == 1, r.stderr.strip())
    dev.close()

    # stale / reboot challenge after an upload, and a concurrent 401 between the pre-upload GET and the POST
    dev = Device(ph)
    dev.steal_after_reboot = 4                         # first post-reboot polls lose their nonce (sampler running)
    q = io.StringIO()
    n0, mh0, _ = nori_http_ota.nori_ota.check_image(img_path)
    rc = nori_http_ota.upload('http://127.0.0.1:%d' % dev.port, pw, img_path, n0, mh0, reboot_timeout=20, poll=0.3, log=q)
    check('post-reboot transient 401s no longer end the upload as "authentication refused"', rc == 0 and
          'updated: fw=r2.0.1' in q.getvalue() and dev.posts == 1, q.getvalue().strip().replace('\n', ' | ')[-200:])
    check('...with exactly one firmware POST, signed with the pre-upload challenge nonce', dev.posts == 1 and len(dev.bodies) == 1)
    check('...and no secret in the log', not secrets_in(q.getvalue()))
    dev.close()
    dev = Device(ph)
    dev.steal_after_reboot = 50                        # the device refuses every post-reboot poll: a real refusal
    q = io.StringIO()
    t0 = time.monotonic()
    rc = nori_http_ota.upload('http://127.0.0.1:%d' % dev.port, pw, img_path, n0, mh0, reboot_timeout=30, poll=0.2, log=q)
    check('persistent post-reboot refusal still ends with "authentication refused", bounded (3 polls)', rc == 1 and
          'authentication refused' in q.getvalue() and time.monotonic() - t0 < 15 and dev.posts == 1, q.getvalue().strip()[-120:])
    dev.close()

    # upload: the real requests multipart body goes through the firmware parser
    dev = Device(ph)
    q = io.StringIO()
    n, mh, _ = nori_http_ota.nori_ota.check_image(img_path)
    t0 = time.monotonic()
    rc = nori_http_ota.upload('http://127.0.0.1:%d' % dev.port, pw, img_path, n, mh, reboot_timeout=20, poll=0.3, log=q)
    check('upload succeeds and confirms the reboot', rc == 0 and 'updated: fw=r2.0.1 part=app1 img=pending' in q.getvalue(),
          q.getvalue().strip().splitlines()[-1] if q.getvalue() else '')
    check('firmware parser got the exact image', dev.images == [open(img_path, 'rb').read()], getattr(dev, 'verdict', ''))
    check('upload sent the image MD5 header', any(l.startswith('HEAD POST /update') and 'md5=' + mh in l for l in dev.verdict))
    check('exactly one POST (digest from /status reused, no 401 resend)', dev.posts == 1 and len(dev.bodies) == 1)
    check('upload log has no password or hash', not secrets_in(q.getvalue()))
    check('poll waited through the simulated reboot', time.monotonic() - t0 >= 1.0)
    replay = dev.auth_seen[[i for i, a in enumerate(dev.auth_seen) if 'uri="/update"' in a][0]]
    with dev.lock:
        dev.mode = 'digest'
    s = socket.create_connection(('127.0.0.1', dev.port))
    s.sendall(b'POST /update HTTP/1.1\r\nAuthorization: ' + replay.encode() + b'\r\nContent-Length: 0\r\n\r\n')
    check('a captured /update Authorization cannot be replayed', s.recv(64).startswith(b'HTTP/1.1 401'))
    s.close()
    dev.close()

    if os.path.exists(real):                          # the actual ~1 MB build through the firmware parser
        dev = Device(ph)
        rn, rmd5, _ = nori_http_ota.nori_ota.check_image(real)
        q = io.StringIO()
        rc = nori_http_ota.upload('http://127.0.0.1:%d' % dev.port, pw, real, rn, rmd5, reboot_timeout=20, poll=0.3, log=q)
        check('built firmware.bin uploads byte-exact', rc == 0 and dev.images == [open(real, 'rb').read()], f'{rn} bytes')
        dev.close()

    # CLI end to end (subprocess): same, plus no secrets on stdout/stderr
    dev = Device(ph)
    r = cli('upload', '--host', '127.0.0.1', '--port', str(dev.port), '--password-file', pwfile, '--reboot-timeout', '20', img_path)
    check('CLI upload succeeds', r.returncode == 0 and 'updated: fw=r2.0.1' in r.stderr, r.stderr.strip().splitlines()[-1] if r.stderr else '')
    check('CLI output has no password or hash', not secrets_in(r.stdout + r.stderr))
    dev.close()

    # refusals
    dev = Device(md5(b'some-other-password'))
    q = io.StringIO()
    rc = nori_http_ota.upload('http://127.0.0.1:%d' % dev.port, pw, img_path, n, mh, reboot_timeout=5, poll=0.2, log=q)
    check('wrong password: refused, no body sent', rc == 1 and dev.posts == 0 and not dev.bodies, q.getvalue().strip())
    dev.close()
    for mode in ('noauth', 'basic'):
        dev = Device(ph, mode=mode)
        q = io.StringIO()
        rc = nori_http_ota.upload('http://127.0.0.1:%d' % dev.port, pw, img_path, n, mh, reboot_timeout=5, poll=0.2, log=q)
        check(f'{mode} device refused, nothing uploaded', rc == 1 and not dev.bodies and 'refusing' in q.getvalue(), q.getvalue().strip())
        dev.close()
    dev = Device(ph, mode='refuse')
    q = io.StringIO()
    rc = nori_http_ota.upload('http://127.0.0.1:%d' % dev.port, pw, img_path, n, mh, reboot_timeout=5, poll=0.2, log=q)
    check('device-side failure reported', rc == 1 and 'refused the image: HTTP 500 ERR end' in q.getvalue(), q.getvalue().strip())
    dev.close()
    dev = Device(ph, mode='noreboot')
    q = io.StringIO()
    t0 = time.monotonic()
    rc = nori_http_ota.upload('http://127.0.0.1:%d' % dev.port, pw, img_path, n, mh, reboot_timeout=3, poll=0.3, log=q)
    check('no slot switch after reboot -> failure within the bound', rc == 1 and 'previous slot' in q.getvalue()
          and time.monotonic() - t0 < 10, q.getvalue().strip().splitlines()[-1])
    dev.close()
    r = cli('status', '--host', '127.0.0.1', '--port', '1', '--password-file', pwfile, '--timeout', '2')
    check('unreachable device -> clean error', r.returncode == 1 and 'not reachable' in r.stderr, r.stderr.strip())
    # wt32-r3: the exception detail is reported (it was only the type name), without secrets
    check('unreachable: the cause is named, not just the type', re.search(r'not reachable: (ConnectionError|ConnectTimeout): .*'
          r'(?i:refused|timed out)', r.stderr) is not None, r.stderr.strip()[:300])
    check('unreachable: no secret in the detail', not secrets_in(r.stdout + r.stderr))
    dev = Device(ph, mode='reset')
    q = io.StringIO()
    rc = nori_http_ota.upload('http://127.0.0.1:%d' % dev.port, pw, img_path, n, mh, reboot_timeout=3, poll=0.2, log=q)
    out = q.getvalue()
    check('RST mid-upload: failure with the cause (reset / aborted) and elapsed time', rc == 1 and
          re.search(r'upload failed after \d+\.\d s: ConnectionError: .*(?i:reset|aborted|broken pipe)', out) is not None,
          out.strip().splitlines()[-1][:200] if out else '')
    check('RST mid-upload: no secret in the detail', not secrets_in(out))
    dev.close()
    leak = nori_http_ota.err_detail(RuntimeError('x %s y Digest username="nori", response="abc" %s nonce=zz' % (pw.decode(), ph)), pw, ph)
    check('err_detail redacts password, hash and Digest material', pw.decode() not in leak and ph not in leak and 'abc' not in leak
          and 'zz' not in leak and leak.startswith('RuntimeError: '), leak)

    # local validation happens before any network traffic
    dev = Device(ph)
    fs = os.path.join(T, 'spiffs.bin')
    open(fs, 'wb').write(b'\xff' * 65536)
    r = cli('upload', '--host', '127.0.0.1', '--port', str(dev.port), '--password-file', pwfile, fs)
    check('filesystem image refused locally', r.returncode == 2 and not dev.auth_seen and dev.nonce == '', r.stderr.strip())
    r = cli('upload', '--host', '127.0.0.1', '--port', str(dev.port), img_path)
    check('no password from a pipe', r.returncode == 2 and 'not a terminal' in r.stderr)
    r = cli('upload', '--host', 'user:pw@127.0.0.1', '--password-file', pwfile, img_path)
    check('host with credentials/odd characters refused', r.returncode == 2)
    dev.close()
    loose = os.path.join(T, 'loose.pass')
    open(loose, 'w').write('x' * 20 + '\n')
    os.chmod(loose, 0o644)
    r = cli('status', '--host', '127.0.0.1', '--password-file', loose)
    check('group/other-readable password file refused', r.returncode == 2)

    h = cli('upload', '-h').stdout
    check('CLI offers no password argument', not re.search(r'--password\b(?!-file)', h) and '--auth' not in h)
    created = set(os.listdir(T)) - before - {'wrong.pass', 'spiffs.bin', 'loose.pass'}
    check('updater wrote no files (no credential or cache files)', all(f.startswith('driver-') for f in created), sorted(created))

    print('http-ota-updater: %d checks, %d failures' % (len(RESULTS), RESULTS.count(False)))
    return 0 if all(RESULTS) else 1


if __name__ == '__main__':
    sys.exit(main())
