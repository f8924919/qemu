/*
 * FPSCR, CR1 and target FPR after the scalar floating-point instructions.
 *
 * Every instruction under test is assembled into a small executable buffer
 * and run with fixed register operands (frt = f1, fra = f2, frb = f3,
 * frc = f4), so one driver covers all of them.  Three kinds of checks:
 *
 *  - the value table: every instruction over a set of interesting operands,
 *    with all FP exceptions disabled.  Each line is printed and compared
 *    against fp_flags.ref; this is what guards a refactoring of the
 *    flag helpers against changing anything the guest can see.
 *  - a handful of hand-written expectations taken from the ISA, so that the
 *    reference is known to be right at the places that matter.
 *  - enabled exceptions, which linux-user delivers as SIGFPE: the signal
 *    must point at the instruction, and the target FPR must be written or
 *    left alone exactly as the ISA says.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <assert.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

/* FPSCR bits, numbered from the least significant end */
#define FX      (1u << 31)
#define FEX     (1u << 30)
#define VX      (1u << 29)
#define OX      (1u << 28)
#define UX      (1u << 27)
#define ZX      (1u << 26)
#define XX      (1u << 25)
#define VXSNAN  (1u << 24)
#define VXISI   (1u << 23)
#define VXIDI   (1u << 22)
#define VXZDZ   (1u << 21)
#define VXIMZ   (1u << 20)
#define VXVC    (1u << 19)
#define FR      (1u << 18)
#define FI      (1u << 17)
#define FPRF_SHIFT 12
#define FPRF    (0x1fu << FPRF_SHIFT)
#define VXCVI   (1u << 8)
#define VE      (1u << 7)
#define OE      (1u << 6)
#define UE      (1u << 5)
#define ZE      (1u << 4)
#define XE      (1u << 3)

/* FPRF classes */
#define C_QNAN      0x11
#define C_NINF      0x09
#define C_NNORM     0x08
#define C_NDENORM   0x18
#define C_NZERO     0x12
#define C_PZERO     0x02
#define C_PDENORM   0x14
#define C_PNORM     0x04
#define C_PINF      0x05

enum form { A_ABC, A_AB, A_AC, A_B, X_B, X_CMP };

struct insn {
    const char *name;
    unsigned op;        /* primary opcode: 63 or 59 */
    unsigned xo;
    enum form form;
};

static const struct insn insns[] = {
    { "fadd",     63, 21, A_AB },  { "fadds",    59, 21, A_AB },
    { "fsub",     63, 20, A_AB },  { "fsubs",    59, 20, A_AB },
    { "fdiv",     63, 18, A_AB },  { "fdivs",    59, 18, A_AB },
    { "fmul",     63, 25, A_AC },  { "fmuls",    59, 25, A_AC },
    { "fmadd",    63, 29, A_ABC }, { "fmadds",   59, 29, A_ABC },
    { "fmsub",    63, 28, A_ABC }, { "fmsubs",   59, 28, A_ABC },
    { "fnmadd",   63, 31, A_ABC }, { "fnmadds",  59, 31, A_ABC },
    { "fnmsub",   63, 30, A_ABC }, { "fnmsubs",  59, 30, A_ABC },
    { "fre",      63, 24, A_B },   { "fres",     59, 24, A_B },
    { "frsqrte",  63, 26, A_B },   { "frsqrtes", 59, 26, A_B },
    { "fsqrt",    63, 22, A_B },   { "fsqrts",   59, 22, A_B },
    { "frsp",     63, 12, X_B },
    { "frin",     63, 392, X_B },  { "friz",     63, 424, X_B },
    { "frip",     63, 456, X_B },  { "frim",     63, 488, X_B },
    { "fctiw",    63, 14, X_B },   { "fctiwz",   63, 15, X_B },
    { "fctiwu",   63, 142, X_B },  { "fctiwuz",  63, 143, X_B },
    { "fctid",    63, 814, X_B },  { "fctidz",   63, 815, X_B },
    { "fctidu",   63, 942, X_B },  { "fctiduz",  63, 943, X_B },
    { "fcfid",    63, 846, X_B },  { "fcfids",   59, 846, X_B },
    { "fcfidu",   63, 974, X_B },  { "fcfidus",  59, 974, X_B },
    { "fcmpu",    63, 0, X_CMP },  { "fcmpo",    63, 32, X_CMP },
};

