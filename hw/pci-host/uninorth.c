/*
 * QEMU Uninorth PCI host (for all Mac99 and newer machines)
 *
 * Copyright (c) 2006 Fabrice Bellard
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
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci-host/uninorth.h"
#include "trace.h"

static int pci_unin_map_irq(PCIDevice *pci_dev, int irq_num)
{
    return (irq_num + (pci_dev->devfn >> 3)) & 3;
}

static void pci_unin_set_irq(void *opaque, int irq_num, int level)
{
    UNINHostState *s = opaque;

    trace_unin_set_irq(irq_num, level);
    qemu_set_irq(s->irqs[irq_num], level);
}

static uint32_t unin_get_config_reg(uint32_t reg, uint32_t addr)
{
    uint32_t retval;

    if (reg & (1u << 31)) {
        /* XXX OpenBIOS compatibility hack */
        retval = reg | (addr & 3);
    } else if (reg & 1) {
        /* CFA1 style */
        retval = (reg & ~7u) | (addr & 7);
    } else {
        uint32_t slot, func;

        /* Grab CFA0 style values */
        slot = ctz32(reg & 0xfffff800);
        if (slot == 32) {
            slot = -1; /* XXX: should this be 0? */
        }
        func = PCI_FUNC(reg >> 8);

        /* ... and then convert them to x86 format */
        /* config pointer */
        retval = (reg & (0xff - 7)) | (addr & 7);
        /* slot, fn */
        retval |= PCI_DEVFN(slot, func) << 8;
    }

    trace_unin_get_config_reg(reg, addr, retval);

    return retval;
}

static void unin_data_write(void *opaque, hwaddr addr,
                            uint64_t val, unsigned len)
{
    UNINHostState *s = opaque;
    PCIHostState *phb = PCI_HOST_BRIDGE(s);
    trace_unin_data_write(addr, len, val);
    pci_data_write(phb->bus,
                   unin_get_config_reg(phb->config_reg, addr),
                   val, len);
}

static uint64_t unin_data_read(void *opaque, hwaddr addr,
                               unsigned len)
{
    UNINHostState *s = opaque;
    PCIHostState *phb = PCI_HOST_BRIDGE(s);
    uint32_t val;

    val = pci_data_read(phb->bus,
                        unin_get_config_reg(phb->config_reg, addr),
                        len);
    trace_unin_data_read(addr, len, val);
    return val;
}

