/*
 * Keywest i2c controller (Apple UniNorth/U3 and KeyLargo/K2)
 *
 * Eight byte-wide registers spaced by a power-of-two stride that the
 * firmware advertises as AAPL,address-step.  The programming model is
 * documented by two independent guests:
 *
 *  - Linux arch/powerpc/platforms/powermac/low_i2c.c
 *  - Darwin's AppleI2C (PPCI2CInterface), which MacRISC4CPU uses to
 *    freeze the timebase while bringing up the second CPU
 *
 * Transfers complete synchronously inside the MMIO handler.  That is not
 * a shortcut: Linux polls this controller with the timebase frozen and
 * says so in low_i2c.c ("we cannot rely on udelay nor schedule when in
 * polled mode"), so a model that deferred completion to a timer would
 * deadlock the guest rather than merely being slow.
 *
 * Copyright (c) 2026 qemu-g5 contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TYPE_KEYWEST_I2C "keywest-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(KeywestI2CState, KEYWEST_I2C)

/* Register indices, before the AAPL,address-step shift */
enum {
    KW_REG_MODE = 0,
    KW_REG_CONTROL,
    KW_REG_STATUS,
    KW_REG_ISR,
    KW_REG_IER,
    KW_REG_ADDR,
    KW_REG_SUBADDR,
    KW_REG_DATA,
    KW_REG_COUNT,
};

/* Mode */
#define KW_MODE_MODE_MASK   0x0c
#define KW_MODE_DUMB        0x00
#define KW_MODE_STANDARD    0x04
#define KW_MODE_STANDARDSUB 0x08
#define KW_MODE_COMBINED    0x0c
#define KW_MODE_CHAN_MASK   0xf0
#define KW_MODE_CHAN_SHIFT  4

/* Control */
#define KW_CTL_AAK          0x01
#define KW_CTL_XADDR        0x02
#define KW_CTL_STOP         0x04
#define KW_CTL_START        0x08

/* Status */
#define KW_STAT_BUSY        0x01
#define KW_STAT_LAST_AAK    0x02
#define KW_STAT_LAST_RW     0x04
#define KW_STAT_SDA         0x08
#define KW_STAT_SCL         0x10

/* ISR / IER */
#define KW_IRQ_DATA         0x01
#define KW_IRQ_ADDR         0x02
#define KW_IRQ_STOP         0x04
#define KW_IRQ_START        0x08
#define KW_IRQ_MASK         0x0f

#define KEYWEST_I2C_MAX_CHANNELS 2

struct KeywestI2CState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    qemu_irq irq;

    /*
     * The window Linux ioremaps is 4KB regardless of how far apart the
     * registers actually sit, so the region size and the stride are
     * independent knobs.
     */
    uint32_t reg_shift;
    uint32_t num_channels;

    I2CBus *bus[KEYWEST_I2C_MAX_CHANNELS];

    uint8_t mode;
    uint8_t control;
    uint8_t status;
    uint8_t isr;
    uint8_t ier;
    uint8_t addr;
    uint8_t subaddr;
    uint8_t data;

    /* True between a successful address phase and the STOP that ends it */
    bool xfer_active;
    /* Direction of the active transfer, taken from bit 0 of addr */
    bool xfer_recv;
    /*
     * Whether a data interrupt is still owed to the guest.
     *
     * low_i2c.c acknowledges KW_I2C_IRQ_DATA *twice* for one interrupt:
     * once inside the read branch and once unconditionally at the end of
     * the block, with the AAK change sitting between them.  Real hardware
     * keeps the condition asserted, so the second acknowledge sees the
     * interrupt again.  Dropping it on the first one loses the byte.
     */
    bool data_ready;        /* a received byte is waiting to be read */
    bool write_done;        /* the byte last written has gone out */
};

static void keywest_i2c_stop(KeywestI2CState *s);

static I2CBus *keywest_i2c_active_bus(KeywestI2CState *s)
{
    uint32_t chan = (s->mode & KW_MODE_CHAN_MASK) >> KW_MODE_CHAN_SHIFT;

    if (chan >= s->num_channels) {
        return NULL;
    }
    return s->bus[chan];
}

