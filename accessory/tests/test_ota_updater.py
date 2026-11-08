#!/usr/bin/env python3
"""Host-only tests for tools/nori_ota.py against a fake ArduinoOTA device on
localhost (same UDP/TCP exchange as ArduinoOTA 2.0.17).  No hardware, no real
password: everything is generated in a temp directory.  Run via tests/run_tests.sh."""
import hashlib, io, os, socket, struct, subprocess, sys, tempfile, threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import nori_ota  # noqa: E402

T = tempfile.mkdtemp(prefix='nori-ota-test-')     # /tmp: real POSIX permissions (not DrvFs)
RESULTS = []


def check(name, ok, detail=''):
    RESULTS.append(bool(ok))
    print(('PASS ' if ok else 'FAIL ') + name + (f'  [{detail}]' if detail else ''), flush=True)


def exits(fn, *a):
    try:
        fn(*a)
    except SystemExit as e:
        return e.code
    return None


def fake_app_image(size=40000):
    hdr = bytearray(24)
    hdr[0] = 0xE9; hdr[1] = 1                          # magic, one segment; chip_id (12..13) = 0 ESP32
    seg = struct.pack('<II', 0x3F400020, size - 32)
    desc = struct.pack('<IIII', 0xABCD5432, 0, 0, 0) + b'r2.0.0'.ljust(32, b'\0') + b'nori-test'.ljust(32, b'\0')
    body = bytes(hdr) + seg + desc
    return body + bytes((i * 7) & 0xFF for i in range(size - len(body)))


class FakeDevice(threading.Thread):
    """ArduinoOTA's device side with password hash `passmd5` (None = no auth)."""
    def __init__(self, passmd5):
        super().__init__(daemon=True)
        self.passmd5 = passmd5
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(('127.0.0.1', 0))
        self.udp.settimeout(5)
        self.port = self.udp.getsockname()[1]
        self.cmd = self.received = self.connected = None
        self.start()

    def run(self):
        try:
            data, peer = self.udp.recvfrom(256)
            cmd, lport, size, md5 = data.decode().split()
            self.cmd, lport, size = int(cmd), int(lport), int(size)
            if self.cmd not in (0, 100):
                return
            if self.passmd5:
                nonce = hashlib.md5(b'micros').hexdigest()
                self.udp.sendto(('AUTH ' + nonce).encode(), peer)
                data, peer = self.udp.recvfrom(256)
                c, cnonce, resp = data.decode().split()
                want = hashlib.md5(f'{self.passmd5}:{nonce}:{cnonce}'.encode()).hexdigest()
                if int(c) != 200 or resp != want:
                    self.udp.sendto(b'Authentication Failed', peer)
                    return
            self.udp.sendto(b'OK', peer)
            s = socket.create_connection((peer[0], lport), timeout=5)
            self.connected = True
            buf = b''
            while len(buf) < size:
                chunk = s.recv(1460)
                if not chunk:
                    break
                buf += chunk
                s.sendall(str(len(chunk)).encode())
            self.received = buf
            s.sendall(b'OK' if hashlib.md5(buf).hexdigest() == md5 else b'Error: MD5 Check Failed')
            s.close()
        except (socket.timeout, OSError, ValueError):
            pass


