/*
 * Test that a backing image for the macio NVRAM survives a machine reset.
 *
 * The board formats the NVRAM after the device has read its backing image,
 * which threw away whatever the image held.  Check that an image the
 * firmware or a guest has already initialised is left alone, and that an
 * empty one is initialised and written back.
 *
 * The check reads the image file rather than the device, so it works on
 * the OldWorld machine too, where the NVRAM sits behind a PCI BAR that
 * nothing has assigned an address to when no firmware runs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "migration/migration-qmp.h"
#include "migration/migration-util.h"
#include "qobject/qdict.h"

/*
 * Both layouts carry a big endian generation number that the formatting
 * code writes, so a machine that reformats can be told from one that does
 * not by the value that survives.
 */
#define FORMATTED_GENERATION 2
#define POKED_GENERATION     7
#define OTHER_GENERATION     5

/*
 * One entry of the CHRP partition chain, as the formatting code leaves it.
 * The name is the string the field is expected to hold; the field itself is
 * twelve bytes, zero padded, and a name of exactly twelve characters fills
 * it with no terminator at all.  That case is the point of the check: Mac OS
 * X compares the free space name with strncmp(..., 12), so a NUL in the last
 * byte makes the match fail and the guest never finds the free space.
 */
typedef struct {
    uint8_t sig;
    const char *name;
} NvramPart;

#define CHRP_PART_NAME_LEN 12

/*
 * An empty NewWorld machine formats its NVRAM into two identical banks:
 * the 32 byte core99 header, the Open Firmware variables, and the free
 * space that Mac OS X looks for by its twelve character name.
 */
static const NvramPart newworld_parts[] = {
    { 0x5a, "nvram" }, { 0x70, "common" }, { 0x7f, "wwwwwwwwwwww" },
    { 0x5a, "nvram" }, { 0x70, "common" }, { 0x7f, "wwwwwwwwwwww" },
};

/*
 * OldWorld is left alone: it has no guest here to tell whether Mac OS X
 * would go on to carve a panic partition out of the free space, so the
 * names it writes are pinned as they are rather than changed blind.
 */
static const NvramPart oldworld_parts[] = {
    { 0x70, "common" },
    { 0x7f, "free" },
    { 0x5a, "wwwwwwwwwww" },        /* Eleven, written with pstrcpy() */
};

typedef struct {
    const char *machine;
    unsigned size;
    unsigned bank;      /* Erase block, or the whole part where there is none */
    unsigned gen_off;
    /*
     * Where the part answers, or zero where it cannot be read without
     * firmware: the OldWorld NVRAM sits behind a PCI BAR that nothing has
     * given an address to.
     */
    uint64_t addr;
    const NvramPart *parts;
    unsigned nparts;
} NvramLayout;

static const NvramLayout layouts[] = {
    /* NewWorld: two core99 banks, generation inside the first bank header */
    { "mac99",        0x4000, 0x2000, 0x14,   0xfff04000,
      newworld_parts, ARRAY_SIZE(newworld_parts) },
    { "powermac7_3",  0x4000, 0x2000, 0x14,   0xfff04000,
      newworld_parts, ARRAY_SIZE(newworld_parts) },
    /* OldWorld: an Open Firmware half and a Mac OS X half, one erase block */
    { "g3beige",      0x2000, 0x2000, 0x1014, 0,
      oldworld_parts, ARRAY_SIZE(oldworld_parts) },
};