static void keywest_i2c_update_irq(KeywestI2CState *s)
{
    qemu_set_irq(s->irq, !!(s->isr & s->ier & KW_IRQ_MASK));
}

static void keywest_i2c_raise(KeywestI2CState *s, uint8_t bits)
{
    s->isr |= bits;
    keywest_i2c_update_irq(s);
}

/*
 * Start a transfer.  Both guests kick this off by writing XADDR to the
 * control register after loading mode/addr/subaddr.
 *
 * COMBINED is the awkward one: the address register already holds the read
 * address (bit 0 set), but the first phase still addresses the slave for
 * writing so that the subaddress can be sent, and only then is the bus
 * restarted for reading.
 */
static void keywest_i2c_start(KeywestI2CState *s)
{
    I2CBus *bus = keywest_i2c_active_bus(s);
    uint8_t saddr = s->addr >> 1;
    uint8_t xmode = s->mode & KW_MODE_MODE_MASK;
    bool nak = true;

    s->xfer_recv = s->addr & 1;

    trace_keywest_i2c_xfer_start(s->addr, s->mode, s->subaddr);

    if (bus == NULL) {
        /*
         * A channel nobody wired up.  Report a NAK rather than staying
         * silent: the guest polls for the address interrupt and would
         * otherwise wait for one that never comes.
         */
        goto done;
    }

    switch (xmode) {
    case KW_MODE_STANDARD:
        nak = i2c_start_transfer(bus, saddr, s->xfer_recv) != 0;
        break;

    case KW_MODE_STANDARDSUB:
        nak = i2c_start_send(bus, saddr) != 0;
        if (!nak) {
            nak = i2c_send(bus, s->subaddr) != 0;
        }
        if (!nak && s->xfer_recv) {
            /* Repeated start into the read phase */
            nak = i2c_start_recv(bus, saddr) != 0;
        }
        break;

    case KW_MODE_COMBINED:
        nak = i2c_start_send(bus, saddr) != 0;
        if (!nak) {
            nak = i2c_send(bus, s->subaddr) != 0;
        }
        if (!nak) {
            nak = i2c_start_recv(bus, saddr) != 0;
        }
        break;

    default:
        /*
         * DUMB and anything else we have not seen a guest use.  Fail the
         * transfer instead of asserting: a half-implemented mode that
         * stalls is worse than one that reports an error.
         */
        qemu_log_mask(LOG_UNIMP, "keywest-i2c: unsupported mode 0x%02x\n",
                      xmode);
        break;
    }

done:
    if (nak) {
        if (bus != NULL) {
            i2c_end_transfer(bus);
        }
        s->status &= ~KW_STAT_LAST_AAK;
        s->xfer_active = false;
        trace_keywest_i2c_nak(s->addr, s->mode);
        /*
         * A NAK ends the transfer by itself.  low_i2c.c reacts to the
         * address interrupt by recording -ENXIO and moving to state_stop
         * without writing STOP, then waits for the stop interrupt, so
         * raising only ADDR here leaves it spinning until it times out.
         */
        keywest_i2c_raise(s, KW_IRQ_STOP);
    } else {
        s->status |= KW_STAT_LAST_AAK;
        s->xfer_active = true;
        if (xmode == KW_MODE_COMBINED) {
            s->xfer_recv = true;
        }
        s->status |= KW_STAT_BUSY;
    }
    keywest_i2c_raise(s, KW_IRQ_ADDR);
}

static void keywest_i2c_stop(KeywestI2CState *s)
{
    I2CBus *bus = keywest_i2c_active_bus(s);

    if (s->xfer_active && bus != NULL) {
        i2c_end_transfer(bus);
    }
    s->xfer_active = false;
    s->data_ready = false;
    s->write_done = false;
    s->status &= ~KW_STAT_BUSY;
    keywest_i2c_raise(s, KW_IRQ_STOP);
}

/* Pull the next byte in during a read transfer. */
static void keywest_i2c_recv_byte(KeywestI2CState *s)
{
    I2CBus *bus = keywest_i2c_active_bus(s);

    if (!s->xfer_active || bus == NULL) {
        return;
    }
    s->data = i2c_recv(bus);
    s->data_ready = true;
    keywest_i2c_raise(s, KW_IRQ_DATA);
}

