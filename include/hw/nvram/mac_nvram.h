/*
 * PowerMac NVRAM emulation
 *
 * Copyright (c) 2004-2007 Fabrice Bellard
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

#ifndef MAC_NVRAM_H
#define MAC_NVRAM_H

#include "system/memory.h"
#include "hw/core/sysbus.h"

/*
 * The OldWorld machines expose an 8 KiB NVRAM.  The NewWorld ones expose a
 * 16 KiB window that the guest operating systems read as two 8 KiB "core99"
 * banks laid out back to back; each bank is a complete image and carries a
 * generation number, and the newer one wins.
 */
#define MACIO_NVRAM_SIZE 0x2000

#define CORE99_NVRAM_BANK_SIZE 0x2000
#define CORE99_NVRAM_NBANKS 2
#define CORE99_NVRAM_SIZE (CORE99_NVRAM_BANK_SIZE * CORE99_NVRAM_NBANKS)

#define TYPE_MACIO_NVRAM "macio-nvram"
OBJECT_DECLARE_SIMPLE_TYPE(MacIONVRAMState, MACIO_NVRAM)

struct MacIONVRAMState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    uint32_t size;
    uint32_t it_shift;
    /*
     * Size of an erase block, or zero on the parts that are plain SRAM.
     * The NewWorld machines carry a flash part, and a store to it only
     * lands once it has been unlocked by the command sequence below.
     */
    uint32_t block_size;

    MemoryRegion mem;
    uint8_t *data;
    BlockBackend *blk;

    /* Flash command state, unused when block_size is zero */
    uint8_t cmd;
    uint8_t status;
    bool reading_status;

    /* Set while a migration is waiting to hand the contents to the image */
    VMChangeStateEntry *vmstate;
};

void pmac_format_nvram_partition(MacIONVRAMState *nvr, int len);
void pmac_format_nvram_partition_core99(MacIONVRAMState *nvr, int len);

#endif /* MAC_NVRAM_H */
