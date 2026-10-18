/*
 * Single stepping for the 64-bit bridge test (bridge.c, case (d)).
 *
 * br_trace_run() enters the code from br_l0 with MSR[SE] set; every
 * instruction then ends in the 0xd00 handler, which appends SRR0 (the next
 * instruction) to br_trace_state and returns.  The last instruction,
 * mtmsrd, clears SE again.  The stepped code uses neither CTR nor r11 and
 * r12, which the vector stub and the handler use.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define SPR_SRR0	26
#define SPR_SRR1	27
#define SPR_SPRG1	273
#define SPR_SPRG2	274

#define MSR_SE		0x400

	.macro	LOADSYM reg, sym
	lis	\reg, (\sym)@highest
	ori	\reg, \reg, (\sym)@higher
	rldicr	\reg, \reg, 32, 31
	oris	\reg, \reg, (\sym)@h
	ori	\reg, \reg, (\sym)@l
	.endm

	.section .text

/* void br_trace_run(unsigned long ea); */
	.globl	br_trace_run
br_trace_run:
	mfmsr	6
	ori	7, 6, MSR_SE
	LOADSYM 8, br_l0
	mtspr	SPR_SRR0, 8
	mtspr	SPR_SRR1, 7
	rfid
br_l0:	tlbie	3
	.globl	br_l1
br_l1:	sync
	.globl	br_l2
br_l2:	nop
	.globl	br_l3
br_l3:	nop
	.globl	br_l4
br_l4:	mtmsrd	6
	.globl	br_l5
br_l5:	blr

/* 0xd00: note SRR0 (16 at most) and go on with the next instruction */
br_trace_handler:
	mtspr	SPR_SPRG1, 12
	LOADSYM 11, br_trace_state
	ld	12, 0(11)
	addi	12, 12, 1
	cmpdi	12, 15
	bgt	1f
	std	12, 0(11)
	sldi	12, 12, 3
	add	11, 11, 12
	mfspr	12, SPR_SRR0
	std	12, 0(11)
1:	mfspr	12, SPR_SPRG1
	mfspr	11, SPR_SPRG2
	rfid

/* What gets copied onto 0xd00: position independent, r11 kept in SPRG2 */
	.globl	br_trace_stub
	.globl	br_trace_stub_end
br_trace_stub:
	mtspr	SPR_SPRG2, 11
	LOADSYM 11, br_trace_handler
	mtctr	11
	bctr
br_trace_stub_end:

	.section .data
	.balign 8
	.globl	br_trace_state
br_trace_state:
	.fill	16, 8, 0
