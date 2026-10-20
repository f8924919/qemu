/*
 * Flash ID read-out of the NewWorld NVRAM (x-flash-id)
 *
 * A real PowerMac G5 boot ROM writes the Read Identifier command (0x90)
 * to the NVRAM flash and reads a 16-bit manufacturer/device word to pick
 * the chip type.  With x-flash-id on, the part answers as a Sharp part
 * (0xb0, 0x4b); with it off, 0x90 stays an unknown command and the
 * contents keep coming back.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "migration/migration-qmp.h"
#include "migration/migration-util.h"
#include "qobject/qdict.h"

#define NVRAM_BASE      0xfff04000ULL
#define NVRAM_SIZE      0x4000
#define NVRAM_BANK      0x2000

#define CMD_ERASE_SETUP     0x20
#define CMD_ERASE_CONFIRM   0xd0
#define CMD_WRITE_SETUP     0x40
#define CMD_WRITE_SETUP2    0x10
#define CMD_CLEAR_STATUS    0x50
#define CMD_READ_STATUS     0x70
#define CMD_READ_ID         0x90
#define CMD_RESET           0xff

#define ID_MANUFACTURER     0xb0
#define ID_DEVICE           0x4b
#define STATUS_DONE         0x80

#define MACHINE_ON  "-machine powermac7_3,x-nvram-flash-id=on"
#define MACHINE_OFF "-machine powermac7_3"

static void cmd(QTestState *qts, uint8_t value)
{
    qtest_writeb(qts, NVRAM_BASE, value);
}

/* A byte from the array, read with the part back in read-array mode */
static uint8_t array_byte(QTestState *qts, uint32_t off)
{
    cmd(qts, CMD_RESET);
    return qtest_readb(qts, NVRAM_BASE + off);
}

static void check_id(QTestState *qts)
{
    g_assert_cmphex(qtest_readb(qts, NVRAM_BASE), ==, ID_MANUFACTURER);
    g_assert_cmphex(qtest_readb(qts, NVRAM_BASE + 1), ==, ID_DEVICE);
    g_assert_cmphex(qtest_readb(qts, NVRAM_BASE + 2), ==, 0);
    g_assert_cmphex(qtest_readb(qts, NVRAM_BASE + NVRAM_SIZE - 1), ==, 0);
    g_assert_cmphex(qtest_readw(qts, NVRAM_BASE), ==,
                    ID_MANUFACTURER << 8 | ID_DEVICE);
}

/* Offsets whose contents are not what the ID read-out would return */
static void check_array(QTestState *qts, const uint8_t *want)
{
    g_assert_cmphex(qtest_readb(qts, NVRAM_BASE), ==, want[0]);
    g_assert_cmphex(qtest_readb(qts, NVRAM_BASE + 1), ==, want[1]);
    g_assert_cmphex(qtest_readb(qts, NVRAM_BASE + 2), ==, want[2]);
}

static void read_head(QTestState *qts, uint8_t *head)
{
    cmd(qts, CMD_RESET);
    qtest_memread(qts, NVRAM_BASE, head, 3);
}

static void test_id_offsets(void)
{
    QTestState *qts = qtest_init(MACHINE_ON);

    cmd(qts, CMD_READ_ID);
    check_id(qts);
    /* Out of Read Status too: the ID replaces the status */
    cmd(qts, CMD_READ_STATUS);
    cmd(qts, CMD_READ_ID);
    check_id(qts);
    /* A second 0x90 keeps the part reading out its ID */
    cmd(qts, CMD_READ_ID);
    check_id(qts);

    qtest_quit(qts);
}

