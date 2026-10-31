/*
 * QEMU PowerPC CHRP (currently NewWorld PowerMac) hardware System Emulator
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
 *
 * PCI bus layout on a real G5 (U3 based):
 *
 * 0000:f0:0b.0 Host bridge [0600]: Apple Computer Inc. U3 AGP [106b:004b]
 * 0000:f0:10.0 VGA compatible controller [0300]: ATI Technologies Inc RV350 AP [Radeon 9600] [1002:4150]
 * 0001:00:00.0 Host bridge [0600]: Apple Computer Inc. CPC945 HT Bridge [106b:004a]
 * 0001:00:01.0 PCI bridge [0604]: Advanced Micro Devices [AMD] AMD-8131 PCI-X Bridge [1022:7450] (rev 12)
 * 0001:00:02.0 PCI bridge [0604]: Advanced Micro Devices [AMD] AMD-8131 PCI-X Bridge [1022:7450] (rev 12)
 * 0001:00:03.0 PCI bridge [0604]: Apple Computer Inc. K2 HT-PCI Bridge [106b:0045]
 * 0001:00:04.0 PCI bridge [0604]: Apple Computer Inc. K2 HT-PCI Bridge [106b:0046]
 * 0001:00:05.0 PCI bridge [0604]: Apple Computer Inc. K2 HT-PCI Bridge [106b:0047]
 * 0001:00:06.0 PCI bridge [0604]: Apple Computer Inc. K2 HT-PCI Bridge [106b:0048]
 * 0001:00:07.0 PCI bridge [0604]: Apple Computer Inc. K2 HT-PCI Bridge [106b:0049]
 * 0001:01:07.0 Class [ff00]: Apple Computer Inc. K2 KeyLargo Mac/IO [106b:0041] (rev 20)
 * 0001:01:08.0 USB Controller [0c03]: Apple Computer Inc. K2 KeyLargo USB [106b:0040]
 * 0001:01:09.0 USB Controller [0c03]: Apple Computer Inc. K2 KeyLargo USB [106b:0040]
 * 0001:02:0b.0 USB Controller [0c03]: NEC Corporation USB [1033:0035] (rev 43)
 * 0001:02:0b.1 USB Controller [0c03]: NEC Corporation USB [1033:0035] (rev 43)
 * 0001:02:0b.2 USB Controller [0c03]: NEC Corporation USB 2.0 [1033:00e0] (rev 04)
 * 0001:03:0d.0 Class [ff00]: Apple Computer Inc. K2 ATA/100 [106b:0043]
 * 0001:03:0e.0 FireWire (IEEE 1394) [0c00]: Apple Computer Inc. K2 FireWire [106b:0042]
 * 0001:04:0f.0 Ethernet controller [0200]: Apple Computer Inc. K2 GMAC (Sun GEM) [106b:004c]
 * 0001:05:0c.0 IDE interface [0101]: Broadcom K2 SATA [1166:0240]
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "exec/target_page.h"
#include "hw/ppc/ppc.h"
#include "hw/core/qdev-properties.h"
#include "hw/nvram/mac_nvram.h"
#include "hw/core/boards.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci-host/uninorth.h"
#include "hw/pci-host/u3_dart.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/smbus_eeprom.h"
#include "hw/input/adb.h"
#include "hw/ppc/mac_dbdma.h"
#include "hw/pci/pci.h"
#include "net/net.h"
#include "system/system.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/char/escc.h"
#include "hw/misc/macio/macio.h"
#include "hw/core/irq.h"
#include "hw/ppc/openpic.h"
#include "hw/core/loader.h"
#include "hw/core/fw-path-provider.h"
#include "elf.h"
#include "qemu/error-report.h"
#include "system/kvm.h"
#include "system/reset.h"
#include "kvm_ppc.h"
#include "hw/usb/usb.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "hw/core/sysbus.h"
#include "trace.h"

#define MAX_IDE_BUS 2
#define CFG_ADDR 0xf0000510
#define TBFREQ (25UL * 1000UL * 1000UL)
#define CLOCKFREQ (900UL * 1000UL * 1000UL)
#define BUSFREQ (100UL * 1000UL * 1000UL)

#define NDRV_VGA_FILENAME "qemu_vga.ndrv"

#define PROM_FILENAME "openbios-ppc"
#define PROM_BASE 0xfff00000
#define PROM_SIZE (1 * MiB)

#define KERNEL_LOAD_ADDR 0x01000000
#define KERNEL_GAP       0x00100000

#define TYPE_CORE99_MACHINE MACHINE_TYPE_NAME("mac99")
typedef struct Core99MachineState Core99MachineState;
DECLARE_INSTANCE_CHECKER(Core99MachineState, CORE99_MACHINE,
                         TYPE_CORE99_MACHINE)

#define U3_MEMCTL_BASE 0xf8002000
#define U3_MEMCTL_SIZE 0x1000

typedef enum {
    CORE99_VIA_CONFIG_CUDA = 0,
    CORE99_VIA_CONFIG_PMU,
    CORE99_VIA_CONFIG_PMU_ADB
} Core99ViaConfig;


/*
 * x-ht-k2-layout: dev3 is the layout QEMU has always had.  A real boot
 * ROM takes HT dev 1 to be a K2 HT-PCI bridge unless it is an AMD-8131,
 * so dev1 moves the bridge there, and dev1-5 also fills dev 2..5 with
 * the other K2 functions (0x0046..0x0049) as bridges with nothing behind.
 */
enum {
    HT_K2_LAYOUT_DEV3,
    HT_K2_LAYOUT_DEV1,
    HT_K2_LAYOUT_DEV1_5,
};

static const char *const ht_k2_layout_names[] = {
    [HT_K2_LAYOUT_DEV3] = "dev3",
    [HT_K2_LAYOUT_DEV1] = "dev1",
    [HT_K2_LAYOUT_DEV1_5] = "dev1-5",
};

struct Core99MachineState {
    /*< private >*/
    MachineState parent;

    Core99ViaConfig via_config;
    /* PowerMac7,3 also gets the U3 HyperTransport PCI domain */
    bool has_u3_ht;
    /* PowerMac7,3: allow RAM beyond 2 GiB, placed from 4 GiB up */
    bool highmem;
    /* PowerMac7,3: whether the U3 gets a DART (x-dart) */
    OnOffAuto dart;
    /* PowerMac7,3: also show the MacIO at 0x80000000 (x-macio-80000000) */
    bool macio_low;
    MemoryRegion macio_low_alias;
    /* PowerMac7,3: HT bus memory at 0x80000000 (x-ht-mmio-80000000) */
    bool ht_mmio_low;
    MemoryRegion ht_mmio_low_alias;
    MemoryRegion agp_hole_high_alias;
    /* PowerMac7,3: U3 i2c also decodes byte 3 (x-u3-i2c-lane3) */
    bool u3_i2c_lane3;
    /* PowerMac7,3: memory SPD EEPROMs on U3 i2c channel 1 (x-u3-i2c-spd) */
    bool u3_i2c_spd;
    /* PowerMac7,3: U3 i2c drives DUMB mode transfers (x-u3-i2c-dumb) */
    bool u3_i2c_dumb;
    /* PowerMac7,3: DBDMA control writes set s0-s7 (x-dbdma-devstat) */
    bool dbdma_devstat;
    /* PowerMac7,3: the fans' TACH counts move (x-k2-fan-tach) */
    bool k2_fan_tach;
    /* PowerMac7,3: NVRAM flash answers Read Identifier (x-nvram-flash-id) */
    bool nvram_flash_id;
    /* PowerMac7,3: U3 registers at 0xf8002000 as storage (x-u3-memctl) */
    bool u3_memctl;
    MemoryRegion u3_memctl_mr;
    uint8_t u3_memctl_regs[U3_MEMCTL_SIZE];
    /* PowerMac7,3: the NMI line (GPIO 9) rests high (x-gpio9-idle-high) */
    bool gpio9_idle_high;
    /* PowerMac7,3: where the K2 HT-PCI bridge sits (x-ht-k2-layout) */
    int ht_k2_layout;
    /* PowerMac7,3: ht.1 INTx as a real boot ROM maps them (x-ht-k2-rom-irq) */
    bool ht_k2_rom_irq;
    qemu_irq ht_k2_rom_irqs[3];
    /* PowerMac7,3: leave the MacIO GPIO CPU reset lines unconnected */
    bool no_gpio_cpu_reset;
    /* PowerMac7,3: the U3 register at +0x60 reads 1 (x-u3-reg60-nonzero) */
    bool u3_reg60_nonzero;
    /* PowerMac7,3: start CPU 1 the way a real boot ROM does (x-rom-smp) */
    bool rom_smp;
    /* PowerMac7,3: the switches above default to what a real ROM wants */
    bool real_rom;
    /* Which of them were given, as BIT(RealRomSwitch) */
    uint32_t real_rom_given;
};

/*
 * The switches x-real-rom covers.  A switch that was given keeps its
 * value: the keys of one -machine option reach their setters in the
 * order of their hash, not as written, so the defaults are only filled
 * in once the machine is built.
 */
typedef enum RealRomSwitch {
    REAL_ROM_MACIO_LOW,
    REAL_ROM_HT_MMIO_LOW,
    REAL_ROM_U3_I2C_LANE3,
    REAL_ROM_U3_I2C_SPD,
    REAL_ROM_U3_I2C_DUMB,
    REAL_ROM_DBDMA_DEVSTAT,
    REAL_ROM_NVRAM_FLASH_ID,
    REAL_ROM_U3_MEMCTL,
    REAL_ROM_HT_K2_ROM_IRQ,
    REAL_ROM_NO_GPIO_CPU_RESET,
    REAL_ROM_U3_REG60_NONZERO,
    REAL_ROM_ROM_SMP,
    REAL_ROM_GPIO9_IDLE_HIGH,
    REAL_ROM_DART,
    REAL_ROM_HT_K2_LAYOUT,
} RealRomSwitch;

/* Whether x-real-rom sets this one: on, and the switch was not given */
static bool real_rom_fills(Core99MachineState *cms, RealRomSwitch sw)
{
    return cms->real_rom && !(cms->real_rom_given & BIT(sw));
}

/*
 * Outside TARGET_PPC64 with ppc_core99_init(): on the other machines
 * real_rom stays false and nothing changes.
 */
