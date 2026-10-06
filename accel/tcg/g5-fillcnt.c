/*
 * qemu-g5 #373: throwaway counters for the reasons of TLB refills.
 * NOT FOR UPSTREAM.  See include/exec/g5-fillcnt.h.
 *
 * Every counter row g5l_rows[cpu_index] and every shadow structure behind it
 * is written only by the thread of that vCPU (flush works, refills and the
 * ppc hooks all run there), so plain increments suffice.  The monitor reads
 * them racily in g5l_dump, which is acceptable because snapshots are taken
 * with the VM stopped.  Each writer counts a violation of that rule in
 * chk/thread (qemu_cpu_is_self) and then writes anyway.
 *
 * Shadow record (per vCPU): a direct-mapped table keyed by (mmu_idx, page)
 * that keeps the translation the page had when it was last filled, the TLB
 * flush generation of that mmu_idx at that time, and a mark that is written
 * when the page is first killed by a single-page flush or by a victim TLB
 * overflow.  Full flushes only bump the generation and write one event into
 * a per-mmu_idx ring indexed by generation, so a refill that finds the
 * generation advanced looks up the first flush after its fill.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "exec/g5-fillcnt.h"
#include "exec/g5-flushcnt.h"
#include "exec/memattrs.h"
#include "exec/page-protection.h"
#include "hw/core/cpu.h"

#define G5L_MAXCPU      8
#define SHADOW_BITS     20
#define SEEN_BITS       27

__thread uint32_t g5l_caller;
__thread uint32_t g5l_flush_path;
__thread uint32_t g5l_flush_trig;
__thread uint32_t g5l_flush_wk;
__thread uint32_t g5l_mark_kind;
__thread uint64_t g5l_kill_page;
__thread int32_t g5l_slb_idx = G5L_SLB_UNSET;

static const char *const caller_names[G5L_NCALLER] = {
    "probe", "lookup1", "atomic_st", "atomic_ld"
};
static const char *const mmu_names[9] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "other"
};
/* indexed by MMUAccessType */
static const char *const acc_names[3] = { "read", "write", "inst" };
static const char *const trig_names[4] = {
    "slbia", "tlbie", "other", "unknown"
};
static const char *const path_names[G5L_NPATH] = { "async", "large", "range" };
static const char *const wkind_names[3] = { "full", "part", "elide" };
#ifndef G5L_NO_SHADOW
static const char *const cause_names[G5L_NCAUSE] = {
    "upgrade", "vflag_mmio", "vflag_discard", "vflag_watch", "vflag_notdirty",
    "vflag_other", "main_unclass", "full", "ring_lost", "large", "range_all",
    "range_page", "page", "conflict", "first", "ovf", "unclass"
};
static const char *const slb_names[6] = {
    "0", "ge1", "vrma", "real", "unset", "fail"
};
static const char *const res_names[4] = { "same", "prot", "diff", "fail" };
static const char *const wk_names[2] = { "full", "part" };
static const char *const g5r_cls_names[7] = {
    "ok", "miss1", "miss2", "miss3", "miss4", "ovf", "none"
};
static const char *const w_names[3] = { "gain", "lose", "none" };
#endif

#define SLBC_0      0
#define SLBC_GE1    1
#define SLBC_VRMA   2
#define SLBC_REAL   3
#define SLBC_UNSET  4
#define SLBC_FAIL   5

#define RES_SAME    0
#define RES_PROT    1
#define RES_DIFF    2
#define RES_FAIL    3

#ifndef G5L_NO_SHADOW
typedef struct Shadow {
    uint64_t key;       /* 0 = empty, else mkkey() */
    uint64_t phys;
    uint64_t attrs;
    uint32_t prot;
    uint32_t gen;       /* flush generation of the mmu_idx at fill time */
    uint8_t lg;
    uint8_t mark;       /* G5L_M_* */
    uint8_t acc;        /* access type of that fill */
} Shadow;

