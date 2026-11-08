#!/usr/bin/env python3
"""Nori accessory R2: HTTP push OTA updater (firmware r2.0.1 or later).

The uploader connects to the device (TCP 80), so unlike ArduinoOTA/espota it
needs no inbound firewall rule.  HTTP Digest auth (MD5, qop=auth), user "nori",
keyed with md5(password): the same secret OTAKEY provisioned, never sent in the
clear.  The password comes from the same user-only file as nori_ota.py (or a
hidden prompt); it never appears on the command line, in output or on disk.

  nori_http_ota.py status --host HOST [--password-file PASSFILE] [--json]
  nori_http_ota.py diag --host HOST [--password-file PASSFILE]
        wt32-r3: the whole authenticated diagnostics document (reset cause and history, previous
        boot, network counters, sensor, presence, event log) as JSON; never contains a secret
  nori_http_ota.py upload --host HOST [--password-file PASSFILE] IMAGE
        validate IMAGE (app image only), POST it to /update, then poll /status
        until the device has rebooted into the other slot running the image's
        firmware version with ArduinoOTA and HTTP OTA ready again.

Digest protects the secret and stops replays (the device burns the nonce on
every upload), but not the image itself: use it on a trusted LAN only.
"""
import argparse
import hashlib
import json
import os
import re
import sys
import time

try:
    import requests
    from requests.auth import HTTPDigestAuth
except ImportError:                    # pragma: no cover
    sys.stderr.write('nori_http_ota: needs the "requests" package (pip install requests)\n')
    sys.exit(2)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nori_ota  # noqa: E402  (password file handling and image validation)

USER, REALM = 'nori', 'nori-ota'
HTTP_PORT = 80
FW_TAG = re.compile(rb'NORI_FW=([A-Za-z0-9._-]{1,31})\x00')


class Refused(Exception):
    pass


def digest_auth(password):
    return HTTPDigestAuth(USER, hashlib.md5(password).hexdigest())


def image_fw(data):
    """Firmware version tagged in the image (main.cpp nori_fw_tag), or None."""
    m = FW_TAG.search(data)
    return m.group(1).decode() if m else None


def session():
    s = requests.Session()
    s.trust_env = False                 # no proxies or .netrc from the environment
    return s


AUTH_ATTEMPTS = 3        # fresh Digest challenges per GET before a 401 counts as a real refusal


def _digest_challenge(r):
    chal = r.headers.get('WWW-Authenticate', '')
    return chal if chal.startswith('Digest') and ('realm="%s"' % REALM) in chal else None


def get_status_auth(sess, base, auth, timeout, path='/status', attempts=AUTH_ATTEMPTS):
    """GET /status (or wt32-r3 /diag) with a FRESH Digest exchange per attempt, insisting that the device
    demanded Digest auth for realm nori-ota first.  Returns (status dict, the auth object that answered
    the last challenge; upload() signs its POST with it, i.e. with that challenge's nonce).

    wt32-r4 host-tool fix: the device's WebServer keeps ONE server nonce and every challenge it sends (to
    any client) replaces it, so a concurrent client (a diag sampler, a browser) can invalidate our signed
    retry and the device answers it with a fresh 401.  That transient 401 used to end status/diag and the
    post-upload polling with "authentication refused".  Now a 401 that carries a proper Digest challenge
    is retried with a new exchange, at most `attempts` times; a wrong password still fails (every attempt
    is refused).  Never retried: a 401 without a Digest challenge or for another realm, and a 200 that
    came without a challenge (no unauthenticated success).  The auth object is never reused across GETs,
    so no stale-nonce preemptive Authorization is ever sent.  Only GETs are retried, never the POST."""
    for i in range(max(1, attempts)):
        a = HTTPDigestAuth(auth.username, auth.password)            # fresh: no cached nonce, nc=1
        r = sess.get(base + path, auth=a, timeout=timeout, allow_redirects=False)
        if r.status_code == 401:
            if not r.headers.get('WWW-Authenticate', '').startswith('Digest'):
                raise Refused('device offers no Digest authentication; refusing')
            if not _digest_challenge(r):
                raise Refused('device asks for a Digest realm other than %s; refusing' % REALM)
            if i + 1 < attempts:
                time.sleep(0.15 * (i + 1))                          # let a concurrent client's exchange finish
                continue
            raise Refused('authentication refused (wrong password, or the device has another OTA key)')
        break
    if not (r.history and _digest_challenge(r.history[0])):
        raise Refused('device answered without a Digest challenge for realm %s; refusing' % REALM)
    if r.status_code != 200:
        raise Refused('%s: HTTP %d' % (path, r.status_code))
    try:
        st = r.json()
    except ValueError:
        raise Refused('%s: not JSON' % path)
    if not isinstance(st, dict) or not {'fw', 'part', 'img'} <= set(st):
        raise Refused('%s: unexpected reply' % path)
    return st, a


