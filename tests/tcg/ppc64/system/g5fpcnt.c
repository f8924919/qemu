/*
 * qemu-g5 #387 (measurement only): drive the G5FP counters.
 *
 * Every instruction below is run once, and the counts it should add are
 * printed as EXPECT lines.  tests/fixtures/fp-387/check-system.py (in the
 * qemu-g5 repository) compares them with the G5FP lines QEMU prints at exit
 * with G5FP_DUMP_AT_EXIT set: the op and tr counts must match exactly, so
 * every FP instruction this image runs, including the ones fp_exec() runs
 * around the instruction under test, is accounted for here.
 *
 * The expected tiers are worked out by hand from the inputs, not by the
 * predicate in target/ppc/g5-fpelig.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <minilib.h>
#include "fpexc.h"

void g5u_run(unsigned long ea);

/*
 * Problem state: on the 970, MSR[PR] forces IR, DR and EE on, so the code
 * run there is mapped.  One hash table of 256 KiB (HTABSIZE = 0), one SLB
 * entry and one PTE: EA_U -> PAGE_U.  RAM starts out zeroed, which makes
 * every other PTE invalid.
 */
#define SPR_SDR1 25
#define HTAB     0x00400000UL
#define PAGE_U   0x01000000UL
#define EA_U     0x40000000UL
#define VSID_U   0x100UL

#define MSR_FE  0x900                   /* FE0 | FE1 */
#define SRR1_ILLEGAL 0x00080000

/* FPSCR, low word */
#define FX      (1u << 31)
#define XX      (1u << 25)
#define FI      (1u << 17)
#define VE      (1u << 7)
#define OE      (1u << 6)
#define UE      (1u << 5)
#define ZE      (1u << 4)
#define XE      (1u << 3)
#define RN_ZERO 1

#define ONE     0x3ff0000000000000ull
#define TWO     0x4000000000000000ull
#define THREE   0x4008000000000000ull
#define FOUR    0x4010000000000000ull
#define THIRD   0x3fd5555555555555ull   /* not a single */
#define MONE    0xbff0000000000000ull
#define MZERO   0x8000000000000000ull
#define INF     0x7ff0000000000000ull
#define DMIN    0x0010000000000000ull   /* DBL_MIN */
#define DMIN15  0x0018000000000000ull   /* 1.5 * DBL_MIN */
#define MDMIN   0x8010000000000000ull
#define DDEN    0x0008000000000000ull   /* double denormal */
#define TINY    0x0170000000000000ull   /* 2^-1000 */
#define P100    0x39b0000000000000ull   /* 2^-100 */
#define P30     0x3e10000000000000ull   /* 2^-30 */
#define P70     0x3b90000000000000ull   /* 2^-70, a single */

#define A_AB(op, xo)    ((op) << 26 | 1 << 21 | 2 << 16 | 3 << 11 | (xo) << 1)
#define A_AC(op, xo)    ((op) << 26 | 1 << 21 | 2 << 16 | 4 << 6 | (xo) << 1)
#define A_ABC(op, xo)   ((op) << 26 | 1 << 21 | 2 << 16 | 3 << 11 | \
                         4 << 6 | (xo) << 1)
#define A_B(op, xo)     ((op) << 26 | 1 << 21 | 3 << 11 | (xo) << 1)
#define X_B(op, xo)     ((op) << 26 | 1 << 21 | 3 << 11 | (xo) << 1)
#define X_CMP(op, xo)   ((op) << 26 | 6 << 23 | 2 << 16 | 3 << 11 | (xo) << 1)

#define MFFS            (63 << 26 | 1 << 21 | 583 << 1)
#define MCRFS(s)        (63 << 26 | 7 << 23 | (s) << 18 | 64 << 1)
#define MTFSB0(bt)      (63 << 26 | (bt) << 21 | 70 << 1)
#define MTFSB1(bt)      (63 << 26 | (bt) << 21 | 38 << 1)
#define MTFSF(flm)      (63 << 26 | (flm) << 17 | 3 << 11 | 711 << 1)
#define MTFSFI(bf, u)   (63 << 26 | (bf) << 23 | (u) << 12 | 134 << 1)
#define RC              1

static int ok = 1;
static unsigned long infra;    /* fp_exec() calls */

static void expect_op(const char *h, const char *t, int x,
                      unsigned long fpscr)
{
    ml_printf("EXPECT op h=%s t=%s x=%d o=%d e0=%d e1=%d n=1\n", h, t, x,
              !!(fpscr & (OE | UE)),
              !(fpscr & (VE | OE | UE | ZE | XE)), !!(fpscr & XX));
}

static void expect_tr(const char *k, int a, int pr)
{
    ml_printf("EXPECT tr k=%s a=%d pr=%d n=1\n", k, a, pr);
}