static void core99_resolve_real_rom(Core99MachineState *cms)
{
    if (real_rom_fills(cms, REAL_ROM_MACIO_LOW)) {
        cms->macio_low = true;
    }
    if (real_rom_fills(cms, REAL_ROM_HT_MMIO_LOW)) {
        cms->ht_mmio_low = true;
    }
    if (real_rom_fills(cms, REAL_ROM_U3_I2C_LANE3)) {
        cms->u3_i2c_lane3 = true;
    }
    if (real_rom_fills(cms, REAL_ROM_U3_I2C_SPD)) {
        cms->u3_i2c_spd = true;
    }
    if (real_rom_fills(cms, REAL_ROM_U3_I2C_DUMB)) {
        cms->u3_i2c_dumb = true;
    }
    if (real_rom_fills(cms, REAL_ROM_DBDMA_DEVSTAT)) {
        cms->dbdma_devstat = true;
    }
    if (real_rom_fills(cms, REAL_ROM_NVRAM_FLASH_ID)) {
        cms->nvram_flash_id = true;
    }
    if (real_rom_fills(cms, REAL_ROM_U3_MEMCTL)) {
        cms->u3_memctl = true;
    }
    if (real_rom_fills(cms, REAL_ROM_HT_K2_ROM_IRQ)) {
        cms->ht_k2_rom_irq = true;
    }
    if (real_rom_fills(cms, REAL_ROM_NO_GPIO_CPU_RESET)) {
        cms->no_gpio_cpu_reset = true;
    }
    if (real_rom_fills(cms, REAL_ROM_U3_REG60_NONZERO)) {
        cms->u3_reg60_nonzero = true;
    }
    if (real_rom_fills(cms, REAL_ROM_ROM_SMP)) {
        cms->rom_smp = true;
    }
    if (real_rom_fills(cms, REAL_ROM_GPIO9_IDLE_HIGH)) {
        cms->gpio9_idle_high = true;
    }
    if (real_rom_fills(cms, REAL_ROM_DART)) {
        cms->dart = ON_OFF_AUTO_ON;
    }
    if (real_rom_fills(cms, REAL_ROM_HT_K2_LAYOUT)) {
        cms->ht_k2_layout = HT_K2_LAYOUT_DEV1_5;
    }
}

/*
 * A real boot ROM writes U3 registers at 0xf8002000, seemingly to set
 * up the memory banks, and reads some of them back.  Nothing here
 * models what they mean: they only hold what was written.
 */
static uint64_t u3_memctl_read(void *opaque, hwaddr addr, unsigned size)
{
    Core99MachineState *cms = opaque;

    return ldn_be_p(cms->u3_memctl_regs + addr, size);
}

static void u3_memctl_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    Core99MachineState *cms = opaque;

    stn_be_p(cms->u3_memctl_regs + addr, size, val);
}

