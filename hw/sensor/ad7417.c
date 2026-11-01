/*
 * Analog Devices AD7417 temperature sensor and four-channel ADC.
 *
 * The part that the PowerMac7,2 boot ROM's device tree lists on the U3 i2c bus,
 * which Apple's AppleAD741x kext and Linux's windfarm_ad7417_sensor.c
 * drive.  The temperature is a property in 0.001 degrees C and each ADC
 * channel is a property holding the raw 10-bit conversion; none of them is
 * reset, so the machine sets them once and a guest reset leaves them
 * alone.  The alert output and the one-shot conversion are not modelled.
 *
 * Registers (8-bit pointer; the 16-bit ones are sent high byte first):
 *   0  temperature, 10 bits, 0.25 C a step, left-justified
 *   1  configuration, bits 7-5 select the ADC channel
 *   2  T_hyst        3  T_oti
 *   4  ADC, the selected channel, 10 bits left-justified
 *   5  configuration 2
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

#define TYPE_AD7417 "ad7417"
OBJECT_DECLARE_SIMPLE_TYPE(AD7417State, AD7417)

#define AD7417_REG_TEMPERATURE  0
#define AD7417_REG_CONFIG       1
#define AD7417_REG_T_HYST       2
#define AD7417_REG_T_OTI        3
#define AD7417_REG_ADC          4
#define AD7417_REG_CONFIG2      5

#define AD7417_CONFIG_CHANNEL_SHIFT 5
#define AD7417_ADC_MAX          0x3ff

/* The ADC channels 0-4 (channel 0 is the temperature input) */
#define AD7417_ADC_CHANNELS     5

struct AD7417State {
    /*< private >*/
    I2CSlave i2c;
    /*< public >*/

    /* 0.001 degrees C and the raw conversions.  Kept over a reset. */
    int32_t temperature;
    uint16_t adc[AD7417_ADC_CHANNELS];

    uint8_t pointer;
    uint8_t config;
    uint8_t config2;
    uint16_t t_hyst;
    uint16_t t_oti;

    /* What a read returns: the register as it was when the read began */
    uint8_t buf[2];
    uint8_t len;
    /* Next byte of buf to return */
    uint8_t pos;
    /* Bytes sent since the write began, the pointer included */
    uint8_t sent;
    /* High byte of a 16-bit register being written */
    uint8_t whi;
};

/* 0.25 C steps, rounded toward minus infinity like a two's complement cut */
static uint16_t ad7417_temperature_reg(int32_t milli)
{
    int32_t q = milli / 250;

    if (milli % 250 < 0) {
        q--;
    }
    q = MAX(-512, MIN(511, q));
    return (uint16_t)((q & 0x3ff) << 6);
}

static uint16_t ad7417_adc_reg(AD7417State *s)
{
    unsigned ch = s->config >> AD7417_CONFIG_CHANNEL_SHIFT;

    if (ch >= AD7417_ADC_CHANNELS) {
        return 0;
    }
    return s->adc[ch] << 6;
}

static void ad7417_get_temperature(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    AD7417State *s = AD7417(obj);
    int64_t value = s->temperature;

    visit_type_int(v, name, &value, errp);
}

/* Units are 0.001 degrees C; 10 bits at 0.25 C a step hold +-128 C */
static void ad7417_set_temperature(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    AD7417State *s = AD7417(obj);
    int64_t temp;

    if (!visit_type_int(v, name, &temp, errp)) {
        return;
    }
    if (temp >= 128000 || temp < -128000) {
        error_setg(errp, "value %" PRId64 " is out of range (+-128000)", temp);
        return;
    }

    s->temperature = temp;
}

static void ad7417_get_adc(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    AD7417State *s = AD7417(obj);
    int64_t value = s->adc[(uintptr_t)opaque];

    visit_type_int(v, name, &value, errp);
}

static void ad7417_set_adc(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    AD7417State *s = AD7417(obj);
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }
    if (value < 0 || value > AD7417_ADC_MAX) {
        error_setg(errp, "value %" PRId64 " is out of range (0-%d)", value,
                   AD7417_ADC_MAX);
        return;
    }

    s->adc[(uintptr_t)opaque] = value;
}

static void ad7417_read(AD7417State *s)
{
    uint16_t v = 0;

    s->len = 2;
    switch (s->pointer) {
    case AD7417_REG_TEMPERATURE:
        v = ad7417_temperature_reg(s->temperature);
        break;
    case AD7417_REG_CONFIG:
        s->len = 1;
        v = s->config;
        break;
    case AD7417_REG_T_HYST:
        v = s->t_hyst;
        break;
    case AD7417_REG_T_OTI:
        v = s->t_oti;
        break;
    case AD7417_REG_ADC:
        v = ad7417_adc_reg(s);
        break;
    case AD7417_REG_CONFIG2:
        s->len = 1;
        v = s->config2;
        break;
    default:
        s->len = 1;
        break;
    }

    if (s->len == 1) {
        s->buf[0] = v;
    } else {
        s->buf[0] = v >> 8;
        s->buf[1] = v;
    }
    s->pos = 0;
    trace_ad7417_read(s->i2c.address, s->pointer, v);
}

