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
#include "migration/blocker.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "trace.h"

/*
 * U3 AGP GART, in the AGP host bridge's config space.  GART_BASE holds the
 * table's physical address with the aperture size in 4 MB units in its low
 * bits; AGP_BASE holds the aperture's 256 MB index on the bus in its top
 * nibble and bits 35:32 of the table address in its low bits.  Entries are
 * big-endian: bit 31 valid, the page number below.  Only AGP transactions
 * are translated; everything else goes straight to system memory, as there
 * is no DART behind the bridge.
 *
 * The registers only come alive when an AGP master sits on the bus (see
 * u3_agp_machine_done()); otherwise the bridge looks as it always has.
 */
#define TYPE_U3_AGP_IOMMU_MEMORY_REGION "u3-agp-iommu-memory-region"

#define U3_CFG_AGP_COMMAND      (U3_AGP_CAP_OFFSET + PCI_AGP_COMMAND)
#define U3_CFG_GART_BASE        0x8c
#define U3_CFG_AGP_BASE         0x90
#define U3_CFG_GART_CTRL        0x94
#define U3_CFG_INTERNAL_STATUS  0x98
#define U3_CFG_DUMMY_PAGE       0xa4

#define U3_GART_CTRL_INV        0x00000001
#define U3_GART_CTRL_EN         0x00000100

#define U3_INTERNAL_STATUS_IDLE 0x00000001

#define U3_GART_ENTRY_VALID     0x80000000
#define U3_GART_PAGE_SHIFT      12
#define U3_GART_PAGE_MASK       ((1ULL << U3_GART_PAGE_SHIFT) - 1)
#define U3_GART_BYTES_PER_UNIT  (4 * MiB)

/*
 * Where the AGP capability sits in the U3 bridge's config space.  Anything
 * past the Apple-specific registers at 0x48-0x4b will do; 0x80 is the offset
 * the UniNorth bridge below was going to use.
 */
#define U3_AGP_CAP_OFFSET  0x80

static IOMMUTLBEntry u3_agp_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                      IOMMUAccessFlags flag, int iommu_idx)
{
    UNINHostState *s = container_of(iommu, UNINHostState, agp_iommu);
    const uint8_t *cfg = s->agp_bridge->config;
    uint32_t gart_base = pci_get_long(cfg + U3_CFG_GART_BASE);
    uint32_t agp_base = pci_get_long(cfg + U3_CFG_AGP_BASE);
    uint32_t dummy = pci_get_long(cfg + U3_CFG_DUMMY_PAGE);
    uint64_t size = (uint64_t)(gart_base & U3_GART_PAGE_MASK) *
                    U3_GART_BYTES_PER_UNIT;
    uint64_t base = agp_base & 0xf0000000;
    uint64_t table, page;
    uint32_t entry;
    IOMMUTLBEntry ret = {
        .target_as = &address_space_memory,
        .iova = addr & ~U3_GART_PAGE_MASK,
        .translated_addr = addr & ~U3_GART_PAGE_MASK,
        .addr_mask = U3_GART_PAGE_MASK,
        .perm = IOMMU_RW,
    };

    if (!s->agp_master ||
        !(pci_get_long(cfg + U3_CFG_GART_CTRL) & U3_GART_CTRL_EN) ||
        addr < base || addr - base >= size) {
        return ret;
    }

    table = (gart_base & ~U3_GART_PAGE_MASK) |
            ((uint64_t)(agp_base & 0xf) << 32);
    page = (addr - base) >> U3_GART_PAGE_SHIFT;
    entry = ldl_be_phys(&address_space_memory, table + page * 4);
    trace_u3_agp_gart_translate(addr, entry);
    if (entry & U3_GART_ENTRY_VALID) {
        ret.translated_addr = (uint64_t)(entry & ~U3_GART_ENTRY_VALID)
                              << U3_GART_PAGE_SHIFT;
    } else if (dummy) {
        ret.translated_addr = (uint64_t)dummy << U3_GART_PAGE_SHIFT;
    } else {
        ret.perm = IOMMU_NONE;
    }
    return ret;
}

static int pci_unin_map_irq(PCIDevice *pci_dev, int irq_num)
{
    return (irq_num + (pci_dev->devfn >> 3)) & 3;
}

