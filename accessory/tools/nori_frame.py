#!/usr/bin/env python3
"""r5.1: print one Nori link frame for sending by hand on a serial port (PM3 side, linkd stopped).

    $NL1 <seq> <KIND>[ <args>]*<CRC-16/CCITT-FALSE of the text between '$' and '*', 4 hex>\\n

Same framing as tools/nori_provision_usb.py and shared/nori_link/src/nori_link.c.
Examples:
    nori_frame.py CFG axis=3 axr=70
    nori_frame.py --seq 7 IICSCAN hz=100
    nori_frame.py DIAG
Only the standard library is used (binascii.crc_hqx with init 0xFFFF is CRC-16/CCITT-FALSE).
"""
import binascii
import re
import sys


def frame(kind, args=(), seq=1):
    if not re.fullmatch(r'[A-Z]{2,8}', kind):
        raise ValueError('KIND must be 2-8 letters A-Z (nl_parse refuses anything else)')
    if len(args) > 10:
        raise ValueError('at most 10 args per frame (NL_ARGS_MAX)')
    for a in args:
        if not a or any(c < '!' or c > '~' or c in '$*' for c in a):
            raise ValueError(f'bad token {a!r}')
    body = f'NL1 {seq} {kind}' + ''.join(' ' + a for a in args)
    line = f'${body}*{binascii.crc_hqx(body.encode(), 0xFFFF):04X}'
    if len(line) > 320:
        raise ValueError('line longer than NL_LINE_MAX (320)')
    return line + '\n'


def main(argv):
    seq = 1
    if len(argv) >= 2 and argv[0] == '--seq':
        seq = int(argv[1])
        argv = argv[2:]
    if not argv:
        print(__doc__, file=sys.stderr)
        return 2
    sys.stdout.write(frame(argv[0], argv[1:], seq))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