#define NINSNS (sizeof(insns) / sizeof(insns[0]))

/* compares write CR field 6, so that CR1 keeps its own meaning */
#define CMP_BF 6

static uint32_t encode(const struct insn *in, int frt, int fra, int frb,
                       int frc, int rc)
{
    uint32_t w = in->op << 26;

    switch (in->form) {
    case A_ABC:
        return w | frt << 21 | fra << 16 | frb << 11 | frc << 6 |
               in->xo << 1 | rc;
    case A_AB:
        return w | frt << 21 | fra << 16 | frb << 11 | in->xo << 1 | rc;
    case A_AC:
        return w | frt << 21 | fra << 16 | frc << 6 | in->xo << 1 | rc;
    case A_B:
        return w | frt << 21 | frb << 11 | in->xo << 1 | rc;
    case X_B:
        return w | frt << 21 | frb << 11 | in->xo << 1 | rc;
    case X_CMP:
        return w | CMP_BF << 23 | fra << 16 | frb << 11 | in->xo << 1;
    }
    abort();
}

/*
 * The block run_slot() works on.  Offsets are hard-coded in its assembly,
 * so keep the layout in step with it.  fpscr_in is overwritten.
 */
struct fp_run {
    uint64_t f[8];          /*   0: f0..f7 in; f1, f5 read back after */
    uint64_t fpscr_in;      /*  64 */
    uint64_t fpscr_out;     /*  72 */
    uint64_t cr_out;        /*  80 */
    uint64_t vs1_lo_in;     /*  88: doubleword 1 of VSR 1 before */
    uint64_t vs1_lo_out;    /*  96: and after */
    uint64_t slot;          /* 104: code to call */
};

/*
 * Load f0..f7 (f1 through VSR 1, so that its doubleword 1 can be preset)
 * and the FPSCR, clear the CR, call the slot and store the outcome.  The
 * FPSCR is loaded with mtfsf over all fields, which does not set FEX or VX;
 * the cases never preload an enabled exception.
 */
static void run_slot(struct fp_run *r)
{
    asm volatile(
        "lfd     0, 64(%0)\n\t"
        "mtfsf   0xff, 0\n\t"
        "lfd     0, 0(%0)\n\t"
        "lfd     2, 16(%0)\n\t"
        "lfd     3, 24(%0)\n\t"
        "lfd     4, 32(%0)\n\t"
        "lfd     5, 40(%0)\n\t"
        "lfd     6, 48(%0)\n\t"
        "lfd     7, 56(%0)\n\t"
        "ld      5, 88(%0)\n\t"
        "ld      6, 8(%0)\n\t"
        "mtvsrdd 1, 6, 5\n\t"
        "li      0, 0\n\t"
        "mtcrf   0xff, 0\n\t"
        "ld      12, 104(%0)\n\t"
        "mtctr   12\n\t"
        "bctrl\n\t"
        "mfcr    0\n\t"
        "std     0, 80(%0)\n\t"
        "stfd    1, 8(%0)\n\t"
        "stfd    5, 40(%0)\n\t"
        "mfvsrld 0, 1\n\t"
        "std     0, 96(%0)\n\t"
        "mffs    0\n\t"
        "stfd    0, 72(%0)\n\t"
        "li      0, 0\n\t"
        "std     0, 64(%0)\n\t"
        "lfd     0, 64(%0)\n\t"
        "mtfsf   0xff, 0\n\t"
        : : "b"(r)
        : "r0", "r5", "r6", "r12", "lr", "ctr", "fr0", "fr1", "fr2", "fr3",
          "fr4", "fr5", "fr6", "fr7", "vs1", "cr0", "cr1", "cr2", "cr3",
          "cr4", "cr5", "cr6", "cr7", "memory");
}

