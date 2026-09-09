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

/* Fields behind the CHRP header at the start of a core99 bank */
#define CORE99_ADLER_OFFSET 0x10
#define CORE99_GEN_OFFSET   0x14

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
    /* Whether every bank starts with a core99 header */
    bool core99;
    const NvramPart *parts;
    unsigned nparts;
} NvramLayout;

/*
 * Two identical core99 banks: the header the guests validate a bank by, the
 * Open Firmware variables and a free partition covering the rest.
 */
static const NvramPart mac99_parts[] = {
    { 0x5a, "nvram" }, { 0x70, "common" }, { 0x7f, "free" },
};

static const NvramLayout layouts[] = {
    { "mac99", 0, 0x4000, 2, true, mac99_parts, ARRAY_SIZE(mac99_parts) },
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

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/*
 * The adler32 over a core99 bank, as Linux computes it in
 * arch/powerpc/platforms/powermac/nvram.c: from the generation field to
 * the end of the bank, starting from the usual initial value of 1.
 */
static uint32_t core99_adler(const uint8_t *bank, unsigned bank_size)
{
    uint32_t low = 1, high = 0;
    unsigned i;

    for (i = CORE99_GEN_OFFSET; i < bank_size; i++) {
        low = (low + bank[i]) % 65521;
        high = (high + low) % 65521;
    }
    return (high << 16) | low;
}

/*
 * The guests take the bank with the larger generation, so the banks are
 * given different ones and the first bank wins.
 */
static void check_core99_bank(const uint8_t *bank, unsigned bank_size,
                              uint32_t generation)
{
    g_assert_cmphex(get_be32(&bank[CORE99_GEN_OFFSET]), ==, generation);
    g_assert_cmphex(get_be32(&bank[CORE99_ADLER_OFFSET]), ==,
                    core99_adler(bank, bank_size));
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
        if (l->core99) {
            check_core99_bank(&data[i * bank_size], bank_size,
                              l->nbanks - i);
        }
    }
}

/*
 * The NewWorld part is a flash part driven with the Sharp/Micron command
 * set.  Every store is a command, so each case starts a machine of its own
 * and prepares nothing by writing: it reads what the board formatted.
 */
#define FLASH_ERASE_SETUP   0x20
#define FLASH_ERASE_CONFIRM 0xd0
#define FLASH_WRITE_SETUP   0x40
#define FLASH_READ_ARRAY    0xff
#define FLASH_STATUS_DONE   0x80

#define BANK1               0x2000
#define BANK_SIZE           0x2000

static void flash_erase(QTestState *qts, uint64_t addr)
{
    qtest_writeb(qts, addr, FLASH_ERASE_SETUP);
    qtest_writeb(qts, addr, FLASH_ERASE_CONFIRM);
}

/* Erase confirm clears the block the address falls in, and only that one */
static void test_flash_erase(const void *opaque)
{
    const NvramLayout *l = opaque;
    QTestState *qts = qtest_initf("-M %s", l->machine);
    unsigned i;

    flash_erase(qts, NVRAM_ADDR + BANK1);
    g_assert_cmphex(qtest_readb(qts, NVRAM_ADDR + BANK1), ==,
                    FLASH_STATUS_DONE);
    qtest_writeb(qts, NVRAM_ADDR + BANK1, FLASH_READ_ARRAY);

    for (i = 0; i < BANK_SIZE; i++) {
        g_assert_cmphex(qtest_readb(qts, NVRAM_ADDR + BANK1 + i), ==, 0xff);
    }
    g_assert_cmphex(qtest_readb(qts, NVRAM_ADDR), ==, 0x5a);

    qtest_quit(qts);
}

/* Erase setup followed by anything but erase confirm leaves the block */
static void test_flash_erase_setup_alone(const void *opaque)
{
    const NvramLayout *l = opaque;
    QTestState *qts = qtest_initf("-M %s", l->machine);

    qtest_writeb(qts, NVRAM_ADDR + BANK1, FLASH_ERASE_SETUP);
    qtest_writeb(qts, NVRAM_ADDR + BANK1, FLASH_READ_ARRAY);
    qtest_writeb(qts, NVRAM_ADDR + BANK1, FLASH_READ_ARRAY);

    g_assert_cmphex(qtest_readb(qts, NVRAM_ADDR + BANK1), ==, 0x5a);

    qtest_quit(qts);
}

/* A store that is not part of a command sequence does not change the part */
static void test_flash_plain_store(const void *opaque)
{
    const NvramLayout *l = opaque;
    QTestState *qts = qtest_initf("-M %s", l->machine);
    uint64_t addr = NVRAM_ADDR + BANK1 - 1;
    uint8_t old = qtest_readb(qts, addr);
    uint8_t val = old == 0xa5 ? 0x5a : 0xa5;

    qtest_writeb(qts, addr, val);
    g_assert_cmphex(qtest_readb(qts, addr), ==, old);

    qtest_quit(qts);
}

/*
 * Write setup followed by a store programs one byte, and reads answer with
 * the status until read array puts the part back.
 */
static void test_flash_program(const void *opaque)
{
    const NvramLayout *l = opaque;
    QTestState *qts = qtest_initf("-M %s", l->machine);
    uint64_t addr = NVRAM_ADDR + BANK1 + 0x10;

    flash_erase(qts, addr);
    qtest_writeb(qts, addr, FLASH_READ_ARRAY);

    qtest_writeb(qts, addr, FLASH_WRITE_SETUP);
    qtest_writeb(qts, addr, 0x12);
    g_assert_cmphex(qtest_readb(qts, addr), ==, FLASH_STATUS_DONE);
    g_assert_cmphex(qtest_readb(qts, addr + 1), ==, FLASH_STATUS_DONE);

    qtest_writeb(qts, addr, FLASH_READ_ARRAY);
    g_assert_cmphex(qtest_readb(qts, addr), ==, 0x12);
    g_assert_cmphex(qtest_readb(qts, addr + 1), ==, 0xff);

    qtest_quit(qts);
}

static void add_test(const NvramLayout *l, const char *name,
                     GTestDataFunc fn)
{
    g_autofree char *path = g_strdup_printf("macio-nvram/%s/%s",
                                            l->machine, name);

    qtest_add_data_func(path, l, fn);
}

int main(int argc, char **argv)
{
    unsigned i;

    g_test_init(&argc, &argv, NULL);

    for (i = 0; i < ARRAY_SIZE(layouts); i++) {
        const NvramLayout *l = &layouts[i];

        if (!qtest_has_machine(l->machine)) {
            continue;
        }
        add_test(l, "partitions", test_nvram_partitions);
        if (l->core99) {
            add_test(l, "flash/erase", test_flash_erase);
            add_test(l, "flash/erase-setup-alone",
                     test_flash_erase_setup_alone);
            add_test(l, "flash/plain-store", test_flash_plain_store);
            add_test(l, "flash/program", test_flash_program);
        }
    }

    return g_test_run();
}
