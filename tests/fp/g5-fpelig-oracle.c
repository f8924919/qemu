/*
 * qemu-g5 #387 (measurement only, not for upstream): check the hardfloat
 * eligibility predicates of target/ppc/g5-fpelig.c against softfloat.
 *
 * fpu/softfloat.c is built into this program with -DG5_SF_ORACLE, which
 * makes it record in g5_sf_path whether an operation returned a hardfloat
 * result (G5_SF_HARD) or went back to softfloat after computing one
 * (G5_SF_POST).  For every operation and every combination of inputs and
 * rounding modes, the operation is run twice:
 *
 *  - with the flags cleared, as target/ppc runs it, to get the result the
 *    predicate is given;
 *  - with the inexact flag already set, to see which path softfloat takes.
 *
 * The predicate's verdict must match the path taken.  Exits non-zero and
 * prints the first mismatches if it does not.
 */
#ifndef HW_POISON_H
#error Must define HW_POISON_H to work around TARGET_* poisoning
#endif

#include "qemu/osdep.h"
#include <float.h>
#include <math.h>
#include "fpu/softfloat.h"
#include "../../target/ppc/g5-fpelig.h"

extern int g5_sf_path;
extern bool g5_sf_force_soft_fma(void);

enum { G5_SF_NONE, G5_SF_HARD, G5_SF_POST };

static const FloatRoundMode modes[] = {
    float_round_nearest_even, float_round_to_zero,
    float_round_up, float_round_down,
};

static uint64_t d2u(double d)
{
    union { double d; uint64_t u; } x = { .d = d };
    return x.u;
}

static double u2d(uint64_t u)
{
    union { double d; uint64_t u; } x = { .u = u };
    return x.d;
}

static uint64_t vals[64];
static int nvals;

static void add(double d)
{
    vals[nvals++] = d2u(d);
    vals[nvals++] = d2u(-d);
}

static void init_vals(void)
{
    add(0.0);
    add(1.0);
    add(0.5);
    add(1.5);
    add(3.0);
    add(1.0 / 3.0);                     /* not a single */
    add(DBL_MIN);
    add(DBL_MIN * 2);
    add(DBL_MIN * 1.5);
    add(DBL_MIN / 2);                   /* double denormal */
    add(FLT_MIN);
    add(FLT_MIN * 2);
    add((double)FLT_MIN / 2);           /* single denormal, double normal */
    add(1e300);
    add(DBL_MAX);
    add(1e39);                          /* above FLT_MAX */
    add(INFINITY);
    vals[nvals++] = 0x7ff8000000000000ull;   /* qNaN */
    vals[nvals++] = 0x7ff4000000000000ull;   /* sNaN */
    g_assert(nvals <= ARRAY_SIZE(vals));
}

static int fails, checks;

static void status(float_status *s, FloatRoundMode m, bool inexact)
{
    memset(s, 0, sizeof(*s));
    set_float_rounding_mode(m, s);
    set_float_exception_flags(inexact ? float_flag_inexact : 0, s);
    set_float_2nan_prop_rule(float_2nan_prop_ab, s);
    set_float_3nan_prop_rule(float_3nan_prop_acb, s);
    set_float_infzeronan_rule(float_infzeronan_dnan_never, s);
    set_float_default_nan_pattern(0b01000000, s);
}

/* What the predicate should say, given the path softfloat took. */
static G5FpTier seen(FloatRoundMode m, int path)
{
    if (m != float_round_nearest_even) {
        return G5FPE_RN;
    }
    switch (path) {
    case G5_SF_HARD:
        return G5FPE_OK;
    case G5_SF_POST:
        return G5FPE_POST;
    default:
        return G5FPE_PRE;
    }
}

/* How often each verdict came up, per operation: all must be exercised. */
#define NWHAT 32
static const char *whats[NWHAT];
static int tally[NWHAT][G5FPE_NB];

static void count(const char *what, G5FpTier want)
{
    int i;

    for (i = 0; i < NWHAT && whats[i] && strcmp(whats[i], what); i++) {
        continue;
    }
    g_assert(i < NWHAT);
    whats[i] = what;
    tally[i][want]++;
}

