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
#include "hw/pci/pci_regs.h"

#define U3_HT_SELF_BASE   0xf8070000ULL
#define U3_HT_CFG_BASE    0xf2000000ULL

/*
 * The AGP domain is a single 32MB window: I/O at +0, CONFIG_ADDR at
 * +0x800000 and CONFIG_DATA at +0xc00000.  Derive all three from one
 * base so that they cannot drift apart: Linux hardcodes the config
 * registers at 0xf0000000 + 0x800000/0xc00000 while Mac OS X computes
 * them from the I/O window base in the device tree, so moving only one
 * of them silently breaks one of the two guests.
 */
#define U3_AGP_BASE       0xf0000000ULL
#define U3_AGP_IO_BASE    (U3_AGP_BASE)
#define U3_AGP_CFG_ADDR   (U3_AGP_BASE + 0x800000)
#define U3_AGP_CFG_DATA   (U3_AGP_BASE + 0xc00000)

/* The AGP host bridge sits at dev 11; UniNorth encodes bus 0 as 1 << dev */
#define U3_AGP_HOST_DEVFN (11 << 3)
#define U3_AGP_CFA(dev, off)  ((1u << (dev)) | (off))
#define U3_AGP_IDS        ((0x004b << 16) | 0x106b)

/*
 * PCI config offsets and values used by the AGP capability test.  Mac OS X's
 * AppleMacRiscAGP::configure() starts with
 *   findPCICapability(getBridgeSpace(), kIOPCIAGPCapability, ...)
 * and returns false without logging anything when it comes up empty, so a
 * host bridge without a capability list makes the whole AGP domain fail to
 * start: no nub is published for anything below it, the VGA included.
 */
#define PCI_CFG_STATUS       0x04    /* dword: command | status << 16 */
#define PCI_STATUS_CAP_LIST  0x10
#define PCI_CFG_CAP_PTR      0x34
#define PCI_CAP_ID_AGP       0x02

/* fw_cfg lives at ISA port 0x510 of the AGP domain */
#define FW_CFG_CTL        (U3_AGP_BASE + 0x510)

/* IDs of the u3-ht host bridge itself (Apple CPC945 HT bridge) */
#define U3_HT_IDS         ((0x004a << 16) | 0x106b)

/*
 * pci-testdev (1b36:0005) plugged at slot 2 function 0
 * (slot 1 is taken by the default sungem NIC on the HT bus)
 */
#define TESTDEV_IDS       ((0x0005 << 16) | 0x1b36)
#define TESTDEV_DEVFN     (2 << 3)

/* K2 HT-PCI bridge at bus 0 / dev 3 / fn 0, macio behind it at dev 7 */
#define K2_DEVFN          (3 << 3)
#define K2_IDS            ((0x0045 << 16) | 0x106b)
#define MACIO_DEVFN       (7 << 3)
#define MACIO_IDS         ((0x0022 << 16) | 0x106b)

/* HT PCI memory window (identity-mapped, 16MB) */
#define U3_HT_MEM_BASE    0xfa000000ULL

/*
 * UniNorth/U3 register block: the version register is at offset 0 of
 * the /u3 (resp. /uni-n) reg window.  Mac OS X's AppleU3::start()
 * refuses to drive anything reporting <= 0x2f ("UniN version 0x%x not
 * supported"), so a machine that calls itself a U3 must report a
 * U3-class version.  mac99 stays on the UniNorth 1.0.8 value.
 */
#define UNIN_BASE         0xf8000000ULL
#define UNIN_VERSION_U3   0x30
#define UNIN_VERSION_10A  0x07

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

static void cfg_writel(QTestState *qts, uint64_t addr, uint32_t val)
{
    qtest_writel(qts, addr, bswap32(val));
}

static void cfg_writew(QTestState *qts, uint64_t addr, uint16_t val)
{
    qtest_writew(qts, addr, bswap16(val));
}

/*
 * Firmware does not run under qtest, so the K2 bridge comes up with all
 * bus number registers at 0.  Program the minimum needed for type 1
 * config cycles and downstream memory decoding by hand: bus numbers,
 * a memory window covering 0xfa000000-0xfaffffff and memory decoding.
 */
static void k2_program_bridge_windows(QTestState *qts)
{
    qtest_writeb(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x18), 0); /* primary */
    qtest_writeb(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x19), 1); /* secondary */
    qtest_writeb(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x1a), 1); /* subord. */
    cfg_writew(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x20), 0xfa00);
    cfg_writew(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x22), 0xfaf0);
}

static void k2_enable_bridge_decoding(QTestState *qts)
{
    cfg_writew(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 4), PCI_COMMAND_MEMORY);
}

