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
#include "qobject/qdict.h"
#include "qobject/qlist.h"
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
#define U3_AGP_CAP_OFFSET    0x80    /* where the U3 bridge puts it */

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
/*
 * The real 7,3 carries a K2 KeyLargo (106b:0041), not the plain KeyLargo
 * (106b:0022) the mac99 machines have.  Linux picks the macio chip type
 * from the device tree's "compatible", and OpenBIOS in turn keys the node
 * off the PCI id, so the id is what splits the two machines apart.
 */
#define MACIO_IDS         ((0x0041 << 16) | 0x106b)

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
static void k2_program_bridge_windows_at(QTestState *qts, uint16_t base,
                                         uint16_t limit)
{
    qtest_writeb(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x18), 0); /* primary */
    qtest_writeb(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x19), 1); /* secondary */
    qtest_writeb(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x1a), 1); /* subord. */
    cfg_writew(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x20), base);
    cfg_writew(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x22), limit);
}

static void k2_program_bridge_windows(QTestState *qts)
{
    k2_program_bridge_windows_at(qts, 0xfa00, 0xfaf0);
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

static void agp_cfg_writel(QTestState *qts, int dev, uint32_t off,
                           uint32_t val)
{
    qtest_writel(qts, U3_AGP_CFG_ADDR, bswap32(U3_AGP_CFA(dev, off & 0xf8)));
    qtest_writel(qts, U3_AGP_CFG_DATA + (off & 4), bswap32(val));
}

/*
 * Registers of the U3 AGP bridge past the AGP capability: the GART
 * (0x8c-0x94, 0xa4) and the internal status (0x98).  A card that is an
 * AGP master is what makes them come alive; see check_agp_bridge_idle().
 */
#define U3_AGP_GART_BASE        0x8c
#define U3_AGP_AGP_BASE         0x90
#define U3_AGP_GART_CTRL        0x94
#define U3_AGP_INTERNAL_STATUS  0x98
#define U3_AGP_DUMMY_PAGE       0xa4
#define U3_AGP_GART_CTRL_INV    0x00000001
#define U3_AGP_GART_CTRL_EN     0x00000100

static const uint32_t u3_agp_gart_regs[] = {
    U3_AGP_GART_BASE, U3_AGP_AGP_BASE, U3_AGP_GART_CTRL,
    U3_AGP_INTERNAL_STATUS, U3_AGP_DUMMY_PAGE,
};

/*
 * With no AGP master below it, the U3 AGP bridge has to look exactly the
 * way it always has: an AGP 2.0 capability that supports no rate and
 * cannot be enabled, and nothing behind the GART registers.  That is the
 * honest description of a bridge whose only clients are a VGA adapter
 * and a USB controller, neither of which does AGP transfers.
 */
static void check_agp_bridge_idle(QTestState *qts)
{
    uint32_t status = agp_cfg_readl(qts, 11, PCI_CFG_STATUS) >> 16;
    uint32_t cap;
    int i;

    g_assert_cmphex(status & PCI_STATUS_CAP_LIST, ==, PCI_STATUS_CAP_LIST);

    cap = agp_find_capability(qts, 11, PCI_CAP_ID_AGP);
    g_assert_cmphex(cap, ==, U3_AGP_CAP_OFFSET);

    /* AGP 2.0: the highest revision that claims nothing more */
    g_assert_cmphex((agp_cfg_readl(qts, 11, cap) >> 16) & 0xff, ==, 0x20);
    g_assert_cmphex(agp_cfg_readl(qts, 11, cap + 4), ==, 0);
    g_assert_cmphex(agp_cfg_readl(qts, 11, cap + 8), ==, 0);

    /* The command register cannot turn AGP on */
    agp_cfg_writel(qts, 11, cap + 8, 0xffffffff);
    g_assert_cmphex(agp_cfg_readl(qts, 11, cap + 8), ==, 0);

    /*
     * Nothing is behind the GART registers: they read as zero and, like
     * the rest of the device-specific config space, merely hold what is
     * written - in particular an invalidate does not complete and the
     * internal status never reports idle.
     */
    for (i = 0; i < ARRAY_SIZE(u3_agp_gart_regs); i++) {
        uint32_t off = u3_agp_gart_regs[i];

        g_assert_cmphex(agp_cfg_readl(qts, 11, off), ==, 0);
        agp_cfg_writel(qts, 11, off, 0x5a5a0101);
        g_assert_cmphex(agp_cfg_readl(qts, 11, off), ==, 0x5a5a0101);
        agp_cfg_writel(qts, 11, off, 0);
    }

    /* kMacRISCPCIAddressSelect still names the 0x80000000 region only */
    g_assert_cmphex(agp_cfg_readl(qts, 11, 0x48), ==, 0x01000000);
}

/*
 * The U3 AGP host bridge must advertise an AGP capability.  Without it
 * AppleMacRiscAGP::configure() bails out and Mac OS X never brings up the
 * AGP domain, which is where powermac7_3 puts the VGA adapter.
 */
static void test_agp_capability(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    check_agp_bridge_idle(qts);
    qtest_quit(qts);
}

/* mac99 + 970fx shares the U3 AGP bridge and must not change either */
static void test_mac99_970_agp_capability(void)
{
    QTestState *qts = qtest_init("-machine mac99 -cpu 970fx");

    check_agp_bridge_idle(qts);
    qtest_quit(qts);
}

/* The AGP slot of the U3, where an AGP card goes */
#define U3_AGP_SLOT             0x10

/*
 * An AGP 2.0 card (the R350's own capability says 2.0) in the AGP slot
 * makes the bridge describe the port it now drives: AGP 3.0 on the U3,
 * with the rate bits read in AGP 2.0 mode (1x/2x/4x), and a GART the
 * guest can program.
 */
static void test_agp_master_r350(void)
{
    QTestState *qts;
    uint32_t cap;
    int i;

    if (!qtest_has_device("ati-radeon9800")) {
        g_test_skip("ati-radeon9800 not available");
        return;
    }
    qts = qtest_init("-machine powermac7_3 -vga none "
                     "-device ati-radeon9800,bus=pci.1,addr=0x10");

    /* The card itself is where the test put it, on the AGP bus */
    g_assert_cmphex(agp_cfg_readl(qts, U3_AGP_SLOT, 0), ==,
                    (0x4e48 << 16) | 0x1002);

    cap = agp_find_capability(qts, 11, PCI_CAP_ID_AGP);
    g_assert_cmphex(cap, ==, U3_AGP_CAP_OFFSET);
    g_assert_cmphex((agp_cfg_readl(qts, 11, cap) >> 16) & 0xff, ==, 0x30);
    /* RQ 32, sideband, 4x/2x/1x */
    g_assert_cmphex(agp_cfg_readl(qts, 11, cap + 4), ==, 0x1f000207);

    /* The command register takes the bits a driver enables AGP with */
    agp_cfg_writel(qts, 11, cap + 8, 0xffffffff);
    g_assert_cmphex(agp_cfg_readl(qts, 11, cap + 8), ==, 0xff000317);

    /* The GART registers hold what is written ... */
    agp_cfg_writel(qts, 11, U3_AGP_GART_BASE, 0x12345004);
    agp_cfg_writel(qts, 11, U3_AGP_AGP_BASE, 0x80000001);
    agp_cfg_writel(qts, 11, U3_AGP_DUMMY_PAGE, 0x00012345);
    g_assert_cmphex(agp_cfg_readl(qts, 11, U3_AGP_GART_BASE), ==, 0x12345004);
    g_assert_cmphex(agp_cfg_readl(qts, 11, U3_AGP_AGP_BASE), ==, 0x80000001);
    g_assert_cmphex(agp_cfg_readl(qts, 11, U3_AGP_DUMMY_PAGE), ==, 0x00012345);

    /* ... an invalidate completes at once, the enable bit stays */
    agp_cfg_writel(qts, 11, U3_AGP_GART_CTRL,
                   U3_AGP_GART_CTRL_EN | U3_AGP_GART_CTRL_INV);
    g_assert_cmphex(agp_cfg_readl(qts, 11, U3_AGP_GART_CTRL), ==,
                    U3_AGP_GART_CTRL_EN);

    /* ... and the engine always reports idle */
    g_assert_cmphex(agp_cfg_readl(qts, 11, U3_AGP_INTERNAL_STATUS), ==, 1);

    /*
     * A reset from inside the guest has to take the GART down with it;
     * the generic PCI reset does not touch these offsets.
     */
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_cmphex(agp_cfg_readl(qts, 11, cap + 8), ==, 0);
    for (i = 0; i < ARRAY_SIZE(u3_agp_gart_regs); i++) {
        uint32_t off = u3_agp_gart_regs[i];

        g_assert_cmphex(agp_cfg_readl(qts, 11, off), ==,
                        off == U3_AGP_INTERNAL_STATUS ? 1 : 0);
    }
    /* ... but the bridge still describes the card it drives */
    g_assert_cmphex((agp_cfg_readl(qts, 11, cap) >> 16) & 0xff, ==, 0x30);

    qtest_quit(qts);
}

/*
 * The same card on the HyperTransport side (pci.0 on powermac7_3) is
 * nothing to the AGP bridge, which stays as it was.
 */
static void test_agp_master_on_ht(void)
{
    QTestState *qts;

    if (!qtest_has_device("ati-radeon9800")) {
        g_test_skip("ati-radeon9800 not available");
        return;
    }
    qts = qtest_init("-machine powermac7_3 -vga none "
                     "-device ati-radeon9800,bus=pci.0,addr=0x5");

    check_agp_bridge_idle(qts);
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

/*
 * x-macio-80000000=on mirrors the macio BAR at 0x80000000 for running a
 * real boot ROM, which uses the macio there before any BAR is assigned.
 * The offsets below are relative to the macio BAR (hw/misc/macio/k2_sound.h
 * and k2_sound.c); the K2 feature control registers and the I2S cell are
 * little-endian.
 */
#define MACIO_LOW           0x80000000ULL
#define MACIO_LOW_SIZE      0x80000ULL
#define MACIO_OFF_FCR1      0x3c
#define MACIO_OFF_I2S_INT   0x10000
#define FCR1_I2S0_CELL_EN   0x00000400
#define FCR1_I2S0_RESET     0x00000800
#define FCR1_I2S0_CLK_EN    0x00001000
#define FCR1_I2S0_ENABLE    0x00002000
#define I2S_CLOCKS_STOPPED  0x01000000

static void le_writel(QTestState *qts, uint64_t addr, uint32_t val)
{
    qtest_writel(qts, addr, bswap32(val));
}

static uint32_t le_readl(QTestState *qts, uint64_t addr)
{
    return bswap32(qtest_readl(qts, addr));
}

static bool mtree_has(QTestState *qts, const char *name)
{
    g_autofree char *out = qtest_hmp(qts, "info mtree");

    return strstr(out, name) != NULL;
}

static void test_macio_low_fcr(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3,x-macio-80000000=on");

    le_writel(qts, MACIO_LOW + MACIO_OFF_FCR1, 0x12345678);
    g_assert_cmphex(le_readl(qts, MACIO_LOW + MACIO_OFF_FCR1), ==, 0x12345678);
    g_assert_true(mtree_has(qts, "macio-80000000"));

    qtest_quit(qts);
}

/*
 * The FCR1 reset value (0) already latches clocks-stopped, so start from
 * a running, cleared cell and watch the bit go from 0 to 1.
 */
static void macio_low_i2s_running(QTestState *qts)
{
    le_writel(qts, MACIO_LOW + MACIO_OFF_FCR1,
              FCR1_I2S0_CELL_EN | FCR1_I2S0_CLK_EN | FCR1_I2S0_ENABLE);
    le_writel(qts, MACIO_LOW + MACIO_OFF_I2S_INT, I2S_CLOCKS_STOPPED);
    g_assert_cmphex(le_readl(qts, MACIO_LOW + MACIO_OFF_I2S_INT) &
                    I2S_CLOCKS_STOPPED, ==, 0);
}

static void test_macio_low_clocks_stopped(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3,x-macio-80000000=on");

    macio_low_i2s_running(qts);
    le_writel(qts, MACIO_LOW + MACIO_OFF_FCR1,
              FCR1_I2S0_CELL_EN | FCR1_I2S0_ENABLE);
    g_assert_cmphex(le_readl(qts, MACIO_LOW + MACIO_OFF_I2S_INT) &
                    I2S_CLOCKS_STOPPED, ==, I2S_CLOCKS_STOPPED);

    qtest_quit(qts);
}

/* The order a real boot ROM uses before it waits for clocks-stopped */
static void test_macio_low_rom_sequence(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3,x-macio-80000000=on");
    uint32_t fcr1;

    macio_low_i2s_running(qts);
    fcr1 = le_readl(qts, MACIO_LOW + MACIO_OFF_FCR1);
    fcr1 = (fcr1 | FCR1_I2S0_RESET) & ~(FCR1_I2S0_CLK_EN | FCR1_I2S0_ENABLE);
    le_writel(qts, MACIO_LOW + MACIO_OFF_FCR1, fcr1);
    fcr1 &= ~FCR1_I2S0_RESET;
    le_writel(qts, MACIO_LOW + MACIO_OFF_FCR1, fcr1);
    g_assert_cmphex(le_readl(qts, MACIO_LOW + MACIO_OFF_I2S_INT) &
                    I2S_CLOCKS_STOPPED, ==, I2S_CLOCKS_STOPPED);

    qtest_quit(qts);
}

/* Only the macio devices are covered; the AGP hole shows elsewhere */
static void test_macio_low_bounds(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3,x-macio-80000000=on");

    qtest_writel(qts, MACIO_LOW + MACIO_LOW_SIZE, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, MACIO_LOW + MACIO_LOW_SIZE), ==, 0);
    qtest_writel(qts, MACIO_LOW, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, MACIO_LOW), ==, 0);

    qtest_quit(qts);
}

