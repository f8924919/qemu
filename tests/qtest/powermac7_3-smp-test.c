/*
 * QTest testcase for PowerMac7,3 SMP (dual CPU)
 *
 * The real PowerMac7,3 is a dual CPU machine.  Three things have to line up
 * before the guest sees the second processor, and each of them can fail
 * silently, so they are pinned down separately here:
 *
 *  - the machine has to admit to more than one CPU.  mc->max_cpus lives in
 *    core99_machine_class_init(), which mac99 shares, so raising it in the
 *    wrong place would quietly turn mac99 into an SMP machine as well.
 *
 *  - the KeyLargo MPIC has to accept more than one destination.  It used to
 *    refuse outright ("Only UP supported today"), and because macio never
 *    passed a CPU count down, the interrupt controller and the board
 *    disagreed about how many output lines existed: the board connected
 *    OPENPIC_OUTPUT_NB lines per CPU while the MPIC had only allocated one
 *    CPU's worth.  That mismatch aborts before the guest runs a single
 *    instruction, so a plain "does it boot" test never reaches the firmware.
 *
 *  - the secondary has to come up held.  Linux kicks it by patching the
 *    reset vector at physical 0x100 and pulsing a macio GPIO, so a secondary
 *    that started executing the ROM on its own would be past that vector
 *    long before the guest ever asked for it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "hw/ppc/openpic.h"

/* The hardware is a 2-way machine; mac99 stays uniprocessor. */
#define POWERMAC7_3_MAX_CPUS  2
#define MAC99_MAX_CPUS        1

static char *qom_get_str(QTestState *qts, const char *path, const char *prop)
{
    QDict *resp;
    char *val = NULL;

    resp = qtest_qmp(qts,
                     "{ 'execute': 'qom-get',"
                     "  'arguments': { 'path': %s, 'property': %s } }",
                     path, prop);
    if (qdict_haskey(resp, "return")) {
        val = g_strdup(qdict_get_str(resp, "return"));
    }
    qobject_unref(resp);
    return val;
}

static bool qom_get_bool(QTestState *qts, const char *path, const char *prop,
                         bool *found)
{
    QDict *resp;
    bool val = false;

    *found = false;
    resp = qtest_qmp(qts,
                     "{ 'execute': 'qom-get',"
                     "  'arguments': { 'path': %s, 'property': %s } }",
                     path, prop);
    if (qdict_haskey(resp, "return")) {
        val = qdict_get_bool(resp, "return");
        *found = true;
    }
    qobject_unref(resp);
    return val;
}

/*
 * MacIO is created without an id, so it lands somewhere under
 * /machine/unattached with an index that depends on creation order.  Find
 * it by the pic it owns rather than hardcoding that index.
 */
static char *find_macio_pic(QTestState *qts)
{
    int i;

    for (i = 0; i < 64; i++) {
        g_autofree char *path = g_strdup_printf(
            "/machine/unattached/device[%d]/pic", i);
        g_autofree char *type = qom_get_str(qts, path, "type");

        if (type && g_str_equal(type, "openpic")) {
            return g_steal_pointer(&path);
        }
    }
    return NULL;
}

static int64_t machine_cpu_max(QTestState *qts, const char *machine)
{
    QDict *resp;
    QList *list;
    const QListEntry *p;
    int64_t cpu_max = -1;

    resp = qtest_qmp(qts, "{ 'execute': 'query-machines' }");
    g_assert(resp);
    list = qdict_get_qlist(resp, "return");
    g_assert(list);

    for (p = qlist_first(list); p; p = qlist_next(p)) {
        QDict *minfo = qobject_to(QDict, qlist_entry_obj(p));

        g_assert(minfo);
        if (g_str_equal(qdict_get_str(minfo, "name"), machine)) {
            cpu_max = qdict_get_int(minfo, "cpu-max");
            break;
        }
    }

    qobject_unref(resp);
    return cpu_max;
}

/*
 * max_cpus is set on the shared core99 base class, so the PowerMac7,3 and
 * mac99 values have to be checked together: bumping the base class would
 * make the first assertion pass and only the second one would notice.
 */
static void test_cpu_max(void)
{
    QTestState *qts = qtest_init("-machine none");

    g_assert_cmpint(machine_cpu_max(qts, "powermac7_3"), ==,
                    POWERMAC7_3_MAX_CPUS);
    g_assert_cmpint(machine_cpu_max(qts, "mac99"), ==, MAC99_MAX_CPUS);

    qtest_quit(qts);
}

/*
 * Starting at all with -smp 2 is the interesting part: the MPIC used to
 * reject a second destination, and the board/MPIC line count mismatch
 * aborted during realize.
 */