static const MemoryRegionOps u3_memctl_ops = {
    .read = u3_memctl_read,
    .write = u3_memctl_write,
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

static void u3_memctl_reset(void *opaque)
{
    Core99MachineState *cms = opaque;

    memset(cms->u3_memctl_regs, 0, sizeof(cms->u3_memctl_regs));
}

static void fw_cfg_boot_set(void *opaque, const char *boot_device,
                            Error **errp)
{
    fw_cfg_modify_i16(opaque, FW_CFG_BOOT_DEVICE, boot_device[0]);
}

static uint64_t translate_kernel_address(void *opaque, uint64_t addr)
{
    return (addr & 0x0fffffff) + KERNEL_LOAD_ADDR;
}

static void ppc_core99_reset(void *opaque)
{
    PowerPCCPU *cpu = opaque;

    cpu_reset(CPU(cpu));
    /*
     * 970 CPUs want to get their initial IP as part of their boot protocol.
     *
     * Only the CPU that runs the firmware, though: a held processor is
     * released by a soft reset and has to come out of it at the real vector,
     * where the OS has left a trampoline for it, rather than in ROM.  The
     * pic can raise OPENPIC_OUTPUT_RESET on any CPU, so make that true
     * whatever ends up unhalting it.
     */
    if (!CPU(cpu)->start_powered_off) {
        cpu->env.nip = PROM_BASE + 0x100;
    }
}

/*
 * Bring a CPU out of reset the way the KeyLargo reset GPIO does on the
 * hardware.  Linux patches the reset vector at physical 0x100 with a branch
 * to its secondary entry point, pulses the line and puts the original
 * instruction back a millisecond later, so the processor has to start
 * executing at the architected vector: cpu_reset() leaves nip there and
 * nothing must overwrite it.
 */
static void ppc_core99_cpu_reset_work(CPUState *cs, run_on_cpu_data arg)
{
    cpu_reset(cs);
    cs->halted = 0;
}

static void ppc_core99_cpu_reset_line(void *opaque, int n, int level)
{
    CPUState *cs = opaque;

    /* Active low: releasing the line is what starts the processor. */
    if (!level) {
        return;
    }

    /*
     * cpu_reset() invalidates TLBs and breakpoints, so it cannot run
     * underneath the target while it is executing.  The write that got us
     * here came from another vCPU thread holding the BQL; waiting for this
     * to finish there would stall that guest's store, so hand it over and
     * let it happen exclusively on the CPU being reset.
     */
    async_safe_run_on_cpu(cs, ppc_core99_cpu_reset_work, RUN_ON_CPU_NULL);
}

/*
 * x-rom-smp: a real boot ROM starts the second processor through mac-io
 * byte 0x72 (~sreset1) and expects it to come out in ROM, at the same
 * vector CPU 0 starts from.  Deliver it as a system reset interrupt rather
 * than cpu_reset(): the ROM's reset vector looks at HID0 to tell a wakeup
 * from its wait loop apart from a fresh start, so the registers have to
 * survive.  CPU 1 has been running the ROM since power-on, so by the time
 * the ROM probes for it, it is parked in that loop with the HID0 bit set
 * and the vector sends it on to the trampoline the ROM left in RAM.
 */
static void ppc_core99_rom_sreset_work(CPUState *cs, run_on_cpu_data arg)
{
    PowerPCCPU *cpu = POWERPC_CPU(cs);

    ppc_cpu_do_system_reset(cs);
    cpu->env.nip = PROM_BASE + 0x100;
}

static void ppc_core99_rom_sreset_line(void *opaque, int n, int level)
{
    CPUState *cs = opaque;

    /* Active low, as the KeyLargo reset GPIOs above */
    if (!level) {
        return;
    }
    async_safe_run_on_cpu(cs, ppc_core99_rom_sreset_work, RUN_ON_CPU_NULL);
}

/* PowerPC Mac99 hardware initialisation */
/*
 * x-ht-k2-rom-irq: a real boot ROM's /ht/pci@1 interrupt-map sends dev 8
 * to mpic 0x1b and dev 9 to 0x1c, looking at the slot only (its mask
 * drops the function and the pin).  ht.1 then gets an IRQ routing of its
 * own instead of the swizzle up to the HT root: line 0 and 1 for dev 8
 * and 9, line 2 for anything else, which goes nowhere.  This puts set_irq
 * on a bus that is not a root bus, so pci_device_route_intx_to_irq(),
 * which climbs to the root, does not see it; the U3 HT root has no
 * route_intx_to_irq anyway.
 */
static int ht_k2_rom_irq_map(PCIDevice *pci_dev, int irq_num)
{
    switch (PCI_SLOT(pci_dev->devfn)) {
    case 8:
        return 0;
    case 9:
        return 1;
    default:
        return 2;
    }
}

static void ht_k2_rom_irq_set(void *opaque, int irq_num, int level)
{
    Core99MachineState *cms = opaque;

    trace_ht_k2_rom_irq(irq_num, level);
    qemu_set_irq(cms->ht_k2_rom_irqs[irq_num], level);
}

static void ppc_core99_init(MachineState *machine)
{
    Core99MachineState *core99_machine = CORE99_MACHINE(machine);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    PowerPCCPU *cpu = NULL;
    CPUPPCState *env = NULL;
    char *filename;
    IrqLines *openpic_irqs;
    int i, j, k, ppc_boot_device, machine_arch, bios_size = -1;
    const char *bios_name = machine->firmware ?: PROM_FILENAME;
    MemoryRegion *bios = g_new(MemoryRegion, 1);
    hwaddr kernel_base = 0, initrd_base = 0, cmdline_base = 0;
    long kernel_size = 0, initrd_size = 0;
    PCIBus *pci_bus;
    DeviceState *u3_ht_dev = NULL;
    PCIBus *ht_bus = NULL, *k2_bus = NULL;
    DeviceState *dart_dev = NULL;
    ram_addr_t low_ram_size = machine->ram_size;
    bool has_pmu, has_adb;
    Object *macio;
    MACIOIDEState *macio_ide;
    BusState *adb_bus;
    MacIONVRAMState *nvr;
    DriveInfo *hd[MAX_IDE_BUS * MAX_IDE_DEVS];
    DriveInfo *dinfo;
    void *fw_cfg;
    SysBusDevice *s;
    DeviceState *dev, *pic_dev, *uninorth_pci_dev;
    DeviceState *uninorth_internal_dev = NULL, *uninorth_agp_dev = NULL;
    hwaddr nvram_addr = 0xFFF04000;
    uint64_t tbfreq = kvm_enabled() ? kvmppc_get_tbfreq() : TBFREQ;

    /* Before anything reads the switches x-real-rom covers */
    core99_resolve_real_rom(core99_machine);

    /* init CPUs */
    for (i = 0; i < machine->smp.cpus; i++) {
        Object *cpuobj = object_new(machine->cpu_type);

        /*
         * A PowerMac7,3 straps the 970's hypervisor facilities on, so the
         * part comes up with MSR[HV] set the way it does on the hardware.
         * Linux checks that bit before it undoes the 32-byte dcbz mode the
         * firmware leaves enabled, and dies clearing pages without it.
         */
        if (core99_machine->has_u3_ht &&
            object_property_find(cpuobj, "hv-strap")) {
            object_property_set_bool(cpuobj, "hv-strap", true, &error_fatal);
        }

        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
        cpu = POWERPC_CPU(cpuobj);
        env = &cpu->env;

        /*
         * Only the boot processor runs the firmware.  The rest wait for the
         * OS to release them through the KeyLargo reset GPIO, which is how
         * the hardware does it.
         *
         * With x-rom-smp the second processor runs the ROM from power-on
         * as well.  The ROM parks it in a wait loop, and only a processor
         * woken from that loop goes on to the trampoline the ROM probes
         * for it with; one that starts fresh on ~sreset1 parks again.
         */
        if (i != 0 && !core99_machine->rom_smp) {
            object_property_set_bool(cpuobj, "start-powered-off", true,
                                     &error_fatal);
        }

        /* Set time-base frequency to 100 Mhz */
        cpu_ppc_tb_init(env, TBFREQ);
        qemu_register_reset(ppc_core99_reset, cpu);
    }

    if (object_property_find(OBJECT(machine), "via")) {
        if (PPC_INPUT(env) == PPC_FLAGS_INPUT_970) {
            warn_report("mac99 with G5 CPU is deprecated, "
                        "use powermac7_3 instead");
        } else {
            if (core99_machine->via_config == CORE99_VIA_CONFIG_PMU) {
                warn_report("mac99,via=pmu is deprecated, "
                            "use powermac3_1 instead");
            }
            if (core99_machine->via_config == CORE99_VIA_CONFIG_PMU_ADB) {
                warn_report("mac99,via=pmu-adb is deprecated, "
                            "use powerbook3_2 instead");
            }
        }
    }
    /*
     * allocate RAM
     *
     * The PCI hole starts at 2 GiB.  On a PowerMac7,3 the RAM beyond that
     * continues at 4 GiB, as two aliases of the one RAM block.  A 32-bit
     * DMA engine cannot reach that RAM, so the machine gets a DART (see
     * x-dart below) to remap the DMA of the mac-io and of the AGP slot.
     * It is opt-in all the same, to leave the default machine as it was.
     */
    if (machine->ram_size > 2 * GiB) {
        MemoryRegion *low, *high;

        if (!core99_machine->has_u3_ht ||
            PPC_INPUT(env) != PPC_FLAGS_INPUT_970) {
            error_report("RAM size more than 2 GiB is not supported");
            exit(1);
        }
        if (!core99_machine->highmem) {
            error_report("RAM size more than 2 GiB requires highmem=on");
            exit(1);
        }
        if (machine->ram_size > 8 * GiB) {
            error_report("RAM size more than 8 GiB is not supported");
            exit(1);
        }
        low_ram_size = 2 * GiB;
        low = g_new(MemoryRegion, 1);
        high = g_new(MemoryRegion, 1);
        memory_region_init_alias(low, OBJECT(machine), "ram-low", machine->ram,
                                 0, low_ram_size);
        memory_region_init_alias(high, OBJECT(machine), "ram-high",
                                 machine->ram, low_ram_size,
                                 machine->ram_size - low_ram_size);
        memory_region_add_subregion(get_system_memory(), 0, low);
        memory_region_add_subregion(get_system_memory(), 4 * GiB, high);
    } else {
        memory_region_add_subregion(get_system_memory(), 0, machine->ram);
    }

    /* allocate and load firmware ROM */
    memory_region_init_rom(bios, NULL, "ppc_core99.bios", PROM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), PROM_BASE, bios);

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        /* Load OpenBIOS (ELF) */
        bios_size = load_elf(filename, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL,
                             ELFDATA2MSB, PPC_ELF_MACHINE, 0, 0);

        if (bios_size <= 0) {
            /* or load binary ROM image */
            bios_size = load_image_targphys(filename, PROM_BASE, PROM_SIZE,
                                            &error_fatal);
        }
        g_free(filename);
    }
    if (bios_size < 0 || bios_size > PROM_SIZE) {
        error_report("could not load PowerPC bios '%s'", bios_name);
        exit(1);
    }

    if (machine->kernel_filename) {
        kernel_base = KERNEL_LOAD_ADDR;
        kernel_size = load_elf(machine->kernel_filename, NULL,
                               translate_kernel_address, NULL, NULL, NULL,
                               NULL, NULL, ELFDATA2MSB, PPC_ELF_MACHINE, 0, 0);
        if (kernel_size < 0) {
            kernel_size = load_aout(machine->kernel_filename, kernel_base,
                                    low_ram_size - kernel_base,
                                    true, TARGET_PAGE_SIZE);
        }
        if (kernel_size < 0) {
            kernel_size = load_image_targphys(machine->kernel_filename,
                                              kernel_base,
                                              low_ram_size - kernel_base,
                                              &error_fatal);
        }
        /* load initrd */
        if (machine->initrd_filename) {
            initrd_base = TARGET_PAGE_ALIGN(kernel_base + kernel_size + KERNEL_GAP);
            initrd_size = load_image_targphys(machine->initrd_filename,
                                              initrd_base,
                                              low_ram_size - initrd_base,
                                              &error_fatal);
            cmdline_base = TARGET_PAGE_ALIGN(initrd_base + initrd_size);
        } else {
            cmdline_base = TARGET_PAGE_ALIGN(kernel_base + kernel_size + KERNEL_GAP);
        }
        ppc_boot_device = 'm';
    } else {
        ppc_boot_device = '\0';
        /* We consider that NewWorld PowerMac never have any floppy drive
         * For now, OHW cannot boot from the network.
         */
        for (i = 0; machine->boot_config.order[i] != '\0'; i++) {
            if (machine->boot_config.order[i] >= 'c' &&
                machine->boot_config.order[i] <= 'f') {
                ppc_boot_device = machine->boot_config.order[i];
                break;
            }
        }
        if (ppc_boot_device == '\0') {
            error_report("No valid boot device for Mac99 machine");
            exit(1);
        }
    }

    openpic_irqs = g_new0(IrqLines, machine->smp.cpus);
    for (i = 0; i < machine->smp.cpus; i++) {
        /*
         * Mac99 IRQ connection between OpenPIC outputs pins
         * and PowerPC input pins.
         *
         * Take the CPU for this destination rather than whichever one the
         * loop above happened to leave behind: with more than one of them
         * every destination would otherwise land on the last CPU, and the
         * first would never see an interrupt.
         */
        CPUState *cs = qemu_get_cpu(i);

        dev = DEVICE(cs);
        env = &POWERPC_CPU(cs)->env;

        switch (PPC_INPUT(env)) {
        case PPC_FLAGS_INPUT_6xx:
            openpic_irqs[i].irq[OPENPIC_OUTPUT_INT] =
                qdev_get_gpio_in(dev, PPC6xx_INPUT_INT);
            openpic_irqs[i].irq[OPENPIC_OUTPUT_CINT] =
                 qdev_get_gpio_in(dev, PPC6xx_INPUT_INT);
            openpic_irqs[i].irq[OPENPIC_OUTPUT_MCK] =
                qdev_get_gpio_in(dev, PPC6xx_INPUT_MCP);
            /* Not connected ? */
            openpic_irqs[i].irq[OPENPIC_OUTPUT_DEBUG] = NULL;
            /* Check this */
            openpic_irqs[i].irq[OPENPIC_OUTPUT_RESET] =
                qdev_get_gpio_in(dev, PPC6xx_INPUT_HRESET);
            break;
#if defined(TARGET_PPC64)
        case PPC_FLAGS_INPUT_970:
            openpic_irqs[i].irq[OPENPIC_OUTPUT_INT] =
                qdev_get_gpio_in(dev, PPC970_INPUT_INT);
            openpic_irqs[i].irq[OPENPIC_OUTPUT_CINT] =
                qdev_get_gpio_in(dev, PPC970_INPUT_INT);
            openpic_irqs[i].irq[OPENPIC_OUTPUT_MCK] =
                qdev_get_gpio_in(dev, PPC970_INPUT_MCP);
            /* Not connected ? */
            openpic_irqs[i].irq[OPENPIC_OUTPUT_DEBUG] = NULL;
            /* Check this */
            openpic_irqs[i].irq[OPENPIC_OUTPUT_RESET] =
                qdev_get_gpio_in(dev, PPC970_INPUT_HRESET);
            break;
#endif /* defined(TARGET_PPC64) */
        default:
            error_report("Bus model not supported on mac99 machine");
            exit(1);
        }
    }

    /* UniN init */
    dev = qdev_new(TYPE_UNI_NORTH);
    if (core99_machine->has_u3_ht) {
        /*
         * powermac7_3 publishes a /u3 node, so the memory controller has
         * to identify itself as a U3.  Mac OS X's AppleU3::start() bails
         * out on anything below 0x30 and, on the way out, leaves an
         * IOSimpleLock held: the config thread then panics with
         * "thread_invoke: preemption_level 1".
         */
        qdev_prop_set_uint32(dev, "version", UNINORTH_VERSION_U3);
        qdev_prop_set_bit(dev, "x-reg60-nonzero",
                          core99_machine->u3_reg60_nonzero);
        qdev_prop_set_bit(dev, "x-cpu-number", core99_machine->rom_smp);
    }
    s = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(s, &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0xf8000000,
                                sysbus_mmio_get_region(s, 0));

    if (core99_machine->has_u3_ht) {
        /*
         * The Keywest i2c controller inside U3.  Darwin needs it to freeze
         * the timebase before it will use the second CPU, and Linux uses
         * the very same clock chip for its own timebase sync, so this is
         * what makes -smp 2 useful rather than merely accepted.
         *
         * Plain map, not an overlap: nothing else decodes 0xf8001000, and
         * claiming priority here would silently hide whatever grew into
         * the region later.
         */
        DeviceState *i2c_dev = qdev_new("keywest-i2c");
        SysBusDevice *i2c_sbd = SYS_BUS_DEVICE(i2c_dev);

        qdev_prop_set_bit(i2c_dev, "x-lane3", core99_machine->u3_i2c_lane3);
        qdev_prop_set_bit(i2c_dev, "x-dumb", core99_machine->u3_i2c_dumb);
        sysbus_realize_and_unref(i2c_sbd, &error_fatal);
        sysbus_mmio_map(i2c_sbd, 0, 0xf8001000);

        /*
         * i2c-hwclock@d2 in the device tree: an 8-bit address, so the bus
         * sees 0x69.  It lives on channel 0.  The firmware describes a
         * second channel too; nothing is on it unless x-u3-i2c-spd is on.
         */
        i2c_slave_create_simple(I2C_BUS(qdev_get_child_bus(i2c_dev, "i2c0")),
                                "mac-hwclock", 0xd2 >> 1);

        /*
         * A real PowerMac7,2 Boot ROM names that node a CY28508, and for a
         * CY28508 Darwin and Linux address 8-bit 0xd0 instead, ignoring the
         * node's reg.  Without a chip there Darwin's startCPU() waits
         * forever for the timebase handshake.  Create it after the 0x69 one:
         * both share the "mac-hwclock" vmstate section and are told apart
         * by instance number in creation order.
         */
        if (core99_machine->rom_smp) {
            i2c_slave_create_simple(
                I2C_BUS(qdev_get_child_bus(i2c_dev, "i2c0")),
                "mac-hwclock", 0xd0 >> 1);
        }

        /*
         * A real boot ROM addresses channel 1 at 7-bit 0x50, 0x52, 0x54
         * and 0x56, where memory SPD EEPROMs usually sit, and with this
         * pair in place reads bytes from 0x50 and 0x51; what it does with
         * them has not been traced.  Give it one pair of DDR DIMMs, as the
         * board takes them in pairs: 0x50 and 0x51, each with half of the
         * RAM.
         */
        if (core99_machine->u3_i2c_spd) {
            I2CBus *spd_bus = I2C_BUS(qdev_get_child_bus(i2c_dev, "i2c1"));
            ram_addr_t dimm_size = machine->ram_size / 2;

            /* What spd_data_generate() takes for DDR, without asserting */
            if (!is_power_of_2(dimm_size) || dimm_size < 32 * MiB) {
                error_report("x-u3-i2c-spd needs each of the two DIMMs "
                             "(half of the RAM) to be a power of 2 of at "
                             "least 32 MiB");
                exit(1);
            }
            smbus_eeprom_init_one(spd_bus, 0x50,
                                  spd_data_generate(DDR, dimm_size));
            smbus_eeprom_init_one(spd_bus, 0x51,
                                  spd_data_generate(DDR, dimm_size));
        }

        /* Plain map, for the same reason as the i2c above */
        if (core99_machine->u3_memctl) {
            memory_region_init_io(&core99_machine->u3_memctl_mr,
                                  OBJECT(machine), &u3_memctl_ops,
                                  core99_machine, "u3-memctl",
                                  U3_MEMCTL_SIZE);
            memory_region_add_subregion(get_system_memory(), U3_MEMCTL_BASE,
                                        &core99_machine->u3_memctl_mr);
            qemu_register_reset(u3_memctl_reset, core99_machine);
        }
    }

    if (PPC_INPUT(env) == PPC_FLAGS_INPUT_970) {
        /*
         * powermac7_3 has its own machine id so that the firmware can
         * build a PowerMac7,3/MacRISC4 device tree; mac99 with a 970
         * keeps the old id.
         */
        machine_arch = core99_machine->has_u3_ht ? ARCH_POWERMAC7_3
                                                 : ARCH_MAC99_U3;
        /* 970 gets a U3 bus */
        if (core99_machine->has_u3_ht) {
            /*
             * U3 HyperTransport bus (PowerMac7,3). This must be created
             * before the AGP bridge so that the AGP bus stays the
             * default for -device (the bus of the last created host
             * bridge is found first).
             */
            s = SYS_BUS_DEVICE(qdev_new(TYPE_U3_HT_HOST_BRIDGE));
            u3_ht_dev = DEVICE(s);
            sysbus_realize_and_unref(s, &error_fatal);
            /*
             * Self registers: 0xf8000000 + U3_HT_CONFIG_BASE.  The
             * window falls inside the range covered by the mpic
             * region at 0xf8040000 (mapped later); the mpic container
             * has a hole there, but map with an explicit priority so
             * the layering survives mpic region changes.  The real
             * device tree has the same containment (/u3/mpic reg
             * covers 0x40000-0x80000).
             */
            sysbus_mmio_map_overlap(s, 0, 0xf8070000, 1);
            /*
             * External config window, at the address used by the real
             * machine (and hardcoded by the Linux HT PIC scan).  The
             * AGP ISA IO space lives at its own domain base and does
             * not contend for this address.
             */
            sysbus_mmio_map(s, 1, 0xf2000000);
            /* HT I/O space, fixed address expected by kernels */
            sysbus_mmio_map(s, 2, 0xf4000000);
            /* HT PCI memory window (16 MB, identity-mapped) */
            sysbus_mmio_map(s, 3, 0xfa000000);
        }
        /* Uninorth AGP bus */
        uninorth_pci_dev = qdev_new(TYPE_U3_AGP_HOST_BRIDGE);
        if (core99_machine->has_u3_ht) {
            /* The AGP slot's own interrupt line, mpic 0x30 (below) */
            qdev_prop_set_bit(uninorth_pci_dev, "agp-slot-irq", true);
        }
        s = SYS_BUS_DEVICE(uninorth_pci_dev);
        sysbus_realize_and_unref(s, &error_fatal);
        sysbus_mmio_map(s, 0, 0xf0800000);
        sysbus_mmio_map(s, 1, 0xf0c00000);
        if (u3_ht_dev && core99_machine->ht_mmio_low) {
            MemoryRegion *agp_mmio =
                &U3_AGP_HOST_BRIDGE(uninorth_pci_dev)->pci_mmio;
            MemoryRegion *ht_mmio = &U3_HT_HOST_BRIDGE(u3_ht_dev)->pci_mmio;

            /*
             * A real boot ROM was seen putting the BARs of the HT bus
             * devices at 0x8xxxxxxx and those of the AGP card from
             * 0x90000000 up.  Give the first 256 MB of the AGP PCI hole
             * to the HT bus, both identity-mapped, and leave the rest of
             * the hole to the AGP bus.  The decode registers are not
             * looked at.
             */
            memory_region_init_alias(&core99_machine->agp_hole_high_alias,
                                     OBJECT(machine), "unin-pci-hole-90000000",
                                     agp_mmio, 0x90000000ULL, 0x60000000ULL);
            memory_region_add_subregion(get_system_memory(), 0x90000000,
                                        &core99_machine->agp_hole_high_alias);
            memory_region_init_alias(&core99_machine->ht_mmio_low_alias,
                                     OBJECT(machine), "u3-ht-pci-mmio-80000000",
                                     ht_mmio, 0x80000000ULL, 0x10000000ULL);
            memory_region_add_subregion(get_system_memory(), 0x80000000,
                                        &core99_machine->ht_mmio_low_alias);
        } else {
            /* PCI hole */
            memory_region_add_subregion(get_system_memory(), 0x80000000,
                                        sysbus_mmio_get_region(s, 2));
        }
        /*
         * Register 8 MB of ISA IO space at the base of the AGP domain.
         * The domain is one 32 MB window: IO at +0, CONFIG_ADDR at
         * +0x800000 and CONFIG_DATA at +0xc00000, matching the layout
         * the other UniNorth domains use.  Linux hardcodes the config
         * registers at 0xf0000000 + 0x800000/0xc00000 while Mac OS X
         * derives them from the IO window base in the device tree, so
         * the window has to sit at the base the registers belong to.
         */
        memory_region_add_subregion(get_system_memory(), 0xf0000000,
                                    sysbus_mmio_get_region(s, 3));
    } else {
        machine_arch = ARCH_MAC99;
        /* Use values found on a real PowerMac */
        /* Uninorth AGP bus */
        uninorth_agp_dev = qdev_new(TYPE_UNI_NORTH_AGP_HOST_BRIDGE);
        s = SYS_BUS_DEVICE(uninorth_agp_dev);
        sysbus_realize_and_unref(s, &error_fatal);
        sysbus_mmio_map(s, 0, 0xf0800000);
        sysbus_mmio_map(s, 1, 0xf0c00000);

        /* Uninorth internal bus */
        uninorth_internal_dev = qdev_new(
                                TYPE_UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE);
        s = SYS_BUS_DEVICE(uninorth_internal_dev);
        sysbus_realize_and_unref(s, &error_fatal);
        sysbus_mmio_map(s, 0, 0xf4800000);
        sysbus_mmio_map(s, 1, 0xf4c00000);

        /* Uninorth main bus - this must be last to make it the default */
        uninorth_pci_dev = qdev_new(TYPE_UNI_NORTH_PCI_HOST_BRIDGE);
        qdev_prop_set_uint32(uninorth_pci_dev, "ofw-addr", 0xf2000000);
        s = SYS_BUS_DEVICE(uninorth_pci_dev);
        sysbus_realize_and_unref(s, &error_fatal);
        sysbus_mmio_map(s, 0, 0xf2800000);
        sysbus_mmio_map(s, 1, 0xf2c00000);
        /* PCI hole */
        memory_region_add_subregion(get_system_memory(), 0x80000000,
                                    sysbus_mmio_get_region(s, 2));
        /* Register 8 MB of ISA IO space */
        memory_region_add_subregion(get_system_memory(), 0xf2000000,
                                    sysbus_mmio_get_region(s, 3));
    }

    machine->usb |= defaults_enabled() && !machine->usb_disabled;
    has_pmu = (core99_machine->via_config != CORE99_VIA_CONFIG_CUDA);
    has_adb = (core99_machine->via_config == CORE99_VIA_CONFIG_CUDA ||
               core99_machine->via_config == CORE99_VIA_CONFIG_PMU_ADB);

    /* init basic PC hardware */
    pci_bus = PCI_HOST_BRIDGE(uninorth_pci_dev)->bus;

    if (u3_ht_dev) {
        PCIDevice *br;

        ht_bus = PCI_HOST_BRIDGE(u3_ht_dev)->bus;

        /*
         * The DART sits in front of the HT bus and the AGP bus.  It has
         * to be attached before the K2 bridge and the mac-io below are
         * realized: the mac-io picks its DMA address space when it is.
         */
        if (core99_machine->dart == ON_OFF_AUTO_ON ||
            (core99_machine->dart == ON_OFF_AUTO_AUTO &&
             machine->ram_size > 2 * GiB)) {
            dart_dev = qdev_new(TYPE_U3_DART);
            s = SYS_BUS_DEVICE(dart_dev);
            sysbus_realize_and_unref(s, &error_fatal);
            sysbus_mmio_map(s, 0, U3_DART_BASE);
            u3_dart_attach(U3_DART(dart_dev), ht_bus);
            u3_dart_attach(U3_DART(dart_dev), pci_bus);
            /*
             * AGP accesses that the GART leaves alone are bus addresses
             * like any other, so they go through the DART as well.
             */
            u3_agp_set_gart_exit_as(U3_AGP_HOST_BRIDGE(uninorth_pci_dev),
                                    &U3_DART(dart_dev)->as);
        }

        /*
         * K2 HT-PCI bridge at dev 3, carrying the MacIO on its
         * secondary bus at dev 7 (both devfns match the real 7,3:
         * 0001:00:03.0 / 0001:01:07.0).  x-ht-k2-layout moves it to
         * dev 1 for a real boot ROM; the secondary bus stays ht.1.
         */
        br = pci_new(PCI_DEVFN(core99_machine->ht_k2_layout ==
                               HT_K2_LAYOUT_DEV3 ? 3 : 1, 0),
                     TYPE_U3_HT_PCI_BRIDGE);
        pci_bridge_map_irq(PCI_BRIDGE(br), "ht.1",
                           core99_machine->ht_k2_rom_irq ?
                           ht_k2_rom_irq_map : NULL);
        pci_realize_and_unref(br, ht_bus, &error_fatal);
        k2_bus = pci_bridge_get_sec_bus(PCI_BRIDGE(br));

        if (core99_machine->ht_k2_layout == HT_K2_LAYOUT_DEV1_5) {
            static const char *const names[] = {
                "ht.2", "ht.3", "ht.4", "ht.5",
            };

            for (i = 0; i < ARRAY_SIZE(names); i++) {
                br = pci_new(PCI_DEVFN(2 + i, 0), TYPE_U3_HT_PCI_BRIDGE);
                qdev_prop_set_uint16(DEVICE(br), "x-device-id",
                                     U3_HT_PCI_BRIDGE_DEVICE_ID + 1 + i);
                pci_bridge_map_irq(PCI_BRIDGE(br), names[i], NULL);
                pci_realize_and_unref(br, ht_bus, &error_fatal);
            }
        }
    }

    /* MacIO */
    macio = OBJECT(pci_new(k2_bus ? PCI_DEVFN(7, 0) : -1,
                           u3_ht_dev ? TYPE_K2_MACIO : TYPE_NEWWORLD_MACIO));
    dev = DEVICE(macio);
    qdev_prop_set_uint64(dev, "frequency", tbfreq);
    qdev_prop_set_bit(dev, "has-pmu", has_pmu);
    qdev_prop_set_bit(dev, "has-adb", has_adb);
    qdev_prop_set_uint32(dev, "nb_cpus", machine->smp.cpus);
    if (u3_ht_dev) {
        /*
         * The mpic's registers live on the sysbus inside the U3 rather
         * than inside the MacIO BAR; the machine maps them itself below
         * and mirrors them back into the BAR, because the device tree
         * describes the same registers from both places.
         */
        qdev_prop_set_bit(dev, "pic-in-bar", false);
        /* The K2 carries the I2S-a sound cell; -M powermac7_3,audiodev= */
        if (machine->audiodev) {
            qdev_prop_set_string(dev, "audiodev", machine->audiodev);
        }
        /*
         * Only when on, as with x-dbdma-devstat, and only here: the K2
         * and its i2s child exist with a 970 CPU only.
         */
        if (core99_machine->k2_fan_tach) {
            DeviceState *i2s = DEVICE(object_resolve_path_component(
                                      OBJECT(dev), "i2s"));

            qdev_prop_set_bit(i2s, "x-fan-tach", true);
        }
    }

    if (core99_machine->gpio9_idle_high) {
        macio_newworld_set_gpio9_idle_high(NEWWORLD_MACIO(macio), true);
    }

    /*
     * Only when on: left alone, the DBDMA keeps whatever -global set, so
     * the switch can be tried on the other machines too.
     */
    if (core99_machine->dbdma_devstat) {
        dev = DEVICE(object_resolve_path_component(macio, "dbdma"));
        qdev_prop_set_bit(dev, "x-devstat", true);
    }

    dev = DEVICE(object_resolve_path_component(macio, "escc"));
    qdev_prop_set_chr(dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(dev, "chrB", serial_hd(1));

    pci_realize_and_unref(PCI_DEVICE(macio), k2_bus ? k2_bus : pci_bus,
                          &error_fatal);

    if (core99_machine->macio_low) {
        MemoryRegion *bar = &MACIO(macio)->bar;

        /*
         * A real boot ROM drives the MacIO at 0x80000000 long before it
         * assigns any BAR.  Show the BAR contents there too, above the
         * AGP PCI hole, leaving the PCI side alone; only where the BAR
         * has a device is the hole covered.
         */
        memory_region_init_alias(&core99_machine->macio_low_alias,
                                 OBJECT(machine), "macio-80000000", bar, 0,
                                 memory_region_size(bar));
        memory_region_add_subregion_overlap(get_system_memory(), 0x80000000,
                                            &core99_machine->macio_low_alias,
                                            1);
    }

    /*
     * Hook the KeyLargo CPU reset lines up to the processors they name.
     * MacIO only grows them when it has a PMU, which is what carries the
     * GPIO controller.  These are Linux's KL_GPIO_RESET_CPUn, which the
     * soft-reset property OpenBIOS writes points at.  A real PowerMac G5
     * boot ROM names GPIO 3 and 4 codec-input-data-mux and
     * line-input-detect and Mac OS X drives them as such, so with
     * x-no-gpio-cpu-reset they stay plain registers.
     */
    if (has_pmu && !core99_machine->no_gpio_cpu_reset) {
        CPUState *cs;

        CPU_FOREACH(cs) {
            if (cs->cpu_index >= MACIO_GPIO_RESET_CPU_CNT) {
                break;
            }
            qdev_connect_gpio_out_named(DEVICE(macio), "cpu-reset",
                                        cs->cpu_index,
                                        qemu_allocate_irq(
                                            ppc_core99_cpu_reset_line, cs, 0));
        }
    }

    /*
     * x-rom-smp: only ~sreset1, for CPU 1.  ~sreset0 would reset CPU 0,
     * which the boot ROM never needs and x-no-gpio-cpu-reset is there to
     * avoid.
     */
    if (has_pmu && core99_machine->rom_smp) {
        CPUState *cs1 = qemu_get_cpu(1);

        if (cs1) {
            qdev_connect_gpio_out_named(DEVICE(macio), "sreset", 1,
                                        qemu_allocate_irq(
                                            ppc_core99_rom_sreset_line,
                                            cs1, 0));
        }
    }

    pic_dev = DEVICE(object_resolve_path_component(macio, "pic"));
    if (u3_ht_dev) {
        /*
         * mpic inside the U3 at 0xf8040000 (see the self window map).
         * This is only where the registers are; the node the OS drives
         * is the one below mac-io, which reaches them through the alias
         * MacIO puts in its BAR.
         */
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(pic_dev), 0, 0xf8040000, 0);
    }
    /*
     * With x-ht-k2-rom-irq the AGP INTx stay unconnected: ht.1 dev 8 and
     * 9 take 0x1b and 0x1c, and the boot ROM's AGP interrupt-map has the
     * slot only.  The flag is never set on mac99.
     */
    for (i = 0; !core99_machine->ht_k2_rom_irq && i < 4; i++) {
        qdev_connect_gpio_out(uninorth_pci_dev, i,
                              qdev_get_gpio_in(pic_dev, 0x1b + i));
    }
    if (core99_machine->ht_k2_rom_irq && k2_bus) {
        core99_machine->ht_k2_rom_irqs[0] = qdev_get_gpio_in(pic_dev, 0x1b);
        core99_machine->ht_k2_rom_irqs[1] = qdev_get_gpio_in(pic_dev, 0x1c);
        pci_bus_irqs(k2_bus, ht_k2_rom_irq_set, core99_machine,
                     ARRAY_SIZE(core99_machine->ht_k2_rom_irqs));
    }
    if (core99_machine->has_u3_ht) {
        /* Must match the AGP slot entry of the firmware's interrupt-map */
        qdev_connect_gpio_out(uninorth_pci_dev, U3_AGP_SLOT_IRQ_LINE,
                              qdev_get_gpio_in(pic_dev, U3_AGP_SLOT_IRQ));
    }
    if (u3_ht_dev) {
        /*
         * HT INTx to mpic pins 0x1f..0x22; must match the firmware's
         * HT interrupt-map (OpenBIOS u3_ht_arch.irqs[]).
         */
        for (i = 0; i < 4; i++) {
            qdev_connect_gpio_out(u3_ht_dev, i,
                                  qdev_get_gpio_in(pic_dev, 0x1f + i));
        }
    }

    /* TODO: additional PCI buses only wired up for 32-bit machines */
    if (PPC_INPUT(env) != PPC_FLAGS_INPUT_970) {
        /* Uninorth AGP bus */
        for (i = 0; i < 4; i++) {
            qdev_connect_gpio_out(uninorth_agp_dev, i,
                                  qdev_get_gpio_in(pic_dev, 0x1b + i));
        }

        /* Uninorth internal bus */
        for (i = 0; i < 4; i++) {
            qdev_connect_gpio_out(uninorth_internal_dev, i,
                                  qdev_get_gpio_in(pic_dev, 0x1b + i));
        }
    }

    /* OpenPIC */
    s = SYS_BUS_DEVICE(pic_dev);
    k = 0;
    for (i = 0; i < machine->smp.cpus; i++) {
        for (j = 0; j < OPENPIC_OUTPUT_NB; j++) {
            sysbus_connect_irq(s, k++, openpic_irqs[i].irq[j]);
        }
    }
    g_free(openpic_irqs);

    /* We only emulate 2 out of 3 IDE controllers for now */
    ide_drive_get(hd, ARRAY_SIZE(hd));

    macio_ide = MACIO_IDE(object_resolve_path_component(macio, "ide[0]"));
    macio_ide_init_drives(macio_ide, hd);

    macio_ide = MACIO_IDE(object_resolve_path_component(macio, "ide[1]"));
    macio_ide_init_drives(macio_ide, &hd[MAX_IDE_DEVS]);

    if (has_adb) {
        if (has_pmu) {
            dev = DEVICE(object_resolve_path_component(macio, "pmu"));
        } else {
            dev = DEVICE(object_resolve_path_component(macio, "cuda"));
        }

        adb_bus = qdev_get_child_bus(dev, "adb.0");
        dev = qdev_new(TYPE_ADB_KEYBOARD);
        qdev_realize_and_unref(dev, adb_bus, &error_fatal);

        dev = qdev_new(TYPE_ADB_MOUSE);
        qdev_realize_and_unref(dev, adb_bus, &error_fatal);
    }

    if (machine->usb) {
        pci_create_simple(pci_bus, -1, "pci-ohci");

        /* U3 needs to use USB for input because Linux doesn't support via-cuda
        on PPC64 */
        if (!has_adb || machine_arch == ARCH_MAC99_U3) {
            USBBus *usb_bus;

            usb_bus = USB_BUS(object_resolve_type_unambiguous(TYPE_USB_BUS,
                                                              &error_abort));
            usb_create_simple(usb_bus, "usb-kbd");
            usb_create_simple(usb_bus, "usb-mouse");
        }
    }

    pci_vga_init(pci_bus);

    if (!graphic_width) {
        graphic_width = 800;
    }
    if (!graphic_height) {
        graphic_height = 600;
    }
    if (!graphic_depth) {
        graphic_depth = 32;
    }
    if (graphic_depth != 15 && graphic_depth != 32 && graphic_depth != 8) {
        graphic_depth = 15;
    }

    pci_init_nic_devices(ht_bus ? ht_bus : pci_bus, mc->default_nic);

    /* The NewWorld NVRAM is not located in the MacIO device */
    if (kvm_enabled() && qemu_real_host_page_size() > 4096) {
        /* We can't combine read-write and read-only in a single page, so
           move the NVRAM out of ROM again for KVM */
        nvram_addr = 0xFFE00000;
    }
    dev = qdev_new(TYPE_MACIO_NVRAM);
    qdev_prop_set_uint32(dev, "size", CORE99_NVRAM_SIZE);
    /*
     * The part is a flash chip, and the guests unlock it a bank at a time
     * with the Sharp/Micron command set before they program it.
     */
    qdev_prop_set_uint32(dev, "block-size", CORE99_NVRAM_BANK_SIZE);
    qdev_prop_set_uint32(dev, "it_shift", 0);
    /* Only when on, so that -global can turn it on on mac99 as well */
    if (core99_machine->nvram_flash_id) {
        qdev_prop_set_bit(dev, "x-flash-id", true);
    }
    /* Same interface the OldWorld machine takes its NVRAM image on */
    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive(dev, "drive", blk_by_legacy_dinfo(dinfo));
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, nvram_addr);
    nvr = MACIO_NVRAM(dev);
    pmac_format_nvram_partition_core99(nvr, CORE99_NVRAM_SIZE);
    /* No PCI init: the BIOS will do it */

    dev = qdev_new(TYPE_FW_CFG_MEM);
    fw_cfg = FW_CFG(dev);
    qdev_prop_set_uint32(dev, "data_width", 1);
    qdev_prop_set_bit(dev, "dma_enabled", false);
    object_property_add_child(OBJECT(machine), TYPE_FW_CFG, OBJECT(fw_cfg));
    s = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(s, &error_fatal);
    /*
     * fw_cfg sits at ISA port 0x510 of the AGP domain, so on the 970
     * machines it lands inside the ISA IO window registered above.  Map
     * both regions with priority so they keep answering instead of being
     * shadowed by the window; OpenBIOS reads the machine id from here.
     */
    sysbus_mmio_map_overlap(s, 0, CFG_ADDR, 1);
    sysbus_mmio_map_overlap(s, 1, CFG_ADDR + 2, 1);

    fw_cfg_add_i16(fw_cfg, FW_CFG_NB_CPUS, (uint16_t)machine->smp.cpus);
    fw_cfg_add_i16(fw_cfg, FW_CFG_MAX_CPUS, (uint16_t)machine->smp.max_cpus);
    /*
     * Only the RAM below the PCI hole: the firmware lays itself out from
     * the top of it.  powermac7_3 passes the rest separately.
     */
    fw_cfg_add_i64(fw_cfg, FW_CFG_RAM_SIZE, (uint64_t)low_ram_size);
    if (core99_machine->has_u3_ht) {
        fw_cfg_add_i64(fw_cfg, FW_CFG_PPC_HIGH_RAM_SIZE,
                       (uint64_t)(machine->ram_size - low_ram_size));
        /* The firmware describes the DART to the guest only if there is one */
        fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_DART, dart_dev ? 1 : 0);
    }
    fw_cfg_add_i16(fw_cfg, FW_CFG_MACHINE_ID, machine_arch);
    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_ADDR, kernel_base);
    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_SIZE, kernel_size);
    if (machine->kernel_cmdline) {
        fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_CMDLINE, cmdline_base);
        pstrcpy_targphys("cmdline", cmdline_base, TARGET_PAGE_SIZE,
                         machine->kernel_cmdline);
    } else {
        fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_CMDLINE, 0);
    }
    fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_ADDR, initrd_base);
    fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_SIZE, initrd_size);
    fw_cfg_add_i16(fw_cfg, FW_CFG_BOOT_DEVICE, ppc_boot_device);

    fw_cfg_add_i16(fw_cfg, FW_CFG_PPC_WIDTH, graphic_width);
    fw_cfg_add_i16(fw_cfg, FW_CFG_PPC_HEIGHT, graphic_height);
    fw_cfg_add_i16(fw_cfg, FW_CFG_PPC_DEPTH, graphic_depth);

    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_VIACONFIG, core99_machine->via_config);

    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_IS_KVM, kvm_enabled());
    if (kvm_enabled()) {
        uint8_t *hypercall;

        hypercall = g_malloc(16);
        kvmppc_get_hypercall(env, hypercall, 16);
        fw_cfg_add_bytes(fw_cfg, FW_CFG_PPC_KVM_HC, hypercall, 16);
        fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_KVM_PID, getpid());
    }
    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_TBFREQ, tbfreq);
    /* Mac OS X requires a "known good" clock-frequency value; pass it one. */
    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_CLOCKFREQ, CLOCKFREQ);
    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_BUSFREQ, BUSFREQ);
    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_NVRAM_ADDR, nvram_addr);
    /*
     * Tell OpenBIOS that the NVRAM bytes are contiguous.  Without this it
     * assumes they are spaced out one every two addresses, halves the size
     * it advertises in the device tree, and reads only every other byte.
     */
    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_NVRAM_FLAT, 1);

    /* MacOS NDRV VGA driver */
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, NDRV_VGA_FILENAME);
    if (filename) {
        gchar *ndrv_file;
        gsize ndrv_size;

        if (g_file_get_contents(filename, &ndrv_file, &ndrv_size, NULL)) {
            fw_cfg_add_file(fw_cfg, "ndrv/qemu_vga.ndrv", ndrv_file, ndrv_size);
        }
        g_free(filename);
    }

    qemu_register_boot_set(fw_cfg_boot_set, fw_cfg);
}

