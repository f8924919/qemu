/*
 * Helper for taking a program interrupt on purpose.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define SPR_SRR0	26
#define SPR_SRR1	27

	.macro	LOADSYM reg, sym
	lis	\reg, (\sym)@highest
	ori	\reg, \reg, (\sym)@higher
	rldicr	\reg, \reg, 32, 31
	oris	\reg, \reg, (\sym)@h
	ori	\reg, \reg, (\sym)@l
	.endm

	.section .text
	.balign 8

/*
 * unsigned long take_program_fault(void);
 *
 * Executes a word that no 970 implements and returns the vector that fired
 * (0 if nothing did).  SRR0 as the handler saw it is left in prog_srr0, so
 * the caller can check that the interrupt points at the offending word
 * rather than somewhere else.
 */
	.globl take_program_fault
take_program_fault:
	mflr	0
	LOADSYM	10, prog_lr
	std	0, 0(10)

	li	9, 0x700		/* program */
	bl	install_prog_stub

	LOADSYM	10, prog_area
	li	11, 0
	std	11, 0(10)		/* vector = 0 until the handler runs */

	LOADSYM	11, prog_insn
	LOADSYM	12, prog_expect
	std	11, 0(12)		/* remember where the bad word is */
prog_insn:
	.long	0x00000000		/* not an instruction on any 970 */

	.globl prog_resume
prog_resume:				/* the handler rfids back to here */
	LOADSYM	10, prog_area
	ld	3, 0(10)
	LOADSYM	10, prog_lr
	ld	0, 0(10)
	mtlr	0
	blr

/* r9 = vector address to copy the stub onto */
install_prog_stub:
	mflr	8
	LOADSYM	5, prog_stub
	li	7, (prog_stub_end - prog_stub) / 4
	mtctr	7
1:	lwz	6, 0(5)
	stw	6, 0(9)
	dcbst	0, 9
	sync
	icbi	0, 9
	addi	5, 5, 4
	addi	9, 9, 4
	bdnz	1b
	isync
	mtlr	8
	blr

/* Entered with relocation already off. */
prog_stub:
	bl	2f
2:	mflr	11
	subi	11, 11, 4		/* the vector we came in on */
	LOADSYM	12, prog_area
	std	11, 0(12)
	mfspr	11, SPR_SRR0
	std	11, 8(12)
	LOADSYM	11, prog_resume
	mtspr	SPR_SRR0, 11
	mfmsr	11
	mtspr	SPR_SRR1, 11
	rfid
prog_stub_end:

	.section .data
	.balign 8
prog_area:
	.globl prog_vector
prog_vector:
	.quad	0
	.globl prog_srr0
prog_srr0:
	.quad	0
	.globl prog_expect
prog_expect:
	.quad	0
prog_lr:
	.quad	0
