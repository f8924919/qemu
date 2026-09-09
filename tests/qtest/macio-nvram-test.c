/*
 * QTest testcase for the macio NVRAM on the NewWorld PowerMac machines
 *
 * The board formats the NVRAM when the machine is created, so its contents
 * can be read back through the MMIO window with no firmware running.  The
 * checks walk the CHRP partition chain the way the guests do and pin the
 * signature, header checksum and twelve byte name field of every partition.
 *
 * The OldWorld machine is not covered: there the NVRAM sits behind a PCI
 * BAR that nothing has assigned an address to when no firmware runs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define NVRAM_ADDR          0xfff04000

#define CHRP_PART_HDR_LEN   16
#define CHRP_PART_NAME_LEN  12

/*
 * One entry of the CHRP partition chain.  The name is the string the field
 * is expected to hold; the field itself is twelve bytes and zero padded.
 */
typedef struct {
    uint8_t sig;
    const char *name;
} NvramPart;

typedef struct {
    const char *machine;
    /* The part is spread out one byte every (1 << it_shift) addresses */
    unsigned it_shift;
    /* Size of the part in bytes */
    unsigned size;
    /* Number of banks the part is split into, each holding the same chain */
    unsigned nbanks;
    const NvramPart *parts;
    unsigned nparts;
} NvramLayout;

/*
 * A single bank: an Open Firmware half holding the variables and a free
 * partition, and a Mac OS X half.
 */
static const NvramPart mac99_parts[] = {
    { 0x70, "common" }, { 0x7f, "free" }, { 0x5a, "wwwwwwwwwww" },
};

static const NvramLayout layouts[] = {
    { "mac99", 1, 0x2000, 1, mac99_parts, ARRAY_SIZE(mac99_parts) },
};

static void read_part(QTestState *qts, const NvramLayout *l, uint8_t *out)
{
    unsigned i;

    for (i = 0; i < l->size; i++) {
        out[i] = qtest_readb(qts, NVRAM_ADDR + (i << l->it_shift));
    }
}

/* The CHRP header checksum, as the firmware and the guests compute it */
static uint8_t chrp_checksum(const uint8_t *hdr)
{
    unsigned i, sum = hdr[0];

    for (i = 2; i < CHRP_PART_HDR_LEN; i++) {
        sum += hdr[i];
        sum = (sum + ((sum & 0xff00) >> 8)) & 0xff;
    }
    return sum;
}

/* Walk the partition chain from @off to @end and compare it with @parts */
static void check_chain(const uint8_t *data, unsigned off, unsigned end,
                        const NvramPart *parts, unsigned nparts)
{
    unsigned n = 0;

    while (off < end) {
        const uint8_t *hdr = &data[off];
        unsigned len = ((hdr[2] << 8) | hdr[3]) * 16;
        char name[CHRP_PART_NAME_LEN] = { 0 };

        g_assert_cmpuint(n, <, nparts);
        g_assert_cmphex(hdr[0], ==, parts[n].sig);
        g_assert_cmphex(hdr[1], ==, chrp_checksum(hdr));
        memcpy(name, parts[n].name,
               MIN(strlen(parts[n].name), sizeof(name)));
        g_assert_cmpmem(&hdr[4], CHRP_PART_NAME_LEN, name, sizeof(name));

        g_assert_cmpuint(len, >=, CHRP_PART_HDR_LEN);
        off += len;
        n++;
    }
    g_assert_cmpuint(off, ==, end);
    g_assert_cmpuint(n, ==, nparts);
}

static void test_nvram_partitions(const void *opaque)
{
    const NvramLayout *l = opaque;
    unsigned bank_size = l->size / l->nbanks;
    g_autofree uint8_t *data = g_malloc(l->size);
    QTestState *qts;
    unsigned i;

    qts = qtest_initf("-M %s", l->machine);
    read_part(qts, l, data);
    qtest_quit(qts);

    for (i = 0; i < l->nbanks; i++) {
        check_chain(data, i * bank_size, (i + 1) * bank_size,
                    l->parts, l->nparts);
    }
}

int main(int argc, char **argv)
{
    unsigned i;

    g_test_init(&argc, &argv, NULL);

    for (i = 0; i < ARRAY_SIZE(layouts); i++) {
        const NvramLayout *l = &layouts[i];
        g_autofree char *path = NULL;

        if (!qtest_has_machine(l->machine)) {
            continue;
        }
        path = g_strdup_printf("macio-nvram/%s/partitions", l->machine);
        qtest_add_data_func(path, l, test_nvram_partitions);
    }

    return g_test_run();
}
