/*
 * Enabled floating-point exceptions on the 970: where the program
 * interrupt points, and whether the target FPR was written before it.
 *
 * The ISA splits the enabled exceptions in two.  Invalid operation (VE) and
 * zero divide (ZE) leave the target FPR and FPRF alone; overflow (OE),
 * underflow (UE) and inexact (XE) write them first and then interrupt.
 * QEMU implements the second group by raising the interrupt from a helper
 * that runs after the result has been stored, so this test pins that order
 * down before anything moves the helpers around.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <minilib.h>
#include "fpexc.h"

#define MSR_FE0         0x800
#define MSR_FE1         0x100
#define SRR1_FP         0x00100000      /* program interrupt: FP enabled */

#define FX      (1u << 31)
#define FEX     (1u << 30)
#define VX      (1u << 29)
#define OX      (1u << 28)
#define UX      (1u << 27)
#define ZX      (1u << 26)
#define XX      (1u << 25)
#define VXVC    (1u << 19)
#define FPRF_SHIFT 12
#define FPRF    (0x1fu << FPRF_SHIFT)
#define FPCC    (0xfu << FPRF_SHIFT)
#define VE      (1u << 7)
#define OE      (1u << 6)
#define UE      (1u << 5)
#define ZE      (1u << 4)
#define XE      (1u << 3)

#define C_NNORM 0x08
#define C_PNORM 0x04

#define ONE     0x3ff0000000000000ull
#define THREE   0x4008000000000000ull
#define INF     0x7ff0000000000000ull
#define QNAN    0x7ff8000000000000ull
#define HUGE    0x7e70000000000000ull   /* 2^1000 */
#define TINY    0x0170000000000000ull   /* 2^-1000 */
#define MARK    0x7ff7deadbeef0001ull

/* A-form and X-form with frt = 1, fra = 2, frb = 3, frc = 4 */
#define A_AB(op, xo)    ((op) << 26 | 1 << 21 | 2 << 16 | 3 << 11 | (xo) << 1)
#define A_AC(op, xo)    ((op) << 26 | 1 << 21 | 2 << 16 | 4 << 6 | (xo) << 1)
#define A_ABC(op, xo)   ((op) << 26 | 1 << 21 | 2 << 16 | 3 << 11 | \
                         4 << 6 | (xo) << 1)
#define X_B(op, xo)     ((op) << 26 | 1 << 21 | 3 << 11 | (xo) << 1)
#define X_CMP(op, xo)   ((op) << 26 | 6 << 23 | 2 << 16 | 3 << 11 | (xo) << 1)

#define FADD    A_AB(63, 21)
#define FSUB    A_AB(63, 20)
#define FDIV    A_AB(63, 18)
#define FMUL    A_AC(63, 25)
#define FMADDS  A_ABC(59, 29)
#define FRSP    X_B(63, 12)
#define FCTIW   X_B(63, 14)
#define FCFID   X_B(63, 846)
#define FCMPO   X_CMP(63, 32)

static int ok = 1;

static void check(const char *name, const char *what, unsigned long got,
                  unsigned long want)
{
    if (got != want) {
        ml_printf("FAIL: fpexc %s %s 0x%lx expected 0x%lx\n", name, what,
                  (unsigned long)got, (unsigned long)want);
        ok = 0;
    }
}

/* No struct copies: there is no memset or memcpy to link against. */
static void run(struct fpx *x, unsigned int insn, unsigned long a,
                unsigned long b, unsigned long c, unsigned long fpscr_in)
{
    x->insn = insn;
    x->pad = 0;
    x->f2 = a;
    x->f3 = b;
    x->f4 = c;
    x->f1_in = MARK;
    /* a class the results below never have, so that a stale FPRF shows */
    x->fpscr_in = fpscr_in | C_NNORM << FPRF_SHIFT;
    x->msr_bits = MSR_FE0 | MSR_FE1;
    fp_exec(x);
}

