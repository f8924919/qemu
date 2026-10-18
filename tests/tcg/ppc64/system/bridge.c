/*
 * The 64-bit bridge of the 970: what a 32-bit kernel does to the MMU.
 *
 * A 32-bit kernel on the 970 keeps using the instructions it knows: it
 * writes segment registers with mtsr, which land in SLB entries 0-15,
 * reads them back with mfsrin, and completes a tlbie with tlbsync and a
 * heavyweight sync, without ptesync.  Each case below checks one of those
 * against what the next data access sees:
 *
 *   (a) after "tlbie; tlbsync; sync" a remapped page reads through its new
 *       translation (no ptesync, no isync in between);
 *   (b) after mtsr gives a segment another VSID and isync, the page reads
 *       through the new segment; writing the same value again changes
 *       nothing;
 *   (c) mfsrin returns what mtsr wrote, and 0 for a segment the SLB does
 *       not hold.
 *
 * The test code runs in real mode; only the load under test is done with
 * data relocation on, inside one asm statement, so that nothing else (the
 * stack in particular) needs a mapping.  mtmsrd does not complete a
 * pending TLB flush in QEMU, so it does not hide (a).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <minilib.h>

#define SPR_SDR1 25

#define SLB_ESID_ESID  0xfffffffff0000000UL
#define SLB_ESID_V     0x0000000008000000UL
#define SLB_VSID_SHIFT 12

/* A hash table of 256 KiB (HTABSIZE = 0, so 2048 PTEGs), zeroed at reset */
#define HTAB 0x00400000UL
#define HTAB_PTEG_MASK 0x7ffUL
#define PTEG_SIZE 128UL

/* Physical pages read through the tested translations */
#define PAGE_1 0x01000000UL
#define PAGE_2 0x01001000UL

/* Segment 4, which mtsr 4 / slbmte to slot 4 both describe */
#define SR_NUM 4
#define EA_D   0x40000000UL

#define VSID_1 0x100UL
#define VSID_2 0x200UL

#define PTE0_V    1UL
#define PTE1_RC   0x180UL      /* referenced and changed already set */

#define MSR_DR 0x10UL

#define VAL_1 0x1111111111111111UL
#define VAL_2 0x2222222222222222UL

static int ok = 1;

/*
 * Real-mode stores to RAM that the MMU (page tables) or a relocated load
 * reads behind the compiler's back: none may be dropped or reordered.
 */
static void wr64(unsigned long addr, unsigned long val)
{
    /* volatile: see above */
    *(volatile unsigned long *)addr = val;
}

static unsigned long pte_addr(unsigned long vsid, unsigned long ea)
{
    unsigned long page = (ea & ~SLB_ESID_ESID) >> 12;

    return HTAB + ((vsid ^ page) & HTAB_PTEG_MASK) * PTEG_SIZE;
}

static unsigned long pte0_of(unsigned long vsid, unsigned long ea)
{
    /* the VSID and the top bits of the page index (AVPN) */
    return (vsid << SLB_VSID_SHIFT) |
           (((ea & ~SLB_ESID_ESID) >> 16) & 0xf80) | PTE0_V;
}

/* Map ea in the segment of vsid to phys; completed with ptesync */
static void pte_put(unsigned long vsid, unsigned long ea, unsigned long phys)
{
    unsigned long addr = pte_addr(vsid, ea);

    wr64(addr, 0);                      /* invalid while it changes */
    wr64(addr + 8, phys | PTE1_RC);
    wr64(addr, pte0_of(vsid, ea));
    asm volatile("ptesync" : : : "memory");
}

/* Load the doubleword at ea with data relocation on */
static unsigned long load_dr(unsigned long ea)
{
    unsigned long msr, val;

    asm volatile("mfmsr %0" : "=r"(msr));
    asm volatile("mtmsrd %2; ld %0,0(%1); mtmsrd %3"
                 : "=&r"(val)
                 : "b"(ea), "r"(msr | MSR_DR), "r"(msr)
                 : "memory");
    return val;
}