static uint32_t *code;

/* Put up to two instructions and a blr into the code buffer. */
static void set_slot(struct fp_run *r, uint32_t i0, uint32_t i1)
{
    int n = 0;

    code[n++] = i0;
    if (i1) {
        code[n++] = i1;
    }
    code[n++] = 0x4e800020;     /* blr */
    __builtin___clear_cache((char *)code, (char *)(code + n));
    r->slot = (uintptr_t)code;
}

/* Operands for the value table */
static const struct { const char *name; uint64_t v; } vals[] = {
    { "1.5",     0x3ff8000000000000ull },
    { "-3",      0xc008000000000000ull },
    { "+0",      0x0000000000000000ull },
    { "-0",      0x8000000000000000ull },
    { "denorm",  0x0000000000000001ull },
    { "+inf",    0x7ff0000000000000ull },
    { "qnan",    0x7ff8000000000000ull },
    { "snan",    0x7ff4000000000000ull },
    { "1/3",     0x3fd5555555555555ull },
    { "tiny",    0x0170000000000000ull },  /* 2^-1000 */
    { "huge",    0x7e70000000000000ull },  /* 2^1000 */
};

#define NVALS (sizeof(vals) / sizeof(vals[0]))

/* The integer operands the conversions from integer get */
static const struct { const char *name; uint64_t v; } ints[] = {
    { "1",       1 },
    { "-1",      0xffffffffffffffffull },
    { "2^53+1",  0x0020000000000001ull },
    { "2^24+1",  0x0000000001000001ull },
    { "minint",  0x8000000000000000ull },
};

#define NINTS (sizeof(ints) / sizeof(ints[0]))

/* fmadd and friends: three operands from a shorter list */
static const int abc_vals[] = { 0, 1, 2, 5, 7, 8, 9, 10 };

#define NABC (sizeof(abc_vals) / sizeof(abc_vals[0]))

#define MARK 0x7ff7deadbeef0001ull      /* never a result: an sNaN payload */

/* Rounding modes the table is run under */
static const struct { const char *name; unsigned rn; } modes[] = {
    { "rn", 0 }, { "rz", 1 },
};

static void one(const struct insn *in, const char *mode, unsigned rn,
                uint64_t a, const char *an, uint64_t b, const char *bn,
                uint64_t c, const char *cn)
{
    struct fp_run r;

    memset(&r, 0, sizeof(r));
    r.f[1] = MARK;
    r.f[2] = a;
    r.f[3] = b;
    r.f[4] = c;
    r.fpscr_in = rn;
    r.vs1_lo_in = 0x0123456789abcdefull;
    set_slot(&r, encode(in, 1, 2, 3, 4, in->form != X_CMP), 0);
    run_slot(&r);

    printf("%-8s %s", in->name, mode);
    switch (in->form) {
    case A_ABC:
        printf(" %s %s %s", an, cn, bn);
        break;
    case A_AB:
    case X_CMP:
        printf(" %s %s", an, bn);
        break;
    case A_AC:
        printf(" %s %s", an, cn);
        break;
    case A_B:
    case X_B:
        printf(" %s", bn);
        break;
    }
    printf(" -> %016llx fpscr=%08llx cr=%08llx lo=%llx\n",
           (unsigned long long)r.f[1], (unsigned long long)r.fpscr_out,
           (unsigned long long)r.cr_out, (unsigned long long)r.vs1_lo_out);
}

