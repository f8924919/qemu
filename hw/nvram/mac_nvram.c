/*
 * PowerMac NVRAM emulation
 *
 * Copyright (c) 2005-2007 Fabrice Bellard
 * Copyright (c) 2007 Jocelyn Mayer
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/nvram/chrp_nvram.h"
#include "hw/nvram/mac_nvram.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/block-backend.h"
#include "migration/vmstate.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "trace.h"
#include <zlib.h> /* for adler32 */

#define DEF_SYSTEM_SIZE 0xc10

/* macio style NVRAM device */
static void macio_nvram_writeb(void *opaque, hwaddr addr,
                               uint64_t value, unsigned size)
{
    MacIONVRAMState *s = opaque;

    addr = (addr >> s->it_shift) & (s->size - 1);
    trace_macio_nvram_write(addr, value);
    s->data[addr] = value;
    if (s->blk) {
        if (blk_pwrite(s->blk, addr, 1, &s->data[addr], 0) < 0) {
            error_report("%s: write of NVRAM data to backing store failed",
                         blk_name(s->blk));
        }
    }
}

static uint64_t macio_nvram_readb(void *opaque, hwaddr addr,
                                  unsigned size)
{
    MacIONVRAMState *s = opaque;
    uint32_t value;

    addr = (addr >> s->it_shift) & (s->size - 1);
    value = s->data[addr];
    trace_macio_nvram_read(addr, value);

    return value;
}

static const MemoryRegionOps macio_nvram_ops = {
    .read = macio_nvram_readb,
    .write = macio_nvram_writeb,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_BIG_ENDIAN,
};

static const VMStateDescription vmstate_macio_nvram = {
    .name = "macio_nvram",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_VBUFFER_UINT32(data, MacIONVRAMState, 0, NULL, size),
        VMSTATE_END_OF_LIST()
    }
};


static void macio_nvram_reset(DeviceState *dev)
{
}

static void macio_nvram_realizefn(DeviceState *dev, Error **errp)
{
    SysBusDevice *d = SYS_BUS_DEVICE(dev);
    MacIONVRAMState *s = MACIO_NVRAM(dev);

    s->data = g_malloc0(s->size);

    if (s->blk) {
        int64_t len = blk_getlength(s->blk);
        if (len < 0) {
            error_setg_errno(errp, -len,
                             "could not get length of nvram backing image");
            return;
        } else if (len != s->size) {
            error_setg_errno(errp, -len,
                             "invalid size nvram backing image");
            return;
        }
        if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0) {
            return;
        }
        if (blk_pread(s->blk, 0, s->size, s->data, 0) < 0) {
            error_setg(errp, "can't read-nvram contents");
            return;
        }
    }

    memory_region_init_io(&s->mem, OBJECT(s), &macio_nvram_ops, s,
                          "macio-nvram", s->size << s->it_shift);
    sysbus_init_mmio(d, &s->mem);
}

static void macio_nvram_unrealizefn(DeviceState *dev)
{
    MacIONVRAMState *s = MACIO_NVRAM(dev);

    g_free(s->data);
}

static const Property macio_nvram_properties[] = {
    DEFINE_PROP_UINT32("size", MacIONVRAMState, size, 0),
    DEFINE_PROP_UINT32("it_shift", MacIONVRAMState, it_shift, 0),
    DEFINE_PROP_DRIVE("drive", MacIONVRAMState, blk),
};

static void macio_nvram_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = macio_nvram_realizefn;
    dc->unrealize = macio_nvram_unrealizefn;
    device_class_set_legacy_reset(dc, macio_nvram_reset);
    dc->vmsd = &vmstate_macio_nvram;
    device_class_set_props(dc, macio_nvram_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo macio_nvram_type_info = {
    .name = TYPE_MACIO_NVRAM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MacIONVRAMState),
    .class_init = macio_nvram_class_init,
};

static void macio_nvram_register_types(void)
{
    type_register_static(&macio_nvram_type_info);
}

/* Set up a system OpenBIOS NVRAM partition */
static void pmac_format_nvram_partition_of(MacIONVRAMState *nvr, int off,
                                           int len)
{
    int sysp_end;

    /* OpenBIOS nvram variables partition */
    sysp_end = chrp_nvram_create_system_partition(&nvr->data[off],
                                                  DEF_SYSTEM_SIZE, len) + off;

    /* Free space partition */
    chrp_nvram_create_free_partition(&nvr->data[sysp_end], len - sysp_end);
}

