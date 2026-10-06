/*
 * PowerPC single-precision scalar FP: a float32 hardfloat path whose
 * result bits and softfloat exception flags match float64r32_*().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_PPC_FPU_HARD32_H
#define TARGET_PPC_FPU_HARD32_H

#include <float.h>
#include <math.h>
#include "fpu/softfloat.h"

/*
 * The PowerPC FP helpers clear the softfloat flags before every
 * instruction, so softfloat's own hardfloat (which needs inexact to be
 * set already) never runs, and FPSCR[FI] must be exact.  This path takes
 * an operation to the host only when the inputs are float32 zeros or
 * normals, the rounding mode is nearest-even and the float32 result is
 * zero or normal; it then computes the inexact flag exactly, with
 * error-free transformations in double.  Everything else (NaN, infinity,
 * denormals, overflow, underflow, other rounding modes) is left to
 * float64r32_*(), so the only flag this path raises is inexact.
 *
 * Each ppc_hard32_*() returns true and stores the result in *r when it
 * took the host path.  It returns false without touching @s or *r when
 * the operation must go through softfloat instead.
 *
 * Products of two float32 values are exact in double (48 bits), so
 * contracting a * b into a fused multiply-add cannot change any value
 * computed here; no -ffp-contract setting is needed.
 */
#if FLT_EVAL_METHOD == 0 && !defined(__FAST_MATH__)
#define PPC_HARD32_ENABLED 1
#else
#define PPC_HARD32_ENABLED 0
#endif

#define PPC_HARD32_LOW29 ((1ULL << 29) - 1)

static inline double ppc_hard32_to_double(float64 x)
{
    union { uint64_t u; double d; } v = { .u = float64_val(x) };

    return v.d;
}

static inline float64 ppc_hard32_from_double(double x)
{
    union { uint64_t u; double d; } v = { .d = x };

    return make_float64(v.u);
}

/* x is +-0 or a float32 normal, exactly. */
static inline bool ppc_hard32_input_ok(float64 x)
{
    uint64_t v = float64_val(x);
    uint64_t exp = (v >> 52) & 0x7ff;

    if (v & PPC_HARD32_LOW29) {
        return false;
    }
    if (exp == 0) {
        return (v << 1) == 0;
    }
    return exp >= 1023 - 126 && exp <= 1023 + 127;
}

static inline bool ppc_hard32_status_ok(const float_status *s)
{
    return PPC_HARD32_ENABLED &&
           get_float_rounding_mode(s) == float_round_nearest_even &&
           !get_flush_to_zero(s) && !get_flush_inputs_to_zero(s);
}

/* No overflow, and no result softfloat would denormalize or flush. */
static inline bool ppc_hard32_result_ok(float r)
{
    return !isinf(r) && (r == 0 || fabsf(r) > FLT_MIN);
}

static inline bool ppc_hard32_finish(float r, bool inexact, float64 *res,
                                     float_status *s)
{
    if (r == 0 && inexact) {
        return false;       /* a nonzero result underflowed to zero */
    }
    if (inexact) {
        float_raise(float_flag_inexact, s);
    }
    *res = ppc_hard32_from_double(r);
    return true;
}

static inline bool ppc_hard32_addsub(float64 a, float64 b, bool sub,
                                     float64 *r, float_status *s)
{
    double da, db, sum, t, err;
    float fr;

    if (!ppc_hard32_status_ok(s) ||
        !ppc_hard32_input_ok(a) || !ppc_hard32_input_ok(b)) {
        return false;
    }
    da = ppc_hard32_to_double(a);
    db = ppc_hard32_to_double(b);
    if (sub) {
        db = -db;
    }
    fr = (float)da + (float)db;
    if (!ppc_hard32_result_ok(fr)) {
        return false;
    }
    /* TwoSum: sum + err is the exact sum */
    sum = da + db;
    t = sum - da;
    err = (da - (sum - t)) + (db - t);
    return ppc_hard32_finish(fr, err != 0 || (double)fr != sum, r, s);
}

