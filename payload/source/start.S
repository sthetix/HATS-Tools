/*
 * Copyright (c) 2018 naehrwert
 */

.section .text._start
.arm

.extern memset
.type memset, %function

.extern ipl_main
.type ipl_main, %function

.globl _start
.type _start, %function
_start:
	ADR R0, _start
	LDR R1, =__ipl_start
	CMP R0, R1
	BEQ _real_start

	ADR R2, _reloc_ipl
	LDR R3, =0x4003FF00
	MOV R4, #(_real_start - _reloc_ipl)
_copy_loop:
	LDMIA R2!, {R5}
	STMIA R3!, {R5}
	SUBS R4, #4
	BNE _copy_loop

	LDR R2, =__ipl_end
	SUB R2, R2, R1
	LDR R3, =_real_start
	LDR R4, =0x4003FF00
	BX R4

_reloc_ipl:
	LDMIA R0!, {R4-R7}
	STMIA R1!, {R4-R7}
	SUBS R2, #0x10
	BNE _reloc_ipl
	BX R3

_real_start:
	LDR SP, =0x4003FF00
	LDR R0, =__bss_start
	EOR R1, R1, R1
	LDR R2, =__bss_end
	SUB R2, R2, R0
	BL memset
	BL ipl_main
	B .

.globl pivot_stack
.type pivot_stack, %function
pivot_stack:
	MOV SP, R0
	BX LR
