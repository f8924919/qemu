/*
 * The NMI line of the NewWorld MacIO GPIO block (GPIO 9, x-gpio9-idle-high)
 *
 * QEMU treats GPIO 9 as the NMI line and leaves it low, so a real
 * PowerMac G5 boot ROM reading byte 0x61 of mac-io takes the NMI button
 * as held down.  With the powermac7_3 x-gpio9-idle-high switch on, reset
 * leaves the line high without touching the interrupt and an NMI pulls it
 * low and lets it go again; the interrupt follows "pressed", so the guest
 * still sees one raise and one lower.  Off, nothing changes.
 *
 * libqtest only keeps the last level of a line, so the raise and lower
 * are read off the mpic: an edge source hands out the vector once, a
 * level source has nothing left pending, and the input ends up low.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "hw/pci/pci_regs.h"
#include "hw/ppc/openpic.h"

#define MACHINE_ON      "-machine powermac7_3,x-gpio9-idle-high=on"
#define MACHINE_OFF     "-machine powermac7_3"
#define PROP            "x-gpio9-idle-high"

/* powermac7_3: K2 bridge at bus 0 / dev 3, macio behind it at dev 7 */
#define U3_HT_CFG_BASE  0xf2000000ULL
#define K2_DEVFN        (3 << 3)
#define K2_MACIO_DEVFN  (7 << 3)
#define CFA0(devfn, off)        (((devfn) << 8) | (off))
#define CFA1(bus, devfn, off)   (CFA0(devfn, off) + ((bus) << 16) + 0x01000000)
#define K2_MACIO_BAR    0xfa000000ULL

/* mac99 (970): macio on the U3 AGP bus, slot chosen at creation */
#define UNIN_CFG_ADDR   0xf0800000ULL
#define UNIN_CFG_DATA   0xf0c00000ULL
#define MAC99_MACIO_BAR 0x80000000ULL

/* GPIO 9 in the macio BAR (GPIO window at 0x50, levels first) */
#define GPIO9           0x61
#define GPIO_IN_DATA    0x02
#define GPIO_OUT_ENABLE 0x04

/* powermac7_3 mpic (powermac7_3-smp-test) and the GPIO 9 input */
#define MPIC_BASE       0xf8040000ULL
#define MPIC_SPVE       (MPIC_BASE + 0x10e0)
#define MPIC_IVPR(n)    (MPIC_BASE + 0x10000 + (n) * 0x20)
#define MPIC_IDR(n)     (MPIC_IVPR(n) + 0x10)
#define MPIC_CPU0_CTPR  (MPIC_BASE + 0x20080)
#define MPIC_CPU0_IACK  (MPIC_BASE + 0x200a0)
#define MPIC_CPU0_EOI   (MPIC_BASE + 0x200b0)
#define IRQ_GPIO9       0x37

static void le_writel(QTestState *qts, uint64_t addr, uint32_t val)
{
    qtest_writel(qts, addr, bswap32(val));
}

static uint32_t le_readl(QTestState *qts, uint64_t addr)
{
    return bswap32(qtest_readl(qts, addr));
}

static void le_writew(QTestState *qts, uint64_t addr, uint16_t val)
{
    qtest_writew(qts, addr, bswap16(val));
}

/* Firmware does not run under qtest: place the macio BAR by hand */
static void k2_bring_up(QTestState *qts)
{
    qtest_writeb(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x18), 0);
    qtest_writeb(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x19), 1);
    qtest_writeb(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x1a), 1);
    le_writew(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x20), 0xfa00);
    le_writew(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x22), 0xfaf0);
    le_writew(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 4), PCI_COMMAND_MEMORY);

    le_writel(qts, U3_HT_CFG_BASE + CFA1(1, K2_MACIO_DEVFN,
                                         PCI_BASE_ADDRESS_0),
              (uint32_t)K2_MACIO_BAR);
    le_writew(qts, U3_HT_CFG_BASE + CFA1(1, K2_MACIO_DEVFN, PCI_COMMAND),
              PCI_COMMAND_MEMORY);
}

static QTestState *k2_init(const char *machine)
{
    QTestState *qts = qtest_init(machine);

    k2_bring_up(qts);
    qtest_irq_intercept_in(qts, "pic");
    return qts;
}

static uint8_t k2_gpio9(QTestState *qts)
{
    return qtest_readb(qts, K2_MACIO_BAR + GPIO9);
}