static const MemoryRegionOps unin_data_ops = {
    .read = unin_data_read,
    .write = unin_data_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static char *pci_unin_main_ofw_unit_address(const SysBusDevice *dev)
{
    UNINHostState *s = UNI_NORTH_PCI_HOST_BRIDGE(dev);

    return g_strdup_printf("%x", s->ofw_addr);
}

static void pci_unin_main_realize(DeviceState *dev, Error **errp)
{
    UNINHostState *s = UNI_NORTH_PCI_HOST_BRIDGE(dev);
    PCIHostState *h = PCI_HOST_BRIDGE(dev);

    h->bus = pci_register_root_bus(dev, NULL,
                                   pci_unin_set_irq, pci_unin_map_irq,
                                   s,
                                   &s->pci_mmio,
                                   &s->pci_io,
                                   PCI_DEVFN(11, 0), 4, TYPE_PCI_BUS);

    pci_create_simple(h->bus, PCI_DEVFN(11, 0), "uni-north-pci");

    /*
     * DEC 21154 bridge was unused for many years, this comment is
     * a placeholder for whoever wishes to resurrect it
     */
}

static void pci_unin_main_init(Object *obj)
{
    UNINHostState *s = UNI_NORTH_PCI_HOST_BRIDGE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    PCIHostState *h = PCI_HOST_BRIDGE(obj);

    /* Use values found on a real PowerMac */
    /* Uninorth main bus */
    memory_region_init_io(&h->conf_mem, OBJECT(h), &pci_host_conf_le_ops,
                          obj, "unin-pci-conf-idx", 0x1000);
    memory_region_init_io(&h->data_mem, OBJECT(h), &unin_data_ops, obj,
                          "unin-pci-conf-data", 0x1000);

    memory_region_init(&s->pci_mmio, OBJECT(s), "unin-pci-mmio",
                       0x100000000ULL);
    memory_region_init_io(&s->pci_io, OBJECT(s), &unassigned_io_ops, obj,
                          "unin-pci-isa-mmio", 0x00800000);

    memory_region_init_alias(&s->pci_hole, OBJECT(s),
                             "unin-pci-hole", &s->pci_mmio,
                             0x80000000ULL, 0x10000000ULL);

    sysbus_init_mmio(sbd, &h->conf_mem);
    sysbus_init_mmio(sbd, &h->data_mem);
    sysbus_init_mmio(sbd, &s->pci_hole);
    sysbus_init_mmio(sbd, &s->pci_io);

    qdev_init_gpio_out(DEVICE(obj), s->irqs, ARRAY_SIZE(s->irqs));
}

static void pci_u3_agp_realize(DeviceState *dev, Error **errp)
{
    UNINHostState *s = U3_AGP_HOST_BRIDGE(dev);
    PCIHostState *h = PCI_HOST_BRIDGE(dev);

    h->bus = pci_register_root_bus(dev, NULL,
                                   pci_unin_set_irq, pci_unin_map_irq,
                                   s,
                                   &s->pci_mmio,
                                   &s->pci_io,
                                   PCI_DEVFN(11, 0), 4, TYPE_PCI_BUS);

    pci_create_simple(h->bus, PCI_DEVFN(11, 0), "u3-agp");
}

static void pci_u3_agp_init(Object *obj)
{
    UNINHostState *s = U3_AGP_HOST_BRIDGE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    PCIHostState *h = PCI_HOST_BRIDGE(obj);

    /* Uninorth U3 AGP bus */
    memory_region_init_io(&h->conf_mem, OBJECT(h), &pci_host_conf_le_ops,
                          obj, "unin-pci-conf-idx", 0x1000);
    memory_region_init_io(&h->data_mem, OBJECT(h), &unin_data_ops, obj,
                          "unin-pci-conf-data", 0x1000);

    memory_region_init(&s->pci_mmio, OBJECT(s), "unin-pci-mmio",
                       0x100000000ULL);
    memory_region_init_io(&s->pci_io, OBJECT(s), &unassigned_io_ops, obj,
                          "unin-pci-isa-mmio", 0x00800000);

    memory_region_init_alias(&s->pci_hole, OBJECT(s),
                             "unin-pci-hole", &s->pci_mmio,
                             0x80000000ULL, 0x70000000ULL);

    sysbus_init_mmio(sbd, &h->conf_mem);
    sysbus_init_mmio(sbd, &h->data_mem);
    sysbus_init_mmio(sbd, &s->pci_hole);
    sysbus_init_mmio(sbd, &s->pci_io);

    qdev_init_gpio_out(DEVICE(obj), s->irqs, ARRAY_SIZE(s->irqs));
}

/*
 * U3 HyperTransport host bridge (PowerMac7,3).
 *
 * Unlike the other UniNorth bridges this one uses memory-mapped config
 * space access, in the layout expected by the Linux u3-ht support code
 * (arch/powerpc/platforms/powermac/pci.c) and FreeBSD's cpcht(4):
 *
 * - external config window (32 MB), little-endian:
 *     U3_HT_CFA0(devfn, off) = (devfn << 8) | off          (type 0, bus 0)
 *     U3_HT_CFA1(bus, devfn, off) = CFA0 + (bus << 16) + 0x01000000
 * - self-register window (4 KB), big-endian: byte address (off << 2)
 *   accesses config offset 'off' of the host bridge itself (bus 0
 *   devfn 0).  The region decode register read by the kernels at
 *   cfg_addr + 0x80 (in u32 units, i.e. byte offset 0x200) is thus
 *   simply config offset 0x80 of the bridge.
 */

static void pci_u3_ht_set_irq(void *opaque, int irq_num, int level)
{
    /*
     * The real bridge delivers interrupts via HT-APIC capability
     * blocks; here INTx is routed to mpic input pins by the machine
     * (0x1f..0x22, matching the firmware's HT interrupt-map).
     */
    U3HTHostState *s = opaque;

    qemu_set_irq(s->irqs[irq_num], level);
}

static uint32_t u3_ht_get_config_reg(hwaddr addr)
{
    uint32_t bus = 0;

    if (addr & 0x01000000) {
        /* CFA1 */
        bus = (addr >> 16) & 0xff;
    }
    return (bus << 16) | (addr & 0xffff);
}

static void u3_ht_cfg_write(void *opaque, hwaddr addr,
                            uint64_t val, unsigned len)
{
    U3HTHostState *s = opaque;
    PCIHostState *phb = PCI_HOST_BRIDGE(s);

    trace_u3_ht_cfg_write(addr, len, val);
    pci_data_write(phb->bus, u3_ht_get_config_reg(addr), val, len);
}

static uint64_t u3_ht_cfg_read(void *opaque, hwaddr addr, unsigned len)
{
    U3HTHostState *s = opaque;
    PCIHostState *phb = PCI_HOST_BRIDGE(s);
    uint32_t val;

    val = pci_data_read(phb->bus, u3_ht_get_config_reg(addr), len);
    trace_u3_ht_cfg_read(addr, len, val);
    return val;
}

static const MemoryRegionOps u3_ht_cfg_ops = {
    .read = u3_ht_cfg_read,
    .write = u3_ht_cfg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void u3_ht_self_write(void *opaque, hwaddr addr,
                             uint64_t val, unsigned len)
{
    U3HTHostState *s = opaque;
    PCIHostState *phb = PCI_HOST_BRIDGE(s);

    trace_u3_ht_self_write(addr, len, val);
    pci_data_write(phb->bus, (addr >> 2) & 0xff, val, len);
}

static uint64_t u3_ht_self_read(void *opaque, hwaddr addr, unsigned len)
{
    U3HTHostState *s = opaque;
    PCIHostState *phb = PCI_HOST_BRIDGE(s);
    uint32_t val;

    val = pci_data_read(phb->bus, (addr >> 2) & 0xff, len);
    trace_u3_ht_self_read(addr, len, val);
    return val;
}

static const MemoryRegionOps u3_ht_self_ops = {
    .read = u3_ht_self_read,
    .write = u3_ht_self_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void pci_u3_ht_realize(DeviceState *dev, Error **errp)
{
    U3HTHostState *s = U3_HT_HOST_BRIDGE(dev);
    PCIHostState *h = PCI_HOST_BRIDGE(dev);

    h->bus = pci_register_root_bus(dev, NULL,
                                   pci_u3_ht_set_irq, pci_unin_map_irq,
                                   s,
                                   &s->pci_mmio,
                                   &s->pci_io,
                                   PCI_DEVFN(0, 0), 4, TYPE_PCI_BUS);

    pci_create_simple(h->bus, PCI_DEVFN(0, 0), "u3-ht");
}

static void pci_u3_ht_init(Object *obj)
{
    U3HTHostState *s = U3_HT_HOST_BRIDGE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->self_mem, obj, &u3_ht_self_ops, obj,
                          "u3-ht-self-cfg", 0x1000);
    memory_region_init_io(&s->cfg_mem, obj, &u3_ht_cfg_ops, obj,
                          "u3-ht-cfg", 0x02000000);

    memory_region_init(&s->pci_mmio, OBJECT(s), "u3-ht-pci-mmio",
                       0x100000000ULL);
    memory_region_init_io(&s->pci_io, OBJECT(s), &unassigned_io_ops, obj,
                          "u3-ht-pci-io", 0x00400000);

    /*
     * PCI memory window: identity-mapped 16 MB at 0xfa000000,
     * matching decode register bit i=10 (0xf0000000 + (i << 24)).
     */
    memory_region_init_alias(&s->pci_hole, OBJECT(s),
                             "u3-ht-pci-hole", &s->pci_mmio,
                             0xfa000000ULL, 0x01000000ULL);

    sysbus_init_mmio(sbd, &s->self_mem);
    sysbus_init_mmio(sbd, &s->cfg_mem);
    sysbus_init_mmio(sbd, &s->pci_io);
    sysbus_init_mmio(sbd, &s->pci_hole);

    qdev_init_gpio_out(DEVICE(obj), s->irqs, ARRAY_SIZE(s->irqs));
}

static void pci_unin_agp_realize(DeviceState *dev, Error **errp)
{
    UNINHostState *s = UNI_NORTH_AGP_HOST_BRIDGE(dev);
    PCIHostState *h = PCI_HOST_BRIDGE(dev);

    h->bus = pci_register_root_bus(dev, NULL,
                                   pci_unin_set_irq, pci_unin_map_irq,
                                   s,
                                   &s->pci_mmio,
                                   &s->pci_io,
                                   PCI_DEVFN(11, 0), 4, TYPE_PCI_BUS);

    pci_create_simple(h->bus, PCI_DEVFN(11, 0), "uni-north-agp");
}

static void pci_unin_agp_init(Object *obj)
{
    UNINHostState *s = UNI_NORTH_AGP_HOST_BRIDGE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    PCIHostState *h = PCI_HOST_BRIDGE(obj);

    /* Uninorth AGP bus */
    memory_region_init_io(&h->conf_mem, OBJECT(h), &pci_host_conf_le_ops,
                          obj, "unin-agp-conf-idx", 0x1000);
    memory_region_init_io(&h->data_mem, OBJECT(h), &pci_host_data_le_ops,
                          obj, "unin-agp-conf-data", 0x1000);

    sysbus_init_mmio(sbd, &h->conf_mem);
    sysbus_init_mmio(sbd, &h->data_mem);

    qdev_init_gpio_out(DEVICE(obj), s->irqs, ARRAY_SIZE(s->irqs));
}

static void pci_unin_internal_realize(DeviceState *dev, Error **errp)
{
    UNINHostState *s = UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE(dev);
    PCIHostState *h = PCI_HOST_BRIDGE(dev);

    h->bus = pci_register_root_bus(dev, NULL,
                                   pci_unin_set_irq, pci_unin_map_irq,
                                   s,
                                   &s->pci_mmio,
                                   &s->pci_io,
                                   PCI_DEVFN(14, 0), 4, TYPE_PCI_BUS);

    pci_create_simple(h->bus, PCI_DEVFN(14, 0), "uni-north-internal-pci");
}

static void pci_unin_internal_init(Object *obj)
{
    UNINHostState *s = UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    PCIHostState *h = PCI_HOST_BRIDGE(obj);

    /* Uninorth internal bus */
    memory_region_init_io(&h->conf_mem, OBJECT(h), &pci_host_conf_le_ops,
                          obj, "unin-pci-conf-idx", 0x1000);
    memory_region_init_io(&h->data_mem, OBJECT(h), &pci_host_data_le_ops,
                          obj, "unin-pci-conf-data", 0x1000);

    sysbus_init_mmio(sbd, &h->conf_mem);
    sysbus_init_mmio(sbd, &h->data_mem);

    qdev_init_gpio_out(DEVICE(obj), s->irqs, ARRAY_SIZE(s->irqs));
}

static void unin_main_pci_host_realize(PCIDevice *d, Error **errp)
{
    d->config[PCI_CACHE_LINE_SIZE] = 0x08;
    d->config[PCI_LATENCY_TIMER] = 0x10;
    d->config[PCI_CAPABILITY_LIST] = 0x00;

    /*
     * Set kMacRISCPCIAddressSelect (0x48) register to indicate PCI
     * memory space with base 0x80000000, size 0x10000000 for Apple's
     * AppleMacRiscPCI driver
     */
    d->config[0x48] = 0x0;
    d->config[0x49] = 0x0;
    d->config[0x4a] = 0x0;
    d->config[0x4b] = 0x1;
}

static void unin_agp_pci_host_realize(PCIDevice *d, Error **errp)
{
    d->config[PCI_CACHE_LINE_SIZE] = 0x08;
    d->config[PCI_LATENCY_TIMER] = 0x10;
    /* d->config[PCI_CAPABILITY_LIST] = 0x80; */
}

static void u3_agp_pci_host_realize(PCIDevice *d, Error **errp)
{
    d->config[PCI_CACHE_LINE_SIZE] = 0x08;
    d->config[PCI_LATENCY_TIMER] = 0x10;

    /*
     * Set kMacRISCPCIAddressSelect (0x48) register to indicate PCI
     * memory space with base 0x80000000, size 0x10000000 for Apple's
     * AppleMacRiscPCI driver, the same way the UniNorth main bridge
     * does.  Without it the driver has no memory range to work with.
     */
    d->config[0x48] = 0x0;
    d->config[0x49] = 0x0;
    d->config[0x4a] = 0x0;
    d->config[0x4b] = 0x1;
}

static void u3_ht_pci_host_realize(PCIDevice *d, Error **errp)
{
    d->config[PCI_CACHE_LINE_SIZE] = 0x08;
    d->config[PCI_LATENCY_TIMER] = 0x10;

    /*
     * Region decode register, read by Linux's parse_region_decode() at
     * config offset 0x80.  Bits i = 4, 8, 9 are the regions the Linux
     * kernel comment names as enabled on a real machine (bit i meaning
     * a 16 MB window at 0xf0000000 + (i << 24), i.e. 0xf4000000
     * (HT I/O), 0xf8000000 and 0xf9000000); bit i = 10 advertises the
     * PCI memory window at 0xfa000000 backing the BARs of the devices
     * on the HT bus.  Read-only.
     */
    pci_set_long(d->config + 0x80, 0x08e00000);
    pci_set_long(d->wmask + 0x80, 0);
}

static void unin_internal_pci_host_realize(PCIDevice *d, Error **errp)
{
    d->config[PCI_CACHE_LINE_SIZE] = 0x08;
    d->config[PCI_LATENCY_TIMER] = 0x10;
    d->config[PCI_CAPABILITY_LIST] = 0x00;
}

static void unin_main_pci_host_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize   = unin_main_pci_host_realize;
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->device_id = PCI_DEVICE_ID_APPLE_UNI_N_PCI;
    k->revision  = 0x00;
    k->class_id  = PCI_CLASS_BRIDGE_HOST;
    /*
     * PCI-facing part of the host bridge, not usable without the
     * host-facing part, which can't be device_add'ed, yet.
     */
    dc->user_creatable = false;
}

static const TypeInfo unin_main_pci_host_info = {
    .name = "uni-north-pci",
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIDevice),
    .class_init = unin_main_pci_host_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void u3_agp_pci_host_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize   = u3_agp_pci_host_realize;
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->device_id = PCI_DEVICE_ID_APPLE_U3_AGP;
    k->revision  = 0x00;
    k->class_id  = PCI_CLASS_BRIDGE_HOST;
    /*
     * PCI-facing part of the host bridge, not usable without the
     * host-facing part, which can't be device_add'ed, yet.
     */
    dc->user_creatable = false;
}