static void test_macio_low_off(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    le_writel(qts, MACIO_LOW + MACIO_OFF_FCR1, 0x12345678);
    g_assert_cmphex(le_readl(qts, MACIO_LOW + MACIO_OFF_FCR1), !=, 0x12345678);
    g_assert_cmphex(le_readl(qts, MACIO_LOW + MACIO_OFF_FCR1), ==, 0);
    g_assert_false(mtree_has(qts, "macio-80000000"));

    qtest_quit(qts);
}

/* With the BAR assigned, both addresses reach the same registers */
static void test_macio_low_same_as_bar(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3,x-macio-80000000=on");
    uint64_t bar = U3_HT_MEM_BASE;

    k2_program_bridge(qts);
    cfg_writel(qts, U3_HT_CFG_BASE + CFA1(1, MACIO_DEVFN, PCI_BASE_ADDRESS_0),
               (uint32_t)bar);
    cfg_writew(qts, U3_HT_CFG_BASE + CFA1(1, MACIO_DEVFN, PCI_COMMAND),
               PCI_COMMAND_MEMORY);

    le_writel(qts, MACIO_LOW + MACIO_OFF_FCR1, 0x12345678);
    g_assert_cmphex(le_readl(qts, bar + MACIO_OFF_FCR1), ==, 0x12345678);
    le_writel(qts, bar + MACIO_OFF_FCR1, 0x87654321);
    g_assert_cmphex(le_readl(qts, MACIO_LOW + MACIO_OFF_FCR1), ==, 0x87654321);

    qtest_quit(qts);
}

