/*
 * Apple U3 DART: the IOMMU between the U3 and its PCI buses
 *
 * With the DART disabled, a bus address is a physical address.  Enabled,
 * bus page n is looked up in a table of 32-bit big-endian entries in
 * memory: bit 31 is valid, bits 23:0 the physical page (4 KiB pages, so it
 * reaches 36 bits of physical address).
 *
 * The register layout follows Linux (arch/powerpc/sysdev/dart.h) and
 * Apple's AppleU3Art.cpp.  There is no data sheet for the U3, so what
 * neither of them touches is a guess: what DART_EXCP holds, what the
 * other offsets do, and that a table size of 0 stands for 512 pages.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "hw/pci-host/u3_dart.h"
#include "trace.h"

#define DART_CNTL               0x00
#define DART_EXCP               0x10

#define DART_CNTL_BASE_MASK     0xfffff000
#define DART_CNTL_FLUSHTLB      0x400
#define DART_CNTL_ENABLE        0x200
#define DART_CNTL_SIZE_MASK     0x1ff

#define DART_ENTRY_VALID        0x80000000
#define DART_ENTRY_RPN_MASK     0x00ffffff

#define DART_PAGE_SHIFT         12
#define DART_PAGE_MASK          ((1ULL << DART_PAGE_SHIFT) - 1)
/* A table size is counted in 4 KiB pages of entries, 0 meaning 512 */
#define DART_ENTRIES_PER_PAGE   (1 << (DART_PAGE_SHIFT - 2))
#define DART_MAX_TABLE_PAGES    512

#define TYPE_U3_DART_IOMMU_MEMORY_REGION "u3-dart-iommu-memory-region"

static IOMMUTLBEntry u3_dart_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                       IOMMUAccessFlags flag, int iommu_idx)
{
    U3DARTState *s = container_of(iommu, U3DARTState, iommu);
    IOMMUTLBEntry ret = {
        .target_as = &address_space_memory,
        .iova = addr & ~DART_PAGE_MASK,
        .translated_addr = addr & ~DART_PAGE_MASK,
        .addr_mask = DART_PAGE_MASK,
        .perm = IOMMU_RW,
    };
    uint64_t page = addr >> DART_PAGE_SHIFT;
    uint64_t table_pages;
    hwaddr table;
    uint32_t entry = 0;

    if (!(s->cntl & DART_CNTL_ENABLE)) {
        return ret;
    }

    table_pages = s->cntl & DART_CNTL_SIZE_MASK;
    if (!table_pages) {
        table_pages = DART_MAX_TABLE_PAGES;
    }
    table = s->cntl & DART_CNTL_BASE_MASK;

    /* A page beyond the table has no entry, so it does not translate */
    if (page < table_pages * DART_ENTRIES_PER_PAGE) {
        entry = ldl_be_phys(&address_space_memory, table + page * 4);
    }
    if (!(entry & DART_ENTRY_VALID)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "u3-dart: no mapping for bus address 0x%" HWADDR_PRIx
                      "\n", addr);
        trace_u3_dart_fault(addr);
        s->excp = page;
        ret.perm = IOMMU_NONE;
        return ret;
    }

    ret.translated_addr = ((hwaddr)(entry & DART_ENTRY_RPN_MASK)
                           << DART_PAGE_SHIFT) | (addr & DART_PAGE_MASK);
    if (ret.translated_addr >> 32) {
        trace_u3_dart_xlate_high(addr, ret.translated_addr);
    }
    return ret;
}

/*
 * The table is read at every translation, so nothing is cached for a
 * notifier to keep up to date.  Say no to the ones that would ask for it
 * (vfio): a replay of this region would walk all of UINT64_MAX.
 */
static int u3_dart_notify_flag_changed(IOMMUMemoryRegion *iommu,
                                       IOMMUNotifierFlag old_flags,
                                       IOMMUNotifierFlag new_flags,
                                       Error **errp)
{
    if (new_flags & IOMMU_NOTIFIER_MAP) {
        error_setg(errp, "u3-dart does not support MAP notifiers");
        return -ENOTSUP;
    }
    return 0;
}

static uint64_t u3_dart_read(void *opaque, hwaddr addr, unsigned size)
{
    U3DARTState *s = opaque;

    switch (addr) {
    case DART_CNTL:
        return s->cntl;
    case DART_EXCP:
        return s->excp;
    default:
        return 0;
    }
}

static void u3_dart_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    U3DARTState *s = opaque;

    switch (addr) {
    case DART_CNTL:
        trace_u3_dart_write(addr, val);
        /* Nothing is cached, so a flush is over by the time it is written */
        s->cntl = val & ~DART_CNTL_FLUSHTLB;
        break;
    case DART_EXCP:
        trace_u3_dart_write(addr, val);
        s->excp = 0;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps u3_dart_ops = {
    .read = u3_dart_read,
    .write = u3_dart_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static AddressSpace *u3_dart_get_address_space(PCIBus *bus, void *opaque,
                                               int devfn)
{
    U3DARTState *s = opaque;

    return &s->as;
}

static const PCIIOMMUOps u3_dart_iommu_ops = {
    .get_address_space = u3_dart_get_address_space,
};

void u3_dart_attach(U3DARTState *s, PCIBus *bus)
{
    pci_setup_iommu(bus, &u3_dart_iommu_ops, s);
}

static void u3_dart_reset(DeviceState *dev)
{
    U3DARTState *s = U3_DART(dev);

    s->cntl = 0;
    s->excp = 0;
}

static void u3_dart_init(Object *obj)
{
    U3DARTState *s = U3_DART(obj);

    memory_region_init_io(&s->mem, obj, &u3_dart_ops, s, "u3-dart",
                          U3_DART_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mem);

    memory_region_init_iommu(&s->iommu, sizeof(s->iommu),
                             TYPE_U3_DART_IOMMU_MEMORY_REGION, obj,
                             "u3-dart-iommu", UINT64_MAX);
    address_space_init(&s->as, MEMORY_REGION(&s->iommu), "u3-dart");
}

static const VMStateDescription vmstate_u3_dart = {
    .name = TYPE_U3_DART,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cntl, U3DARTState),
        VMSTATE_UINT32(excp, U3DARTState),
        VMSTATE_END_OF_LIST()
    }
};

static void u3_dart_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, u3_dart_reset);
    dc->vmsd = &vmstate_u3_dart;
}

static void u3_dart_iommu_class_init(ObjectClass *oc, const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(oc);

    imrc->translate = u3_dart_translate;
    imrc->notify_flag_changed = u3_dart_notify_flag_changed;
}

static const TypeInfo u3_dart_types[] = {
    {
        .name          = TYPE_U3_DART,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(U3DARTState),
        .instance_init = u3_dart_init,
        .class_init    = u3_dart_class_init,
    }, {
        .name          = TYPE_U3_DART_IOMMU_MEMORY_REGION,
        .parent        = TYPE_IOMMU_MEMORY_REGION,
        .class_init    = u3_dart_iommu_class_init,
    },
};

DEFINE_TYPES(u3_dart_types)
