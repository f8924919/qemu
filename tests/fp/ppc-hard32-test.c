/*
 * Differential test for target/ppc/fpu_hard32.h: the float32 hardfloat
 * path of the PowerPC single-precision scalar FP helpers must return the
 * same result bits and raise the same softfloat flags as float64r32_*().
 *
 * Every case runs float64r32_*() and ppc_hard32_*() (falling back to
 * float64r32_*() when the host path is not taken) from identical
 * float_status and compares the result and the whole exception flag
 * word.  Cases are tallied by input partition: partitions built to take
 * the host path must take it at least once under round-to-nearest-even,
 * and partitions built to stay on softfloat must never take it.
 *
 * Usage: ppc-hard32-test [-n random-cases-per-op-and-rounding] [seed...]
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include <math.h>
#include "fpu/softfloat.h"
#include "../../target/ppc/fpu_hard32.h"

enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MADD, OP_MSUB, OP_NMADD, OP_NMSUB,
       N_OPS };

static const char *const op_name[N_OPS] = {
    "fadds", "fsubs", "fmuls", "fdivs", "fmadds", "fmsubs", "fnmadds",
    "fnmsubs",
};

/* as target/ppc/fpu_helper.c MADD_FLGS ... NMSUB_FLGS */
static const int madd_flags[N_OPS] = {
    [OP_MADD] = 0,
    [OP_MSUB] = float_muladd_negate_c,
    [OP_NMADD] = float_muladd_negate_result,
    [OP_NMSUB] = float_muladd_negate_c | float_muladd_negate_result,
};

#define N_RM 4
static const FloatRoundMode rmodes[N_RM] = {
    float_round_nearest_even, float_round_to_zero, float_round_up,
    float_round_down,
};
static const char *const rm_name[N_RM] = { "rne", "rz", "rup", "rdown" };

/*
 * Input partitions.  The first group is built to take the host path
 * under round-to-nearest-even; the second is built so that it never
 * may (wrong rounding mode or a result softfloat must produce).
 */
enum {
    P_RANDOM,       /* float32-exact zero/normal inputs */
    P_ALIGNED,      /* cancellation, near-reciprocals, FMA residuals */
    P_EXACT_ZERO,   /* the exact result is zero */
    P_FLTMAX_OK,    /* inexact just above FLT_MAX, rounds to FLT_MAX */
    P_BOUNDARY,     /* every combination of the boundary values */
    P_FIRST_NEVER,
    P_INEXACT_LOW = P_FIRST_NEVER, /* an input with low 29 bits set */
    P_EXP_HIGH,     /* an input above the float32 range */
    P_EXP_LOW,      /* an input below the float32 normal range */
    P_NAN_INF,      /* an input is a NaN or an infinity */
    P_TINY,         /* nonzero result at or below FLT_MIN */
    P_FLTMIN_UP,    /* exact result just below FLT_MIN, rounds to it */
    P_UNDER_ZERO,   /* nonzero exact result that rounds to zero */
    P_OVERFLOW,     /* result overflows */
    P_MIDPOINT,     /* FMA: the double sum is a float32 midpoint */
    P_DIVZERO,      /* divisor is zero */
    N_PARTS
};

static const char *const part_name[N_PARTS] = {
    "random", "aligned", "exact-zero", "fltmax-ok", "boundary",
    "inexact-low", "exp-high", "exp-low", "nan-inf", "tiny", "fltmin-up",
    "under-zero", "overflow", "midpoint", "divzero",
};

/* Boundary partition mixes both kinds: only "taken at least once". */
static bool part_must_take(int part, int rm)
{
    return rm == 0 && part < P_FIRST_NEVER;
}

static bool part_may_take(int part, int rm)
{
    return rm == 0 && part < P_FIRST_NEVER;
}

typedef struct {
    uint64_t n, taken;
} Tally;

static Tally tally[N_PARTS][N_OPS][N_RM];
static uint64_t mismatches, untouched_failures, generator_failures;

/*
 * The generators are checked against softfloat's own answer under
 * round-to-nearest-even, so that a partition really holds the cases it
 * is named after.
 */
