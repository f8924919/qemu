/*
 * qemu-g5 #368: throwaway counters that explain why tb_lookup() misses the
 * per-vCPU jump cache.  NOT FOR UPSTREAM.
 *
 * Every counter row belongs to one vCPU.  Rows are written by the owning
 * vCPU thread with plain increments, except for the counters that other
 * threads (tb_jmp_cache_inval_tb, tb_flush, reset, plugins) may touch; those
 * use qatomic_add.  The monitor reads the rows racily; snapshots are taken
 * with the VM stopped.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef G5_TBCNT_H
#define G5_TBCNT_H

#include "qemu/typedefs.h"
#include "exec/vaddr.h"

#define G5T_MAXCPU 8   /* must be a power of two */

/* tb_lookup() entry points */
enum {
    G5T_E_HELPER,
    G5T_E_LOOP,
    G5T_E_ATOMIC,
    G5T_E_RELOOP,
    G5T_E_N
};

/* lookup results (the 'all' row is counted separately) */
enum {
    G5T_R_HIT,
    G5T_R_COLD_A,
    G5T_R_COLD_B,
    G5T_R_COLD_C,
    G5T_R_COLD_C_PC,
    G5T_R_COLD_X,
    G5T_R_CONFLICT,
    G5T_R_MM_FLAGS,
    G5T_R_MM_CFLAGS,
    G5T_R_MM_CSBASE,
    G5T_R_N
};

/* jump cache clear causes */
enum {
    G5T_C_ASYNC_FULL,
    G5T_C_ASYNC_PART,
    G5T_C_ASYNC_ELIDE,
    G5T_C_PAGE,
    G5T_C_RANGE_ALL,
    G5T_C_RANGE_PAGE,
    G5T_C_TBFLUSH,
    G5T_C_PCREL,
    G5T_C_SLOT,
    G5T_C_RESET,
    G5T_C_OTHER,
    G5T_C_N
};

/* kinds of helper_lookup_tb_ptr callers */
enum {
    G5T_K_LR,
    G5T_K_CTR,
    G5T_K_TAR,
    G5T_K_XPAGE_B,
    G5T_K_XPAGE_BC_TAKEN,
    G5T_K_XPAGE_BC_FALL,
    G5T_K_PAGEEND,
    G5T_K_NOGOTOTB,
    G5T_K_OTHER,
    G5T_K_UNTAGGED,
    G5T_K_N
};

/* kind x result columns */
enum {
    G5T_KR_CALLS,
    G5T_KR_HIT,
    G5T_KR_COLD_B,
    G5T_KR_COLD_OTHER,
    G5T_KR_CONFLICT,
    G5T_KR_MISMATCH,
    G5T_KR_QHT_MISS,
    G5T_KR_N
};

/* qht result columns */
enum {
    G5T_Q_TRY,
    G5T_Q_HIT,
    G5T_Q_NOPHYS,
    G5T_Q_MISS,
    G5T_Q_N
};

/* tb_gen_code columns */
enum {
    G5T_G_ENTER,
    G5T_G_NEW,
    G5T_G_ONESHOT,
    G5T_G_REPLACED,
    G5T_G_N
};

/* what the previous clear of a jump cache slot threw away */
typedef struct G5TShadow {
    TranslationBlock *tb;
    unsigned flush_count;
    uint8_t cause;              /* G5T_C_* + 1; 0 = never cleared non-empty */
} G5TShadow;

#define G5T_SHADOW_SIZE 4096    /* == TB_JMP_CACHE_SIZE */

