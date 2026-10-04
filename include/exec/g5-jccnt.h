/*
 * qemu-g5 #369: throwaway counters for the jump cache recheck.
 * NOT FOR UPSTREAM.
 *
 * Every row is written by the vCPU thread it is indexed by; the monitor
 * reads them racily, which is fine because snapshots are taken with the
 * VM stopped.
 *
 * Two knobs, read once from the environment:
 *   G5JC_SKIP_RECHECK=1  treat a stale entry as checked without the recheck
 *                        (the mutation); the cross-check still runs and
 *                        the QHT answer is what gets executed
 *   G5JC_OLD=1           also empty the jump cache on a full TLB flush
 *                        (the behaviour before the change)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef G5_JCCNT_H
#define G5_JCCNT_H

#include <stdint.h>

enum {
    G5JC_LOOKUP,        /* tb_lookup() calls */
    G5JC_HIT_CHECKED,   /* key matched and the entry was checked */
    G5JC_ENTER,         /* key matched, entry stale: recheck started */
    G5JC_PASS,          /* recheck passed */
    G5JC_CMPFAIL,       /* recheck failed the QHT test */
    G5JC_MINUS1,        /* recheck refused: no RAM behind pc */
    G5JC_SKIPPED,       /* stale entry taken without recheck (knob) */
    G5JC_X_SAME,        /* cross-check: QHT returned the same TB */
    G5JC_X_DIFF,        /* cross-check: QHT returned another TB */
    G5JC_X_NULL,        /* cross-check: QHT returned nothing */
    G5JC_QHT,           /* QHT lookups on the miss path */
    G5JC_N
};

#define G5JC_MAXCPU 8

extern int g5jc_skip_recheck;
extern int g5jc_old;

void g5jc_count(int cpu, int what);
void g5jc_dump(void *buf);

#endif