/*
 * x-u3-memctl: the U3 registers at 0xf8002000 that a real boot ROM
 * writes and reads back (the memory bank setup, as far as one can tell),
 * as plain storage.
 */
#define U3_MEMCTL           0xf8002000ULL
#define U3_MEMCTL_SIZE      0x1000
#define U3_MEMCTL_BANK0     (U3_MEMCTL + 0x1c0)
#define U3_MEMCTL_MACHINE   "-machine powermac7_3,x-u3-memctl=on"

static void test_memctl_ends(void)
{
    QTestState *qts = qtest_init(U3_MEMCTL_MACHINE);
    uint64_t last = U3_MEMCTL + U3_MEMCTL_SIZE - 4;

    g_assert_cmphex(qtest_readl(qts, U3_MEMCTL), ==, 0);
    g_assert_cmphex(qtest_readl(qts, last), ==, 0);
    qtest_writel(qts, U3_MEMCTL, 0x12345678);
    qtest_writel(qts, last, 0x9abcdef0);
    g_assert_cmphex(qtest_readl(qts, U3_MEMCTL), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, last), ==, 0x9abcdef0);

    qtest_quit(qts);
}

/* The ROM writes a bank register as a word and reads its top byte back */
static void test_memctl_widths(void)
{
    QTestState *qts = qtest_init(U3_MEMCTL_MACHINE);

    qtest_writel(qts, U3_MEMCTL_BANK0, 0xc4a1b2c3);
    g_assert_cmphex(qtest_readb(qts, U3_MEMCTL_BANK0), ==, 0xc4);
    g_assert_cmphex(qtest_readb(qts, U3_MEMCTL_BANK0 + 1), ==, 0xa1);
    g_assert_cmphex(qtest_readb(qts, U3_MEMCTL_BANK0 + 2), ==, 0xb2);
    g_assert_cmphex(qtest_readb(qts, U3_MEMCTL_BANK0 + 3), ==, 0xc3);

    /* A byte write leaves its neighbours alone */
    qtest_writeb(qts, U3_MEMCTL_BANK0 + 1, 0x5a);
    g_assert_cmphex(qtest_readl(qts, U3_MEMCTL_BANK0), ==, 0xc45ab2c3);

    qtest_writew(qts, U3_MEMCTL_BANK0 + 0x10, 0xbeef);
    g_assert_cmphex(qtest_readw(qts, U3_MEMCTL_BANK0 + 0x10), ==, 0xbeef);
    g_assert_cmphex(qtest_readl(qts, U3_MEMCTL_BANK0 + 0x10), ==, 0xbeef0000);

    qtest_quit(qts);
}

static void test_memctl_mtree(void)
{
    QTestState *qts = qtest_init(U3_MEMCTL_MACHINE);

    g_assert_true(mtree_has(qts, "u3-memctl"));

    qtest_quit(qts);
}

static void test_memctl_reset(void)
{
    QTestState *qts = qtest_init(U3_MEMCTL_MACHINE);

    qtest_writel(qts, U3_MEMCTL_BANK0, 0xc4a1b2c3);
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_cmphex(qtest_readl(qts, U3_MEMCTL_BANK0), ==, 0);

    qtest_quit(qts);
}

static void test_memctl_off(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    qtest_writel(qts, U3_MEMCTL_BANK0, 0xc4a1b2c3);
    g_assert_cmphex(qtest_readl(qts, U3_MEMCTL_BANK0), ==, 0);
    g_assert_false(mtree_has(qts, "u3-memctl"));

    qtest_quit(qts);
}

/* On or off, the neighbours read the same: the i2c below, nothing above */
static void test_memctl_bounds(void)
{
    QTestState *off = qtest_init("-machine powermac7_3");
    QTestState *on = qtest_init(U3_MEMCTL_MACHINE);
    static const uint64_t addrs[] = { U3_MEMCTL - 4,
                                      U3_MEMCTL + U3_MEMCTL_SIZE };
    int i;

    for (i = 0; i < ARRAY_SIZE(addrs); i++) {
        qtest_writel(off, addrs[i], 0x12345678);
        qtest_writel(on, addrs[i], 0x12345678);
        g_assert_cmphex(qtest_readl(on, addrs[i]), ==,
                        qtest_readl(off, addrs[i]));
    }
    g_assert_cmphex(qtest_readl(on, U3_MEMCTL + U3_MEMCTL_SIZE), ==, 0);
    g_assert_cmphex(qtest_readl(on, UNIN_BASE), ==, UNIN_VERSION_U3);

    qtest_quit(on);
    qtest_quit(off);
}