static void test_id_leave(void)
{
    static const uint8_t leave[] = { CMD_RESET, CMD_CLEAR_STATUS, 0xf0, 0xaa,
                                     0x55, CMD_ERASE_CONFIRM };
    QTestState *qts = qtest_init(MACHINE_ON);
    uint8_t head[3];
    int i;

    read_head(qts, head);
    g_assert_false(head[0] == ID_MANUFACTURER && head[1] == ID_DEVICE);

    /* Read Status leaves, and the status comes back (before 0x50 clears it) */
    cmd(qts, CMD_READ_ID);
    cmd(qts, CMD_READ_STATUS);
    g_assert_cmphex(qtest_readb(qts, NVRAM_BASE + 1), ==, STATUS_DONE);
    g_assert_cmphex(qtest_readb(qts, NVRAM_BASE + 2), ==, STATUS_DONE);

    for (i = 0; i < ARRAY_SIZE(leave); i++) {
        cmd(qts, CMD_READ_ID);
        g_assert_cmphex(qtest_readb(qts, NVRAM_BASE), ==, ID_MANUFACTURER);
        cmd(qts, leave[i]);
        check_array(qts, head);
    }


    qtest_quit(qts);
}

/* Erase and program from read-ID mode, the way OpenBIOS updates a bank */
static void test_id_then_program(void)
{
    QTestState *qts = qtest_init(MACHINE_ON);

    cmd(qts, CMD_READ_ID);
    qtest_writeb(qts, NVRAM_BASE + NVRAM_BANK, CMD_ERASE_SETUP);
    qtest_writeb(qts, NVRAM_BASE + NVRAM_BANK, CMD_ERASE_CONFIRM);
    g_assert_cmphex(qtest_readb(qts, NVRAM_BASE), ==, STATUS_DONE);
    qtest_writeb(qts, NVRAM_BASE + NVRAM_BANK + 5, CMD_WRITE_SETUP2);
    qtest_writeb(qts, NVRAM_BASE + NVRAM_BANK + 5, 0x3c);
    g_assert_cmphex(array_byte(qts, NVRAM_BANK + 5), ==, 0x3c);
    g_assert_cmphex(array_byte(qts, NVRAM_BANK + 6), ==, 0xff);

    qtest_quit(qts);
}

/* A 0x90 that an erase or a program is waiting on is that data */
static void test_id_mid_sequence(void)
{
    QTestState *qts = qtest_init(MACHINE_ON);

    qtest_writeb(qts, NVRAM_BASE + NVRAM_BANK, CMD_WRITE_SETUP);
    qtest_writeb(qts, NVRAM_BASE + NVRAM_BANK + 1, CMD_READ_ID);
    g_assert_cmphex(qtest_readb(qts, NVRAM_BASE), ==, STATUS_DONE);
    g_assert_cmphex(qtest_readb(qts, NVRAM_BASE + 1), ==, STATUS_DONE);
    g_assert_cmphex(array_byte(qts, NVRAM_BANK + 1), ==, CMD_READ_ID);

    /* An erase setup followed by 0x90 is a failed erase, not an ID read */
    cmd(qts, CMD_CLEAR_STATUS);
    qtest_writeb(qts, NVRAM_BASE + NVRAM_BANK, CMD_ERASE_SETUP);
    qtest_writeb(qts, NVRAM_BASE + NVRAM_BANK, CMD_READ_ID);
    g_assert_cmphex(qtest_readb(qts, NVRAM_BASE), ==, STATUS_DONE | 0x20);
    g_assert_cmphex(qtest_readb(qts, NVRAM_BASE + 1), ==, STATUS_DONE | 0x20);
    g_assert_cmphex(array_byte(qts, NVRAM_BANK + 1), ==, CMD_READ_ID);

    qtest_quit(qts);
}

static void test_id_reset(void)
{
    QTestState *qts = qtest_init(MACHINE_ON);
    uint8_t head[3];

    read_head(qts, head);
    cmd(qts, CMD_READ_ID);
    qtest_system_reset(qts);
    check_array(qts, head);

    qtest_quit(qts);
}

