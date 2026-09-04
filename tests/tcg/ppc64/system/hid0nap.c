/*
 * HID0 and the nap state on a 970.
 *
 * HID0 is an ordinary read/write register on the 970, and its power-save
 * bits sit in the upper word: nap is bit 9 of the 64-bit register.  Mac OS X
 * relies on both.  machine_idle() sets HID0[NAP], sets MSR[POW] with EE on,
 * and the exception entry that wakes it reads HID0 back to decide whether
 * the interrupted code was the idle loop.  If the write does not stick that
 * decision goes wrong and a runnable thread is left waiting for a CPU that
 * believes it has nothing to do.
 *
 * machine_idle() also sets POW briefly with EE off while it re-arms the
 * decrementer, and expects to keep running.  A core that halts there can
 * never be woken.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <minilib.h>

#include "hid0nap.h"

#define SPR_HID0        1008

#define HID0_NAP        (1UL << 54)

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
    unsigned long hid0, orig, spins;
    int ok = 1;

    orig = mfhid0();

    /* The write must stick, and a write without the bit must clear it. */
    mthid0(orig | HID0_NAP);
    hid0 = mfhid0();
    if (!(hid0 & HID0_NAP)) {
        ml_printf("FAIL: hid0nap HID0 is 0x%lx after setting nap\n", hid0);
        ok = 0;
    }
    mthid0(orig);
    hid0 = mfhid0();
    if (hid0 & HID0_NAP) {
        ml_printf("FAIL: hid0nap HID0 is 0x%lx after clearing nap\n", hid0);
        ok = 0;
    }

    /*
     * With nap selected but EE off, POW must be ignored.  This hangs, and
     * fails by timeout, on an implementation that puts the core away here.
     * Load the decrementer first: a pending, undelivered interrupt would
     * keep any implementation from stopping and hide a broken check.
     */
    asm volatile("mtdec %0" : : "r"(0x7fffffffUL));
    mthid0(orig | HID0_NAP);
    set_pow_with_ee_off();

    /* With nap selected and EE on, POW must really stop the core ... */
    spins = nap_until_decrementer();
    if (spins != 0) {
        ml_printf("FAIL: hid0nap core kept running for %lu spins with "
                  "MSR[POW] set and HID0[NAP] on\n", spins);
        ok = 0;
    }
    /* ... and the decrementer must be what brought it back. */
    if (nap_srr0 == 0) {
        ml_printf("FAIL: hid0nap decrementer handler did not run\n");
        ok = 0;
    }
    mthid0(orig);

    if (ok) {
        ml_printf("PASS: hid0nap\n");
    }

    return 0;
}