static uint64_t keywest_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    KeywestI2CState *s = KEYWEST_I2C(opaque);
    unsigned reg = addr >> s->reg_shift;
    uint64_t val = 0;

    if ((addr & ((1u << s->reg_shift) - 1)) || reg >= KW_REG_COUNT) {
        /* Unmapped hole inside the 4KB window */
        return 0;
    }

    switch (reg) {
    case KW_REG_MODE:
        val = s->mode;
        break;
    case KW_REG_CONTROL:
        val = s->control;
        break;
    case KW_REG_STATUS:
        val = s->status;
        break;
    case KW_REG_ISR:
        val = s->isr;
        break;
    case KW_REG_IER:
        val = s->ier;
        break;
    case KW_REG_ADDR:
        val = s->addr;
        break;
    case KW_REG_SUBADDR:
        /*
         * low_i2c.c reads this register after *every* register write as a
         * write barrier, so it must stay free of side effects.
         */
        val = s->subaddr;
        break;
    case KW_REG_DATA:
        /*
         * Plain read.  The next byte is fetched when the guest acknowledges
         * this one, not here: low_i2c.c reads the register and immediately
         * writes KW_I2C_IRQ_DATA to the ISR, so anything raised now would
         * be cleared by that ack and the guest would wait forever.
         */
        val = s->data;
        s->data_ready = false;
        break;
    }

    trace_keywest_i2c_reg_read(reg, val);
    return val;
}

static void keywest_i2c_write(void *opaque, hwaddr addr, uint64_t val64,
                              unsigned size)
{
    KeywestI2CState *s = KEYWEST_I2C(opaque);
    unsigned reg = addr >> s->reg_shift;
    uint8_t val = val64;
    I2CBus *bus;

    if ((addr & ((1u << s->reg_shift) - 1)) || reg >= KW_REG_COUNT) {
        return;
    }

    trace_keywest_i2c_reg_write(reg, val);

    switch (reg) {
    case KW_REG_MODE:
        s->mode = val;
        break;

    case KW_REG_CONTROL:
        s->control = val;
        if (val & KW_CTL_XADDR) {
            keywest_i2c_start(s);
        } else if (val & KW_CTL_STOP) {
            keywest_i2c_stop(s);
        }
        break;

    case KW_REG_STATUS:
        /* low_i2c.c clears this before each transfer */
        s->status = val & (KW_STAT_SDA | KW_STAT_SCL);
        break;

    case KW_REG_ISR:
        /*
         * Write-1-to-clear, per bit.  low_i2c.c acknowledges ADDR on its
         * own while a DATA interrupt may already be pending; storing the
         * written value would drop DATA and the guest would poll forever.
         */
        s->isr &= ~(val & KW_IRQ_MASK);
        /*
         * Acking the address phase of a read is what produces the first
         * data byte -- the guest arms AAK and then acks, and expects a
         * DATA interrupt to follow.
         */
        if ((val & KW_IRQ_ADDR) && s->xfer_active && s->xfer_recv) {
            keywest_i2c_recv_byte(s);
        }
        /*
         * Acking a data byte is what asks for the next one.  AAK says
         * whether the guest wants another: with it set the byte that
         * follows is fetched, and without it this was the last one, so the
         * transfer ends here.  low_i2c.c never writes STOP for reads -- it
         * moves to state_stop and waits for the interrupt.
         */
        if ((val & KW_IRQ_DATA) && s->xfer_active) {
            if (s->xfer_recv) {
                if (s->data_ready) {
                    /*
                     * Not consumed yet -- the second acknowledge of the
                     * same interrupt must not swallow it
                     */
                    keywest_i2c_raise(s, KW_IRQ_DATA);
                } else if (s->control & KW_CTL_AAK) {
                    keywest_i2c_recv_byte(s);
                } else {
                    keywest_i2c_stop(s);
                }
            } else if (s->write_done) {
                keywest_i2c_raise(s, KW_IRQ_DATA);
            }
        }
        keywest_i2c_update_irq(s);
        break;

    case KW_REG_IER:
        s->ier = val;
        keywest_i2c_update_irq(s);
        break;

    case KW_REG_ADDR:
        s->addr = val;
        break;

    case KW_REG_SUBADDR:
        s->subaddr = val;
        break;

    case KW_REG_DATA:
        s->data = val;
        if (s->xfer_active && !s->xfer_recv) {
            bus = keywest_i2c_active_bus(s);
            s->write_done = false;
            if (bus != NULL && i2c_send(bus, val) != 0) {
                s->status &= ~KW_STAT_LAST_AAK;
                keywest_i2c_raise(s, KW_IRQ_DATA);
                /* Same as an address NAK: the guest waits for STOP */
                keywest_i2c_stop(s);
            } else {
                s->status |= KW_STAT_LAST_AAK;
                s->write_done = true;
                keywest_i2c_raise(s, KW_IRQ_DATA);
            }
        }
        break;
    }
}