static void k2_program_bridge(QTestState *qts)
{
    k2_program_bridge_windows(qts);
    k2_enable_bridge_decoding(qts);
}

static void test_self_window(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    /* Config offset 0: device/vendor ID of the host bridge itself */
    g_assert_cmphex(qtest_readl(qts, U3_HT_SELF_BASE), ==, U3_HT_IDS);

    /*
     * Region decode register = config offset 0x80.  Linux reads it at
     * cfg_addr + 0x80 in units of u32, i.e. byte offset 0x200.  Bits
     * i=4,8,9 (0x80000000 >> i) are the ones the Linux kernel comment
     * names as enabled on a real machine; i=10 is added here to
     * advertise the 0xfa000000 PCI memory window that backs the BARs
     * of the devices on the HT bus.
     */
    g_assert_cmphex(qtest_readl(qts, U3_HT_SELF_BASE + 0x200), ==,
                    0x08e00000);

    /*
     * Write path: PCI_INTERRUPT_LINE (offset 0x3c, byte address
     * 0x3c << 2 = 0xf0) is a writable config register.
     */
    qtest_writeb(qts, U3_HT_SELF_BASE + 0xf0, 0xa5);
    g_assert_cmphex(qtest_readb(qts, U3_HT_SELF_BASE + 0xf0), ==, 0xa5);

    /* The decode register is read-only */
    qtest_writel(qts, U3_HT_SELF_BASE + 0x200, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, U3_HT_SELF_BASE + 0x200), ==,
                    0x08e00000);

    qtest_quit(qts);
}

static void test_cfa0(void)
{
    /*
     * Root buses are named in creation order; the HT bridge is created
     * first (so that the AGP bus stays the default), making it pci.0.
     */
    QTestState *qts = qtest_init("-machine powermac7_3 "
                                 "-device pci-testdev,bus=pci.0,addr=2");

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

static void test_cfa1_bridge(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    /* The K2 bridge itself: type 0 access at bus 0 / devfn 0x18 */
    g_assert_cmphex(cfg_readl(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0)), ==,
                    K2_IDS);

    k2_program_bridge(qts);

    /* CFA1 bus field [23:16]: macio behind the K2 bridge on bus 1 */
    g_assert_cmphex(cfg_readl(qts, U3_HT_CFG_BASE + CFA1(1, MACIO_DEVFN, 0)),
                    ==, MACIO_IDS);

    qtest_quit(qts);
}

/*
 * Memory decoding on the K2 bridge is off until someone turns it on.  The
 * board comes up with COMMAND clear and nothing in QEMU puts the bit back:
 * arranging the windows is the firmware's job, or the guest's if it runs
 * without one.  Check that by driving the two halves separately.
 */
static void test_k2_decoding_off_until_enabled(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3 "
                                 "-device pci-testdev,bus=ht.1,addr=8,"
                                 "membar=4096,membar-backed=true");
    uint64_t bar = U3_HT_MEM_BASE + 0x100000;
    uint32_t devfn = 8 << 3;

    /* Everything except the bridge's own COMMAND. */
    k2_program_bridge_windows(qts);
    cfg_writel(qts, U3_HT_CFG_BASE + CFA1(1, devfn, 0x18), (uint32_t)bar);
    cfg_writel(qts, U3_HT_CFG_BASE + CFA1(1, devfn, 0x1c), 0);
    cfg_writew(qts, U3_HT_CFG_BASE + CFA1(1, devfn, 4), PCI_COMMAND_MEMORY);

    /*
     * The access must not reach the device.  Nothing claims the address
     * while the window is closed, so the write is dropped and the read
     * comes back as whatever an unclaimed read gives (0 today); what
     * matters is only that it is not what we wrote.
     */
    qtest_writel(qts, bar + 8, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, bar + 8), !=, 0x12345678);

    k2_enable_bridge_decoding(qts);

    qtest_writel(qts, bar + 8, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, bar + 8), ==, 0x12345678);

    qtest_quit(qts);
}

static void test_ht_mem_window(void)
{
    /* pci-testdev behind the K2 bridge, on its secondary bus "ht.1" */
    QTestState *qts = qtest_init("-machine powermac7_3 "
                                 "-device pci-testdev,bus=ht.1,addr=8,"
                                 "membar=4096,membar-backed=true");
    uint64_t bar = U3_HT_MEM_BASE + 0x100000; /* inside the 16MB window */
    uint32_t devfn = 8 << 3;

    k2_program_bridge(qts);

    /*
     * Program the RAM-backed 64-bit BAR2 (config offset 0x18/0x1c) of
     * the pci-testdev behind the K2 bridge via CFA1 and enable memory
     * decoding, then check that the BAR is reachable from the CPU
     * through the K2 window inside the 0xfa000000 host window
     * (identity-mapped: PCI address == system address).
     */
    cfg_writel(qts, U3_HT_CFG_BASE + CFA1(1, devfn, 0x18), (uint32_t)bar);
    cfg_writel(qts, U3_HT_CFG_BASE + CFA1(1, devfn, 0x1c), 0);
    cfg_writew(qts, U3_HT_CFG_BASE + CFA1(1, devfn, 4), 0x0002);

    qtest_writel(qts, bar + 8, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, bar + 8), ==, 0x12345678);

    qtest_quit(qts);
}