static const TypeInfo u3_agp_pci_host_info = {
    .name = "u3-agp",
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIDevice),
    .class_init = u3_agp_pci_host_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void u3_ht_pci_host_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize   = u3_ht_pci_host_realize;
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->device_id = PCI_DEVICE_ID_APPLE_U3_HT;
    k->revision  = 0x00;
    k->class_id  = PCI_CLASS_BRIDGE_HOST;
    /*
     * PCI-facing part of the host bridge, not usable without the
     * host-facing part, which can't be device_add'ed, yet.
     */
    dc->user_creatable = false;
}

static const TypeInfo u3_ht_pci_host_info = {
    .name = "u3-ht",
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIDevice),
    .class_init = u3_ht_pci_host_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

/*
 * K2 HT-PCI bridge (106b:0045): on a real PowerMac7,3 this is the P2P
 * bridge at HT bus 0 / dev 3 carrying the K2 KeyLargo MacIO on its
 * secondary bus.
 */
/*
 * The firmware writes the bridge windows but never sets the command
 * register, so enable memory decoding here (same reasoning as the
 * Simba bridge) and allow 32-bit I/O addresses.  Must be re-applied
 * on reset: the generic PCI device reset clears the writable COMMAND
 * bits again after realize.
 */
static void u3_ht_pci_bridge_setup(PCIDevice *dev)
{
    pci_set_word(dev->config + PCI_COMMAND, PCI_COMMAND_MEMORY);
    pci_set_word(dev->config + PCI_STATUS,
                 PCI_STATUS_66MHZ | PCI_STATUS_DEVSEL_MEDIUM);

    pci_set_word(dev->config + PCI_IO_BASE, PCI_IO_RANGE_TYPE_32);
    pci_set_word(dev->config + PCI_IO_LIMIT, PCI_IO_RANGE_TYPE_32);

    pci_bridge_update_mappings(PCI_BRIDGE(dev));
}

