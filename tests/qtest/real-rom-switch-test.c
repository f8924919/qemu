/*
 * The x-real-rom switch of powermac7_3
 *
 * Running a real PowerMac G5 boot ROM with -bios takes fifteen x-
 * switches on powermac7_3.  x-real-rom turns them all to what the ROM
 * wants, and a switch given on its own keeps the value it was given,
 * whichever came first.  The properties are read back with qom-get
 * after the machine is built, and so are the values the machine hands
 * to the devices it makes.
 *
 * The keys of one -machine option do not reach their setters in the
 * order they were written but in the order of their hash, so the order
 * on the command line says nothing.  --preconfig and qom-set are what
 * set them one way round and then the other.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qbool.h"

#define SWITCH          "x-real-rom"
/* x-u3-i2c-spd wants half of the RAM to be a power of 2 */
#define RAM             "-m 2048"

typedef struct RomProp {
    const char *name;
    /* NULL for a bool, which is off by default and on for the ROM */
    const char *def;
    const char *rom;
} RomProp;

static const RomProp rom_props[] = {
    { "x-macio-80000000" },
    { "x-ht-mmio-80000000" },
    { "x-u3-i2c-lane3" },
    { "x-u3-i2c-spd" },
    { "x-u3-i2c-dumb" },
    { "x-dbdma-devstat" },
    { "x-nvram-flash-id" },
    { "x-u3-memctl" },
    { "x-ht-k2-rom-irq" },
    { "x-no-gpio-cpu-reset" },
    { "x-u3-reg60-nonzero" },
    { "x-rom-smp" },
    { "x-gpio9-idle-high" },
    { "x-dart", "auto", "on" },
    { "x-ht-k2-layout", "dev3", "dev1-5" },
};

static QDict *qom_get(QTestState *qts, const char *path, const char *prop)
{
    QDict *resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments':"
                                 " { 'path': %s, 'property': %s } }",
                            path, prop);

    g_assert(qdict_haskey(resp, "return"));
    return resp;
}

static bool get_bool(QTestState *qts, const char *path, const char *prop)
{
    QDict *resp = qom_get(qts, path, prop);
    bool val = qdict_get_bool(resp, "return");

    qobject_unref(resp);
    return val;
}

static char *get_str(QTestState *qts, const char *path, const char *prop)
{
    QDict *resp = qom_get(qts, path, prop);
    char *val = g_strdup(qdict_get_str(resp, "return"));

    qobject_unref(resp);
    return val;
}

static void set_prop(QTestState *qts, const char *prop, const char *val)
{
    QDict *resp;

    if (g_str_equal(val, "on") || g_str_equal(val, "off")) {
        resp = qtest_qmp(qts, "{ 'execute': 'qom-set', 'arguments':"
                              " { 'path': '/machine', 'property': %s,"
                              " 'value': %i } }",
                         prop, g_str_equal(val, "on"));
    } else {
        resp = qtest_qmp(qts, "{ 'execute': 'qom-set', 'arguments':"
                              " { 'path': '/machine', 'property': %s,"
                              " 'value': %s } }", prop, val);
    }
    g_assert(!qdict_haskey(resp, "error"));
    qobject_unref(resp);
}

/*
 * What the machine property @p should read: the ROM's value with the
 * switch on, the default without, and @over's value if @p is named there
 * ("name=value,..." in the -machine syntax).
 */
static const char *want(const RomProp *p, bool rom, const char *over)
{
    g_auto(GStrv) pairs = g_strsplit(over ? over : "", ",", -1);
    static char buf[32];

    for (int i = 0; pairs[i]; i++) {
        g_auto(GStrv) kv = g_strsplit(pairs[i], "=", 2);

        if (kv[0] && kv[1] && g_str_equal(kv[0], p->name)) {
            g_strlcpy(buf, kv[1], sizeof(buf));
            return buf;
        }
    }
    if (!p->def) {
        return rom ? "on" : "off";
    }
    return rom ? p->rom : p->def;
}

static void check_machine(QTestState *qts, bool rom, const char *over)
{
    for (int i = 0; i < ARRAY_SIZE(rom_props); i++) {
        const RomProp *p = &rom_props[i];
        const char *exp = want(p, rom, over);

        if (!p->def) {
            g_assert_cmpint(get_bool(qts, "/machine", p->name), ==,
                            g_str_equal(exp, "on"));
        } else {
            g_autofree char *val = get_str(qts, "/machine", p->name);

            g_assert_cmpstr(val, ==, exp);
        }
    }
}

static void run_machine(const char *opts, bool rom, const char *over)
{
    QTestState *qts = qtest_initf("-machine powermac7_3%s " RAM, opts);

    check_machine(qts, rom, over);
    qtest_quit(qts);
}

static void test_default(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3 " RAM);

    g_assert_false(get_bool(qts, "/machine", SWITCH));
    check_machine(qts, false, NULL);
    qtest_quit(qts);
}

static void test_off(void)
{
    run_machine("," SWITCH "=off", false, NULL);
}