static void check(const char *what, FloatRoundMode m, uint64_t a, uint64_t b,
                  uint64_t c, G5FpTier want, G5FpTier got)
{
    checks++;
    count(what, want);
    if (want != got && fails++ < 20) {
        fprintf(stderr, "%s rm=%d a=%016" PRIx64 " b=%016" PRIx64
                " c=%016" PRIx64 ": softfloat %d predicate %d\n",
                what, m, a, b, c, want, got);
    }
}

typedef float64 (*op2_64)(float64, float64, float_status *);
typedef float32 (*op2_32)(float32, float32, float_status *);

static void run2(const char *what, const char *what32, G5FpKind k, op2_64 f64, op2_32 f32,
                 op2_64 f64r32)
{
    bool fsf = g5_sf_force_soft_fma();

    for (int m = 0; m < ARRAY_SIZE(modes); m++) {
        for (int i = 0; i < nvals; i++) {
            for (int j = 0; j < nvals; j++) {
                uint64_t a = vals[i], b = vals[j], r;
                float_status s;
                int path;

                status(&s, modes[m], false);
                r = f64(a, b, &s);
                status(&s, modes[m], true);
                g5_sf_path = G5_SF_NONE;
                f64(a, b, &s);
                path = g5_sf_path;
                check(what, modes[m], a, b, 0, seen(modes[m], path),
                      g5fpe_tier64(k, modes[m] == float_round_nearest_even,
                                   fsf, a, b, 0, r, 0));

                /* Single precision: float32 hardfloat on exact inputs. */
                status(&s, modes[m], false);
                r = f64r32(a, b, &s);
                if (g5fpe_f32_exact(a) && g5fpe_f32_exact(b)) {
                    float32 a32 = float64_to_float32(a, &s);
                    float32 b32 = float64_to_float32(b, &s);
                    status(&s, modes[m], true);
                    g5_sf_path = G5_SF_NONE;
                    f32(a32, b32, &s);
                    path = seen(modes[m], g5_sf_path);
                } else {
                    path = G5FPE_NA;
                }
                check(what32, modes[m], a, b, 1, path,
                      g5fpe_tier32(k, modes[m] == float_round_nearest_even,
                                   fsf, a, b, 0, r, 0));
            }
        }
    }
}

static void run_fma(void)
{
    static const int negs[] = {
        0, float_muladd_negate_c, float_muladd_negate_result,
        float_muladd_negate_c | float_muladd_negate_result,
    };
    bool fsf = g5_sf_force_soft_fma();

    for (int m = 0; m < ARRAY_SIZE(modes); m++) {
        bool rne = modes[m] == float_round_nearest_even;

        for (int n = 0; n < ARRAY_SIZE(negs); n++) {
            for (int i = 0; i < nvals; i++) {
                for (int j = 0; j < nvals; j++) {
                    for (int l = 0; l < nvals; l++) {
                        uint64_t a = vals[i], b = vals[j], c = vals[l], r;
                        float_status s;
                        int want;

                        status(&s, modes[m], false);
                        r = float64_muladd(a, b, c, negs[n], &s);
                        status(&s, modes[m], true);
                        g5_sf_path = G5_SF_NONE;
                        float64_muladd(a, b, c, negs[n], &s);
                        check("fma", modes[m], a, b, c,
                              seen(modes[m], g5_sf_path),
                              g5fpe_tier64(G5FPK_FMA, rne, fsf, a, b, c, r,
                                           0));

                        status(&s, modes[m], false);
                        r = float64r32_muladd(a, b, c, negs[n], &s);
                        if (g5fpe_f32_exact(a) && g5fpe_f32_exact(b) &&
                            g5fpe_f32_exact(c)) {
                            float32 a32 = float64_to_float32(a, &s);
                            float32 b32 = float64_to_float32(b, &s);
                            float32 c32 = float64_to_float32(c, &s);
                            status(&s, modes[m], true);
                            g5_sf_path = G5_SF_NONE;
                            float32_muladd(a32, b32, c32, negs[n], &s);
                            want = seen(modes[m], g5_sf_path);
                        } else {
                            want = G5FPE_NA;
                        }
                        check("fmas", modes[m], a, b, c, want,
                              g5fpe_tier32(G5FPK_FMA, rne, fsf, a, b, c, r,
                                           0));
                    }
                }
            }
        }
    }
}