typedef struct RingEnt {
    uint32_t gen;
    uint8_t path;
    uint8_t trig;       /* 0 slbia, 1 tlbie, 2 other */
    uint8_t resize;
    uint8_t wk;         /* 0 full work, 1 partial work */
    uint8_t spcls;      /* #385: S' class of the consumption, G5R_SP_NONE */
} RingEnt;
#endif

typedef struct G5LRow {
    uint64_t enter[G5L_NCALLER][9][3];
    uint64_t ok;
    uint64_t cause[G5L_NCAUSE][4];      /* n same prot diff */
    uint64_t fullk[2][2];
    uint64_t ffull[4][6][4];
    uint64_t ffullr[4][6][4][2][7];     /* #385: + skip, S' class */
    uint64_t protx[G5L_NCAUSE][3][3][3];
    uint64_t vflag[256];
    uint64_t ev[G5L_NPATH];
    uint64_t work[3][3];
    uint64_t pbits, ebits;
    uint64_t chk_slb_unset, chk_thread;
    uint64_t ref_vhit, ref_resize, ref_sec_hash, ref_slbmte0;
    bool used;
    bool slbdump_active, slbdump_done;
#ifndef G5L_NO_SHADOW
    Shadow *shadow;
    uint64_t *seen;
    RingEnt *ring[NB_MMU_MODES];
    uint32_t gen[NB_MMU_MODES];
    uint32_t lastkeep[NB_MMU_MODES];    /* #385: gen of the last unskippable */
#endif
} QEMU_ALIGNED(64) G5LRow;

static G5LRow g5l_rows[G5L_MAXCPU];

/* State carried from g5l_enter to g5l_ok (or the next g5l_enter). */
static __thread struct {
    bool open;
    int cause;
    int trig;
    int acc;
    int row;
    int skip;           /* #385: 1 = no unskippable flush since the fill */
    int sp;             /* #385: S' class of the first flush after it */
} st;

__thread bool g5r_cur_valid;
__thread bool g5r_cur_skip;
__thread uint8_t g5r_cur_sp;
void (*g5r_reset_hook)(CPUState *cpu, uint32_t asked);
void (*g5r_dump_hook)(void *buf);

static GOnce g5l_once = G_ONCE_INIT;
#ifndef G5L_NO_SHADOW
static unsigned ring_bits = 16;
#endif
static unsigned slbdump_n;
static unsigned dump_calls;
static bool slbdump_armed;

static gpointer g5l_init(gpointer unused)
{
    const char *s;

#ifndef G5L_NO_SHADOW
    s = getenv("G5L_RING_BITS");
    if (s) {
        unsigned long v;

        if (!qemu_strtoul(s, NULL, 0, &v) && v >= 4 && v <= 24) {
            ring_bits = v;
        }
    }
#endif
    s = getenv("G5L_SLBDUMP");
    if (s) {
        unsigned long v;

        if (!qemu_strtoul(s, NULL, 0, &v)) {
            slbdump_n = v;
        }
    }
    return NULL;
}

static inline int row_idx(CPUState *cpu)
{
    return cpu->cpu_index & (G5L_MAXCPU - 1);
}

#ifndef G5L_NO_SHADOW
static inline uint64_t mkkey(uint64_t page, int mmu_idx)
{
    return (((page >> 12) << 5) | (mmu_idx & 31)) + 1;
}

static inline uint64_t shadow_slot(uint64_t key)
{
    return (key * 0x9E3779B97F4A7C15ull) >> (64 - SHADOW_BITS);
}

/* Deliberately a different multiplier from shadow_slot(). */
static inline uint64_t seen_slot(uint64_t key)
{
    return (key * 0xD6E8FEB86659FD93ull) >> (64 - SEEN_BITS);
}
#endif

