INCLUDE cvi_board_memmap.ld
ENTRY(_start)
MEMORY { firmware (rwx) : ORIGIN = CVIMMAP_FREERTOS_ADDR, LENGTH = CVIMMAP_FREERTOS_SIZE }
SECTIONS {
 .text : { KEEP(*(.vectors)) *(.text*) } > firmware
 .rodata : { *(.rodata*) *(.srodata*) } > firmware
 .data : { . = ALIGN(64); _data = .; *(.data*) *(.sdata*) . = ALIGN(8); _edata = .; } > firmware
 _data_lma = LOADADDR(.data);
 .bss (NOLOAD) : { . = ALIGN(64); _bss = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(64); _ebss = .; } > firmware
 .stack (NOLOAD) : { . = ALIGN(64); . += 0x8000; _stack_top = .; } > firmware
 ASSERT(_stack_top <= ORIGIN(firmware) + LENGTH(firmware), "firmware exceeds reserved memory")
 /DISCARD/ : { *(.eh_frame*) *(.comment*) }
}