/*
 * Implementation of an interface to adjust firmware path
 * for the bootindex property handling.
 */
static char *core99_fw_dev_path(FWPathProvider *p, BusState *bus,
                                DeviceState *dev)
{
    PCIDevice *pci;
    MACIOIDEState *macio_ide;

    /*
     * Match the type rather than its name: the PowerMac7,3 plugs in a
     * macio-k2, which is a macio-newworld with a different PCI id, and a
     * name comparison would walk straight past it and hand out a device
     * path built from the type name.
     */
    if (object_dynamic_cast(OBJECT(dev), TYPE_NEWWORLD_MACIO)) {
        pci = PCI_DEVICE(dev);
        return g_strdup_printf("mac-io@%x", PCI_SLOT(pci->devfn));
    }

    if (!strcmp(object_get_typename(OBJECT(dev)), "macio-ide")) {
        macio_ide = MACIO_IDE(dev);
        return g_strdup_printf("ata-3@%x", macio_ide->addr);
    }

    if (!strcmp(object_get_typename(OBJECT(dev)), "ide-hd")) {
        return g_strdup("disk");
    }

    if (!strcmp(object_get_typename(OBJECT(dev)), "ide-cd")) {
        return g_strdup("cdrom");
    }

    if (!strcmp(object_get_typename(OBJECT(dev)), "virtio-blk-device")) {
        return g_strdup("disk");
    }

    return NULL;
}
static int core99_kvm_type(MachineState *machine, const char *arg)
{
    /* Always force PR KVM */
    return 2;
}

