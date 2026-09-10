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
#include "qemu/log.h"
#include "system/system.h"
#include "trace.h"
#include <zlib.h> /* for adler32 */

#define DEF_SYSTEM_SIZE 0xc10

/*
 * Command set of the flash part behind macio on the NewWorld machines.
 * Linux spells the same values out in arch/powerpc/platforms/powermac/
 * nvram.c, and Mac OS X drives the part the same way: erase a bank, then
 * program it a byte at a time.  A store that is not part of one of those
 * sequences does not change what the part holds.
 */
#define SM_FLASH_CMD_ERASE_SETUP    0x20
#define SM_FLASH_CMD_ERASE_CONFIRM  0xd0
#define SM_FLASH_CMD_WRITE_SETUP    0x40
#define SM_FLASH_CMD_WRITE_SETUP2   0x10
#define SM_FLASH_CMD_CLEAR_STATUS   0x50
#define SM_FLASH_CMD_READ_STATUS    0x70
#define SM_FLASH_CMD_RESET          0xff

#define SM_FLASH_STATUS_DONE        0x80
#define SM_FLASH_STATUS_ERASE_ERR   0x20
#define SM_FLASH_STATUS_WRITE_ERR   0x10

static void macio_nvram_store(MacIONVRAMState *s, hwaddr addr, unsigned len)
{
    if (s->blk) {
        if (blk_pwrite(s->blk, addr, len, &s->data[addr], 0) < 0) {
            error_report("%s: write of NVRAM data to backing store failed",
                         blk_name(s->blk));
        }
    }
}

/* Erase the block the address falls in, leaving the other blocks alone */
static void macio_nvram_erase_block(MacIONVRAMState *s, hwaddr addr)
{
    hwaddr base = addr & ~((hwaddr)s->block_size - 1);

    memset(&s->data[base], 0xff, s->block_size);
    macio_nvram_store(s, base, s->block_size);
}

/* Consume one store, either inside a command sequence or starting one */
static void macio_nvram_command(MacIONVRAMState *s, hwaddr addr, uint8_t value)
{
    switch (s->cmd) {
    case SM_FLASH_CMD_ERASE_SETUP:
        if (value == SM_FLASH_CMD_ERASE_CONFIRM) {
            macio_nvram_erase_block(s, addr);
            s->status |= SM_FLASH_STATUS_DONE;
        } else {
            s->status |= SM_FLASH_STATUS_DONE | SM_FLASH_STATUS_ERASE_ERR;
        }
        s->cmd = 0;
        s->reading_status = true;
        return;

    case SM_FLASH_CMD_WRITE_SETUP:
        s->data[addr] = value;
        macio_nvram_store(s, addr, 1);
        s->status |= SM_FLASH_STATUS_DONE;
        s->cmd = 0;
        s->reading_status = true;
        return;

    default:
        break;
    }

    switch (value) {
    case SM_FLASH_CMD_ERASE_SETUP:
        s->cmd = value;
        break;
    case SM_FLASH_CMD_WRITE_SETUP:
    case SM_FLASH_CMD_WRITE_SETUP2:
        s->cmd = SM_FLASH_CMD_WRITE_SETUP;
        break;
    case SM_FLASH_CMD_READ_STATUS:
        s->reading_status = true;
        break;
    case SM_FLASH_CMD_CLEAR_STATUS:
        s->status = 0;
        break;
    case SM_FLASH_CMD_RESET:
        s->reading_status = false;
        break;
    default:
        /* Anything else leaves the part reading out its contents again */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "macio-nvram: unknown flash command 0x%02x\n", value);
        s->reading_status = false;
        break;
    }

    /* Every store lands in one of the arms above; nothing to hand back */
}

/* macio style NVRAM device */
static void macio_nvram_writeb(void *opaque, hwaddr addr,
                               uint64_t value, unsigned size)
{
    MacIONVRAMState *s = opaque;

    addr = (addr >> s->it_shift) & (s->size - 1);
    trace_macio_nvram_write(addr, value);

    if (s->block_size) {
        trace_macio_nvram_flash_cmd(addr, value, s->cmd, s->status);
        macio_nvram_command(s, addr, value);
        return;
    }

    s->data[addr] = value;
    macio_nvram_store(s, addr, 1);
}

