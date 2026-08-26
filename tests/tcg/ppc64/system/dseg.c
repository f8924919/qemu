/*
 * DSISR on a Data Segment interrupt.
 *
 * A data segment interrupt means the effective address has no segment at
 * all, so there is nothing to report about the page tables and DSISR must
 * read as zero.  Leaving the previous contents in place makes the guest
 * believe it took an ordinary page fault: Mac OS X then goes looking for a
 * PTE instead of installing the missing SLB entry, and never reaches the
 * slbmte its fault handler was on its way to.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <minilib.h>

#include "mmufault.h"

#define SPR_SDR1 25

#define SLB_ESID_SHIFT 28
#define SLB_ESID_V 0x08000000UL

/* An empty hash table, so that a page walk misses in a predictable way. */
#define HTAB_BASE 0x00400000UL

#define TEST_EA  0x40000000UL
#define TEST_ESID (TEST_EA >> SLB_ESID_SHIFT)
#define TEST_SLOT 7UL

#define DSISR_SEED 0x12345678UL

#define VEC_DSI  0x300UL
#define VEC_DSEG 0x380UL


static void map_segment(void)
{
    unsigned long rb = (TEST_ESID << SLB_ESID_SHIFT) | SLB_ESID_V | TEST_SLOT;
    unsigned long rs = (0x400UL + TEST_SLOT) << 12;

    asm volatile("slbmte %0,%1" : : "r"(rs), "r"(rb) : "memory");
}

static void drop_all_segments(void)
{
    asm volatile("slbia" : : : "memory");
}

int main(void)
{
    unsigned long vec;
    int ok = 1;

    asm volatile("mtspr %0,%1" : : "i"(SPR_SDR1), "r"(HTAB_BASE));
    drop_all_segments();

    /*
     * With the segment present the access gets as far as the page tables
     * and comes back as a data storage interrupt.  This is what tells us
     * the segment machinery is working, so that the data segment
     * interrupt below really is caused by the missing segment.
     */
    map_segment();
    vec = take_data_fault(TEST_EA, DSISR_SEED);
    if (vec != VEC_DSI) {
        ml_printf("FAIL: dseg mapped access took vector 0x%lx,"
                  " expected 0x%lx\n", vec, VEC_DSI);
        ok = 0;
    }

    /* Now take the segment away and do it again. */
    drop_all_segments();
    vec = take_data_fault(TEST_EA, DSISR_SEED);
    if (vec != VEC_DSEG) {
        ml_printf("FAIL: dseg unmapped access took vector 0x%lx,"
                  " expected 0x%lx\n", vec, VEC_DSEG);
        ok = 0;
    } else {
        if (fault_dar != TEST_EA) {
            ml_printf("FAIL: dseg DAR is 0x%lx, expected 0x%lx\n",
                  fault_dar, TEST_EA);
            ok = 0;
        }
        if (fault_dsisr != 0) {
            ml_printf("FAIL: dseg DSISR is 0x%lx, expected 0 "
                  "(seeded with 0x%lx)\n",
                  fault_dsisr, DSISR_SEED);
            ok = 0;
        }
    }

    if (ok) {
        ml_printf("PASS: dseg\n");
    }

    return 0;
}