static void test_smp2_starts(void)
{
    QTestState *qts;
    QDict *resp;
    QList *cpus;

    qts = qtest_init("-machine powermac7_3 -smp 2");

    resp = qtest_qmp(qts, "{ 'execute': 'query-cpus-fast' }");
    g_assert(resp);
    cpus = qdict_get_qlist(resp, "return");
    g_assert(cpus);
    g_assert_cmpint(qlist_size(cpus), ==, 2);
    qobject_unref(resp);

    qtest_quit(qts);
}

/*
 * -smp 1 has to keep working unchanged, and it must not grow a second CPU
 * just because the machine is now capable of one.
 */
static void test_smp1_unchanged(void)
{
    QTestState *qts;
    QDict *resp;
    QList *cpus;

    qts = qtest_init("-machine powermac7_3");

    resp = qtest_qmp(qts, "{ 'execute': 'query-cpus-fast' }");
    g_assert(resp);
    cpus = qdict_get_qlist(resp, "return");
    g_assert(cpus);
    g_assert_cmpint(qlist_size(cpus), ==, 1);
    qobject_unref(resp);

    qtest_quit(qts);
}

/*
 * Every MPIC destination has to reach the processor it stands for.  The
 * board wires OPENPIC_OUTPUT_NB lines per CPU, and taking the CPU from
 * outside that loop points all of them at whichever one was created last.
 * The guest still boots, because Linux spreads interrupts over both
 * destinations, but the first processor never sees one -- so check where
 * the lines actually go rather than that the guest survives.
 */
static void test_irq_destinations(void)
{
    QTestState *qts;
    g_autofree char *pic = NULL;
    g_autofree char *dest0 = NULL;
    g_autofree char *dest1 = NULL;
    g_autofree char *line1 = NULL;

    qts = qtest_init("-machine powermac7_3 -smp 2");

    pic = find_macio_pic(qts);
    g_assert_nonnull(pic);

    line1 = g_strdup_printf("sysbus-irq[%d]", OPENPIC_OUTPUT_NB);
    dest0 = qom_get_str(qts, pic, "sysbus-irq[0]");
    dest1 = qom_get_str(qts, pic, line1);

    g_assert_nonnull(dest0);
    g_assert_nonnull(dest1);
    g_assert_cmpstr(dest0, !=, dest1);

    qtest_quit(qts);
}

/*
 * Every processor but the boot one has to come up held: an OS releases a
 * secondary by leaving a branch at the reset vector and pulsing a GPIO, so
 * one that started running the firmware on its own would be long past that
 * vector before it was ever asked for.  A machine like that looks healthy
 * right up until an OS tries to start its second processor.
 */
static void test_secondary_held(void)
{
    QTestState *qts;
    QDict *resp;
    QList *cpus;
    const QListEntry *e;
    int held_count = 0;

    qts = qtest_init("-machine powermac7_3 -smp 2");

    resp = qtest_qmp(qts, "{ 'execute': 'query-cpus-fast' }");
    g_assert(resp);
    cpus = qdict_get_qlist(resp, "return");
    g_assert(cpus);

    for (e = qlist_first(cpus); e; e = qlist_next(e)) {
        QDict *cpu = qobject_to(QDict, qlist_entry_obj(e));
        g_autofree char *path = NULL;
        bool found;

        g_assert(cpu);
        path = g_strdup(qdict_get_str(cpu, "qom-path"));

        if (qom_get_bool(qts, path, "start-powered-off", &found)) {
            held_count++;
            /* Never the one that runs the firmware. */
            g_assert_cmpint(qdict_get_int(cpu, "cpu-index"), !=, 0);
        }
        g_assert(found);
    }
    qobject_unref(resp);

    /* Every processor but the boot one. */
    g_assert_cmpint(held_count, ==, 1);

    qtest_quit(qts);
}

/*
 * The MPIC sits at 0xf8040000 and is little endian.  Firmware does not run
 * under qtest, so the sources are programmed by hand through the source and
 * per-CPU register blocks.
 */
#define MPIC_BASE          0xf8040000ULL
#define MPIC_SPVE          (MPIC_BASE + 0x10e0)
#define MPIC_IVPR(n)       (MPIC_BASE + 0x10000 + (n) * 0x20)
#define MPIC_IDR(n)        (MPIC_IVPR(n) + 0x10)
#define MPIC_CPU0_CTPR     (MPIC_BASE + 0x20080)
#define MPIC_CPU0_IACK     (MPIC_BASE + 0x200a0)
#define MPIC_CPU0_EOI      (MPIC_BASE + 0x200b0)
#define MPIC_IPI_IVPR(n)   (MPIC_BASE + 0x10a0 + (n) * 0x10)
#define MPIC_TMR_TVPR(n)   (MPIC_BASE + 0x1120 + (n) * 0x40)
#define MPIC_CPU0_IPIDR(n) (MPIC_BASE + 0x20040 + (n) * 0x10)
#define IVPR_MASK          (1u << 31)