static void core99_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    FWPathProviderClass *fwc = FW_PATH_PROVIDER_CLASS(oc);

    mc->desc = "Mac99 based PowerMac";
    mc->init = ppc_core99_init;
    mc->block_default_type = IF_IDE;
    /* SMP is not supported currently */
    mc->max_cpus = 1;
    mc->default_boot_order = "cd";
    mc->default_display = "std";
    mc->default_nic = "sungem";
    mc->kvm_type = core99_kvm_type;
#ifdef TARGET_PPC64
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("970fx_v3.1");
#else
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("7400_v2.9");
#endif
    mc->default_ram_id = "ppc_core99.ram";
    mc->ignore_boot_device_suffixes = true;
    fwc->get_dev_path = core99_fw_dev_path;
}

static char *core99_get_via_config(Object *obj, Error **errp)
{
    Core99MachineState *cms = CORE99_MACHINE(obj);

    switch (cms->via_config) {
    default:
    case CORE99_VIA_CONFIG_CUDA:
        return g_strdup("cuda");

    case CORE99_VIA_CONFIG_PMU:
        return g_strdup("pmu");

    case CORE99_VIA_CONFIG_PMU_ADB:
        return g_strdup("pmu-adb");
    }
}

static void core99_set_via_config(Object *obj, const char *value, Error **errp)
{
    Core99MachineState *cms = CORE99_MACHINE(obj);

    if (!strcmp(value, "cuda")) {
        cms->via_config = CORE99_VIA_CONFIG_CUDA;
    } else if (!strcmp(value, "pmu")) {
        cms->via_config = CORE99_VIA_CONFIG_PMU;
    } else if (!strcmp(value, "pmu-adb")) {
        cms->via_config = CORE99_VIA_CONFIG_PMU_ADB;
    } else {
        error_setg(errp, "Invalid via value");
        error_append_hint(errp, "Valid values are cuda, pmu, pmu-adb.\n");
    }
}