/* Problem state: the FPSCR values at the read are known exactly. */
static void expect_tru(const char *k, int a, unsigned long fpscr)
{
    ml_printf("EXPECT tr k=%s a=%d pr=1 n=1 fi0=%d xx0=%d fx0=%d\n", k, a,
              !(fpscr & FI), !(fpscr & XX), !(fpscr & FX));
}

static void run(struct fpx *x, unsigned int insn, unsigned long a,
                unsigned long b, unsigned long c, unsigned long fpscr,
                unsigned long msr)
{
    x->insn = insn;
    x->pad = 0;
    x->f2 = a;
    x->f3 = b;
    x->f4 = c;
    x->f1_in = 0;
    x->fpscr_in = fpscr;
    x->msr_bits = msr;
    fp_exec(x);
    infra++;
}

/* One arithmetic instruction, no interrupt expected. */
static void op(const char *h, unsigned int insn, unsigned long a,
               unsigned long b, unsigned long c, unsigned long fpscr,
               const char *t, int xs)
{
    struct fpx x;

    run(&x, insn, a, b, c, fpscr, 0);
    if (x.vector) {
        ml_printf("FAIL: g5fpcnt %s interrupted\n", h);
        ok = 0;
    }
    expect_op(h, t, xs, fpscr);
}

/* Not on the 970: an illegal instruction, nothing counted. */
static void illegal(const char *name, unsigned int insn)
{
    struct fpx x;

    run(&x, insn, ONE, ONE, ONE, 0, 0);
    if (x.vector != 0x700 || !(x.srr1 & SRR1_ILLEGAL)) {
        ml_printf("FAIL: g5fpcnt %s is not illegal (vector 0x%lx srr1 "
                  "0x%lx)\n", name, x.vector, x.srr1);
        ok = 0;
    }
}

/* MSR[FP] stays on from here: g5u_run() comes back with it set as well. */
static void set_fpscr(unsigned long v)
{
    unsigned long msr;

    asm volatile("mfmsr %0" : "=r"(msr));
    msr |= 0x2000;
    asm volatile("mtmsrd %0; isync" : : "r"(msr) : "memory");
    asm volatile("std %0, -8(1)\n\tlfd 0, -8(1)\n\tmtfsf 0xff, 0"
                 : : "r"(v) : "fr0", "memory");
    expect_tr("MTFSF", 0xff, 0);
}

/* stores the MMU or the code reads behind the compiler's back */
static void wr64(unsigned long addr, unsigned long val)
{
    *(volatile unsigned long *)addr = val;
}

static void wr32(unsigned long addr, unsigned int val)
{
    *(volatile unsigned int *)addr = val;
}

static void map_user(void)
{
    /* the PTEG of VSID_U ^ page 0; AVPN 0 */
    unsigned long pte = HTAB + (VSID_U & 0x7ff) * 128;
    unsigned long rb = (EA_U & 0xfffffffff0000000UL) | 0x08000000UL;

    wr64(pte + 8, PAGE_U | 0x180);      /* R and C already set, RW */
    wr64(pte, VSID_U << 12 | 1);
    asm volatile("ptesync; mtspr %0,%1; isync" : : "i"(SPR_SDR1), "r"(HTAB)
                 : "memory");
    asm volatile("slbia; slbmte %0,%1; isync" : : "r"(VSID_U << 12), "r"(rb)
                 : "memory");
}

static void user(const char *k, int a, unsigned int insn,
                 unsigned long fpscr)
{
    wr32(PAGE_U, insn);
    wr32(PAGE_U + 4, 0x44000002);       /* sc */
    asm volatile("dcbst 0,%0; sync; icbi 0,%0; isync" : : "r"(PAGE_U)
                 : "memory");
    /* EE comes on with PR: keep the decrementer away */
    asm volatile("mtdec %0" : : "r"(0x7fffffffUL));
    set_fpscr(fpscr);
    g5u_run(EA_U);
    expect_tru(k, a, fpscr);
}

