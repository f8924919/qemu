/*
 * qemu-g5 #387 (measurement only, not for upstream): see g5-fpelig.h.
 *
 * Each check below names the softfloat code it copies.  Keep them in step
 * with fpu/softfloat.c; tests/fp/g5-fpelig-oracle.c compares the two.
 */

#include "qemu/osdep.h"
#include <float.h>
#include <math.h>
#include "fpu/softfloat.h"
#include "g5-fpelig.h"

static double d(uint64_t u)
{
    union { double d; uint64_t u; } x = { .u = u };
    return x.d;
}

static uint32_t f2u(float f)
{
    union { float f; uint32_t u; } x = { .f = f };
    return x.u;
}

static bool zon64(uint64_t a)
{
    return float64_is_zero_or_normal(make_float64(a));
}

static bool zon32(float a)
{
    return float32_is_zero_or_normal(make_float32(f2u(a)));
}

/* gen2 post: result not inf, |r| <= min, and post(a, b) (f64_*_post) */
static bool tiny(double r, double min)
{
    return !isinf(r) && fabs(r) <= min;
}

G5FpTier g5fpe_tier64(G5FpKind k, bool rne, bool force_soft_fma,
                      uint64_t a, uint64_t b, uint64_t c,
                      uint64_t r, uint64_t mid)
{
    float64 fa = make_float64(a), fb = make_float64(b);

    if (k == G5FPK_CMP) {
        /*
         * float64_hs_compare: ignores the rounding mode and the flags;
         * denormal inputs go to softfloat, and so does unordered.
         */
        if (float64_is_denormal(fa) || float64_is_denormal(fb) ||
            float64_is_any_nan(fa) || float64_is_any_nan(fb)) {
            return G5FPE_PRE;
        }
        return G5FPE_OK;
    }
    /* can_use_fpu(), apart from the inexact flag */
    if (!rne) {
        return G5FPE_RN;
    }
    switch (k) {
    case G5FPK_ADDSUB:
    case G5FPK_MUL:
        /* f64_is_zon2, f64_addsubmul_post */
        if (!zon64(a) || !zon64(b)) {
            return G5FPE_PRE;
        }
        if (tiny(d(r), DBL_MIN) &&
            !(float64_is_zero(fa) && float64_is_zero(fb))) {
            return G5FPE_POST;
        }
        return G5FPE_OK;
    case G5FPK_DIV:
        /* f64_div_pre, f64_div_post */
        if (!zon64(a) || !float64_is_normal(fb)) {
            return G5FPE_PRE;
        }
        if (tiny(d(r), DBL_MIN) && !float64_is_zero(fa)) {
            return G5FPE_POST;
        }
        return G5FPE_OK;
    case G5FPK_FMA:
        /* float64_muladd */
        if (!zon64(a) || !zon64(b) || !zon64(c) || force_soft_fma) {
            return G5FPE_PRE;
        }
        if (float64_is_zero(fa) || float64_is_zero(fb)) {
            return G5FPE_OK;
        }
        if (tiny(d(r), FLT_MIN)) {
            return G5FPE_POST;
        }
        return G5FPE_OK;
    case G5FPK_SQRT:
        /* float64_sqrt */
        if (!zon64(a) || float64_is_neg(fa)) {
            return G5FPE_PRE;
        }
        return G5FPE_OK;
    case G5FPK_RSQRTE:
        /* float64_sqrt, then float64_div(1, mid) */
        if (!zon64(a) || float64_is_neg(fa) ||
            !float64_is_normal(make_float64(mid))) {
            return G5FPE_PRE;
        }
        if (tiny(d(r), DBL_MIN)) {
            return G5FPE_POST;
        }
        return G5FPE_OK;
    case G5FPK_ITOF:
        /* int64_to_float64_scalbn and friends with scale == 0 */
        return G5FPE_OK;
    default:
        return G5FPE_NA;
    }
}

bool g5fpe_f32_exact(uint64_t a)
{
    double x = d(a);

    return !isnan(x) && (double)(float)x == x;
}

G5FpTier g5fpe_tier32(G5FpKind k, bool rne, bool force_soft_fma,
                      uint64_t a, uint64_t b, uint64_t c,
                      uint64_t r, uint64_t mid)
{
    float fa = d(a), fb = d(b), fc = d(c), fr = d(r);

    switch (k) {
    case G5FPK_ADDSUB:
    case G5FPK_MUL:
    case G5FPK_DIV:
        if (!g5fpe_f32_exact(a) || !g5fpe_f32_exact(b)) {
            return G5FPE_NA;
        }
        break;
    case G5FPK_FMA:
        if (!g5fpe_f32_exact(a) || !g5fpe_f32_exact(b) ||
            !g5fpe_f32_exact(c)) {
            return G5FPE_NA;
        }
        break;
    case G5FPK_SQRT:
        if (!g5fpe_f32_exact(a)) {
            return G5FPE_NA;
        }
        break;
    case G5FPK_RSQRTE:
        /*
         * frsqrtes takes the square root in double precision and divides
         * in single precision: the division is a float32 candidate only if
         * that square root is a single.
         */
        if (!rne) {
            return G5FPE_RN;
        }
        if (!zon64(a) || float64_is_neg(make_float64(a))) {
            return G5FPE_PRE;
        }
        if (!g5fpe_f32_exact(mid)) {
            return G5FPE_NA;
        }
        fb = d(mid);
        if (!float32_is_normal(make_float32(f2u(fb)))) {
            return G5FPE_PRE;
        }
        if (tiny(fr, FLT_MIN)) {
            return G5FPE_POST;
        }
        return G5FPE_OK;
    default:
        return G5FPE_NA;
    }
    if (!rne) {
        return G5FPE_RN;
    }
    switch (k) {
    case G5FPK_ADDSUB:
    case G5FPK_MUL:
        /* f32_is_zon2, f32_addsubmul_post */
        if (!zon32(fa) || !zon32(fb)) {
            return G5FPE_PRE;
        }
        if (tiny(fr, FLT_MIN) && !(fa == 0 && fb == 0)) {
            return G5FPE_POST;
        }
        return G5FPE_OK;
    case G5FPK_DIV:
        /* f32_div_pre, f32_div_post */
        if (!zon32(fa) || !float32_is_normal(make_float32(f2u(fb)))) {
            return G5FPE_PRE;
        }
        if (tiny(fr, FLT_MIN) && fa != 0) {
            return G5FPE_POST;
        }
        return G5FPE_OK;
    case G5FPK_FMA:
        /* float32_muladd */
        if (!zon32(fa) || !zon32(fb) || !zon32(fc) || force_soft_fma) {
            return G5FPE_PRE;
        }
        if (fa == 0 || fb == 0) {
            return G5FPE_OK;
        }
        if (tiny(fr, FLT_MIN)) {
            return G5FPE_POST;
        }
        return G5FPE_OK;
    case G5FPK_SQRT:
        /* float32_sqrt */
        if (!zon32(fa) || float32_is_neg(make_float32(f2u(fa)))) {
            return G5FPE_PRE;
        }
        return G5FPE_OK;
    default:
        return G5FPE_NA;
    }
}
