/*
 * QTest testcase for the K2 UATA PCI device (106b:0043) on powermac7_3
 *
 * Firmware does not run under qtest, so the HT bridge at dev 3 of the
 * x-ht-k2-layout=dev1-5 layout gets its bus numbers and memory window by
 * hand.  A real boot ROM numbers that bridge's secondary bus 2 and looks for
 * ata-6 at 02:0d.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "hw/pci/pci_regs.h"

#define U3_HT_CFG_BASE   0xf2000000ULL
#define U3_HT_MEM_BASE   0xfa000000ULL

#define CFA0(devfn, off)  (((devfn) << 8) | (off))
#define CFA1(bus, devfn, off)  (CFA0(devfn, off) + ((bus) << 16) + 0x01000000)

#define BRIDGE_DEVFN     (3 << 3)
#define UATA_BUS         2
#define UATA_DEVFN       (0xd << 3)
#define UATA_CFG(off)    (U3_HT_CFG_BASE + CFA1(UATA_BUS, UATA_DEVFN, (off)))

#define UATA_BAR         (U3_HT_MEM_BASE + 0x100000)
#define UATA_BAR_SIZE    0x4000
#define UATA_DBDMA       (UATA_BAR + 0x1000)
#define UATA_ATA         (UATA_BAR + 0x2000)
#define ATA_REG(n)       (UATA_ATA + ((n) << 4))
#define ATA_INTREG       (UATA_ATA + 0x300)

#define ATA_NSECTOR      2
#define ATA_DEVICE       6
#define ATA_CMD          7
#define ATA_CMD_READ_DMA 0xc8
#define ATA_STAT_ERR     0x01

/* The inputs of the ROM's finish-pci-interrupts for k2-uata */
#define MPIC_UATA_DMA    0x26
#define MPIC_UATA_ATA    0x27
#define MPIC_HT_INTA     0x1f

#define DBDMA_CONTROL    0x00
#define DBDMA_STATUS     0x04
#define DBDMA_CMDPTR_LO  0x0c
#define DBDMA_WAIT_SEL   0x18
#define DBDMA_RUN        0x8000
#define DBDMA_ACTIVE     0x0400

#define CMD_INPUT_LAST   0x3000
#define CMD_NOP          0x6000
#define CMD_STOP         0x7000
#define KEY_STREAM0      0x0000
#define INTR_ALWAYS      0x0030
#define WAIT_IFSET       0x0001

#define PROG_ADDR        0x100000ULL
#define DATA_ADDR        0x110000ULL

#define MACHINE          "-machine powermac7_3,x-ht-k2-layout=dev1-5,"
#define DISK             "-blockdev driver=null-co,node-name=d0," \
                         "read-zeroes=on,size=1048576 " \
                         "-device ide-hd,bus=ide.2,drive=d0 "

static void bring_up(QTestState *qts)
{
    uint64_t cfg = U3_HT_CFG_BASE;

    qtest_writeb(qts, cfg + CFA0(BRIDGE_DEVFN, PCI_PRIMARY_BUS), 0);
    qtest_writeb(qts, cfg + CFA0(BRIDGE_DEVFN, PCI_SECONDARY_BUS), UATA_BUS);
    qtest_writeb(qts, cfg + CFA0(BRIDGE_DEVFN, PCI_SUBORDINATE_BUS), UATA_BUS);
    qtest_writew(qts, cfg + CFA0(BRIDGE_DEVFN, PCI_MEMORY_BASE),
                 bswap16(0xfa00));
    qtest_writew(qts, cfg + CFA0(BRIDGE_DEVFN, PCI_MEMORY_LIMIT),
                 bswap16(0xfaf0));
    qtest_writew(qts, cfg + CFA0(BRIDGE_DEVFN, PCI_COMMAND),
                 bswap16(PCI_COMMAND_MEMORY));
    qtest_writel(qts, UATA_CFG(PCI_BASE_ADDRESS_0),
                 bswap32((uint32_t)UATA_BAR));
    qtest_writew(qts, UATA_CFG(PCI_COMMAND),
                 bswap16(PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER));
}

static QTestState *uata_init(const char *extra)
{
    QTestState *qts = qtest_initf(MACHINE "x-k2-uata=on %s", extra);

    bring_up(qts);
    qtest_irq_intercept_in(qts, "pic");
    return qts;
}

static void test_config(void)
{
    QTestState *qts = uata_init("");
    uint32_t bar;

    g_assert_cmphex(bswap32(qtest_readl(qts, UATA_CFG(PCI_VENDOR_ID))), ==,
                    0x0043106b);
    /* The ROM's uata takes its interrupts from the tree, not from a pin */
    g_assert_cmphex(qtest_readb(qts, UATA_CFG(PCI_INTERRUPT_PIN)), ==, 0);

    qtest_writel(qts, UATA_CFG(PCI_BASE_ADDRESS_0), 0xffffffff);
    bar = bswap32(qtest_readl(qts, UATA_CFG(PCI_BASE_ADDRESS_0)));
    g_assert_cmphex(bar & PCI_BASE_ADDRESS_MEM_MASK, ==,
                    (uint32_t)-UATA_BAR_SIZE);
    qtest_quit(qts);
}

static void test_off_by_default(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3,x-ht-k2-layout=dev1-5");

    bring_up(qts);
    g_assert_cmphex(qtest_readl(qts, UATA_CFG(PCI_VENDOR_ID)), ==, 0xffffffff);
    qtest_quit(qts);
}