static bool part_is_sound(int part, float64 want, int fl)
{
    float64 mag = float64_abs(want);
    float64 fmin = make_float64(0x3810000000000000ULL);   /* FLT_MIN */
    float64 fmax = make_float64(0x47efffffe0000000ULL);   /* FLT_MAX */

    switch (part) {
    case P_EXACT_ZERO:
        return float64_is_zero(want) && fl == 0;
    case P_FLTMAX_OK:
        return float64_val(mag) == float64_val(fmax) &&
               fl == float_flag_inexact;
    case P_TINY:
        return !float64_is_zero(want) &&
               float64_val(mag) < float64_val(fmin);
    case P_FLTMIN_UP:
        return float64_val(mag) == float64_val(fmin);
    case P_UNDER_ZERO:
        return float64_is_zero(want) && (fl & float_flag_underflow);
    case P_OVERFLOW:
        return fl & float_flag_overflow;
    case P_DIVZERO:
        return fl & (float_flag_divbyzero | float_flag_invalid);
    default:
        return true;
    }
}

/* xorshift64* */
static uint64_t rng_state;

static uint64_t rnd(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

static uint64_t rnd_below(uint64_t n)
{
    return rnd() % n;
}

static float64 d2f64(double d)
{
    union { double d; uint64_t u; } x = { .d = d };
    return make_float64(x.u);
}

static float64 f2f64(float f)
{
    return d2f64(f);
}

static float u2f(uint32_t u)
{
    union { float f; uint32_t u; } x = { .u = u };
    return x.f;
}

static uint32_t f2u(float f)
{
    union { float f; uint32_t u; } x = { .f = f };
    return x.u;
}

/* float32 with the given biased exponent (1..254) and random mantissa */
static float rnd_f32_exp(int bexp)
{
    uint32_t sign = (uint32_t)(rnd() & 1) << 31;
    uint32_t frac = rnd() & 0x7fffff;

    return u2f(sign | ((uint32_t)bexp << 23) | frac);
}

/* zero or normal float32, mostly in a range that keeps results normal */
static float rnd_f32(void)
{
    uint64_t k = rnd_below(64);

    if (k == 0) {
        return (rnd() & 1) ? -0.0f : 0.0f;
    }
    if (k < 16) {
        return rnd_f32_exp(1 + rnd_below(254));
    }
    return rnd_f32_exp(127 - 40 + rnd_below(81));
}

/* float32 with few mantissa bits, so products of two are float32-exact */
static float rnd_f32_short(void)
{
    uint32_t sign = (uint32_t)(rnd() & 1) << 31;
    uint32_t frac = (rnd() & 0x7ff) << 12;
    int bexp = 127 - 20 + rnd_below(41);

    return u2f(sign | ((uint32_t)bexp << 23) | frac);
}

static float64 ref_op(int op, float64 a, float64 b, float64 c,
                      float_status *s)
{
    switch (op) {
    case OP_ADD:
        return float64r32_add(a, b, s);
    case OP_SUB:
        return float64r32_sub(a, b, s);
    case OP_MUL:
        return float64r32_mul(a, b, s);
    case OP_DIV:
        return float64r32_div(a, b, s);
    default:
        return float64r32_muladd(a, b, c, madd_flags[op], s);
    }
}

static bool hard_op(int op, float64 a, float64 b, float64 c, float64 *r,
                    float_status *s)
{
    switch (op) {
    case OP_ADD:
        return ppc_hard32_addsub(a, b, false, r, s);
    case OP_SUB:
        return ppc_hard32_addsub(a, b, true, r, s);
    case OP_MUL:
        return ppc_hard32_mul(a, b, r, s);
    case OP_DIV:
        return ppc_hard32_div(a, b, r, s);
    default:
        return ppc_hard32_muladd(a, b, c, madd_flags[op], r, s);
    }
}

static float64 wrap_op(int op, float64 a, float64 b, float64 c,
                       float_status *s)
{
    switch (op) {
    case OP_ADD:
        return ppc_f64r32_add(a, b, s);
    case OP_SUB:
        return ppc_f64r32_sub(a, b, s);
    case OP_MUL:
        return ppc_f64r32_mul(a, b, s);
    case OP_DIV:
        return ppc_f64r32_div(a, b, s);
    default:
        return ppc_f64r32_muladd(a, b, c, madd_flags[op], s);
    }
}

/* As target/ppc/cpu_init.c ppc_cpu_reset_hold() and ppc_store_fpscr(). */
static void ppc_status(float_status *s, int rm, int rebias)
{
    memset(s, 0, sizeof(*s));
    set_float_detect_tininess(float_tininess_before_rounding, s);
    set_float_ftz_detection(float_ftz_before_rounding, s);
    set_float_2nan_prop_rule(float_2nan_prop_ab, s);
    set_float_3nan_prop_rule(float_3nan_prop_acb, s);
    set_float_infzeronan_rule(float_infzeronan_dnan_never, s);
    set_float_default_nan_pattern(0b01000000, s);
    set_float_rounding_mode(rmodes[rm], s);
    set_float_rebias_overflow(rebias & 1, s);
    set_float_rebias_underflow(rebias & 2, s);
}

static void check(int part, int op, float64 a, float64 b, float64 c)
{
    for (int rm = 0; rm < N_RM; rm++) {
        float_status s_ref, s_hard, s_wrap;
        int rebias = rnd_below(4);
        float64 want, got, wrapped;
        int want_fl, got_fl, wrap_fl;
        bool taken;

        ppc_status(&s_ref, rm, rebias);
        ppc_status(&s_hard, rm, rebias);
        ppc_status(&s_wrap, rm, rebias);

        want = ref_op(op, a, b, c, &s_ref);
        want_fl = get_float_exception_flags(&s_ref);

        taken = hard_op(op, a, b, c, &got, &s_hard);
        if (!taken) {
            if (get_float_exception_flags(&s_hard) != 0) {
                untouched_failures++;
            }
            got = ref_op(op, a, b, c, &s_hard);
        }
        got_fl = get_float_exception_flags(&s_hard);

        wrapped = wrap_op(op, a, b, c, &s_wrap);
        wrap_fl = get_float_exception_flags(&s_wrap);

        if (rm == 0 && rebias == 0 && !part_is_sound(part, want, want_fl)) {
            if (generator_failures++ < 20) {
                printf("GENERATOR %s %s a=%016" PRIx64 " b=%016" PRIx64
                       " c=%016" PRIx64 " soft=%016" PRIx64 "/0x%x\n",
                       part_name[part], op_name[op], float64_val(a),
                       float64_val(b), float64_val(c), float64_val(want),
                       want_fl);
            }
        }
        tally[part][op][rm].n++;
        tally[part][op][rm].taken += taken;

        if (float64_val(want) != float64_val(got) || want_fl != got_fl ||
            float64_val(want) != float64_val(wrapped) || want_fl != wrap_fl) {
            if (mismatches++ < 20) {
                printf("MISMATCH %s %s %s rebias=%d taken=%d "
                       "a=%016" PRIx64 " b=%016" PRIx64 " c=%016" PRIx64
                       " soft=%016" PRIx64 "/0x%x hard=%016" PRIx64 "/0x%x"
                       " wrap=%016" PRIx64 "/0x%x\n",
                       part_name[part], op_name[op], rm_name[rm], rebias,
                       taken, float64_val(a), float64_val(b),
                       float64_val(c), float64_val(want), want_fl,
                       float64_val(got), got_fl, float64_val(wrapped),
                       wrap_fl);
            }
        }
    }
}

static bool is_fma(int op)
{
    return op >= OP_MADD;
}

/* Replace one of the inputs used by @op with @v. */
static void put_one(int op, float64 *a, float64 *b, float64 *c, float64 v)
{
    switch (rnd_below(is_fma(op) ? 3 : 2)) {
    case 0:
        *a = v;
        break;
    case 1:
        *b = v;
        break;
    default:
        *c = v;
        break;
    }
}

static void gen_random(int op, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        check(P_RANDOM, op, f2f64(rnd_f32()), f2f64(rnd_f32()),
              f2f64(rnd_f32()));
    }
}