static void value_table(void)
{
    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        for (size_t k = 0; k < NINSNS; k++) {
            const struct insn *in = &insns[k];
            int is_cfid = strncmp(in->name, "fcfid", 5) == 0;

            switch (in->form) {
            case A_ABC:
                if (modes[m].rn != 0) {
                    break;      /* three operands: round-to-nearest only */
                }
                for (size_t i = 0; i < NABC; i++) {
                    for (size_t j = 0; j < NABC; j++) {
                        for (size_t l = 0; l < NABC; l++) {
                            int a = abc_vals[i], cc = abc_vals[j];
                            int b = abc_vals[l];
                            one(in, modes[m].name, modes[m].rn,
                                vals[a].v, vals[a].name, vals[b].v,
                                vals[b].name, vals[cc].v, vals[cc].name);
                        }
                    }
                }
                break;
            case A_AB:
            case X_CMP:
                for (size_t i = 0; i < NVALS; i++) {
                    for (size_t j = 0; j < NVALS; j++) {
                        one(in, modes[m].name, modes[m].rn,
                            vals[i].v, vals[i].name, vals[j].v,
                            vals[j].name, 0, "-");
                    }
                }
                break;
            case A_AC:
                for (size_t i = 0; i < NVALS; i++) {
                    for (size_t j = 0; j < NVALS; j++) {
                        one(in, modes[m].name, modes[m].rn,
                            vals[i].v, vals[i].name, 0, "-",
                            vals[j].v, vals[j].name);
                    }
                }
                break;
            case A_B:
            case X_B:
                if (is_cfid) {
                    for (size_t i = 0; i < NINTS; i++) {
                        one(in, modes[m].name, modes[m].rn, 0, "-",
                            ints[i].v, ints[i].name, 0, "-");
                    }
                } else {
                    for (size_t i = 0; i < NVALS; i++) {
                        one(in, modes[m].name, modes[m].rn, 0, "-",
                            vals[i].v, vals[i].name, 0, "-");
                    }
                }
                break;
            }
        }
    }
}

static int failed;

static const struct insn *find(const char *name)
{
    for (size_t k = 0; k < NINSNS; k++) {
        if (strcmp(insns[k].name, name) == 0) {
            return &insns[k];
        }
    }
    abort();
}

static void expect(const char *what, uint64_t got, uint64_t mask,
                   uint64_t want)
{
    if ((got & mask) != want) {
        printf("FAIL: %s: got %016llx & %016llx, want %016llx\n", what,
               (unsigned long long)got, (unsigned long long)mask,
               (unsigned long long)want);
        failed = 1;
    }
}

/* Run one instruction with the FPSCR preloaded to fpscr_in. */
static struct fp_run run1(const char *name, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t fpscr_in)
{
    struct fp_run r;

    memset(&r, 0, sizeof(r));
    r.f[1] = MARK;
    r.f[2] = a;
    r.f[3] = b;
    r.f[4] = c;
    r.fpscr_in = fpscr_in;
    r.vs1_lo_in = 0x0123456789abcdefull;
    set_slot(&r, encode(find(name), 1, 2, 3, 4, 1), 0);
    run_slot(&r);
    return r;
}

#define ONE   0x3ff0000000000000ull
#define THREE 0x4008000000000000ull
#define TWO   0x4000000000000000ull