static void u3_ht_pci_bridge_realize(PCIDevice *dev, Error **errp)
{
    pci_bridge_initfn(dev, TYPE_PCI_BUS);

    pci_set_word(dev->wmask + PCI_IO_BASE_UPPER16, 0xffff);
    pci_set_word(dev->wmask + PCI_IO_LIMIT_UPPER16, 0xffff);

    u3_ht_pci_bridge_setup(dev);
}

static void u3_ht_pci_bridge_reset(DeviceState *qdev)
{
    pci_bridge_reset(qdev);
    u3_ht_pci_bridge_setup(PCI_DEVICE(qdev));
}

static void u3_ht_pci_bridge_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = u3_ht_pci_bridge_realize;
    k->exit = pci_bridge_exitfn;
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->device_id = 0x0045; /* K2 HT-PCI bridge */
    k->revision = 0x00;
    k->config_write = pci_bridge_write_config;
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    device_class_set_legacy_reset(dc, u3_ht_pci_bridge_reset);
    dc->vmsd = &vmstate_pci_device;
}

static const TypeInfo u3_ht_pci_bridge_info = {
    .name          = TYPE_U3_HT_PCI_BRIDGE,
    .parent        = TYPE_PCI_BRIDGE,
    .instance_size = sizeof(PCIBridge),
    .class_init    = u3_ht_pci_bridge_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void unin_agp_pci_host_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize   = unin_agp_pci_host_realize;
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->device_id = PCI_DEVICE_ID_APPLE_UNI_N_AGP;
    k->revision  = 0x00;
    k->class_id  = PCI_CLASS_BRIDGE_HOST;
    /*
     * PCI-facing part of the host bridge, not usable without the
     * host-facing part, which can't be device_add'ed, yet.
     */
    dc->user_creatable = false;
}

