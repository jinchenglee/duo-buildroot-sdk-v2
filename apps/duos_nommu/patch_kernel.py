#!/usr/bin/env python3
"""Apply hardware-specific experiment changes only to the downloaded v6.18 tree."""
from pathlib import Path
import shutil, sys
root = Path(sys.argv[1])
source = Path(__file__).resolve().parent
marker = root / '.duos-nommu-patched'
if marker.exists():
    raise SystemExit('kernel already patched; use build.sh to refresh the isolated tree')
def replace(path, old, new):
    file = root / path
    text = file.read_text()
    if text.count(old) != 1:
        raise RuntimeError(f'{path}: expected one exact patch site, found {text.count(old)}')
    file.write_text(text.replace(old, new))
# This isolated configuration targets C906L only. Cover both architecture
# get_cycles and the driver's clocksource; merely adding a DT binding is insufficient.
replace('arch/riscv/include/asm/timex.h',
        'return readq_relaxed(clint_time_val);', 'return csr_read(CSR_TIME);')
replace('drivers/clocksource/timer-clint.c',
        '#define clint_get_cycles()\treadq_relaxed(clint_timer_val)',
        '#define clint_get_cycles()\tcsr_read(CSR_TIME)')
replace('drivers/clocksource/timer-clint.c',
        '\tcsr_set(CSR_IE, IE_TIE);\n\twriteq_relaxed(clint_get_cycles64() + delta, r);',
        '''\tu64 deadline = clint_get_cycles64() + delta;
	/* C906L compare registers support 32-bit accesses. Avoid a transient
	 * early deadline while replacing the two halves. */
	writel_relaxed(~0u, (u8 __iomem *)r + 4);
	writel_relaxed((u32)deadline, r);
	writel_relaxed((u32)(deadline >> 32), (u8 __iomem *)r + 4);
	csr_set(CSR_IE, IE_TIE);''')
with (root / 'drivers/clocksource/timer-clint.c').open('a') as out:
    out.write('\nTIMER_OF_DECLARE(duos_clint, "thead,c900-clint", clint_timer_init_dt);\n')
with (root / 'drivers/tty/serial/Makefile').open('a') as out:
    out.write('\nobj-$(CONFIG_DUOS_NOMMU_PROBE) += duos_nommu.o\n')
    out.write('obj-$(CONFIG_DUOS_NOMMU_SHELL) += duos_nommu_console.o\n')
with (root / 'drivers/tty/serial/Kconfig').open('a') as out:
    out.write('''
config DUOS_NOMMU_PROBE
	bool "Duo S isolated NOMMU kernel-only scheduler probe"
	depends on RISCV && RISCV_M_MODE && 64BIT && !MMU && !SMP
	select SERIAL_EARLYCON
	select SERIAL_CORE
	select SERIAL_CORE_CONSOLE

config DUOS_NOMMU_USER_PROBE
	bool "Run the isolated libc-free userspace init"
	depends on DUOS_NOMMU_PROBE

config DUOS_NOMMU_SHELL
	bool "Shared-memory userspace diagnostic shell"
	depends on DUOS_NOMMU_USER_PROBE
''')
shutil.copyfile(source / 'platform.c', root / 'drivers/tty/serial/duos_nommu.c')
shutil.copyfile(source / 'log.h', root / 'drivers/tty/serial/duos_nommu_log.h')
shutil.copyfile(source / 'console.c', root / 'drivers/tty/serial/duos_nommu_console.c')
shutil.copyfile(source / 'console.h', root / 'drivers/tty/serial/duos_nommu_console.h')
# Keep kernel PID 1 alive without claiming a successful userspace boot.
replace('init/main.c', '\tdo_sysctl_args();\n', '''\tdo_sysctl_args();
	if (IS_ENABLED(CONFIG_DUOS_NOMMU_PROBE) &&
	    !IS_ENABLED(CONFIG_DUOS_NOMMU_USER_PROBE)) {
		pr_info("duos-nommu: PID 1 remains a kernel task; userspace not attempted\\n");
		for (;;) msleep(1000);
	}
''')
replace('init/main.c', '\t\tprepare_namespace();',
        '\t\tif (!IS_ENABLED(CONFIG_DUOS_NOMMU_PROBE)) prepare_namespace();')
# Capture traps during setup_vm before the normal exception handler exists.
replace('arch/riscv/kernel/head.S', '.Lsecondary_park:\n', '.Lsecondary_park:\n#ifdef CONFIG_DUOS_NOMMU_PROBE\n\tli t0, 0x9fff0000\n\tcsrr t1, CSR_CAUSE\n\tsd t1, 24(t0)\n\tcsrr t1, CSR_EPC\n\tsd t1, 32(t0)\n\tcsrr t1, CSR_TVAL\n\tsd t1, 40(t0)\n\tli t1, 99\n\tsw t1, 48(t0)\n\tmv a0, t0\n\t.long 0x0295000b\n\t.long 0x0190000b\n#endif\n')
marker.write_text('isolated v6.18 C906L kernel-only probe\n')
