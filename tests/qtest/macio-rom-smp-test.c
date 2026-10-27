/*
 * Starting the second CPU the way a real boot ROM does (x-rom-smp)
 *
 * A real PowerMac G5 boot ROM names mac-io byte 0x71 and 0x72 ~sreset0 and
 * ~sreset1, and pulses 0x72 to start the second processor.  With the
 * x-rom-smp switch on, powermac7_3 wires 0x72 to a soft reset of CPU 1
 * that comes out at the firmware's vector (PROM_BASE + 0x100), and the U3
 * register at +0x50 reads the number of the CPU reading it.  0x71 stays a
 * plain register.
 *
 * Nothing runs under qtest, so a reset is read off the NIP and SRR0 of the
 * CPU in "info registers -a".  With the switch on CPU 1 runs the ROM from
 * power-on, like CPU 0, so it starts at the firmware's vector with SRR0
 * clear, and a release shows as that vector landing in SRR0.  Without it
 * CPU 1 starts powered off at 0x100.  The
 * per-CPU U3 values need a CPU to do the read and cannot be checked here;
 * a qtest access has none and reads as CPU 0.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "hw/pci/pci_regs.h"

#define PROP            "x-rom-smp"
#define MACHINE_ON      "-machine powermac7_3,x-rom-smp=on -smp 2"
#define MACHINE_OFF     "-machine powermac7_3 -smp 2"
/* What a real boot ROM runs with: the KeyLargo reset GPIOs cut as well */
#define MACHINE_ROM     "-machine powermac7_3,x-rom-smp=on," \
                        "x-no-gpio-cpu-reset=on -smp 2"

/* powermac7_3: K2 bridge at bus 0 / dev 3, macio behind it at dev 7 */
#define U3_HT_CFG_BASE  0xf2000000ULL
#define K2_DEVFN        (3 << 3)
#define K2_MACIO_DEVFN  (7 << 3)
#define CFA0(devfn, off)        (((devfn) << 8) | (off))
#define CFA1(bus, devfn, off)   (CFA0(devfn, off) + ((bus) << 16) + 0x01000000)
#define K2_MACIO_BAR    0xfa000000ULL

/* mac-io bytes: ~sreset0 and ~sreset1, and KeyLargo's GPIO 3 and 4 */
#define SRESET0         0x71
#define SRESET1         0x72
#define GPIO3           0x5b
#define GPIO4           0x5c
#define GPIO_OUT_DATA   0x01
#define GPIO_OUT_ENABLE 0x04

#define UNIN_BASE       0xf8000000ULL

#define NIP_FIRMWARE    0xfff00100ULL
#define NIP_RESET       0x100ULL

static void le_writel(QTestState *qts, uint64_t addr, uint32_t val)
{
    qtest_writel(qts, addr, bswap32(val));
}

static void le_writew(QTestState *qts, uint64_t addr, uint16_t val)
{
    qtest_writew(qts, addr, bswap16(val));
}

/* Firmware does not run under qtest: place the macio BAR by hand */
static QTestState *k2_init(const char *machine)
{
    QTestState *qts = qtest_init(machine);

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
    return qts;
}

/*
 * The "CPU#n" heading "info registers -a" puts on a line of its own before
 * each CPU's dump.  The NIP line ends in "CPU#n" too, so only take the one
 * that starts a line and ends it.
 */
static const char *cpu_head(const char *regs, int n)
{
    g_autofree char *head = g_strdup_printf("CPU#%d", n);
    const char *p = regs;
    size_t len = strlen(head);

    while ((p = strstr(p, head)) != NULL) {
        bool line_start = p == regs || p[-1] == '\n';
        bool line_end = p[len] == '\r' || p[len] == '\n';

        if (line_start && line_end) {
            return p + len;
        }
        p += len;
    }
    return NULL;
}

/* A register of CPU n, between its heading and the next CPU's */
static uint64_t cpu_reg(QTestState *qts, int n, const char *name)
{
    g_autofree char *regs = qtest_hmp(qts, "info registers -a");
    g_autofree char *key = g_strdup_printf("%s ", name);
    const char *start = cpu_head(regs, n);
    const char *end, *reg;

    g_assert(start);
    end = cpu_head(start, n + 1);
    reg = strstr(start, key);
    g_assert(reg && (!end || reg < end));
    return g_ascii_strtoull(reg + strlen(key), NULL, 16);
}

static bool has_cpu(QTestState *qts, int n)
{
    g_autofree char *regs = qtest_hmp(qts, "info registers -a");

    return cpu_head(regs, n) != NULL;
}