static const TypeInfo unin_agp_pci_host_info = {
    .name = "uni-north-agp",
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIDevice),
    .class_init = unin_agp_pci_host_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void unin_internal_pci_host_class_init(ObjectClass *klass,
                                              const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize   = unin_internal_pci_host_realize;
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->device_id = PCI_DEVICE_ID_APPLE_UNI_N_I_PCI;
    k->revision  = 0x00;
    k->class_id  = PCI_CLASS_BRIDGE_HOST;
    /*
     * PCI-facing part of the host bridge, not usable without the
     * host-facing part, which can't be device_add'ed, yet.
     */
    dc->user_creatable = false;
}

static const TypeInfo unin_internal_pci_host_info = {
    .name = "uni-north-internal-pci",
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIDevice),
    .class_init = unin_internal_pci_host_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static const Property pci_unin_main_pci_host_props[] = {
    DEFINE_PROP_UINT32("ofw-addr", UNINHostState, ofw_addr, -1),
};

static void pci_unin_main_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *sbc = SYS_BUS_DEVICE_CLASS(klass);

    dc->realize = pci_unin_main_realize;
    device_class_set_props(dc, pci_unin_main_pci_host_props);
    dc->fw_name = "pci";
    sbc->explicit_ofw_unit_address = pci_unin_main_ofw_unit_address;
}