static void test_on(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3," SWITCH "=on " RAM);

    g_assert_true(get_bool(qts, "/machine", SWITCH));
    check_machine(qts, true, NULL);
    qtest_quit(qts);
}

/*
 * One of each kind, and keys that reach their setter before x-real-rom
 * (x-rom-smp, x-dart, x-ht-k2-layout) as well as after it
 * (x-u3-reg60-nonzero, x-ht-mmio-80000000).  x-rom-smp=off is the
 * default: given, it still counts.
 */
#define OVER    "x-rom-smp=off,x-u3-reg60-nonzero=off,x-ht-mmio-80000000=off," \
                "x-dart=auto,x-ht-k2-layout=dev1"

static void test_override_after(void)
{
    run_machine("," SWITCH "=on," OVER, true, OVER);
}

static void test_override_before(void)
{
    run_machine("," OVER "," SWITCH "=on", true, OVER);
}

/* An individual switch on its own still works without x-real-rom */
static void test_single(void)
{
    run_machine(",x-u3-memctl=on", false, "x-u3-memctl=on");
}

/* x-gpio9-idle-high=off is how a real ROM run stops at the prompt */
static void test_gpio9_off(void)
{
    run_machine("," SWITCH "=on,x-gpio9-idle-high=off", true,
                "x-gpio9-idle-high=off");
}

static void test_gpio9_single(void)
{
    run_machine(",x-gpio9-idle-high=on", false, "x-gpio9-idle-high=on");
}

static void test_gpio9_switch_off(void)
{
    run_machine("," SWITCH "=off,x-gpio9-idle-high=on", false,
                "x-gpio9-idle-high=on");
}

/*
 * Where GPIO 9 rests after reset.  x-macio-80000000 is on with the
 * switch, so the MacIO is at 0x80000000 without placing a BAR; the
 * level is bit 1 of the GPIO 9 byte.
 */
#define MACIO_LOW       0x80000000ULL
#define GPIO9           0x61
#define GPIO_IN_DATA    0x02
#define GPIO_OUT_ENABLE 0x04

static void run_gpio9(const char *opts, bool high)
{
    QTestState *qts = qtest_initf("-machine powermac7_3%s " RAM, opts);

    g_assert_cmpint(!!(qtest_readb(qts, MACIO_LOW + GPIO9) & GPIO_IN_DATA),
                    ==, high);
    /* A low level reads as 0 from nothing too: check the byte is there */
    qtest_writeb(qts, MACIO_LOW + GPIO9, GPIO_OUT_ENABLE);
    g_assert_cmphex(qtest_readb(qts, MACIO_LOW + GPIO9) & GPIO_OUT_ENABLE, ==,
                    GPIO_OUT_ENABLE);
    qtest_quit(qts);
}

static void test_gpio9_rest_on(void)
{
    run_gpio9("," SWITCH "=on", true);
}

static void test_gpio9_rest_off(void)
{
    run_gpio9("," SWITCH "=on,x-gpio9-idle-high=off", false);
}

/* Find the n-th device of @type under /machine/unattached */
static char *find_type(QTestState *qts, const char *type, int n)
{
    for (int i = 0; i < 128; i++) {
        g_autofree char *path = g_strdup_printf(
            "/machine/unattached/device[%d]", i);
        QDict *resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments':"
                                     " { 'path': %s, 'property': 'type' } }",
                                path);
        bool hit = qdict_haskey(resp, "return") &&
                   g_str_has_suffix(qdict_get_str(resp, "return"), type);

        qobject_unref(resp);
        if (hit && n-- == 0) {
            return g_steal_pointer(&path);
        }
    }
    return NULL;
}

typedef struct ChildWant {
    bool lane3, dumb, devstat, flash_id, reg60, cpu_number;
    bool cpu1_off;
} ChildWant;

static void check_children(QTestState *qts, const ChildWant *w)
{
    g_autofree char *i2c = find_type(qts, "keywest-i2c", 0);
    g_autofree char *macio = find_type(qts, "macio-k2", 0);
    g_autofree char *nvram = find_type(qts, "macio-nvram", 0);
    g_autofree char *unin = find_type(qts, "uni-north", 0);
    g_autofree char *cpu1 = find_type(qts, "-powerpc64-cpu", 1);
    g_autofree char *dbdma = NULL;

    g_assert(i2c && macio && nvram && unin && cpu1);
    dbdma = g_strdup_printf("%s/dbdma", macio);
    g_assert_cmpint(get_bool(qts, i2c, "x-lane3"), ==, w->lane3);
    g_assert_cmpint(get_bool(qts, i2c, "x-dumb"), ==, w->dumb);
    g_assert_cmpint(get_bool(qts, dbdma, "x-devstat"), ==, w->devstat);
    g_assert_cmpint(get_bool(qts, nvram, "x-flash-id"), ==, w->flash_id);
    g_assert_cmpint(get_bool(qts, unin, "x-reg60-nonzero"), ==, w->reg60);
    g_assert_cmpint(get_bool(qts, unin, "x-cpu-number"), ==, w->cpu_number);
    g_assert_cmpint(get_bool(qts, cpu1, "start-powered-off"), ==,
                    w->cpu1_off);
}

