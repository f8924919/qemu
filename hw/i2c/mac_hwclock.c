/*
 * Apple i2c clock generator (Pulsar / Cypress), as used for MP timebase sync
 *
 * PowerMac7,2/7,3 and RackMac3,1 hang a clock generator off the UniNorth/U3
 * i2c bus and freeze the timebase through it while the second CPU is
 * brought up.  Both guests drive the same register:
 *
 *  - Linux arch/powerpc/platforms/powermac/smp.c reads register 0x2e,
 *    masks with 0x77 and writes 0x22 to freeze / 0x11 to thaw
 *  - Darwin's MacRISC4CPU::enableCPUTimeBase() does the identical
 *    read-modify-write (the constants live in _pulsarD2 inside
 *    AppleMacRISC4PE)
 *
 * The two derivations were made independently -- one from GPL source, one
 * from a disassembly -- and agree on every value including the 8-bit bus
 * address 0xd2.
 *
 * Freezing is not modelled as an effect on the timebase: every vCPU in
 * QEMU already shares one, so there is nothing to pull back into step.
 * The device therefore only has to hold the value it is given, which is
 * what both guests read back.
 *
 * Copyright (c) 2026 qemu-g5 contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TYPE_MAC_HWCLOCK "mac-hwclock"
OBJECT_DECLARE_SIMPLE_TYPE(MacHwclockState, MAC_HWCLOCK)

/*
 * The register file is sparse and undocumented; only the timebase control
 * register is ever touched.  Backing the whole 8-bit space keeps the model
 * honest about writes to registers we have not characterised, instead of
 * silently dropping them.
 */
#define MAC_HWCLOCK_NREGS 256

struct MacHwclockState {
    I2CSlave parent_obj;

    uint8_t regs[MAC_HWCLOCK_NREGS];
    uint8_t ptr;
    bool have_ptr;
};

static int mac_hwclock_event(I2CSlave *i2c, enum i2c_event event)
{
    MacHwclockState *s = MAC_HWCLOCK(i2c);

    switch (event) {
    case I2C_START_SEND:
        /* A fresh write transaction; the next byte is the register number */
        s->have_ptr = false;
        break;
    case I2C_START_RECV:
        /*
         * Reads always follow a write that set the pointer (the controller
         * issues a repeated start for both STANDARDSUB and COMBINED), so
         * the pointer is left alone here.
         */
        break;
    case I2C_FINISH:
        break;
    default:
        break;
    }
    return 0;
}

static int mac_hwclock_send(I2CSlave *i2c, uint8_t data)
{
    MacHwclockState *s = MAC_HWCLOCK(i2c);

    if (!s->have_ptr) {
        s->ptr = data;
        s->have_ptr = true;
        trace_mac_hwclock_set_ptr(data);
        return 0;
    }

    s->regs[s->ptr] = data;
    trace_mac_hwclock_write(s->ptr, data);
    s->ptr++;
    return 0;
}

static uint8_t mac_hwclock_recv(I2CSlave *i2c)
{
    MacHwclockState *s = MAC_HWCLOCK(i2c);
    uint8_t val = s->regs[s->ptr];

    trace_mac_hwclock_read(s->ptr, val);
    s->ptr++;
    return val;
}

static void mac_hwclock_reset_hold(Object *obj, ResetType type)
{
    MacHwclockState *s = MAC_HWCLOCK(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->ptr = 0;
    s->have_ptr = false;
}

static const VMStateDescription vmstate_mac_hwclock = {
    .name = "mac-hwclock",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, MacHwclockState),
        VMSTATE_UINT8_ARRAY(regs, MacHwclockState, MAC_HWCLOCK_NREGS),
        VMSTATE_UINT8(ptr, MacHwclockState),
        VMSTATE_BOOL(have_ptr, MacHwclockState),
        VMSTATE_END_OF_LIST()
    }
};

static void mac_hwclock_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->vmsd = &vmstate_mac_hwclock;
    dc->desc = "Apple i2c clock generator (timebase sync)";
    k->event = mac_hwclock_event;
    k->send = mac_hwclock_send;
    k->recv = mac_hwclock_recv;
    rc->phases.hold = mac_hwclock_reset_hold;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo mac_hwclock_types[] = {
    {
        .name          = TYPE_MAC_HWCLOCK,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(MacHwclockState),
        .class_init    = mac_hwclock_class_init,
    },
};

DEFINE_TYPES(mac_hwclock_types)
