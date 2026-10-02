#!/usr/bin/env python3
"""Reject a loader-incompatible executable before it enters a boot bundle."""
import struct
import sys
from pathlib import Path
p = Path(sys.argv[1]); b = p.read_bytes()
assert b[:6] == b'\x7fELF\x02\x01', 'requires ELF64 little endian'
assert struct.unpack_from('<HH', b, 16) == (3, 243), 'requires RISC-V ET_DYN'
abi = sys.argv[2] if len(sys.argv) > 2 else 'lp64'
assert abi in ('lp64', 'lp64d')
assert struct.unpack_from('<I', b, 48)[0] & 6 == (4 if abi == 'lp64d' else 0), 'unexpected float ABI' 
off = struct.unpack_from('<Q', b, 32)[0]
size, count = struct.unpack_from('<HH', b, 54)
for i in range(count):
    kind, flags, pos, va, pa, filesz, memsz, align = struct.unpack_from('<IIQQQQQQ', b, off+i*size)
    assert kind != 3, 'PT_INTERP requires a dynamic loader'
    if kind == 2:
        for j in range(pos, pos+filesz, 16):
            tag, val = struct.unpack_from('<QQ', b, j)
            assert tag != 1, 'DT_NEEDED requires shared libraries'
            if tag == 0: break

shoff = struct.unpack_from('<Q', b, 40)[0]
shsize, shnum = struct.unpack_from('<HH', b, 58)
for i in range(shnum):
    name, kind, flags, va, pos, length, link, info, align, entsize = struct.unpack_from('<IIQQQQIIQQ', b, shoff+i*shsize)
    if kind == 4 and flags & 2:
        assert entsize == 24
        for at in range(pos, pos+length, 24):
            target, rinfo, addend = struct.unpack_from('<QQq', b, at)
            assert rinfo & 0xffffffff in (0, 3), 'unresolved static runtime relocation'
            if rinfo & 0xffffffff == 3:
                assert rinfo >> 32 == 0, 'RELATIVE relocation must not require a symbol'
print(f'{p.name}: static PIE, RV64 {abi}, {len(b)} bytes')
