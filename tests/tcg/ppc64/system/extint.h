/*
 * Helper for taking an external interrupt on purpose (see extint.S).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PPC64_EXTINT_H
#define PPC64_EXTINT_H

/*
 * Enable MSR[EE] with an interrupt pending and return the vector that fired
 * (0 if none did).  SRR0/SRR1 as the handler saw them are left behind.
 */
unsigned long take_external_interrupt(void);

extern unsigned long ext_srr0;
extern unsigned long ext_srr1;

#endif /* PPC64_EXTINT_H */
