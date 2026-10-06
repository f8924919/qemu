/*
 * qemu-g5 #373: throwaway counters that attribute TLB refills
 * (tlb_fill_align) to the reason the translation was missing.
 * NOT FOR UPSTREAM.
 *
 * The counters are one row per vCPU, written only by that vCPU's thread.
 * Build with -DG5L_NO_SHADOW to drop the shadow record, the "seen" bitmap
 * and the flush event ring; only the counters that do not depend on them
 * (enter, ok, ev, work, abits, chk, ref) are then printed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef G5_FILLCNT_H
#define G5_FILLCNT_H

#include <stdint.h>
#include <stdbool.h>
#include "qemu/typedefs.h"

/* Callers of tlb_fill_align (G5L_C_*) */
#define G5L_C_PROBE     0
#define G5L_C_LOOKUP1   1
#define G5L_C_ATOMIC_ST 2
#define G5L_C_ATOMIC_LD 3
#define G5L_NCALLER     4

/* Causes of a refill (exclusive).  Order is the README order. */
enum {
    G5L_CA_UPGRADE,
    G5L_CA_VFLAG_MMIO,
    G5L_CA_VFLAG_DISCARD,
    G5L_CA_VFLAG_WATCH,
    G5L_CA_VFLAG_NOTDIRTY,
    G5L_CA_VFLAG_OTHER,
    G5L_CA_MAIN_UNCLASS,
    G5L_CA_FULL,
    G5L_CA_RING_LOST,
    G5L_CA_LARGE,
    G5L_CA_RANGE_ALL,
    G5L_CA_RANGE_PAGE,
    G5L_CA_PAGE,
    G5L_CA_CONFLICT,
    G5L_CA_FIRST,
    G5L_CA_OVF,
    G5L_CA_UNCLASS,
    G5L_NCAUSE,
};

/* Classification done by cputlb.c from the live TLB (steps 1 and 2). */
#define G5L_PRE_NONE        (-1)   /* falls through to the shadow record */
/* otherwise a G5L_CA_UPGRADE / G5L_CA_VFLAG_* / G5L_CA_MAIN_UNCLASS value */

/* Flush paths (the "ev" table) */
#define G5L_P_ASYNC  0
#define G5L_P_LARGE  1
#define G5L_P_RANGE  2
#define G5L_NPATH    3

/* Marks written on the shadow record by a single-page kill */
#define G5L_M_NONE      0
#define G5L_M_PAGE      1
#define G5L_M_RANGE     2
#define G5L_M_CONFLICT  3
/* g5l_mark_kind value that makes kill notifications be ignored */
#define G5L_M_IGNORE    0

/* SLB slot variable */
#define G5L_SLB_UNSET   (-1)
#define G5L_SLB_VRMA    (-2)

/* Caller of the tlb_fill_align about to run on this thread (G5L_C_*). */
extern __thread uint32_t g5l_caller;
/* Path of the tlb_flush_one_mmuidx_locked about to run (G5L_P_*). */
extern __thread uint32_t g5l_flush_path;
/* Folded trigger (0 slbia, 1 tlbie, 2 other) and work kind (0 full, 1 part). */
extern __thread uint32_t g5l_flush_trig;
extern __thread uint32_t g5l_flush_wk;
/* Mark kind for single-page kill notifications (G5L_M_*; NONE = ignore). */
extern __thread uint32_t g5l_mark_kind;
/* Page of the entry that was just killed (read before the memset). */
extern __thread uint64_t g5l_kill_page;
/* SLB slot used by the last ppc_hash64_xlate on this thread. */
extern __thread int32_t g5l_slb_idx;

/*
 * Entry of tlb_fill_align.  @pre is G5L_PRE_NONE or a cause decided from the
 * live TLB; @vbits are the flag bits for G5L_CA_VFLAG_* (see g5-fillcnt.c).
 */
void g5l_enter(CPUState *cpu, uint64_t page, int mmu_idx, int acc, int pre,
               unsigned vbits);
/* Success: end of tlb_set_page_full. */
void g5l_ok(CPUState *cpu, int mmu_idx, uint64_t page, uint64_t phys,
            int prot, const void *attrs, unsigned attrs_size, int lg_page_size);
/* One tlb_flush_one_mmuidx_locked call (the TLB of @mmu_idx was emptied). */
void g5l_flush_event(CPUState *cpu, int mmu_idx, bool resized);
/* Convert a #364 tag into the folded trigger (0 slbia, 1 tlbie, 2 other). */
uint32_t g5l_fold_trig(uint32_t tag);
/* Single-page kill: mark the shadow record if its key matches. */
void g5l_mark(CPUState *cpu, int mmu_idx, uint64_t page, int kind);
/* A flush work ran (same arguments as g5f_work). */
void g5l_work(CPUState *cpu, uint32_t tag, int kind, int pbits, int ebits);
/* Victim TLB hit. */
void g5l_vhit(CPUState *cpu);
/* Secondary hash group searched. */
void g5l_sec_hash(CPUState *cpu);
/* slbmte changed the ESID of entry 0. */
void g5l_slbmte0(CPUState *cpu);

/*
 * SLB dump around the first helper_SLBIA after G5L_SLBDUMP armed it.
 * g5l_slbdump_begin returns true when the caller must print the "before"
 * dump (and later the "after" one); g5l_slbdump_end returns true for the
 * "after" dump.  g5l_slbdump_line prints one G5LSLB line to stderr.
 */
bool g5l_slbdump_begin(CPUState *cpu);
bool g5l_slbdump_end(CPUState *cpu);
void g5l_slbdump_line(CPUState *cpu, bool before, int idx, uint64_t esid,
                      uint64_t vsid);

/* Append "G5L ..." lines to @buf (a GString *). */
void g5l_dump(void *buf);

/*
 * qemu-g5 #385: classification of the flush being issued on this thread by
 * check_tlb_flush (set by target/ppc around tlb_flush*; valid = false when
 * the flush did not come from a classified consumption).
 */
#define G5R_SP_NONE 6
extern __thread bool g5r_cur_valid;
extern __thread bool g5r_cur_skip;
extern __thread uint8_t g5r_cur_sp;        /* G5S_C_* of S' */
/* Called at the end of every flush work with the mmu_idx map it was asked. */
extern void (*g5r_reset_hook)(CPUState *cpu, uint32_t asked);
/* Appends the "G5R ..." lines of target/ppc. */
extern void (*g5r_dump_hook)(void *buf);
/*
 * Walks the live entries (main and victim) of the mmu_idx in @idxmask and
 * calls @ok on each page; returns the pages for which @ok returned false.
 * *@scanned is increased by the pages visited.
 */
uint64_t g5r_scan(CPUState *cpu, uint32_t idxmask,
                  bool (*ok)(void *opaque, uint64_t page), void *opaque,
                  uint64_t *scanned);

#endif
