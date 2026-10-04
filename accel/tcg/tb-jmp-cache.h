/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#include "qemu/rcu.h"
#include "exec/cpu-common.h"

#define TB_JMP_CACHE_BITS 12
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 *
 * A TLB flush does not empty the cache.  It clears 'checked' instead:
 * an entry whose bit is clear may have been filled under a mapping that
 * no longer holds, so its TB is checked against the current translation
 * of pc, exactly like a QHT lookup would, before it is used again.
 * 'checked' is only touched by the owning CPU (or while it is stopped).
 */
typedef struct CPUJumpCache {
    struct rcu_head rcu;
    struct {
        TranslationBlock *tb;
        vaddr pc;
    } array[TB_JMP_CACHE_SIZE];
    uint64_t checked[TB_JMP_CACHE_SIZE / 64];
} CPUJumpCache;

static inline bool tb_jmp_cache_is_checked(const CPUJumpCache *jc,
                                           uint32_t hash)
{
    return jc->checked[hash / 64] & (1ull << (hash % 64));
}

static inline void tb_jmp_cache_set_checked(CPUJumpCache *jc, uint32_t hash)
{
    jc->checked[hash / 64] |= 1ull << (hash % 64);
}

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