static void gen_aligned(int op, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        float a = rnd_f32_exp(127 - 20 + rnd_below(41));
        double da = a;
        float b, c = rnd_f32();
        int k = (int)rnd_below(64) - 32;

        switch (op) {
        case OP_ADD:
        case OP_SUB: {
            /* same or near exponent: cancellation and carries */
            int shift = rnd_below(28);
            float near = nextafterf(a, (rnd() & 1) ? INFINITY : -INFINITY);

            b = ldexpf(near, -shift);
            if (rnd() & 1) {
                b = ldexpf(a, -shift) + (float)k * ldexpf(FLT_EPSILON,
                                                          -shift);
            }
            if (rnd() & 1) {
                b = -b;
            }
            break;
        }
        case OP_MUL:
            b = u2f(f2u(1.0f / a) + k);
            break;
        case OP_DIV:
            b = (rnd() & 1) ? a : (float)(da * (1.0 + ldexp(k, -23)));
            break;
        default: {
            /* c cancels most of the product: exact or tiny residuals */
            float bb = rnd_f32_exp(127 - 20 + rnd_below(41));
            float prod = a * bb;

            b = bb;
            c = u2f(f2u(prod) + k);
            if (op == OP_MADD || op == OP_NMADD) {
                c = -c;
            }
            break;
        }
        }
        check(P_ALIGNED, op, f2f64(a), f2f64(b), f2f64(c));
    }
}