static G5LRow *row_get(CPUState *cpu)
{
    G5LRow *r = &g5l_rows[row_idx(cpu)];

    if (!r->used) {
        g_once(&g5l_once, g5l_init, NULL);
        r->used = true;
    }
#ifndef G5L_NO_SHADOW
    if (!r->shadow) {
        r->shadow = g_malloc0((size_t)sizeof(Shadow) << SHADOW_BITS);
        r->seen = g_malloc0((size_t)1 << (SEEN_BITS - 3));
    }
#endif
    return r;
}

static inline void thread_chk(G5LRow *r, CPUState *cpu)
{
    if (!qemu_cpu_is_self(cpu)) {
        r->chk_thread++;
    }
}

uint32_t g5l_fold_trig(uint32_t tag)
{
    if (tag & G5F_TAG_VALID) {
        uint32_t set = tag & 0x3f;

        if (set & G5F_T_TLBIE) {
            return 1;
        }
        if (set == G5F_T_SLBIA) {
            return 0;
        }
    }
    return 2;
}

void g5l_enter(CPUState *cpu, uint64_t page, int mmu_idx, int acc, int pre,
               unsigned vbits)
{
    G5LRow *r = row_get(cpu);
    int cause = pre;
    int trig = 3;

    thread_chk(r, cpu);
    g5l_slb_idx = G5L_SLB_UNSET;

#ifndef G5L_NO_SHADOW
    if (st.open) {
        /* the previous refill of this thread never reached its success */
        G5LRow *pr = &g5l_rows[st.row];

        if (st.cause == G5L_CA_FULL || st.cause == G5L_CA_RING_LOST) {
            pr->ffull[st.trig][SLBC_FAIL][RES_FAIL]++;
            pr->ffullr[st.trig][SLBC_FAIL][RES_FAIL][st.skip][st.sp]++;
        }
    }
#endif
    st.open = false;

    r->enter[g5l_caller & (G5L_NCALLER - 1)][mmu_idx < 8 ? mmu_idx : 8][acc]++;

#ifndef G5L_NO_SHADOW
    if (pre == G5L_PRE_NONE) {
        uint64_t key = mkkey(page, mmu_idx);
        Shadow *s = &r->shadow[shadow_slot(key)];

        st.skip = 0;
        st.sp = G5R_SP_NONE;
        if (s->key == key) {
            /* #385: no unskippable flush of this mmu_idx since the fill */
            st.skip = r->lastkeep[mmu_idx] <= s->gen;
            if (s->mark) {
                cause = s->mark == G5L_M_PAGE ? G5L_CA_PAGE :
                        s->mark == G5L_M_RANGE ? G5L_CA_RANGE_PAGE :
                        G5L_CA_CONFLICT;
            } else if (r->gen[mmu_idx] != s->gen) {
                RingEnt *ring = r->ring[mmu_idx];
                uint32_t g1 = s->gen + 1;
                RingEnt *e = ring ? &ring[g1 & ((1u << ring_bits) - 1)] : NULL;

                if (e && e->gen == g1) {
                    switch (e->path) {
                    case G5L_P_ASYNC:
                        cause = G5L_CA_FULL;
                        trig = e->trig;
                        st.sp = e->spcls;
                        r->fullk[e->resize][e->wk]++;
                        break;
                    case G5L_P_LARGE:
                        cause = G5L_CA_LARGE;
                        break;
                    default:
                        cause = G5L_CA_RANGE_ALL;
                        break;
                    }
                } else {
                    cause = G5L_CA_RING_LOST;
                }
            } else {
                cause = G5L_CA_UNCLASS;
            }
        } else {
            bool seen = r->seen[seen_slot(key) >> 6] &
                        (1ull << (seen_slot(key) & 63));

            cause = seen ? G5L_CA_OVF : G5L_CA_FIRST;
        }
    }
    r->cause[cause][0]++;
    if (cause >= G5L_CA_VFLAG_MMIO && cause <= G5L_CA_VFLAG_OTHER) {
        r->vflag[vbits & 0xff]++;
    }
#endif
    st.open = true;
    st.cause = cause;
    st.trig = trig;
    st.acc = acc;
    st.row = row_idx(cpu);
}