/* x-k2-uata needs the bridge the ROM looks behind */
static void test_needs_layout(void)
{
    g_autofree char *cmdline = NULL;
    g_autofree char *err = NULL;
    g_auto(GStrv) argv = NULL;
    int status = 0;

    cmdline = g_strdup_printf("%s -M powermac7_3,x-k2-uata=on -accel qtest "
                              "-display none", qtest_qemu_binary(NULL));
    g_assert_true(g_shell_parse_argv(cmdline, NULL, &argv, NULL));
    g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH |
                               G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL,
                               NULL, &err, &status, NULL));
    g_assert_cmpint(status, !=, 0);
    g_assert_nonnull(strstr(err, "x-k2-uata"));
}

static void test_registers(void)
{
    QTestState *qts = uata_init(DISK);

    /* The ROM writes the FCR and the kext reads it back */
    qtest_writel(qts, UATA_BAR, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, UATA_BAR), ==, 0x12345678);

    /* The DBDMA window: the status of an idle channel */
    g_assert_cmphex(bswap32(qtest_readl(qts, UATA_DBDMA + DBDMA_STATUS)) &
                    DBDMA_RUN, ==, 0);

    /* The ATA window: a task file register written and read back */
    qtest_writeb(qts, ATA_REG(ATA_DEVICE), 0xe0);
    qtest_writeb(qts, ATA_REG(ATA_NSECTOR), 0x5a);
    g_assert_cmphex(qtest_readb(qts, ATA_REG(ATA_NSECTOR)), ==, 0x5a);
    qtest_quit(qts);
}

static void put_cmd(QTestState *qts, uint64_t addr, uint16_t req, uint16_t cmd,
                    uint32_t phy)
{
    qtest_writew(qts, addr, bswap16(req));
    qtest_writew(qts, addr + 2, bswap16(cmd));
    qtest_writel(qts, addr + 4, bswap32(phy));
    qtest_writel(qts, addr + 8, 0);
    qtest_writel(qts, addr + 12, 0);
}

static uint32_t dbdma_status(QTestState *qts)
{
    return bswap32(qtest_readl(qts, UATA_DBDMA + DBDMA_STATUS));
}

static void dbdma_wait_idle(QTestState *qts)
{
    int i;

    for (i = 0; i < 1000 && (dbdma_status(qts) & DBDMA_ACTIVE); i++) {
        qtest_clock_step(qts, 1000000);
    }
    g_assert_cmphex(dbdma_status(qts) & DBDMA_ACTIVE, ==, 0);
}

/*
 * The shape of AppleKauaiATA's program: the last data command waits for s7,
 * which is the ATA interrupt line, then a NOP raises the DMA interrupt.
 */
static void test_read_dma(void)
{
    QTestState *qts = uata_init(DISK);
    uint8_t buf[512];

    memset(buf, 0xa5, sizeof(buf));
    qtest_memwrite(qts, DATA_ADDR, buf, sizeof(buf));
    put_cmd(qts, PROG_ADDR, sizeof(buf),
            CMD_INPUT_LAST | KEY_STREAM0 | WAIT_IFSET, DATA_ADDR);
    put_cmd(qts, PROG_ADDR + 16, 0, CMD_NOP | INTR_ALWAYS, 0);
    put_cmd(qts, PROG_ADDR + 32, 0, CMD_STOP, 0);

    g_assert_false(qtest_get_irq(qts, MPIC_UATA_ATA));
    g_assert_false(qtest_get_irq(qts, MPIC_UATA_DMA));

    qtest_writel(qts, UATA_DBDMA + DBDMA_CMDPTR_LO,
                 bswap32((uint32_t)PROG_ADDR));
    /* wait while s7 is 0 */
    qtest_writel(qts, UATA_DBDMA + DBDMA_WAIT_SEL, bswap32(0x00800000));
    qtest_writel(qts, UATA_DBDMA + DBDMA_CONTROL,
                 bswap32((DBDMA_RUN << 16) | DBDMA_RUN));

    qtest_writeb(qts, ATA_REG(ATA_DEVICE), 0xe0);
    qtest_writeb(qts, ATA_REG(ATA_NSECTOR), 1);
    qtest_writeb(qts, ATA_REG(ATA_CMD), ATA_CMD_READ_DMA);
    dbdma_wait_idle(qts);

    /* read-zeroes: the data went through the channel to guest RAM */
    qtest_memread(qts, DATA_ADDR, buf, sizeof(buf));
    g_assert_cmphex(buf[0], ==, 0);
    g_assert_cmphex(buf[511], ==, 0);

    g_assert_true(qtest_get_irq(qts, MPIC_UATA_ATA));
    g_assert_true(qtest_get_irq(qts, MPIC_UATA_DMA));
    g_assert_false(qtest_get_irq(qts, MPIC_HT_INTA));
    /* The ROM reads the DMA/ATA bits here with lwz */
    g_assert_cmphex(bswap32(qtest_readl(qts, ATA_INTREG)) & 0xc0000000, ==,
                    0xc0000000);

    /* Reading status acknowledges the drive; ATA goes down, s7 with it */
    g_assert_cmphex(qtest_readb(qts, ATA_REG(ATA_CMD)) & ATA_STAT_ERR, ==, 0);
    g_assert_false(qtest_get_irq(qts, MPIC_UATA_ATA));
    g_assert_cmphex(dbdma_status(qts) & 0x80, ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("k2-uata/config", test_config);
    qtest_add_func("k2-uata/off-by-default", test_off_by_default);
    qtest_add_func("k2-uata/needs-layout", test_needs_layout);
    qtest_add_func("k2-uata/registers", test_registers);
    qtest_add_func("k2-uata/read-dma", test_read_dma);

    return g_test_run();
}
