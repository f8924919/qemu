/*
 * qemu-g5 #364/#369: throwaway TLB flush attribution counters.  NOT FOR UPSTREAM.
 * See include/exec/g5-flushcnt.h for the tag layout.
 *
 * Each counter row is written only by the vCPU thread it is indexed by
 * (pending sets, requests, coalescing) or by the vCPU that executes the
 * flush work (work counters), so plain increments suffice; the monitor
 * reads them racily, which is acceptable because snapshots are taken with
 * the VM stopped.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "exec/g5-flushcnt.h"

__thread uint32_t g5f_cur_tag;
__thread uint32_t g5f_cur_entry;
__thread uint32_t g5f_store_caller;

static uint32_t pend_l[G5F_MAXCPU], pend_g[G5F_MAXCPU];
static uint64_t ntlbie_l[G5F_MAXCPU], ntlbie_g[G5F_MAXCPU];
static uint64_t req[G5F_MAXCPU][G5F_NTRIG];
static uint64_t coal[G5F_MAXCPU][2][G5F_COAL_MAX];
static uint64_t work[G5F_MAXCPU][G5F_NTAG][5];  /* full part elide pbits ebits */
static uint64_t rb_seen[G5F_MAXCPU];
static uint64_t rb_ring[G5F_MAXCPU][G5F_RB_RING];
static uint64_t rb_n[G5F_MAXCPU];
static uint64_t store_n[G5F_MAXCPU][3][2];      /* [caller] total ow */
static uint64_t slbia_n[G5F_MAXCPU][2];         /* total empty */
static const char *const store_names[3] = { "other", "slbmte", "mtsr" };

static inline int idx(int cpu)
{
    return cpu & (G5F_MAXCPU - 1);
}

void g5f_arm(int cpu, uint32_t trig)
{
    int c = idx(cpu);

    pend_l[c] |= trig;
    pend_g[c] |= trig & G5F_T_GLOBAL;   /* #369: only global requests */
    for (int i = 0; i < G5F_NTRIG; i++) {
        if (trig & (1u << i)) {
            req[c][i]++;
        }
    }
    if (trig & G5F_T_TLBIE) {
        ntlbie_l[c]++;
        ntlbie_g[c]++;
    }
}

void g5f_tlbie_rb(int cpu, uint64_t rb)
{
    int c = idx(cpu);

    if (rb_seen[c]++ % G5F_RB_EVERY == 0) {
        rb_ring[c][rb_n[c]++ % G5F_RB_RING] = rb;
    }
}

uint32_t g5f_take(int cpu, int global, uint32_t entry, uint32_t extra_trig)
{
    int c = idx(cpu);
    uint32_t set;
    uint64_t n;

    if (global) {
        set = pend_l[c] | pend_g[c];
        n = ntlbie_g[c];
        pend_l[c] = pend_g[c] = 0;
        ntlbie_l[c] = ntlbie_g[c] = 0;
    } else {
        set = pend_l[c];
        n = ntlbie_l[c];
        pend_l[c] = 0;
        ntlbie_l[c] = 0;
    }
    set |= extra_trig;
    for (int i = 0; i < G5F_NTRIG; i++) {
        if (extra_trig & (1u << i)) {
            req[c][i]++;
        }
    }
    coal[c][global ? 1 : 0][MIN(n, G5F_COAL_MAX - 1)]++;
    return G5F_TAG_VALID | (set & 0x1ff) | ((entry & 7) << 9) | (c << 12);
}

void g5f_store(int cpu, int ow)
{
    int c = idx(cpu);
    uint32_t caller = g5f_store_caller < 3 ? g5f_store_caller : G5F_S_OTHER;

    store_n[c][caller][0]++;
    if (ow) {
        store_n[c][caller][1]++;
        g5f_arm(cpu, G5F_T_SLBOW);
    }
}

void g5f_slbia(int cpu, int empty)
{
    int c = idx(cpu);

    slbia_n[c][0]++;
    if (empty) {
        slbia_n[c][1]++;
    } else {
        g5f_arm(cpu, G5F_T_SLBIAINV);
    }
}

void g5f_work(int cpu, uint32_t tag, int kind, int pbits, int ebits)
{
    uint64_t *w = work[idx(cpu)][tag & (G5F_NTAG - 1)];

    w[kind]++;
    w[3] += pbits;
    w[4] += ebits;
}

void g5f_dump(void *opaque)
{
    GString *buf = opaque;

    for (int c = 0; c < G5F_MAXCPU; c++) {
        uint64_t any = rb_seen[c];

        for (int i = 0; i < G5F_NTRIG; i++) {
            any |= req[c][i];
        }
        if (any) {
            g_string_append_printf(buf,
                "G5FLUSH req cpu=%d tlbie=%" PRIu64 " tlbieg=%" PRIu64
                " slbie=%" PRIu64 " slbia=%" PRIu64 " sdr1=%" PRIu64
                " reset=%" PRIu64 " slbiainv=%" PRIu64 " slbow=%" PRIu64
                " hpte=%" PRIu64 "\n", c, req[c][0], req[c][1], req[c][2],
                req[c][3], req[c][4], req[c][5], req[c][6], req[c][7],
                req[c][8]);
        }
        for (int k = 0; k < 3; k++) {
            if (store_n[c][k][0]) {
                g_string_append_printf(buf,
                    "G5FLUSH store cpu=%d caller=%s total=%" PRIu64
                    " ow=%" PRIu64 "\n", c, store_names[k],
                    store_n[c][k][0], store_n[c][k][1]);
            }
        }
        if (slbia_n[c][0]) {
            g_string_append_printf(buf,
                "G5FLUSH slbia cpu=%d total=%" PRIu64 " empty=%" PRIu64 "\n",
                c, slbia_n[c][0], slbia_n[c][1]);
        }
        for (int g = 0; g < 2; g++) {
            for (int n = 0; n < G5F_COAL_MAX; n++) {
                if (coal[c][g][n]) {
                    g_string_append_printf(buf,
                        "G5FLUSH coal cpu=%d scope=%s ntlbie=%d count=%" PRIu64
                        "\n", c, g ? "global" : "local", n, coal[c][g][n]);
                }
            }
        }
        for (int t = 0; t < G5F_NTAG; t++) {
            uint64_t *w = work[c][t];

            if (w[0] | w[1] | w[2]) {
                g_string_append_printf(buf,
                    "G5FLUSH work cpu=%d valid=%d origin=%d entry=%d "
                    "trig=0x%03x full=%" PRIu64 " part=%" PRIu64
                    " elide=%" PRIu64 " pbits=%" PRIu64 " ebits=%" PRIu64
                    "\n", c, !!(t & G5F_TAG_VALID),
                    (t >> 12) & 7, (t >> 9) & 7, t & 0x1ff, w[0], w[1], w[2],
                    w[3], w[4]);
            }
        }
        if (rb_seen[c]) {
            uint64_t k = MIN(rb_n[c], G5F_RB_RING);

            g_string_append_printf(buf, "G5FLUSH rbseen cpu=%d seen=%" PRIu64
                                   " sampled=%" PRIu64 "\n",
                                   c, rb_seen[c], rb_n[c]);
            for (uint64_t i = 0; i < k; i++) {
                g_string_append_printf(buf, "G5FLUSH rb cpu=%d rb=0x%016"
                                       PRIx64 "\n", c, rb_ring[c][i]);
            }
        }
    }
}