int main(void)
{
    struct fpx x;

    /* double, path: the tiers */
    op("FADD", A_AB(63, 21), ONE, ONE, 0, 0, "ok", 0);
    op("FADD", A_AB(63, 21), ONE, ONE, 0, RN_ZERO, "rn", 0);
    op("FADD", A_AB(63, 21), DDEN, ONE, 0, 0, "pre", 0);
    op("FADD", A_AB(63, 21), DMIN15, MDMIN, 0, 0, "post", 0);
    /* x - x is 0: tiny, and not both inputs 0 */
    op("FADD", A_AB(63, 21), ONE, MONE, 0, 0, "post", 0);
    /* the FPSCR bits */
    op("FADD", A_AB(63, 21), ONE, ONE, 0, OE, "ok", 0);
    op("FADD", A_AB(63, 21), ONE, ONE, 0, UE, "ok", 0);
    op("FADD", A_AB(63, 21), ONE, ONE, 0, VE, "ok", 0);
    op("FADD", A_AB(63, 21), ONE, ONE, 0, XX | FX, "ok", 0);
    op("FSUB", A_AB(63, 20), THREE, ONE, 0, 0, "ok", 0);
    op("FMUL", A_AC(63, 25), ONE, 0, THREE, 0, "ok", 0);
    op("FMUL", A_AC(63, 25), TINY, 0, TINY, 0, "post", 0);
    op("FMUL", A_AC(63, 25), 0, 0, THREE, 0, "post", 0);
    op("FDIV", A_AB(63, 18), ONE, THREE, 0, 0, "ok", 0);
    op("FDIV", A_AB(63, 18), 0, THREE, 0, 0, "ok", 0);
    op("FDIV", A_AB(63, 18), ONE, DDEN, 0, 0, "pre", 0);
    op("FDIV", A_AB(63, 18), DMIN, FOUR, 0, 0, "post", 0);
    /* fma: frt = fra * frc + frb; softfloat's a, b, c = fra, frc, frb */
    op("FMADD", A_ABC(63, 29), ONE, ONE, ONE, 0, "ok", 0);
    op("FMADD", A_ABC(63, 29), 0, MONE, ONE, 0, "ok", 0);
    op("FMADD", A_ABC(63, 29), P100, 0, P30, 0, "post", 0);
    op("FMADD", A_ABC(63, 29), DDEN, ONE, ONE, 0, "pre", 0);
    op("FMSUB", A_ABC(63, 28), ONE, ONE, THREE, 0, "ok", 0);
    op("FNMADD", A_ABC(63, 31), ONE, ONE, THREE, 0, "ok", 0);
    op("FNMSUB", A_ABC(63, 30), ONE, ONE, THREE, 0, "ok", 0);
    op("FSQRT", A_B(63, 22), 0, FOUR, 0, 0, "ok", 0);
    op("FSQRT", A_B(63, 22), 0, MZERO, 0, 0, "pre", 0);
    op("FRSQRTE", A_B(63, 26), 0, FOUR, 0, 0, "ok", 0);
    op("FRSQRTE", A_B(63, 26), 0, 0, 0, 0, "pre", 0);
    op("FCFID", X_B(63, 846), 0, 3, 0, 0, "ok", 0);
    op("FCFID", X_B(63, 846), 0, 3, 0, RN_ZERO, "rn", 0);

    /* single: float32's conditions on exact inputs (x) */
    op("FADDS", A_AB(59, 21), ONE, ONE, 0, 0, "ok", 1);
    op("FADDS", A_AB(59, 21), THIRD, ONE, 0, 0, "na", 0);
    op("FADDS", A_AB(59, 21), ONE, THIRD, 0, 0, "na", 0);
    op("FADDS", A_AB(59, 21), ONE, ONE, 0, RN_ZERO, "rn", 1);
    op("FSUBS", A_AB(59, 20), THREE, ONE, 0, 0, "ok", 1);
    op("FMULS", A_AC(59, 25), ONE, 0, THREE, 0, "ok", 1);
    op("FMULS", A_AC(59, 25), P70, 0, P70, 0, "post", 1);
    op("FDIVS", A_AB(59, 18), ONE, THREE, 0, 0, "ok", 1);
    op("FMADDS", A_ABC(59, 29), ONE, ONE, ONE, 0, "ok", 1);
    op("FMSUBS", A_ABC(59, 28), ONE, ONE, THREE, 0, "ok", 1);
    op("FNMADDS", A_ABC(59, 31), ONE, ONE, THREE, 0, "ok", 1);
    op("FNMSUBS", A_ABC(59, 30), ONE, ONE, THREE, 0, "ok", 1);
    op("FSQRTS", A_B(59, 22), 0, FOUR, 0, 0, "ok", 1);
    op("FSQRTS", A_B(59, 22), 0, MONE, 0, 0, "pre", 1);
    op("FRES", A_B(59, 24), 0, THREE, 0, 0, "ok", 1);

    /* no path */
    op("FRSP", X_B(63, 12), 0, THIRD, 0, 0, "na", 0);
    op("FCTIW", X_B(63, 14), 0, THREE, 0, 0, "na", 0);
    op("FCTIWZ", X_B(63, 15), 0, THREE, 0, 0, "na", 0);
    op("FCTID", X_B(63, 814), 0, THREE, 0, 0, "na", 0);
    op("FCTIDZ", X_B(63, 815), 0, THREE, 0, 0, "na", 0);

    /* compare: hardfloat unless denormal or unordered */
    op("FCMPU", X_CMP(63, 0), ONE, THREE, 0, 0, "ok", 0);
    op("FCMPU", X_CMP(63, 0), DDEN, THREE, 0, 0, "pre", 0);
    op("FCMPU", X_CMP(63, 0), ONE, THREE, 0, RN_ZERO, "ok", 0);
    op("FCMPO", X_CMP(63, 0) | 32 << 1, ONE, THREE, 0, 0, "ok", 0);

    /* not on the 970 */
    illegal("fre", A_B(63, 24));
    illegal("frsqrtes", A_B(59, 26));
    illegal("frin", X_B(63, 392));
    illegal("friz", X_B(63, 424));
    illegal("frip", X_B(63, 456));
    illegal("frim", X_B(63, 488));
    illegal("fcfids", X_B(59, 846));
    illegal("fcfidu", X_B(63, 974));
    illegal("fcfidus", X_B(59, 974));
    illegal("fctiwu", X_B(63, 142));
    illegal("fctiwuz", X_B(63, 143));
    illegal("fctidu", X_B(63, 942));
    illegal("fctiduz", X_B(63, 943));

    /* readers and writers in supervisor state, through fp_exec() */
    run(&x, MFFS, 0, 0, 0, 0, 0);
    expect_tr("MFFS", 0, 0);
    run(&x, MFFS | RC, 0, 0, 0, 0, 0);
    expect_tr("MFFS", 0, 0);
    expect_tr("CR1", 0, 0);
    for (int s = 0; s < 8; s++) {
        run(&x, MCRFS(s), 0, 0, 0, 0, 0);
        expect_tr("MCRFS", s, 0);
    }
    run(&x, MTFSB0(6), 0, 0, 0, XX | FX, 0);       /* clears XX */
    expect_tr("MTFSB0", 31 - 6, 0);
    run(&x, MTFSB0(6) | RC, 0, 0, 0, 0, 0);
    expect_tr("MTFSB0", 31 - 6, 0);
    expect_tr("CR1MTFS", 10, 0);                    /* G5FP_TR_MTFSB0 */
    run(&x, MTFSB1(28), 0, 0, 0, 0, 0);             /* XE */
    expect_tr("MTFSB1", 31 - 28, 0);
    run(&x, MTFSF(0x01), 0, 0, 0, 0, 0);
    expect_tr("MTFSF", 0x01, 0);
    run(&x, MTFSFI(7, 0) | RC, 0, 0, 0, 0, 0);
    expect_tr("MTFSFI", 7, 0);
    expect_tr("CR1MTFS", 13, 0);                    /* G5FP_TR_MTFSFI */
    /* an arithmetic instruction with Rc=1 updates CR1 */
    op("FADD", A_AB(63, 21) | RC, ONE, ONE, 0, 0, "ok", 0);
    expect_tr("CR1", 0, 0);

    /* readers in problem state, with known FI, XX and FX */
    map_user();
    user("MFFS", 0, MFFS, FI);
    user("MFFS", 0, MFFS, XX | FX);
    user("MCRFS", 3, MCRFS(3), FI);
    user("MCRFS", 1, MCRFS(1), 0);
    user("MCRFS", 0, MCRFS(0), FX);
    set_fpscr(0);

    /* exceptions: enabled with FE on (delivered), and with FE off */
    run(&x, A_AB(63, 20), INF, INF, 0, VE, MSR_FE);     /* VXISI */
    if (x.vector != 0x700) {
        ml_printf("FAIL: g5fpcnt fsub-ve not delivered\n");
        ok = 0;
    }
    expect_op("FSUB", "pre", 0, VE);
    ml_printf("EXPECTGE excreq cause=6 fe=on n=1\n");
    ml_printf("EXPECTGE exc cause=6 out=deliver n=1\n");
    run(&x, A_AB(63, 18), ONE, 0, 0, ZE, 0);            /* ZX, FE off */
    expect_op("FDIV", "pre", 0, ZE);
    ml_printf("EXPECTGE excreq cause=3 fe=off n=1\n");
    /*
     * An enabled inexact exception with FE off is left pending in QEMU
     * (cs->exception_index).  powerpc_excp() only sees it, and drops it
     * because FE is off, when the vCPU next leaves its execution loop for
     * an exit request (a kick from the main loop, such as the stop of a
     * snapshot); an interrupt taken first discards it instead.  Nothing
     * the guest does here forces that, so out=ignore is not checked by
     * this test: the W1 runs show it.
     */
    run(&x, A_AB(63, 18), ONE, THREE, 0, XE, 0);
    expect_op("FDIV", "ok", 0, XE);
    ml_printf("EXPECTGE excreq cause=4 fe=off n=1\n");

    for (unsigned long i = 0; i < infra; i++) {
        expect_tr("MTFSF", 0xff, 0);
        expect_tr("MTFSF", 0xff, 0);
        expect_tr("MFFS", 0, 0);
    }

    if (ok) {
        ml_printf("PASS: g5fpcnt\n");
    }
    return 0;
}
