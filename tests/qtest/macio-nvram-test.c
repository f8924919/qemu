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

/*
 * Both layouts carry a big endian generation number that the formatting
 * code writes, so a machine that reformats can be told from one that does
 * not by the value that survives.
 */
#define FORMATTED_GENERATION 2
#define POKED_GENERATION     7

typedef struct {
    const char *machine;
    unsigned size;
    unsigned bank;      /* Erase block, or the whole part where there is none */
    unsigned gen_off;
} NvramLayout;

static const NvramLayout layouts[] = {
    /* NewWorld: two core99 banks, generation inside the first bank header */
    { "mac99",        0x4000, 0x2000, 0x14 },
    { "powermac7_3",  0x4000, 0x2000, 0x14 },
    /* OldWorld: an Open Firmware half and a Mac OS X half, one erase block */
    { "g3beige",      0x2000, 0x2000, 0x1014 },
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

int main(int argc, char *argv[])
{
    const char *arch = qtest_get_arch();
    int i;

    g_test_init(&argc, &argv, NULL);

    for (i = 0; i < ARRAY_SIZE(layouts); i++) {
        g_autofree char *name = g_strdup_printf("%s/macio-nvram/%s", arch,
                                                layouts[i].machine);
        g_autofree char *rubbish = NULL;
        g_autofree char *ones = NULL;
        g_autofree char *erased = NULL;
        g_autofree char *erased_last = NULL;

        if (!qtest_has_machine(layouts[i].machine)) {
            continue;
        }
        qtest_add_data_func(name, &layouts[i], test_nvram_image);

        rubbish = g_strdup_printf("%s/rubbish", name);
        qtest_add_data_func(rubbish, &layouts[i], test_nvram_rubbish);

        ones = g_strdup_printf("%s/all-ones", name);
        qtest_add_data_func(ones, &layouts[i], test_nvram_all_ones);

        /* Only a part with more than one erase block can lose one */
        if (layouts[i].size > layouts[i].bank) {
            erased = g_strdup_printf("%s/erased-first", name);
            qtest_add_data_func(erased, &layouts[i], test_nvram_erased_first);
            erased_last = g_strdup_printf("%s/erased-last", name);
            qtest_add_data_func(erased_last, &layouts[i],
                                test_nvram_erased_last);
        }
    }

    return g_test_run();
}