static char *make_image(unsigned size)
{
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("macio-nvram-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, size), ==, 0);
    close(fd);

    return g_steal_pointer(&path);
}

static void read_image(const char *path, unsigned size, uint8_t *out)
{
    g_autofree char *data = NULL;
    gsize len = 0;

    g_assert_true(g_file_get_contents(path, &data, &len, NULL));
    g_assert_cmpuint(len, ==, size);
    memcpy(out, data, size);
}

static void write_image(const char *path, const uint8_t *data, unsigned size)
{
    g_assert_true(g_file_set_contents(path, (const char *)data, size, NULL));
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static void run_machine(const NvramLayout *l, const char *path)
{
    QTestState *qts;

    qts = qtest_initf("-M %s -accel qtest "
                      "-drive if=mtd,format=raw,file=%s", l->machine, path);
    qtest_quit(qts);
}

static void test_nvram_image(const void *opaque)
{
    const NvramLayout *l = opaque;
    g_autofree char *path = make_image(l->size);
    g_autofree uint8_t *image = g_malloc0(l->size);

    /* An empty image is initialised, and the result reaches the file */
    run_machine(l, path);
    read_image(path, l->size, image);
    g_assert_cmpuint(get_be32(&image[l->gen_off]), ==, FORMATTED_GENERATION);

    /*
     * Now that the image holds a valid one, the machine has to leave it
     * alone: a generation the formatting code would never write has to
     * still be there afterwards.
     */
    put_be32(&image[l->gen_off], POKED_GENERATION);
    write_image(path, image, l->size);

    run_machine(l, path);
    read_image(path, l->size, image);
    g_assert_cmpuint(get_be32(&image[l->gen_off]), ==, POKED_GENERATION);

    unlink(path);
}

/*
 * A backing image has to be the size of the part, and a machine given the
 * wrong size refuses to start.  The message has to say both what the part
 * wanted and what it got: the length is not an errno, and putting it through
 * one leaves the user with "Unknown error -8192" and nothing to act on.
 *
 * A refused realize is a refused startup, so qtest_init() cannot watch it
 * happen: run QEMU and read what it said.  QTEST_QEMU_BINARY may carry
 * arguments of its own, hence the split into an argv.
 */
static void test_nvram_wrong_size(const void *opaque)
{
    const NvramLayout *l = opaque;
    /* The other layout's size: too small on NewWorld, too large on OldWorld */
    unsigned wrong = l->size == 0x4000 ? 0x2000 : 0x4000;
    g_autofree char *path = make_image(wrong);
    g_autofree char *cmdline = NULL;
    g_autofree char *expected = NULL;
    g_autofree char *err = NULL;
    g_auto(GStrv) argv = NULL;
    int status = 0;

    cmdline = g_strdup_printf("%s -M %s -accel qtest -display none "
                              "-drive if=mtd,format=raw,file=%s",
                              qtest_qemu_binary(NULL), l->machine, path);
    g_assert_true(g_shell_parse_argv(cmdline, NULL, &argv, NULL));
    g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH |
                               G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL,
                               NULL, &err, &status, NULL));
    g_assert_cmpint(status, !=, 0);

    expected = g_strdup_printf("is %u bytes, must be %u", wrong, l->size);
    g_assert_nonnull(strstr(err, expected));
    g_assert_null(strstr(err, "Unknown error"));

    unlink(path);
}

/*
 * Walk the partition chain an empty machine leaves behind and check the
 * signature and the whole twelve byte name field of every partition.  The
 * offsets are not written down: they follow from the lengths, so a chain
 * that does not tile the part exactly is a failure on its own.
 */
static void test_nvram_partitions(const void *opaque)
{
    const NvramLayout *l = opaque;
    g_autofree char *path = make_image(l->size);
    g_autofree uint8_t *image = g_malloc0(l->size);
    unsigned off = 0, i;

    run_machine(l, path);
    read_image(path, l->size, image);

    for (i = 0; i < l->nparts; i++) {
        char want[CHRP_PART_NAME_LEN];
        unsigned len;

        g_assert_cmpuint(off + 16, <=, l->size);

        len = ((image[off + 2] << 8) | image[off + 3]) * 16;
        g_assert_cmpuint(len, >=, 16);
        g_assert_cmpuint(off + len, <=, l->size);

        g_assert_cmphex(image[off], ==, l->parts[i].sig);

        /* The field is zero padded, and a twelve character name fills it */
        memset(want, 0, sizeof(want));
        g_assert_cmpuint(strlen(l->parts[i].name), <=, sizeof(want));
        memcpy(want, l->parts[i].name, strlen(l->parts[i].name));
        g_assert_cmpmem(&image[off + 4], sizeof(want), want, sizeof(want));

        off += len;
    }

    /* The chain covers the part and stops there */
    g_assert_cmpuint(off, ==, l->size);

    unlink(path);
}

/*
 * A bank that is being erased reads back as all ones, which is a state the
 * guests pass through every time they update the other one.  The image is
 * still worth keeping, so an interrupted erase must not cost the bank that
 * survived it.
 */
static void erased_bank_case(const NvramLayout *l, unsigned erased)
{
    unsigned kept = erased ? 0 : l->bank;
    g_autofree char *path = make_image(l->size);
    g_autofree uint8_t *image = g_malloc0(l->size);

    run_machine(l, path);
    read_image(path, l->size, image);

    memset(&image[erased], 0xff, l->bank);
    put_be32(&image[kept + 0x14], POKED_GENERATION);
    write_image(path, image, l->size);

    run_machine(l, path);
    read_image(path, l->size, image);

    g_assert_cmpuint(get_be32(&image[kept + 0x14]), ==, POKED_GENERATION);
    g_assert_cmphex(image[erased], ==, 0xff);

    unlink(path);
}

static void test_nvram_erased_first(const void *opaque)
{
    const NvramLayout *l = opaque;

    erased_bank_case(l, 0);
}

static void test_nvram_erased_last(const void *opaque)
{
    const NvramLayout *l = opaque;

    erased_bank_case(l, l->bank);
}

