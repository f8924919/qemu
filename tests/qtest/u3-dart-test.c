/*
 * QTest testcase for the U3 DART of powermac7_3
 *
 * The DART is the IOMMU between the U3 and its PCI buses.  powermac7_3
 * only has one when it is given RAM above 4 GiB (-M powermac7_3,highmem=on
 * with more than 2 GiB, or x-dart=on), so that 32-bit DMA engines such as
 * the mac-io DBDMA can reach that RAM.  Without it, Linux fails disk reads
 * with "DMA addr ... overflow" and Mac OS X 10.4.0 truncates the address.
 *
 * The register layout follows Linux (arch/powerpc/sysdev/dart.h) and
 * Apple's AppleU3ART driver: DART_CNTL holds the table base page in
 * [31:12], FLUSHTLB 0x400, ENABLE 0x200 and the table size in pages in
 * [8:0], where 0 stands for 512.  Table entries are big-endian, with
 * 0x80000000 for valid and the physical page number in [23:0].
 *
 * The DMA tests run the K2 sound input channel (DBDMA 2) through the DART:
 * it reads its descriptor, zero-fills its buffer and writes the status back,
 * all three through the address space that the DART translates.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "hw/pci/pci_regs.h"

#define DART_BASE         0xf8033000ULL
#define DART_CNTL         (DART_BASE + 0x00)
#define DART_EXCP         (DART_BASE + 0x10)
#define DART_TAGS         (DART_BASE + 0x1000)

#define DART_CNTL_FLUSHTLB 0x400
#define DART_CNTL_ENABLE   0x200
#define DART_CNTL_SIZE     0x1ff
#define DARTMAP_VALID      0x80000000

/* The table: 2 MiB (512 pages of entries), 16 MiB aligned, below 2 GiB */
#define TABLE_ADDR        0x01000000ULL

#define U3_HT_CFG_BASE    0xf2000000ULL
#define K2_DEVFN          (3 << 3)
#define MACIO_DEVFN       (7 << 3)
#define CFA0(devfn, off)  (((devfn) << 8) | (off))
#define CFA1(bus, devfn, off)  (CFA0(devfn, off) + ((bus) << 16) + 0x01000000)
#define MACIO_BAR         0xfa000000ULL

#define K2_FCR1           (MACIO_BAR + 0x3c)
#define K2_FCR1_I2S0_BITS 0x00003400
#define DBDMA_CH(n)       (MACIO_BAR + 0x8000 + ((n) << 7))
#define DBDMA_CONTROL     0x00
#define DBDMA_CMDPTR_LO   0x0c
#define DBDMA_RUN         0x8000
#define INPUT_LAST        0x3000
#define DBDMA_STOP        0x7000

/* Bus addresses the DBDMA is told to use, and where the DART sends them */
#define DESC_BUS          0x00100000ULL
#define BUF_BUS           0x00200000ULL
#define DESC_PHYS         0x100010000ULL     /* above 4 GiB */
#define BUF_PHYS          0x100020000ULL
#define BUF_LEN           256

static void le_writel(QTestState *qts, uint64_t addr, uint32_t val)
{
    qtest_writel(qts, addr, bswap32(val));
}

static void cfg_writew(QTestState *qts, uint64_t addr, uint16_t val)
{
    qtest_writew(qts, addr, bswap16(val));
}

/* As in k2-sound-test: open the K2 bridge window and place the macio BAR */
static void k2_bring_up(QTestState *qts)
{
    qtest_writeb(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x18), 0);
    qtest_writeb(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x19), 1);
    qtest_writeb(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x1a), 1);
    cfg_writew(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x20), 0xfa00);
    cfg_writew(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x22), 0xfaf0);
    cfg_writew(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 4), PCI_COMMAND_MEMORY);

    le_writel(qts, U3_HT_CFG_BASE + CFA1(1, MACIO_DEVFN, PCI_BASE_ADDRESS_0),
              (uint32_t)MACIO_BAR);
    cfg_writew(qts, U3_HT_CFG_BASE + CFA1(1, MACIO_DEVFN, PCI_COMMAND),
               PCI_COMMAND_MEMORY);
}

static bool has_dart(const char *machine_args)
{
    QTestState *qts = qtest_initf("-machine %s", machine_args);
    g_autofree char *tree = qtest_hmp(qts, "info qtree");
    bool found = strstr(tree, "u3-dart") != NULL;

    qtest_quit(qts);
    return found;
}

/* No DART unless the RAM goes above 4 GiB, or x-dart=on asks for one */
static void test_presence(void)
{
    g_assert_false(has_dart("powermac7_3"));
    g_assert_false(has_dart("powermac7_3,highmem=on -m 2048"));
    g_assert_true(has_dart("powermac7_3,highmem=on -m 3072"));
    g_assert_false(has_dart("powermac7_3,highmem=on,x-dart=off -m 3072"));
    g_assert_true(has_dart("powermac7_3,x-dart=on"));
}

static QTestState *dart_init(void)
{
    return qtest_init("-machine powermac7_3,highmem=on -m 3072");
}