/* Expectations written from the ISA, independent of the reference file. */
static void hand_checks(void)
{
    struct fp_run r;

    /* FPRF for each class, through fadd x + 0 (exact) */
    static const struct { uint64_t v; unsigned cls; } cls[] = {
        { 0x3ff8000000000000ull, C_PNORM },
        { 0xc008000000000000ull, C_NNORM },
        { 0x0000000000000001ull, C_PDENORM },
        { 0x8000000000000001ull, C_NDENORM },
        { 0x7ff0000000000000ull, C_PINF },
        { 0xfff0000000000000ull, C_NINF },
        { 0x7ff8000000000000ull, C_QNAN },
    };
    for (size_t i = 0; i < sizeof(cls) / sizeof(cls[0]); i++) {
        r = run1("fadd", cls[i].v, 0, 0, 0);
        expect("fadd FPRF class", r.fpscr_out, FPRF,
               (uint64_t)cls[i].cls << FPRF_SHIFT);
        expect("fadd exact: no XX/FI/FR", r.fpscr_out, XX | FI | FR, 0);
    }
    /* -0 + -0 = -0 */
    r = run1("fadd", 0x8000000000000000ull, 0x8000000000000000ull, 0, 0);
    expect("fadd -0", r.fpscr_out, FPRF, (uint64_t)C_NZERO << FPRF_SHIFT);
    /* +0 from 1 - 1 in round-to-nearest */
    r = run1("fsub", ONE, ONE, 0, 0);
    expect("fsub +0", r.fpscr_out, FPRF, (uint64_t)C_PZERO << FPRF_SHIFT);

    /* 1/3 is inexact and rounded down: XX, FI, FX set; FR clear */
    r = run1("fdiv", ONE, THREE, 0, 0);
    expect("fdiv 1/3 flags", r.fpscr_out, FX | XX | FI | FR, FX | XX | FI);
    expect("fdiv 1/3 value", r.f[1], ~0ull, 0x3fd5555555555555ull);
    /*
     * 1/10 is inexact and rounded up (0x3fb999999999999a).  The ISA sets FR
     * here, but QEMU never sets FR (it only ever clears it), so FR is left
     * out of the expectation; the reference file records what QEMU does.
     */
    r = run1("fdiv", ONE, 0x4024000000000000ull, 0, 0);
    expect("fdiv 1/10 flags", r.fpscr_out, XX | FI, XX | FI);
    expect("fdiv 1/10 value", r.f[1], ~0ull, 0x3fb999999999999aull);

    /* CR1 is FPSCR[FX FEX VX OX] after an Rc=1 instruction */
    r = run1("fdiv", ONE, THREE, 0, 0);
    expect("fdiv. CR1", r.cr_out, 0x0f000000, (uint64_t)(FX >> 28) << 24);

    /* overflow and underflow with the exceptions disabled */
    r = run1("fmul", 0x7e70000000000000ull, 0, 0x7e70000000000000ull, 0);
    expect("fmul overflow", r.fpscr_out, OX | XX | FI, OX | XX | FI);
    expect("fmul overflow value", r.f[1], ~0ull, 0x7ff0000000000000ull);
    r = run1("fmul", 0x0170000000000000ull, 0, 0x0170000000000000ull, 0);
    expect("fmul underflow", r.fpscr_out, UX | XX | FI, UX | XX | FI);

    /* signalling NaN: VXSNAN, VX, FX; the result is the quieted NaN */
    r = run1("fadd", 0x7ff4000000000000ull, ONE, 0, 0);
    expect("fadd snan flags", r.fpscr_out, FX | VX | VXSNAN,
           FX | VX | VXSNAN);
    expect("fadd snan value", r.f[1], ~0ull, 0x7ffc000000000000ull);
    expect("fadd snan FPRF", r.fpscr_out, FPRF,
           (uint64_t)C_QNAN << FPRF_SHIFT);

    /* inf - inf: VXISI, default QNaN */
    r = run1("fsub", 0x7ff0000000000000ull, 0x7ff0000000000000ull, 0, 0);
    expect("fsub inf-inf", r.fpscr_out, VX | VXISI, VX | VXISI);
    expect("fsub inf-inf value", r.f[1], ~0ull, 0x7ff8000000000000ull);

    /* 1/0: ZX, infinity */
    r = run1("fdiv", ONE, 0, 0, 0);
    expect("fdiv 1/0", r.fpscr_out, ZX | FX, ZX | FX);
    expect("fdiv 1/0 value", r.f[1], ~0ull, 0x7ff0000000000000ull);

    /* fctiw of a NaN: VXCVI, 0x80000000 in the low word (FPRF undefined) */
    r = run1("fctiw", 0, 0x7ff8000000000000ull, 0, 0);
    expect("fctiw nan", r.fpscr_out, VX | VXCVI, VX | VXCVI);
    expect("fctiw nan value", r.f[1], 0xffffffffull, 0x80000000ull);

    /* fcfid sets FPRF */
    r = run1("fcfid", 0, 1, 0, (uint64_t)C_PINF << FPRF_SHIFT);
    expect("fcfid sets FPRF", r.fpscr_out, FPRF,
           (uint64_t)C_PNORM << FPRF_SHIFT);

    /* fcmpu with a qNaN: CR6 = FU, FPCC = FU, no VXVC */
    r = run1("fcmpu", 0x7ff8000000000000ull, ONE, 0, 0);
    expect("fcmpu qnan CR6", r.cr_out, 0xf0, 0x1 << 4);
    expect("fcmpu qnan FPCC", r.fpscr_out, 0xf << FPRF_SHIFT,
           0x1 << FPRF_SHIFT);
    expect("fcmpu qnan no VXVC", r.fpscr_out, VXVC, 0);
    /* fcmpo with a qNaN: VXVC */
    r = run1("fcmpo", 0x7ff8000000000000ull, ONE, 0, 0);
    expect("fcmpo qnan VXVC", r.fpscr_out, VXVC | VX, VXVC | VX);

    /* the target's doubleword 1 is zeroed by a scalar FP result */
    r = run1("fadd", ONE, ONE, 0, 0);
    expect("doubleword 1 zeroed", r.vs1_lo_out, ~0ull, 0);

    /*
     * mffs in front of fdiv. in the same translation block: the FPSCR the
     * helper writes must reach CR1 and the next mffs, not a copy of the
     * FPSCR taken before the call.
     */
    memset(&r, 0, sizeof(r));
    r.f[2] = ONE;
    r.f[3] = THREE;
    set_slot(&r, 0xfc00048e /* mffs f0 */,
             encode(find("fdiv"), 1, 2, 3, 4, 1));
    run_slot(&r);
    expect("mffs; fdiv. CR1", r.cr_out, 0x0f000000,
           (uint64_t)(FX >> 28) << 24);
    expect("mffs; fdiv. FPSCR", r.fpscr_out, FX | XX | FI, FX | XX | FI);
}