/* KeyLargo: 64 external sources, then the IPIs and the timers in src[] */
#define KL_IPI0            64
#define KL_TMR0            68

/* Two external inputs nothing else drives under qtest */
#define SRC_LOW            0x30
#define SRC_HIGH           0x31

static void mpic_writel(QTestState *qts, uint64_t addr, uint32_t val)
{
    qtest_writel(qts, addr, bswap32(val));
}

static uint32_t mpic_readl(QTestState *qts, uint64_t addr)
{
    return bswap32(qtest_readl(qts, addr));
}

/* Unmasked, edge sensitive, delivered to CPU 0 */
static void mpic_edge_source(QTestState *qts, int n, int priority)
{
    mpic_writel(qts, MPIC_IDR(n), 1 << 0);
    mpic_writel(qts, MPIC_IVPR(n), (priority << 16) | n);
}

/*
 * An edge whose priority is not above CTPR has to stay pending until the
 * guest lowers CTPR.  OpenBSD raises CTPR while it handles an interrupt;
 * before upstream 2c87618bcc, the EOI of a higher priority interrupt
 * re-raised INT without looking at CTPR, and the IACK that followed
 * returned the spurious vector and dropped the masked edge on the way, so
 * it never arrived once CTPR came back down.  No CPU runs here, so watch
 * CPU 0's INT output (the first "sysbus-irq" line) and what IACK hands out.
 */
static void test_mpic_ctpr_keeps_edge(void)
{
    QTestState *qts;
    g_autofree char *pic = NULL;
    uint32_t spve;

    qts = qtest_init("-machine powermac7_3");
    pic = find_macio_pic(qts);
    g_assert_nonnull(pic);

    spve = mpic_readl(qts, MPIC_SPVE) & 0xff;
    mpic_edge_source(qts, SRC_LOW, 2);
    mpic_edge_source(qts, SRC_HIGH, 6);
    mpic_writel(qts, MPIC_CPU0_CTPR, 4);
    qtest_irq_intercept_out_named(qts, pic, "sysbus-irq");

    qtest_set_irq_in(qts, pic, NULL, SRC_LOW, 1);
    qtest_set_irq_in(qts, pic, NULL, SRC_LOW, 0);
    g_assert_false(qtest_get_irq(qts, OPENPIC_OUTPUT_INT));
    qtest_set_irq_in(qts, pic, NULL, SRC_HIGH, 1);
    qtest_set_irq_in(qts, pic, NULL, SRC_HIGH, 0);
    g_assert_true(qtest_get_irq(qts, OPENPIC_OUTPUT_INT));

    /* Only the source above CTPR is handed out */
    g_assert_cmphex(mpic_readl(qts, MPIC_CPU0_IACK) & 0xff, ==, SRC_HIGH);
    g_assert_false(qtest_get_irq(qts, OPENPIC_OUTPUT_INT));
    mpic_writel(qts, MPIC_CPU0_EOI, 0);

    /* The low one is still masked by CTPR: no INT, and IACK is spurious */
    g_assert_false(qtest_get_irq(qts, OPENPIC_OUTPUT_INT));
    g_assert_cmphex(mpic_readl(qts, MPIC_CPU0_IACK) & 0xff, ==, spve);

    /* ... and arrives once CTPR is lowered */
    mpic_writel(qts, MPIC_CPU0_CTPR, 1);
    g_assert_true(qtest_get_irq(qts, OPENPIC_OUTPUT_INT));
    g_assert_cmphex(mpic_readl(qts, MPIC_CPU0_IACK) & 0xff, ==, SRC_LOW);
    mpic_writel(qts, MPIC_CPU0_EOI, 0);
    g_assert_cmphex(mpic_readl(qts, MPIC_CPU0_IACK) & 0xff, ==, spve);

    qtest_quit(qts);
}

/*
 * The KeyLargo source window covers the 64 external sources only.  QEMU
 * keeps the IPIs and timers right after them in src[], and NetBSD masks
 * every source up to its IPI vector through the window; that must not
 * reach the IPI (or timer) registers, and the window reads 0 past the
 * last external source.
 */
