#!/usr/bin/env python3
"""Seal single-module TLS relocations in this self-contained static executable.

musl rcrt1 relocates RELATIVE entries only. PIC SDK libstdc++ uses local-dynamic
TLS with a DTPMOD64 relocation. The sole static TLS module is module 1.
No symbol lookup, external module or dynamic TLS loading is permitted here.
"""
from pathlib import Path
import struct
import sys
p = Path(sys.argv[1]); b = bytearray(p.read_bytes())
assert b[:6] == b'\x7fELF\x02\x01'
assert struct.unpack_from('<HH', b, 16) == (3, 243)
phoff = struct.unpack_from('<Q', b, 32)[0]
phsize, phnum = struct.unpack_from('<HH', b, 54)
loads = []; tls = []
for i in range(phnum):
    kind, flags, off, va, pa, filesz, memsz, align = struct.unpack_from('<IIQQQQQQ', b, phoff+i*phsize)
    if kind == 1: loads.append((va, off, filesz))
    if kind == 7: tls.append((va, filesz, memsz))
def file_offset(va):
    for start, off, size in loads:
        if start <= va and va+8 <= start+size: return off+va-start
    raise ValueError(f'relocation target not file-backed: {va:x}')
shoff = struct.unpack_from('<Q', b, 40)[0]
shsize, shnum = struct.unpack_from('<HH', b, 58)
sealed = 0
for i in range(shnum):
    name, kind, flags, va, off, size, link, info, align, entsize = struct.unpack_from('<IIQQQQIIQQ', b, shoff+i*shsize)
    if kind != 4 or not flags & 2: continue  # allocated SHT_RELA only
    assert entsize == 24
    for at in range(off, off+size, entsize):
        target, rinfo, addend = struct.unpack_from('<QQq', b, at)
        rtype, symbol = rinfo & 0xffffffff, rinfo >> 32
        if rtype == 7:  # R_RISCV_TLS_DTPMOD64
            assert len(tls) == 1 and tls[0][2] > 0
            assert symbol == 0 and addend == 0, 'only local static module TLS supported'
            struct.pack_into('<Q', b, file_offset(target), 1)
            struct.pack_into('<Q', b, at+8, 0)  # R_RISCV_NONE: already resolved
            sealed += 1
        else:
            assert rtype in (0, 3), f'unsupported static PIE relocation {rtype}, symbol {symbol}'
            if rtype == 3: assert symbol == 0
p.write_bytes(b)
print(f'{p.name}: sealed {sealed} local TLS module relocation(s)')