static void gen_exact_zero(int op, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        float a = rnd_f32(), b, c = 0.0f;
        float z = (rnd() & 1) ? -0.0f : 0.0f;

        switch (op) {
        case OP_ADD:
        case OP_SUB:
            if (rnd_below(4) == 0) {
                a = z;
                b = (rnd() & 1) ? -0.0f : 0.0f;
            } else {
                b = op == OP_ADD ? -a : a;
            }
            break;
        case OP_MUL:
            b = a;
            if (rnd() & 1) {
                a = z;
            } else {
                b = z;
            }
            break;
        case OP_DIV:
            b = a == 0 ? 1.0f : a;
            a = z;
            break;
        default:
            if (rnd() & 1) {
                /* product of short mantissas is float32-exact */
                a = rnd_f32_short();
                b = rnd_f32_short();
                c = a * b;
                if (op == OP_MADD || op == OP_NMADD) {
                    c = -c;
                }
            } else {
                /* 0 * x + (+-0) */
                b = (rnd() & 1) ? z : a;
                a = (rnd() & 1) ? z : -z;
                c = (rnd() & 1) ? 0.0f : -0.0f;
            }
            break;
        }
        check(P_EXACT_ZERO, op, f2f64(a), f2f64(b), f2f64(c));
    }
}

static void gen_fltmax_ok(int op, uint64_t n)
{
    if (op == OP_MUL || op == OP_DIV) {
        return;
    }
    for (uint64_t i = 0; i < n; i++) {
        /* below half an ulp of FLT_MAX (2^103): rounds to FLT_MAX */
        float small = rnd_f32_exp(127 + 60 + rnd_below(43));
        float big = FLT_MAX;
        float a = big, b = fabsf(small), c = 0.0f;

        if (rnd() & 1) {
            a = -a;
            b = -b;
        }
        switch (op) {
        case OP_ADD:
            break;
        case OP_SUB:
            b = -b;
            break;
        default:
            c = b;
            b = 1.0f;
            if (op == OP_MSUB || op == OP_NMSUB) {
                c = -c;
            }
            break;
        }
        check(P_FLTMAX_OK, op, f2f64(a), f2f64(b), f2f64(c));
    }
}

static float64 boundary[64];
static int n_boundary;

static void add_boundary(float64 v)
{
    boundary[n_boundary++] = v;
    boundary[n_boundary++] = make_float64(float64_val(v) ^ (1ULL << 63));
}

