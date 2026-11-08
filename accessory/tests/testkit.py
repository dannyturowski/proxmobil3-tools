"""Minimal test helpers for the accessory's Python suites (stands in for the private
repository's larger integration harness, which also drives an application bridge).

B        directory of the built host test binaries (accessory/build/tests)
T        scratch directory for logs of this run
check()  record and print one PASS/FAIL line
Proc     a child process whose combined output goes to a log file in T
cleanup  kill every Proc still running
"""
import os
import re
import signal
import subprocess
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
B = os.environ.get('WT32_TEST_BIN', os.path.join(ROOT, 'build', 'tests'))
T = tempfile.mkdtemp(prefix='wt32-test-')
PROCS = []
RESULTS = []


def check(name, ok, detail=''):
    RESULTS.append((name, bool(ok)))
    print(('PASS ' if ok else 'FAIL ') + name + (f'  [{detail}]' if detail else ''), flush=True)


class Proc:
    def __init__(self, name, argv, stdin=False):
        self.name, self.log = name, os.path.join(T, name + '.log')
        self.f = open(self.log, 'w')
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE if stdin else subprocess.DEVNULL, stdout=self.f,
                                  stderr=subprocess.STDOUT, text=True)
        self.pos = 0
        PROCS.append(self)

    def text(self):
        with open(self.log, errors='replace') as f:
            return f.read()

    def mark(self):
        self.pos = len(self.text())

    def wait_for(self, pattern, timeout=6.0):
        end = time.monotonic() + timeout
        rx = re.compile(pattern)
        while time.monotonic() < end:
            m = rx.search(self.text(), self.pos)
            if m:
                self.pos = m.end()
                return m
            time.sleep(0.02)
        return None

    def since(self):
        return self.text()[self.pos:]

    def cmd(self, c):
        self.p.stdin.write(c + '\n')
        self.p.stdin.flush()

    def stop(self, sig=signal.SIGTERM):
        if self.p.poll() is None:
            self.p.send_signal(sig)
            try:
                self.p.wait(5)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        return self.p.returncode


def cleanup():
    for p in PROCS:
        if p.p.poll() is None:
            p.p.kill()
