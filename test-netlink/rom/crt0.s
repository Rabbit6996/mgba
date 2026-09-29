	.section .text.start, "ax"
	.arm
	.global _start
_start:
	b rom_start
	.fill 188, 1, 0
rom_start:
	mov r0, #0x12
	msr cpsr_c, r0
	ldr sp, =0x03007FA0
	mov r0, #0x1F
	msr cpsr_c, r0
	ldr sp, =0x03007F00
	ldr r0, =main
	mov lr, pc
	bx r0
1:	b 1b
	.pool
