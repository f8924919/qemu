/*
 * Helpers for putting a 970 to sleep on purpose.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define SPR_DEC		22
#define SPR_SRR0	26
#define SPR_SRR1	27

#define MSR_EE		0x8000
/* MSR[POW] is 0x40000, so it goes in with oris. */
#define MSR_POW_H	0x4

#define DEC_DELAY_H	0x2		/* 0x20000 ticks: lis with this */

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
 * void set_pow_with_ee_off(void);
 *
 * Mac OS X does this in machine_idle() while it re-arms the decrementer.
 * A core that goes to sleep here can never be woken, so the test simply
 * times out if the implementation gets it wrong.
 */
	.globl set_pow_with_ee_off
set_pow_with_ee_off:
	mfmsr	5
	oris	6, 5, MSR_POW_H
	mtmsrd	6
	isync
	mtmsrd	5
	isync
	blr

/*
 * unsigned long nap_until_decrementer(void);
 */
	.globl nap_until_decrementer
nap_until_decrementer:
	mflr	0
	LOADSYM	10, nap_lr
	std	0, 0(10)

	li	9, 0x900		/* decrementer */
	bl	install_nap_stub

	LOADSYM	10, nap_area
	li	11, 0
	std	11, 0(10)		/* spins = 0 */

	lis	7, DEC_DELAY_H
	mtspr	SPR_DEC, 7

	mfmsr	5
	oris	6, 5, MSR_POW_H
	ori	6, 6, MSR_EE
	mtmsrd	6
	isync
	/*
	 * A napping core stops right here and does not execute this loop at
	 * all.  One that ignored POW spins until the decrementer fires.
	 */
1:	ld	11, 0(10)
	addi	11, 11, 1
	std	11, 0(10)
	b	1b

	.globl nap_resume
nap_resume:				/* the handler rfids back to here */
	LOADSYM	10, nap_area
	ld	3, 0(10)
	LOADSYM	10, nap_lr
	ld	0, 0(10)
	mtlr	0
	blr

/* r9 = vector address to copy the stub onto */
install_nap_stub:
	mflr	8
	LOADSYM	5, nap_stub
	li	7, (nap_stub_end - nap_stub) / 4
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

/* Entered with relocation already off and EE cleared by the hardware. */
nap_stub:
	LOADSYM	12, nap_area
	mfspr	11, SPR_SRR0
	std	11, 8(12)
	LOADSYM	11, nap_resume
	mtspr	SPR_SRR0, 11
	mfmsr	11
	mtspr	SPR_SRR1, 11
	rfid
nap_stub_end:

	.section .data
	.balign 8
nap_area:
nap_spins:
	.quad	0
	.globl nap_srr0
nap_srr0:
	.quad	0
nap_lr:
	.quad	0