static bool machine_has_prop(QTestState *qts, const char *prop)
{
    QDict *resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                            "{ 'path': '/machine', 'property': %s } }", prop);
    bool found = qdict_haskey(resp, "return");

    qobject_unref(resp);
    return found;
}

static void test_memctl_mac99(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    g_assert_true(machine_has_prop(qts, "x-u3-memctl"));
    qtest_quit(qts);

    qts = qtest_init("-machine mac99 -cpu 970fx");
    g_assert_false(machine_has_prop(qts, "x-u3-memctl"));
    qtest_quit(qts);
}

/*
 * x-ht-mmio-80000000: a real boot ROM puts the BARs of the devices on
 * the HT bus at 0x8xxxxxxx and those of the AGP card from 0x90000000
 * up.  With the switch on, 0x80000000-0x8fffffff is the HT bus memory
 * (identity-mapped) and the AGP hole starts at 0x90000000.
 */
#define HT_LOW              0x80000000ULL
#define HT_LOW_SIZE         0x10000000ULL
#define HT_LOW_MACHINE      "-machine powermac7_3,x-ht-mmio-80000000=on "
#define HT_TESTDEV          "-device pci-testdev,bus=ht.1,addr=8," \
                            "membar=4096,membar-backed=true "
#define HT_TESTDEV_DEVFN    (8 << 3)
#define AGP_TESTDEV         "-device pci-testdev,bus=pci.1,addr=0x12," \
                            "membar=4096,membar-backed=true "
#define AGP_TESTDEV_DEV     0x12

/* RAM-backed 64-bit BAR2 of the pci-testdev behind the K2 bridge */
static void ht_testdev_place(QTestState *qts, uint64_t bar)
{
    cfg_writel(qts, U3_HT_CFG_BASE + CFA1(1, HT_TESTDEV_DEVFN, 0x18),
               (uint32_t)bar);
    cfg_writel(qts, U3_HT_CFG_BASE + CFA1(1, HT_TESTDEV_DEVFN, 0x1c), 0);
    cfg_writew(qts, U3_HT_CFG_BASE + CFA1(1, HT_TESTDEV_DEVFN, 4),
               PCI_COMMAND_MEMORY);
}

/* The same BAR of the pci-testdev on the AGP bus */
static void agp_testdev_place(QTestState *qts, uint64_t bar)
{
    agp_cfg_writel(qts, AGP_TESTDEV_DEV, 0x18, (uint32_t)bar);
    agp_cfg_writel(qts, AGP_TESTDEV_DEV, 0x1c, 0);
    agp_cfg_writel(qts, AGP_TESTDEV_DEV, 4, PCI_COMMAND_MEMORY);
}

/* Two values, each through 4, 2 and 1 byte accesses */
static void check_reaches(QTestState *qts, uint64_t addr)
{
    qtest_writel(qts, addr, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x12345678);
    qtest_writel(qts, addr, 0x9abcdef0);
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x9abcdef0);
    qtest_writew(qts, addr, 0x5aa5);
    g_assert_cmphex(qtest_readw(qts, addr), ==, 0x5aa5);
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x5aa5def0);
    qtest_writeb(qts, addr + 3, 0x3c);
    g_assert_cmphex(qtest_readb(qts, addr + 3), ==, 0x3c);
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x5aa5de3c);
}

static void check_misses(QTestState *qts, uint64_t addr)
{
    qtest_writel(qts, addr, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, addr), !=, 0x12345678);
    qtest_writel(qts, addr, 0x9abcdef0);
    g_assert_cmphex(qtest_readl(qts, addr), !=, 0x9abcdef0);
}

/* Where the ROM put the OHCI behind the K2 bridge */
static void test_ht_low_window(void)
{
    QTestState *qts = qtest_init(HT_LOW_MACHINE HT_TESTDEV);

    k2_program_bridge_windows_at(qts, 0x8000, 0x8ff0);
    k2_enable_bridge_decoding(qts);
    ht_testdev_place(qts, HT_LOW + 0x100000);
    check_reaches(qts, HT_LOW + 0x100000);
    check_reaches(qts, HT_LOW + 0x100ffc);

    qtest_quit(qts);
}

static void test_ht_low_end(void)
{
    QTestState *qts = qtest_init(HT_LOW_MACHINE HT_TESTDEV);
    uint64_t bar = HT_LOW + HT_LOW_SIZE - 0x1000;

    k2_program_bridge_windows_at(qts, 0x8000, 0x8ff0);
    k2_enable_bridge_decoding(qts);
    ht_testdev_place(qts, bar);
    check_reaches(qts, bar);
    check_reaches(qts, bar + 0xffc);

    qtest_quit(qts);
}

/* The window stops at 256MB: the HT bus does not answer at 0x90000000 */
static void test_ht_low_beyond(void)
{
    QTestState *qts = qtest_init(HT_LOW_MACHINE HT_TESTDEV);
    uint64_t bar = HT_LOW + HT_LOW_SIZE;

    k2_program_bridge_windows_at(qts, 0x8000, 0x9000);
    k2_enable_bridge_decoding(qts);
    ht_testdev_place(qts, bar);
    check_misses(qts, bar);

    qtest_quit(qts);
}

/* The shrunk AGP hole still reaches the AGP bus from 0x90000000 ... */
static void test_ht_low_agp_above(void)
{
    QTestState *qts = qtest_init(HT_LOW_MACHINE AGP_TESTDEV);

    agp_testdev_place(qts, 0x90000000);
    check_reaches(qts, 0x90000000);
    qtest_quit(qts);

    qts = qtest_init(HT_LOW_MACHINE AGP_TESTDEV);
    agp_testdev_place(qts, 0xa0000000);
    check_reaches(qts, 0xa0000000);
    qtest_quit(qts);

    qts = qtest_init(HT_LOW_MACHINE AGP_TESTDEV);
    agp_testdev_place(qts, 0xeffff000);
    check_reaches(qts, 0xeffffffc);
    qtest_quit(qts);
}

/* ... and no longer below it */
static void test_ht_low_agp_below(void)
{
    QTestState *qts = qtest_init(HT_LOW_MACHINE AGP_TESTDEV);

    agp_testdev_place(qts, 0x8ff00000);
    check_misses(qts, 0x8ff00000);

    qtest_quit(qts);
}

