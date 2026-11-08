#!/usr/bin/env python3
"""wt32-r2 build check: which functions does FUNC call, as linked in an Xtensa ELF?

Direct calls (call0/4/8/12 <sym>) are read from the disassembly.  Calls from IRAM into flash (and
any other far call) are `l32r aN, <literal>` + `callx8 aN`: objdump shows only the literal's address,
so the 32-bit literal is read from the ELF section that holds it and mapped back to a symbol.

  elf_calls.py [--refs] ELF OBJDUMP NM FUNC [CALLEE ...]
prints every callee found; with CALLEEs, exits 1 unless all of them are called from FUNC.
--refs also counts functions whose address FUNC only loads (e.g. to store a function pointer).
Standard library only.
"""
import re
import struct
import subprocess
import sys


def sections(elf):
    with open(elf, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
        raise SystemExit('not a 32-bit little-endian ELF')
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x2e)
    out = []
    for i in range(shnum):
        _, typ, _, addr, off, size = struct.unpack_from('<IIIIII', data, shoff + i * shentsize)
        if typ == 1 and addr and size:            # SHT_PROGBITS, loaded
            out.append((addr, size, off))
    return data, out


def word_at(data, secs, addr):
    for a, n, off in secs:
        if a <= addr and addr + 4 <= a + n:
            return struct.unpack_from('<I', data, off + addr - a)[0]
    return None


def main(argv):
    refs = len(argv) > 1 and argv[1] == '--refs'
    if refs:
        argv = argv[:1] + argv[2:]
    elf, objdump, nm, func, want = argv[1], argv[2], argv[3], argv[4], argv[5:]
    syms = {}
    for line in subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in 'Tt':
            syms.setdefault(int(p[0], 16), p[2])
    dis = subprocess.run([objdump, '-d', '--disassemble=' + func, elf], capture_output=True, check=True).stdout.decode('latin-1')
    data, secs = sections(elf)
    called, lit = set(), {}
    for line in dis.splitlines():
        m = re.search(r'\bcall(?:0|4|8|12)\s+[0-9a-f]+ <([^>+]+)>', line)
        if m:
            called.add(m.group(1))
            continue
        m = re.search(r'\bl32r\s+(a\d+),\s*([0-9a-f]+)', line)
        if m:
            v = word_at(data, secs, int(m.group(2), 16))
            lit[m.group(1)] = syms.get(v) if v is not None else None
            if refs and lit[m.group(1)]:
                called.add(lit[m.group(1)])
            continue
        m = re.search(r'\bcallx(?:0|4|8|12)\s+(a\d+)', line)
        if m and lit.get(m.group(1)):
            called.add(lit[m.group(1)])
    for c in sorted(called):
        print(c)
    missing = [w for w in want if w not in called]
    if missing:
        sys.stderr.write('%s does not call: %s\n' % (func, ' '.join(missing)))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