static void core99_instance_init(Object *obj)
{
    Core99MachineState *cms = CORE99_MACHINE(obj);

    /* Default via_config is CORE99_VIA_CONFIG_CUDA */
    cms->via_config = CORE99_VIA_CONFIG_CUDA;
    object_property_add_str(obj, "via", core99_get_via_config,
                            core99_set_via_config);
    object_property_set_description(obj, "via",
                                    "Set VIA configuration. "
                                    "Valid values are cuda, pmu and pmu-adb");
}

static const TypeInfo core99_machine_info = {
    .name          = MACHINE_TYPE_NAME("mac99"),
    .parent        = TYPE_MACHINE,
    .class_init    = core99_machine_class_init,
    .instance_init = core99_instance_init,
    .instance_size = sizeof(Core99MachineState),
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_FW_PATH_PROVIDER },
        { }
    },
};

static void powermac3_1_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Apple Power Mac G4 AGP (Sawtooth)";
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("7400_v2.9");
}

static void powermac3_1_instance_init(Object *obj)
{
    Core99MachineState *cms = CORE99_MACHINE(obj);

    /* Fixed configuration, remove the "via" property inherited from mac99 */
    object_property_del(obj, "via");
    cms->via_config = CORE99_VIA_CONFIG_PMU;
}

static const TypeInfo powermac3_1_machine_info = {
    .name          = MACHINE_TYPE_NAME("powermac3_1"),
    .parent        = TYPE_CORE99_MACHINE,
    .class_init    = powermac3_1_machine_class_init,
    .instance_init = powermac3_1_instance_init,
};

static void powerbook3_2_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Apple PowerBook G4 Titanium (Mercury)";
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("7400_v2.9");
}

static void powerbook3_2_instance_init(Object *obj)
{
    Core99MachineState *cms = CORE99_MACHINE(obj);

    /* Fixed configuration, remove the "via" property inherited from mac99 */
    object_property_del(obj, "via");
    cms->via_config = CORE99_VIA_CONFIG_PMU_ADB;
}

static const TypeInfo powerbook3_2_machine_info = {
    .name          = MACHINE_TYPE_NAME("powerbook3_2"),
    .parent        = TYPE_CORE99_MACHINE,
    .class_init    = powerbook3_2_machine_class_init,
    .instance_init = powerbook3_2_instance_init,
};