/*
 * A part that has been erased and not programmed again reads back as all
 * ones, and a header of ones passes its own checksum, so nothing but the
 * value of the signature says that there is no NVRAM here.
 */
static void test_nvram_all_ones(const void *opaque)
{
    const NvramLayout *l = opaque;
    g_autofree char *path = make_image(l->size);
    g_autofree uint8_t *image = g_malloc0(l->size);

    memset(image, 0xff, l->size);
    write_image(path, image, l->size);

    run_machine(l, path);
    read_image(path, l->size, image);

    g_assert_cmpuint(get_be32(&image[l->gen_off]), ==, FORMATTED_GENERATION);

    unlink(path);
}

/* An image that is not an NVRAM at all leaves nothing of itself behind */
static void test_nvram_rubbish(const void *opaque)
{
    const NvramLayout *l = opaque;
    g_autofree char *path = make_image(l->size);
    g_autofree uint8_t *image = g_malloc0(l->size);
    unsigned i;

    memset(image, 0xab, l->size);
    write_image(path, image, l->size);

    run_machine(l, path);
    read_image(path, l->size, image);

    g_assert_cmpuint(get_be32(&image[l->gen_off]), ==, FORMATTED_GENERATION);
    for (i = 0; i < l->size; i++) {
        if (image[i] == 0xab) {
            g_error("byte %u of the image was left at 0xab", i);
        }
    }

    unlink(path);
}

/*
 * A machine that is waiting for an incoming migration has its block
 * backends inactive, so the initialisation must not write to them.  The
 * image the migration brings replaces whatever is there anyway.
 */
static void test_nvram_incoming(const void *opaque)
{
    const NvramLayout *l = opaque;
    g_autofree char *path = make_image(l->size);
    g_autofree uint8_t *image = g_malloc0(l->size);
    QTestState *qts;
    QDict *rsp;
    unsigned i;

    qts = qtest_initf("-M %s -accel qtest -incoming defer "
                      "-drive if=mtd,format=raw,file=%s", l->machine, path);

    rsp = qtest_qmp_assert_success_ref(qts, "{ 'execute': 'query-status' }");
    g_assert_cmpstr(qdict_get_str(rsp, "status"), ==, "inmigrate");
    qobject_unref(rsp);
    qtest_quit(qts);

    /* Nothing was written, so the image is as empty as it started */
    read_image(path, l->size, image);
    for (i = 0; i < l->size; i++) {
        if (image[i]) {
            g_error("byte %u of the image was written to", i);
        }
    }

    unlink(path);
}

static void wait_for_running(QTestState *qts)
{
    unsigned i;

    /*
     * The write back runs from vm_start(), and a QMP command is answered
     * from the main loop after that has finished.  Waiting for the RESUME
     * event instead would race: it is sent before the state handlers run.
     */
    for (i = 0; i < 5000; i++) {
        QDict *rsp = qtest_qmp_assert_success_ref(qts,
                                                  "{ 'execute': 'query-status' }");
        bool running = qdict_get_bool(rsp, "running");

        qobject_unref(rsp);
        if (running) {
            return;
        }
        g_usleep(1000);
    }

    g_error("the destination never started running");
}

/* Give a machine a valid image carrying a generation of its own */
static void seed_image(const NvramLayout *l, const char *path, uint32_t gen)
{
    g_autofree uint8_t *image = g_malloc0(l->size);

    run_machine(l, path);
    read_image(path, l->size, image);
    put_be32(&image[l->gen_off], gen);
    write_image(path, image, l->size);
}

/*
 * What the destination holds has to be replaced by what came over the
 * wire.  Both sides start from a valid image so that the check cannot
 * pass by the destination simply being formatted, and the two carry
 * different generations so that the direction of the copy is visible.
 */
static void test_nvram_migrate(const void *opaque)
{
    const NvramLayout *l = opaque;
    g_autofree char *src_path = make_image(l->size);
    g_autofree char *dst_path = make_image(l->size);
    g_autofree uint8_t *image = g_malloc0(l->size);
    g_autofree uint8_t *sent = g_malloc0(l->size);
    g_autofree char *uri = NULL;
    QTestState *src, *dst;

    seed_image(l, src_path, POKED_GENERATION);
    seed_image(l, dst_path, OTHER_GENERATION);
    read_image(src_path, l->size, sent);

    /* The two must not share a file: the second one could not lock it */
    uri = g_strdup_printf("unix:%s/macio-nvram-migrate-%s",
                          g_get_tmp_dir(), l->machine);
    unlink(uri + strlen("unix:"));

    src = qtest_initf("-M %s -accel qtest "
                      "-drive if=mtd,format=raw,file=%s", l->machine, src_path);
    dst = qtest_initf("-M %s -accel qtest -incoming defer "
                      "-drive if=mtd,format=raw,file=%s", l->machine, dst_path);

    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, NULL, uri, NULL, "{}");
    wait_for_migration_complete(src);
    wait_for_running(dst);

    read_image(dst_path, l->size, image);
    g_assert_cmpuint(get_be32(&image[l->gen_off]), ==, POKED_GENERATION);
    g_assert_cmpmem(image, l->size, sent, l->size);

    /* The source keeps what it had: it writes through on every store */
    read_image(src_path, l->size, image);
    g_assert_cmpmem(image, l->size, sent, l->size);

    qtest_quit(dst);
    qtest_quit(src);
    unlink(src_path);
    unlink(dst_path);
    unlink(uri + strlen("unix:"));
}

