/*
 * Run one floating-point instruction with FP exceptions enabled, and catch
 * the program interrupt it may raise (see fpexc.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define SPR_SRR0	26
#define SPR_SRR1	27
#define MSR_FP		0x2000

/*
 * The image runs from ROM, so the instruction under test is put in RAM,
 * below where the linker script starts .rwdata.
 */
#define SLOT		0x00080000

	.macro	LOADSYM reg, sym
	lis	\reg, (\sym)@highest
	ori	\reg, \reg, (\sym)@higher
	rldicr	\reg, \reg, 32, 31
	oris	\reg, \reg, (\sym)@h
	ori	\reg, \reg, (\sym)@l
	.endm

	.section .text
	.balign 8

/* void fp_exec(struct fpx *x); */
	.globl fp_exec
fp_exec:
	mflr	0
	LOADSYM	10, fpx_save
	std	0, 0(10)		/* LR */
	std	3, 8(10)		/* x */

	li	9, 0x700		/* program */
	bl	install_fpx_stub
	LOADSYM	10, fpx_save
	ld	3, 8(10)

	/*
	 * The slot: three nops, so that the instruction does not start a
	 * translation block, the instruction, and a branch back through CTR.
	 */
	lis	5, SLOT@h
	ori	5, 5, SLOT@l
	lis	6, 0x6000		/* nop */
	stw	6, 0(5)
	stw	6, 4(5)
	stw	6, 8(5)
	lwz	4, 0(3)
	stw	4, 12(5)
	lis	6, 0x7d89		/* mtctr 12 */
	ori	6, 6, 0x03a6
	stw	6, 16(5)
	lis	6, 0x4e80		/* bctr */
	ori	6, 6, 0x0420
	stw	6, 20(5)
	dcbst	0, 5
	sync
	icbi	0, 5
	isync
	addi	6, 5, 12
	std	6, 104(3)

	li	0, 0
	std	0, 80(3)		/* vector = 0 until the handler runs */
	std	0, 88(3)
	std	0, 96(3)

	mfmsr	6
	std	6, 16(10)		/* MSR to go back to */
	ori	6, 6, MSR_FP
	mtmsrd	6
	isync

	lfd	0, 40(3)
	mtfsf	0xff, 0
	lfd	1, 32(3)
	lfd	2, 8(3)
	lfd	3, 16(3)
	lfd	4, 24(3)
	li	0, 0
	mtcrf	0xff, 0

	LOADSYM	12, fpx_resume
	lis	5, SLOT@h
	ori	5, 5, SLOT@l
	mtctr	5
	ld	7, 48(3)
	or	6, 6, 7
	mtmsrd	6
	isync
	bctr

	.globl fpx_resume
fpx_resume:				/* the handler rfids back to here */
	LOADSYM	10, fpx_save
	ld	7, 16(10)
	ori	7, 7, MSR_FP		/* FE0 and FE1 off, FP still on */
	mtmsrd	7
	isync
	ld	3, 8(10)
	stfd	1, 56(3)
	mffs	0
	stfd	0, 64(3)
	mfcr	0
	std	0, 72(3)

	li	0, 0
	std	0, 24(10)
	lfd	0, 24(10)
	mtfsf	0xff, 0
	ld	7, 16(10)
	mtmsrd	7
	isync

	ld	0, 0(10)
	mtlr	0
	blr

/* r9 = vector address to copy the stub onto */
install_fpx_stub:
	mflr	8
	LOADSYM	5, fpx_stub
	li	7, (fpx_stub_end - fpx_stub) / 4
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

/*
 * Entered with relocation off and the FP exceptions disabled.  r3 still
 * points at the struct fpx: an interrupt does not touch the GPRs, and the
 * slot does not use r11.
 */
fpx_stub:
	li	11, 0x700
	std	11, 80(3)
	mfspr	11, SPR_SRR0
	std	11, 88(3)
	mfspr	11, SPR_SRR1
	std	11, 96(3)
	LOADSYM	11, fpx_resume
	mtspr	SPR_SRR0, 11
	mfmsr	11
	mtspr	SPR_SRR1, 11
	rfid
fpx_stub_end:

	.section .data
	.balign 8
fpx_save:
	.quad	0		/* LR */
	.quad	0		/* x */
	.quad	0		/* MSR */
	.quad	0		/* scratch */