static void inject_nmi(QTestState *qts)
{
    qtest_qmp_assert_success(qts, "{ 'execute': 'inject-nmi' }");
}

/* GPIO 9 unmasked to CPU 0, edge or level, above CTPR */
static void mpic_gpio9_source(QTestState *qts, bool level)
{
    le_writel(qts, MPIC_CPU0_CTPR, 0);
    le_writel(qts, MPIC_IDR(IRQ_GPIO9), 1 << 0);
    le_writel(qts, MPIC_IVPR(IRQ_GPIO9),
              (level ? IVPR_SENSE_MASK : 0) | (5 << 16) | IRQ_GPIO9);
}

static uint32_t mpic_iack(QTestState *qts)
{
    return le_readl(qts, MPIC_CPU0_IACK) & 0xff;
}

static uint32_t mpic_spurious(QTestState *qts)
{
    return le_readl(qts, MPIC_SPVE) & 0xff;
}

static bool machine_has_prop(QTestState *qts)
{
    QDict *resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments':"
                                 " { 'path': '/machine', 'property': %s } }",
                            PROP);
    bool found = qdict_haskey(resp, "return");

    qobject_unref(resp);
    return found;
}

/* Reset leaves the line high on, low off, and the interrupt alone */
static void reset_level(const char *machine, uint8_t want)
{
    QTestState *qts = k2_init(machine);

    g_assert_cmphex(k2_gpio9(qts), ==, want);
    g_assert_false(qtest_get_irq(qts, IRQ_GPIO9));

    mpic_gpio9_source(qts, false);
    g_assert_cmphex(mpic_iack(qts), ==, mpic_spurious(qts));
    qtest_quit(qts);
}

static void test_reset_on(void)
{
    reset_level(MACHINE_ON, GPIO_IN_DATA);
}

static void test_reset_off(void)
{
    reset_level(MACHINE_OFF, 0);
}

/*
 * An edge source takes the vector once per NMI, the line is back where it
 * idles, and the input is low again; twice, so a line left at the wrong
 * level by the first NMI shows on the second.
 */
static void nmi_edge(const char *machine, uint8_t idle)
{
    QTestState *qts = k2_init(machine);
    int i;

    mpic_gpio9_source(qts, false);
    for (i = 0; i < 2; i++) {
        inject_nmi(qts);
        g_assert_cmphex(k2_gpio9(qts), ==, idle);
        g_assert_false(qtest_get_irq(qts, IRQ_GPIO9));
        g_assert_cmphex(mpic_iack(qts), ==, IRQ_GPIO9);
        le_writel(qts, MPIC_CPU0_EOI, 0);
        g_assert_cmphex(mpic_iack(qts), ==, mpic_spurious(qts));
    }
    qtest_quit(qts);
}

static void test_nmi_edge_on(void)
{
    nmi_edge(MACHINE_ON, GPIO_IN_DATA);
}

static void test_nmi_edge_off(void)
{
    nmi_edge(MACHINE_OFF, 0);
}

/* A level source has nothing left once the NMI is over: raised, then lowered */
static void nmi_level(const char *machine, uint8_t idle)
{
    QTestState *qts = k2_init(machine);

    mpic_gpio9_source(qts, true);
    inject_nmi(qts);
    g_assert_cmphex(k2_gpio9(qts), ==, idle);
    g_assert_false(qtest_get_irq(qts, IRQ_GPIO9));
    g_assert_cmphex(le_readl(qts, MPIC_IVPR(IRQ_GPIO9)) & IVPR_ACTIVITY_MASK,
                    ==, 0);
    g_assert_cmphex(mpic_iack(qts), ==, mpic_spurious(qts));
    qtest_quit(qts);
}

static void test_nmi_level_on(void)
{
    nmi_level(MACHINE_ON, GPIO_IN_DATA);
}

static void test_nmi_level_off(void)
{
    nmi_level(MACHINE_OFF, 0);
}

/*
 * A guest that drove GPIO 9 low before a system_reset: on, the line comes
 * back as the input high and nothing else; off keeps what the guest wrote
 * (as before this switch existed).
 */