static inline bool ppc_hard32_mul(float64 a, float64 b, float64 *r,
                                  float_status *s)
{
    double da, db;
    float fr;

    if (!ppc_hard32_status_ok(s) ||
        !ppc_hard32_input_ok(a) || !ppc_hard32_input_ok(b)) {
        return false;
    }
    da = ppc_hard32_to_double(a);
    db = ppc_hard32_to_double(b);
    fr = (float)da * (float)db;
    if (!ppc_hard32_result_ok(fr)) {
        return false;
    }
    /* the double product is exact */
    return ppc_hard32_finish(fr, (double)fr != da * db, r, s);
}

static inline bool ppc_hard32_div(float64 a, float64 b, float64 *r,
                                  float_status *s)
{
    double da, db;
    float fr;

    if (!ppc_hard32_status_ok(s) ||
        !ppc_hard32_input_ok(a) || !ppc_hard32_input_ok(b) ||
        float64_is_zero(b)) {
        return false;
    }
    da = ppc_hard32_to_double(a);
    db = ppc_hard32_to_double(b);
    fr = (float)da / (float)db;
    if (!ppc_hard32_result_ok(fr)) {
        return false;
    }
    /* the quotient is exact iff fr * b, exact in double, equals a */
    return ppc_hard32_finish(fr, (double)fr * db != da, r, s);
}

static inline bool ppc_hard32_muladd(float64 a, float64 b, float64 c,
                                     int flags, float64 *r, float_status *s)
{
    double da, db, dc, prod, sum, t, err;
    float fr;
    bool inexact;

    if ((flags & ~(float_muladd_negate_c | float_muladd_negate_result)) ||
        !ppc_hard32_status_ok(s) || !ppc_hard32_input_ok(a) ||
        !ppc_hard32_input_ok(b) || !ppc_hard32_input_ok(c)) {
        return false;
    }
    da = ppc_hard32_to_double(a);
    db = ppc_hard32_to_double(b);
    dc = ppc_hard32_to_double(c);
    if (flags & float_muladd_negate_c) {
        dc = -dc;
    }
    /* the double product is exact; TwoSum gives sum + err exactly */
    prod = da * db;
    sum = prod + dc;
    t = sum - prod;
    err = (prod - (sum - t)) + (dc - t);
    /*
     * sum is the double nearest the exact value, so rounding it to
     * float32 gives the correctly rounded result unless it sits exactly
     * on a float32 midpoint (25 significant bits) while err != 0.
     */
    if (err != 0 &&
        (float64_val(ppc_hard32_from_double(sum)) & PPC_HARD32_LOW29) ==
        (1ULL << 28)) {
        return false;
    }
    fr = (float)sum;
    if (!ppc_hard32_result_ok(fr)) {
        return false;
    }
    inexact = err != 0 || (double)fr != sum;
    /* negate after rounding, zeros included, as float64r32_muladd() */
    if (flags & float_muladd_negate_result) {
        fr = -fr;
    }
    return ppc_hard32_finish(fr, inexact, r, s);
}

/* Drop-in replacements for float64r32_*() in the scalar FP helpers. */
static inline float64 ppc_f64r32_add(float64 a, float64 b, float_status *s)
{
    float64 r;

    return ppc_hard32_addsub(a, b, false, &r, s) ? r
                                                 : float64r32_add(a, b, s);
}

static inline float64 ppc_f64r32_sub(float64 a, float64 b, float_status *s)
{
    float64 r;

    return ppc_hard32_addsub(a, b, true, &r, s) ? r
                                                : float64r32_sub(a, b, s);
}

static inline float64 ppc_f64r32_mul(float64 a, float64 b, float_status *s)
{
    float64 r;

    return ppc_hard32_mul(a, b, &r, s) ? r : float64r32_mul(a, b, s);
}

static inline float64 ppc_f64r32_div(float64 a, float64 b, float_status *s)
{
    float64 r;

    return ppc_hard32_div(a, b, &r, s) ? r : float64r32_div(a, b, s);
}

static inline float64 ppc_f64r32_muladd(float64 a, float64 b, float64 c,
                                        int flags, float_status *s)
{
    float64 r;

    return ppc_hard32_muladd(a, b, c, flags, &r, s)
           ? r : float64r32_muladd(a, b, c, flags, s);
}

#endif
