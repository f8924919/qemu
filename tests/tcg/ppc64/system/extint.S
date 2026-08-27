/*
 * Helper for taking an external interrupt on purpose.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define SPR_SRR0	26
#define SPR_SRR1	27

#define MSR_EE		0x8000

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
 * unsigned long take_external_interrupt(void);
 *
 * Enables MSR[EE] with an interrupt already pending at the controller and
 * returns the vector that fired (0 if nothing did).  SRR0/SRR1 as the
 * handler saw them are left in ext_srr0 and ext_srr1: on a core that
 * delivers external interrupts through HSRR0/1 instead, SRR0 keeps whatever
 * it held before and the check in the caller notices.
 */
	.globl take_external_interrupt
take_external_interrupt:
	mflr	0
	LOADSYM	10, ext_lr
	std	0, 0(10)

	li	9, 0x500		/* external */
	bl	install_ext_stub

	LOADSYM	10, ext_area
	li	11, 0
	std	11, 0(10)		/* vector = 0 until the handler runs */

	/* Poison SRR0 so that a handler entered through HSRR0/1 shows it. */
	LOADSYM	11, 0xdeadbeef
	mtspr	SPR_SRR0, 11

	mfmsr	5
	ori	6, 5, MSR_EE
	mtmsrd	6
	isync
	/*
	 * Give the interrupt a moment to arrive.  It is already pending at the
	 * controller, so this is only here to bound the wait.
	 */
	li	7, 0x1000
	mtctr	7
1:	bdnz	1b
	mtmsrd	5
	isync

	.globl ext_resume
ext_resume:				/* the handler rfids back to here */
	LOADSYM	10, ext_area
	ld	3, 0(10)
	LOADSYM	10, ext_lr
	ld	0, 0(10)
	mtlr	0
	blr

/* r9 = vector address to copy the stub onto */
install_ext_stub:
	mflr	8
	LOADSYM	5, ext_stub
	li	7, (ext_stub_end - ext_stub) / 4
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
ext_stub:
	bl	2f
2:	mflr	11
	subi	11, 11, 4		/* the vector we came in on */
	LOADSYM	12, ext_area
	std	11, 0(12)
	mfspr	11, SPR_SRR0
	std	11, 8(12)
	mfspr	11, SPR_SRR1
	std	11, 16(12)
	LOADSYM	11, ext_resume
	mtspr	SPR_SRR0, 11
	mfmsr	11
	mtspr	SPR_SRR1, 11
	rfid
ext_stub_end:

	.section .data
	.balign 8
ext_area:
	.globl ext_vector
ext_vector:
	.quad	0
	.globl ext_srr0
ext_srr0:
	.quad	0
	.globl ext_srr1
ext_srr1:
	.quad	0
ext_lr:
	.quad	0
