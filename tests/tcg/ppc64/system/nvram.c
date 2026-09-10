/*
 * The macio NVRAM as the NewWorld guests drive it.
 *
 * Two things are checked here, and they used to be one.
 *
 * Access width: the NVRAM behind macio is a byte-wide part on a bus that
 * used to space the bytes out, and nothing on that bus limits how wide a
 * transfer may be.  Mac OS X copies it out with bcopy(), which on a 64-bit
 * kernel moves 8 bytes at a time.  The device used to accept at most
 * 4-byte accesses, so those loads never reached it: the memory core
 * answered with zeroes, the kernel's shadow of the NVRAM was blank, and its
 * partition walk spun forever on a zero-length partition.
 *
 * This has to be a guest load.  A qtest "readq" goes through
 * address_space_read(), which quietly splits the access down to the
 * device's maximum width and so never sees the problem; only the CPU's
 * own load/store path dispatches the full 8 bytes to the device.
 *
 * Command interface: the part is a flash chip, and a store on its own does
 * not change what it holds.  Both Mac OS X and Linux erase a bank and then
 * program it a byte at a time with the Sharp/Micron command set, so a plain
 * store has to be ignored and the command sequence has to work.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <minilib.h>

/* Where mac99 and its derivatives map the NVRAM (hw/ppc/mac_newworld.c). */
#define NVRAM_BASE      0xfff04000UL
#define NVRAM_IT_SHIFT  0

/* Sharp/Micron command set, as Linux names it in powermac/nvram.c */
#define SM_ERASE_SETUP      0x20
#define SM_ERASE_CONFIRM    0xd0
#define SM_WRITE_SETUP      0x40
#define SM_READ_STATUS      0x70
#define SM_CLEAR_STATUS     0x50
#define SM_RESET            0xff
#define SM_STATUS_DONE      0x80
#define SM_STATUS_ERASE_ERR 0x20

/* A byte that means nothing to the part, so it can only be data */
#define NOT_A_COMMAND       0xa5

static void st8(unsigned long addr, unsigned char val)
{
    asm volatile("stb %0, 0(%1); eieio" : : "r"(val), "r"(addr) : "memory");
}

static unsigned char ld8(unsigned long addr)
{
    unsigned char val;

    asm volatile("lbz %0, 0(%1); eieio" : "=r"(val) : "r"(addr) : "memory");
    return val;
}

static unsigned long ld64(unsigned long addr)
{
    unsigned long val;

    asm volatile("ld %0, 0(%1); eieio" : "=r"(val) : "r"(addr) : "memory");
    return val;
}

static unsigned long byte_addr(int i)
{
    return NVRAM_BASE + (i << NVRAM_IT_SHIFT);
}

int main(void)
{
    unsigned char was, status;
    unsigned long val;
    int i;

    /* Whatever mode the part came up in, start from the array */
    st8(NVRAM_BASE, SM_RESET);

    /* A store outside a command sequence leaves the contents alone */
    was = ld8(NVRAM_BASE);
    st8(NVRAM_BASE, NOT_A_COMMAND);
    if (ld8(NVRAM_BASE) != was) {
        ml_printf("FAIL: a bare store changed the nvram (0x%x -> 0x%x)\n",
                  was, ld8(NVRAM_BASE));
        return 0;
    }

    /*
     * Erase setup on its own does not erase anything.  A bank holds text,
     * and 0x20 is a space, so a part that erased on the setup cycle would
     * lose a bank to a variable that happens to contain one.
     */
    val = ld64(NVRAM_BASE);
    st8(NVRAM_BASE, SM_ERASE_SETUP);
    st8(NVRAM_BASE, 0x00);              /* eaten as an invalid confirm */
    status = ld8(NVRAM_BASE);
    if (!(status & SM_STATUS_ERASE_ERR)) {
        ml_printf("FAIL: a bad erase sequence left status 0x%x\n", status);
        return 0;
    }
    st8(NVRAM_BASE, SM_CLEAR_STATUS);
    st8(NVRAM_BASE, SM_RESET);
    if (ld64(NVRAM_BASE) != val) {
        ml_printf("FAIL: erase setup without confirm erased the bank "
                  "(0x%lx -> 0x%lx)\n", val, ld64(NVRAM_BASE));
        return 0;
    }

    /* Erase the bank this address falls in */
    st8(NVRAM_BASE, SM_ERASE_SETUP);
    st8(NVRAM_BASE, SM_ERASE_CONFIRM);
    status = ld8(NVRAM_BASE);
    if (!(status & SM_STATUS_DONE)) {
        ml_printf("FAIL: erase status 0x%x has no done bit\n", status);
        return 0;
    }
    st8(NVRAM_BASE, SM_CLEAR_STATUS);
    st8(NVRAM_BASE, SM_RESET);

    val = ld64(NVRAM_BASE);
    if (val != 0xffffffffffffffffUL) {
        ml_printf("FAIL: after erase the nvram reads 0x%lx, "
                  "expected all ones\n", val);
        return 0;
    }

    /* Program a pattern one byte at a time */
    for (i = 0; i < 8; i++) {
        st8(byte_addr(i), SM_WRITE_SETUP);
        st8(byte_addr(i), 0x10 + i);
        status = ld8(byte_addr(i));
        if (!(status & SM_STATUS_DONE)) {
            ml_printf("FAIL: program status 0x%x at byte %d has no done bit\n",
                      status, i);
            return 0;
        }
    }
    st8(NVRAM_BASE, SM_CLEAR_STATUS);
    st8(NVRAM_BASE, SM_RESET);

    /*
     * The bytes are contiguous on the bus, so 8 bus bytes cover 8 NVRAM
     * bytes, and the load has to reach the device in one piece.
     */
    val = ld64(NVRAM_BASE);
    if (val != 0x1011121314151617UL) {
        ml_printf("FAIL: nvram 8-byte load returned 0x%lx, "
                  "expected 0x1011121314151617\n", val);
        return 0;
    }

    ml_printf("PASS: nvram\n");
    return 0;
}
