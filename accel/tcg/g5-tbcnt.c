/*
 * qemu-g5 #368: throwaway counters that explain why tb_lookup() misses the
 * per-vCPU jump cache.  NOT FOR UPSTREAM.  See include/exec/g5-tbcnt.h.
 *
 * Every jump cache clear remembers, per slot, what it threw away (the
 * "shadow"): the cause, the TB pointer and tb_ctx.tb_flush_count.  The clear
 * paths only ever NULL the .tb field and leave .pc alone, so the stale pc of
 * an empty slot is the pc the TB had before the clear.  A later lookup that
 * finds the slot empty uses the shadow to tell "never filled" (a), "cleared
 * while holding the very key we are asking for" (b), "cleared while holding
 * some other key" (c) and "cleared before a tb_flush" (x).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "exec/g5-tbcnt.h"
#include "hw/core/cpu.h"
#include "exec/translation-block.h"
#include "tb-jmp-cache.h"
#include "tb-hash.h"
#include "tb-context.h"

QEMU_BUILD_BUG_ON(G5T_SHADOW_SIZE != TB_JMP_CACHE_SIZE);

G5TRow g5t_rows[G5T_MAXCPU];

/* Causes whose counters may be written by a thread other than the owner. */
static inline bool g5t_shared_cause(int cause)
{
    return cause == G5T_C_TBFLUSH || cause == G5T_C_PCREL ||
           cause == G5T_C_SLOT || cause == G5T_C_RESET ||
           cause == G5T_C_OTHER;
}

static inline void g5t_add(uint64_t *p, uint64_t v, bool shared)
{
    if (shared) {
        qatomic_add(p, v);
    } else {
        *p += v;
    }
}

TranslationBlock *g5t_lk_miss(int cpu_index, int entry, int kind,
                              unsigned slot, TranslationBlock *tb,
                              vaddr slot_pc, vaddr pc, uint64_t cs_base,
                              uint32_t flags, uint32_t cflags)
{
    G5TRow *r = g5t_row(cpu_index);
    TranslationBlock *ret = NULL;
    int kcol;

    if (tb) {
        if (slot_pc != pc) {
            r->lk[entry][G5T_R_CONFLICT]++;
            kcol = G5T_KR_CONFLICT;
        } else {
            if (tb->flags != flags) {
                r->lk[entry][G5T_R_MM_FLAGS]++;
            } else if (tb_cflags(tb) != cflags) {
                r->lk[entry][G5T_R_MM_CFLAGS]++;
            } else {
                r->lk[entry][G5T_R_MM_CSBASE]++;
            }
            kcol = G5T_KR_MISMATCH;
        }
    } else {
        G5TShadow *sh = &r->shadow[slot];

        if (!sh->cause) {
            r->lk[entry][G5T_R_COLD_A]++;
            kcol = G5T_KR_COLD_OTHER;
        } else if (sh->flush_count != qatomic_read(&tb_ctx.tb_flush_count)) {
            r->lk[entry][G5T_R_COLD_X]++;
            kcol = G5T_KR_COLD_OTHER;
        } else {
            TranslationBlock *stb = sh->tb;

            if (stb->pc == pc && stb->cs_base == cs_base &&
                stb->flags == flags && tb_cflags(stb) == cflags) {
                r->lk[entry][G5T_R_COLD_B]++;
                r->coldb[entry][sh->cause - 1]++;
                kcol = G5T_KR_COLD_B;
                ret = stb;
            } else {
                r->lk[entry][G5T_R_COLD_C]++;
                if (stb->pc == pc) {
                    r->lk[entry][G5T_R_COLD_C_PC]++;
                }
                kcol = G5T_KR_COLD_OTHER;
            }
        }
    }
    if (kind >= 0) {
        r->kind[kind][kcol]++;
    }
    return ret;
}

void g5t_helper_missed(int cpu_index, vaddr pc, uint32_t flags,
                       uint32_t cflags)
{
    G5TRow *r = g5t_row(cpu_index);

    r->relook_valid = true;
    r->relook_pc = pc;
    r->relook_flags = flags;
    r->relook_cflags = cflags;
}