static uint64_t macio_nvram_readb(void *opaque, hwaddr addr,
                                  unsigned size)
{
    MacIONVRAMState *s = opaque;
    uint32_t value;

    addr = (addr >> s->it_shift) & (s->size - 1);
    if (s->block_size && s->reading_status) {
        trace_macio_nvram_read(addr, s->status);
        return s->status;
    }
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

static bool macio_nvram_flash_needed(void *opaque)
{
    MacIONVRAMState *s = opaque;

    return s->block_size != 0;
}

/*
 * Kept in a subsection so that the OldWorld machines, whose NVRAM has no
 * command state at all, still migrate as they always did.
 */
static const VMStateDescription vmstate_macio_nvram_flash = {
    .name = "macio_nvram/flash",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = macio_nvram_flash_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(cmd, MacIONVRAMState),
        VMSTATE_UINT8(status, MacIONVRAMState),
        VMSTATE_BOOL(reading_status, MacIONVRAMState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_macio_nvram = {
    .name = "macio_nvram",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_VBUFFER_UINT32(data, MacIONVRAMState, 0, NULL, size),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_macio_nvram_flash,
        NULL
    }
};


static void macio_nvram_reset(DeviceState *dev)
{
    MacIONVRAMState *s = MACIO_NVRAM(dev);

    s->cmd = 0;
    /* A part that has nothing to do reports itself ready */
    s->status = SM_FLASH_STATUS_DONE;
    s->reading_status = false;
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
    DEFINE_PROP_UINT32("block-size", MacIONVRAMState, block_size, 0),
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

/*
 * The machines format the NVRAM once the device has been realized, which
 * throws away whatever a backing image holds.  An image a guest has
 * already written is worth keeping: say whether this one looks like one.
 *
 * A bank the guest is in the middle of erasing reads back as all ones, and
 * that is a normal state of the ping-pong the core99 layout runs on, so one
 * good bank is enough.  Anything past that - the adler32 over the bank, the
 * generation numbers - is what the guests themselves check, and getting it
 * wrong here would throw away an image they could have used.
 */
static bool pmac_nvram_image_valid(MacIONVRAMState *nvr, int len)
{
    int bank = nvr->block_size ? nvr->block_size : len;
    int off;

    for (off = 0; off + bank <= len; off += bank) {
        const ChrpNvramPartHdr *hdr = (ChrpNvramPartHdr *)&nvr->data[off];

        if (hdr->signature != 0x00 && hdr->signature != 0xff &&
            hdr->checksum == chrp_nvram_checksum(hdr)) {
            return true;
        }
    }

    return false;
}

static bool pmac_nvram_keep_image(MacIONVRAMState *nvr, int len)
{
    if (!nvr->blk || !pmac_nvram_image_valid(nvr, len)) {
        return false;
    }

    /*
     * The Open Firmware variables live in the image from now on, and the
     * ones on the command line only ever reach the NVRAM through the
     * formatting below.
     */
    if (nb_prom_envs) {
        warn_report("macio-nvram: keeping the backing image, "
                    "-prom-env is ignored");
    }

    return true;
}

/* Hand the freshly formatted contents to the backing image, if there is one */
static void pmac_nvram_flush(MacIONVRAMState *nvr, int len)
{
    if (nvr->blk) {
        macio_nvram_store(nvr, 0, len);
    }
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

    if (pmac_nvram_keep_image(nvr, len)) {
        return;
    }
    memset(nvr->data, 0, len);

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

    pmac_nvram_flush(nvr, len);
}

/* Set up NVRAM with OF and OSX partitions */
void pmac_format_nvram_partition(MacIONVRAMState *nvr, int len)
{
    if (pmac_nvram_keep_image(nvr, len)) {
        return;
    }
    memset(nvr->data, 0, len);

    /*
     * Mac OS X expects side "B" of the flash at the second half of NVRAM,
     * so we use half of the chip for OF and the other half for a free OSX
     * partition.
     */
    pmac_format_nvram_partition_of(nvr, 0, len / 2);
    pmac_format_nvram_partition_osx(nvr, len / 2, len / 2);

    pmac_nvram_flush(nvr, len);
}
type_init(macio_nvram_register_types)