/* One data byte of a write; the pointer has been taken already */
static void ad7417_write(AD7417State *s, unsigned n, uint8_t data)
{
    trace_ad7417_write(s->i2c.address, s->pointer, data);

    switch (s->pointer) {
    case AD7417_REG_CONFIG:
        if (n == 0) {
            s->config = data;
        }
        break;
    case AD7417_REG_CONFIG2:
        if (n == 0) {
            s->config2 = data;
        }
        break;
    case AD7417_REG_T_HYST:
    case AD7417_REG_T_OTI:
        if (n == 0) {
            s->whi = data;
        } else if (n == 1) {
            uint16_t v = (s->whi << 8) | data;

            if (s->pointer == AD7417_REG_T_HYST) {
                s->t_hyst = v;
            } else {
                s->t_oti = v;
            }
        }
        break;
    default:
        /* The temperature and ADC registers and unknown ones: dropped */
        break;
    }
}

static uint8_t ad7417_rx(I2CSlave *i2c)
{
    AD7417State *s = AD7417(i2c);

    if (s->pos < s->len) {
        return s->buf[s->pos++];
    }
    return 0;
}

static int ad7417_tx(I2CSlave *i2c, uint8_t data)
{
    AD7417State *s = AD7417(i2c);

    if (s->sent == 0) {
        s->pointer = data;
    } else {
        ad7417_write(s, s->sent - 1, data);
    }
    if (s->sent < 0xff) {
        s->sent++;
    }
    return 0;
}

static int ad7417_event(I2CSlave *i2c, enum i2c_event event)
{
    AD7417State *s = AD7417(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->sent = 0;
        break;
    case I2C_START_RECV:
        ad7417_read(s);
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

static const VMStateDescription vmstate_ad7417 = {
    .name = "AD7417",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(temperature, AD7417State),
        VMSTATE_UINT16_ARRAY(adc, AD7417State, AD7417_ADC_CHANNELS),
        VMSTATE_UINT8(pointer, AD7417State),
        VMSTATE_UINT8(config, AD7417State),
        VMSTATE_UINT8(config2, AD7417State),
        VMSTATE_UINT16(t_hyst, AD7417State),
        VMSTATE_UINT16(t_oti, AD7417State),
        VMSTATE_UINT8_ARRAY(buf, AD7417State, 2),
        VMSTATE_UINT8(len, AD7417State),
        VMSTATE_UINT8(pos, AD7417State),
        VMSTATE_UINT8(sent, AD7417State),
        VMSTATE_UINT8(whi, AD7417State),
        VMSTATE_I2C_SLAVE(i2c, AD7417State),
        VMSTATE_END_OF_LIST()
    }
};

/* The temperature and the ADC values are properties: they stay */
static void ad7417_reset(DeviceState *dev)
{
    AD7417State *s = AD7417(dev);

    s->pointer = 0;
    s->config = 0;
    s->config2 = 0;
    s->t_hyst = 0;
    s->t_oti = 0;
    memset(s->buf, 0, sizeof(s->buf));
    s->len = 0;
    s->pos = 0;
    s->sent = 0;
    s->whi = 0;
}

static void ad7417_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
    unsigned i;

    k->event = ad7417_event;
    k->recv = ad7417_rx;
    k->send = ad7417_tx;
    dc->vmsd = &vmstate_ad7417;
    device_class_set_legacy_reset(dc, ad7417_reset);

    object_class_property_add(klass, "temperature", "int",
                              ad7417_get_temperature,
                              ad7417_set_temperature, NULL, NULL);
    object_class_property_set_description(klass, "temperature",
                                          "Temperature, 0.001 C");
    for (i = 0; i < AD7417_ADC_CHANNELS; i++) {
        g_autofree char *name = g_strdup_printf("adc%u", i);

        object_class_property_add(klass, name, "int", ad7417_get_adc,
                                  ad7417_set_adc, NULL,
                                  (void *)(uintptr_t)i);
        object_class_property_set_description(klass, name,
                                              "Raw 10-bit ADC value");
    }
}

static const TypeInfo ad7417_info = {
    .name          = TYPE_AD7417,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(AD7417State),
    .class_init    = ad7417_class_init,
};

static void ad7417_register_types(void)
{
    type_register_static(&ad7417_info);
}

type_init(ad7417_register_types)
