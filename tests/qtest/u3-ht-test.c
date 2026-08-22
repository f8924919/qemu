/*
 * QTest testcase for the U3 HyperTransport PCI host bridge (powermac7_3)
 *
 * Verifies the config access layout expected by the Linux u3-ht support
 * code (arch/powerpc/platforms/powermac/pci.c):
 *  - self-register window at 0xf8070000: word-indexed (offset << 2),
 *    big-endian access to the host bridge's own config space; the region
 *    decode register is config offset 0x80 (byte offset 0x200).
 *  - external config window at 0xf2000000 (the address used by the real
 *    machine): flat memory-mapped U3_HT_CFA0/CFA1 encoding, little-endian.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define U3_HT_SELF_BASE   0xf8070000ULL
#define U3_HT_CFG_BASE    0xf2000000ULL

/* AGP (ISA) PCI I/O window, moved out of the way of the config window */
#define U3_AGP_IO_BASE    0xf6000000ULL

/* IDs of the u3-ht host bridge itself (Apple CPC945 HT bridge) */
#define U3_HT_IDS         ((0x004a << 16) | 0x106b)

/* pci-testdev (1b36:0005) plugged at slot 1 function 0 */
#define TESTDEV_IDS       ((0x0005 << 16) | 0x1b36)
#define TESTDEV_DEVFN     (1 << 3)

/* U3_HT_CFA0(devfn, off) = (devfn << 8) | off */
#define CFA0(devfn, off)  (((devfn) << 8) | (off))
/* U3_HT_CFA1(bus, devfn, off) = CFA0(devfn, off) + (bus << 16) + 0x01000000 */
#define CFA1(bus, devfn, off)  (CFA0(devfn, off) + ((bus) << 16) + 0x01000000)

/*
 * The self-register window is a big-endian device, so qtest_readl()
 * (which returns what a big-endian guest load sees) yields the value
 * as-is.  The external config window is little-endian, so the value
 * arrives byte-swapped and must be swapped back.
 */
static uint32_t cfg_readl(QTestState *qts, uint64_t addr)
{
    return bswap32(qtest_readl(qts, addr));
}

static void test_self_window(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    /* Config offset 0: device/vendor ID of the host bridge itself */
    g_assert_cmphex(qtest_readl(qts, U3_HT_SELF_BASE), ==, U3_HT_IDS);

    /*
     * Region decode register = config offset 0x80.  Linux reads it at
     * cfg_addr + 0x80 in units of u32, i.e. byte offset 0x200.  Fixed
     * value mimics the real machine: bits i=4,8,9 (0x80000000 >> i).
     */
    g_assert_cmphex(qtest_readl(qts, U3_HT_SELF_BASE + 0x200), ==,
                    0x08c00000);

    /*
     * Write path: PCI_INTERRUPT_LINE (offset 0x3c, byte address
     * 0x3c << 2 = 0xf0) is a writable config register.
     */
    qtest_writeb(qts, U3_HT_SELF_BASE + 0xf0, 0xa5);
    g_assert_cmphex(qtest_readb(qts, U3_HT_SELF_BASE + 0xf0), ==, 0xa5);

    /* The decode register is read-only */
    qtest_writel(qts, U3_HT_SELF_BASE + 0x200, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, U3_HT_SELF_BASE + 0x200), ==,
                    0x08c00000);

    qtest_quit(qts);
}

static void test_cfa0(void)
{
    /*
     * Root buses are named in creation order; the HT bridge is created
     * first (so that the AGP bus stays the default), making it pci.0.
     */
    QTestState *qts = qtest_init("-machine powermac7_3 "
                                 "-device pci-testdev,bus=pci.0,addr=1");

    /* Type 0 access to bus 0 via the external window (little-endian) */
    g_assert_cmphex(cfg_readl(qts, U3_HT_CFG_BASE + CFA0(TESTDEV_DEVFN, 0)),
                    ==, TESTDEV_IDS);

    qtest_quit(qts);
}

static void test_cfa1_master_abort(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    /* Type 1 access to a bus that does not exist: expect all-ones */
    g_assert_cmphex(qtest_readl(qts, U3_HT_CFG_BASE + CFA1(2, 0, 0)), ==,
                    0xffffffff);

    qtest_quit(qts);
}

static void test_agp_io_mapped(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    /*
     * The AGP ISA I/O window must follow the machine to 0xf6000000.
     * A mapped PCI I/O region with no BAR behind it reads as all-ones
     * (unassigned_io_ops), while unmapped system memory reads as 0,
     * so this distinguishes "window mapped" from "nothing there".
     */
    g_assert_cmphex(qtest_readl(qts, U3_AGP_IO_BASE), ==, 0xffffffff);

    qtest_quit(qts);
}

static void test_mac99_unmapped(void)
{
    QTestState *qts = qtest_init("-machine mac99 -cpu 970fx");

    /*
     * mac99 must not grow the HT bridge.  Only the self-register window
     * can be checked as unmapped (reads of unassigned memory return 0):
     * the config window address 0xf2000000 hosts the AGP ISA I/O window
     * on mac99 + 970fx, where a read returns all-ones, not 0.
     */
    g_assert_cmphex(qtest_readl(qts, U3_HT_SELF_BASE), ==, 0);

    /* AGP ISA I/O stays at 0xf2000000 on mac99 + 970fx (mapped: all-1s) */
    g_assert_cmphex(qtest_readl(qts, U3_HT_CFG_BASE), ==, 0xffffffff);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine("powermac7_3")) {
        g_test_skip("powermac7_3 machine not available");
        return 0;
    }

    qtest_add_func("/u3-ht/self-window", test_self_window);
    qtest_add_func("/u3-ht/cfa0", test_cfa0);
    qtest_add_func("/u3-ht/cfa1-master-abort", test_cfa1_master_abort);
    qtest_add_func("/u3-ht/agp-io-mapped", test_agp_io_mapped);
    qtest_add_func("/u3-ht/mac99-unmapped", test_mac99_unmapped);

    return g_test_run();
}