/*
 * gas does not take mtsr and mfsrin for -mcpu=970 (they are 32-bit
 * instructions, which the 970 keeps for the bridge), so spell them out
 * with fixed registers.
 */
#define MTSR_R9(sr)      (0x7c0001a4 | (9 << 21) | ((sr) << 16))
#define MFSRIN_R9_R10    (0x7c000526 | (9 << 21) | (10 << 11))

#define STR_(x) #x
#define STR(x) STR_(x)

static void mtsr(unsigned long val)
{
    register unsigned long r9 asm("r9") = val;

    asm volatile(".long " STR(MTSR_R9(SR_NUM)) : : "r"(r9) : "memory");
}

static unsigned long mfsrin(unsigned long ea)
{
    register unsigned long r9 asm("r9");
    register unsigned long r10 asm("r10") = ea;

    asm volatile(".long " STR(MFSRIN_R9_R10) : "=r"(r9) : "r"(r10));
    return r9;
}

static void isync(void)
{
    asm volatile("isync" : : : "memory");
}

/*
 * A fresh start: empty SLB (slbia also drops the TLB at the isync), the
 * table in SDR1, both pages filled
 */
static void start_case(void)
{
    asm volatile("slbmte %0,%1; slbia; isync" : : "r"(0UL), "r"(0UL)
                 : "memory");
    asm volatile("mtspr %0,%1; isync" : : "i"(SPR_SDR1), "r"(HTAB)
                 : "memory");
    wr64(PAGE_1, VAL_1);
    wr64(PAGE_2, VAL_2);
}

static void expect(const char *name, const char *step, unsigned long got,
                   unsigned long want)
{
    ml_printf("bridge %s %s: %lx\n", name, step, got);
    if (got != want) {
        ml_printf("FAIL: bridge %s %s %lx expected %lx\n", name, step, got,
                  want);
        ok = 0;
    }
}

/* (a) tlbie completed by tlbsync and a heavyweight sync */
static void case_tlbie_sync(void)
{
    start_case();
    pte_put(VSID_1, EA_D, PAGE_1);
    mtsr(VSID_1);
    isync();
    expect("tlbie-sync", "before", load_dr(EA_D), VAL_1);

    /* remap, then complete the tlbie the way a 32-bit kernel does */
    pte_put(VSID_1, EA_D, PAGE_2);
    asm volatile("tlbie %0,%1; tlbsync; sync"
                 : : "r"(EA_D), "r"(0UL) : "memory");
    expect("tlbie-sync", "after", load_dr(EA_D), VAL_2);
}

/* (b) mtsr giving the segment another VSID */
static void case_mtsr(void)
{
    start_case();
    pte_put(VSID_1, EA_D, PAGE_1);
    pte_put(VSID_2, EA_D, PAGE_2);
    mtsr(VSID_1);
    isync();
    expect("mtsr", "vsid1", load_dr(EA_D), VAL_1);

    mtsr(VSID_2);
    isync();
    expect("mtsr", "vsid2", load_dr(EA_D), VAL_2);

    /* the same value again: nothing changes */
    mtsr(VSID_2);
    isync();
    expect("mtsr", "same", load_dr(EA_D), VAL_2);
}

/* (c) mfsrin reads what mtsr wrote */
static void case_mfsrin(void)
{
    /* VSID with Ks and Kp set, the format mtsr takes */
    unsigned long val = 0x60000123UL;

    start_case();
    mtsr(val);
    isync();
    expect("mfsrin", "written", mfsrin(EA_D), val);
    /* nothing describes segment 5 */
    expect("mfsrin", "absent", mfsrin(EA_D + 0x10000000UL), 0);
}

int main(void)
{
    case_tlbie_sync();
    case_mtsr();
    case_mfsrin();

    if (ok) {
        ml_printf("PASS: bridge\n");
    }

    return 0;
}