/*
 * Without a backing image there is nothing on disk to look at, so read the
 * part itself.  The two ends are given different Open Firmware variables so
 * that the comparison cannot pass by both having been formatted the same.
 */
static void test_nvram_migrate_no_drive(const void *opaque)
{
    const NvramLayout *l = opaque;
    g_autofree uint8_t *from = g_malloc0(l->size);
    g_autofree uint8_t *to = g_malloc0(l->size);
    g_autofree char *uri = NULL;
    QTestState *src, *dst;

    uri = g_strdup_printf("unix:%s/macio-nvram-nodrive-%s",
                          g_get_tmp_dir(), l->machine);
    unlink(uri + strlen("unix:"));

    src = qtest_initf("-M %s -accel qtest -prom-env g5mig=migrated",
                      l->machine);
    dst = qtest_initf("-M %s -accel qtest -incoming defer", l->machine);

    qtest_memread(src, l->addr, from, l->size);
    qtest_memread(dst, l->addr, to, l->size);
    g_assert_cmpint(memcmp(from, to, l->size), !=, 0);

    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, NULL, uri, NULL, "{}");
    wait_for_migration_complete(src);
    wait_for_running(dst);

    qtest_memread(dst, l->addr, to, l->size);
    g_assert_cmpmem(to, l->size, from, l->size);

    qtest_quit(dst);
    qtest_quit(src);
    unlink(uri + strlen("unix:"));
}

int main(int argc, char *argv[])
{
    const char *arch = qtest_get_arch();
    int i;

    g_test_init(&argc, &argv, NULL);

    for (i = 0; i < ARRAY_SIZE(layouts); i++) {
        g_autofree char *name = g_strdup_printf("%s/macio-nvram/%s", arch,
                                                layouts[i].machine);
        g_autofree char *parts = NULL;
        g_autofree char *rubbish = NULL;
        g_autofree char *ones = NULL;
        g_autofree char *incoming = NULL;
        g_autofree char *migrate = NULL;
        g_autofree char *no_drive = NULL;
        g_autofree char *erased = NULL;
        g_autofree char *erased_last = NULL;
        g_autofree char *wrong = NULL;

        if (!qtest_has_machine(layouts[i].machine)) {
            continue;
        }
        qtest_add_data_func(name, &layouts[i], test_nvram_image);

        parts = g_strdup_printf("%s/partitions", name);
        qtest_add_data_func(parts, &layouts[i], test_nvram_partitions);

        wrong = g_strdup_printf("%s/wrong-size", name);
        qtest_add_data_func(wrong, &layouts[i], test_nvram_wrong_size);

        rubbish = g_strdup_printf("%s/rubbish", name);
        qtest_add_data_func(rubbish, &layouts[i], test_nvram_rubbish);

        ones = g_strdup_printf("%s/all-ones", name);
        qtest_add_data_func(ones, &layouts[i], test_nvram_all_ones);

        incoming = g_strdup_printf("%s/incoming", name);
        qtest_add_data_func(incoming, &layouts[i], test_nvram_incoming);

        migrate = g_strdup_printf("%s/migrate", name);
        qtest_add_data_func(migrate, &layouts[i], test_nvram_migrate);

        /* Only a part with more than one erase block can lose one */
        if (layouts[i].size > layouts[i].bank) {
            erased = g_strdup_printf("%s/erased-first", name);
            qtest_add_data_func(erased, &layouts[i], test_nvram_erased_first);
            erased_last = g_strdup_printf("%s/erased-last", name);
            qtest_add_data_func(erased_last, &layouts[i],
                                test_nvram_erased_last);
        }

        if (layouts[i].addr) {
            no_drive = g_strdup_printf("%s/migrate-no-drive", name);
            qtest_add_data_func(no_drive, &layouts[i],
                                test_nvram_migrate_no_drive);
        }
    }

    return g_test_run();
}