typedef struct G5TRow {
    uint64_t all[G5T_E_N];
    uint64_t lk[G5T_E_N][G5T_R_N];
    uint64_t coldb[G5T_E_N][G5T_C_N];
    uint64_t b1[G5T_E_N];
    uint64_t qht[G5T_E_N][G5T_Q_N];
    uint64_t gen[G5T_G_N];
    uint64_t iorecomp;
    uint64_t clr_events[G5T_C_N];
    uint64_t clr_nonnull[G5T_C_N];
    uint64_t pbits;
    uint64_t ebits;
    uint64_t kind[G5T_K_N][G5T_KR_N];
    uint64_t kindchk_bad[G5T_K_N];
    /* mark left by a helper lookup that fell through to the main loop */
    bool relook_valid;
    vaddr relook_pc;
    uint32_t relook_flags;
    uint32_t relook_cflags;
    G5TShadow shadow[G5T_SHADOW_SIZE];
} QEMU_ALIGNED(64) G5TRow;

extern G5TRow g5t_rows[G5T_MAXCPU];

static inline G5TRow *g5t_row(int cpu_index)
{
    return &g5t_rows[cpu_index & (G5T_MAXCPU - 1)];
}

/* hot path: a jump cache hit; @kind < 0 unless entry == G5T_E_HELPER */
static inline void g5t_lk_enter(int cpu_index, int entry, int kind)
{
    G5TRow *r = g5t_row(cpu_index);

    r->all[entry]++;
    if (kind >= 0) {
        r->kind[kind][G5T_KR_CALLS]++;
    }
}

static inline void g5t_lk_hit(int cpu_index, int entry, int kind)
{
    G5TRow *r = g5t_row(cpu_index);

    r->lk[entry][G5T_R_HIT]++;
    if (kind >= 0) {
        r->kind[kind][G5T_KR_HIT]++;
    }
}

/*
 * The jump cache slot did not match (slot @slot holds @tb, which may be
 * NULL, with the stale pc @slot_pc).  Classify it, count it, and return the
 * TB that the previous clear threw away if this is a "cold (b)" miss, else
 * NULL.
 */
TranslationBlock *g5t_lk_miss(int cpu_index, int entry, int kind,
                              unsigned slot, TranslationBlock *tb,
                              vaddr slot_pc, vaddr pc, uint64_t cs_base,
                              uint32_t flags, uint32_t cflags);

static inline void g5t_qht_try(int cpu_index, int entry)
{
    g5t_row(cpu_index)->qht[entry][G5T_Q_TRY]++;
}

static inline void g5t_qht_res(int cpu_index, int entry, int res)
{
    g5t_row(cpu_index)->qht[entry][res]++;
}

static inline void g5t_b1(int cpu_index, int entry)
{
    g5t_row(cpu_index)->b1[entry]++;
}

static inline void g5t_kind_qht_miss(int cpu_index, int kind)
{
    g5t_row(cpu_index)->kind[kind][G5T_KR_QHT_MISS]++;
}

static inline void g5t_gen(int cpu_index, int res)
{
    g5t_row(cpu_index)->gen[res]++;
}

static inline void g5t_iorecomp(int cpu_index)
{
    g5t_row(cpu_index)->iorecomp++;
}

/* the helper lookup missed the jump cache and qht: mark for the main loop */
void g5t_helper_missed(int cpu_index, vaddr pc, uint32_t flags,
                       uint32_t cflags);
/* the main loop's lookup: G5T_E_RELOOP or G5T_E_LOOP; clears the mark */
int g5t_loop_entry(int cpu_index, vaddr pc, uint32_t flags, uint32_t cflags);

/* check that the guest nip matches the CTR / LR the branch used */
void g5t_kindchk(int cpu_index, int kind, bool ok);

/* jump cache clears, with the cause attributed */
void g5t_flush_jmp_cache(CPUState *cpu, int cause);
void g5t_jmp_cache_clear_page(CPUState *cpu, vaddr page_addr, int cause);
void g5t_jmp_cache_clear_slot(CPUState *cpu, unsigned slot,
                              TranslationBlock *tb);
/* the bit counts an async TLB flush added to 'info jit' partial / elided */
void g5t_abits(int cpu_index, int pbits, int ebits);

/* shared body of helper_lookup_tb_ptr (cpu-exec.c) */
const void *g5t_lookup_tb_ptr(CPUState *cpu, int kind);

/* append the "G5TB ..." lines to @buf (a GString *) */
void g5t_dump(void *buf);

#endif
