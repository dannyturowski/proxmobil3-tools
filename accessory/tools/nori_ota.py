#!/usr/bin/env python3
"""Nori accessory R2: OTA password tooling and an espota-compatible app updater.

The OTA password never appears on a command line, in the environment or in the
firmware/source tree: it is generated into a 0600 file (or typed at a hidden
prompt) and only its MD5 ever reaches the device (OTAKEY over the CRC-framed
link).  Uploads are app images only (espota command 0); there is no
filesystem option.

  nori_ota.py genkey  PASSFILE                 new random password -> PASSFILE (0600, must not exist)
  nori_ota.py frame   --password-file PASSFILE --out FRAMEFILE
                                               "$NL1 1 OTAKEY h=<md5>*CRC" line -> FRAMEFILE (0600)
  nori_ota.py check   IMAGE                    validate an app image (no network)
  nori_ota.py upload  --host HOST [--password-file PASSFILE] IMAGE
                                               authenticated espota upload (prompts if no file)

Note: ArduinoOTA's digest uses MD5(password) as the key, so the hash and the
frame file are as sensitive as the password itself.  Delete FRAMEFILE once the
device answered "OK OTAKEY".
"""
import argparse
import getpass
import hashlib
import os
import secrets
import socket
import stat
import struct
import subprocess
import sys

OTA_PORT = 3232
CMD_FLASH, CMD_AUTH = 0, 200          # espota/ArduinoOTA: U_FLASH, U_AUTH (U_SPIFFS=100 is never sent)
APP_SLOT = 0x140000                   # default.csv app0/app1
ESP_IMAGE_MAGIC = 0xE9
APP_DESC_MAGIC = 0xABCD5432           # esp_app_desc_t, first bytes of the first (DROM) segment
EMPTY_MD5 = hashlib.md5(b'').hexdigest()


def die(msg, code=2):
    sys.stderr.write('nori_ota: %s\n' % msg)
    sys.exit(code)


# ---- password handling --------------------------------------------------------------
def read_password_file(path):
    st = os.stat(path)
    if os.name == 'nt':
        # Windows stat() fabricates POSIX mode bits; check the actual NTFS ACL.
        env = dict(os.environ, NORI_SECRET_CHECK=os.path.abspath(path))
        env.pop('PSModulePath', None)  # Do not inject PowerShell 7 modules into Windows PowerShell 5.
        script = r'''$ErrorActionPreference='Stop'; $me=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value; $allowed=@($me,'S-1-5-18','S-1-5-32-544'); $acl=[IO.File]::GetAccessControl($env:NORI_SECRET_CHECK); foreach($rule in $acl.Access) { if($rule.AccessControlType -eq 'Allow') { $sid=$rule.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value; if($sid -notin $allowed) { exit 1 } } }; exit 0'''
        result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', script], env=env, capture_output=True)
        if result.returncode:
            if result.stderr:
                sys.stderr.write(result.stderr.decode(errors='replace'))
            die('password file must have a user-only Windows ACL')
    elif st.st_mode & (stat.S_IRWXG | stat.S_IRWXO):
        die('%s is accessible by group/others; chmod 600 it' % path)
    with open(path, 'rb') as f:
        data = f.read(4096)
    pw = data.split(b'\n', 1)[0].rstrip(b'\r')
    if not pw:
        die('%s: empty password' % path)
    return pw


def get_password(path):
    if path:
        return read_password_file(path)
    if not sys.stdin.isatty():
        die('no --password-file and stdin is not a terminal (refusing to read a password from a pipe)')
    pw = getpass.getpass('OTA password: ').encode()
    if not pw:
        die('empty password')
    return pw


def write_private(path, data):
    if os.name == 'nt':
        die('create a user-only NTFS directory and password file first; genkey/frame creation currently requires POSIX permissions')
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)   # never overwrite, never world-readable
    except FileExistsError:
        die('%s already exists; not overwriting' % path)
    with os.fdopen(fd, 'wb') as f:
        f.write(data)


# ---- link frame (shared/nori_link: CRC-16/CCITT-FALSE, upper-case hex) ---------------
def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def otakey_frame(password, seq=1):
    h = hashlib.md5(password).hexdigest()
    if h == EMPTY_MD5:
        die('empty password')
    body = 'NL1 %d OTAKEY h=%s' % (seq, h)
    return ('$%s*%04X\n' % (body, crc16(body.encode()))).encode()


# ---- image validation -----------------------------------------------------------------
def check_image(path, max_size=APP_SLOT):
    """Return (size, md5hex, info) or die.  Accepts only an ESP32 application image."""
    with open(path, 'rb') as f:
        img = f.read()
    n = len(img)
    if n < 0x20 + 0x100:
        die('%s: too small for an ESP32 app image' % path)
    if n > max_size:
        die('%s: %d bytes does not fit the %d byte app slot' % (path, n, max_size))
    if img[0] != ESP_IMAGE_MAGIC:
        die('%s: not an ESP32 image (magic %02x); filesystem images are not accepted' % (path, img[0]))
    chip_id = struct.unpack_from('<H', img, 12)[0]
    if chip_id != 0:
        die('%s: image is for chip id %d, not the original ESP32' % (path, chip_id))
    if struct.unpack_from('<I', img, 0x20)[0] != APP_DESC_MAGIC:
        die('%s: no esp_app_desc (not an application image)' % path)
    ver = img[0x30:0x50].split(b'\0', 1)[0].decode(errors='replace')
    proj = img[0x50:0x70].split(b'\0', 1)[0].decode(errors='replace')
    return n, hashlib.md5(img).hexdigest(), 'project=%s idf_app_version=%s' % (proj, ver)


