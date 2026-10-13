/*
 * Apple U3 DART (the IOMMU between the U3 and its PCI buses)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PCI_HOST_U3_DART_H
#define HW_PCI_HOST_U3_DART_H

#include "hw/core/sysbus.h"
#include "hw/pci/pci.h"
#include "system/memory.h"

/* Inside the U3's 0xf8000000 window */
#define U3_DART_BASE        0xf8033000
#define U3_DART_SIZE        0x7000

#define TYPE_U3_DART "u3-dart"
OBJECT_DECLARE_SIMPLE_TYPE(U3DARTState, U3_DART)

struct U3DARTState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    IOMMUMemoryRegion iommu;
    AddressSpace as;
    uint32_t cntl;
    uint32_t excp;
};

/*
 * DMA from the devices on @bus (and on the buses behind its bridges) goes
 * through the DART.  Call this before realizing those devices: the mac-io
 * picks its DMA address space when it is realized.
 */
void u3_dart_attach(U3DARTState *s, PCIBus *bus);

#endif
