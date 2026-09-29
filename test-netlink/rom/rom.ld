ENTRY(_start)
MEMORY { ROM (rx) : ORIGIN = 0x08000000, LENGTH = 32M }
SECTIONS {
	.text : { *(.text.start) *(.text*) *(.rodata*) } > ROM
	/DISCARD/ : { *(.data*) *(.bss*) *(.ARM.*) *(.comment) }
}