void g5l_ok(CPUState *cpu, int mmu_idx, uint64_t page, uint64_t phys,
            int prot, const void *attrs, unsigned attrs_size, int lg_page_size)
{
    G5LRow *r = row_get(cpu);
    bool counted = st.open;
    int slbc = SLBC_UNSET;

    thread_chk(r, cpu);
    if (counted) {
        r->ok++;
        if (g5l_slb_idx == G5L_SLB_VRMA) {
            slbc = SLBC_VRMA;
        } else if (mmu_idx & 2) {
            slbc = SLBC_REAL;
        } else if (g5l_slb_idx == G5L_SLB_UNSET) {
            slbc = SLBC_UNSET;
            r->chk_slb_unset++;
        } else {
            slbc = g5l_slb_idx == 0 ? SLBC_0 : SLBC_GE1;
        }
    }

#ifndef G5L_NO_SHADOW
    {
        uint64_t key = mkkey(page, mmu_idx);
        Shadow *s = &r->shadow[shadow_slot(key)];
        uint64_t aw = 0;
        uint64_t sl = seen_slot(key);

        memcpy(&aw, attrs, MIN(attrs_size, sizeof(aw)));

        if (counted) {
            int c = st.cause;
            bool shadowable = c <= G5L_CA_VFLAG_OTHER ||
                              (c >= G5L_CA_FULL && c <= G5L_CA_CONFLICT);
            int res = RES_DIFF;

            if (shadowable) {
                /*
                 * No record of the translation before (overwritten by a
                 * collision) counts as "diff": only upgrade and vflag_*
                 * can get here without a matching record.
                 */
                if (s->key == key) {
                    if (s->phys == phys && s->attrs == aw &&
                        s->lg == lg_page_size) {
                        res = s->prot == prot ? RES_SAME : RES_PROT;
                    }
                }
                r->cause[c][1 + res]++;
                if (res == RES_PROT) {
                    bool had = s->prot & PAGE_WRITE;
                    bool has = prot & PAGE_WRITE;
                    int w = has && !had ? 0 : (!has && had ? 1 : 2);

                    r->protx[c][s->acc][st.acc][w]++;
                }
            }
            if (c == G5L_CA_FULL || c == G5L_CA_RING_LOST) {
                r->ffull[st.trig][slbc][res]++;
                r->ffullr[st.trig][slbc][res][st.skip][st.sp]++;
            }
        }

        s->key = key;
        s->phys = phys;
        s->attrs = aw;
        s->prot = prot;
        s->gen = r->gen[mmu_idx];
        s->lg = lg_page_size;
        s->mark = G5L_M_NONE;
        s->acc = counted ? st.acc : 0;
        r->seen[sl >> 6] |= 1ull << (sl & 63);
    }
#else
    (void)slbc;
#endif
    st.open = false;
}

void g5l_flush_event(CPUState *cpu, int mmu_idx, bool resized)
{
    G5LRow *r = row_get(cpu);

    thread_chk(r, cpu);
    r->ev[g5l_flush_path]++;
    if (resized) {
        r->ref_resize++;
    }
#ifndef G5L_NO_SHADOW
    {
        uint32_t g = ++r->gen[mmu_idx];
        RingEnt *e;

        if (!r->ring[mmu_idx]) {
            r->ring[mmu_idx] = g_malloc0(sizeof(RingEnt) << ring_bits);
        }
        e = &r->ring[mmu_idx][g & ((1u << ring_bits) - 1)];
        e->gen = g;
        e->path = g5l_flush_path;
        e->trig = g5l_flush_trig;
        e->resize = resized;
        e->wk = g5l_flush_wk;
        e->spcls = g5r_cur_valid ? g5r_cur_sp : G5R_SP_NONE;
        if (!(g5r_cur_valid && g5r_cur_skip)) {
            r->lastkeep[mmu_idx] = g;
        }
    }
#endif
}