# ---- espota protocol (same wire format as framework tools/espota.py) -----------------
def upload(host, port, password, image, size, md5hex, timeout=10.0, log=sys.stderr, listen_port=0):
    addr = socket.getaddrinfo(host, port, socket.AF_INET, socket.SOCK_DGRAM)[0][4]
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.bind(('0.0.0.0', listen_port))
    srv.listen(1)
    lport = srv.getsockname()[1]
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.settimeout(timeout)
    try:
        invite = ('%d %d %d %s\n' % (CMD_FLASH, lport, size, md5hex)).encode()
        data = None
        for _ in range(10):
            udp.sendto(invite, addr)
            try:
                data = udp.recv(64).decode(errors='replace')
                break
            except socket.timeout:
                log.write('.')
                log.flush()
        if data is None:
            log.write('no answer from %s:%d\n' % addr)
            return 1
        if not data.startswith('AUTH '):
            # an OK here would mean the device runs without a password: refuse to use it
            log.write('device did not ask for authentication (%r); refusing\n' % data[:20])
            return 1
        nonce = data.split()[1]
        cnonce = secrets.token_hex(16)
        passmd5 = hashlib.md5(password).hexdigest()
        resp = hashlib.md5(('%s:%s:%s' % (passmd5, nonce, cnonce)).encode()).hexdigest()
        udp.sendto(('%d %s %s\n' % (CMD_AUTH, cnonce, resp)).encode(), addr)
        try:
            data = udp.recv(64).decode(errors='replace')
        except socket.timeout:
            log.write('no answer to authentication\n')
            return 1
        if data != 'OK':
            log.write('authentication refused\n')
            return 1
        log.write('authenticated; waiting for the device to connect back on port %d\n' % lport)
        srv.settimeout(timeout)
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            log.write('device did not connect back (host firewall?)\n')
            return 1
        with conn, open(image, 'rb') as f:
            conn.settimeout(timeout)
            sent, last = 0, ''
            while True:
                chunk = f.read(1024)
                if not chunk:
                    break
                conn.sendall(chunk)
                sent += len(chunk)
                last = conn.recv(32).decode(errors='replace')
                log.write('\r%3d%%' % (sent * 100 // size))
                log.flush()
            log.write('\n')
            for _ in range(5):
                if 'OK' in last:
                    log.write('device accepted the image; it reboots into it now\n')
                    return 0
                if 'rror' in last:
                    break
                conn.settimeout(60)
                last = conn.recv(64).decode(errors='replace')
                if not last:
                    break
            log.write('device reported failure: %s\n' % last.strip()[:60])
            return 1
    finally:
        udp.close()
        srv.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    g = sub.add_parser('genkey')
    g.add_argument('passfile')
    fr = sub.add_parser('frame')
    fr.add_argument('--password-file', required=True)
    fr.add_argument('--out', required=True)
    c = sub.add_parser('check')
    c.add_argument('image')
    c.add_argument('--max-size', type=lambda s: int(s, 0), default=APP_SLOT)
    u = sub.add_parser('upload')
    u.add_argument('--host', required=True, help='IP or nori-accessory.local')
    u.add_argument('--port', type=int, default=OTA_PORT)
    u.add_argument('--password-file')
    u.add_argument('--max-size', type=lambda s: int(s, 0), default=APP_SLOT)
    u.add_argument('--timeout', type=float, default=10.0)
    u.add_argument('--listen-port', type=int, default=0, help='Fixed callback TCP port for a narrow firewall rule')
    u.add_argument('image')
    a = ap.parse_args(argv)

    if a.cmd == 'genkey':
        write_private(a.passfile, (secrets.token_urlsafe(32) + '\n').encode())
        print('wrote a new OTA password to %s (0600); keep it off the device and out of git' % a.passfile)
        return 0
    if a.cmd == 'frame':
        write_private(a.out, otakey_frame(read_password_file(a.password_file)))
        print('wrote the OTAKEY frame to %s (0600); send it over the link, then delete it' % a.out)
        return 0
    if a.cmd == 'check':
        n, md5hex, info = check_image(a.image, a.max_size)
        print('ok: %d bytes (%.1f%% of slot) md5=%s %s' % (n, 100.0 * n / a.max_size, md5hex, info))
        return 0
    n, md5hex, info = check_image(a.image, a.max_size)
    sys.stderr.write('image: %d bytes md5=%s %s\n' % (n, md5hex, info))
    return upload(a.host, a.port, get_password(a.password_file), a.image, n, md5hex, a.timeout, listen_port=a.listen_port)


if __name__ == '__main__':
    sys.exit(main())