#define OSX_NVRAM_SIGNATURE     (0x5A)

/* Set up a Mac OS X NVRAM partition */
static void pmac_format_nvram_partition_osx(MacIONVRAMState *nvr, int off,
                                            int len)
{
    uint32_t start = off;
    ChrpNvramPartHdr *part_header;
    unsigned char *data = &nvr->data[start];

    /* empty partition */
    part_header = (ChrpNvramPartHdr *)data;
    part_header->signature = OSX_NVRAM_SIGNATURE;
    pstrcpy(part_header->name, sizeof(part_header->name), "wwwwwwwwwwww");

    chrp_nvram_finish_partition(part_header, len);

    /* Generation */
    stl_be_p(&data[20], 2);

    /* Adler32 checksum */
    stl_be_p(&data[16], adler32(0, &data[20], len - 20));
}

/*
 * Offsets inside the 32-byte core99 header.  The first 16 bytes are an
 * ordinary CHRP partition header, so a firmware that only knows about CHRP
 * walks straight over it; Mac OS X and Linux read the two fields behind it.
 */
#define CORE99_ADLER_OFFSET     0x10
#define CORE99_GENERATION_OFFSET 0x14
#define CORE99_HEADER_SIZE      0x20

/*
 * Format one core99 bank: the header above, the "common" partition holding
 * the Open Firmware variables, and a free partition covering the rest.
 *
 * Both Mac OS X (Core99NVRAM) and Linux (arch/powerpc/platforms/powermac)
 * validate a bank by its signature, the CHRP checksum of its header, and an
 * adler32 over everything from the generation field to the end of the bank.
 */
static void pmac_format_nvram_bank_core99(uint8_t *bank, uint32_t generation)
{
    ChrpNvramPartHdr *hdr = (ChrpNvramPartHdr *)bank;
    int end;

    hdr->signature = OSX_NVRAM_SIGNATURE;
    pstrcpy(hdr->name, sizeof(hdr->name), "nvram");
    chrp_nvram_finish_partition(hdr, CORE99_HEADER_SIZE);

    /* The bank with the larger generation is the one the guest reads */
    stl_be_p(&bank[CORE99_GENERATION_OFFSET], generation);

    end = CORE99_HEADER_SIZE +
          chrp_nvram_create_system_partition(&bank[CORE99_HEADER_SIZE],
                                             DEF_SYSTEM_SIZE,
                                             CORE99_NVRAM_BANK_SIZE -
                                             CORE99_HEADER_SIZE);
    if (end < CORE99_NVRAM_BANK_SIZE) {
        chrp_nvram_create_free_partition(&bank[end],
                                         CORE99_NVRAM_BANK_SIZE - end);
    }

    /*
     * Covers everything that follows it, so it has to be computed last.
     * The seed is the adler32 initial value of 1, which is what the guests
     * use: Linux starts its open-coded loop with low = 1, and Mac OS X
     * calls the ordinary two-argument adler32().
     */
    stl_be_p(&bank[CORE99_ADLER_OFFSET],
             adler32(adler32(0, NULL, 0), &bank[CORE99_GENERATION_OFFSET],
                     CORE99_NVRAM_BANK_SIZE - CORE99_GENERATION_OFFSET));
}

/* Set up NVRAM as the two core99 banks the NewWorld guests expect */
void pmac_format_nvram_partition_core99(MacIONVRAMState *nvr, int len)
{
    int i;

    assert(len == CORE99_NVRAM_SIZE);

    /*
     * Every bank is a complete image.  They are given different generations
     * so that no tie has to be broken: Mac OS X 10.4 takes the second bank
     * when the generations are equal, Linux the first one.  Counting down
     * from the first bank makes both of them pick that one.
     */
    for (i = 0; i < CORE99_NVRAM_NBANKS; i++) {
        pmac_format_nvram_bank_core99(&nvr->data[i * CORE99_NVRAM_BANK_SIZE],
                                      CORE99_NVRAM_NBANKS - i);
    }
}

/* Set up NVRAM with OF and OSX partitions */
void pmac_format_nvram_partition(MacIONVRAMState *nvr, int len)
{
    /*
     * Mac OS X expects side "B" of the flash at the second half of NVRAM,
     * so we use half of the chip for OF and the other half for a free OSX
     * partition.
     */
    pmac_format_nvram_partition_of(nvr, 0, len / 2);
    pmac_format_nvram_partition_osx(nvr, len / 2, len / 2);
}
type_init(macio_nvram_register_types)