void g5l_mark(CPUState *cpu, int mmu_idx, uint64_t page, int kind)
{
#ifndef G5L_NO_SHADOW
    G5LRow *r;
    uint64_t key;
    Shadow *s;

    if (kind == G5L_M_NONE) {
        return;
    }
    r = row_get(cpu);
    thread_chk(r, cpu);
    key = mkkey(page, mmu_idx);
    s = &r->shadow[shadow_slot(key)];
    if (s->key == key && s->mark == G5L_M_NONE) {
        s->mark = kind;
    }
#endif
}

void g5l_work(CPUState *cpu, uint32_t tag, int kind, int pbits, int ebits)
{
    G5LRow *r = row_get(cpu);

    thread_chk(r, cpu);
    r->work[g5l_fold_trig(tag)][kind]++;
    r->pbits += pbits;
    r->ebits += ebits;
}

void g5l_vhit(CPUState *cpu)
{
    G5LRow *r = row_get(cpu);

    thread_chk(r, cpu);
    r->ref_vhit++;
}

void g5l_sec_hash(CPUState *cpu)
{
    G5LRow *r = row_get(cpu);

    thread_chk(r, cpu);
    r->ref_sec_hash++;
}

void g5l_slbmte0(CPUState *cpu)
{
    G5LRow *r = row_get(cpu);

    thread_chk(r, cpu);
    r->ref_slbmte0++;
}

bool g5l_slbdump_begin(CPUState *cpu)
{
    G5LRow *r = &g5l_rows[row_idx(cpu)];

    if (slbdump_armed && !r->slbdump_done) {
        r->slbdump_active = true;
        return true;
    }
    return false;
}

bool g5l_slbdump_end(CPUState *cpu)
{
    G5LRow *r = &g5l_rows[row_idx(cpu)];

    if (r->slbdump_active) {
        r->slbdump_active = false;
        r->slbdump_done = true;
        return true;
    }
    return false;
}

void g5l_slbdump_line(CPUState *cpu, bool before, int idx, uint64_t esid,
                      uint64_t vsid)
{
    fprintf(stderr, "G5LSLB cpu=%d when=%s idx=%d esid=0x%016" PRIx64
            " vsid=0x%016" PRIx64 "\n", cpu->cpu_index,
            before ? "before" : "after", idx, esid, vsid);
}