static void init_boundary(void)
{
    add_boundary(d2f64(0.0));
    add_boundary(f2f64(u2f(0x00000001)));       /* float32 denormal min */
    add_boundary(f2f64(u2f(0x007fffff)));       /* float32 denormal max */
    add_boundary(f2f64(FLT_MIN));
    add_boundary(f2f64(nextafterf(FLT_MIN, 1)));
    add_boundary(d2f64(1.0));
    add_boundary(f2f64(nextafterf(1.0f, 2)));
    add_boundary(f2f64(nextafterf(1.0f, 0)));
    add_boundary(d2f64(3.0));
    add_boundary(f2f64(FLT_MAX));
    add_boundary(f2f64(nextafterf(FLT_MAX, 0)));
    add_boundary(d2f64(INFINITY));
    add_boundary(make_float64(0x7ff8000000000000ULL)); /* qNaN */
    add_boundary(make_float64(0x7ff8000012340000ULL)); /* qNaN payload */
    add_boundary(make_float64(0x7ff0000020000000ULL)); /* sNaN */
    add_boundary(d2f64(1.0 + ldexp(1.0, -30)));    /* not float32 */
    add_boundary(d2f64(ldexp(1.0, -140)));         /* float32 denormal */
    add_boundary(d2f64(ldexp(1.0, -160)));         /* below float32 */
    add_boundary(d2f64(ldexp(1.0, 130)));          /* above float32 */
}

static void gen_boundary(int op)
{
    for (int i = 0; i < n_boundary; i++) {
        for (int j = 0; j < n_boundary; j++) {
            if (!is_fma(op)) {
                check(P_BOUNDARY, op, boundary[i], boundary[j], d2f64(0));
                continue;
            }
            for (int k = 0; k < n_boundary; k++) {
                check(P_BOUNDARY, op, boundary[i], boundary[j], boundary[k]);
            }
        }
    }
}

static void gen_inexact(int op, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        float64 a = f2f64(rnd_f32()), b = f2f64(rnd_f32());
        float64 c = f2f64(rnd_f32());
        double v = rnd_f32_exp(127 - 20 + rnd_below(41));
        uint64_t low = rnd() & 0x1fffffff;

        put_one(op, &a, &b, &c,
                make_float64(float64_val(d2f64(v)) | (low ? low : 1)));
        check(P_INEXACT_LOW, op, a, b, c);
    }
}

static void gen_exp_out(int op, uint64_t n, bool high)
{
    for (uint64_t i = 0; i < n; i++) {
        float64 a = f2f64(rnd_f32()), b = f2f64(rnd_f32());
        float64 c = f2f64(rnd_f32());
        double v = ldexp(1.0 + ldexp(rnd_below(1 << 23), -23),
                         high ? 128 + (int)rnd_below(200)
                              : -127 - (int)rnd_below(200));

        put_one(op, &a, &b, &c, d2f64((rnd() & 1) ? -v : v));
        check(high ? P_EXP_HIGH : P_EXP_LOW, op, a, b, c);
    }
}

static void gen_nan_inf(int op, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        float64 a = f2f64(rnd_f32()), b = f2f64(rnd_f32());
        float64 c = f2f64(rnd_f32());
        uint64_t v;

        switch (rnd_below(3)) {
        case 0:
            v = 0x7ff0000000000000ULL;                      /* inf */
            break;
        case 1:
            v = 0x7ff8000000000000ULL | (rnd() & 0x7ffffffffffffULL);
            break;
        default:
            v = 0x7ff0000000000000ULL | ((rnd() & 0x7ffffffffffffULL) | 1);
            break;
        }
        put_one(op, &a, &b, &c, make_float64(v | (rnd() & 1) << 63));
        check(P_NAN_INF, op, a, b, c);
    }
}

/* float32 whose value is m * 2^e2, m in [1, 2) random */
static float f32_pow2(int e2)
{
    return rnd_f32_exp(127 + e2);
}