/* x-macio-80000000 stays on top; past the MacIO the HT bus answers */
static void test_ht_low_with_macio_low(void)
{
    QTestState *qts = qtest_init(HT_LOW_MACHINE HT_TESTDEV
                                 "-machine x-macio-80000000=on");

    k2_program_bridge_windows_at(qts, 0x8000, 0x8ff0);
    k2_enable_bridge_decoding(qts);
    ht_testdev_place(qts, HT_LOW + 0x100000);

    le_writel(qts, MACIO_LOW + MACIO_OFF_FCR1, 0x12345678);
    g_assert_cmphex(le_readl(qts, MACIO_LOW + MACIO_OFF_FCR1), ==, 0x12345678);
    check_reaches(qts, HT_LOW + 0x100000);

    qtest_quit(qts);
}

/*
 * Even with an HT bus BAR right at 0x80000000, the MacIO copy is what
 * answers there: the ROM drives the MacIO at that address.
 */
static void test_ht_low_macio_on_top(void)
{
    QTestState *qts = qtest_init(HT_LOW_MACHINE HT_TESTDEV
                                 "-machine x-macio-80000000=on");

    k2_program_bridge_windows_at(qts, 0x8000, 0x8ff0);
    k2_enable_bridge_decoding(qts);
    ht_testdev_place(qts, HT_LOW);

    le_writel(qts, MACIO_LOW + MACIO_OFF_FCR1, 0x12345678);
    g_assert_cmphex(le_readl(qts, MACIO_LOW + MACIO_OFF_FCR1), ==, 0x12345678);
    /* The testdev RAM below would keep any value; FCR1 is the MacIO's */
    cfg_writew(qts, U3_HT_CFG_BASE + CFA1(1, HT_TESTDEV_DEVFN, 4), 0);
    g_assert_cmphex(le_readl(qts, MACIO_LOW + MACIO_OFF_FCR1), ==, 0x12345678);

    qtest_quit(qts);
}

static void test_ht_low_off(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3 " HT_TESTDEV);

    k2_program_bridge_windows_at(qts, 0x8000, 0x8ff0);
    k2_enable_bridge_decoding(qts);
    ht_testdev_place(qts, HT_LOW + 0x100000);
    check_misses(qts, HT_LOW + 0x100000);

    /* The same device answers once moved into the 0xfa000000 window */
    k2_program_bridge_windows_at(qts, 0xfa00, 0xfaf0);
    ht_testdev_place(qts, U3_HT_MEM_BASE + 0x100000);
    check_reaches(qts, U3_HT_MEM_BASE + 0x100000);
    g_assert_false(mtree_has(qts, "u3-ht-pci-mmio-80000000"));
    g_assert_false(mtree_has(qts, "unin-pci-hole-90000000"));
    g_assert_true(mtree_has(qts, "alias unin-pci-hole @"));

    qtest_quit(qts);
}

/* Off, the AGP hole still starts at 0x80000000 */
static void test_ht_low_off_agp(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3 " AGP_TESTDEV);

    agp_testdev_place(qts, HT_LOW + 0x100000);
    check_reaches(qts, HT_LOW + 0x100000);

    qtest_quit(qts);
}

/* The decode registers do not move with the switch */
static void test_ht_low_decode_regs(void)
{
    const char *machines[] = { "-machine powermac7_3", HT_LOW_MACHINE };

    for (int i = 0; i < ARRAY_SIZE(machines); i++) {
        QTestState *qts = qtest_init(machines[i]);

        g_assert_cmphex(qtest_readl(qts, U3_HT_SELF_BASE + 0x200), ==,
                        0x08e00000);
        qtest_writel(qts, U3_HT_SELF_BASE + 0x200, 0xffff001f);
        g_assert_cmphex(qtest_readl(qts, U3_HT_SELF_BASE + 0x200), ==,
                        0x08e00000);
        g_assert_cmphex(agp_cfg_readl(qts, 11, 0x48), ==, 0x01000000);
        agp_cfg_writel(qts, 11, 0x48, 0x07000000);
        g_assert_cmphex(agp_cfg_readl(qts, 11, 0x48), ==, 0x07000000);
        qtest_quit(qts);
    }
}

static void test_ht_low_mtree(void)
{
    QTestState *qts = qtest_init(HT_LOW_MACHINE);

    g_assert_true(mtree_has(qts, "u3-ht-pci-mmio-80000000"));
    g_assert_true(mtree_has(qts, "unin-pci-hole-90000000"));
    /* The full-size hole is gone rather than left underneath */
    g_assert_false(mtree_has(qts, "alias unin-pci-hole @"));

    qtest_quit(qts);
}