int g5t_loop_entry(int cpu_index, vaddr pc, uint32_t flags, uint32_t cflags)
{
    G5TRow *r = g5t_row(cpu_index);
    int entry = G5T_E_LOOP;

    if (r->relook_valid) {
        if (r->relook_pc == pc && r->relook_flags == flags &&
            r->relook_cflags == cflags) {
            entry = G5T_E_RELOOP;
        }
        r->relook_valid = false;
    }
    return entry;
}

void g5t_kindchk(int cpu_index, int kind, bool ok)
{
    if (!ok) {
        g5t_row(cpu_index)->kindchk_bad[kind]++;
    }
}

/*
 * Clear @n slots from @i0, remembering in the shadow what each non-empty
 * slot held.  The NULL store is kept unconditional, as upstream.
 */
static void g5t_clear(int cpu_index, CPUJumpCache *jc, int i0, int n,
                      int cause)
{
    G5TRow *r = g5t_row(cpu_index);
    unsigned fc = qatomic_read(&tb_ctx.tb_flush_count);
    uint64_t nonnull = 0;

    for (int i = i0; i < i0 + n; i++) {
        TranslationBlock *tb = qatomic_read(&jc->array[i].tb);

        if (tb) {
            G5TShadow *sh = &r->shadow[i];

            sh->tb = tb;
            sh->flush_count = fc;
            sh->cause = cause + 1;
            nonnull++;
        }
        qatomic_set(&jc->array[i].tb, NULL);
    }
    g5t_add(&r->clr_events[cause], 1, g5t_shared_cause(cause));
    g5t_add(&r->clr_nonnull[cause], nonnull, g5t_shared_cause(cause));
}

void g5t_flush_jmp_cache(CPUState *cpu, int cause)
{
    CPUJumpCache *jc = cpu->tb_jmp_cache;

    /* During early initialization, the cache may not yet be allocated. */
    if (unlikely(jc == NULL)) {
        return;
    }
    g5t_clear(cpu->cpu_index, jc, 0, TB_JMP_CACHE_SIZE, cause);
}

void g5t_jmp_cache_clear_page(CPUState *cpu, vaddr page_addr, int cause)
{
    CPUJumpCache *jc = cpu->tb_jmp_cache;

    if (unlikely(!jc)) {
        return;
    }
    g5t_clear(cpu->cpu_index, jc, tb_jmp_cache_hash_page(page_addr),
              TB_JMP_PAGE_SIZE, cause);
}

/* tb_jmp_cache_inval_tb(): @cpu is not necessarily the calling thread */
void g5t_jmp_cache_clear_slot(CPUState *cpu, unsigned slot,
                              TranslationBlock *tb)
{
    CPUJumpCache *jc = cpu->tb_jmp_cache;

    if (qatomic_read(&jc->array[slot].tb) == tb) {
        G5TRow *r = g5t_row(cpu->cpu_index);
        G5TShadow *sh = &r->shadow[slot];

        sh->tb = tb;
        sh->flush_count = qatomic_read(&tb_ctx.tb_flush_count);
        sh->cause = G5T_C_SLOT + 1;
        /* publish the shadow before the lookup side can see the NULL */
        smp_wmb();
        qatomic_set(&jc->array[slot].tb, NULL);
        qatomic_add(&r->clr_events[G5T_C_SLOT], 1);
        qatomic_add(&r->clr_nonnull[G5T_C_SLOT], 1);
    }
}

void g5t_abits(int cpu_index, int pbits, int ebits)
{
    G5TRow *r = g5t_row(cpu_index);

    r->pbits += pbits;
    r->ebits += ebits;
}

