/*
 * qemu-g5 #385: per-vCPU sets of SLB values that model "skip the flush of
 * an slbia-only request when every SLB value the TLB still depends on is
 * mapped again with the same value".  Pure C, no QEMU headers, so that the
 * logic can be tested on the host (tests/tools/test_slbset_385.py).
 * NOT FOR UPSTREAM.
 *
 * The canonical copy is tests/fixtures/tcg-385/g5-slbset.{c,h} in the meta
 * repository; the counter branch carries an identical copy in
 * accel/tcg/g5-slbset.c and include/exec/g5-slbset.h.
 *
 * Sets:
 *   spp  S'' : values used by refills since the last flush that the
 *              modelled change would NOT have skipped (decision metric)
 *   sp   S'  : values used by refills since the last flush that actually
 *              emptied the SLB-backed mmu_idx (reference)
 *   s    S   : values valid when the SLB-backed mmu_idx was last emptied,
 *              plus every value written with V since (reference)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef G5_SLBSET_H
#define G5_SLBSET_H

#include <stdbool.h>
#include <stdint.h>

#define G5S_NSLOT      64      /* SLB entries tracked (970: 64) */
#define G5S_CAP_SPP    256
#define G5S_CAP_SP     128
#define G5S_CAP_S      256
#define G5S_CAP_PEND   64
#define G5S_CAP_MISS   64

/* Classification of one set at a consumption point */
#define G5S_C_OK       0       /* every value maps to itself */
#define G5S_C_MISS1    1
#define G5S_C_MISS2    2
#define G5S_C_MISS3    3
#define G5S_C_MISS4    4       /* 4 or more missing */
#define G5S_C_OVF      5       /* the set overflowed: not judged */
#define G5S_NCLS       6

/* Writers of an SLB entry */
#define G5S_W_SLBMTE   0
#define G5S_W_MTSR     1
#define G5S_W_OTHER    2       /* migration and anything not marked */
#define G5S_NWRITER    3

/* One SLB value.  esid is stored without the V bit; sps is compared as a
 * pointer value. */
typedef struct G5SVal {
    uint64_t esid;
    uint64_t vsid;
    uintptr_t sps;
} G5SVal;

typedef struct G5SSet {
    G5SVal *v;
    uint32_t n;
    uint32_t cap;
    bool ovf;
    uint32_t gen;              /* bumped by g5s_set_clear (starts at 1) */
    uint64_t novf;             /* times the set became overflowed */
    uint64_t nclr;             /* times g5s_set_clear ran (not g5s_init) */
} G5SSet;

typedef struct G5SPend {
    G5SVal v;
    uint64_t gen;              /* consumption number that found it missing */
} G5SPend;

/*
 * Looks up @esid the way slb_lookup() does (first valid match in slot
 * order) and stores the value found in @out.  Returns false if no valid
 * entry maps @esid.
 */
typedef bool (*G5SLookup)(void *opaque, uint64_t esid, G5SVal *out);

typedef struct G5SState {
    G5SSet spp, sp, s;
    G5SVal spp_buf[G5S_CAP_SPP];
    G5SVal sp_buf[G5S_CAP_SP];
    G5SVal s_buf[G5S_CAP_S];
    /* per slot: generation of spp / sp in which the slot's value was added */
    uint32_t mark_spp[G5S_NSLOT];
    uint32_t mark_sp[G5S_NSLOT];

    /* result of the last g5s_consume, valid until g5s_after_flush */
    bool skip;
    int cls_spp, cls_sp, cls_s;
    G5SVal miss[G5S_CAP_MISS];
    uint32_t nmiss;

    /* values missing from spp at a consumption, waiting to be written */
    G5SPend pend[G5S_CAP_PEND];
    uint32_t npend;
    uint64_t ncons;            /* consumptions so far (pending generation) */

    /* counters */
    uint64_t late[G5S_NWRITER];
    uint64_t never;
    uint64_t pend_ovf;         /* missing values dropped: miss[] or pend[] full */
} G5SState;

/* Zero @st and bind the set buffers.  All sets start empty, gen = 1. */
void g5s_init(G5SState *st);

/* Low-level set operations (exposed for the tests) */
void g5s_set_clear(G5SSet *set);
/* Adds @v unless an equal value is present.  Returns false (and sets ovf,
 * counting novf on the first refusal since the last clear) when the set is
 * full. */
bool g5s_set_add(G5SSet *set, const G5SVal *v);
bool g5s_set_has(const G5SSet *set, const G5SVal *v);
/*
 * Classifies @set against the current SLB: G5S_C_OVF if the set overflowed,
 * otherwise the number of values whose ESID does not look up to an equal
 * value (G5S_C_OK .. G5S_C_MISS4).  If @miss is not NULL, the missing
 * values are copied there (up to @maxmiss) and *@nmiss is set.
 */
int g5s_classify(const G5SSet *set, G5SLookup lk, void *opaque,
                 G5SVal *miss, uint32_t maxmiss, uint32_t *nmiss);

/*
 * A refill translated through SLB @slot whose value is @v.  Adds @v to spp
 * and sp, skipping the search when the slot is already marked for the
 * current generation of that set.
 */
void g5s_fill(G5SState *st, int slot, const G5SVal *v);

/*
 * ppc_store_slb wrote @v into @slot (@valid = V bit set) on behalf of
 * @writer (G5S_W_*).  Drops the slot marks; if @valid, adds @v to s and
 * closes a pending missing value equal to @v as "late" for @writer.
 */
void g5s_store(G5SState *st, int slot, const G5SVal *v, bool valid,
               int writer);

/*
 * check_tlb_flush is about to flush.  @slbia_only: the request set is
 * exactly {slbia}.  Classifies spp, sp and s; the flush is skippable when
 * @slbia_only and spp classifies as G5S_C_OK.  The values missing from spp
 * are kept until g5s_after_flush (up to G5S_CAP_MISS; the rest count as
 * pend_ovf).  Increments ncons first.  Returns st->skip.
 */
bool g5s_consume(G5SState *st, bool slbia_only, G5SLookup lk, void *opaque);

/*
 * The flush issued after g5s_consume returned (any reset it caused has
 * run).  Moves the values missing from spp into the pending list, tagged
 * with this consumption's number (ncons); counts pend_ovf for each value
 * that does not fit.
 */
void g5s_after_flush(G5SState *st);

/*
 * The SLB-backed mmu_idx were emptied.  @skippable: the flush was one that
 * the modelled change would have skipped.  Clears sp and rebuilds s from
 * the @nvalid values in @valid; unless @skippable, also clears spp and
 * counts every pending value tagged with an older consumption than ncons
 * as "never" and removes it.
 */
void g5s_reset(G5SState *st, bool skippable, const G5SVal *valid,
               uint32_t nvalid);

#endif
