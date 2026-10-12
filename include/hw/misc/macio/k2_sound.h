/*
 * K2 mac-io feature control registers and I2S-a sound cell
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_MACIO_K2_SOUND_H
#define HW_MISC_MACIO_K2_SOUND_H

#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qemu/audio.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_K2_I2S "k2-i2s"
OBJECT_DECLARE_SIMPLE_TYPE(K2I2SState, K2_I2S)

/* FCR10..FCR6 at 0x24..0x34, FCR0..FCR5 at 0x38..0x4c */
#define K2_FCR_BASE                 0x24
#define K2_FCR_COUNT                11
#define K2_FCR1                     0x3c

#define K2_FCR1_I2S0_CELL_ENABLE    0x00000400
#define K2_FCR1_I2S0_RESET          0x00000800
#define K2_FCR1_I2S0_CLK_ENABLE     0x00001000
#define K2_FCR1_I2S0_ENABLE         0x00002000

#define K2_I2S_BASE                 0x10000
#define K2_I2S_SIZE                 0x100

/* DBDMA register blocks of i2s-a, relative to the mac-io DBDMA base */
#define K2_I2S_DMA_OUT_OFFSET       0x000
#define K2_I2S_DMA_IN_OFFSET        0x100

/*
 * mpic inputs of the two DMA channels.  The real K2 uses 1 and 2, but
 * powermac7_3 keeps the mac-io IDE, whose DMA already takes 2 and 3, and
 * the ESCC DMA takes 4..7, so the input channel moves to 8.  The firmware's
 * "interrupts" property of i2s-a must match.
 */
#define K2_I2S_TX_DMA_IRQ           0x01
#define K2_I2S_RX_DMA_IRQ           0x08

/* The K2 Keywest i2c that carries the codecs */
#define K2_I2C_BASE                 0x18000
#define K2_I2C_IRQ                  0x1a

typedef struct K2I2SDir {
    QEMUTimer *timer;
    bool pending;
    int64_t deadline_ns;
} K2I2SDir;

struct K2I2SState {
    SysBusDevice parent_obj;

    MemoryRegion fcr_mem;
    MemoryRegion i2s_mem;
    qemu_irq irq[2];            /* 0 out, 1 in: to the pic */
    qemu_irq dma_irq[2];        /* handed to the DBDMA, forwarded to irq[] */
    uint32_t fcr[K2_FCR_COUNT];

    uint32_t intr_ctl;
    uint32_t serial_format;
    uint32_t data_word_sizes;
    uint64_t walk_frames;
    uint32_t frame_count_bias;
    K2I2SDir dir[2];            /* 0 out, 1 in */

    /* Properties */
    AudioBackend *audio_be;
    I2CSlave *codec;
    Object *dbdma;

    /* Host side; not migrated */
    SWVoiceOut *voice;
    int voice_rate;
    int voice_frame_bytes;
    uint8_t out_fifo[0x80000];
    uint32_t fifo_rptr;
    uint32_t fifo_wptr;
    uint32_t fifo_count;
    bool prebuffering;
    int64_t last_push_ns;
    int backend_size;
};

#endif
