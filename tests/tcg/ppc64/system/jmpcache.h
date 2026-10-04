/*
 * Helpers for the jump cache test (see jmpcache.S).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PPC64_JMPCACHE_H
#define PPC64_JMPCACHE_H

/*
 * Leave for entry with instruction relocation on (r4 = ea) and return what
 * the code that was reached put in r3 before its sc.
 */
unsigned long jc_run(unsigned long entry, unsigned long ea);

/*
 * What the 0x400 handler works with: [0] times it ran, [1] SRR0 of the last
 * time, [2] address of the PTE to install, [3] and [4] its two doublewords.
 */
extern unsigned long jc_isi_state[5];

/* Handler stubs, to be copied onto the vectors */
extern char jc_sc_stub[], jc_sc_stub_end[];
extern char jc_isi_stub[], jc_isi_stub_end[];

#endif /* PPC64_JMPCACHE_H */
