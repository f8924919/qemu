/*
 * Helper for taking data storage faults on purpose (see mmufault.S).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PPC64_MMUFAULT_H
#define PPC64_MMUFAULT_H

/*
 * Seed DSISR, read from ea with data relocation enabled, and return the
 * vector that fired (0 if the access succeeded).  What the handler saw is
 * left in fault_dsisr and fault_dar.
 */
unsigned long take_data_fault(unsigned long ea, unsigned long seed);

extern unsigned long fault_dsisr;
extern unsigned long fault_dar;

#endif /* PPC64_MMUFAULT_H */
