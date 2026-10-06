/*
 * qemu-g5 #387 (measurement only, not for upstream): would a scalar FP
 * operation have taken softfloat's hardfloat path, had the FP flags not
 * been cleared before it?
 *
 * The conditions are transcribed from fpu/softfloat.c (can_use_fpu, the
 * pre/post checks of float64_gen2/float32_gen2, float64_muladd,
 * float32_muladd, float64_sqrt, float32_sqrt).  The functions only look at
 * bit patterns and do not read any QEMU state, so that
 * tests/fp/g5-fpelig-oracle.c can check them against softfloat itself.
 */

#ifndef G5_FPELIG_H
#define G5_FPELIG_H

#include <stdbool.h>
#include <stdint.h>

/* First stage that fails; G5FPE_OK when all of them pass. */
typedef enum {
    G5FPE_NA,       /* no hardfloat path, or not a candidate */
    G5FPE_RN,       /* (a) rounding mode is not nearest-even */
    G5FPE_PRE,      /* (b) the input check fails */
    G5FPE_POST,     /* (c) the result sends it back to softfloat */
    G5FPE_OK,
    G5FPE_NB,
} G5FpTier;

typedef enum {
    G5FPK_ADDSUB,   /* a +- b */
    G5FPK_MUL,      /* a * b */
    G5FPK_DIV,      /* a / b (fre: a = 1.0) */
    G5FPK_FMA,      /* a * b + c, any negation */
    G5FPK_SQRT,     /* sqrt(a) */
    G5FPK_RSQRTE,   /* 1 / sqrt(a): mid = sqrt(a) */
    G5FPK_ITOF,     /* integer to float */
    G5FPK_CMP,      /* compare a, b */
} G5FpKind;

/*
 * Double precision.  a, b, c are the inputs as passed to softfloat, r is
 * the result softfloat returned and mid the intermediate result (sqrt) of
 * G5FPK_RSQRTE.  Unused arguments are ignored.
 */
G5FpTier g5fpe_tier64(G5FpKind k, bool rne, bool force_soft_fma,
                      uint64_t a, uint64_t b, uint64_t c,
                      uint64_t r, uint64_t mid);

/* True if the double a is exactly representable as a single (not NaN). */
bool g5fpe_f32_exact(uint64_t a);

/*
 * Single precision with float32 hardfloat (H2).  The inputs and results
 * are doubles holding single precision values, as float64r32_* takes and
 * returns them.  Returns G5FPE_NA unless all used inputs are
 * g5fpe_f32_exact().  G5FPK_ITOF and G5FPK_CMP are not valid here.
 */
G5FpTier g5fpe_tier32(G5FpKind k, bool rne, bool force_soft_fma,
                      uint64_t a, uint64_t b, uint64_t c,
                      uint64_t r, uint64_t mid);

#endif