static void test_mpic_src_window_ends(void)
{
    static const int beyond[] = { KL_IPI0, KL_TMR0, 72, 255 };
    QTestState *qts;
    g_autofree char *pic = NULL;
    uint32_t spve, ipi, tmr;
    int i;

    qts = qtest_init("-machine powermac7_3");
    pic = find_macio_pic(qts);
    g_assert_nonnull(pic);
    spve = mpic_readl(qts, MPIC_SPVE) & 0xff;

    /* The last external sources still decode */
    mpic_writel(qts, MPIC_IVPR(0), IVPR_MASK | (3 << 16) | 0x11);
    mpic_writel(qts, MPIC_IDR(0), 1 << 0);
    mpic_writel(qts, MPIC_IVPR(63), IVPR_MASK | (4 << 16) | 0x22);
    mpic_writel(qts, MPIC_IDR(63), 1 << 0);
    g_assert_cmphex(mpic_readl(qts, MPIC_IVPR(0)), ==,
                    IVPR_MASK | (3 << 16) | 0x11);
    g_assert_cmphex(mpic_readl(qts, MPIC_IDR(0)), ==, 1 << 0);
    g_assert_cmphex(mpic_readl(qts, MPIC_IVPR(63)), ==,
                    IVPR_MASK | (4 << 16) | 0x22);
    g_assert_cmphex(mpic_readl(qts, MPIC_IDR(63)), ==, 1 << 0);

    /* IPI 0 and timer 0, unmasked, through their own registers */
    mpic_writel(qts, MPIC_IPI_IVPR(0), (5 << 16) | 0x50);
    mpic_writel(qts, MPIC_TMR_TVPR(0), (6 << 16) | 0x60);
    ipi = mpic_readl(qts, MPIC_IPI_IVPR(0));
    tmr = mpic_readl(qts, MPIC_TMR_TVPR(0));
    g_assert_cmphex(ipi, ==, (5 << 16) | 0x50);
    g_assert_cmphex(tmr, ==, (6 << 16) | 0x60);

    /* Masking "every source" past the last one leaves them alone */
    for (i = 0; i < ARRAY_SIZE(beyond); i++) {
        mpic_writel(qts, MPIC_IVPR(beyond[i]), IVPR_MASK | (1 << 16) | 0x7f);
        mpic_writel(qts, MPIC_IDR(beyond[i]), 0);
    }
    g_assert_cmphex(mpic_readl(qts, MPIC_IPI_IVPR(0)), ==, ipi);
    g_assert_cmphex(mpic_readl(qts, MPIC_TMR_TVPR(0)), ==, tmr);

    /* ... and the window reads 0 there */
    for (i = 0; i < ARRAY_SIZE(beyond); i++) {
        g_assert_cmphex(mpic_readl(qts, MPIC_IVPR(beyond[i])), ==, 0);
        g_assert_cmphex(mpic_readl(qts, MPIC_IDR(beyond[i])), ==, 0);
    }

    /* IPI 0 still arrives at CPU 0 (CTPR comes out of reset at 15) */
    mpic_writel(qts, MPIC_CPU0_CTPR, 0);
    qtest_irq_intercept_out_named(qts, pic, "sysbus-irq");
    g_assert_false(qtest_get_irq(qts, OPENPIC_OUTPUT_INT));
    mpic_writel(qts, MPIC_CPU0_IPIDR(0), 1 << 0);
    g_assert_true(qtest_get_irq(qts, OPENPIC_OUTPUT_INT));
    g_assert_cmphex(mpic_readl(qts, MPIC_CPU0_IACK) & 0xff, ==, 0x50);
    mpic_writel(qts, MPIC_CPU0_EOI, 0);
    g_assert_false(qtest_get_irq(qts, OPENPIC_OUTPUT_INT));
    g_assert_cmphex(mpic_readl(qts, MPIC_CPU0_IACK) & 0xff, ==, spve);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine("powermac7_3")) {
        g_test_skip("powermac7_3 machine not available");
        return 0;
    }

    qtest_add_func("/powermac7_3-smp/cpu-max", test_cpu_max);
    qtest_add_func("/powermac7_3-smp/smp2-starts", test_smp2_starts);
    qtest_add_func("/powermac7_3-smp/smp1-unchanged", test_smp1_unchanged);
    qtest_add_func("/powermac7_3-smp/irq-destinations", test_irq_destinations);
    qtest_add_func("/powermac7_3-smp/secondary-held", test_secondary_held);
    qtest_add_func("/powermac7_3-smp/mpic-ctpr-keeps-edge",
                   test_mpic_ctpr_keeps_edge);
    qtest_add_func("/powermac7_3-smp/mpic-src-window-ends",
                   test_mpic_src_window_ends);

    return g_test_run();
}