static void gen_small_results(int op, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        float a, b, c = 0.0f;
        int split = 20 + rnd_below(40);

        /* nonzero results in (0, FLT_MIN] */
        switch (op) {
        case OP_ADD:
        case OP_SUB:
            /* exact differences of nearby small floats, b >= FLT_MIN */
            a = f32_pow2(-126 + rnd_below(3));
            if (a == FLT_MIN) {
                a = nextafterf(a, 1);
            }
            b = u2f(f2u(a) - MIN(1 + rnd_below(1 << 20),
                                 f2u(a) - f2u(FLT_MIN)));
            if (op == OP_ADD) {
                b = -b;
            }
            break;
        case OP_MUL:
            /* mantissa product < 4: below 2^-126 */
            a = f32_pow2(-128 + split - (int)rnd_below(MIN(split - 2, 20)));
            b = f32_pow2(-split);
            break;
        case OP_DIV:
            /* mantissa quotient < 2: below 2^-127 */
            a = f32_pow2(-128 + split - (int)rnd_below(MIN(split - 2, 20)));
            b = f32_pow2(split);
            break;
        default:
            a = f32_pow2(-128 + split - (int)rnd_below(MIN(split - 2, 20)));
            b = f32_pow2(-split);
            c = (rnd() & 1) ? 0.0f : -0.0f;
            break;
        }
        check(P_TINY, op, f2f64(a), f2f64(b), f2f64(c));

        if (op == OP_ADD || op == OP_SUB) {
            continue;
        }

        /* nonzero exact results that round to zero */
        a = f32_pow2(-60 - (int)rnd_below(60));
        b = f32_pow2(op == OP_DIV ? 100 + (int)rnd_below(20)
                                  : -100 - (int)rnd_below(20));
        check(P_UNDER_ZERO, op, f2f64(a), f2f64(b), f2f64(0.0f));

        /* (1 - 2^-24) * FLT_MIN: a tie that rounds up to FLT_MIN */
        a = ldexpf(1.0f - ldexpf(1.0f, -24), -split);
        b = op == OP_DIV ? ldexpf(1.0f, 126 - split)
                         : ldexpf(1.0f, -126 + split);
        if (rnd() & 1) {
            a = -a;
        }
        check(P_FLTMIN_UP, op, f2f64(a), f2f64(b), f2f64(0.0f));
    }
    if (op == OP_ADD || op == OP_SUB) {
        for (uint64_t i = 0; i < n; i++) {
            /* 2 FLT_MIN - FLT_MIN: exactly FLT_MIN, not taken */
            float a = 2 * FLT_MIN, b = op == OP_ADD ? -FLT_MIN : FLT_MIN;

            if (rnd() & 1) {
                a = -a;
                b = -b;
            }
            check(P_FLTMIN_UP, op, f2f64(a), f2f64(b), f2f64(0.0f));
        }
    }
}

static void gen_overflow(int op, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        float a, b, c = 0.0f;

        switch (op) {
        case OP_ADD:
        case OP_SUB:
            a = fabsf(f32_pow2(127));
            b = fabsf(f32_pow2(127));
            /* FLT_MAX + 2^103 is a tie that rounds to infinity */
            if (rnd_below(4) == 0) {
                a = FLT_MAX;
                b = 0x1p103f;
            }
            if (op == OP_SUB) {
                b = -b;
            }
            break;
        case OP_MUL:
            a = f32_pow2(64 + rnd_below(63));
            b = f32_pow2(128 - 64 + rnd_below(63));
            break;
        case OP_DIV:
            /* mantissa quotient > 1/2: at least 2^128 */
            a = f32_pow2(64 + rnd_below(63));
            b = f32_pow2(-65 - (int)rnd_below(61));
            break;
        default:
            a = f32_pow2(64 + rnd_below(63));
            b = f32_pow2(128 - 64 + rnd_below(63));
            c = rnd_f32_exp(127 - 40 + rnd_below(81));
            break;
        }
        if (rnd() & 1) {
            a = -a;
            if (op == OP_ADD || op == OP_SUB) {
                b = -b;
            }
        }
        check(P_OVERFLOW, op, f2f64(a), f2f64(b), f2f64(c));
    }
}

/*
 * (2 + 2^-22) * (1 - 2^-23) = 2 - 2^-45.  With a addend near 2^25, where
 * float32 spacing is 4, the double sum lands on a float32 midpoint while
 * the exact sum is 2^-45 off it: rounding the double sum again gives the
 * wrong neighbour when the tie goes the other way.
 */
static void gen_midpoint(int op, uint64_t n)
{
    if (!is_fma(op)) {
        return;
    }
    for (uint64_t i = 0; i < n; i++) {
        int scale = (int)rnd_below(200) - 100;
        float a = ldexpf(2.0f + ldexpf(1.0f, -22), scale);
        float b = 1.0f - ldexpf(1.0f, -23);
        /* addend = 2^25 + 4j, so p + addend is 2^25 + 4j + 2 - 2^-45 */
        float addend = ldexpf(0x1p25f + 4.0f * (float)(1 + rnd_below(8)),
                              scale);
        float c;

        if (rnd() & 1) {
            /* mirror: -p - addend, the residual changes sign */
            a = -a;
            addend = -addend;
        }
        /* c' = addend; c' = -c for msub/nmsub */
        c = (op == OP_MSUB || op == OP_NMSUB) ? -addend : addend;
        check(P_MIDPOINT, op, f2f64(a), f2f64(b), f2f64(c));
    }
}

