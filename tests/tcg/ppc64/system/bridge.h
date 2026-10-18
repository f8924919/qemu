/*
 * Helpers for the 64-bit bridge test (see bridge.S).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PPC64_BRIDGE_H
#define PPC64_BRIDGE_H

/*
 * Run br_l0..br_l5 with MSR[SE] set; ea is what the tlbie at br_l0
 * invalidates.  br_trace_state[0] counts the trace interrupts and [1..]
 * are their SRR0s.
 */
void br_trace_run(unsigned long ea);
extern unsigned long br_trace_state[16];

/* Where each stepped instruction ends */
extern char br_l1[], br_l2[], br_l3[], br_l4[], br_l5[];

/* Handler stub, to be copied onto the 0xd00 vector */
extern char br_trace_stub[], br_trace_stub_end[];

#endif /* PPC64_BRIDGE_H */