static void test_agp_io_mapped(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    /*
     * A mapped PCI I/O region with no BAR behind it reads as all-ones
     * (unassigned_io_ops), while unmapped system memory reads as 0,
     * so this distinguishes "window mapped" from "nothing there".
     * Read above the legacy port range so that the probe cannot be
     * answered by a device sitting at a low ISA port.
     */
    g_assert_cmphex(qtest_readl(qts, U3_AGP_IO_BASE + 0x1000), ==, 0xffffffff);

    qtest_quit(qts);
}

/*
 * Mac OS X derives the AGP config registers from the I/O window base in
 * the device tree, so a config round trip through them is what ties the
 * window position to the registers.  Nothing else in the test suite
 * covers the AGP config path.
 */
static void test_agp_config(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");
    uint32_t addr = U3_AGP_CFA(11, 0);

    /* The CONFIG_ADDR register must read back exactly what was written */
    qtest_writel(qts, U3_AGP_CFG_ADDR, bswap32(addr));
    g_assert_cmphex(bswap32(qtest_readl(qts, U3_AGP_CFG_ADDR)), ==, addr);

    /* ... and CONFIG_DATA must then return the host bridge's own IDs */
    g_assert_cmphex(bswap32(qtest_readl(qts, U3_AGP_CFG_DATA)), ==,
                    U3_AGP_IDS);

    qtest_quit(qts);
}

/*
 * fw_cfg sits inside the AGP I/O window; it must keep priority over it,
 * otherwise OpenBIOS cannot read the machine id and the guest never
 * boots.  Note this only catches a regression, not a missing overlap:
 * fw_cfg is mapped after the window, so at equal priority it would still
 * win by ordering.
 */
static void test_fw_cfg_not_shadowed(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    /* An unbacked I/O port reads all-ones; fw_cfg must not do that */
    g_assert_cmphex(qtest_readw(qts, FW_CFG_CTL), !=, 0xffff);

    qtest_quit(qts);
}

/*
 * Read one config dword of an AGP-domain device through CONFIG_ADDR/DATA.
 *
 * In the CFA0 encoding the register offset is split: CONFIG_ADDR carries
 * bits 3-7 while bits 0-2 come from the address used to touch CONFIG_DATA
 * (see unin_get_config_reg()).  Putting the whole offset in CONFIG_ADDR
 * silently reads the dword at the start of the same 8-byte group instead.
 */
static uint32_t agp_cfg_readl(QTestState *qts, int dev, uint32_t off)
{
    qtest_writel(qts, U3_AGP_CFG_ADDR, bswap32(U3_AGP_CFA(dev, off & 0xf8)));
    return bswap32(qtest_readl(qts, U3_AGP_CFG_DATA + (off & 4)));
}

/*
 * Walk the capability chain of the AGP host bridge and return the offset of
 * the given capability, or 0 when it is absent.  The walk is bounded so that
 * a malformed chain fails the test instead of hanging it.
 */
static uint32_t agp_find_capability(QTestState *qts, int dev, uint8_t id)
{
    uint32_t off = agp_cfg_readl(qts, dev, PCI_CFG_CAP_PTR) & 0xfc;
    int guard;

    for (guard = 0; off >= 0x40 && guard < 48; guard++) {
        uint32_t cap = agp_cfg_readl(qts, dev, off);

        if ((cap & 0xff) == id) {
            return off;
        }
        off = (cap >> 8) & 0xfc;
    }
    g_assert_cmpint(guard, <, 48);
    return 0;
}

/*
 * The U3 AGP host bridge must advertise an AGP capability.  Without it
 * AppleMacRiscAGP::configure() bails out and Mac OS X never brings up the
 * AGP domain, which is where powermac7_3 puts the VGA adapter.
 */
static void test_agp_capability(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");
    uint32_t status = agp_cfg_readl(qts, 11, PCI_CFG_STATUS) >> 16;
    uint32_t cap;

    g_assert_cmphex(status & PCI_STATUS_CAP_LIST, ==, PCI_STATUS_CAP_LIST);

    cap = agp_find_capability(qts, 11, PCI_CAP_ID_AGP);
    g_assert_cmphex(cap, !=, 0);

    /*
     * Mac OS X only checks that the capability exists, but leaving the
     * revision at zero would advertise a nonexistent AGP revision.
     */
    g_assert_cmphex((agp_cfg_readl(qts, 11, cap) >> 20) & 0xf, !=, 0);

    qtest_quit(qts);
}

