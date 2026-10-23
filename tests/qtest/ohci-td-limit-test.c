/*
 * QTest testcase for long TD chains on one OHCI endpoint
 *
 * ohci_service_ed_list() walks at most a bounded number of TDs of one
 * ED (34) each time it goes over the list.  A guest that queues more
 * than that on one ED (a real PowerMac G5 boot ROM reading a USB disk
 * does) must see the rest of the chain completed in the following
 * frames, not the
 * controller dying with an Unrecoverable Error.  A TD that comes back
 * on the ED head again after each retire must still not make a frame
 * loop forever.
 *
 * The controller is a pci-ohci on the U3 AGP bus of powermac7_3 (no
 * DART, GART off, so bus addresses are guest physical addresses) with
 * a usb-kbd behind it.  The TDs make up one GET_DESCRIPTOR control
 * transfer to address 0, padded with zero-length IN TDs, which the
 * device keeps accepting once the data stage is over.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "hw/pci/pci_regs.h"

#define MACHINE "-machine powermac7_3,usb=off " \
                "-device pci-ohci,id=ohci,bus=pci.1,addr=0x12 " \
                "-device usb-kbd,bus=ohci.0"

/* AGP domain config access, as in u3-ht-test.c */
#define U3_AGP_CFG_ADDR   0xf0800000ULL
#define U3_AGP_CFG_DATA   0xf0c00000ULL
#define U3_AGP_CFA(dev, off)  ((1u << (dev)) | (off))
#define OHCI_DEV          0x12
#define OHCI_BAR          0x90000000ULL

/* OHCI registers */
#define HC_CONTROL        0x04
#define HC_COMMAND_STATUS 0x08
#define HC_INTR_STATUS    0x0c
#define HC_HCCA           0x18
#define HC_CTRL_HEAD_ED   0x20
#define HC_FM_INTERVAL    0x34
#define HC_FM_NUMBER      0x3c
#define HC_RH_PORT1       0x54

#define CTL_CLE           (1 << 4)
#define CTL_OPERATIONAL   (2 << 6)
#define STATUS_CLF        (1 << 1)
#define INTR_UE           (1 << 4)
#define PORT_PRS          (1 << 4)
#define PORT_PES          (1 << 1)

/* Descriptor bits */
#define ED_MPS(n)         ((n) << 16)
#define TD_DP_SETUP       (0 << 19)
#define TD_DP_OUT         (1 << 19)
#define TD_DP_IN          (2 << 19)
#define TD_DI_NONE        (7 << 21)
#define TD_T_DATA0        (2 << 24)
#define TD_T_DATA1        (3 << 24)
#define TD_CC_NOT_ACCESSED (0xfu << 28)
#define TD_CC(flags)      ((flags) >> 28)

/* Guest memory layout */
#define HCCA_ADDR         0x10000
#define ED_ADDR           0x20000
#define TD_ADDR(i)        (0x30000 + (i) * 16)
#define SETUP_ADDR        0x40000
#define DATA_ADDR(i)      (0x50000 + (i) * 8)

static void agp_cfg_writel(QTestState *qts, int dev, uint32_t off,
                           uint32_t val)
{
    qtest_writel(qts, U3_AGP_CFG_ADDR, bswap32(U3_AGP_CFA(dev, off & 0xf8)));
    qtest_writel(qts, U3_AGP_CFG_DATA + (off & 4), bswap32(val));
}

static void ohci_writel(QTestState *qts, uint32_t reg, uint32_t val)
{
    qtest_writel(qts, OHCI_BAR + reg, bswap32(val));
}

static uint32_t ohci_readl(QTestState *qts, uint32_t reg)
{
    return bswap32(qtest_readl(qts, OHCI_BAR + reg));
}

static void mem_le32(QTestState *qts, uint64_t addr, uint32_t val)
{
    uint32_t le = cpu_to_le32(val);

    qtest_memwrite(qts, addr, &le, sizeof(le));
}

static uint32_t mem_rd32(QTestState *qts, uint64_t addr)
{
    uint32_t le;

    qtest_memread(qts, addr, &le, sizeof(le));
    return le32_to_cpu(le);
}

static void write_td(QTestState *qts, int i, uint32_t flags, uint32_t cbp,
                     uint32_t next, uint32_t be)
{
    mem_le32(qts, TD_ADDR(i), flags | TD_CC_NOT_ACCESSED);
    mem_le32(qts, TD_ADDR(i) + 4, cbp);
    mem_le32(qts, TD_ADDR(i) + 8, next);
    mem_le32(qts, TD_ADDR(i) + 12, be);
}

/*
 * A chain of n TDs (n >= 3): SETUP of GET_DESCRIPTOR(device, 8 bytes),
 * the 8 byte IN data stage, zero-length INs, and the status OUT.
 * TD n is the empty tail.
 */
static void write_chain(QTestState *qts, int n)
{
    static const uint8_t setup[8] = { 0x80, 0x06, 0x00, 0x01,
                                      0x00, 0x00, 0x08, 0x00 };

    qtest_memwrite(qts, SETUP_ADDR, setup, sizeof(setup));
    write_td(qts, 0, TD_DP_SETUP | TD_DI_NONE | TD_T_DATA0,
             SETUP_ADDR, TD_ADDR(1), SETUP_ADDR + 7);
    write_td(qts, 1, TD_DP_IN | TD_DI_NONE | TD_T_DATA1,
             DATA_ADDR(0), TD_ADDR(2), DATA_ADDR(0) + 7);
    for (int i = 2; i < n - 1; i++) {
        write_td(qts, i, TD_DP_IN | TD_DI_NONE | TD_T_DATA1,
                 0, TD_ADDR(i + 1), 0);
    }
    write_td(qts, n - 1, TD_DP_OUT | TD_DI_NONE | TD_T_DATA1,
             0, TD_ADDR(n), 0);
    write_td(qts, n, 0, 0, 0, 0);
}