void g5l_dump(void *opaque)
{
    GString *buf = opaque;

    g_once(&g5l_once, g5l_init, NULL);
    if (slbdump_n && ++dump_calls == slbdump_n) {
        slbdump_armed = true;
    }

    for (int c = 0; c < G5L_MAXCPU; c++) {
        G5LRow *r = &g5l_rows[c];

        if (!r->used) {
            continue;
        }
        for (int k = 0; k < G5L_NCALLER; k++) {
            for (int m = 0; m < 9; m++) {
                for (int a = 0; a < 3; a++) {
                    g_string_append_printf(buf,
                        "G5L enter cpu=%d caller=%s mmu=%s acc=%s n=%" PRIu64
                        "\n", c, caller_names[k], mmu_names[m], acc_names[a],
                        r->enter[k][m][a]);
                }
            }
        }
        g_string_append_printf(buf, "G5L ok cpu=%d n=%" PRIu64 "\n", c, r->ok);
#ifndef G5L_NO_SHADOW
        for (int k = 0; k < G5L_NCAUSE; k++) {
            g_string_append_printf(buf,
                "G5L cause cpu=%d cause=%s n=%" PRIu64 " same=%" PRIu64
                " prot=%" PRIu64 " diff=%" PRIu64 "\n", c, cause_names[k],
                r->cause[k][0], r->cause[k][1], r->cause[k][2],
                r->cause[k][3]);
        }
        for (int rs = 0; rs < 2; rs++) {
            for (int w = 0; w < 2; w++) {
                g_string_append_printf(buf,
                    "G5L fullk cpu=%d rs=%d wk=%s n=%" PRIu64 "\n", c, rs,
                    wk_names[w], r->fullk[rs][w]);
            }
        }
        for (int t = 0; t < 4; t++) {
            for (int s = 0; s < 6; s++) {
                for (int x = 0; x < 4; x++) {
                    g_string_append_printf(buf,
                        "G5L ffull cpu=%d trig=%s slb=%s res=%s n=%" PRIu64
                        "\n", c, trig_names[t], slb_names[s], res_names[x],
                        r->ffull[t][s][x]);
                    for (int k = 0; k < 2; k++) {
                        for (int p = 0; p < 7; p++) {
                            if (r->ffullr[t][s][x][k][p]) {
                                g_string_append_printf(buf,
                                    "G5R ffullr cpu=%d trig=%s slb=%s res=%s"
                                    " skip=%d sp=%s n=%" PRIu64 "\n", c,
                                    trig_names[t], slb_names[s], res_names[x],
                                    k, g5r_cls_names[p],
                                    r->ffullr[t][s][x][k][p]);
                            }
                        }
                    }
                }
            }
        }
        for (int k = 0; k < G5L_NCAUSE; k++) {
            for (int p = 0; p < 3; p++) {
                for (int q = 0; q < 3; q++) {
                    for (int w = 0; w < 3; w++) {
                        if (r->protx[k][p][q][w]) {
                            g_string_append_printf(buf,
                                "G5L protx cpu=%d cause=%s prev=%s cur=%s w=%s"
                                " n=%" PRIu64 "\n", c, cause_names[k],
                                acc_names[p], acc_names[q], w_names[w],
                                r->protx[k][p][q][w]);
                        }
                    }
                }
            }
        }
        for (int b = 0; b < 256; b++) {
            if (r->vflag[b]) {
                g_string_append_printf(buf,
                    "G5L vflag cpu=%d bits=0x%x n=%" PRIu64 "\n", c, b,
                    r->vflag[b]);
            }
        }
#endif
        for (int p = 0; p < G5L_NPATH; p++) {
            g_string_append_printf(buf, "G5L ev cpu=%d path=%s n=%" PRIu64
                                   "\n", c, path_names[p], r->ev[p]);
        }
        for (int t = 0; t < 3; t++) {
            for (int k = 0; k < 3; k++) {
                g_string_append_printf(buf,
                    "G5L work cpu=%d trig=%s kind=%s n=%" PRIu64 "\n", c,
                    trig_names[t], wkind_names[k], r->work[t][k]);
            }
        }
        g_string_append_printf(buf,
            "G5L abits cpu=%d pbits=%" PRIu64 " ebits=%" PRIu64 "\n", c,
            r->pbits, r->ebits);
        g_string_append_printf(buf,
            "G5L chk cpu=%d name=slb_unset n=%" PRIu64 "\n", c,
            r->chk_slb_unset);
        g_string_append_printf(buf,
            "G5L chk cpu=%d name=thread n=%" PRIu64 "\n", c, r->chk_thread);
        g_string_append_printf(buf,
            "G5L ref cpu=%d name=vhit n=%" PRIu64 "\n", c, r->ref_vhit);
        g_string_append_printf(buf,
            "G5L ref cpu=%d name=resize n=%" PRIu64 "\n", c, r->ref_resize);
        g_string_append_printf(buf,
            "G5L ref cpu=%d name=sec_hash n=%" PRIu64 "\n", c,
            r->ref_sec_hash);
        g_string_append_printf(buf,
            "G5L ref cpu=%d name=slbmte0 n=%" PRIu64 "\n", c,
            r->ref_slbmte0);
    }
}
