/*
 * Hypervisor state on the 970.
 *
 * A real 970 that is not under an LPAR runs with MSR[HV] set: that is how
 * Linux's __cpu_preinit_ppc970() gets to run at all, and it is what lets it
 * undo the 32-byte dcbz mode that Apple's Open Firmware leaves enabled.
 * Without HV the preinit is skipped, the mode stays on and Linux dies
 * clearing pages.
 *
 * Two things have to hold for the guest to survive that state:
 *
 *   - MSR[HV] reads as 1, and stays 1.  The hypervisor facilities are
 *     strapped off on this part, so there is no partition to drop into and
 *     software cannot clear the bit.  Mac OS X builds MSR from 32-bit
 *     constants and returns through rfid, which would otherwise take HV
 *     away from it.
 *
 *   - SDR1 is still reachable from supervisor state.  QEMU turns SDR1 into
 *     a hypervisor resource once the core has an HV mode, and Open Firmware
 *     writes it with plain mtsdr1 while setting up the hash table.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <minilib.h>

#define SPR_SDR1 25

#define MSR_HV_BIT 60

#define stringify_(x) #x
#define stringify(x) stringify_(x)

#define mtspr(spr, val) \
    asm volatile("mtspr " stringify(spr) ",%0" : : "r"(val))

#define mfspr(spr)                                  \
    ({                                              \
        unsigned long val_;                         \
        asm volatile("mfspr %0," stringify(spr)     \
                     : "=r"(val_));                 \
        val_;                                       \
    })

static unsigned long get_msr(void)
{
    unsigned long msr;

    asm volatile("mfmsr %0" : "=r"(msr));
    return msr;
}

/* A hash table base that is aligned well enough for SDR1 to keep it. */
#define SDR1_PATTERN 0x0000000000400000UL

int main(void)
{
    unsigned long msr, sdr1;
    int ok = 1;

    /*
     * Read MSR before anything else touches it: a later mtmsr would hide a
     * missing HV by carrying the current value forward.
     */
    msr = get_msr();
    if (!((msr >> MSR_HV_BIT) & 1)) {
        ml_printf("FAIL: hvmode MSR[HV] is 0 (msr 0x%lx)\n", msr);
        ok = 0;
    }

    /*
     * Writing MSR without HV must not take it away.  This is what Mac OS X
     * does on its way back out of an exception.
     */
    asm volatile("mtmsrd %0" : : "r"(msr & ~(1UL << MSR_HV_BIT)));
    msr = get_msr();
    if (!((msr >> MSR_HV_BIT) & 1)) {
        ml_printf("FAIL: hvmode MSR[HV] cleared by mtmsrd (msr 0x%lx)\n", msr);
        ok = 0;
    }

    /* SDR1 has to answer from supervisor state. */
    mtspr(SPR_SDR1, SDR1_PATTERN);
    sdr1 = mfspr(SPR_SDR1);
    if (sdr1 != SDR1_PATTERN) {
        ml_printf("FAIL: hvmode SDR1 reads back 0x%lx, expected 0x%lx\n",
                  sdr1, SDR1_PATTERN);
        ok = 0;
    }

    if (ok) {
        ml_printf("PASS: hvmode\n");
    }

    return 0;
}
