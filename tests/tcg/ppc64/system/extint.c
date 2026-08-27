/*
 * External interrupt delivery on a 970 in hypervisor state.
 *
 * QEMU decides between SRR0/1 and HSRR0/1 for an external interrupt by
 * looking at LPCR[LPES0], but only once the core has a hypervisor state.
 * A 970 has no LPCR, so the register reads as zero and would select
 * HSRR0/1 -- which the 970 does not implement and cannot return from.  On
 * hardware these interrupts arrive through SRR0/1, and the guest dies
 * quickly if they do not: Mac OS X panics as soon as it enables them.
 *
 * Raise one for real through the interrupt controller and check where it
 * landed.  An IPI sent to ourselves is the shortest path to a live external
 * interrupt on this board.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <minilib.h>

#include "extint.h"

/* The mpic on a PowerMac7,3, as the board maps it on the sysbus. */
#define MPIC_BASE       0xf8040000UL

#define MPIC_CPU_BASE   (MPIC_BASE + 0x20000)
#define MPIC_CTPR       (MPIC_CPU_BASE + 0x80)
#define MPIC_IACK       (MPIC_CPU_BASE + 0xa0)
#define MPIC_EOI        (MPIC_CPU_BASE + 0xb0)
#define MPIC_IPI0_DR    (MPIC_CPU_BASE + 0x40)

/*
 * Vector/priority for IPI 0 lives in the global block, which the board maps
 * as the first 0x10f0 bytes at MPIC_BASE (info mtree: "glb").
 */
#define MPIC_IPI0_IVPR  (MPIC_BASE + 0x10a0)

#define IVPR_MASK       0x80000000U
#define IVPR_PRIORITY   (8U << 16)
#define IPI_VECTOR      0x2aU

#define VEC_EXTERNAL    0x500UL

#define SRR0_POISON     0xdeadbeefUL

/*
 * The mpic presents its registers little-endian (see the openpic memory ops),
 * so go through the byte-reversing accesses rather than plain lwz/stw.
 */
static void st_le32(unsigned long addr, unsigned int val)
{
    asm volatile("stwbrx %0, 0, %1; eieio" : : "r"(val), "r"(addr) : "memory");
}

static unsigned int ld_le32(unsigned long addr)
{
    unsigned int val;

    asm volatile("lwbrx %0, 0, %1; eieio" : "=r"(val) : "r"(addr) : "memory");
    return val;
}

int main(void)
{
    unsigned long vec;
    unsigned int iack;
    int ok = 1;

    /* Let everything through, then arm IPI 0 and send it to ourselves. */
    st_le32(MPIC_CTPR, 0);
    st_le32(MPIC_IPI0_IVPR, IVPR_PRIORITY | IPI_VECTOR);
    st_le32(MPIC_IPI0_DR, 1);

    vec = take_external_interrupt();

    if (vec != VEC_EXTERNAL) {
        ml_printf("FAIL: extint took vector 0x%lx, expected 0x%lx\n",
                  vec, VEC_EXTERNAL);
        ok = 0;
    } else if (ext_srr0 == SRR0_POISON) {
        ml_printf("FAIL: extint left SRR0 untouched (0x%lx): the interrupt "
                  "was delivered through HSRR0/1\n", ext_srr0);
        ok = 0;
    }

    /* Acknowledge it so the controller does not stay stuck in service. */
    iack = ld_le32(MPIC_IACK);
    st_le32(MPIC_EOI, 0);

    if (vec == VEC_EXTERNAL && iack != IPI_VECTOR) {
        ml_printf("FAIL: extint acknowledged vector 0x%x, expected 0x%x\n",
                  iack, IPI_VECTOR);
        ok = 0;
    }

    if (ok) {
        ml_printf("PASS: extint\n");
    }

    return 0;
}