static const MemoryRegionOps keywest_i2c_ops = {
    .read = keywest_i2c_read,
    .write = keywest_i2c_write,
    /* Byte registers, so the endianness only has to be stated, not chosen */
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void keywest_i2c_reset_hold(Object *obj, ResetType type)
{
    KeywestI2CState *s = KEYWEST_I2C(obj);

    s->mode = 0;
    s->control = 0;
    s->status = KW_STAT_SDA | KW_STAT_SCL;
    s->isr = 0;
    s->ier = 0;
    s->addr = 0;
    s->subaddr = 0;
    s->data = 0;
    s->xfer_active = false;
    s->xfer_recv = false;
    s->data_ready = false;
    s->write_done = false;
    keywest_i2c_update_irq(s);
}

static void keywest_i2c_realize(DeviceState *dev, Error **errp)
{
    KeywestI2CState *s = KEYWEST_I2C(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    unsigned i;

    if (s->num_channels < 1 || s->num_channels > KEYWEST_I2C_MAX_CHANNELS) {
        error_setg(errp, "keywest-i2c: num-channels must be 1 or %d",
                   KEYWEST_I2C_MAX_CHANNELS);
        return;
    }

    for (i = 0; i < s->num_channels; i++) {
        g_autofree char *name = g_strdup_printf("i2c%u", i);
        s->bus[i] = i2c_init_bus(dev, name);
    }

    memory_region_init_io(&s->mem, OBJECT(dev), &keywest_i2c_ops, s,
                          "keywest-i2c", 0x1000);
    sysbus_init_mmio(sbd, &s->mem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_keywest_i2c = {
    .name = "keywest-i2c",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(mode, KeywestI2CState),
        VMSTATE_UINT8(control, KeywestI2CState),
        VMSTATE_UINT8(status, KeywestI2CState),
        VMSTATE_UINT8(isr, KeywestI2CState),
        VMSTATE_UINT8(ier, KeywestI2CState),
        VMSTATE_UINT8(addr, KeywestI2CState),
        VMSTATE_UINT8(subaddr, KeywestI2CState),
        VMSTATE_UINT8(data, KeywestI2CState),
        VMSTATE_BOOL(xfer_active, KeywestI2CState),
        VMSTATE_BOOL(xfer_recv, KeywestI2CState),
        VMSTATE_BOOL(data_ready, KeywestI2CState),
        VMSTATE_BOOL(write_done, KeywestI2CState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property keywest_i2c_properties[] = {
    /* AAPL,address-step = 0x10 on the machines we model, hence 4 */
    DEFINE_PROP_UINT32("reg-shift", KeywestI2CState, reg_shift, 4),
    /* U3 exposes two channels; KeyLargo/K2 has one */
    DEFINE_PROP_UINT32("num-channels", KeywestI2CState, num_channels, 2),
};

static void keywest_i2c_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = keywest_i2c_realize;
    dc->vmsd = &vmstate_keywest_i2c;
    dc->desc = "Apple Keywest i2c controller";
    rc->phases.hold = keywest_i2c_reset_hold;
    device_class_set_props(dc, keywest_i2c_properties);
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

static const TypeInfo keywest_i2c_types[] = {
    {
        .name          = TYPE_KEYWEST_I2C,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(KeywestI2CState),
        .class_init    = keywest_i2c_class_init,
    },
};

DEFINE_TYPES(keywest_i2c_types)
