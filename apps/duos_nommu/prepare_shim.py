#!/usr/bin/env python3
"""Reuse vendor startup in a disposable generated file; leave FreeRTOS intact."""
from pathlib import Path
import sys
sdk, out = map(Path, sys.argv[1:])
text = (sdk / 'freertos/cvitek/arch/riscv64/src/start.S').read_text()
assert text.count('li x3, 0x880') == 1
assert text.count('// Continue primary hart') == 1
text = text.replace('li x3, 0x880', 'li x3, 0')
text = text.replace('// Continue primary hart', '''csrc mstatus, 8
 csrw mie, zero
 la t0, nommu_shim_trap
 csrw mtvec, t0
 // Continue primary hart''')
text += '''
.balign 4
nommu_shim_trap:
 csrw mie, zero
 csrc mstatus, 8
 li t0, 0x9fff0000
 li t1, 0x4e4f4d4d
 sw t1, 0(t0)
 li t1, 1
 sw t1, 4(t0)
 sw zero, 8(t0)
 sw zero, 12(t0)
 csrr t1, mcause
 sd t1, 24(t0)
 csrr t1, mepc
 sd t1, 32(t0)
 csrr t1, mtval
 sd t1, 40(t0)
 li t1, 99
 sw t1, 48(t0)
 mv a0, t0
 .long 0x0295000b
 .long 0x0190000b
1:
 wfi
 j 1b
'''
(out / 'start.S').write_text(text)
