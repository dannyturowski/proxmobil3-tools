#!/usr/bin/env python3
"""wt32-r5.1.3: source checks for the loop watchdog and the network-stall fix (firmware/src/main.cpp, CMakeLists.txt).

The ELF checks are in tools/verify_build.sh; these pin what the build cannot show:
  - every loop in main.cpp that sleeps (delay) feeds the watchdog, apart from WDTTEST's deliberate stall;
  - the ArduinoOTA hole opens only in onStart and is closed by onError and after handle(); progress is wired;
  - loop() feeds first; the watchdog is never unsubscribed outside the HAL;
  - every HTTP reply goes through the bounded tx();
  - mDNS is off and ArduinoOTA is not restarted on an address change unless mDNS is turned back on;
  - unchanged on purpose: pins, TX power, modem sleep, brownout detector, partitions (verify_build checks the files).
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = open(os.path.join(ROOT, 'firmware/src/main.cpp'), encoding='utf-8').read()
CMAKE = open(os.path.join(ROOT, 'firmware/src/CMakeLists.txt'), encoding='utf-8').read()
checks = fails = 0


def check(what, ok):
    global checks, fails
    checks += 1
    if not ok:
        fails += 1
        print('FAIL', what)


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', lambda m: ' ' * len(m.group(0)), s, flags=re.S)
    return re.sub(r'//[^\n]*', lambda m: ' ' * len(m.group(0)), s)


CODE = strip_comments(SRC)


def block_after(s, i):
    """The statement after a loop header starting at s[i] ('while (' / 'for ('): its text."""
    depth, j = 0, s.index('(', i)
    while True:
        if s[j] == '(':
            depth += 1
        elif s[j] == ')':
            depth -= 1
            if depth == 0:
                break
        j += 1
    k = j + 1
    while s[k].isspace():
        k += 1
    if s[k] != '{':
        return s[i:s.index(';', k) + 1]
    depth, m = 0, k
    while True:
        if s[m] == '{':
            depth += 1
        elif s[m] == '}':
            depth -= 1
            if depth == 0:
                return s[i:m + 1]
        m += 1


def function_body(name):
    m = re.search(r'\n[^\n;]*\b' + re.escape(name) + r'\s*\([^;{]*\)\s*(?:override\s*)?\{', CODE)
    assert m, name
    return _body_from(m.end() - 1)


def _body_from(k):
    depth, m = 0, k
    while True:
        if CODE[m] == '{':
            depth += 1
        elif CODE[m] == '}':
            depth -= 1
            if depth == 0:
                return CODE[k:m + 1]
        m += 1


# 1. sleeping loops feed
sleepers = 0
for m in re.finditer(r'\b(while|for)\s*\(', CODE):
    body = block_after(CODE, m.start())
    if 'delay(' not in body:
        continue
    sleepers += 1
    line = CODE.count('\n', 0, m.start()) + 1
    if 'for (;;) delay(10);' in body:
        before = CODE[max(0, m.start() - 200):m.start()]
        check('main.cpp:%d the only unfed loop is WDTTEST\'s deliberate stall' % line, 'ph_go(NPH_WDT_TEST)' in before)
        continue
    check('main.cpp:%d a loop that sleeps also feeds the watchdog' % line, 'wdt_feed()' in body)
check('found the bounded wait loops (head, upload, drain, Wi-Fi body, reply, WDTTEST)', sleepers >= 6)

# 2. the ArduinoOTA hole
start, err, svc, lp = (function_body(n) for n in ('ota_on_start', 'ota_on_error', 'ota_service', 'loop'))
check('ota_on_start opens the hole', 'ota_hole_begin()' in start)
check('ota_on_error closes it before anything else', err.split(';')[1].strip().startswith('ota_hole_end()') if ';' in err else False)
check('loop() closes it after handle(), whatever happened', re.search(r'ota->handle\(\);\s*ota_hole_end\(\);', lp) is not None)
check('progress callbacks re-arm the guard', 'onProgress(ota_on_progress)' in svc and 'nwd_progress' in function_body('ota_on_progress'))
check('ota_hole_begin is only called from ota_on_start', CODE.count('ota_hole_begin()') == 2)   # definition + call
check('loop() feeds first', lp.lstrip('{ \n').startswith('wdt_feed();'))
check('nothing unsubscribes outside the HAL', CODE.count('esp_task_wdt_delete') == 1 and 'disableLoopWDT' not in CODE)
check('setup() starts the watchdog', 'nwd_start(&lwd)' in function_body('setup'))

# 3. replies
check('every WebServer write goes through tx() (both _currentClientWrite overrides), none bypasses it',
      re.search(r'_currentClientWrite\(const char \*b, size_t l\) override \{ return tx\(b, l\)', CODE) is not None and
      re.search(r'_currentClientWrite_P\(PGM_P b, size_t l\) override \{ return tx\(b, l\)', CODE) is not None and
      '_currentClient.write(' not in CODE and '_currentClient.print' not in CODE)
check('a connection whose write failed fails at once afterwards, reset per request',
      'if (tx_failed) return false;' in CODE and 'tx_failed = false;' in function_body('handleClient'))
TX = function_body('tx')
check('tx() feeds and is bounded', all(x in TX for x in ('wdt_feed()', 'HTTP_TX_IDLE_MS', 'HTTP_TX_MS', 'MSG_DONTWAIT')))
TXLOOP = block_after(TX, TX.index('while (ok && n)'))
check('tx() checks the total deadline on every pass, before each send (short writes cannot extend it)',
      re.search(r'if \(now - t0 > HTTP_TX_MS[^\n]*\{ ok = false; break; \}', TXLOOP) is not None and
      TXLOOP.index('HTTP_TX_MS') < TXLOOP.index('lwip_send(') and TXLOOP.index('continue;') > TXLOOP.index('HTTP_TX_MS'))

# 2b. guard re-arm fails closed (nori_lwdt.h + the HAL)
LWDT = strip_comments(open(os.path.join(ROOT, 'shared/nori_sense/src/nori_lwdt.h'), encoding='utf-8').read())
check('wd_arm() stops before starting (so a failed start leaves no guard: the case nwd_progress handles)',
      re.search(r'esp_timer_stop\(guard_timer\);\s*return esp_timer_start_once', function_body('wd_arm')) is not None)
check('nwd_progress: a failed re-arm resubscribes, else calls fail()',
      re.search(r'rearm_fails\+\+;\s*if \(w->h->add\(w->h->ctx\) == 0\)', LWDT) is not None and 'w->h->fail(w->h->ctx);' in LWDT)
check('the HAL fail() aborts and marks the record', all(x in function_body('wd_fail') for x in ('esp_system_abort', 'guard_ms')) and
      re.search(r'WD_HAL = \{[^}]*wd_fail \}', CODE) is not None)
check('progress syncs the phase record state', 'ph_rtc.wdt = (uint32_t)lwd.state;' in function_body('ota_on_progress'))

# 4. mDNS and the address-change restart
check('NORI_OTA_MDNS defaults to 0', re.search(r'^#define NORI_OTA_MDNS 0$', SRC, re.M) is not None)
check('not overridden by the build', not re.search(r'^\s*NORI_OTA_MDNS=', CMAKE, re.M))
check('ArduinoOTA restarts on an address change only with mDNS', 'NORI_OTA_MDNS && ip != ota_ip' in svc and svc.count('ip != ota_ip') == 1)

# 5. unchanged on purpose
for k, v in (('NORI_PIN_BTN1', '14'), ('NORI_PIN_BTN2', '4'), ('NORI_PIN_TOG_FIVE', '15'), ('NORI_PIN_TOG_ONE', '2'),
             ('NORI_PIN_SDA', '33'), ('NORI_PIN_SCL', '32'), ('NORI_TOF_HZ', '5')):
    check('pin/sensor define %s=%s unchanged' % (k, v), re.search(r'^\s*%s=%s\b' % (k, v), CMAKE, re.M) is not None)
check('no TX power change', 'setTxPower' not in CODE and 'esp_wifi_set_max_tx_power' not in CODE)
check('modem sleep still off, once', CODE.count('WiFi.setSleep(false)') == 1 and 'setSleep(true)' not in CODE and 'esp_wifi_set_ps' not in CODE)
check('brownout detector untouched', not re.search(r'BROWN_OUT|brownout_hal|esp_brownout', CODE))
check('version r5.1.3', 'NORI_FW_VERSION="wt32-r5.1.3"' in CMAKE)

print('check_wdt_source: %d checks, %d failures' % (checks, fails))
sys.exit(1 if fails else 0)