/* The interrupt fired and points at the instruction. */
static void took(const char *name, const struct fpx *x)
{
    check(name, "vector", x->vector, 0x700);
    check(name, "srr0", x->srr0, x->insn_addr);
    check(name, "srr1 FP", x->srr1 & SRR1_FP, SRR1_FP);
}

/* OE / UE / XE: result and FPRF written first, then the interrupt. */
static void written(const char *name, unsigned int insn, unsigned long a,
                    unsigned long b, unsigned long c, unsigned long enable,
                    unsigned long want, unsigned long flags)
{
    struct fpx x;

    run(&x, insn, a, b, c, enable);
    took(name, &x);
    check(name, "f1", x.f1_out, want);
    check(name, "FPRF", x.fpscr_out & FPRF, C_PNORM << FPRF_SHIFT);
    check(name, "flags", x.fpscr_out & (FX | FEX | flags), FX | FEX | flags);
}

/* VE / ZE: neither the target nor FPRF is touched. */
static void untouched(const char *name, unsigned int insn,
                      unsigned long a, unsigned long b, unsigned long c,
                      unsigned long enable)
{
    struct fpx x;

    run(&x, insn, a, b, c, enable);
    took(name, &x);
    check(name, "f1", x.f1_out, MARK);
    check(name, "FEX", x.fpscr_out & (FX | FEX), FX | FEX);
}

int main(void)
{
    struct fpx x;

    /* nothing enabled goes off: no interrupt */
    run(&x, FADD, ONE, ONE, 0, VE | OE | UE | ZE | XE);
    check("fadd", "vector", x.vector, 0);
    check("fadd", "f1", x.f1_out, 0x4000000000000000ull);

    /*
     * Overflow and underflow with the exception enabled deliver the result
     * with the exponent adjusted by 1536 (2^2000 -> 2^464, 2^-2000 ->
     * 2^-464).
     */
    written("fmul-oe", FMUL, HUGE, 0, HUGE, OE, 0x5cf0000000000000ull, OX);
    written("fmul-ue", FMUL, TINY, 0, TINY, UE, 0x22f0000000000000ull, UX);
    written("fdiv-xe", FDIV, ONE, THREE, 0, XE, 0x3fd5555555555555ull, XX);
    written("fmadds-xe", FMADDS, ONE, 0x3fd5555555555555ull, THREE, XE,
            0x400aaaaaa0000000ull, XX);
    written("frsp-xe", FRSP, 0, 0x3fd5555555555555ull, 0, XE,
            0x3fd5555560000000ull, XX);

    untouched("fsub-ve", FSUB, INF, INF, 0, VE);
    untouched("fdiv-ze", FDIV, ONE, 0, 0, ZE);
    untouched("fctiw-ve", FCTIW, 0, QNAN, 0, VE);

    /* fcmpo on a QNaN: CR6 and FPCC say unordered before the interrupt */
    run(&x, FCMPO, QNAN, ONE, 0, VE);
    took("fcmpo-ve", &x);
    check("fcmpo-ve", "CR6", (x.cr_out >> 4) & 0xf, 0x1);
    check("fcmpo-ve", "FPCC", x.fpscr_out & FPCC, 0x1 << FPRF_SHIFT);
    check("fcmpo-ve", "VXVC", x.fpscr_out & (VX | VXVC), VX | VXVC);

    /*
     * fcfid raises an enabled inexact exception before it writes the target
     * or FPRF.  The ISA says otherwise, but this is what QEMU does today and
     * #382 keeps it.
     */
    run(&x, FCFID, 0, 0x0020000000000001ull, 0, XE);
    took("fcfid-xe", &x);
    check("fcfid-xe", "f1", x.f1_out, MARK);
    check("fcfid-xe", "FPRF", x.fpscr_out & FPRF, C_NNORM << FPRF_SHIFT);

    if (ok) {
        ml_printf("PASS: fpexc\n");
    }
    return 0;
}