static void write_ed(QTestState *qts, uint32_t head, uint32_t tail)
{
    mem_le32(qts, ED_ADDR, ED_MPS(8));      /* FA 0, EN 0, D from TD */
    mem_le32(qts, ED_ADDR + 4, tail);
    mem_le32(qts, ED_ADDR + 8, head);
    mem_le32(qts, ED_ADDR + 12, 0);
}

static QTestState *ohci_start(void)
{
    QTestState *qts = qtest_init(MACHINE);

    agp_cfg_writel(qts, OHCI_DEV, PCI_BASE_ADDRESS_0, OHCI_BAR);
    agp_cfg_writel(qts, OHCI_DEV, PCI_COMMAND,
                   PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    /* Reset and enable the port the keyboard sits on */
    ohci_writel(qts, HC_RH_PORT1, PORT_PRS);
    g_assert_cmphex(ohci_readl(qts, HC_RH_PORT1) & PORT_PES, ==, PORT_PES);

    ohci_writel(qts, HC_HCCA, HCCA_ADDR);
    ohci_writel(qts, HC_FM_INTERVAL, 0x27782edf);
    ohci_writel(qts, HC_CTRL_HEAD_ED, ED_ADDR);
    return qts;
}

static void ohci_run(QTestState *qts)
{
    ohci_writel(qts, HC_CONTROL, CTL_CLE | CTL_OPERATIONAL);
    ohci_writel(qts, HC_COMMAND_STATUS, STATUS_CLF);
}

static void frame(QTestState *qts)
{
    qtest_clock_step(qts, 1000 * 1000);     /* 1 ms */
}

/* Frames until the ED head reaches the tail; asserts the chain went fine */
static int run_chain(int n)
{
    QTestState *qts = ohci_start();
    int frames;

    write_chain(qts, n);
    write_ed(qts, TD_ADDR(0), TD_ADDR(n));
    ohci_run(qts);

    for (frames = 1; frames <= 16; frames++) {
        frame(qts);
        g_assert_cmphex(ohci_readl(qts, HC_INTR_STATUS) & INTR_UE, ==, 0);
        if ((mem_rd32(qts, ED_ADDR + 8) & ~0xf) == TD_ADDR(n)) {
            break;
        }
    }
    g_assert_cmpint(frames, <=, 16);
    for (int i = 0; i < n; i++) {
        g_assert_cmpuint(TD_CC(mem_rd32(qts, TD_ADDR(i))), ==, 0);
    }

    qtest_quit(qts);
    return frames;
}

static void test_chain_33(void)
{
    run_chain(33);
}

static void test_chain_34(void)
{
    run_chain(34);
}

static void test_chain_35(void)
{
    run_chain(35);
}

static void test_chain_99(void)
{
    run_chain(99);
}

/* One frame takes up to 34 TDs of an ED; the rest wait for the next ones */
static void test_chain_frames(void)
{
    int f34 = run_chain(34);

    g_assert_cmpint(run_chain(35), ==, f34 + 1);
    g_assert_cmpint(run_chain(68), ==, f34 + 1);
    g_assert_cmpint(run_chain(69), ==, f34 + 2);
}

/*
 * A TD that never ends: once TD y has been retired, the done queue
 * points at it, and with y.next = y and the ED head on y, every retire
 * of y leaves the ED head on y again.  Each frame must still end.
 */
static void test_endless_td(void)
{
    QTestState *qts = ohci_start();
    const int y = 2;
    uint32_t fm;
    int frames;

    write_chain(qts, 4);
    write_ed(qts, TD_ADDR(0), TD_ADDR(y + 1));
    ohci_run(qts);
    for (frames = 1; frames <= 16; frames++) {
        frame(qts);
        if ((mem_rd32(qts, ED_ADDR + 8) & ~0xf) == TD_ADDR(y + 1)) {
            break;
        }
    }
    g_assert_cmpint(frames, <=, 16);

    mem_le32(qts, TD_ADDR(y) + 8, TD_ADDR(y));
    mem_le32(qts, TD_ADDR(y), TD_DP_IN | TD_DI_NONE | TD_T_DATA1 |
                              TD_CC_NOT_ACCESSED);
    write_ed(qts, TD_ADDR(y), TD_ADDR(y + 1));
    ohci_writel(qts, HC_COMMAND_STATUS, STATUS_CLF);

    fm = ohci_readl(qts, HC_FM_NUMBER) & 0xffff;
    for (int i = 0; i < 4; i++) {
        frame(qts);
    }
    g_assert_cmphex(ohci_readl(qts, HC_INTR_STATUS) & INTR_UE, ==, 0);
    g_assert_cmpuint((ohci_readl(qts, HC_FM_NUMBER) - fm) & 0xffff, ==, 4);
    g_assert_cmphex(mem_rd32(qts, ED_ADDR + 8) & ~0xf, ==, TD_ADDR(y));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine("powermac7_3") ||
        !qtest_has_device("pci-ohci") || !qtest_has_device("usb-kbd")) {
        g_test_skip("powermac7_3, pci-ohci or usb-kbd not available");
        return g_test_run();
    }

    qtest_add_func("/ohci-td-limit/chain-33", test_chain_33);
    qtest_add_func("/ohci-td-limit/chain-34", test_chain_34);
    qtest_add_func("/ohci-td-limit/chain-35", test_chain_35);
    qtest_add_func("/ohci-td-limit/chain-99", test_chain_99);
    qtest_add_func("/ohci-td-limit/chain-frames", test_chain_frames);
    qtest_add_func("/ohci-td-limit/endless-td", test_endless_td);

    return g_test_run();
}
