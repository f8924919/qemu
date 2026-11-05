/*
 * QTest testcase for what a reset does to the U3 AGP bridge's 0x48
 *
 * A real PowerMac G5 boot ROM reads the address select register at config
 * offset 0x48 of the bridge and sets the bits of the windows it opens.
 * The generic PCI reset leaves the device-specific config space alone, so
 * the bits used to survive a reset, and the ROM met them already set on the
 * way back up, after which Mac OS X panicked in ATIRadeon9700::start().  A
 * reset has to bring back the value the bridge starts with, with an AGP card
 * behind it and without.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define MACHINE           "-machine powermac7_3 -cpu 970fx "
#define NO_CARD           MACHINE "-vga none"
#define CARD              MACHINE "-vga none " \
                          "-device ati-radeon9800,bus=pci.1,addr=0x10"

#define U3_AGP_CFG_ADDR   0xf0800000ULL
#define U3_AGP_CFG_DATA   0xf0c00000ULL
#define U3_AGP_DEV        11
#define U3_AGP_CFA(off)   ((1u << U3_AGP_DEV) | ((off) & 0xf8))

#define ADDRESS_SELECT    0x48
#define SELECT_START      0x01000000u
#define SELECT_OPENED     0x07000000u

static void agp_writel(QTestState *qts, uint32_t off, uint32_t val)
{
    qtest_writel(qts, U3_AGP_CFG_ADDR, bswap32(U3_AGP_CFA(off)));
    qtest_writel(qts, U3_AGP_CFG_DATA + (off & 4), bswap32(val));
}

static uint32_t agp_readl(QTestState *qts, uint32_t off)
{
    qtest_writel(qts, U3_AGP_CFG_ADDR, bswap32(U3_AGP_CFA(off)));
    return bswap32(qtest_readl(qts, U3_AGP_CFG_DATA + (off & 4)));
}

static void check_reset(const char *args)
{
    QTestState *qts = qtest_init(args);

    g_assert_cmphex(agp_readl(qts, ADDRESS_SELECT), ==, SELECT_START);
    agp_writel(qts, ADDRESS_SELECT, SELECT_OPENED);
    g_assert_cmphex(agp_readl(qts, ADDRESS_SELECT), ==, SELECT_OPENED);

    qtest_system_reset(qts);
    g_assert_cmphex(agp_readl(qts, ADDRESS_SELECT), ==, SELECT_START);

    qtest_quit(qts);
}

static void test_no_card(void)
{
    check_reset(NO_CARD);
}

static void test_card(void)
{
    if (!qtest_has_device("ati-radeon9800")) {
        g_test_skip("ati-radeon9800 is not built");
        return;
    }
    check_reset(CARD);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/u3-agp-reset/no-card", test_no_card);
    qtest_add_func("/u3-agp-reset/card", test_card);
    return g_test_run();
}