def get_status(sess, base, auth, timeout, path='/status', attempts=AUTH_ATTEMPTS):
    return get_status_auth(sess, base, auth, timeout, path, attempts)[0]


def fmt(st):
    return ' '.join('%s=%s' % (k, st.get(k)) for k in ('fw', 'part', 'img', 'ota', 'http', 'uptime_s', 'rt', 'lan', 'pm3_ip', 'tog', 'wifi_i', 'wifi_n', 'wdt', 'ph_prev') if k in st)   # wt32-r4: rt lan pm3_ip; r5: tog; r5.1: wifi_i wifi_n; r5.1.3: wdt ph_prev


def short(text):
    return ''.join(c if c.isprintable() else ' ' for c in text.strip())[:160]


_DIGEST = re.compile(r'(?i)(digest\s+[^)\]]*|(?:authorization|response|cnonce|nonce|opaque)\s*[=:]\s*\S+)')


def err_detail(e, *secret):
    """wt32-r3: why a request failed, for the operator: the exception type plus its message (which
    names the root cause: refused, reset by peer, remote end closed, timed out, ...), never a secret.
    The password, md5(password) and anything that looks like a Digest header or parameter are
    removed, the text is reduced to printable characters and capped."""
    msg = str(e)
    for x in secret:
        if x:
            x = x.decode('utf-8', 'replace') if isinstance(x, bytes) else x
            msg = msg.replace(x, '<redacted>')
    msg = _DIGEST.sub('<redacted>', msg)
    msg = re.sub(r'<[^<>]* object at 0x[0-9a-fA-F]+>,? ?', '', msg)       # urllib3 reprs: noise only
    msg = ''.join(c if c.isprintable() else ' ' for c in msg.strip())[:320]
    name = type(e).__name__
    return '%s: %s' % (name, msg) if msg else name


