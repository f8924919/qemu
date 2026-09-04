/*
 * Helpers for putting a 970 to sleep on purpose (see hid0nap.S).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PPC64_HID0NAP_H
#define PPC64_HID0NAP_H

/* Set MSR[POW] with MSR[EE] clear.  Returns if the core kept running. */
void set_pow_with_ee_off(void);

/*
 * Arm the decrementer, then set MSR[POW] and MSR[EE] together and spin
 * until the decrementer interrupt arrives.  Returns the number of spins:
 * a core that actually napped never gets to spin and returns 0.  SRR0 as
 * the handler saw it is left in nap_srr0.
 */
unsigned long nap_until_decrementer(void);

extern unsigned long nap_srr0;

#endif /* PPC64_HID0NAP_H */
