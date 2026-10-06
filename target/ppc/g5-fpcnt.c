/*
 * qemu-g5 #387 (measurement only, not for upstream): see g5-fpcnt.h.
 *
 * Each row is written only by its own vCPU's thread, so plain increments
 * are enough; a write from another thread is counted in chk/thread first.
 * Counts are cumulative since start-up; the aggregator takes the
 * difference of two "info jit" snapshots around the workload.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "fpu/softfloat.h"
#include "hw/core/cpu.h"
#include "exec/g5-fpdump.h"
#include "g5-fpcnt.h"

#define G5FP_MAXCPU 8

bool g5_sf_force_soft_fma(void);

typedef struct G5FpRow {
    uint64_t entry;
    /* [helper][tier][x][o][e0][e1] */
    uint64_t op[G5FP_NH][G5FPE_NB][2][2][2][2];
    /* rn ve oe ue ze xe ni xx fx fe0 fe1: 12 bits */
    uint64_t hist[1 << 12];
    /* [kind][arg][pr][n, fi0, xx0, fx0] */
    uint64_t tr[G5FP_NTR][G5FP_TR_ARGS][2][4];
    /* [cause][fe off, on] */
    uint64_t excreq[16][2];
    /* [cause][ignore, deliver] */
    uint64_t exc[16][2];
    uint64_t chk_thread;
    uint64_t chk_rn_mismatch;
} QEMU_ALIGNED(64) G5FpRow;

static G5FpRow g5fp_rows[G5FP_MAXCPU];

static const struct {
    const char *name;
    bool single;
    G5FpKind kind;
    int path;
} g5fp_helpers[G5FP_NH] = {
#define G5FP_INFO(n, s, k, p) [G5FP_##n] = { #n, s, k, p },
    G5FP_HELPERS(G5FP_INFO)
#undef G5FP_INFO
};

static const char *const g5fp_tr_names[G5FP_NTR] = {
#define G5FP_TR_NAME(n) [G5FP_TR_##n] = #n,
    G5FP_TRS(G5FP_TR_NAME)
#undef G5FP_TR_NAME
};

static const char *const g5fp_tier_names[G5FPE_NB] = {
    "na", "rn", "pre", "post", "ok",
};

static G5FpRow *g5fp_row(CPUPPCState *env)
{
    CPUState *cs = env_cpu(env);
    G5FpRow *row = &g5fp_rows[cs->cpu_index % G5FP_MAXCPU];

    if (!qemu_cpu_is_self(cs)) {
        row->chk_thread++;
    }
    return row;
}

void __attribute__((noinline)) g5fp_entry(CPUPPCState *env)
{
    G5FpRow *row = g5fp_row(env);
    uint64_t f = env->fpscr;
    unsigned bin;
    bool rne = get_float_rounding_mode(&env->fp_status) ==
               float_round_nearest_even;

    row->entry++;
    bin = (f & FP_RN) |
          !!(f & FP_VE) << 2 | !!(f & FP_OE) << 3 | !!(f & FP_UE) << 4 |
          !!(f & FP_ZE) << 5 | !!(f & FP_XE) << 6 | !!(f & FP_NI) << 7 |
          !!(f & FP_XX) << 8 | !!(f & FP_FX) << 9 |
          FIELD_EX64(env->msr, MSR, FE0) << 10 |
          FIELD_EX64(env->msr, MSR, FE1) << 11;
    row->hist[bin]++;
    if (rne != ((f & FP_RN) == 0)) {
        row->chk_rn_mismatch++;
    }
}

void __attribute__((noinline)) g5fp_op(CPUPPCState *env, int h, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t r, uint64_t mid)
{
    G5FpRow *row = g5fp_row(env);
    uint64_t f = env->fpscr;
    bool rne = get_float_rounding_mode(&env->fp_status) ==
               float_round_nearest_even;
    bool fsf = g5_sf_force_soft_fma();
    G5FpKind k = g5fp_helpers[h].kind;
    bool x = false;
    G5FpTier t;

    switch (g5fp_helpers[h].path) {
    case G5FP_PATH:
    case G5FP_CMP:
        t = g5fpe_tier64(k, rne, fsf, a, b, c, r, mid);
        break;
    case G5FP_H2:
        t = g5fpe_tier32(k, rne, fsf, a, b, c, r, mid);
        switch (k) {
        case G5FPK_FMA:
            x = g5fpe_f32_exact(a) && g5fpe_f32_exact(b) &&
                g5fpe_f32_exact(c);
            break;
        case G5FPK_SQRT:
            x = g5fpe_f32_exact(a);
            break;
        case G5FPK_RSQRTE:
            x = g5fpe_f32_exact(mid);
            break;
        default:
            x = g5fpe_f32_exact(a) && g5fpe_f32_exact(b);
            break;
        }
        break;
    default:
        t = G5FPE_NA;
        break;
    }
    row->op[h][t][x][!!(f & (FP_OE | FP_UE))]
        [!(f & (FP_VE | FP_OE | FP_UE | FP_ZE | FP_XE))][!!(f & FP_XX)]++;
}