def upload(base, password, image, size, md5hex, expect_fw=None, timeout=10.0, reboot_timeout=120.0,
           wait_valid=False, poll=2.0, log=sys.stderr):
    with open(image, 'rb') as f:
        data = f.read()
    if len(data) != size or hashlib.md5(data).hexdigest() != md5hex:
        log.write('image changed while reading it\n')
        return 1
    expect_fw = expect_fw or image_fw(data)
    sess = session()
    auth = digest_auth(password)
    hidden = (password, hashlib.md5(password).hexdigest())
    try:
        before, auth = get_status_auth(sess, base, auth, timeout)   # auth now holds the nonce the upload is signed with
    except Refused as e:
        log.write('%s\n' % e)
        return 1
    except requests.RequestException as e:
        log.write('device not reachable: %s\n' % err_detail(e, *hidden))
        return 1
    log.write('device: %s\n' % fmt(before))
    log.write('uploading %d bytes%s\n' % (size, ' (fw %s)' % expect_fw if expect_fw else ''))
    t_up = time.monotonic()
    try:
        r = sess.post(base + '/update', auth=auth, timeout=(timeout, 300), allow_redirects=False,
                      headers={'X-Nori-Image-MD5': md5hex},
                      files={'firmware': ('firmware.bin', data, 'application/octet-stream')})
    except requests.RequestException as e:
        log.write('upload failed after %.1f s: %s (the device keeps its current image unless it received all of it; '
                  'check with "status")\n' % (time.monotonic() - t_up, err_detail(e, *hidden)))
        return 1
    if r.status_code != 200 or not r.text.startswith('OK'):
        log.write('device refused the image: HTTP %d %s\n' % (r.status_code, short(r.text)))
        return 1
    log.write('device accepted the image and is rebooting\n')

    deadline = time.monotonic() + reboot_timeout
    last, seen_new, last_err = None, False, None
    refused = 0                                      # consecutive polls refused even after AUTH_ATTEMPTS fresh challenges
    time.sleep(poll)
    while time.monotonic() < deadline:
        try:
            st = get_status(session(), base, digest_auth(password), min(timeout, 3.0))
        except Refused as e:
            # A just-rebooted device, or a concurrent client rotating the single server nonce, can refuse a
            # poll; the password was proven before the upload.  Bounded: 3 refused polls in a row (each
            # with 3 fresh challenges) end it.  Protocol refusals (no Digest, other realm, no challenge)
            # end it at once.
            if 'authentication refused' not in str(e) or refused >= 2:
                log.write('%s\n' % e)
                return 1
            refused += 1
            last_err = str(e)
            log.write('a')
            log.flush()
            time.sleep(poll)
            continue
        except requests.RequestException as e:
            last_err = err_detail(e, *hidden)
            log.write('.')
            log.flush()
            time.sleep(poll)
            continue
        refused = 0
        last = st
        new = st.get('part') != before.get('part') and (not expect_fw or st.get('fw') == expect_fw)
        seen_new = seen_new or new
        if new and st.get('ota') == 'ready' and st.get('http') == 'ready' and (not wait_valid or st.get('img') == 'valid'):
            log.write('\nupdated: %s\n' % fmt(st))
            if st.get('img') == 'pending':
                log.write('image is rollback-pending; it is marked valid after ~30 s with OTA up\n')
            return 0
        time.sleep(poll)
    log.write('\nno confirmation within %.0f s; last status: %s\n' % (reboot_timeout, fmt(last) if last else 'unreachable'))
    if last_err:
        log.write('last poll error: %s\n' % last_err)
    if last and not seen_new:
        log.write('device still runs the previous slot/firmware (rolled back or never switched)\n')
    return 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('status', 'diag', 'upload'):
        p = sub.add_parser(name)
        p.add_argument('--host', required=True, help='IP or nori-accessory.local')
        p.add_argument('--port', type=int, default=HTTP_PORT)
        p.add_argument('--password-file')
        p.add_argument('--timeout', type=float, default=10.0)
        if name == 'status':
            p.add_argument('--json', action='store_true', help='print the whole /status reply (wt32-r3 adds diagnostics)')
        if name == 'upload':
            p.add_argument('--max-size', type=lambda s: int(s, 0), default=nori_ota.APP_SLOT)
            p.add_argument('--expect-fw', help='firmware version to wait for (default: read from the image)')
            p.add_argument('--reboot-timeout', type=float, default=120.0)
            p.add_argument('--wait-valid', action='store_true', help='also wait until the image is marked valid')
            p.add_argument('image')
    a = ap.parse_args(argv)
    if not re.fullmatch(r'[A-Za-z0-9.-]+', a.host) or not 0 < a.port < 65536:
        nori_ota.die('--host must be a plain IPv4 address or host name')
    base = 'http://%s:%d' % (a.host, a.port)

    if a.cmd in ('status', 'diag'):
        password = nori_ota.get_password(a.password_file)
        try:
            st = get_status(session(), base, digest_auth(password), a.timeout, '/diag' if a.cmd == 'diag' else '/status')
            print(json.dumps(st, indent=1, sort_keys=False) if a.cmd == 'diag' or a.json else fmt(st))
        except Refused as e:
            sys.stderr.write('%s\n' % e)
            return 1
        except requests.RequestException as e:
            sys.stderr.write('device not reachable: %s\n' % err_detail(e, password, hashlib.md5(password).hexdigest()))
            return 1
        return 0

    n, md5hex, info = nori_ota.check_image(a.image, a.max_size)   # dies on non-app/oversized/other-chip images
    sys.stderr.write('image: %d bytes md5=%s %s\n' % (n, md5hex, info))
    password = nori_ota.get_password(a.password_file)
    return upload(base, password, a.image, n, md5hex, a.expect_fw, a.timeout, a.reboot_timeout, a.wait_valid)


if __name__ == '__main__':
    sys.exit(main())
