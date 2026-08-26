/*
 * slbmte RB decoding on the 970.
 *
 * RB holds the ESID in bits 0:35, the valid bit in 36, a reserved field in
 * 37:51 and the SLB entry index in 52:63.  Hardware ignores the reserved
 * field and only looks at as many index bits as it has entries, so software
 * is free to hand it an unmasked effective address.  Mac OS X does exactly
 * that: handlePF() builds RB straight from the faulting address, leaving
 * junk in both the reserved field and the top of the index.  Rejecting that
 * as an illegal instruction kills the guest right after it mounts the root
 * filesystem.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <minilib.h>

#define SLB_ESID_SHIFT 28
#define SLB_ESID_V 0x08000000UL /* bit 36 */
#define SLB_RESERVED 0x07fff000UL /* bits 37:51, ignored by hardware */
#define SLB_INDEX_HI 0x00000fc0UL /* index bits above the 970's 64 entries */

#define TEST_ESID 0x123UL
#define TEST_SLOT 5UL

static inline void slbmte(unsigned long rs, unsigned long rb)
{
    asm volatile("slbmte %0,%1" : : "r"(rs), "r"(rb) : "memory");
}

static inline unsigned long slbmfee(unsigned long slot)
{
    unsigned long val;

    asm volatile("slbmfee %0,%1" : "=r"(val) : "r"(slot));
    return val;
}

static inline void slbia(void)
{
    asm volatile("slbia" : : : "memory");
}

static inline unsigned long slbmfev(unsigned long slot)
{
    unsigned long val;

    asm volatile("slbmfev %0,%1" : "=r"(val) : "r"(slot));
    return val;
}

static int check(const char *what, unsigned long got, unsigned long want)
{
    if (got == want) {
        return 1;
    }

    ml_printf("FAIL: slbmte %s is 0x%lx, expected 0x%lx\n",
              what, got, want);
    return 0;
}

int main(void)
{
    unsigned long rb = (TEST_ESID << SLB_ESID_SHIFT) | SLB_ESID_V | TEST_SLOT;
    unsigned long rs = (0x400UL + TEST_SLOT) << 12;
    unsigned long esid_clean, vsid_clean;
    unsigned long esid_dirty, vsid_dirty;
    int ok = 1;

    /*
     * Establish what a well formed RB produces, so that the comparison
     * below cannot pass just because both attempts did nothing.
     */
    slbmte(rs, rb);
    esid_clean = slbmfee(TEST_SLOT);
    vsid_clean = slbmfev(TEST_SLOT);

    ok &= check("ESID", esid_clean >> SLB_ESID_SHIFT, TEST_ESID);
    ok &= check("valid bit", esid_clean & SLB_ESID_V, SLB_ESID_V);

    /*
     * Empty the SLB again, so that the write below has to do the work
     * itself: leaving the entry in place would let an implementation that
     * quietly drops the dirtied form pass the comparison.
     */
    slbia();
    if (slbmfee(TEST_SLOT) & SLB_ESID_V) {
        ml_printf("FAIL: slbmte slbia left entry %ld valid\n", TEST_SLOT);
        ok = 0;
    }

    /*
     * Now the form Mac OS X actually uses.  Reaching the next instruction
     * at all is half the test: an illegal instruction here lands in the
     * catch-all handler, which reports the exception and stops.
     */
    slbmte(rs, rb | SLB_RESERVED | SLB_INDEX_HI);
    esid_dirty = slbmfee(TEST_SLOT);
    vsid_dirty = slbmfev(TEST_SLOT);

    ok &= check("ESID with reserved bits set", esid_dirty, esid_clean);
    ok &= check("VSID with reserved bits set", vsid_dirty, vsid_clean);

    if (ok) {
        ml_printf("PASS: slbmte\n");
    }

    return 0;
}
