/*
 * HSPRG0 and HSPRG1 on the 970.
 *
 * The 970 registers SPR 304/305 through register_970_lpar_sprs().  On this
 * core the hypervisor features are strapped off, so they are plain scratch
 * registers usable from supervisor state -- which is what Mac OS X does with
 * them when it saves and restores GPRs around an exception.  Without them
 * the writes are dropped and the reads return garbage, and the guest
 * corrupts its own registers on the way out of an interrupt.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <minilib.h>

#define SPR_HSPRG0 304
#define SPR_HSPRG1 305

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

/* Distinct in every byte, and distinct between the two registers. */
#define PATTERN0 0x0123456789abcdefUL
#define PATTERN1 0xfedcba9876543210UL

static int check(const char *name, unsigned long got, unsigned long want)
{
    if (got == want) {
        return 1;
    }

    ml_printf("FAIL: hsprg %s reads back 0x%lx, expected 0x%lx\n",
              name, got, want);
    return 0;
}

int main(void)
{
    unsigned long val0, val1;
    int ok = 1;

    mtspr(SPR_HSPRG0, PATTERN0);
    mtspr(SPR_HSPRG1, PATTERN1);

    val0 = mfspr(SPR_HSPRG0);
    val1 = mfspr(SPR_HSPRG1);

    ok &= check("HSPRG0", val0, PATTERN0);
    ok &= check("HSPRG1", val1, PATTERN1);

    /* A single shared register would pass the checks above in isolation. */
    mtspr(SPR_HSPRG0, 0UL);
    val1 = mfspr(SPR_HSPRG1);
    ok &= check("HSPRG1 after writing HSPRG0", val1, PATTERN1);

    if (ok) {
        ml_printf("PASS: hsprg\n");
    }

    return 0;
}