#ifdef TARGET_PPC64

/* A switch given on the command line or with qom-set keeps its value */
static void real_rom_give(Object *obj, RealRomSwitch sw)
{
    CORE99_MACHINE(obj)->real_rom_given |= BIT(sw);
}

static bool powermac7_3_get_highmem(Object *obj, Error **errp)
{
    return CORE99_MACHINE(obj)->highmem;
}

static void powermac7_3_set_highmem(Object *obj, bool value, Error **errp)
{
    CORE99_MACHINE(obj)->highmem = value;
}

static bool powermac7_3_get_macio_low(Object *obj, Error **errp)
{
    return CORE99_MACHINE(obj)->macio_low;
}

static void powermac7_3_set_macio_low(Object *obj, bool value, Error **errp)
{
    real_rom_give(obj, REAL_ROM_MACIO_LOW);
    CORE99_MACHINE(obj)->macio_low = value;
}

static bool powermac7_3_get_ht_mmio_low(Object *obj, Error **errp)
{
    return CORE99_MACHINE(obj)->ht_mmio_low;
}

static void powermac7_3_set_ht_mmio_low(Object *obj, bool value, Error **errp)
{
    real_rom_give(obj, REAL_ROM_HT_MMIO_LOW);
    CORE99_MACHINE(obj)->ht_mmio_low = value;
}

static bool powermac7_3_get_u3_i2c_lane3(Object *obj, Error **errp)
{
    return CORE99_MACHINE(obj)->u3_i2c_lane3;
}

static void powermac7_3_set_u3_i2c_lane3(Object *obj, bool value,
                                         Error **errp)
{
    real_rom_give(obj, REAL_ROM_U3_I2C_LANE3);
    CORE99_MACHINE(obj)->u3_i2c_lane3 = value;
}

static bool powermac7_3_get_u3_i2c_spd(Object *obj, Error **errp)
{
    return CORE99_MACHINE(obj)->u3_i2c_spd;
}

static void powermac7_3_set_u3_i2c_spd(Object *obj, bool value, Error **errp)
{
    real_rom_give(obj, REAL_ROM_U3_I2C_SPD);
    CORE99_MACHINE(obj)->u3_i2c_spd = value;
}

static bool powermac7_3_get_u3_i2c_dumb(Object *obj, Error **errp)
{
    return CORE99_MACHINE(obj)->u3_i2c_dumb;
}

static void powermac7_3_set_u3_i2c_dumb(Object *obj, bool value, Error **errp)
{
    real_rom_give(obj, REAL_ROM_U3_I2C_DUMB);
    CORE99_MACHINE(obj)->u3_i2c_dumb = value;
}

static bool powermac7_3_get_dbdma_devstat(Object *obj, Error **errp)
{
    return CORE99_MACHINE(obj)->dbdma_devstat;
}

static bool powermac7_3_get_k2_fan_tach(Object *obj, Error **errp)
{
    return CORE99_MACHINE(obj)->k2_fan_tach;
}

static void powermac7_3_set_k2_fan_tach(Object *obj, bool value,
                                        Error **errp)
{
    CORE99_MACHINE(obj)->k2_fan_tach = value;
}

static bool powermac7_3_get_nvram_flash_id(Object *obj, Error **errp)
{
    return CORE99_MACHINE(obj)->nvram_flash_id;
}

static void powermac7_3_set_nvram_flash_id(Object *obj, bool value,
                                           Error **errp)
{
    real_rom_give(obj, REAL_ROM_NVRAM_FLASH_ID);
    CORE99_MACHINE(obj)->nvram_flash_id = value;
}

static bool powermac7_3_get_u3_memctl(Object *obj, Error **errp)
{
    return CORE99_MACHINE(obj)->u3_memctl;
}

static void powermac7_3_set_u3_memctl(Object *obj, bool value, Error **errp)
{
    real_rom_give(obj, REAL_ROM_U3_MEMCTL);
    CORE99_MACHINE(obj)->u3_memctl = value;
}

static bool powermac7_3_get_gpio9_idle_high(Object *obj, Error **errp)
{
    return CORE99_MACHINE(obj)->gpio9_idle_high;
}

static void powermac7_3_set_gpio9_idle_high(Object *obj, bool value,
                                            Error **errp)
{
    real_rom_give(obj, REAL_ROM_GPIO9_IDLE_HIGH);
    CORE99_MACHINE(obj)->gpio9_idle_high = value;
}

static bool powermac7_3_get_ht_k2_rom_irq(Object *obj, Error **errp)
{
    return CORE99_MACHINE(obj)->ht_k2_rom_irq;
}

static void powermac7_3_set_ht_k2_rom_irq(Object *obj, bool value,
                                          Error **errp)
{
    real_rom_give(obj, REAL_ROM_HT_K2_ROM_IRQ);
    CORE99_MACHINE(obj)->ht_k2_rom_irq = value;
}

static bool powermac7_3_get_no_gpio_cpu_reset(Object *obj, Error **errp)
{
    return CORE99_MACHINE(obj)->no_gpio_cpu_reset;
}

static void powermac7_3_set_no_gpio_cpu_reset(Object *obj, bool value,
                                              Error **errp)
{
    real_rom_give(obj, REAL_ROM_NO_GPIO_CPU_RESET);
    CORE99_MACHINE(obj)->no_gpio_cpu_reset = value;
}

static bool powermac7_3_get_rom_smp(Object *obj, Error **errp)
{
    return CORE99_MACHINE(obj)->rom_smp;
}

static void powermac7_3_set_rom_smp(Object *obj, bool value, Error **errp)
{
    real_rom_give(obj, REAL_ROM_ROM_SMP);
    CORE99_MACHINE(obj)->rom_smp = value;
}

static bool powermac7_3_get_u3_reg60_nonzero(Object *obj, Error **errp)
{
    return CORE99_MACHINE(obj)->u3_reg60_nonzero;
}

static void powermac7_3_set_u3_reg60_nonzero(Object *obj, bool value,
                                             Error **errp)
{
    real_rom_give(obj, REAL_ROM_U3_REG60_NONZERO);
    CORE99_MACHINE(obj)->u3_reg60_nonzero = value;
}

static void powermac7_3_set_dbdma_devstat(Object *obj, bool value,
                                          Error **errp)
{
    real_rom_give(obj, REAL_ROM_DBDMA_DEVSTAT);
    CORE99_MACHINE(obj)->dbdma_devstat = value;
}

static char *powermac7_3_get_ht_k2_layout(Object *obj, Error **errp)
{
    return g_strdup(ht_k2_layout_names[CORE99_MACHINE(obj)->ht_k2_layout]);
}

static void powermac7_3_set_ht_k2_layout(Object *obj, const char *value,
                                         Error **errp)
{
    Core99MachineState *cms = CORE99_MACHINE(obj);

    for (int i = 0; i < ARRAY_SIZE(ht_k2_layout_names); i++) {
        if (!strcmp(value, ht_k2_layout_names[i])) {
            cms->ht_k2_layout = i;
            real_rom_give(obj, REAL_ROM_HT_K2_LAYOUT);
            return;
        }
    }
    error_setg(errp, "Invalid x-ht-k2-layout value");
    error_append_hint(errp, "Valid values are dev3, dev1, dev1-5.\n");
}

static char *powermac7_3_get_dart(Object *obj, Error **errp)
{
    return g_strdup(OnOffAuto_str(CORE99_MACHINE(obj)->dart));
}

static void powermac7_3_set_dart(Object *obj, const char *value, Error **errp)
{
    Core99MachineState *cms = CORE99_MACHINE(obj);

    if (!strcmp(value, "auto")) {
        cms->dart = ON_OFF_AUTO_AUTO;
    } else if (!strcmp(value, "on")) {
        cms->dart = ON_OFF_AUTO_ON;
    } else if (!strcmp(value, "off")) {
        cms->dart = ON_OFF_AUTO_OFF;
    } else {
        error_setg(errp, "Invalid x-dart value");
        error_append_hint(errp, "Valid values are auto, on, off.\n");
        return;
    }
    real_rom_give(obj, REAL_ROM_DART);
}

static bool powermac7_3_get_real_rom(Object *obj, Error **errp)
{
    return CORE99_MACHINE(obj)->real_rom;
}

static void powermac7_3_set_real_rom(Object *obj, bool value, Error **errp)
{
    CORE99_MACHINE(obj)->real_rom = value;
}

