/*
 * Access widths on the macio NVRAM.
 *
 * The NVRAM behind macio is a byte-wide part on a bus that spaces the bytes
 * out, and nothing on that bus limits how wide a transfer may be.  Mac OS X
 * copies it out with bcopy(), which on a 64-bit kernel moves 8 bytes at a
 * time.  The device used to accept at most 4-byte accesses, so those loads
 * never reached it: the memory core answered with zeroes, the kernel's
 * shadow of the NVRAM was blank, and its partition walk spun forever on a
 * zero-length partition.
 *
 * This has to be a guest load.  A qtest "readq" goes through
 * address_space_read(), which quietly splits the access down to the
 * device's maximum width and so never sees the problem; only the CPU's
 * own load/store path dispatches the full 8 bytes to the device.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <minilib.h>

/* Where mac99 and its derivatives map the NVRAM (hw/ppc/mac_newworld.c). */
#define NVRAM_BASE      0xfff04000UL
#define NVRAM_IT_SHIFT  1

static void st8(unsigned long addr, unsigned char val)
{
    asm volatile("stb %0, 0(%1); eieio" : : "r"(val), "r"(addr) : "memory");
}

static unsigned long ld64(unsigned long addr)
{
    unsigned long val;

    asm volatile("ld %0, 0(%1); eieio" : "=r"(val) : "r"(addr) : "memory");
    return val;
}

int main(void)
{
    unsigned long val;
    int i;

    /*
     * Byte stores have always worked; use them to plant a pattern.  With
     * it_shift = 1 every NVRAM byte appears twice on the bus, so 8 bus
     * bytes cover 4 NVRAM bytes and each shows up as a repeated pair.
     */
    for (i = 0; i < 4; i++) {
        st8(NVRAM_BASE + (i << NVRAM_IT_SHIFT), 0x10 + i);
    }

    val = ld64(NVRAM_BASE);
    if (val != 0x1010111112121313UL) {
        ml_printf("FAIL: nvram 8-byte load returned 0x%lx, "
                  "expected 0x1010111112121313\n", val);
        return 0;
    }

    ml_printf("PASS: nvram\n");
    return 0;
}
