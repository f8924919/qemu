/*
 * The CPU reset lines of the NewWorld MacIO GPIO block (x-no-gpio-cpu-reset)
 *
 * powermac7_3 wires GPIO 3 and 4 (byte 0x5b and 0x5c of mac-io) to the
 * reset of CPU 0 and 1, as Linux's KL_GPIO_RESET_CPUn and the soft-reset
 * property OpenBIOS writes expect.  A real PowerMac G5 boot ROM names
 * those GPIOs codec-input-data-mux and line-input-detect, and Mac OS X
 * drives them as such.  With the x-no-gpio-cpu-reset switch on, the board
 * leaves the lines unconnected and the GPIO block just keeps the value.
 *
 * Nothing runs under qtest, so a reset is read off the NIP: machine reset
 * leaves CPU 0 at the firmware's vector (PROM_BASE + 0x100) and
 * cpu_reset() puts it at 0x100.  CPU 1 starts powered off and already
 * sits at 0x100, so its line is checked through the QOM link instead.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "hw/pci/pci_regs.h"

#define MACHINE_ON      "-machine powermac7_3,x-no-gpio-cpu-reset=on -smp 2"
#define MACHINE_OFF     "-machine powermac7_3 -smp 2"
#define PROP            "x-no-gpio-cpu-reset"

/* powermac7_3: K2 bridge at bus 0 / dev 3, macio behind it at dev 7 */
#define U3_HT_CFG_BASE  0xf2000000ULL
#define K2_DEVFN        (3 << 3)
#define K2_MACIO_DEVFN  (7 << 3)
#define CFA0(devfn, off)        (((devfn) << 8) | (off))
#define CFA1(bus, devfn, off)   (CFA0(devfn, off) + ((bus) << 16) + 0x01000000)
#define K2_MACIO_BAR    0xfa000000ULL

/* The reset GPIOs in the macio BAR (GPIO window at 0x50, levels first) */
static const uint8_t reset_gpio[] = { 0x5b, 0x5c, 0x67, 0x68 };
#define GPIO_OUT_DATA   0x01
#define GPIO_IN_DATA    0x02
#define GPIO_OUT_ENABLE 0x04

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
 * NIP of CPU 0.  CPU 1 sits at 0x100 from the start, so reading its NIP
 * by mistake would look like a reset: take the one under the CPU#0
 * heading and make sure CPU#1 comes after it.
 */
static uint64_t cpu0_nip(QTestState *qts)
{
    g_autofree char *regs = qtest_hmp(qts, "info registers -a");
    const char *cpu0 = strstr(regs, "CPU#0");
    const char *cpu1 = strstr(regs, "CPU#1");
    const char *nip;

    g_assert(cpu0 && cpu1 && cpu0 < cpu1);
    nip = strstr(cpu0, "NIP ");
    g_assert(nip && nip < cpu1);
    return g_ascii_strtoull(nip + 4, NULL, 16);
}

/*
 * The reset is handed to the CPU's own thread, so give it time to land.
 * When it is not expected the full wait is spent before looking; that is
 * a timeout, not a proof, and the wiring tests are what pin the lines.
 */
static uint64_t cpu0_nip_after_wait(QTestState *qts, bool expect_reset)
{
    uint64_t nip = cpu0_nip(qts);
    int i;

    for (i = 0; i < (expect_reset ? 500 : 100) &&
                (!expect_reset || nip != NIP_RESET); i++) {
        g_usleep(10 * 1000);
        nip = cpu0_nip(qts);
    }
    return nip;
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

/* Whether the board connected cpu-reset[n] to anything */
static bool reset_line_connected(QTestState *qts, int n)
{
    g_autofree char *macio = find_macio(qts);
    g_autofree char *prop = g_strdup_printf("cpu-reset[%d]", n);
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

/* Assert, then release, the line the way Linux g5_reset_cpu() does */
static void pulse_cpu0_reset(QTestState *qts)
{
    qtest_writeb(qts, K2_MACIO_BAR + reset_gpio[0], GPIO_OUT_ENABLE);
    qtest_writeb(qts, K2_MACIO_BAR + reset_gpio[0],
                 GPIO_OUT_ENABLE | GPIO_OUT_DATA);
}

static void test_cpu0_reset_off(void)
{
    QTestState *qts = k2_init(MACHINE_OFF);

    g_assert_cmphex(cpu0_nip(qts), ==, NIP_FIRMWARE);
    pulse_cpu0_reset(qts);
    g_assert_cmphex(cpu0_nip_after_wait(qts, true), ==, NIP_RESET);
    qtest_quit(qts);
}

static void test_cpu0_reset_on(void)
{
    QTestState *qts = k2_init(MACHINE_ON);

    g_assert_cmphex(cpu0_nip(qts), ==, NIP_FIRMWARE);
    pulse_cpu0_reset(qts);
    g_assert_cmphex(cpu0_nip_after_wait(qts, false), ==, NIP_FIRMWARE);
    qtest_quit(qts);
}

/* Only two CPUs on powermac7_3, so only the first two lines are wired */
static void test_wiring_off(void)
{
    QTestState *qts = k2_init(MACHINE_OFF);

    g_assert_true(reset_line_connected(qts, 0));
    g_assert_true(reset_line_connected(qts, 1));
    g_assert_false(reset_line_connected(qts, 2));
    g_assert_false(reset_line_connected(qts, 3));
    qtest_quit(qts);
}

static void test_wiring_on(void)
{
    QTestState *qts = k2_init(MACHINE_ON);
    int n;

    for (n = 0; n < ARRAY_SIZE(reset_gpio); n++) {
        g_assert_false(reset_line_connected(qts, n));
    }
    qtest_quit(qts);
}

/* With the lines cut the GPIOs are plain registers */
static void test_readback_on(void)
{
    QTestState *qts = k2_init(MACHINE_ON);
    int n;

    for (n = 0; n < ARRAY_SIZE(reset_gpio); n++) {
        qtest_writeb(qts, K2_MACIO_BAR + reset_gpio[n], GPIO_OUT_ENABLE);
        g_assert_cmphex(qtest_readb(qts, K2_MACIO_BAR + reset_gpio[n]), ==,
                        GPIO_OUT_ENABLE);
        qtest_writeb(qts, K2_MACIO_BAR + reset_gpio[n],
                     GPIO_OUT_ENABLE | GPIO_OUT_DATA);
        g_assert_cmphex(qtest_readb(qts, K2_MACIO_BAR + reset_gpio[n]), ==,
                        GPIO_OUT_ENABLE | GPIO_IN_DATA | GPIO_OUT_DATA);
    }
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

    qtest_add_func("/macio-gpio-cpu-reset/cpu0/off", test_cpu0_reset_off);
    qtest_add_func("/macio-gpio-cpu-reset/cpu0/on", test_cpu0_reset_on);
    qtest_add_func("/macio-gpio-cpu-reset/wiring/off", test_wiring_off);
    qtest_add_func("/macio-gpio-cpu-reset/wiring/on", test_wiring_on);
    qtest_add_func("/macio-gpio-cpu-reset/readback/on", test_readback_on);
    qtest_add_func("/macio-gpio-cpu-reset/prop", test_prop);

    return g_test_run();
}