static void test_ht_low_mac99(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    g_assert_true(machine_has_prop(qts, "x-ht-mmio-80000000"));
    qtest_quit(qts);

    qts = qtest_init("-machine mac99 -cpu 970fx");
    g_assert_false(machine_has_prop(qts, "x-ht-mmio-80000000"));
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

/*
 * x-ht-k2-layout moves the K2 HT-PCI bridge for the real ROM, which
 * takes HT dev 1 to be a K2 bridge unless it is an AMD-8131.  The
 * expected tables come from the design (vendor:device, class and
 * interrupt pin of HT bus 0 dev 0..7), not from what the code does.
 */
#define HT_EMPTY          0xffffffff
#define SUNGEM_IDS        ((0x0021 << 16) | 0x106b)
#define K2_FN_IDS(n)      ((((uint32_t)(n)) << 16) | 0x106b)

typedef struct HTSlot {
    uint32_t ids;
    uint16_t class;
    uint8_t pin;
} HTSlot;

#define SLOT_EMPTY        { HT_EMPTY, 0xffff, 0xff }
#define SLOT_HOST         { U3_HT_IDS, 0x0600, 0 }
#define SLOT_NIC          { SUNGEM_IDS, 0x0200, 1 }
#define SLOT_K2(n)        { K2_FN_IDS(n), 0x0604, 0 }

static const HTSlot ht_dev3[8] = {
    SLOT_HOST, SLOT_NIC, SLOT_EMPTY, SLOT_K2(0x45),
    SLOT_EMPTY, SLOT_EMPTY, SLOT_EMPTY, SLOT_EMPTY,
};
static const HTSlot ht_dev1[8] = {
    SLOT_HOST, SLOT_K2(0x45), SLOT_NIC, SLOT_EMPTY,
    SLOT_EMPTY, SLOT_EMPTY, SLOT_EMPTY, SLOT_EMPTY,
};
static const HTSlot ht_dev1_5[8] = {
    SLOT_HOST, SLOT_K2(0x45), SLOT_K2(0x46), SLOT_K2(0x47),
    SLOT_K2(0x48), SLOT_K2(0x49), SLOT_NIC, SLOT_EMPTY,
};

static void check_ht_slots(const char *args, const HTSlot *want,
                           int nic_dev)
{
    QTestState *qts = qtest_init(args);

    for (int dev = 0; dev < 8; dev++) {
        HTSlot w = want[dev];
        uint64_t cfg = U3_HT_CFG_BASE + CFA0(dev << 3, 0);

        if (dev == nic_dev) {
            w = (HTSlot)SLOT_EMPTY;
        }
        g_assert_cmphex(cfg_readl(qts, cfg), ==, w.ids);
        g_assert_cmphex(cfg_readl(qts, cfg + 0x08) >> 16, ==, w.class);
        g_assert_cmphex(qtest_readb(qts, cfg + PCI_INTERRUPT_PIN), ==, w.pin);
    }
    qtest_quit(qts);
}

static void test_k2_layout_dev3(void)
{
    check_ht_slots("-machine powermac7_3", ht_dev3, -1);
    check_ht_slots("-machine powermac7_3,x-ht-k2-layout=dev3", ht_dev3, -1);
    check_ht_slots("-machine powermac7_3 -nic none", ht_dev3, 1);
}

static void test_k2_layout_dev1(void)
{
    check_ht_slots("-machine powermac7_3,x-ht-k2-layout=dev1", ht_dev1, -1);
    check_ht_slots("-machine powermac7_3,x-ht-k2-layout=dev1 -nic none",
                   ht_dev1, 2);
}

static void test_k2_layout_dev1_5(void)
{
    check_ht_slots("-machine powermac7_3,x-ht-k2-layout=dev1-5",
                   ht_dev1_5, -1);
    check_ht_slots("-machine powermac7_3,x-ht-k2-layout=dev1-5 -nic none",
                   ht_dev1_5, 6);
}

/*
 * The 0045 bridge keeps the ht.1 name and the macio behind it at dev 7,
 * wherever the bridge sits; a device plugged on ht.1 shows up there too.
 */
static void test_k2_layout_secondary(void)
{
    static const char *args[] = {
        "-machine powermac7_3,x-ht-k2-layout=dev1 "
        "-device pci-testdev,bus=ht.1,addr=0xd",
        "-machine powermac7_3,x-ht-k2-layout=dev1-5 "
        "-device pci-testdev,bus=ht.1,addr=0xd",
    };

    for (int i = 0; i < ARRAY_SIZE(args); i++) {
        QTestState *qts = qtest_init(args[i]);
        uint64_t br = U3_HT_CFG_BASE + CFA0(1 << 3, 0);

        qtest_writeb(qts, br + 0x18, 0);
        qtest_writeb(qts, br + 0x19, 1);
        qtest_writeb(qts, br + 0x1a, 1);
        g_assert_cmphex(cfg_readl(qts, U3_HT_CFG_BASE +
                                  CFA1(1, MACIO_DEVFN, 0)), ==, MACIO_IDS);
        g_assert_cmphex(cfg_readl(qts, U3_HT_CFG_BASE +
                                  CFA1(1, 0xd << 3, 0)), ==, TESTDEV_IDS);
        qtest_quit(qts);
    }
}

/*
 * The setter refuses other values.  -machine with a bad value makes QEMU
 * exit before the qtest socket is up, so go through qom-set instead.
 */
static void test_k2_layout_bad(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");
    QDict *resp = qtest_qmp(qts, "{ 'execute': 'qom-set', 'arguments': "
                            "{ 'path': '/machine', "
                            "'property': 'x-ht-k2-layout', "
                            "'value': 'dev2' } }");
    QDict *err = qdict_get_qdict(resp, "error");

    g_assert_nonnull(err);
    g_assert_cmpstr(qdict_get_str(err, "desc"), ==,
                    "Invalid x-ht-k2-layout value");
    qobject_unref(resp);
    qtest_quit(qts);
}

static void test_k2_layout_mac99(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");
    QDict *resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                            "{ 'path': '/machine', "
                            "'property': 'x-ht-k2-layout' } }");

    g_assert_cmpstr(qdict_get_str(resp, "return"), ==, "dev3");
    qobject_unref(resp);
    qtest_quit(qts);

    qts = qtest_init("-machine powermac7_3,x-ht-k2-layout=dev1-5");
    resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                     "{ 'path': '/machine', 'property': 'x-ht-k2-layout' } }");
    g_assert_cmpstr(qdict_get_str(resp, "return"), ==, "dev1-5");
    qobject_unref(resp);
    qtest_quit(qts);

    qts = qtest_init("-machine mac99 -cpu 970fx");
    g_assert_false(machine_has_prop(qts, "x-ht-k2-layout"));
    qtest_quit(qts);
}

/*
 * x-ht-k2-rom-irq: where a device's INTx lands on the mpic.  The device is
 * a pci-ohci whose root hub status change interrupt the test raises and
 * clears by register writes, so the level follows the test exactly.  The
 * mpic inputs are intercepted, and every one of them is compared against
 * the single input expected to move.
 */
#define OHCI_IRQ_BAR_HT     (U3_HT_MEM_BASE + 0x100000)
#define OHCI_IRQ_BAR_HT2    (U3_HT_MEM_BASE + 0x900000)
#define OHCI_IRQ_BAR_AGP    0x90000000ULL
#define HC_INTR_STATUS      0x0c
#define HC_INTR_ENABLE      0x10
#define HC_RH_STATUS        0x50
#define HC_INTR_MIE         (1u << 31)
#define HC_INTR_RHSC        (1u << 6)
#define HC_RHS_DRWE         (1u << 15)
#define MPIC_WATCHED        0x40
#define NO_IRQ              (-1)

typedef enum {
    ON_HT1,         /* behind the K2 bridge */
    ON_HT2,         /* behind the first empty bridge of dev1-5 */
    ON_HT_ROOT,
    ON_AGP,
} OhciBus;

static void ohci_irq_intercept(QTestState *qts)
{
    QDict *resp = qtest_qmp(qts, "{ 'execute': 'qom-list', 'arguments': "
                            "{ 'path': '/machine/unattached' } }");
    QList *list = qdict_get_qlist(resp, "return");
    const QListEntry *e;
    g_autofree char *path = NULL;

    QLIST_FOREACH_ENTRY(list, e) {
        QDict *d = qobject_to(QDict, qlist_entry_obj(e));

        if (!strcmp(qdict_get_str(d, "type"), "child<macio-k2>")) {
            path = g_strdup_printf("/machine/unattached/%s/pic",
                                   qdict_get_str(d, "name"));
        }
    }
    qobject_unref(resp);
    g_assert_nonnull(path);
    qtest_irq_intercept_in(qts, path);
}