static const TypeInfo pci_unin_main_info = {
    .name          = TYPE_UNI_NORTH_PCI_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(UNINHostState),
    .instance_init = pci_unin_main_init,
    .class_init    = pci_unin_main_class_init,
};

static void pci_u3_agp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pci_u3_agp_realize;
}

static const TypeInfo pci_u3_agp_info = {
    .name          = TYPE_U3_AGP_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(UNINHostState),
    .instance_init = pci_u3_agp_init,
    .class_init    = pci_u3_agp_class_init,
};

static void pci_u3_ht_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pci_u3_ht_realize;
}

static const TypeInfo pci_u3_ht_info = {
    .name          = TYPE_U3_HT_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(U3HTHostState),
    .instance_init = pci_u3_ht_init,
    .class_init    = pci_u3_ht_class_init,
};

static void pci_unin_agp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pci_unin_agp_realize;
}

static const TypeInfo pci_unin_agp_info = {
    .name          = TYPE_UNI_NORTH_AGP_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(UNINHostState),
    .instance_init = pci_unin_agp_init,
    .class_init    = pci_unin_agp_class_init,
};

static void pci_unin_internal_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pci_unin_internal_realize;
}

static const TypeInfo pci_unin_internal_info = {
    .name          = TYPE_UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(UNINHostState),
    .instance_init = pci_unin_internal_init,
    .class_init    = pci_unin_internal_class_init,
};

