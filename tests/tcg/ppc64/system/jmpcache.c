/*
 * The jump cache must not hand out a translation block that no longer
 * belongs to the address.
 *
 * TCG keeps, per vCPU, a small table indexed by the virtual address of the
 * next instruction (the jump cache) in front of the real lookup, which is
 * keyed by the physical address.  A flush of the softmmu TLB changes what a
 * virtual address means, so whatever the table remembers about it may be
 * out of date.  This test runs the code at one effective address X, changes
 * what X translates to by every means the 970 gives a guest, and runs X
 * again: the second run has to execute the code at the new physical page.
 *
 * X is reached in two ways, because the table is consulted from two places:
 *   (i)  rfid straight to X from real mode: the main loop looks X up;
 *   (ii) rfid to a trampoline page that does "mtctr; bctr" to X: generated
 *        code looks X up (helper_lookup_tb_ptr).  No direct branch, which
 *        TCG would chain instead.
 * Way (i) is also taken in problem state (iii), so that the translation of
 * X lives in a second softmmu MMU index: a flush that forgets one of the
 * two virtual indexes shows up here.
 * Each case runs on every path, and the runs of a path are identical: the
 * first one is what puts the code at X into the table.
 *
 * Instruction relocation is on and data relocation is off, so the test code
 * itself (in ROM, real mode) needs no mapping.  The tested pages are in RAM
 * and answer with sc, which the 0xc00 handler turns into a return.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <minilib.h>

#include "jmpcache.h"

#define SPR_SDR1 25

#define SLB_ESID_ESID  0xfffffffff0000000UL
#define SLB_ESID_V     0x0000000008000000UL
#define SLB_VSID_SHIFT 12

/*
 * Two hash tables of 256 KiB (HTABSIZE = 0, so 2048 PTEGs).  RAM starts out
 * zeroed, which makes every PTE invalid until the test writes it.
 */
#define HTAB1 0x00400000UL
#define HTAB2 0x00800000UL
#define HTAB_PTEG_MASK 0x7ffUL
#define PTEG_SIZE 128UL

/* Physical pages of the tested code */
#define PAGE_A     0x01000000UL
#define PAGE_B     0x01001000UL
#define PAGE_C     0x01002000UL
#define PAGE_TRAMP 0x01003000UL

/* X and the trampoline, in the same 256 MiB segment */
#define EA_X 0x40000000UL
#define EA_Y 0x40001000UL

/*
 * VSIDs.  The hash of a page is VSID ^ page index in the segment, so these
 * keep every PTE in a PTEG of its own.
 */
#define VSID_A 0x100UL
#define VSID_B 0x200UL
#define VSID_C 0x300UL

#define PTE0_V    1UL
#define PTE1_RC   0x180UL      /* referenced and changed already set */

#define VEC_ISI 0x400UL
#define VEC_SC  0xc00UL

#define MSR_PR 0x4000UL

#define NR_PATHS 3

static int ok = 1;

/*
 * Real-mode stores to RAM that the MMU (page tables) or the tested code
 * reads behind the compiler's back: none may be dropped or reordered.
 */
static void wr64(unsigned long addr, unsigned long val)
{
    /* volatile: see above */
    *(volatile unsigned long *)addr = val;
}

