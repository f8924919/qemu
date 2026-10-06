/*
 * qemu-g5 #385: per-vCPU sets of SLB values (see g5-slbset.h).
 * NOT FOR UPSTREAM.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "g5-slbset.h"

static bool val_eq(const G5SVal *a, const G5SVal *b)
{
    return a->esid == b->esid && a->vsid == b->vsid && a->sps == b->sps;
}

static void set_bind(G5SSet *set, G5SVal *buf, uint32_t cap)
{
    set->v = buf;
    set->n = 0;
    set->cap = cap;
    set->ovf = false;
    set->gen = 1;
    set->novf = 0;
    set->nclr = 0;
}

void g5s_init(G5SState *st)
{
    memset(st, 0, sizeof(*st));
    set_bind(&st->spp, st->spp_buf, G5S_CAP_SPP);
    set_bind(&st->sp, st->sp_buf, G5S_CAP_SP);
    set_bind(&st->s, st->s_buf, G5S_CAP_S);
}

void g5s_set_clear(G5SSet *set)
{
    set->n = 0;
    set->ovf = false;
    set->gen++;
    set->nclr++;
}

bool g5s_set_has(const G5SSet *set, const G5SVal *v)
{
    for (uint32_t i = 0; i < set->n; i++) {
        if (val_eq(&set->v[i], v)) {
            return true;
        }
    }
    return false;
}

bool g5s_set_add(G5SSet *set, const G5SVal *v)
{
    if (g5s_set_has(set, v)) {
        return true;
    }
    if (set->n >= set->cap) {
        if (!set->ovf) {
            set->ovf = true;
            set->novf++;
        }
        return false;
    }
    set->v[set->n++] = *v;
    return true;
}

int g5s_classify(const G5SSet *set, G5SLookup lk, void *opaque,
                 G5SVal *miss, uint32_t maxmiss, uint32_t *nmiss)
{
    uint32_t n = 0, kept = 0;

    if (nmiss) {
        *nmiss = 0;
    }
    if (set->ovf) {
        return G5S_C_OVF;
    }
    for (uint32_t i = 0; i < set->n; i++) {
        G5SVal got;

        if (lk(opaque, set->v[i].esid, &got) && val_eq(&got, &set->v[i])) {
            continue;
        }
        n++;
        if (miss && kept < maxmiss) {
            miss[kept++] = set->v[i];
        }
    }
    if (nmiss) {
        *nmiss = kept;
    }
    if (n == 0) {
        return G5S_C_OK;
    }
    return n >= 4 ? G5S_C_MISS4 : (int)n;
}

void g5s_fill(G5SState *st, int slot, const G5SVal *v)
{
    if (slot < 0 || slot >= G5S_NSLOT) {
        return;
    }
    if (st->mark_spp[slot] != st->spp.gen) {
        if (g5s_set_add(&st->spp, v)) {
            st->mark_spp[slot] = st->spp.gen;
        }
    }
    if (st->mark_sp[slot] != st->sp.gen) {
        if (g5s_set_add(&st->sp, v)) {
            st->mark_sp[slot] = st->sp.gen;
        }
    }
}

void g5s_store(G5SState *st, int slot, const G5SVal *v, bool valid,
               int writer)
{
    if (slot >= 0 && slot < G5S_NSLOT) {
        st->mark_spp[slot] = 0;
        st->mark_sp[slot] = 0;
    }
    if (!valid) {
        return;
    }
    g5s_set_add(&st->s, v);
    for (uint32_t i = 0; i < st->npend; i++) {
        if (val_eq(&st->pend[i].v, v)) {
            if (writer < 0 || writer >= G5S_NWRITER) {
                writer = G5S_W_OTHER;
            }
            st->late[writer]++;
            st->pend[i] = st->pend[--st->npend];
            return;
        }
    }
}

bool g5s_consume(G5SState *st, bool slbia_only, G5SLookup lk, void *opaque)
{
    uint32_t total;

    st->ncons++;
    st->cls_spp = g5s_classify(&st->spp, lk, opaque, st->miss, G5S_CAP_MISS,
                               &st->nmiss);
    st->cls_sp = g5s_classify(&st->sp, lk, opaque, NULL, 0, NULL);
    st->cls_s = g5s_classify(&st->s, lk, opaque, NULL, 0, NULL);
    st->skip = slbia_only && st->cls_spp == G5S_C_OK;

    /* missing values that did not fit in miss[] are lost */
    if (st->cls_spp != G5S_C_OVF) {
        total = 0;
        for (uint32_t i = 0; i < st->spp.n; i++) {
            G5SVal got;

            if (!(lk(opaque, st->spp.v[i].esid, &got) &&
                  val_eq(&got, &st->spp.v[i]))) {
                total++;
            }
        }
        if (total > st->nmiss) {
            st->pend_ovf += total - st->nmiss;
        }
    }
    return st->skip;
}

void g5s_after_flush(G5SState *st)
{
    for (uint32_t i = 0; i < st->nmiss; i++) {
        if (st->npend >= G5S_CAP_PEND) {
            st->pend_ovf++;
            continue;
        }
        st->pend[st->npend].v = st->miss[i];
        st->pend[st->npend].gen = st->ncons;
        st->npend++;
    }
    st->nmiss = 0;
}

void g5s_reset(G5SState *st, bool skippable, const G5SVal *valid,
               uint32_t nvalid)
{
    g5s_set_clear(&st->sp);
    g5s_set_clear(&st->s);
    for (uint32_t i = 0; i < nvalid; i++) {
        g5s_set_add(&st->s, &valid[i]);
    }
    if (skippable) {
        return;
    }
    g5s_set_clear(&st->spp);
    for (uint32_t i = 0; i < st->npend;) {
        if (st->pend[i].gen < st->ncons) {
            st->never++;
            st->pend[i] = st->pend[--st->npend];
        } else {
            i++;
        }
    }
}