static void run_children(const char *opts, const ChildWant *w)
{
    QTestState *qts = qtest_initf("-machine powermac7_3%s -smp 2 " RAM,
                                  opts);

    check_children(qts, w);
    qtest_quit(qts);
}

static void test_children_default(void)
{
    static const ChildWant w = { .cpu1_off = true };

    run_children("", &w);
}

static void test_children_on(void)
{
    static const ChildWant w = {
        .lane3 = true, .dumb = true, .devstat = true, .flash_id = true,
        .reg60 = true, .cpu_number = true, .cpu1_off = false,
    };

    run_children("," SWITCH "=on", &w);
}

static void test_children_override(void)
{
    static const ChildWant w = {
        .lane3 = false, .dumb = true, .devstat = false, .flash_id = true,
        .reg60 = false, .cpu_number = false, .cpu1_off = true,
    };

    run_children("," SWITCH "=on,x-u3-i2c-lane3=off,x-dbdma-devstat=off,"
                 "x-u3-reg60-nonzero=off,x-rom-smp=off", &w);
}

/*
 * Set the switch and the individual ones through qom-set before the
 * machine is built, the switch first or last.
 */
#define PRE_OVER    "x-rom-smp=off,x-u3-reg60-nonzero=off," \
                    "x-ht-k2-layout=dev3,x-gpio9-idle-high=off"

static void run_preconfig(bool switch_first)
{
    /*
     * -nodefaults as in numa-test: with the default devices, powermac7_3
     * aborts on leaving --preconfig whether or not the switch is given
     * (a duplicate console[0]).
     */
    QTestState *qts = qtest_init("-machine powermac7_3 -nodefaults "
                                 "--preconfig " RAM);
    g_auto(GStrv) pairs = g_strsplit(PRE_OVER, ",", -1);
    QDict *resp;

    if (switch_first) {
        set_prop(qts, SWITCH, "on");
    }
    for (int i = 0; pairs[i]; i++) {
        g_auto(GStrv) kv = g_strsplit(pairs[i], "=", 2);

        set_prop(qts, kv[0], kv[1]);
    }
    if (!switch_first) {
        set_prop(qts, SWITCH, "on");
    }
    resp = qtest_qmp(qts, "{ 'execute': 'x-exit-preconfig' }");
    g_assert(!qdict_haskey(resp, "error"));
    qobject_unref(resp);

    check_machine(qts, true, PRE_OVER);
    qtest_quit(qts);
}

static void test_preconfig_switch_first(void)
{
    run_preconfig(true);
}

static void test_preconfig_switch_last(void)
{
    run_preconfig(false);
}

/*
 * Only powermac7_3 takes the switch.  A refused option is a refused
 * startup, so run QEMU and read what it said.
 */
static void test_other_machine(const void *opaque)
{
    const char *machine = opaque;
    g_autofree char *cmdline = NULL;
    g_autofree char *err = NULL;
    g_auto(GStrv) argv = NULL;
    int status = 0;

    cmdline = g_strdup_printf("%s -M %s," SWITCH "=on -accel qtest "
                              "-display none", qtest_qemu_binary(NULL),
                              machine);
    g_assert_true(g_shell_parse_argv(cmdline, NULL, &argv, NULL));
    g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH |
                               G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL,
                               NULL, &err, &status, NULL));
    g_assert_cmpint(status, !=, 0);
    g_assert_nonnull(strstr(err, SWITCH));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/real-rom-switch/default", test_default);
    qtest_add_func("/real-rom-switch/off", test_off);
    qtest_add_func("/real-rom-switch/on", test_on);
    qtest_add_func("/real-rom-switch/override-after", test_override_after);
    qtest_add_func("/real-rom-switch/override-before", test_override_before);
    qtest_add_func("/real-rom-switch/single", test_single);
    qtest_add_func("/real-rom-switch/gpio9-off", test_gpio9_off);
    qtest_add_func("/real-rom-switch/gpio9-single", test_gpio9_single);
    qtest_add_func("/real-rom-switch/gpio9-switch-off",
                   test_gpio9_switch_off);
    qtest_add_func("/real-rom-switch/gpio9-rest-on", test_gpio9_rest_on);
    qtest_add_func("/real-rom-switch/gpio9-rest-off", test_gpio9_rest_off);
    qtest_add_func("/real-rom-switch/children-default",
                   test_children_default);
    qtest_add_func("/real-rom-switch/children-on", test_children_on);
    qtest_add_func("/real-rom-switch/children-override",
                   test_children_override);
    qtest_add_func("/real-rom-switch/preconfig-switch-first",
                   test_preconfig_switch_first);
    qtest_add_func("/real-rom-switch/preconfig-switch-last",
                   test_preconfig_switch_last);
    qtest_add_data_func("/real-rom-switch/other/mac99", "mac99",
                        test_other_machine);
    qtest_add_data_func("/real-rom-switch/other/powermac3_1", "powermac3_1",
                        test_other_machine);
    qtest_add_data_func("/real-rom-switch/other/powerbook3_2",
                        "powerbook3_2", test_other_machine);

    return g_test_run();
}