/*
 * The reset is handed to the CPU's own thread, so give it time to land.
 * When it is not expected the full wait is spent before looking; that is
 * a timeout, not a proof, and the wiring tests are what pin the lines.
 */
static uint64_t reg_after_wait(QTestState *qts, int n, const char *name,
                               bool expect_change, uint64_t want)
{
    uint64_t v = cpu_reg(qts, n, name);
    int i;

    for (i = 0; i < (expect_change ? 500 : 100) &&
                (!expect_change || v != want); i++) {
        g_usleep(10 * 1000);
        v = cpu_reg(qts, n, name);
    }
    return v;
}

/* Hold the line low, then let it go, the way the boot ROM writes it */
static void hold(QTestState *qts, uint8_t byte)
{
    qtest_writeb(qts, K2_MACIO_BAR + byte, GPIO_OUT_ENABLE);
}

static void release(QTestState *qts, uint8_t byte)
{
    qtest_writeb(qts, K2_MACIO_BAR + byte, 0);
}

/* MacIO has no id; find it under /machine/unattached by its type */
static char *find_macio(QTestState *qts)
{
    int i;

    for (i = 0; i < 64; i++) {
        g_autofree char *path = g_strdup_printf(
            "/machine/unattached/device[%d]", i);
        QDict *resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments':"
                                     " { 'path': %s, 'property': 'type' } }",
                                path);
        bool hit = qdict_haskey(resp, "return") &&
                   g_str_equal(qdict_get_str(resp, "return"), "macio-k2");

        qobject_unref(resp);
        if (hit) {
            return g_steal_pointer(&path);
        }
    }
    return NULL;
}

/* Whether the board connected sreset[n] to anything */
static bool sreset_connected(QTestState *qts, int n)
{
    g_autofree char *macio = find_macio(qts);
    g_autofree char *prop = g_strdup_printf("sreset[%d]", n);
    QDict *resp;
    bool connected;

    g_assert(macio);
    resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments':"
                          " { 'path': %s, 'property': %s } }", macio, prop);
    g_assert(qdict_haskey(resp, "return"));
    connected = qdict_get_str(resp, "return")[0] != '\0';
    qobject_unref(resp);
    return connected;
}

/* CPU 1 comes up in ROM next to CPU 0 */
static void test_start_on(void)
{
    QTestState *qts = k2_init(MACHINE_ON);

    g_assert_cmphex(cpu_reg(qts, 0, "NIP"), ==, NIP_FIRMWARE);
    g_assert_cmphex(cpu_reg(qts, 1, "NIP"), ==, NIP_FIRMWARE);
    g_assert_cmphex(cpu_reg(qts, 1, "SRR0"), ==, 0);
    qtest_quit(qts);
}

/* Without the switch CPU 1 waits powered off at 0x100 */
static void test_start_off(void)
{
    QTestState *qts = k2_init(MACHINE_OFF);

    g_assert_cmphex(cpu_reg(qts, 1, "NIP"), ==, NIP_RESET);
    qtest_quit(qts);
}

/* Holding the line does nothing; letting it go resets CPU 1 into ROM */
static void test_release_on(void)
{
    QTestState *qts = k2_init(MACHINE_ON);

    g_assert_cmphex(cpu_reg(qts, 1, "NIP"), ==, NIP_FIRMWARE);
    hold(qts, SRESET1);
    g_assert_cmphex(reg_after_wait(qts, 1, "SRR0", false, 0), ==, 0);
    release(qts, SRESET1);
    /* A system reset interrupt: SRR0 keeps where CPU 1 was */
    g_assert_cmphex(reg_after_wait(qts, 1, "SRR0", true, NIP_FIRMWARE), ==,
                    NIP_FIRMWARE);
    g_assert_cmphex(cpu_reg(qts, 1, "NIP"), ==, NIP_FIRMWARE);
    g_assert_cmphex(cpu_reg(qts, 0, "NIP"), ==, NIP_FIRMWARE);
    g_assert_cmphex(cpu_reg(qts, 0, "SRR0"), ==, 0);
    qtest_quit(qts);
}

/*
 * Every release resets CPU 1 again.  GPIO 4 moves it to 0x100 in between
 * so that the second release leaves a different SRR0 from the first.
 */
