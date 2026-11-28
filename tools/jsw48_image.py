#!/usr/bin/env python3
"""
jsw48_image.py - write the 32768-byte code block (32768-65535) of a Jet Set
Willy 48K tape (TZX or TAP), the 48K input tools/z80c/z80c.py translates.

  python tools/jsw48_image.py "datafiles/Jet Set Willy.tzx" jsw48.bin

SPDX-License-Identifier: MIT
"""
import sys


def tzx_blocks(d):
    out, i = [], 10
    while i < len(d):
        t = d[i]; i += 1
        if t == 0x10:
            n = int.from_bytes(d[i + 2:i + 4], 'little'); out.append(d[i + 4:i + 4 + n]); i += 4 + n
        elif t == 0x11:
            n = int.from_bytes(d[i + 15:i + 18], 'little'); out.append(d[i + 18:i + 18 + n]); i += 18 + n
        elif t == 0x14:
            n = int.from_bytes(d[i + 7:i + 10], 'little'); out.append(d[i + 10:i + 10 + n]); i += 10 + n
        elif t == 0x12: i += 4
        elif t == 0x13: i += 1 + 2 * d[i]
        elif t == 0x20: i += 2
        elif t in (0x21, 0x30): i += 1 + d[i]
        elif t == 0x22: pass
        elif t == 0x32: i += 2 + int.from_bytes(d[i:i + 2], 'little')
        elif t == 0x35: i += 20 + int.from_bytes(d[i + 16:i + 20], 'little')
        elif t == 0x5A: i += 9
        else: sys.exit(f'unsupported TZX block 0x{t:02X} at {i - 1}')
    return out


def tap_blocks(d):
    out, i = [], 0
    while i + 2 <= len(d):
        n = int.from_bytes(d[i:i + 2], 'little'); out.append(d[i + 2:i + 2 + n]); i += 2 + n
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: jsw48_image.py <tape.tzx|tap> <out.bin>')
    d = open(sys.argv[1], 'rb').read()
    blocks = tzx_blocks(d) if d[:7] == b'ZXTape!' else tap_blocks(d)
    data = [b[1:-1] for b in blocks if b and b[0] == 0xFF and len(b) == 32770]
    if not data:
        sys.exit(f'{sys.argv[1]}: no 32768-byte code block')
    open(sys.argv[2], 'wb').write(data[0])


if __name__ == '__main__':
    main()