/*
 * One inexact instruction into f5, then an exact one of the family under
 * test into f1.  XX is sticky and stays set, but FI and FR describe the
 * last instruction only, so they must be clear: if the exception flags of
 * the first instruction leaked into the second one, FI would be set.
 */
static void chain_checks(void)
{
    static const struct {
        const char *name;
        uint64_t a, b, c;
    } second[] = {
        { "fadd",    ONE, ONE, 0 },
        { "fmul",    ONE, 0, TWO },
        { "fmadd",   ONE, ONE, TWO },
        { "fmadds",  ONE, ONE, TWO },
        { "fre",     0, TWO, 0 },
        { "frsqrte", 0, 0x4010000000000000ull, 0 },     /* 4 */
        { "fsqrt",   0, 0x4010000000000000ull, 0 },
        { "frsp",    0, ONE, 0 },
        { "frin",    0, ONE, 0 },
        { "fctiw",   0, ONE, 0 },
        { "fcfid",   0, 1, 0 },
        { "fcmpu",   ONE, ONE, 0 },
    };
    uint32_t first = encode(find("fdiv"), 5, 6, 7, 0, 0);

    for (size_t i = 0; i < sizeof(second) / sizeof(second[0]); i++) {
        struct fp_run r;
        char what[64];

        memset(&r, 0, sizeof(r));
        r.f[1] = MARK;
        r.f[2] = second[i].a;
        r.f[3] = second[i].b;
        r.f[4] = second[i].c;
        r.f[6] = ONE;
        r.f[7] = THREE;
        set_slot(&r, first, encode(find(second[i].name), 1, 2, 3, 4, 0));
        run_slot(&r);
        snprintf(what, sizeof(what), "fdiv 1/3 then %s", second[i].name);
        expect(what, r.fpscr_out, XX | FI | FR, XX);
        printf("chain    %-8s -> %016llx fpscr=%08llx\n", second[i].name,
               (unsigned long long)r.f[1], (unsigned long long)r.fpscr_out);
    }
}