static void wr32(unsigned long addr, unsigned int val)
{
    /* volatile: see above */
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

/* li r3,mark; sc */
static void put_marker(unsigned long page, char mark)
{
    wr32(page, 0x38600000 | mark);
    wr32(page + 4, 0x44000002);
}

static void put_code(void)
{
    put_marker(PAGE_A, 'A');
    put_marker(PAGE_B, 'B');
    put_marker(PAGE_C, 'C');
    /* mtctr r4; bctr */
    wr32(PAGE_TRAMP, 0x7c8903a6);
    wr32(PAGE_TRAMP + 4, 0x4e800420);
    asm volatile("sync; isync" : : : "memory");
}

static unsigned long pte_addr(unsigned long htab, unsigned long vsid,
                              unsigned long ea)
{
    unsigned long page = (ea & ~SLB_ESID_ESID) >> 12;

    return htab + ((vsid ^ page) & HTAB_PTEG_MASK) * PTEG_SIZE;
}

static unsigned long pte0_of(unsigned long vsid, unsigned long ea)
{
    /* the VSID and the top bits of the page index (AVPN) */
    return (vsid << SLB_VSID_SHIFT) |
           (((ea & ~SLB_ESID_ESID) >> 16) & 0xf80) | PTE0_V;
}

static void pte_put(unsigned long htab, unsigned long vsid, unsigned long ea,
                    unsigned long phys)
{
    unsigned long addr = pte_addr(htab, vsid, ea);

    wr64(addr, 0);                      /* invalid while it changes */
    wr64(addr + 8, phys | PTE1_RC);
    wr64(addr, pte0_of(vsid, ea));
    asm volatile("ptesync" : : : "memory");
}

static void pte_drop(unsigned long htab, unsigned long vsid, unsigned long ea)
{
    wr64(pte_addr(htab, vsid, ea), 0);
    asm volatile("ptesync" : : : "memory");
}

/* The page tables every case starts from */
static void put_tables(void)
{
    unsigned long vsids[3] = { VSID_A, VSID_B, VSID_C };
    unsigned long pages[3] = { PAGE_A, PAGE_B, PAGE_C };
    int i;

    for (i = 0; i < 3; i++) {
        pte_put(HTAB1, vsids[i], EA_X, pages[i]);
        pte_put(HTAB1, vsids[i], EA_Y, PAGE_TRAMP);
    }
    /* the second table sends the VSID that meant A to B */
    pte_put(HTAB2, VSID_A, EA_X, PAGE_B);
    pte_put(HTAB2, VSID_A, EA_Y, PAGE_TRAMP);
}

static void slb_store(unsigned long slot, unsigned long vsid)
{
    unsigned long rb = (EA_X & SLB_ESID_ESID) | SLB_ESID_V | slot;
    unsigned long rs = vsid << SLB_VSID_SHIFT;

    asm volatile("slbmte %0,%1" : : "r"(rs), "r"(rb) : "memory");
}

static void slb_clear_entry0(void)
{
    asm volatile("slbmte %0,%1" : : "r"(0UL), "r"(0UL) : "memory");
}

static void slb_ia(void)
{
    asm volatile("slbia" : : : "memory");
}

static void slb_ie(unsigned long ea)
{
    asm volatile("slbie %0" : : "r"(ea) : "memory");
}

static void tlb_ie(unsigned long ea)
{
    asm volatile("tlbie %0,%1" : : "r"(ea), "r"(0UL) : "memory");
}

static void sdr1_set(unsigned long htab)
{
    asm volatile("mtspr %0,%1; isync" : : "i"(SPR_SDR1), "r"(htab)
                 : "memory");
}

static void isync(void)
{
    asm volatile("isync" : : : "memory");
}

/* The state every case starts from: X means A, in SLB entry x_slot */
static void start_case(unsigned long x_slot)
{
    put_tables();
    sdr1_set(HTAB1);
    slb_clear_entry0();
    slb_ia();
    slb_store(x_slot, VSID_A);
    isync();
    jc_isi_state[0] = 0;
    jc_isi_state[1] = 0;
}

/*
 * Run X by the path: 0 = rfid to X, 1 = rfid to the trampoline, bctr to X,
 * 2 = rfid to X in problem state
 */
static unsigned long run_x(int path)
{
    switch (path) {
    case 0:
        return jc_run(EA_X, 0, 0);
    case 1:
        return jc_run(EA_Y, EA_X, 0);
    default:
        return jc_run(EA_X, 0, MSR_PR);
    }
}

static const char *path_name(int path)
{
    switch (path) {
    case 0:
        return "rfid";
    case 1:
        return "bctr";
    default:
        return "user";
    }
}

/* ways to change what X means */

static void sw_slbie(unsigned long vsid)            /* (a) */
{
    slb_ie(EA_X);
    slb_store(1, vsid);
    isync();
}

static void sw_slbia(unsigned long vsid)            /* (b) */
{
    slb_ia();
    slb_store(1, vsid);
    isync();
}

static void sw_slot0(unsigned long vsid)            /* (c) */
{
    /*
     * Overwrite entry 0 without slbie, then slbia: it leaves entry 0 alone
     * but still asks for the flush.
     */
    slb_store(0, vsid);
    slb_ia();
    isync();
}

static void sw_pte(unsigned long vsid)              /* (d) */
{
    pte_put(HTAB1, VSID_A, EA_X, vsid == VSID_B ? PAGE_B : PAGE_A);
    tlb_ie(EA_X);
    asm volatile("eieio; tlbsync; ptesync" : : : "memory");
}

static void sw_sdr1(unsigned long vsid)             /* (e) */
{
    sdr1_set(vsid == VSID_B ? HTAB2 : HTAB1);
}

static void sw_same(unsigned long vsid)             /* case 2: no change */
{
    slb_ia();
    slb_store(1, vsid);
    isync();
}

static char mark_char(unsigned long m)
{
    return (m >= 'A' && m <= 'C') ? (char)m : '?';
}

static void report(const char *name, int path, const char *want,
                   const unsigned long *got, int n)
{
    int i;

    ml_printf("jmpcache %s %s:", name, path_name(path));
    for (i = 0; i < n; i++) {
        ml_printf(" %c", mark_char(got[i]));
    }
    ml_printf("\n");

    for (i = 0; i < n; i++) {
        if (got[i] != (unsigned long)want[i]) {
            ml_printf("FAIL: jmpcache %s %s got %c expected %c (run %d)\n",
                      name, path_name(path), mark_char(got[i]),
                      want[i], i + 1);
            ok = 0;
        }
    }
}

static void check_isi(const char *name, int path, unsigned long want)
{
    if (jc_isi_state[0] != want) {
        ml_printf("FAIL: jmpcache %s %s isi count %lu expected %lu\n",
                  name, path_name(path), jc_isi_state[0], want);
        ok = 0;
    } else if (want && jc_isi_state[1] != EA_X) {
        ml_printf("FAIL: jmpcache %s %s isi at 0x%lx expected 0x%lx\n",
                  name, path_name(path), jc_isi_state[1], EA_X);
        ok = 0;
    }
}

/*
 * The means of switching.  Chosen with a switch rather than a function
 * pointer: the text is in ROM and the TOC in RAM, out of reach of the
 * 32-bit TOC offsets the compiler uses for the address of a function.
 */
enum way { SLBIE, SLBIA, SLOT0, PTE, SDR1 };

static void switch_by(enum way how, unsigned long vsid)
{
    switch (how) {
    case SLBIE:
        sw_slbie(vsid);
        break;
    case SLBIA:
        sw_slbia(vsid);
        break;
    case SLOT0:
        sw_slot0(vsid);
        break;
    case PTE:
        sw_pte(vsid);
        break;
    case SDR1:
        sw_sdr1(vsid);
        break;
    }
}

/* A, switch to B, B: cases 1a to 1e */
static void case_switch(const char *name, unsigned long x_slot,
                        enum way how, int path)
{
    unsigned long got[2];

    start_case(x_slot);
    got[0] = run_x(path);
    switch_by(how, VSID_B);
    got[1] = run_x(path);
    report(name, path, "AB", got, 2);
    check_isi(name, path, 0);
}

static void case_same(int path)
{
    unsigned long got[2];

    start_case(1);
    got[0] = run_x(path);
    sw_same(VSID_A);
    got[1] = run_x(path);
    report("2", path, "AA", got, 2);
    check_isi("2", path, 0);
}

static void case_back(int path)
{
    unsigned long got[3];

    start_case(1);
    got[0] = run_x(path);
    sw_slbie(VSID_B);
    got[1] = run_x(path);
    sw_slbie(VSID_A);
    got[2] = run_x(path);
    report("3", path, "ABA", got, 3);
    check_isi("3", path, 0);
}

static void case_isi(int path)
{
    unsigned long got[2];

    start_case(1);
    /* what the handler installs: X means C */
    jc_isi_state[2] = pte_addr(HTAB1, VSID_A, EA_X);
    jc_isi_state[3] = pte0_of(VSID_A, EA_X);
    jc_isi_state[4] = PAGE_C | PTE1_RC;

    got[0] = run_x(path);
    pte_drop(HTAB1, VSID_A, EA_X);
    tlb_ie(EA_X);
    asm volatile("eieio; tlbsync; ptesync" : : : "memory");
    got[1] = run_x(path);
    report("4", path, "AC", got, 2);
    check_isi("4", path, 1);
}

int main(void)
{
    int path;

    install_vector(VEC_SC, jc_sc_stub, jc_sc_stub_end);
    install_vector(VEC_ISI, jc_isi_stub, jc_isi_stub_end);
    put_code();

    for (path = 0; path < NR_PATHS; path++) {
        case_switch("1a", 1, SLBIE, path);
        case_switch("1b", 1, SLBIA, path);
        case_switch("1c", 0, SLOT0, path);
        case_switch("1d", 1, PTE, path);
        case_switch("1e", 1, SDR1, path);
        case_same(path);
        case_back(path);
        case_isi(path);
    }

    if (ok) {
        ml_printf("PASS: jmpcache\n");
    }

    return 0;
}