static void run1(void)
{
    for (int m = 0; m < ARRAY_SIZE(modes); m++) {
        bool rne = modes[m] == float_round_nearest_even;

        for (int i = 0; i < nvals; i++) {
            uint64_t a = vals[i], r, s1, r2;
            float_status s;
            int p1, p2, want;

            /* sqrt */
            status(&s, modes[m], false);
            r = float64_sqrt(a, &s);
            status(&s, modes[m], true);
            g5_sf_path = G5_SF_NONE;
            float64_sqrt(a, &s);
            check("sqrt", modes[m], a, 0, 0, seen(modes[m], g5_sf_path),
                  g5fpe_tier64(G5FPK_SQRT, rne, false, a, 0, 0, r, 0));

            status(&s, modes[m], false);
            r = float64r32_sqrt(a, &s);
            if (g5fpe_f32_exact(a)) {
                float32 a32 = float64_to_float32(a, &s);
                status(&s, modes[m], true);
                g5_sf_path = G5_SF_NONE;
                float32_sqrt(a32, &s);
                want = seen(modes[m], g5_sf_path);
            } else {
                want = G5FPE_NA;
            }
            check("sqrts", modes[m], a, 0, 0, want,
                  g5fpe_tier32(G5FPK_SQRT, rne, false, a, 0, 0, r, 0));

            /* frsqrte: sqrt then 1 / s; hard only if both are. */
            status(&s, modes[m], false);
            s1 = float64_sqrt(a, &s);
            r2 = float64_div(float64_one, s1, &s);
            status(&s, modes[m], true);
            g5_sf_path = G5_SF_NONE;
            float64_sqrt(a, &s);
            p1 = g5_sf_path;
            g5_sf_path = G5_SF_NONE;
            float64_div(float64_one, s1, &s);
            p2 = g5_sf_path;
            if (p1 != G5_SF_HARD) {
                want = seen(modes[m], G5_SF_NONE);
            } else {
                want = seen(modes[m], p2);
            }
            check("rsqrte", modes[m], a, 0, 0, want,
                  g5fpe_tier64(G5FPK_RSQRTE, rne, false, a, 0, 0, r2, s1));

            /*
             * frsqrtes: double sqrt, then a single division 1 / s, a
             * float32 candidate only if s is a single.
             */
            status(&s, modes[m], false);
            s1 = float64_sqrt(a, &s);
            r2 = float64r32_div(float64_one, s1, &s);
            if (modes[m] != float_round_nearest_even) {
                want = G5FPE_RN;
            } else if (p1 != G5_SF_HARD) {
                want = G5FPE_PRE;
            } else if (!g5fpe_f32_exact(s1)) {
                want = G5FPE_NA;
            } else {
                float32 s32 = float64_to_float32(s1, &s);
                status(&s, modes[m], true);
                g5_sf_path = G5_SF_NONE;
                float32_div(float32_one, s32, &s);
                want = seen(modes[m], g5_sf_path);
            }
            check("rsqrtes", modes[m], a, 0, 0, want,
                  g5fpe_tier32(G5FPK_RSQRTE, rne, false, a, 0, 0, r2, s1));

            /* fre: 1 / a */
            status(&s, modes[m], false);
            r = float64_div(float64_one, a, &s);
            status(&s, modes[m], true);
            g5_sf_path = G5_SF_NONE;
            float64_div(float64_one, a, &s);
            check("fre", modes[m], a, 0, 0, seen(modes[m], g5_sf_path),
                  g5fpe_tier64(G5FPK_DIV, rne, false, float64_one, a, 0, r,
                               0));

            /* compare: no rounding mode, no flags */
            for (int j = 0; j < nvals; j++) {
                uint64_t b = vals[j];

                status(&s, modes[m], false);
                g5_sf_path = G5_SF_NONE;
                float64_compare(a, b, &s);
                check("cmp", modes[m], a, b, 0,
                      g5_sf_path == G5_SF_HARD ? G5FPE_OK : G5FPE_PRE,
                      g5fpe_tier64(G5FPK_CMP, rne, false, a, b, 0, 0, 0));
            }
        }
    }
}