/*
 * The capability belongs to the U3 AGP bridge, so it follows the CPU rather
 * than the machine name: mac_newworld picks the AGP host bridge on
 * PPC_FLAGS_INPUT_970, not on the u3-ht machine property, and mac99 defaults
 * to a 970 on a ppc64 build.  Asking for a G4 selects the UniNorth AGP
 * bridge instead, which is the configuration the Tiger regression gate boots
 * (mac99,via=pmu -cpu g4).  Pin it as untouched by the U3 change; whether
 * the UniNorth bridge should grow a capability of its own is a separate
 * question, and no guest has been seen to need one.
 */
static void test_mac99_g4_no_agp_capability(void)
{
    QTestState *qts = qtest_init("-machine mac99 -cpu g4");
    uint32_t status;

    /*
     * The UniNorth AGP bridge uses the plain pci_host_data_le_ops, so its
     * CONFIG_ADDR takes the ordinary x86 encoding rather than the CFA0 slot
     * bitmap the U3 bridge decodes in unin_get_config_reg().
     */
    qtest_writel(qts, U3_AGP_CFG_ADDR, bswap32(0x80000000u | (11 << 11)));
    g_assert_cmphex(bswap32(qtest_readl(qts, U3_AGP_CFG_DATA)), ==,
                    (0x0020 << 16) | 0x106b);

    qtest_writel(qts, U3_AGP_CFG_ADDR,
                 bswap32(0x80000000u | (11 << 11) | PCI_CFG_STATUS));
    status = bswap32(qtest_readl(qts, U3_AGP_CFG_DATA)) >> 16;
    g_assert_cmphex(status & PCI_STATUS_CAP_LIST, ==, 0);

    qtest_quit(qts);
}

static void test_unin_version(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    /*
     * The version register is big-endian, so qtest_readl() returns it
     * as-is.  Anything <= 0x2f makes Mac OS X's AppleU3::start() bail
     * out, which leaves an IOSimpleLock held and panics the config
     * thread with "thread_invoke: preemption_level 1".
     */
    g_assert_cmphex(qtest_readl(qts, UNIN_BASE), ==, UNIN_VERSION_U3);

    qtest_quit(qts);
}

static void test_mac99_unin_version(void)
{
    QTestState *qts = qtest_init("-machine mac99");

    /* mac99 is a UniNorth machine and must keep the UniNorth version */
    g_assert_cmphex(qtest_readl(qts, UNIN_BASE), ==, UNIN_VERSION_10A);

    qtest_quit(qts);
}

static void test_mac99_unmapped(void)
{
    QTestState *qts = qtest_init("-machine mac99 -cpu 970fx");

    /* mac99 must not grow the HT bridge: nothing answers where it lives */
    g_assert_cmphex(qtest_readl(qts, U3_HT_SELF_BASE), ==, 0);

    /*
     * The AGP I/O window moved to the AGP domain base on mac99 + 970fx
     * too, so nothing answers at the old 0xf2000000 address any more.
     */
    g_assert_cmphex(qtest_readl(qts, U3_HT_CFG_BASE), ==, 0);

    /* ... and the window is where powermac7_3 has it */
    g_assert_cmphex(qtest_readl(qts, U3_AGP_IO_BASE + 0x1000), ==, 0xffffffff);

    /* The HT PCI memory window must not appear on mac99 */
    g_assert_cmphex(qtest_readl(qts, U3_HT_MEM_BASE), ==, 0);

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
    qtest_add_func("/u3-ht/cfa1-bridge", test_cfa1_bridge);
    qtest_add_func("/u3-ht/k2-decoding-off",
                   test_k2_decoding_off_until_enabled);
    qtest_add_func("/u3-ht/ht-mem-window", test_ht_mem_window);
    qtest_add_func("/u3-ht/agp-io-mapped", test_agp_io_mapped);
    qtest_add_func("/u3-ht/agp-config", test_agp_config);
    qtest_add_func("/u3-ht/agp-capability", test_agp_capability);
    qtest_add_func("/u3-ht/mac99-g4-no-agp-capability",
                   test_mac99_g4_no_agp_capability);
    qtest_add_func("/u3-ht/fw-cfg-not-shadowed", test_fw_cfg_not_shadowed);
    qtest_add_func("/u3-ht/unin-version", test_unin_version);
    qtest_add_func("/u3-ht/mac99-unin-version", test_mac99_unin_version);
    qtest_add_func("/u3-ht/mac99-unmapped", test_mac99_unmapped);

    return g_test_run();
}
