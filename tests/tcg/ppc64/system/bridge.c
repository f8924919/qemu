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
 *       not hold;
 *   (d) single stepping (MSR[SE]) over a sync that completes a pending
 *       tlbie still takes the trace interrupt after every instruction;
 *   (e) mfsrin returns what mtsr wrote even when another SLB entry, written
 *       with slbmte, holds the same segment.
 *
 * The 970 drops only the pages a tlbie names, in the segments whose VSID
 * matches the low VSID bits of RB.  These check that nothing it should
 * drop survives:
 *
 *   (f) a tlbie whose RB holds the VSID and the page (the 64-bit form)
 *       drops that page;
 *   (g) another page of the segment, not named, still reads right;
 *   (h) 16 pages (all kept) and 17 pages (one too many, so all is
 *       dropped) are all dropped, the last one included;
 *   (i) a tlbiel in 32-bit mode with stale bits in the upper half of RB
 *       drops the page;
 *   (j) slbmte giving a valid entry another VSID, then isync, makes the
 *       page read through the new segment;
 *   (k) a segment held in two SLB entries (two ESIDs, one VSID) is
 *       dropped through both.
 *
 * The test code runs in real mode; only the load under test is done with
 * data relocation on, inside one asm statement, so that nothing else (the
 * stack in particular) needs a mapping.  mtmsrd does not complete a
 * pending TLB flush in QEMU, so it does not hide (a).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <minilib.h>

#include "bridge.h"

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

#define VEC_TRACE 0xd00UL

#define VAL_1 0x1111111111111111UL
#define VAL_2 0x2222222222222222UL

/* Pages of (h): where they first map and where they are remapped */
#define MANY_OLD  0x01100000UL
#define MANY_NEW  0x01200000UL
#define MANY_MAX  17
#define VAL_OLD(i) (0xa000000000000000UL | (i))
#define VAL_NEW(i) (0xb000000000000000UL | (i))

/* A second segment for (k), held by SLB entry 5 */
#define EA_K   0x50000000UL

#define MSR_SF 0x8000000000000000UL

/* tlbie RB before ISA 2.06: low VSID bits above the page in the segment */
#define TLBIE_VA(vsid, ea) (((vsid) << 28) | ((ea) & 0x0fffffffUL))

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

static void wr32(unsigned long addr, unsigned int val)
{
    /* volatile: see wr64 */
    *(volatile unsigned int *)addr = val;
}