static void test_again_on(void)
{
    QTestState *qts = k2_init(MACHINE_ON);

    hold(qts, SRESET1);
    release(qts, SRESET1);
    g_assert_cmphex(reg_after_wait(qts, 1, "SRR0", true, NIP_FIRMWARE), ==,
                    NIP_FIRMWARE);
    qtest_writeb(qts, K2_MACIO_BAR + GPIO4, GPIO_OUT_ENABLE);
    qtest_writeb(qts, K2_MACIO_BAR + GPIO4, GPIO_OUT_ENABLE | GPIO_OUT_DATA);
    g_assert_cmphex(reg_after_wait(qts, 1, "NIP", true, NIP_RESET), ==,
                    NIP_RESET);
    hold(qts, SRESET1);
    release(qts, SRESET1);
    g_assert_cmphex(reg_after_wait(qts, 1, "SRR0", true, NIP_RESET), ==,
                    NIP_RESET);
    g_assert_cmphex(cpu_reg(qts, 1, "NIP"), ==, NIP_FIRMWARE);
    qtest_quit(qts);
}

/* Without the switch 0x72 is a plain register */
static void test_release_off(void)
{
    QTestState *qts = k2_init(MACHINE_OFF);

    hold(qts, SRESET1);
    release(qts, SRESET1);
    g_assert_cmphex(reg_after_wait(qts, 1, "NIP", false, 0), ==, NIP_RESET);
    g_assert_cmphex(cpu_reg(qts, 1, "SRR0"), ==, 0);
    g_assert_cmphex(qtest_readb(qts, K2_MACIO_BAR + SRESET1), ==, 0);
    qtest_quit(qts);
}

/* ~sreset0 is left alone: neither CPU moves */
static void test_sreset0_on(void)
{
    QTestState *qts = k2_init(MACHINE_ON);

    hold(qts, SRESET0);
    release(qts, SRESET0);
    g_assert_cmphex(reg_after_wait(qts, 1, "SRR0", false, 0), ==, 0);
    g_assert_cmphex(cpu_reg(qts, 1, "NIP"), ==, NIP_FIRMWARE);
    g_assert_cmphex(cpu_reg(qts, 0, "NIP"), ==, NIP_FIRMWARE);
    g_assert_cmphex(cpu_reg(qts, 0, "SRR0"), ==, 0);
    qtest_quit(qts);
}

/* The KeyLargo reset GPIOs keep doing what they did before */
static void test_gpio3_on(void)
{
    QTestState *qts = k2_init(MACHINE_ON);

    qtest_writeb(qts, K2_MACIO_BAR + GPIO3, GPIO_OUT_ENABLE);
    qtest_writeb(qts, K2_MACIO_BAR + GPIO3, GPIO_OUT_ENABLE | GPIO_OUT_DATA);
    g_assert_cmphex(reg_after_wait(qts, 0, "NIP", true, NIP_RESET), ==,
                    NIP_RESET);
    qtest_quit(qts);
}

static void test_gpio3_rom(void)
{
    QTestState *qts = k2_init(MACHINE_ROM);

    qtest_writeb(qts, K2_MACIO_BAR + GPIO3, GPIO_OUT_ENABLE);
    qtest_writeb(qts, K2_MACIO_BAR + GPIO3, GPIO_OUT_ENABLE | GPIO_OUT_DATA);
    g_assert_cmphex(reg_after_wait(qts, 0, "NIP", false, 0), ==,
                    NIP_FIRMWARE);
    /* ...while 0x72 still resets CPU 1 */
    hold(qts, SRESET1);
    release(qts, SRESET1);
    g_assert_cmphex(reg_after_wait(qts, 1, "SRR0", true, NIP_FIRMWARE), ==,
                    NIP_FIRMWARE);
    qtest_quit(qts);
}

/* GPIO 4 resets CPU 1 to 0x100 (Linux's trampoline), not into ROM */
static void test_gpio4_on(void)
{
    QTestState *qts = k2_init(MACHINE_ON);

    hold(qts, SRESET1);
    release(qts, SRESET1);
    g_assert_cmphex(reg_after_wait(qts, 1, "SRR0", true, NIP_FIRMWARE), ==,
                    NIP_FIRMWARE);
    qtest_writeb(qts, K2_MACIO_BAR + GPIO4, GPIO_OUT_ENABLE);
    qtest_writeb(qts, K2_MACIO_BAR + GPIO4, GPIO_OUT_ENABLE | GPIO_OUT_DATA);
    g_assert_cmphex(reg_after_wait(qts, 1, "NIP", true, NIP_RESET), ==,
                    NIP_RESET);
    qtest_quit(qts);
}