static const char *const g5t_entry_names[G5T_E_N] = {
    "helper", "loop", "atomic", "reloop"
};
static const char *const g5t_res_names[G5T_R_N] = {
    "hit", "cold_a", "cold_b", "cold_c", "cold_c_pc", "cold_x", "conflict",
    "mm_flags", "mm_cflags", "mm_csbase"
};
static const char *const g5t_cause_names[G5T_C_N] = {
    "async_full", "async_part", "async_elide", "page", "range_all",
    "range_page", "tbflush", "pcrel", "slot", "reset", "other"
};
static const char *const g5t_kind_names[G5T_K_N] = {
    "lr", "ctr", "tar", "xpage_b", "xpage_bc_taken", "xpage_bc_fall",
    "pageend", "nogototb", "other", "untagged"
};
static const char *const g5t_qht_names[G5T_Q_N] = {
    "try", "hit", "nophys", "miss"
};
static const char *const g5t_gen_names[G5T_G_N] = {
    "enter", "new", "oneshot", "replaced"
};

void g5t_dump(void *opaque)
{
    GString *buf = opaque;
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        int c = cpu->cpu_index;
        const G5TRow *r = g5t_row(c);

        for (int e = 0; e < G5T_E_N; e++) {
            g_string_append_printf(buf,
                "G5TB lk cpu=%d entry=%s res=all n=%" PRIu64 "\n",
                c, g5t_entry_names[e], r->all[e]);
            for (int x = 0; x < G5T_R_N; x++) {
                g_string_append_printf(buf,
                    "G5TB lk cpu=%d entry=%s res=%s n=%" PRIu64 "\n",
                    c, g5t_entry_names[e], g5t_res_names[x], r->lk[e][x]);
            }
        }
        for (int e = 0; e < G5T_E_N; e++) {
            for (int x = 0; x < G5T_C_N; x++) {
                g_string_append_printf(buf,
                    "G5TB coldb cpu=%d entry=%s cause=%s n=%" PRIu64 "\n",
                    c, g5t_entry_names[e], g5t_cause_names[x],
                    r->coldb[e][x]);
            }
            g_string_append_printf(buf,
                "G5TB b1 cpu=%d entry=%s n=%" PRIu64 "\n",
                c, g5t_entry_names[e], r->b1[e]);
            for (int x = 0; x < G5T_Q_N; x++) {
                g_string_append_printf(buf,
                    "G5TB qht cpu=%d entry=%s res=%s n=%" PRIu64 "\n",
                    c, g5t_entry_names[e], g5t_qht_names[x], r->qht[e][x]);
            }
        }
        for (int x = 0; x < G5T_G_N; x++) {
            g_string_append_printf(buf,
                "G5TB gen cpu=%d res=%s n=%" PRIu64 "\n",
                c, g5t_gen_names[x], r->gen[x]);
        }
        g_string_append_printf(buf, "G5TB iorecomp cpu=%d n=%" PRIu64 "\n",
                               c, r->iorecomp);
        for (int x = 0; x < G5T_C_N; x++) {
            g_string_append_printf(buf,
                "G5TB clear cpu=%d cause=%s events=%" PRIu64
                " nonnull=%" PRIu64 "\n",
                c, g5t_cause_names[x], r->clr_events[x], r->clr_nonnull[x]);
        }
        g_string_append_printf(buf,
            "G5TB abits cpu=%d pbits=%" PRIu64 " ebits=%" PRIu64 "\n",
            c, r->pbits, r->ebits);
        for (int k = 0; k < G5T_K_N; k++) {
            const uint64_t *v = r->kind[k];

            g_string_append_printf(buf,
                "G5TB kind cpu=%d kind=%s calls=%" PRIu64 " hit=%" PRIu64
                " cold_b=%" PRIu64 " cold_other=%" PRIu64
                " conflict=%" PRIu64 " mismatch=%" PRIu64
                " qht_miss=%" PRIu64 "\n",
                c, g5t_kind_names[k], v[G5T_KR_CALLS], v[G5T_KR_HIT],
                v[G5T_KR_COLD_B], v[G5T_KR_COLD_OTHER], v[G5T_KR_CONFLICT],
                v[G5T_KR_MISMATCH], v[G5T_KR_QHT_MISS]);
        }
        for (int k = G5T_K_LR; k <= G5T_K_CTR; k++) {
            g_string_append_printf(buf,
                "G5TB kindchk cpu=%d kind=%s bad=%" PRIu64 "\n",
                c, g5t_kind_names[k], r->kindchk_bad[k]);
        }
    }
}