void HELPER(g5fp_tr)(CPUPPCState *env, uint32_t code)
{
    G5FpRow *row = g5fp_row(env);
    uint64_t f = env->fpscr;
    uint64_t *n = row->tr[(code >> 10) % G5FP_NTR]
                         [(code >> 1) & (G5FP_TR_ARGS - 1)][code & 1];

    n[0]++;
    n[1] += !(f & FP_FI);
    n[2] += !(f & FP_XX);
    n[3] += !(f & FP_FX);
}

void __attribute__((noinline)) g5fp_excreq(CPUPPCState *env, int cause, bool fe)
{
    g5fp_row(env)->excreq[cause & 0xf][fe]++;
}

void __attribute__((noinline)) g5fp_exc(CPUPPCState *env, int cause, bool deliver)
{
    g5fp_row(env)->exc[cause & 0xf][deliver]++;
}

static void g5fp_dump(GString *buf)
{
    for (int cpu = 0; cpu < G5FP_MAXCPU; cpu++) {
        G5FpRow *row = &g5fp_rows[cpu];

        g_string_append_printf(buf, "G5FP entry cpu=%d n=%" PRIu64 "\n",
                               cpu, row->entry);
        for (int h = 0; h < G5FP_NH; h++) {
            for (int t = 0; t < G5FPE_NB; t++) {
                for (int i = 0; i < 16; i++) {
                    uint64_t n = row->op[h][t][i >> 3][(i >> 2) & 1]
                                        [(i >> 1) & 1][i & 1];
                    if (n) {
                        g_string_append_printf(buf,
                            "G5FP op cpu=%d h=%s t=%s x=%d o=%d e0=%d e1=%d"
                            " n=%" PRIu64 "\n", cpu, g5fp_helpers[h].name,
                            g5fp_tier_names[t], i >> 3, (i >> 2) & 1,
                            (i >> 1) & 1, i & 1, n);
                    }
                }
            }
        }
        for (int bin = 0; bin < ARRAY_SIZE(row->hist); bin++) {
            if (row->hist[bin]) {
                g_string_append_printf(buf,
                    "G5FP hist cpu=%d rn=%d ve=%d oe=%d ue=%d ze=%d xe=%d"
                    " ni=%d xx=%d fx=%d fe0=%d fe1=%d n=%" PRIu64 "\n", cpu,
                    bin & 3, (bin >> 2) & 1, (bin >> 3) & 1, (bin >> 4) & 1,
                    (bin >> 5) & 1, (bin >> 6) & 1, (bin >> 7) & 1,
                    (bin >> 8) & 1, (bin >> 9) & 1, (bin >> 10) & 1,
                    (bin >> 11) & 1, row->hist[bin]);
            }
        }
        for (int k = 0; k < G5FP_NTR; k++) {
            for (int a = 0; a < G5FP_TR_ARGS; a++) {
                for (int pr = 0; pr < 2; pr++) {
                    uint64_t *n = row->tr[k][a][pr];
                    if (n[0]) {
                        g_string_append_printf(buf,
                            "G5FP tr cpu=%d k=%s a=%d pr=%d n=%" PRIu64
                            " fi0=%" PRIu64 " xx0=%" PRIu64 " fx0=%" PRIu64
                            "\n", cpu, g5fp_tr_names[k], a, pr, n[0], n[1],
                            n[2], n[3]);
                    }
                }
            }
        }
        for (int c = 0; c < 16; c++) {
            for (int i = 0; i < 2; i++) {
                if (row->excreq[c][i]) {
                    g_string_append_printf(buf,
                        "G5FP excreq cpu=%d cause=%d fe=%s n=%" PRIu64 "\n",
                        cpu, c, i ? "on" : "off", row->excreq[c][i]);
                }
                if (row->exc[c][i]) {
                    g_string_append_printf(buf,
                        "G5FP exc cpu=%d cause=%d out=%s n=%" PRIu64 "\n",
                        cpu, c, i ? "deliver" : "ignore", row->exc[c][i]);
                }
            }
        }
        g_string_append_printf(buf,
            "G5FP chk cpu=%d what=thread n=%" PRIu64 "\n"
            "G5FP chk cpu=%d what=rn_mismatch n=%" PRIu64 "\n",
            cpu, row->chk_thread, cpu, row->chk_rn_mismatch);
    }
    g_string_append_printf(buf, "G5FP meta force_soft_fma=%d\n",
                           g5_sf_force_soft_fma());
}

static void g5fp_dump_at_exit(void)
{
    g_autoptr(GString) buf = g_string_new("");

    g5fp_dump(buf);
    fputs(buf->str, stderr);
}

static void __attribute__((constructor)) g5fp_init(void)
{
    g5fp_dump_hook = g5fp_dump;
    if (getenv("G5FP_DUMP_AT_EXIT")) {
        atexit(g5fp_dump_at_exit);
    }
}