static void test_registers(void)
{
    QTestState *qts = dart_init();
    uint32_t cntl = (uint32_t)TABLE_ADDR | DART_CNTL_ENABLE | 0x40;

    /* Reset leaves it disabled */
    g_assert_cmphex(qtest_readl(qts, DART_CNTL), ==, 0);
    g_assert_cmphex(qtest_readl(qts, DART_EXCP), ==, 0);

    qtest_writel(qts, DART_CNTL, cntl);
    g_assert_cmphex(qtest_readl(qts, DART_CNTL), ==, cntl);

    /* A TLB flush completes at once: Linux polls for the bit to drop */
    qtest_writel(qts, DART_CNTL, cntl | DART_CNTL_FLUSHTLB);
    g_assert_cmphex(qtest_readl(qts, DART_CNTL), ==, cntl);

    /* Nothing else is modelled: reads as zero, writes are dropped */
    qtest_writel(qts, DART_TAGS, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, DART_TAGS), ==, 0);

    qtest_quit(qts);
}

/* Map one bus page to one physical page */
static void dart_map(QTestState *qts, uint64_t bus, uint64_t phys)
{
    qtest_writel(qts, TABLE_ADDR + (bus >> 12) * 4,
                 DARTMAP_VALID | (uint32_t)(phys >> 12));
}

/* Enable with a 512-page table: the size field is then 0, as Linux has it */
static void dart_enable(QTestState *qts)
{
    uint32_t size = 512 & DART_CNTL_SIZE;

    qtest_writel(qts, DART_CNTL,
                 (uint32_t)TABLE_ADDR | DART_CNTL_ENABLE | size);
    qtest_writel(qts, DART_CNTL,
                 (uint32_t)TABLE_ADDR | DART_CNTL_ENABLE | DART_CNTL_FLUSHTLB);
}

/* One input descriptor at DESC_PHYS, followed by STOP, then run channel 2 */
static void run_input(QTestState *qts)
{
    uint8_t desc[32] = { 0 };

    stw_le_p(desc + 0, BUF_LEN);
    stw_le_p(desc + 2, INPUT_LAST);
    stl_le_p(desc + 4, BUF_BUS);
    stw_le_p(desc + 16 + 2, DBDMA_STOP);
    qtest_memwrite(qts, DESC_PHYS, desc, sizeof(desc));

    le_writel(qts, K2_FCR1, K2_FCR1_I2S0_BITS);
    le_writel(qts, DBDMA_CH(2) + DBDMA_CMDPTR_LO, (uint32_t)DESC_BUS);
    le_writel(qts, DBDMA_CH(2) + DBDMA_CONTROL,
              (DBDMA_RUN << 16) | DBDMA_RUN);
    qtest_clock_step(qts, 100 * 1000 * 1000);
}

/*
 * The DBDMA follows the table: it fetches its descriptor and writes the
 * buffer and the status at the physical pages above 4 GiB, not at the bus
 * addresses it was given.
 */
static void test_dbdma_through_dart(void)
{
    QTestState *qts = dart_init();
    uint8_t buf[BUF_LEN], low[BUF_LEN], desc[16];
    int i;

    k2_bring_up(qts);
    memset(buf, 0xaa, sizeof(buf));
    qtest_memwrite(qts, BUF_PHYS, buf, sizeof(buf));
    qtest_memwrite(qts, BUF_BUS, buf, sizeof(buf));

    dart_map(qts, DESC_BUS, DESC_PHYS);
    dart_map(qts, BUF_BUS, BUF_PHYS);
    dart_enable(qts);
    run_input(qts);

    /* The descriptor retired: its status came back through the DART */
    qtest_memread(qts, DESC_PHYS, desc, sizeof(desc));
    g_assert_cmphex(lduw_le_p(desc + 14) & DBDMA_RUN, ==, DBDMA_RUN);

    /* The silent input landed in the high page ... */
    qtest_memread(qts, BUF_PHYS, buf, sizeof(buf));
    for (i = 0; i < BUF_LEN; i++) {
        g_assert_cmphex(buf[i], ==, 0);
    }
    /* ... and not at the bus address */
    qtest_memread(qts, BUF_BUS, low, sizeof(low));
    for (i = 0; i < BUF_LEN; i++) {
        g_assert_cmphex(low[i], ==, 0xaa);
    }
    g_assert_cmphex(qtest_readl(qts, DART_EXCP), ==, 0);

    qtest_quit(qts);
}

/* An access through a page without a valid entry is recorded in DART_EXCP */
static void test_fault_recorded(void)
{
    QTestState *qts = dart_init();

    k2_bring_up(qts);
    dart_map(qts, BUF_BUS, BUF_PHYS);
    /* no entry for the descriptor page */
    dart_enable(qts);
    run_input(qts);

    g_assert_cmphex(qtest_readl(qts, DART_EXCP), ==, DESC_BUS >> 12);
    qtest_writel(qts, DART_EXCP, 0);
    g_assert_cmphex(qtest_readl(qts, DART_EXCP), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine("powermac7_3")) {
        g_test_skip("powermac7_3 machine not available");
        return 0;
    }

    qtest_add_func("/u3-dart/presence", test_presence);
    qtest_add_func("/u3-dart/registers", test_registers);
    qtest_add_func("/u3-dart/dbdma-through-dart", test_dbdma_through_dart);
    qtest_add_func("/u3-dart/fault-recorded", test_fault_recorded);

    return g_test_run();
}