static void gen_divzero(int op, uint64_t n)
{
    if (op != OP_DIV) {
        return;
    }
    for (uint64_t i = 0; i < n; i++) {
        float a = rnd_f32();
        float b = (rnd() & 1) ? -0.0f : 0.0f;

        check(P_DIVZERO, op, f2f64(a), f2f64(b), f2f64(0.0f));
    }
}

static bool report(void)
{
    bool ok = true;

    for (int p = 0; p < N_PARTS; p++) {
        for (int op = 0; op < N_OPS; op++) {
            for (int rm = 0; rm < N_RM; rm++) {
                Tally *t = &tally[p][op][rm];

                if (t->n == 0) {
                    continue;
                }
                printf("tally %-11s %-7s %-5s n=%" PRIu64 " taken=%" PRIu64
                       "\n", part_name[p], op_name[op], rm_name[rm],
                       t->n, t->taken);
                if (part_must_take(p, rm) && t->taken == 0) {
                    printf("FAIL taken: %s %s %s never took the host path\n",
                           part_name[p], op_name[op], rm_name[rm]);
                    ok = false;
                }
                if (!part_may_take(p, rm) && t->taken != 0) {
                    printf("FAIL taken: %s %s %s took the host path %"
                           PRIu64 " times\n", part_name[p], op_name[op],
                           rm_name[rm], t->taken);
                    ok = false;
                }
            }
        }
    }
    if (untouched_failures) {
        printf("FAIL untouched: %" PRIu64 " cases raised flags and then "
               "fell back\n", untouched_failures);
        ok = false;
    }
    if (generator_failures) {
        printf("FAIL generator: %" PRIu64 " cases outside their partition\n",
               generator_failures);
        ok = false;
    }
    if (mismatches) {
        printf("FAIL mismatch: %" PRIu64 " cases\n", mismatches);
        ok = false;
    }
    return ok;
}

int main(int argc, char **argv)
{
    uint64_t n = 1000000;
    static const uint64_t default_seeds[] = { 1, 2, 3 };
    uint64_t seeds[16];
    int n_seeds = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            if (qemu_strtou64(argv[++i], NULL, 0, &n)) {
                fprintf(stderr, "bad -n %s\n", argv[i]);
                return 2;
            }
        } else if (n_seeds < ARRAY_SIZE(seeds)) {
            if (qemu_strtou64(argv[i], NULL, 0, &seeds[n_seeds++])) {
                fprintf(stderr, "bad seed %s\n", argv[i]);
                return 2;
            }
        }
    }
    if (n_seeds == 0) {
        memcpy(seeds, default_seeds, sizeof(default_seeds));
        n_seeds = ARRAY_SIZE(default_seeds);
    }

    init_boundary();
    for (int op = 0; op < N_OPS; op++) {
        gen_boundary(op);
    }
    for (int si = 0; si < n_seeds; si++) {
        printf("seed %" PRIu64 " n=%" PRIu64 "\n", seeds[si], n);
        rng_state = seeds[si] * 0x9E3779B97F4A7C15ULL | 1;
        for (int op = 0; op < N_OPS; op++) {
            gen_random(op, n);
            gen_aligned(op, n / 5);
            gen_exact_zero(op, n / 50);
            gen_fltmax_ok(op, n / 50);
            gen_inexact(op, n / 50);
            gen_exp_out(op, n / 50, true);
            gen_exp_out(op, n / 50, false);
            gen_nan_inf(op, n / 50);
            gen_small_results(op, n / 50);
            gen_overflow(op, n / 50);
            gen_midpoint(op, n / 50);
            gen_divzero(op, n / 50);
        }
    }

    if (!report()) {
        return 1;
    }
    printf("PASS\n");
    return 0;
}
