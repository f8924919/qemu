/*
 * Helper for running one floating-point instruction with FP exceptions
 * enabled (see fpexc.S).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PPC64_FPEXC_H
#define PPC64_FPEXC_H


/* Offsets are hard-coded in fpexc.S: keep the layout in step with it. */
struct fpx {
    unsigned int insn;          /*   0: the instruction, frt = f1 */
    unsigned int pad;
    unsigned long f2, f3, f4;    /*   8: operands */
    unsigned long f1_in;         /*  32: f1 before */
    unsigned long fpscr_in;      /*  40 */
    unsigned long msr_bits;      /*  48: ORed into MSR along with MSR[FP] */
    unsigned long f1_out;        /*  56 */
    unsigned long fpscr_out;     /*  64 */
    unsigned long cr_out;        /*  72 */
    unsigned long vector;        /*  80: 0x700 if the program interrupt fired */
    unsigned long srr0;          /*  88 */
    unsigned long srr1;          /*  96 */
    unsigned long insn_addr;     /* 104: where the instruction was run */
};

/*
 * Run x->insn with f1..f4 and the FPSCR loaded from x, MSR[FP] and
 * x->msr_bits set, and three nops in front of it so that it does not start
 * a translation block.  A program interrupt resumes behind the instruction
 * with the FP exceptions disabled again; either way the outcome is stored
 * back into x.
 */
void fp_exec(struct fpx *x);

#endif /* PPC64_FPEXC_H */
