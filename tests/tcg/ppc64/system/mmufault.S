/*
 * Take a data storage fault on purpose and report what the CPU recorded.
 *
 * Only data relocation is turned on, for the duration of a single access:
 * instruction fetch stays real, so no page table has to describe the test
 * itself and the handler can run and store its results without any mapping
 * being in place.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define SPR_DSISR	18
#define SPR_DAR		19
#define SPR_SRR0	26
#define SPR_SRR1	27

#define MSR_DR		0x10

	.macro	LOADSYM reg, sym
	lis	\reg, (\sym)@highest
	ori	\reg, \reg, (\sym)@higher
	rldicr	\reg, \reg, 32, 31
	oris	\reg, \reg, (\sym)@h
	ori	\reg, \reg, (\sym)@l
	.endm

	.section .text

/*
 * unsigned long take_data_fault(unsigned long ea, unsigned long seed);
 *
 * Seeds DSISR with `seed`, reads from `ea` with data relocation enabled and
 * returns the vector that fired (0 if the access simply succeeded).  What
 * the handler saw in DSISR and DAR is left in fault_dsisr and fault_dar.
 */
	.globl take_data_fault
take_data_fault:
	mflr	0
	LOADSYM	10, fault_lr
	std	0, 0(10)

	li	9, 0x300		/* data storage */
	bl	install_stub
	li	9, 0x380		/* data segment */
	bl	install_stub

	mtspr	SPR_DSISR, 4
	li	0, 0
	mtspr	SPR_DAR, 0

	mfmsr	5
	ori	6, 5, MSR_DR
	mtmsrd	6
	isync
	lwz	7, 0(3)			/* this is meant to fault */
	mtmsrd	5
	isync
	li	3, 0			/* it did not */
	b	9f

	.globl fault_resume
fault_resume:				/* the handler rfids back to here */
	LOADSYM	10, fault_area
	ld	3, 0(10)
9:	LOADSYM	10, fault_lr
	ld	0, 0(10)
	mtlr	0
	blr

/* r9 = vector address to copy the stub onto */
install_stub:
	mflr	8
	LOADSYM	5, fault_stub
	li	7, (fault_stub_end - fault_stub) / 4
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
 * Copied to both vectors, so it works out which one it is running on
 * instead of being built twice.  Entered with relocation already off.
 */
fault_stub:
	bl	2f
2:	mflr	11
	subi	11, 11, 4		/* the vector we came in on */
	LOADSYM	12, fault_area
	std	11, 0(12)
	mfspr	11, SPR_DSISR
	std	11, 8(12)
	mfspr	11, SPR_DAR
	std	11, 16(12)
	LOADSYM	11, fault_resume
	mtspr	SPR_SRR0, 11
	mfmsr	11
	mtspr	SPR_SRR1, 11
	rfid
fault_stub_end:

	.section .data
	.balign 8
fault_area:
	.globl fault_vector
fault_vector:
	.quad	0
	.globl fault_dsisr
fault_dsisr:
	.quad	0
	.globl fault_dar
fault_dar:
	.quad	0
fault_lr:
	.quad	0
