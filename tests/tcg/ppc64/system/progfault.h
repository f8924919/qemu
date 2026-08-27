/*
 * Helper for taking a program interrupt on purpose (see progfault.S).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PPC64_PROGFAULT_H
#define PPC64_PROGFAULT_H

/*
 * Execute a word that decodes to nothing and return the vector that fired
 * (0 if none did).  SRR0 as the handler saw it is left in prog_srr0, and the
 * address of the offending word in prog_expect.
 */
unsigned long take_program_fault(void);

extern unsigned long prog_srr0;
extern unsigned long prog_expect;

#endif /* PPC64_PROGFAULT_H */