/* A machine reset puts CPU 1 back in ROM with SRR0 clear */
static void test_system_reset_on(void)
{
    QTestState *qts = k2_init(MACHINE_ON);
    QDict *resp;

    hold(qts, SRESET1);
    release(qts, SRESET1);
    g_assert_cmphex(reg_after_wait(qts, 1, "SRR0", true, NIP_FIRMWARE), ==,
                    NIP_FIRMWARE);
    resp = qtest_qmp(qts, "{ 'execute': 'system_reset' }");
    g_assert(qdict_haskey(resp, "return"));
    qobject_unref(resp);
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_cmphex(reg_after_wait(qts, 1, "SRR0", true, 0), ==, 0);
    g_assert_cmphex(cpu_reg(qts, 1, "NIP"), ==, NIP_FIRMWARE);
    g_assert_cmphex(cpu_reg(qts, 0, "NIP"), ==, NIP_FIRMWARE);
    qtest_quit(qts);
}

/* One CPU: nothing to start, and nothing breaks */
static void test_smp1_on(void)
{
    QTestState *qts = k2_init("-machine powermac7_3,x-rom-smp=on -smp 1");

    g_assert_false(has_cpu(qts, 1));
    hold(qts, SRESET1);
    release(qts, SRESET1);
    g_assert_cmphex(reg_after_wait(qts, 0, "NIP", false, 0), ==,
                    NIP_FIRMWARE);
    g_assert_cmphex(cpu_reg(qts, 0, "SRR0"), ==, 0);
    g_assert_false(sreset_connected(qts, 0));
    g_assert_false(sreset_connected(qts, 1));
    qtest_quit(qts);
}

/* Only ~sreset1, and only for CPU 1 with the switch on */
static void test_wiring(void)
{
    QTestState *qts;

    qts = k2_init(MACHINE_ON);
    g_assert_false(sreset_connected(qts, 0));
    g_assert_true(sreset_connected(qts, 1));
    qtest_quit(qts);

    qts = k2_init(MACHINE_OFF);
    g_assert_false(sreset_connected(qts, 0));
    g_assert_false(sreset_connected(qts, 1));
    qtest_quit(qts);
}

/*
 * U3 +0x50 is the reading CPU's number with the switch on; a qtest access
 * has no CPU and reads as CPU 0, so +0x50 and +0x60 look as before.
 */
static void test_unin(void)
{
    QTestState *qts;

    qts = qtest_init(MACHINE_ON);
    g_assert_cmphex(qtest_readl(qts, UNIN_BASE + 0x50), ==, 0);
    g_assert_cmphex(qtest_readl(qts, UNIN_BASE + 0x60), ==, 0);
    qtest_quit(qts);

    qts = qtest_init("-machine powermac7_3,x-rom-smp=on,"
                     "x-u3-reg60-nonzero=on -smp 2");
    g_assert_cmphex(qtest_readl(qts, UNIN_BASE + 0x50), ==, 0);
    g_assert_cmphex(qtest_readl(qts, UNIN_BASE + 0x60), ==, 1);
    qtest_quit(qts);
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

static bool machine_prop(QTestState *qts)
{
    QDict *resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments':"
                                 " { 'path': '/machine', 'property': %s } }",
                            PROP);
    bool val = qdict_get_bool(resp, "return");

    qobject_unref(resp);
    return val;
}

static void test_prop(void)
{
    QTestState *qts;

    qts = qtest_init(MACHINE_OFF);
    g_assert_false(machine_prop(qts));
    qtest_quit(qts);

    qts = qtest_init(MACHINE_ON);
    g_assert_true(machine_prop(qts));
    qtest_quit(qts);

    qts = qtest_init("-machine mac99");
    g_assert_false(machine_has_prop(qts));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/macio-rom-smp/start/on", test_start_on);
    qtest_add_func("/macio-rom-smp/start/off", test_start_off);
    qtest_add_func("/macio-rom-smp/release/on", test_release_on);
    qtest_add_func("/macio-rom-smp/again/on", test_again_on);
    qtest_add_func("/macio-rom-smp/release/off", test_release_off);
    qtest_add_func("/macio-rom-smp/sreset0/on", test_sreset0_on);
    qtest_add_func("/macio-rom-smp/gpio3/on", test_gpio3_on);
    qtest_add_func("/macio-rom-smp/gpio3/rom", test_gpio3_rom);
    qtest_add_func("/macio-rom-smp/gpio4/on", test_gpio4_on);
    qtest_add_func("/macio-rom-smp/system-reset/on", test_system_reset_on);
    qtest_add_func("/macio-rom-smp/smp1/on", test_smp1_on);
    qtest_add_func("/macio-rom-smp/wiring", test_wiring);
    qtest_add_func("/macio-rom-smp/unin", test_unin);
    qtest_add_func("/macio-rom-smp/prop", test_prop);

    return g_test_run();
}
