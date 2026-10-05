/*
 * Helpers for the jump cache test (jmpcache.c).
 *
 * The test jumps to an effective address that is translated with IR=1, and
 * the code at that address answers with sc.  Everything here runs with the
 * MMU off except the code being tested: jc_run() enters it with rfid and the
 * sc handler brings control back, so that the caller sees a plain function
 * call that returns the mark the tested code left in r3.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define SPR_SRR0	26
#define SPR_SRR1	27

#define MSR_IR		0x20

	.macro	LOADSYM reg, sym
	lis	\reg, (\sym)@highest
	ori	\reg, \reg, (\sym)@higher
	rldicr	\reg, \reg, 32, 31
	oris	\reg, \reg, (\sym)@h
	ori	\reg, \reg, (\sym)@l
	.endm

	.section .text

/*
 * unsigned long jc_run(unsigned long entry, unsigned long ea,
 *                      unsigned long msr_or);
 *
 * Leaves for entry with instruction relocation on and msr_or added to MSR,
 * with ea in r4 (the trampoline page uses it as the bctr target; a direct
 * entry ignores it).  Returns what the code that was reached put in r3
 * before its sc.
 */
	.globl jc_run
jc_run:
	mflr	0
	LOADSYM	10, jc_save
	std	0, 0(10)		/* return address */
	mr	7, 5
	mfmsr	5
	std	5, 8(10)		/* MSR to come back with: real mode */
	ori	6, 5, MSR_IR
	or	6, 6, 7
	std	6, 16(10)		/* MSR the tested code runs with */
	mtspr	SPR_SRR0, 3
	mtspr	SPR_SRR1, 6
	rfid

/* 0xc00: the tested code is done, r3 is its mark */
jc_sc:
	LOADSYM	11, jc_save
	ld	12, 8(11)
	mtspr	SPR_SRR1, 12
	LOADSYM	12, jc_back
	mtspr	SPR_SRR0, 12
	rfid

jc_back:
	LOADSYM	10, jc_save
	ld	0, 0(10)
	mtlr	0
	blr

/*
 * 0x400: the instruction fetch missed in the page table.  Count it, note
 * where, install the PTE the test has prepared and retry the very same
 * instruction (SRR0 is left alone).  SRR1 is rebuilt rather than reused
 * because it carries the cause bits.
 */
jc_isi:
	LOADSYM	11, jc_isi_state
	ld	12, 0(11)
	addi	12, 12, 1
	std	12, 0(11)		/* count */
	mfspr	12, SPR_SRR0
	std	12, 8(11)		/* where it faulted */
	ld	10, 16(11)		/* address of the PTE */
	ld	12, 32(11)
	std	12, 8(10)		/* second doubleword first ... */
	eieio
	ld	12, 24(11)
	std	12, 0(10)		/* ... valid bit last */
	ptesync
	LOADSYM	11, jc_save
	ld	12, 16(11)
	mtspr	SPR_SRR1, 12
	rfid

/*
 * What gets copied onto the vectors.  They have to be position independent,
 * so each one only jumps to the real handler.
 */
	.globl jc_sc_stub
	.globl jc_sc_stub_end
jc_sc_stub:
	LOADSYM	11, jc_sc
	mtctr	11
	bctr
jc_sc_stub_end:

	.globl jc_isi_stub
	.globl jc_isi_stub_end
jc_isi_stub:
	LOADSYM	11, jc_isi
	mtctr	11
	bctr
jc_isi_stub_end:

	.section .data
	.balign 8
jc_save:
	.quad	0, 0, 0

	.globl jc_isi_state
jc_isi_state:
	.quad	0			/* 0: how many times jc_isi ran */
	.quad	0			/* 8: SRR0 of the last one */
	.quad	0			/* 16: address of the PTE to install */
	.quad	0			/* 24: its first doubleword */
	.quad	0			/* 32: its second doubleword */
