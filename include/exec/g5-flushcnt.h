/*
 * qemu-g5 #364: throwaway counters that attribute TCG TLB full flushes to
 * the ppc instruction (or path) that armed them.  NOT FOR UPSTREAM.
 *
 * A "tag" travels with each flush:
 *   bits 0-5   trigger set (G5F_T_*), armed on this vCPU since its last flush
 *   bits 6-8   entry point that executed the flush (G5F_E_*)
 *   bits 9-11  index of the vCPU that issued the flush
 *   bit  12    valid (0 = flush not issued by an instrumented ppc path)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef G5_FLUSHCNT_H
#define G5_FLUSHCNT_H

#include <stdint.h>

#define G5F_T_TLBIE   (1u << 0)  /* tlbie or tlbiel (helper_tlbie) */
#define G5F_T_TLBIEG  (1u << 1)  /* at least one non-local tlbie */
#define G5F_T_SLBIE   (1u << 2)  /* slbie of a valid entry */
#define G5F_T_SLBIA   (1u << 3)
#define G5F_T_SDR1    (1u << 4)  /* SDR1 changed (direct tlb_flush) */
#define G5F_T_RESET   (1u << 5)  /* ppc_tlb_invalidate_all */
#define G5F_NTRIG     6

#define G5F_E_NONE    0
#define G5F_E_ISYNC   1  /* helper_check_tlb_flush_local */
#define G5F_E_PTESYNC 2  /* helper_check_tlb_flush_global */
#define G5F_E_EXCP    3  /* powerpc_do_interrupt */
#define G5F_E_RFI     4  /* do_rfi and friends */
#define G5F_E_DIRECT  5  /* direct tlb_flush (SDR1, reset) */

#define G5F_TAG_VALID (1u << 12)
#define G5F_NTAG      (1u << 13)
#define G5F_MAXCPU    8
#define G5F_RB_RING   256
#define G5F_RB_EVERY  16
#define G5F_COAL_MAX  64

/* Tag for the flush being issued on this thread (0 = none). */
extern __thread uint32_t g5f_cur_tag;
/* Entry point recorded by the caller of check_tlb_flush on this thread. */
extern __thread uint32_t g5f_cur_entry;

/* Arm a trigger on vCPU @cpu (counts a request). */
void g5f_arm(int cpu, uint32_t trig);
/* Record a sampled tlbie RB on vCPU @cpu. */
void g5f_tlbie_rb(int cpu, uint64_t rb);
/*
 * Consume the armed set before issuing a flush.  @global consumes both the
 * local and the global pending sets.  Returns the tag to put in
 * g5f_cur_tag around the tlb_flush*() call.
 */
uint32_t g5f_take(int cpu, int global, uint32_t entry, uint32_t extra_trig);
/*
 * Called from tlb_flush_by_mmuidx_async_work. kind: 0 full, 1 part, 2 elide
 * (nothing cleaned).  pbits/ebits mirror what that call adds to
 * part_flush_count / elide_flush_count, so the sums can be checked against
 * `info jit`.
 */
void g5f_work(int cpu, uint32_t tag, int kind, int pbits, int ebits);
/* Append "G5FLUSH ..." lines to @buf (a GString *). */
void g5f_dump(void *buf);

#endif
