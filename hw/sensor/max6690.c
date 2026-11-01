/*
 * Maxim MAX6690 temperature sensor.
 *
 * The two-channel (internal and remote diode) sensor that the PowerMac7,2
 * boot ROM's device tree lists on the U3 i2c bus.  Only what Apple's
 * AppleMaxim6690 kext and Linux's windfarm_max6690_sensor.c use is
 * modelled: the temperature registers (an integer part and the top three
 * bits of the fraction) and the two ID registers.  Both temperatures are
 * properties in 0.001 degrees C, and they are not reset: the machine sets
 * them and a guest reset leaves them alone.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "trace.h"

#define TYPE_MAX6690 "max6690"
OBJECT_DECLARE_SIMPLE_TYPE(MAX6690State, MAX6690)

/* Register pointers (lm90.c: LM90_REG_*, MAX6657_REG_LOCAL_TEMPL) */
#define MAX6690_REG_LOCAL_TEMP      0x00
#define MAX6690_REG_REMOTE_TEMP     0x01
#define MAX6690_REG_REMOTE_TEMPL    0x10
#define MAX6690_REG_LOCAL_TEMPL     0x11
#define MAX6690_REG_MAN_ID          0xfe
#define MAX6690_REG_CHIP_ID         0xff

#define MAX6690_MAN_ID              0x4d    /* Maxim Integrated */
#define MAX6690_CHIP_ID             0x09

#define MAX6690_CHANNELS            2

struct MAX6690State {
    /*< private >*/
    I2CSlave i2c;
    /*< public >*/

    /* 0.001 degrees C: [0] internal, [1] external.  Kept over a reset. */
    int32_t temperature[MAX6690_CHANNELS];

    uint8_t pointer;
    /* What a read returns: the register as it was when the read began */
    uint8_t buf[1];
    uint8_t len;
    /* Next byte of buf to return */
    uint8_t pos;
    /* Bytes sent since the write began, the pointer included */
    uint8_t sent;
};

/* Round toward minus infinity, as a two's complement truncation does */
static int64_t max6690_eighths(int32_t milli)
{
    int64_t n = (int64_t)milli * 8;
    int64_t q = n / 1000;

    if (n % 1000 < 0) {
        q--;
    }
    return q;
}

static uint8_t max6690_reg_value(MAX6690State *s, uint8_t reg)
{
    switch (reg) {
    case MAX6690_REG_LOCAL_TEMP:
        return (uint8_t)(max6690_eighths(s->temperature[0]) >> 3);
    case MAX6690_REG_LOCAL_TEMPL:
        return (uint8_t)((max6690_eighths(s->temperature[0]) & 7) << 5);
    case MAX6690_REG_REMOTE_TEMP:
        return (uint8_t)(max6690_eighths(s->temperature[1]) >> 3);
    case MAX6690_REG_REMOTE_TEMPL:
        return (uint8_t)((max6690_eighths(s->temperature[1]) & 7) << 5);
    case MAX6690_REG_MAN_ID:
        return MAX6690_MAN_ID;
    case MAX6690_REG_CHIP_ID:
        return MAX6690_CHIP_ID;
    default:
        return 0;
    }
}

static void max6690_get_temperature(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    MAX6690State *s = MAX6690(obj);
    int64_t value = s->temperature[(uintptr_t)opaque];

    visit_type_int(v, name, &value, errp);
}

/* Units are 0.001 degrees C.  The integer register is a signed byte. */
static void max6690_set_temperature(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    MAX6690State *s = MAX6690(obj);
    int64_t temp;

    if (!visit_type_int(v, name, &temp, errp)) {
        return;
    }
    if (temp >= 128000 || temp < -128000) {
        error_setg(errp, "value %" PRId64 " is out of range (+-128000)", temp);
        return;
    }

    s->temperature[(uintptr_t)opaque] = temp;
}

static uint8_t max6690_rx(I2CSlave *i2c)
{
    MAX6690State *s = MAX6690(i2c);

    if (s->pos < s->len) {
        return s->buf[s->pos++];
    }
    return 0;
}

static int max6690_tx(I2CSlave *i2c, uint8_t data)
{
    MAX6690State *s = MAX6690(i2c);

    if (s->sent == 0) {
        /* The first byte of a write is the register pointer */
        s->pointer = data;
    } else {
        /* Nothing is writable; the byte is taken and dropped */
        trace_max6690_write(s->i2c.address, s->pointer, data);
    }
    if (s->sent < 0xff) {
        s->sent++;
    }
    return 0;
}

static int max6690_event(I2CSlave *i2c, enum i2c_event event)
{
    MAX6690State *s = MAX6690(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->sent = 0;
        break;
    case I2C_START_RECV:
        s->buf[0] = max6690_reg_value(s, s->pointer);
        s->len = 1;
        s->pos = 0;
        trace_max6690_read(s->i2c.address, s->pointer, s->buf[0]);
        break;
    case I2C_FINISH:
        s->sent = 0;
        s->pos = 0;
        break;
    default:
        break;
    }
    return 0;
}

static const VMStateDescription vmstate_max6690 = {
    .name = "MAX6690",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32_ARRAY(temperature, MAX6690State, MAX6690_CHANNELS),
        VMSTATE_UINT8(pointer, MAX6690State),
        VMSTATE_UINT8_ARRAY(buf, MAX6690State, 1),
        VMSTATE_UINT8(len, MAX6690State),
        VMSTATE_UINT8(pos, MAX6690State),
        VMSTATE_UINT8(sent, MAX6690State),
        VMSTATE_I2C_SLAVE(i2c, MAX6690State),
        VMSTATE_END_OF_LIST()
    }
};

/* The temperatures are properties, not registers: they stay */
static void max6690_reset(DeviceState *dev)
{
    MAX6690State *s = MAX6690(dev);

    s->pointer = 0;
    s->buf[0] = 0;
    s->len = 0;
    s->pos = 0;
    s->sent = 0;
}

static void max6690_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = max6690_event;
    k->recv = max6690_rx;
    k->send = max6690_tx;
    dc->vmsd = &vmstate_max6690;
    device_class_set_legacy_reset(dc, max6690_reset);

    object_class_property_add(klass, "temperature0", "int",
                              max6690_get_temperature,
                              max6690_set_temperature, NULL,
                              (void *)(uintptr_t)0);
    object_class_property_set_description(klass, "temperature0",
                                          "Internal temperature, 0.001 C");
    object_class_property_add(klass, "temperature1", "int",
                              max6690_get_temperature,
                              max6690_set_temperature, NULL,
                              (void *)(uintptr_t)1);
    object_class_property_set_description(klass, "temperature1",
                                          "External temperature, 0.001 C");
}

static const TypeInfo max6690_info = {
    .name          = TYPE_MAX6690,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(MAX6690State),
    .class_init    = max6690_class_init,
};

static void max6690_register_types(void)
{
    type_register_static(&max6690_info);
}

type_init(max6690_register_types)