static void reset_after_write(const char *machine, uint8_t want)
{
    QTestState *qts = k2_init(machine);

    qtest_writeb(qts, K2_MACIO_BAR + GPIO9, GPIO_OUT_ENABLE);
    g_assert_cmphex(k2_gpio9(qts), ==, GPIO_OUT_ENABLE);

    qtest_system_reset(qts);
    k2_bring_up(qts);
    g_assert_cmphex(k2_gpio9(qts), ==, want);
    g_assert_false(qtest_get_irq(qts, IRQ_GPIO9));
    qtest_quit(qts);
}

static void test_system_reset_on(void)
{
    reset_after_write(MACHINE_ON, GPIO_IN_DATA);
}

static void test_system_reset_off(void)
{
    reset_after_write(MACHINE_OFF, GPIO_OUT_ENABLE);
}

static void test_prop(void)
{
    QTestState *qts = qtest_init(MACHINE_OFF);

    g_assert_true(machine_has_prop(qts));
    qtest_quit(qts);

    qts = qtest_init("-machine mac99,via=pmu");
    g_assert_false(machine_has_prop(qts));
    qtest_quit(qts);
}

/*
 * mac99 with a PMU has the same GPIO block and no switch: the line stays
 * low across reset and an NMI.  The macio slot is chosen at creation, so
 * find it through QOM and reach it with a type 0 cycle on the U3 AGP bus.
 */
static int mac99_macio_slot(QTestState *qts)
{
    int i;

    for (i = 0; i < 64; i++) {
        g_autofree char *path = g_strdup_printf(
            "/machine/unattached/device[%d]", i);
        QDict *resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments':"
                                     " { 'path': %s, 'property': 'type' } }",
                                path);
        bool hit = qdict_haskey(resp, "return") &&
                   g_str_equal(qdict_get_str(resp, "return"), "macio-newworld");
        int addr = -1;

        qobject_unref(resp);
        if (!hit) {
            continue;
        }
        resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments':"
                              " { 'path': %s, 'property': 'addr' } }", path);
        addr = qdict_get_int(resp, "return");
        qobject_unref(resp);
        return addr >> 3;
    }
    return -1;
}

static void unin_cfg_writel(QTestState *qts, int slot, int off, uint32_t val)
{
    /* The low three bits of the offset come from the data address */
    le_writel(qts, UNIN_CFG_ADDR, (1u << slot) | (off & ~7));
    le_writel(qts, UNIN_CFG_DATA + (off & 7), val);
}

static void test_mac99_pmu_unchanged(void)
{
    QTestState *qts = qtest_init("-machine mac99,via=pmu");
    int slot = mac99_macio_slot(qts);

    g_assert_cmpint(slot, >=, 11);
    unin_cfg_writel(qts, slot, PCI_BASE_ADDRESS_0, (uint32_t)MAC99_MACIO_BAR);
    unin_cfg_writel(qts, slot, PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_irq_intercept_in(qts, "pic");

    g_assert_cmphex(qtest_readb(qts, MAC99_MACIO_BAR + GPIO9), ==, 0);
    inject_nmi(qts);
    g_assert_cmphex(qtest_readb(qts, MAC99_MACIO_BAR + GPIO9), ==, 0);
    g_assert_false(qtest_get_irq(qts, IRQ_GPIO9));

    /* The zeros above came from the GPIO block: a driven 1 reads back */
    qtest_writeb(qts, MAC99_MACIO_BAR + GPIO9, GPIO_OUT_ENABLE | 0x01);
    g_assert_cmphex(qtest_readb(qts, MAC99_MACIO_BAR + GPIO9), ==,
                    GPIO_OUT_ENABLE | GPIO_IN_DATA | 0x01);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/macio-gpio-nmi/reset/on", test_reset_on);
    qtest_add_func("/macio-gpio-nmi/reset/off", test_reset_off);
    qtest_add_func("/macio-gpio-nmi/nmi-edge/on", test_nmi_edge_on);
    qtest_add_func("/macio-gpio-nmi/nmi-edge/off", test_nmi_edge_off);
    qtest_add_func("/macio-gpio-nmi/nmi-level/on", test_nmi_level_on);
    qtest_add_func("/macio-gpio-nmi/nmi-level/off", test_nmi_level_off);
    qtest_add_func("/macio-gpio-nmi/system-reset/on", test_system_reset_on);
    qtest_add_func("/macio-gpio-nmi/system-reset/off", test_system_reset_off);
    qtest_add_func("/macio-gpio-nmi/prop", test_prop);
    qtest_add_func("/macio-gpio-nmi/mac99-pmu", test_mac99_pmu_unchanged);

    return g_test_run();
}