static void powermac7_3_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Apple Power Mac G5 (Niagara)";
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("970fx_v3.1");
    /*
     * A PowerMac7,3 is a 2-way machine.  The KeyLargo pic can address more
     * (KEYLARGO_MAX_CPU) and so can the OS, but no such G5 was built, so
     * keep the machine to what the hardware had.  The base class leaves
     * mac99 and the G4 machines uniprocessor.
     */
    mc->max_cpus = 2;
    machine_add_audiodev_property(mc);

    /*
     * Class properties, so that -M powermac7_3,help lists them; the
     * defaults are set in powermac7_3_instance_init().
     */
    object_class_property_add_bool(oc, "highmem", powermac7_3_get_highmem,
                                   powermac7_3_set_highmem);
    object_class_property_set_description(oc, "highmem",
                                          "Allow more than 2 GiB of RAM (up "
                                          "to 8 GiB), the excess placed from "
                                          "4 GiB. The machine then gets a "
                                          "DART, see x-dart");
    object_class_property_add_str(oc, "x-dart", powermac7_3_get_dart,
                                  powermac7_3_set_dart);
    object_class_property_set_description(oc, "x-dart",
                                          "Give the U3 a DART, the IOMMU in "
                                          "front of its PCI buses: auto (the "
                                          "default; only with more than "
                                          "2 GiB of RAM), on or off. An "
                                          "experimental knob for telling "
                                          "DART effects apart");
    object_class_property_add_bool(oc, "x-macio-80000000",
                                   powermac7_3_get_macio_low,
                                   powermac7_3_set_macio_low);
    object_class_property_set_description(oc, "x-macio-80000000",
                                          "Also show the MacIO at "
                                          "0x80000000, for running a real "
                                          "boot ROM with -bios (off by "
                                          "default). It shadows part of "
                                          "the AGP PCI hole, so do not use "
                                          "it with OpenBIOS");
    object_class_property_add_bool(oc, "x-ht-mmio-80000000",
                                   powermac7_3_get_ht_mmio_low,
                                   powermac7_3_set_ht_mmio_low);
    object_class_property_set_description(oc, "x-ht-mmio-80000000",
                                          "Show the HT bus memory at "
                                          "0x80000000-0x8fffffff and start "
                                          "the AGP PCI hole at 0x90000000, "
                                          "for running a real boot ROM "
                                          "with -bios (off by default). "
                                          "OpenBIOS puts AGP BARs from "
                                          "0x80000000, so do not use it "
                                          "with OpenBIOS");
    object_class_property_add_bool(oc, "x-u3-i2c-lane3",
                                   powermac7_3_get_u3_i2c_lane3,
                                   powermac7_3_set_u3_i2c_lane3);
    object_class_property_set_description(oc, "x-u3-i2c-lane3",
                                          "Let the U3 i2c controller also "
                                          "decode byte 3 of each register "
                                          "slot, where a real boot ROM "
                                          "drives it (off by default)");
    object_class_property_add_bool(oc, "x-u3-i2c-spd",
                                   powermac7_3_get_u3_i2c_spd,
                                   powermac7_3_set_u3_i2c_spd);
    object_class_property_set_description(oc, "x-u3-i2c-spd",
                                          "Put the memory SPD EEPROMs of "
                                          "one pair of DDR DIMMs on U3 i2c "
                                          "channel 1, for a real boot ROM "
                                          "(off by default)");
    object_class_property_add_bool(oc, "x-u3-i2c-dumb",
                                   powermac7_3_get_u3_i2c_dumb,
                                   powermac7_3_set_u3_i2c_dumb);
    object_class_property_set_description(oc, "x-u3-i2c-dumb",
                                          "Let the U3 i2c controller drive "
                                          "DUMB (manual) mode transfers, "
                                          "which a real boot ROM uses (off "
                                          "by default)");
    object_class_property_add_bool(oc, "x-dbdma-devstat",
                                   powermac7_3_get_dbdma_devstat,
                                   powermac7_3_set_dbdma_devstat);
    object_class_property_set_description(oc, "x-dbdma-devstat",
                                          "Let DBDMA control writes set "
                                          "the s0-s7 status bits, which a "
                                          "real boot ROM uses (off by "
                                          "default)");
    object_class_property_add_bool(oc, "x-k2-fan-tach",
                                   powermac7_3_get_k2_fan_tach,
                                   powermac7_3_set_k2_fan_tach);
    object_class_property_set_description(oc, "x-k2-fan-tach",
                                          "Let the fans' TACH counts that "
                                          "a real boot ROM reads move with "
                                          "the virtual clock, so that "
                                          "AppleK2Fan sees them turning "
                                          "(off by default)");
    object_class_property_add_bool(oc, "x-nvram-flash-id",
                                   powermac7_3_get_nvram_flash_id,
                                   powermac7_3_set_nvram_flash_id);
    object_class_property_set_description(oc, "x-nvram-flash-id",
                                          "Let the NVRAM flash answer the "
                                          "Read Identifier command as a "
                                          "Sharp part, which a real boot "
                                          "ROM asks for (off by default)");
    object_class_property_add_bool(oc, "x-u3-memctl",
                                   powermac7_3_get_u3_memctl,
                                   powermac7_3_set_u3_memctl);
    object_class_property_set_description(oc, "x-u3-memctl",
                                          "Keep what is written to the U3 "
                                          "registers at 0xf8002000, which "
                                          "a real boot ROM reads back (off "
                                          "by default)");
    object_class_property_add_bool(oc, "x-gpio9-idle-high",
                                   powermac7_3_get_gpio9_idle_high,
                                   powermac7_3_set_gpio9_idle_high);
    object_class_property_set_description(oc, "x-gpio9-idle-high",
                                          "Rest the NMI line (GPIO 9) high, "
                                          "which a real boot ROM reads as a "
                                          "released NMI button (off by "
                                          "default)");
    object_class_property_add_str(oc, "x-ht-k2-layout",
                                  powermac7_3_get_ht_k2_layout,
                                  powermac7_3_set_ht_k2_layout);
    object_class_property_set_description(oc, "x-ht-k2-layout",
                                          "Where the K2 HT-PCI bridge sits "
                                          "on the HT bus: dev3 (the "
                                          "default), dev1, or dev1-5 (dev 1 "
                                          "plus empty bridges 0046-0049 at "
                                          "dev 2-5), for a real boot ROM");
    object_class_property_add_bool(oc, "x-ht-k2-rom-irq",
                                   powermac7_3_get_ht_k2_rom_irq,
                                   powermac7_3_set_ht_k2_rom_irq);
    object_class_property_set_description(oc, "x-ht-k2-rom-irq",
                                          "Route ht.1 dev 8 and 9 INTx to "
                                          "mpic 0x1b and 0x1c as a real "
                                          "boot ROM's interrupt-map does, "
                                          "leave other ht.1 devices and "
                                          "the AGP bus INTx unconnected "
                                          "(off by default). For running a "
                                          "real boot ROM with -bios; do not "
                                          "use it with OpenBIOS");
    object_class_property_add_bool(oc, "x-no-gpio-cpu-reset",
                                   powermac7_3_get_no_gpio_cpu_reset,
                                   powermac7_3_set_no_gpio_cpu_reset);
    object_class_property_set_description(oc, "x-no-gpio-cpu-reset",
                                          "Leave the MacIO GPIO CPU reset "
                                          "lines (GPIO 3 and 4) unconnected, "
                                          "as a real boot ROM's tree uses "
                                          "them for audio (off by default). "
                                          "For running a real boot ROM with "
                                          "-bios; do not use it with "
                                          "OpenBIOS, which then cannot start "
                                          "the second CPU");
    object_class_property_add_bool(oc, "x-u3-reg60-nonzero",
                                   powermac7_3_get_u3_reg60_nonzero,
                                   powermac7_3_set_u3_reg60_nonzero);
    object_class_property_set_description(oc, "x-u3-reg60-nonzero",
                                          "Let the U3 register at +0x60 "
                                          "read 1 instead of 0; a real boot "
                                          "ROM puts the CPU to sleep when "
                                          "it reads 0 (off by default). "
                                          "Every CPU reads the same value "
                                          "unless x-rom-smp is on");
    object_class_property_add_bool(oc, "x-rom-smp",
                                   powermac7_3_get_rom_smp,
                                   powermac7_3_set_rom_smp);
    object_class_property_set_description(oc, "x-rom-smp",
                                          "Start the second CPU the way a "
                                          "real boot ROM does: CPU 1 runs "
                                          "the ROM from power-on, mac-io "
                                          "byte 0x72 soft-resets it into "
                                          "ROM, the U3 register at +0x50 "
                                          "reads the CPU number, and a "
                                          "CY28508 clock chip answers at "
                                          "8-bit 0xd0 on U3 i2c channel 0 "
                                          "(off by "
                                          "default). For running a real "
                                          "boot ROM with -bios");
    object_class_property_add_bool(oc, "x-real-rom",
                                   powermac7_3_get_real_rom,
                                   powermac7_3_set_real_rom);
    object_class_property_set_description(oc, "x-real-rom",
                                          "Default the switches a real boot "
                                          "ROM run with -bios needs to what "
                                          "it wants (off by default): "
                                          "x-dart=on, x-ht-k2-layout=dev1-5, "
                                          "and on for x-macio-80000000, "
                                          "x-ht-mmio-80000000, "
                                          "x-u3-i2c-lane3, x-u3-i2c-spd, "
                                          "x-u3-i2c-dumb, x-dbdma-devstat, "
                                          "x-nvram-flash-id, x-u3-memctl, "
                                          "x-ht-k2-rom-irq, "
                                          "x-no-gpio-cpu-reset, "
                                          "x-u3-reg60-nonzero, x-rom-smp and "
                                          "x-gpio9-idle-high, so that the ROM "
                                          "boots without stopping at its "
                                          "prompt; give boot-device and "
                                          "boot-args with -prom-env. A switch "
                                          "given on its own keeps its value: "
                                          "x-gpio9-idle-high=off stops at the "
                                          "prompt. highmem is left alone. "
                                          "QEMU does not tell the ROM apart, "
                                          "so do not use it with OpenBIOS");
}

static void powermac7_3_instance_init(Object *obj)
{
    Core99MachineState *cms = CORE99_MACHINE(obj);

    /* Fixed configuration, remove the "via" property inherited from mac99 */
    object_property_del(obj, "via");
    cms->via_config = CORE99_VIA_CONFIG_PMU;
    cms->has_u3_ht = true;

    /* Off by default, so that the machine stays as it was measured */
    cms->highmem = false;
    /* auto: a DART only when the RAM goes beyond 2 GiB */
    cms->dart = ON_OFF_AUTO_AUTO;
    /* Off: only a real boot ROM wants the MacIO at 0x80000000 */
    cms->macio_low = false;
    /* Off: only a real boot ROM puts HT bus BARs at 0x8xxxxxxx */
    cms->ht_mmio_low = false;
    /* Off: only a real boot ROM drives the U3 i2c at byte 3 */
    cms->u3_i2c_lane3 = false;
    /* Off: only a real boot ROM reads the SPD; OpenBIOS fills in memory */
    cms->u3_i2c_spd = false;
    /* Off: Linux refuses DUMB mode; only a real boot ROM was seen using it */
    cms->u3_i2c_dumb = false;
    /* Off: no guest driver was seen writing s0-s7 through control */
    cms->dbdma_devstat = false;
    cms->nvram_flash_id = false;
    /* Off: no guest OS was seen touching 0xf8002000; Linux does not */
    cms->u3_memctl = false;
    /* Off: QEMU has always rested the NMI line low */
    cms->gpio9_idle_high = false;
    /* dev3: where QEMU has always put the K2 HT-PCI bridge */
    cms->ht_k2_layout = HT_K2_LAYOUT_DEV3;
    /* Off: OpenBIOS and Linux start the second CPU through these lines */
    cms->no_gpio_cpu_reset = false;
    /* Off: QEMU has always read 0 there */
    cms->u3_reg60_nonzero = false;
    /* Off: CPU 1 only starts through the KeyLargo reset GPIOs */
    cms->rom_smp = false;
    /* Off: the switches above keep the defaults set here */
    cms->real_rom = false;
    cms->real_rom_given = 0;
}

static const TypeInfo powermac7_3_machine_info = {
    .name          = MACHINE_TYPE_NAME("powermac7_3"),
    .parent        = TYPE_CORE99_MACHINE,
    .class_init    = powermac7_3_machine_class_init,
    .instance_init = powermac7_3_instance_init,
};
#endif

static void mac_machine_register_types(void)
{
    type_register_static(&core99_machine_info);
    type_register_static(&powermac3_1_machine_info);
    type_register_static(&powerbook3_2_machine_info);
#ifdef TARGET_PPC64
    type_register_static(&powermac7_3_machine_info);
#endif
}

type_init(mac_machine_register_types)