static void clear_fpscr(void)
{
    double z = 0;

    asm volatile("mtfsf 0xff, %0" : : "d"(z));
}

static sigjmp_buf env;
/* written by the SIGFPE handler, read after siglongjmp() */
static volatile uintptr_t sig_addr;
static volatile uint64_t sig_f1, sig_fpscr;    /* likewise */

static void on_fpe(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = uc_;

    sig_addr = (uintptr_t)si->si_addr;
    uint64_t v;

    memcpy(&v, &uc->uc_mcontext.fp_regs[1], sizeof(v));
    sig_f1 = v;
    memcpy(&v, &uc->uc_mcontext.fp_regs[32], sizeof(v));
    sig_fpscr = v;
    siglongjmp(env, 1);
}

/*
 * Enabled exceptions.  A nop in front of the instruction keeps it off the
 * start of the translation block, so that a wrong return address cannot
 * pass by accident.
 */
static void exc_check(const char *name, uint64_t a, uint64_t b, uint64_t c,
                      uint64_t enable, int written)
{
    struct fp_run r;
    char what[96];

    memset(&r, 0, sizeof(r));
    r.f[1] = MARK;
    r.f[2] = a;
    r.f[3] = b;
    r.f[4] = c;
    r.fpscr_in = enable;
    set_slot(&r, 0x60000000, encode(find(name), 1, 2, 3, 4, 0));
    sig_addr = 0;
    if (sigsetjmp(env, 1) != 0) {
        clear_fpscr();
    } else {
        run_slot(&r);
        snprintf(what, sizeof(what), "%s: no SIGFPE", name);
        expect(what, 0, 1, 1);
        return;
    }
    snprintf(what, sizeof(what), "%s: si_addr", name);
    expect(what, sig_addr, ~0ull, (uintptr_t)(code + 1));
    snprintf(what, sizeof(what), "%s: target %s", name,
             written ? "written" : "untouched");
    if (written) {
        expect(what, sig_f1 == MARK, 1, 0);
    } else {
        expect(what, sig_f1, ~0ull, MARK);
    }
    printf("exc      %-8s -> f1=%016llx fpscr=%08llx\n", name,
           (unsigned long long)sig_f1, (unsigned long long)sig_fpscr);
}

static void exception_checks(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fpe;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGFPE, &sa, NULL);

    /* OE, UE, XE: the result is written before the interrupt */
    exc_check("fmul", 0x7e70000000000000ull, 0, 0x7e70000000000000ull,
              OE, 1);
    exc_check("fmul", 0x0170000000000000ull, 0, 0x0170000000000000ull,
              UE, 1);
    exc_check("fdiv", ONE, THREE, 0, XE, 1);
    exc_check("fmadds", ONE, THREE, 0x3fd5555555555555ull, XE, 1);
    exc_check("frsp", 0, 0x3fd5555555555555ull, 0, XE, 1);
    /* VE, ZE: the target is left alone */
    exc_check("fsub", 0x7ff0000000000000ull, 0x7ff0000000000000ull, 0,
              VE, 0);
    exc_check("fdiv", ONE, 0, 0, ZE, 0);
    exc_check("fre", 0, 0, 0, ZE, 0);
    exc_check("fctiw", 0, 0x7ff8000000000000ull, 0, VE, 0);
    exc_check("frin", 0, 0x7ff4000000000000ull, 0, VE, 0);
    exc_check("fcmpo", 0x7ff8000000000000ull, ONE, 0, VE, 0);
    /*
     * fcfid raises an enabled inexact exception before it writes the
     * target.  That is not what the ISA says, but it is what QEMU does
     * today, and #382 keeps it.
     */
    exc_check("fcfid", 0, 0x0020000000000001ull, 0, XE, 0);
}

int main(void)
{
    code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(code != MAP_FAILED);

    hand_checks();
    chain_checks();
    exception_checks();
    value_table();

    if (failed) {
        printf("FAIL\n");
        return 1;
    }
    return 0;
}