/* UniN device */
static void unin_write(void *opaque, hwaddr addr, uint64_t value,
                       unsigned size)
{
    trace_unin_write(addr, value);
}

static uint64_t unin_read(void *opaque, hwaddr addr, unsigned size)
{
    UNINState *s = opaque;
    uint32_t value;

    switch (addr) {
    case 0:
        value = s->version;
        break;
    default:
        value = 0;
    }

    trace_unin_read(addr, value);

    return value;
}

static const MemoryRegionOps unin_ops = {
    .read = unin_read,
    .write = unin_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void unin_init(Object *obj)
{
    UNINState *s = UNI_NORTH(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mem, obj, &unin_ops, s, "unin", 0x1000);

    sysbus_init_mmio(sbd, &s->mem);
}

/*
 * The version register tells the guest which memory controller generation
 * it is talking to.  Keep the UniNorth value as the default and let the
 * machine raise it to a U3 value when it builds a U3 device tree.
 */
static const Property unin_properties[] = {
    DEFINE_PROP_UINT32("version", UNINState, version, UNINORTH_VERSION_10A),
};

static void unin_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, unin_properties);
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

static const TypeInfo unin_info = {
    .name          = TYPE_UNI_NORTH,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(UNINState),
    .instance_init = unin_init,
    .class_init    = unin_class_init,
};

static void unin_register_types(void)
{
    type_register_static(&unin_main_pci_host_info);
    type_register_static(&u3_agp_pci_host_info);
    type_register_static(&u3_ht_pci_host_info);
    type_register_static(&u3_ht_pci_bridge_info);
    type_register_static(&unin_agp_pci_host_info);
    type_register_static(&unin_internal_pci_host_info);

    type_register_static(&pci_unin_main_info);
    type_register_static(&pci_u3_agp_info);
    type_register_static(&pci_u3_ht_info);
    type_register_static(&pci_unin_agp_info);
    type_register_static(&pci_unin_internal_info);

    type_register_static(&unin_info);
}

type_init(unin_register_types)