static uint64_t ohci_irq_setup(QTestState *qts, OhciBus bus, int k2_dev,
                               int devfn)
{
    uint64_t k2 = U3_HT_CFG_BASE + CFA0(k2_dev << 3, 0);
    uint64_t bar, cfg;

    switch (bus) {
    case ON_HT1:
        qtest_writeb(qts, k2 + 0x18, 0);
        qtest_writeb(qts, k2 + 0x19, 1);
        qtest_writeb(qts, k2 + 0x1a, 1);
        cfg_writew(qts, k2 + 0x20, 0xfa00);
        cfg_writew(qts, k2 + 0x22, 0xfa70);
        cfg_writew(qts, k2 + 4, PCI_COMMAND_MEMORY);
        bar = OHCI_IRQ_BAR_HT;
        cfg = U3_HT_CFG_BASE + CFA1(1, devfn, 0);
        break;
    case ON_HT2: {
        uint64_t br = U3_HT_CFG_BASE + CFA0(2 << 3, 0);

        qtest_writeb(qts, br + 0x18, 0);
        qtest_writeb(qts, br + 0x19, 2);
        qtest_writeb(qts, br + 0x1a, 2);
        cfg_writew(qts, br + 0x20, 0xfa80);
        cfg_writew(qts, br + 0x22, 0xfaf0);
        cfg_writew(qts, br + 4, PCI_COMMAND_MEMORY);
        bar = OHCI_IRQ_BAR_HT2;
        cfg = U3_HT_CFG_BASE + CFA1(2, devfn, 0);
        break;
    }
    case ON_HT_ROOT:
        bar = OHCI_IRQ_BAR_HT;
        cfg = U3_HT_CFG_BASE + CFA0(devfn, 0);
        break;
    case ON_AGP:
        agp_cfg_writel(qts, devfn >> 3, PCI_BASE_ADDRESS_0, OHCI_IRQ_BAR_AGP);
        agp_cfg_writel(qts, devfn >> 3, PCI_COMMAND, PCI_COMMAND_MEMORY);
        return OHCI_IRQ_BAR_AGP;
    default:
        g_assert_not_reached();
    }
    cfg_writel(qts, cfg + PCI_BASE_ADDRESS_0, (uint32_t)bar);
    cfg_writew(qts, cfg + PCI_COMMAND, PCI_COMMAND_MEMORY);
    return bar;
}

static void check_mpic(QTestState *qts, int want, int level)
{
    for (int n = 0; n < MPIC_WATCHED; n++) {
        g_assert_cmpint(qtest_get_irq(qts, n), ==, n == want ? level : 0);
    }
}

/*
 * Raise and clear the OHCI's INTx, with the mpic input that has to follow
 * it (NO_IRQ: none may move).
 */
static void check_ohci_irq(const char *args, OhciBus bus, int k2_dev,
                           int devfn, int want)
{
    QTestState *qts = qtest_init(args);
    uint64_t bar;

    ohci_irq_intercept(qts);
    bar = ohci_irq_setup(qts, bus, k2_dev, devfn);
    check_mpic(qts, NO_IRQ, 0);

    qtest_writel(qts, bar + HC_INTR_ENABLE,
                 bswap32(HC_INTR_MIE | HC_INTR_RHSC));
    qtest_writel(qts, bar + HC_RH_STATUS, bswap32(HC_RHS_DRWE));
    g_assert_cmphex(bswap32(qtest_readl(qts, bar + HC_INTR_STATUS)) &
                    HC_INTR_RHSC, ==, HC_INTR_RHSC);
    check_mpic(qts, want, 1);

    qtest_writel(qts, bar + HC_INTR_STATUS, bswap32(HC_INTR_RHSC));
    check_mpic(qts, NO_IRQ, 0);

    qtest_quit(qts);
}

#define ROM_IRQ_ARGS(layout, rom, dev) \
    "-machine powermac7_3,usb=off,x-ht-k2-layout=" layout \
    ",x-ht-k2-rom-irq=" rom " -vga none " dev

/* Off: the swizzle and the HT root's (pin + dev) & 3 onto 0x1f + line */
static void test_k2_rom_irq_off(void)
{
    /* dev3: K2 at HT dev 3 */
    check_ohci_irq(ROM_IRQ_ARGS("dev3", "off",
                                "-device pci-ohci,bus=ht.1,addr=8"),
                   ON_HT1, 3, 8 << 3, 0x22);
    check_ohci_irq(ROM_IRQ_ARGS("dev3", "off",
                                "-device pci-ohci,bus=ht.1,addr=9"),
                   ON_HT1, 3, 9 << 3, 0x1f);
    /* dev1-5: K2 at HT dev 1 */
    check_ohci_irq(ROM_IRQ_ARGS("dev1-5", "off",
                                "-device pci-ohci,bus=ht.1,addr=8"),
                   ON_HT1, 1, 8 << 3, 0x20);
    check_ohci_irq(ROM_IRQ_ARGS("dev1-5", "off",
                                "-device pci-ohci,bus=ht.1,addr=9"),
                   ON_HT1, 1, 9 << 3, 0x21);
    check_ohci_irq(ROM_IRQ_ARGS("dev1-5", "off",
                                "-device pci-ohci,bus=ht.1,addr=0xd"),
                   ON_HT1, 1, 0xd << 3, 0x21);
    /* the AGP bus: (pin + dev) & 3 onto 0x1b + line */
    check_ohci_irq(ROM_IRQ_ARGS("dev1-5", "off",
                                "-device pci-ohci,bus=pci.1,addr=0x12"),
                   ON_AGP, 1, 0x12 << 3, 0x1d);
}

/*
 * On: ht.1 dev 8 and 9 go where the boot ROM's /ht/pci@1 interrupt-map
 * sends them, whatever the function; other ht.1 devices and the AGP bus
 * are left unconnected; the HT root, ht.2 and the AGP slot are untouched.
 */
static void test_k2_rom_irq_on(void)
{
    static const char *const layouts[] = { "dev3", "dev1", "dev1-5" };

    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        int k2_dev = strcmp(layouts[i], "dev3") ? 1 : 3;
        g_autofree char *a8 = g_strdup_printf(
            "-machine powermac7_3,usb=off,x-ht-k2-layout=%s,"
            "x-ht-k2-rom-irq=on -vga none -device pci-ohci,bus=ht.1,addr=8",
            layouts[i]);
        g_autofree char *a9 = g_strdup_printf(
            "-machine powermac7_3,usb=off,x-ht-k2-layout=%s,"
            "x-ht-k2-rom-irq=on -vga none -device pci-ohci,bus=ht.1,addr=9",
            layouts[i]);

        check_ohci_irq(a8, ON_HT1, k2_dev, 8 << 3, 0x1b);
        check_ohci_irq(a9, ON_HT1, k2_dev, 9 << 3, 0x1c);
    }
    check_ohci_irq(ROM_IRQ_ARGS("dev1-5", "on",
                                "-device pci-ohci,bus=ht.1,addr=8.0,"
                                "multifunction=on "
                                "-device pci-ohci,bus=ht.1,addr=8.1"),
                   ON_HT1, 1, (8 << 3) | 1, 0x1b);
    check_ohci_irq(ROM_IRQ_ARGS("dev1-5", "on",
                                "-device pci-ohci,bus=ht.1,addr=0xd"),
                   ON_HT1, 1, 0xd << 3, NO_IRQ);
    check_ohci_irq(ROM_IRQ_ARGS("dev1-5", "on",
                                "-device pci-ohci,bus=pci.1,addr=0x12"),
                   ON_AGP, 1, 0x12 << 3, NO_IRQ);
    check_ohci_irq(ROM_IRQ_ARGS("dev1-5", "on",
                                "-device pci-ohci,bus=pci.1,addr=0x10"),
                   ON_AGP, 1, 0x10 << 3, 0x30);
    check_ohci_irq(ROM_IRQ_ARGS("dev1-5", "on",
                                "-device pci-ohci,bus=pci.0,addr=0x12"),
                   ON_HT_ROOT, 1, 0x12 << 3, 0x21);
    check_ohci_irq(ROM_IRQ_ARGS("dev1-5", "on",
                                "-device pci-ohci,bus=ht.2,addr=1"),
                   ON_HT2, 1, 1 << 3, 0x22);
}

