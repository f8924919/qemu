/*
 * HID0 bit 19 and interrupts in hypervisor state on a 970.
 *
 * On the 970, bit 19 of the 64-bit HID0 makes the timebase run from the
 * external clock according to NetBSD, which calls it HID0_64_EX_TBEN (not
 * checked against IBM's 970FX manual), and a PowerMac7,2 boot ROM sets it.
 * The same bit is HILE on POWER8: there it makes interrupts taken in
 * hypervisor state run little-endian.  The 970 has no little-endian mode at
 * all, so with the bit set an interrupt still has to arrive big-endian, in a
 * handler that can run and return.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <minilib.h>

#include "progfault.h"

#define SPR_HID0        1008

#define HID0_EXT_TB_EN  (1UL << 44)

#define VEC_PROGRAM     0x700UL

static unsigned long mfhid0(void)
{
    unsigned long val;

    asm volatile("mfspr %0, %1" : "=r"(val) : "i"(SPR_HID0));
    return val;
}

static void mthid0(unsigned long val)
{
    asm volatile("sync; mtspr %0, %1; isync" : : "i"(SPR_HID0), "r"(val));
}

int main(void)
{
    unsigned long orig, hid0, vec;
    int ok = 1;

    orig = mfhid0();
    mthid0(orig | HID0_EXT_TB_EN);
    hid0 = mfhid0();
    if (!(hid0 & HID0_EXT_TB_EN)) {
        ml_printf("FAIL: hid0exttb HID0 is 0x%lx after setting bit 19\n",
                  hid0);
        ok = 0;
    }

    /* The program interrupt is taken with MSR[HV] set (see hvmode.c) */
    vec = take_program_fault();
    mthid0(orig);
    if (vec != VEC_PROGRAM) {
        ml_printf("FAIL: hid0exttb undefined instruction took vector 0x%lx, "
                  "expected 0x%lx\n", vec, VEC_PROGRAM);
        ok = 0;
    } else if (prog_srr0 != prog_expect) {
        ml_printf("FAIL: hid0exttb program interrupt reports 0x%lx, "
                  "expected 0x%lx\n", prog_srr0, prog_expect);
        ok = 0;
    }

    if (ok) {
        ml_printf("PASS: hid0exttb\n");
    }

    return 0;
}