static void test_id_off(void)
{
    QTestState *qts = qtest_init(MACHINE_OFF);
    uint8_t head[3];

    read_head(qts, head);
    g_assert_false(head[0] == ID_MANUFACTURER && head[1] == ID_DEVICE);
    cmd(qts, CMD_READ_ID);
    check_array(qts, head);

    qtest_quit(qts);
}

static void test_id_global(void)
{
    QTestState *qts = qtest_init(MACHINE_OFF
                                 " -global macio-nvram.x-flash-id=on");

    cmd(qts, CMD_READ_ID);
    check_id(qts);

    qtest_quit(qts);
}

static bool machine_has_prop(QTestState *qts, const char *prop)
{
    QDict *resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                            "{ 'path': '/machine', 'property': %s } }", prop);
    bool found = qdict_haskey(resp, "return");

    qobject_unref(resp);
    return found;
}

static void test_id_mac99(void)
{
    QTestState *qts = qtest_init(MACHINE_OFF);

    g_assert_true(machine_has_prop(qts, "x-nvram-flash-id"));
    qtest_quit(qts);

    qts = qtest_init("-machine mac99 -cpu 970fx");
    g_assert_false(machine_has_prop(qts, "x-nvram-flash-id"));
    qtest_quit(qts);
}

static void test_id_migrate(void)
{
    g_autofree char *uri = g_strdup_printf("unix:%s/macio-nvram-flash-id",
                                           g_get_tmp_dir());
    QTestState *src, *dst;

    unlink(uri + strlen("unix:"));
    src = qtest_init(MACHINE_ON);
    dst = qtest_init(MACHINE_ON " -incoming defer");

    cmd(src, CMD_READ_ID);
    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, NULL, uri, NULL, "{}");
    wait_for_migration_complete(src);
    wait_for_migration_complete(dst);

    check_id(dst);

    qtest_quit(dst);
    qtest_quit(src);
    unlink(uri + strlen("unix:"));
}

/* The state only travels, in its own subsection, with the switch on */
static bool stream_has_id_section(const char *machine)
{
    g_autofree char *path = g_strdup_printf("%s/macio-nvram-flash-stream",
                                            g_get_tmp_dir());
    g_autofree char *uri = g_strdup_printf("file:%s", path);
    g_autofree char *data = NULL;
    gsize len;
    QTestState *qts;
    bool found;

    unlink(path);
    qts = qtest_init(machine);
    migrate_qmp(qts, NULL, uri, NULL, "{}");
    wait_for_migration_complete(qts);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(path, &data, &len, NULL));
    /* The stream is binary: search all of it, past any NUL */
    found = memmem(data, len, "macio_nvram/read-id", 19) != NULL;
    g_assert_nonnull(memmem(data, len, "macio_nvram/flash", 17));
    unlink(path);
    return found;
}

static void test_id_stream(void)
{
    g_assert_true(stream_has_id_section(MACHINE_ON));
    g_assert_false(stream_has_id_section(MACHINE_OFF));
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine("powermac7_3")) {
        return g_test_run();
    }
    qtest_add_func("/macio-nvram/flash-id/offsets", test_id_offsets);
    qtest_add_func("/macio-nvram/flash-id/leave", test_id_leave);
    qtest_add_func("/macio-nvram/flash-id/program", test_id_then_program);
    qtest_add_func("/macio-nvram/flash-id/mid-sequence", test_id_mid_sequence);
    qtest_add_func("/macio-nvram/flash-id/reset", test_id_reset);
    qtest_add_func("/macio-nvram/flash-id/off", test_id_off);
    qtest_add_func("/macio-nvram/flash-id/global", test_id_global);
    qtest_add_func("/macio-nvram/flash-id/mac99", test_id_mac99);
    qtest_add_func("/macio-nvram/flash-id/migrate", test_id_migrate);
    qtest_add_func("/macio-nvram/flash-id/stream", test_id_stream);

    return g_test_run();
}