/* The default usb=on controller sits on the AGP bus with nothing behind */
static void test_k2_rom_irq_agp_usb(void)
{
    check_ohci_irq("-machine powermac7_3,x-ht-k2-layout=dev1-5,"
                   "x-ht-k2-rom-irq=on -vga none",
                   ON_AGP, 1, 0xc << 3, NO_IRQ);
    check_ohci_irq("-machine powermac7_3,x-ht-k2-layout=dev1-5 -vga none",
                   ON_AGP, 1, 0xc << 3, 0x1b);
}

static void test_k2_rom_irq_prop(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");
    QDict *resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                            "{ 'path': '/machine', "
                            "'property': 'x-ht-k2-rom-irq' } }");

    g_assert_false(qdict_get_bool(resp, "return"));
    qobject_unref(resp);
    qtest_quit(qts);

    qts = qtest_init("-machine powermac7_3,x-ht-k2-rom-irq=on");
    resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                     "{ 'path': '/machine', 'property': 'x-ht-k2-rom-irq' } }");
    g_assert_true(qdict_get_bool(resp, "return"));
    qobject_unref(resp);
    qtest_quit(qts);

    qts = qtest_init("-machine mac99 -cpu 970fx");
    g_assert_false(machine_has_prop(qts, "x-ht-k2-rom-irq"));
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
    qtest_add_func("/u3-ht/mac99-970-agp-capability",
                   test_mac99_970_agp_capability);
    qtest_add_func("/u3-ht/agp-master-r350", test_agp_master_r350);
    qtest_add_func("/u3-ht/agp-master-on-ht", test_agp_master_on_ht);
    qtest_add_func("/u3-ht/mac99-g4-no-agp-capability",
                   test_mac99_g4_no_agp_capability);
    qtest_add_func("/u3-ht/fw-cfg-not-shadowed", test_fw_cfg_not_shadowed);
    qtest_add_func("/u3-ht/unin-version", test_unin_version);
    qtest_add_func("/u3-ht/mac99-unin-version", test_mac99_unin_version);
    qtest_add_func("/u3-ht/mac99-unmapped", test_mac99_unmapped);
    qtest_add_func("/u3-ht/macio-low-fcr", test_macio_low_fcr);
    qtest_add_func("/u3-ht/macio-low-clocks-stopped",
                   test_macio_low_clocks_stopped);
    qtest_add_func("/u3-ht/macio-low-rom-sequence",
                   test_macio_low_rom_sequence);
    qtest_add_func("/u3-ht/macio-low-bounds", test_macio_low_bounds);
    qtest_add_func("/u3-ht/macio-low-off", test_macio_low_off);
    qtest_add_func("/u3-ht/macio-low-same-as-bar", test_macio_low_same_as_bar);
    qtest_add_func("/u3-ht/memctl-ends", test_memctl_ends);
    qtest_add_func("/u3-ht/memctl-widths", test_memctl_widths);
    qtest_add_func("/u3-ht/memctl-mtree", test_memctl_mtree);
    qtest_add_func("/u3-ht/memctl-reset", test_memctl_reset);
    qtest_add_func("/u3-ht/memctl-off", test_memctl_off);
    qtest_add_func("/u3-ht/memctl-bounds", test_memctl_bounds);
    qtest_add_func("/u3-ht/memctl-mac99", test_memctl_mac99);
    qtest_add_func("/u3-ht/ht-low-window", test_ht_low_window);
    qtest_add_func("/u3-ht/ht-low-end", test_ht_low_end);
    qtest_add_func("/u3-ht/ht-low-beyond", test_ht_low_beyond);
    qtest_add_func("/u3-ht/ht-low-agp-above", test_ht_low_agp_above);
    qtest_add_func("/u3-ht/ht-low-agp-below", test_ht_low_agp_below);
    qtest_add_func("/u3-ht/ht-low-with-macio-low", test_ht_low_with_macio_low);
    qtest_add_func("/u3-ht/ht-low-macio-on-top", test_ht_low_macio_on_top);
    qtest_add_func("/u3-ht/ht-low-off", test_ht_low_off);
    qtest_add_func("/u3-ht/ht-low-off-agp", test_ht_low_off_agp);
    qtest_add_func("/u3-ht/ht-low-decode-regs", test_ht_low_decode_regs);
    qtest_add_func("/u3-ht/ht-low-mtree", test_ht_low_mtree);
    qtest_add_func("/u3-ht/ht-low-mac99", test_ht_low_mac99);
    qtest_add_func("/u3-ht/k2-layout-dev3", test_k2_layout_dev3);
    qtest_add_func("/u3-ht/k2-layout-dev1", test_k2_layout_dev1);
    qtest_add_func("/u3-ht/k2-layout-dev1-5", test_k2_layout_dev1_5);
    qtest_add_func("/u3-ht/k2-layout-secondary", test_k2_layout_secondary);
    qtest_add_func("/u3-ht/k2-layout-bad", test_k2_layout_bad);
    qtest_add_func("/u3-ht/k2-layout-mac99", test_k2_layout_mac99);
    qtest_add_func("/u3-ht/k2-rom-irq-off", test_k2_rom_irq_off);
    qtest_add_func("/u3-ht/k2-rom-irq-on", test_k2_rom_irq_on);
    qtest_add_func("/u3-ht/k2-rom-irq-agp-usb", test_k2_rom_irq_agp_usb);
    qtest_add_func("/u3-ht/k2-rom-irq-prop", test_k2_rom_irq_prop);

    return g_test_run();
}