/* The U3 AGP slot has an interrupt line of its own (powermac7_3) */
static int pci_u3_agp_map_irq(PCIDevice *pci_dev, int irq_num)
{
    if (PCI_SLOT(pci_dev->devfn) == U3_AGP_SLOT) {
        return U3_AGP_SLOT_IRQ_LINE;
    }
    return pci_unin_map_irq(pci_dev, irq_num);
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

static void u3_agp_find_master(PCIBus *bus, PCIDevice *dev, void *opaque)
{
    PCIDevice **master = opaque;

    if (dev->devfn != PCI_DEVFN(11, 0) && !*master &&
        pci_find_capability(dev, PCI_CAP_ID_AGP)) {
        *master = dev;
    }
}

/*
 * Without an AGP master the bridge keeps the capability it always had: AGP
 * 2.0, no rate, AGP disabled, which is all a VGA adapter and a USB
 * controller need (Mac OS X only checks that the capability exists).  A
 * card that does AGP transfers gets the port it can drive: AGP 3.0, as the
 * U3 reports, with the rate bits read in AGP 2.0 mode (1x/2x/4x) when the
 * card itself is an AGP 2.0 one, and a GART the guest can program.  Cards
 * are realized before machine init is done and the AGP bus cannot be
 * hotplugged, so the answer does not change afterwards.
 */
static void u3_agp_machine_done(Notifier *notifier, void *data)
{
    UNINHostState *s = container_of(notifier, UNINHostState, machine_done);
    PCIBus *bus = PCI_HOST_BRIDGE(s)->bus;
    PCIDevice *d = s->agp_bridge;
    PCIDevice *master = NULL;
    uint8_t cap = U3_AGP_CAP_OFFSET;
    uint8_t card_cap;
    uint32_t status;

    pci_for_each_device(bus, pci_bus_num(bus), u3_agp_find_master, &master);
    if (!master) {
        return;
    }

    card_cap = pci_find_capability(master, PCI_CAP_ID_AGP);
    status = 0x1f000000 | PCI_AGP_STATUS_SBA;
    if (pci_get_long(master->config + card_cap + PCI_AGP_STATUS) & (1 << 3)) {
        /* AGP 3.0 mode: RATE1/RATE2 mean 4x/8x */
        status |= (1 << 3) | PCI_AGP_STATUS_RATE2 | PCI_AGP_STATUS_RATE1;
    } else {
        status |= PCI_AGP_STATUS_RATE4 | PCI_AGP_STATUS_RATE2 |
                  PCI_AGP_STATUS_RATE1;
    }

    d->config[cap + PCI_AGP_VERSION] = 0x30;
    pci_set_long(d->config + cap + PCI_AGP_STATUS, status);
    pci_set_long(d->wmask + cap + PCI_AGP_COMMAND,
                 0xff000000 | PCI_AGP_COMMAND_SBA | PCI_AGP_COMMAND_AGP |
                 PCI_AGP_COMMAND_FW | PCI_AGP_COMMAND_RATE4 |
                 PCI_AGP_COMMAND_RATE2 | PCI_AGP_COMMAND_RATE1);
    pci_set_long(d->config + U3_CFG_INTERNAL_STATUS, U3_INTERNAL_STATUS_IDLE);
    pci_set_long(d->wmask + U3_CFG_INTERNAL_STATUS, 0);
    s->agp_master = true;
    trace_u3_agp_master(master->devfn, status);

    /* Nothing carries the GART across a migration */
    error_setg(&s->migration_blocker,
               "U3 AGP GART state is not migratable (AGP card present)");
    if (migrate_add_blocker(&s->migration_blocker, NULL) < 0) {
        error_report("u3-agp: cannot block migration with an AGP card");
    }
}

static void pci_u3_agp_realize(DeviceState *dev, Error **errp)
{
    UNINHostState *s = U3_AGP_HOST_BRIDGE(dev);
    PCIHostState *h = PCI_HOST_BRIDGE(dev);

    h->bus = pci_register_root_bus(dev, NULL,
                                   pci_unin_set_irq,
                                   s->agp_slot_irq ? pci_u3_agp_map_irq
                                                   : pci_unin_map_irq,
                                   s,
                                   &s->pci_mmio,
                                   &s->pci_io,
                                   PCI_DEVFN(11, 0),
                                   s->agp_slot_irq ? 5 : 4, TYPE_PCI_BUS);

    /* AGP transactions; a card finds this as its host's "agp-gart" */
    memory_region_init_iommu(&s->agp_iommu, sizeof(s->agp_iommu),
                             TYPE_U3_AGP_IOMMU_MEMORY_REGION, OBJECT(s),
                             "agp-gart", UINT64_MAX);

    s->agp_bridge = pci_create_simple(h->bus, PCI_DEVFN(11, 0), "u3-agp");

    s->machine_done.notify = u3_agp_machine_done;
    qemu_add_machine_init_done_notifier(&s->machine_done);
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

    /*
     * Advertise an AGP capability.  Mac OS X's AppleMacRiscAGP::configure()
     * opens with
     *   findPCICapability(getBridgeSpace(), kIOPCIAGPCapability, ...)
     * and returns false without logging anything when the bridge has no
     * capability list, so the whole AGP domain fails to start and no nub is
     * published for the devices below it - on powermac7_3 that includes the
     * VGA adapter, leaving the guest stuck on the text console.
     *
     * Only the presence of the capability matters to that driver: a video
     * card is driven as an AGP master through its own AGP capability, which
     * the emulated VGA adapter does not have, so the bridge is never asked
     * to enable AGP transfers.  The status register therefore reads as zero
     * (no rate supported) and the command register keeps AGP disabled, which
     * is the honest description of what is emulated.
     */
    if (pci_add_capability(d, PCI_CAP_ID_AGP, U3_AGP_CAP_OFFSET,
                           PCI_AGP_SIZEOF, errp) < 0) {
        return;
    }
    /*
     * The revision field has to name some AGP revision, and 2.0 is the
     * highest one that does not describe anything beyond what is here: a
     * real U3 reports AGP 3.0 (Mac OS X publishes AAPL,agp3-mode on the
     * card it drives), but 3.0 comes with a transfer mode that is not
     * emulated at all.
     */
    d->config[U3_AGP_CAP_OFFSET + PCI_AGP_VERSION] = 0x20;
}

static UNINHostState *u3_agp_host(PCIDevice *d)
{
    return U3_AGP_HOST_BRIDGE(pci_get_bus(d)->qbus.parent);
}

static void u3_agp_pci_host_config_write(PCIDevice *d, uint32_t addr,
                                         uint32_t val, int len)
{
    UNINHostState *s = u3_agp_host(d);

    pci_default_write_config(d, addr, val, len);
    if (!s->agp_master ||
        !ranges_overlap(addr, len, U3_CFG_GART_BASE,
                        U3_CFG_DUMMY_PAGE + 4 - U3_CFG_GART_BASE)) {
        return;
    }
    trace_u3_agp_gart_cfg(pci_get_long(d->config + U3_CFG_GART_BASE),
                          pci_get_long(d->config + U3_CFG_AGP_BASE),
                          pci_get_long(d->config + U3_CFG_GART_CTRL),
                          pci_get_long(d->config + U3_CFG_DUMMY_PAGE));
    /* Nothing caches translations, so an invalidate completes at once */
    d->config[U3_CFG_GART_CTRL] &= ~U3_GART_CTRL_INV;
}

/*
 * The generic PCI reset leaves the device-specific config space alone, so
 * with an AGP card the GART would survive a reboot of the guest.  Without
 * one these offsets are plain storage and are left as they always were.
 */
static void u3_agp_pci_host_reset(DeviceState *dev)
{
    PCIDevice *d = PCI_DEVICE(dev);

    if (!u3_agp_host(d)->agp_master) {
        return;
    }
    pci_set_long(d->config + U3_CFG_AGP_COMMAND, 0);
    memset(d->config + U3_CFG_GART_BASE, 0,
           U3_CFG_INTERNAL_STATUS - U3_CFG_GART_BASE);
    pci_set_long(d->config + U3_CFG_DUMMY_PAGE, 0);
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
    k->config_write = u3_agp_pci_host_config_write;
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->device_id = PCI_DEVICE_ID_APPLE_U3_AGP;
    device_class_set_legacy_reset(dc, u3_agp_pci_host_reset);
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
static void u3_ht_pci_bridge_realize(PCIDevice *dev, Error **errp)
{
    pci_bridge_initfn(dev, TYPE_PCI_BUS);
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
    device_class_set_legacy_reset(dc, pci_bridge_reset);
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

static const Property pci_u3_agp_properties[] = {
    DEFINE_PROP_BOOL("agp-slot-irq", UNINHostState, agp_slot_irq, false),
};

static void pci_u3_agp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pci_u3_agp_realize;
    device_class_set_props(dc, pci_u3_agp_properties);
}

static void u3_agp_iommu_memory_region_class_init(ObjectClass *klass,
                                                  const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = u3_agp_translate;
}

static const TypeInfo u3_agp_iommu_memory_region_info = {
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .name = TYPE_U3_AGP_IOMMU_MEMORY_REGION,
    .class_init = u3_agp_iommu_memory_region_class_init,
};

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
    type_register_static(&u3_agp_iommu_memory_region_info);
    type_register_static(&pci_u3_ht_info);
    type_register_static(&pci_unin_agp_info);
    type_register_static(&pci_unin_internal_info);

    type_register_static(&unin_info);
}

type_init(unin_register_types)