def main():
    q = io.StringIO()
    pwfile = os.path.join(T, 'ota.pass')

    # genkey: random, 0600, never overwrites
    nori_ota.main(['genkey', pwfile])
    st = os.stat(pwfile)
    pw = nori_ota.read_password_file(pwfile)
    check('genkey writes a 0600 file', (st.st_mode & 0o777) == 0o600)
    pw2file = os.path.join(T, 'ota2.pass')
    nori_ota.main(['genkey', pw2file])
    check('genkey password is long and random', len(pw) >= 40 and pw != nori_ota.read_password_file(pw2file))
    check('genkey refuses to overwrite', exits(nori_ota.main, ['genkey', pwfile]) == 2 and nori_ota.read_password_file(pwfile) == pw)
    loose = os.path.join(T, 'loose.pass')
    with open(loose, 'w') as f:
        f.write('hunter2-but-longer\n')
    os.chmod(loose, 0o644)
    check('group/other-readable password file refused', exits(nori_ota.read_password_file, loose) == 2)
    empty = os.path.join(T, 'empty.pass')
    open(empty, 'w').close(); os.chmod(empty, 0o600)
    check('empty password file refused', exits(nori_ota.read_password_file, empty) == 2)

    # frame: valid link frame carrying MD5(password); cross-checked with the C parser
    frame_path = os.path.join(T, 'otakey.frame')
    nori_ota.main(['frame', '--password-file', pwfile, '--out', frame_path])
    line = open(frame_path, 'rb').read()
    check('frame file is 0600', (os.stat(frame_path).st_mode & 0o777) == 0o600)
    check('CRC-16/CCITT-FALSE check value', nori_ota.crc16(b'123456789') == 0x29B1)
    body, crc = line[1:].rstrip(b'\n').split(b'*')
    check('frame layout and CRC', line.startswith(b'$NL1 1 OTAKEY h=') and int(crc, 16) == nori_ota.crc16(body)
          and body.endswith(hashlib.md5(pw).hexdigest().encode()))
    check('frame does not contain the password', pw not in line)
    fc = os.path.join(ROOT, 'build', 'tests', 'frame_check')
    if os.path.exists(fc):
        out = subprocess.run([fc], input=line, capture_output=True).stdout.decode()
        check('C nl_parse + device core accept the frame', out.strip() == 'OK OTAKEY ' + hashlib.md5(pw).hexdigest(), out.strip())

    # check: app images only
    img = os.path.join(T, 'app.bin')
    open(img, 'wb').write(fake_app_image())
    n, md5, _ = nori_ota.check_image(img)
    check('synthetic app image accepted', n == 40000)
    real = os.path.join(ROOT, 'release', 'wt32', 'firmware.bin')   # wt32-r3: the released application image
    if os.path.exists(real):
        rn, _, info = nori_ota.check_image(real)
        check('released wt32 firmware.bin accepted and fits the slot', rn <= nori_ota.APP_SLOT, f'{rn} bytes, {info}')
    fs = os.path.join(T, 'spiffs.bin')
    open(fs, 'wb').write(b'\xff' * 65536)
    check('filesystem-style image refused', exits(nori_ota.check_image, fs) == 2)
    big = os.path.join(T, 'big.bin')
    open(big, 'wb').write(fake_app_image(nori_ota.APP_SLOT + 4096))
    check('oversized image refused', exits(nori_ota.check_image, big) == 2)
    s3 = bytearray(fake_app_image()); s3[12] = 9
    other = os.path.join(T, 's3.bin'); open(other, 'wb').write(bytes(s3))
    check('image for another chip refused', exits(nori_ota.check_image, other) == 2)

    # upload: correct password -> whole image, app command only
    dev = FakeDevice(hashlib.md5(pw).hexdigest())
    rc = nori_ota.upload('127.0.0.1', dev.port, pw, img, n, md5, timeout=3, log=q)
    dev.join(6)
    check('authenticated upload succeeds', rc == 0, q.getvalue().strip().splitlines()[-1] if q.getvalue() else '')
    check('device got the exact image', dev.received == open(img, 'rb').read())
    check('only the app (U_FLASH=0) command is ever sent', dev.cmd == 0)

    # wrong password -> refused before any transfer
    dev = FakeDevice(hashlib.md5(b'some-other-password').hexdigest())
    rc = nori_ota.upload('127.0.0.1', dev.port, pw, img, n, md5, timeout=3, log=q)
    dev.join(6)
    check('wrong password refused, nothing transferred', rc == 1 and not dev.connected)

    # a device that does not ask for a password is not used
    dev = FakeDevice(None)
    rc = nori_ota.upload('127.0.0.1', dev.port, pw, img, n, md5, timeout=3, log=q)
    dev.join(6)
    check('unauthenticated device refused, nothing transferred', rc == 1 and not dev.received)

    # nothing secret on the command line: the CLI has no password argument at all
    h = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'nori_ota.py'), 'upload', '-h'],
                       capture_output=True, text=True).stdout
    check('CLI offers no password/auth argument', '--password ' not in h and '-a ' not in h and '--auth' not in h)
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'nori_ota.py'), 'upload', '--host', '127.0.0.1', img],
                       stdin=subprocess.DEVNULL, capture_output=True, text=True)
    check('no password from a pipe without a password file', r.returncode == 2 and 'not a terminal' in r.stderr)

    print('ota-updater: %d checks, %d failures' % (len(RESULTS), RESULTS.count(False)))
    return 0 if all(RESULTS) else 1


if __name__ == '__main__':
    sys.exit(main())
