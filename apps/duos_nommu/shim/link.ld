ENTRY(_start)
MEMORY { shim (rwx) : ORIGIN = 0x9e000000, LENGTH = 0x10000 }
SECTIONS {
 .text : { KEEP(*(.vectors)) *(.text*) } > shim
 .rodata : { *(.rodata*) *(.srodata*) } > shim
 .data : { . = ALIGN(64); _data = .; *(.data*) *(.sdata*) . = ALIGN(8); _edata = .; } > shim
 _data_lma = LOADADDR(.data);
 .bss (NOLOAD) : { . = ALIGN(64); _bss = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(64); _ebss = .; } > shim
 .stack (NOLOAD) : { . = ALIGN(64); . += 0x8000; _stack_top = .; } > shim
 ASSERT(_stack_top <= 0x9e010000, "shim overlaps preserved boot transfer structure")
 /DISCARD/ : { *(.eh_frame*) *(.comment*) }
}