static void run_itof(void)
{
    static const int64_t ints[] = {
        0, 1, -1, 3, INT64_MAX, INT64_MIN, (1ll << 53) + 1, 12345678901ll,
    };

    for (int m = 0; m < ARRAY_SIZE(modes); m++) {
        bool rne = modes[m] == float_round_nearest_even;

        for (int i = 0; i < ARRAY_SIZE(ints); i++) {
            float_status s;

            status(&s, modes[m], true);
            g5_sf_path = G5_SF_NONE;
            int64_to_float64(ints[i], &s);
            check("itof64", modes[m], ints[i], 0, 0,
                  seen(modes[m], g5_sf_path),
                  g5fpe_tier64(G5FPK_ITOF, rne, false, ints[i], 0, 0, 0, 0));
            g5_sf_path = G5_SF_NONE;
            uint64_to_float64(ints[i], &s);
            check("utof64", modes[m], ints[i], 0, 0,
                  seen(modes[m], g5_sf_path),
                  g5fpe_tier64(G5FPK_ITOF, rne, false, ints[i], 0, 0, 0, 0));
            g5_sf_path = G5_SF_NONE;
            int64_to_float32(ints[i], &s);
            check("itof32", modes[m], ints[i], 0, 0,
                  seen(modes[m], g5_sf_path),
                  g5fpe_tier64(G5FPK_ITOF, rne, false, ints[i], 0, 0, 0, 0));
            g5_sf_path = G5_SF_NONE;
            uint64_to_float32(ints[i], &s);
            check("utof32", modes[m], ints[i], 0, 0,
                  seen(modes[m], g5_sf_path),
                  g5fpe_tier64(G5FPK_ITOF, rne, false, ints[i], 0, 0, 0, 0));
        }
    }
}

static void run_exact(void)
{
    /* exact as a single: values, signs, NaN */
    static const struct { double d; bool exact; } t[] = {
        { 0.0, true }, { -0.0, true }, { 1.0, true }, { 1.0 / 3.0, false },
        { FLT_MAX, true }, { 1e39, false }, { FLT_MIN, true },
        { (double)FLT_MIN / 2, true }, { DBL_MIN, false },
        { INFINITY, true }, { -INFINITY, true },
        { 16777217.0, false }, { 16777216.0, true },
    };

    for (int i = 0; i < ARRAY_SIZE(t); i++) {
        checks++;
        if (g5fpe_f32_exact(d2u(t[i].d)) != t[i].exact && fails++ < 20) {
            fprintf(stderr, "f32_exact(%g) != %d\n", t[i].d, t[i].exact);
        }
    }
    checks++;
    if (g5fpe_f32_exact(0x7ff8000000000000ull) && fails++ < 20) {
        fprintf(stderr, "f32_exact(NaN) is true\n");
    }
    (void)u2d;
}

int main(void)
{
    init_vals();
    run2("add", "adds", G5FPK_ADDSUB, float64_add, float32_add, float64r32_add);
    run2("sub", "subs", G5FPK_ADDSUB, float64_sub, float32_sub, float64r32_sub);
    run2("mul", "muls", G5FPK_MUL, float64_mul, float32_mul, float64r32_mul);
    run2("div", "divs", G5FPK_DIV, float64_div, float32_div, float64r32_div);
    run_fma();
    run1();
    run_itof();
    run_exact();
    /*
     * Every operation must have met each verdict it can give: otherwise
     * the inputs above do not reach that branch of the predicate.
     */
    for (int i = 0; i < NWHAT && whats[i]; i++) {
        static const char *const tn[] = { "na", "rn", "pre", "post", "ok" };
        const char *w = whats[i];
        bool itof = strlen(w) > 3 && !strncmp(w + 1, "tof", 3);
        bool cmp = !strcmp(w, "cmp");
        bool no_post = cmp || itof || !strcmp(w, "sqrt") ||
            !strcmp(w, "sqrts") || !strcmp(w, "rsqrte") ||
            !strcmp(w, "rsqrtes");

        printf("%-8s", w);
        for (int t = 0; t < G5FPE_NB; t++) {
            printf(" %s=%d", tn[t], tally[i][t]);
        }
        printf("\n");
        if (tally[i][G5FPE_OK] == 0 ||
            (!cmp && tally[i][G5FPE_RN] == 0) ||
            (!itof && tally[i][G5FPE_PRE] == 0) ||
            (!no_post && tally[i][G5FPE_POST] == 0)) {
            fprintf(stderr, "%s: a verdict is never exercised\n", w);
            fails++;
        }
    }
    printf("g5-fpelig-oracle: force_soft_fma=%d checks=%d fails=%d\n",
           g5_sf_force_soft_fma(), checks, fails);
    return fails ? 1 : 0;
}
