/*
 * qemu-g5 #387 (measurement only): run one instruction in problem state.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define SPR_SRR0	26
#define SPR_SRR1	27
#define MSR_FP		0x2000
#define MSR_PR		0x4000

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
 * void g5u_run(unsigned long ea);
 *
 * Enter the code at ea with MSR[PR] and MSR[FP] set; it answers with sc.
 * On the 970, PR=1 forces IR, DR and EE on, so ea must be mapped (the
 * caller sets up the page table).  The FPRs and the FPSCR are whatever
 * the caller left.
 */
	.globl g5u_run
g5u_run:
	mflr	0
	LOADSYM	10, g5u_save
	std	0, 0(10)
	mfmsr	5
	std	5, 8(10)

	/* the sc stub onto 0xc00 */
	LOADSYM	5, g5u_sc_stub
	li	9, 0xc00
	li	7, (g5u_sc_stub_end - g5u_sc_stub) / 4
	mtctr	7
1:	lwz	6, 0(5)
	stw	6, 0(9)
	dcbst	0, 9
	sync
	icbi	0, 9
	addi	5, 5, 4
	addi	9, 9, 4
	bdnz	1b

	mr	5, 3
	ld	6, 8(10)
	ori	6, 6, MSR_FP
	ori	6, 6, MSR_PR
	mtspr	SPR_SRR0, 5
	mtspr	SPR_SRR1, 6
	rfid

g5u_sc:
	LOADSYM	11, g5u_save
	ld	12, 8(11)
	ori	12, 12, MSR_FP
	mtspr	SPR_SRR1, 12
	LOADSYM	12, g5u_back
	mtspr	SPR_SRR0, 12
	rfid

g5u_back:
	LOADSYM	10, g5u_save
	ld	0, 0(10)
	mtlr	0
	blr

g5u_sc_stub:
	LOADSYM	11, g5u_sc
	mtctr	11
	bctr
g5u_sc_stub_end:

	.section .data
	.balign 8
g5u_save:
	.quad	0, 0