static void install_vector(unsigned long vec, const char *start,
                           const char *end)
{
    const unsigned int *src = (const unsigned int *)start;
    unsigned long n = (end - start) / 4;
    unsigned long i;

    for (i = 0; i < n; i++) {
        wr32(vec + 4 * i, src[i]);
        asm volatile("dcbst 0,%0; sync; icbi 0,%0" : : "r"(vec + 4 * i)
                     : "memory");
    }
    asm volatile("isync");
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

/* (d) single stepping over a sync that completes a pending tlbie */
static void case_trace_sync(void)
{
    unsigned long want[5] = {
        (unsigned long)br_l1, (unsigned long)br_l2, (unsigned long)br_l3,
        (unsigned long)br_l4, (unsigned long)br_l5,
    };
    char step[8];
    int i;

    start_case();
    install_vector(VEC_TRACE, br_trace_stub, br_trace_stub_end);
    for (i = 0; i < 16; i++) {
        br_trace_state[i] = 0;
    }
    br_trace_run(EA_D);
    expect("trace-sync", "count", br_trace_state[0], 5);
    for (i = 0; i < 5; i++) {
        step[0] = '0' + i;
        step[1] = 0;
        expect("trace-sync", step, br_trace_state[1 + i], want[i]);
    }
}

/* (e) mfsrin when slbmte put the same segment in another entry */
static void case_mfsrin_slot(void)
{
    unsigned long rb = (EA_D & SLB_ESID_ESID) | SLB_ESID_V | 0;

    start_case();
    asm volatile("slbmte %0,%1" : : "r"(VSID_1 << SLB_VSID_SHIFT), "r"(rb)
                 : "memory");
    mtsr(VSID_2);
    isync();
    expect("mfsrin-slot", "written", mfsrin(EA_D), VSID_2);
}

static void tlbie_sync(unsigned long rb)
{
    asm volatile("tlbie %0,%1; tlbsync; sync" : : "r"(rb), "r"(0UL)
                 : "memory");
}

/* (f) and (g): the 64-bit form of RB names one page */
static void case_tlbie_va(void)
{
    unsigned long ea_g = EA_D + 0x1000;

    start_case();
    pte_put(VSID_1, EA_D, PAGE_1);
    pte_put(VSID_1, ea_g, PAGE_2);
    mtsr(VSID_1);
    isync();
    expect("tlbie-va", "before", load_dr(EA_D), VAL_1);
    expect("tlbie-va", "other-before", load_dr(ea_g), VAL_2);

    pte_put(VSID_1, EA_D, PAGE_2);
    tlbie_sync(TLBIE_VA(VSID_1, EA_D));
    expect("tlbie-va", "after", load_dr(EA_D), VAL_2);
    /* (g) not named, not remapped */
    expect("tlbie-va", "other-after", load_dr(ea_g), VAL_2);
}

/* "b07": page 7 before the remap, "a07" after */
static void page_step(char *step, char when, int i)
{
    step[0] = when;
    step[1] = '0' + i / 10;
    step[2] = '0' + i % 10;
    step[3] = 0;
}

/* (h) n pages remapped, then n tlbies completed by one sync */
static void case_tlbie_many(const char *name, int n)
{
    char step[8];
    int i;

    start_case();
    for (i = 0; i < n; i++) {
        wr64(MANY_OLD + i * 0x1000UL, VAL_OLD(i));
        wr64(MANY_NEW + i * 0x1000UL, VAL_NEW(i));
        pte_put(VSID_1, EA_D + i * 0x1000UL, MANY_OLD + i * 0x1000UL);
    }
    mtsr(VSID_1);
    isync();
    for (i = 0; i < n; i++) {
        page_step(step, 'b', i);
        expect(name, step, load_dr(EA_D + i * 0x1000UL), VAL_OLD(i));
    }

    for (i = 0; i < n; i++) {
        pte_put(VSID_1, EA_D + i * 0x1000UL, MANY_NEW + i * 0x1000UL);
    }
    for (i = 0; i < n; i++) {
        asm volatile("tlbie %0,%1"
                     : : "r"(TLBIE_VA(VSID_1, EA_D + i * 0x1000UL)), "r"(0UL)
                     : "memory");
    }
    asm volatile("tlbsync; sync" : : : "memory");
    for (i = 0; i < n; i++) {
        page_step(step, 'a', i);
        expect(name, step, load_dr(EA_D + i * 0x1000UL), VAL_NEW(i));
    }
}

/*
 * gas does not take tlbiel without the operands of later ISAs; spell it
 * out with a fixed register as for mtsr.
 */
#define TLBIEL_R9 (0x7c000224 | (9 << 11))

/* (i) tlbiel in 32-bit mode; the upper half of RB holds stale bits */
static void case_tlbiel_32(void)
{
    unsigned long msr;

    start_case();
    pte_put(VSID_1, EA_D, PAGE_1);
    mtsr(VSID_1);
    isync();
    expect("tlbiel-32", "before", load_dr(EA_D), VAL_1);

    pte_put(VSID_1, EA_D, PAGE_2);
    asm volatile("mfmsr %0" : "=r"(msr));
    {
        /* set here: a call in between may clobber r9 */
        register unsigned long r9 asm("r9") = 0xffffffff00000000UL | EA_D;

        asm volatile("mtmsrd %1; .long " STR(TLBIEL_R9) "; mtmsrd %2; isync"
                     : : "r"(r9), "r"(msr & ~MSR_SF), "r"(msr) : "memory");
    }
    expect("tlbiel-32", "after", load_dr(EA_D), VAL_2);
}

/* (j) slbmte over a valid entry, without slbie */
static void case_slbmte_over(void)
{
    unsigned long rb = (EA_D & SLB_ESID_ESID) | SLB_ESID_V | SR_NUM;

    start_case();
    pte_put(VSID_1, EA_D, PAGE_1);
    pte_put(VSID_2, EA_D, PAGE_2);
    asm volatile("slbmte %0,%1; isync"
                 : : "r"(VSID_1 << SLB_VSID_SHIFT), "r"(rb) : "memory");
    expect("slbmte-over", "vsid1", load_dr(EA_D), VAL_1);

    asm volatile("slbmte %0,%1; isync"
                 : : "r"(VSID_2 << SLB_VSID_SHIFT), "r"(rb) : "memory");
    expect("slbmte-over", "vsid2", load_dr(EA_D), VAL_2);
}

/* (k) one VSID in two segments: the page drops through both */
static void case_tlbie_two_esid(void)
{
    unsigned long rb = (EA_K & SLB_ESID_ESID) | SLB_ESID_V | 5;

    start_case();
    pte_put(VSID_1, EA_D, PAGE_1);
    mtsr(VSID_1);
    asm volatile("slbmte %0,%1; isync"
                 : : "r"(VSID_1 << SLB_VSID_SHIFT), "r"(rb) : "memory");
    expect("tlbie-two-esid", "before-d", load_dr(EA_D), VAL_1);
    expect("tlbie-two-esid", "before-k", load_dr(EA_K), VAL_1);

    pte_put(VSID_1, EA_D, PAGE_2);
    tlbie_sync(TLBIE_VA(VSID_1, EA_D));
    expect("tlbie-two-esid", "after-d", load_dr(EA_D), VAL_2);
    expect("tlbie-two-esid", "after-k", load_dr(EA_K), VAL_2);
}

int main(void)
{
    case_tlbie_sync();
    case_mtsr();
    case_mfsrin();
    case_trace_sync();
    case_mfsrin_slot();
    case_tlbie_va();
    case_tlbie_many("tlbie-16", 16);
    case_tlbie_many("tlbie-17", MANY_MAX);
    case_tlbiel_32();
    case_slbmte_over();
    case_tlbie_two_esid();

    if (ok) {
        ml_printf("PASS: bridge\n");
    }

    return 0;
}
