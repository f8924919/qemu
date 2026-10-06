/*
 * qemu-g5 #387 (measurement only, not for upstream): count scalar FP
 * operations by whether they could take softfloat's hardfloat path, the
 * FPSCR settings they run under, the instructions that read and write the
 * FPSCR, and FP exceptions.  Rows are per vCPU and written only by that
 * vCPU's thread; they are printed at the end of "info jit" as G5FP lines.
 */

#ifndef TARGET_PPC_G5_FPCNT_H
#define TARGET_PPC_G5_FPCNT_H

#include "g5-fpelig.h"

/* How a helper relates to hardfloat. */
enum {
    G5FP_PATH,      /* softfloat has a hardfloat path for it */
    G5FP_H2,        /* single precision: counted with float32's conditions */
    G5FP_NOPATH,    /* no hardfloat path */
    G5FP_CMP,       /* hardfloat whatever the flags */
};

/*
 * Every scalar FP helper that calls ppc_fp_reset():
 * X(name, single, op kind for g5-fpelig, path)
 */
#define G5FP_HELPERS(X)                                 \
    X(FADD, 0, G5FPK_ADDSUB, G5FP_PATH)                 \
    X(FSUB, 0, G5FPK_ADDSUB, G5FP_PATH)                 \
    X(FADDS, 1, G5FPK_ADDSUB, G5FP_H2)                  \
    X(FSUBS, 1, G5FPK_ADDSUB, G5FP_H2)                  \
    X(FMUL, 0, G5FPK_MUL, G5FP_PATH)                    \
    X(FMULS, 1, G5FPK_MUL, G5FP_H2)                     \
    X(FDIV, 0, G5FPK_DIV, G5FP_PATH)                    \
    X(FDIVS, 1, G5FPK_DIV, G5FP_H2)                     \
    X(FMADD, 0, G5FPK_FMA, G5FP_PATH)                   \
    X(FMSUB, 0, G5FPK_FMA, G5FP_PATH)                   \
    X(FNMADD, 0, G5FPK_FMA, G5FP_PATH)                  \
    X(FNMSUB, 0, G5FPK_FMA, G5FP_PATH)                  \
    X(FMADDS, 1, G5FPK_FMA, G5FP_H2)                    \
    X(FMSUBS, 1, G5FPK_FMA, G5FP_H2)                    \
    X(FNMADDS, 1, G5FPK_FMA, G5FP_H2)                   \
    X(FNMSUBS, 1, G5FPK_FMA, G5FP_H2)                   \
    X(FSQRT, 0, G5FPK_SQRT, G5FP_PATH)                  \
    X(FSQRTS, 1, G5FPK_SQRT, G5FP_H2)                   \
    X(FRE, 0, G5FPK_DIV, G5FP_PATH)                     \
    X(FRES, 1, G5FPK_DIV, G5FP_H2)                      \
    X(FRSQRTE, 0, G5FPK_RSQRTE, G5FP_PATH)              \
    X(FRSQRTES, 1, G5FPK_RSQRTE, G5FP_H2)               \
    X(FCFID, 0, G5FPK_ITOF, G5FP_PATH)                  \
    X(FCFIDU, 0, G5FPK_ITOF, G5FP_PATH)                 \
    X(FCFIDS, 1, G5FPK_ITOF, G5FP_PATH)                 \
    X(FCFIDUS, 1, G5FPK_ITOF, G5FP_PATH)                \
    X(FCTIW, 0, G5FPK_ITOF, G5FP_NOPATH)                \
    X(FCTIWZ, 0, G5FPK_ITOF, G5FP_NOPATH)               \
    X(FCTIWU, 0, G5FPK_ITOF, G5FP_NOPATH)               \
    X(FCTIWUZ, 0, G5FPK_ITOF, G5FP_NOPATH)              \
    X(FCTID, 0, G5FPK_ITOF, G5FP_NOPATH)                \
    X(FCTIDZ, 0, G5FPK_ITOF, G5FP_NOPATH)               \
    X(FCTIDU, 0, G5FPK_ITOF, G5FP_NOPATH)               \
    X(FCTIDUZ, 0, G5FPK_ITOF, G5FP_NOPATH)              \
    X(FRIN, 0, G5FPK_ITOF, G5FP_NOPATH)                 \
    X(FRIZ, 0, G5FPK_ITOF, G5FP_NOPATH)                 \
    X(FRIP, 0, G5FPK_ITOF, G5FP_NOPATH)                 \
    X(FRIM, 0, G5FPK_ITOF, G5FP_NOPATH)                 \
    X(FRSP, 1, G5FPK_ITOF, G5FP_NOPATH)                 \
    X(FCMPU, 0, G5FPK_CMP, G5FP_CMP)                    \
    X(FCMPO, 0, G5FPK_CMP, G5FP_CMP)

#define G5FP_ID(name, s, k, p) G5FP_##name,
enum { G5FP_HELPERS(G5FP_ID) G5FP_NH };
#undef G5FP_ID

/* Instructions that read or write the FPSCR, counted at translation. */
#define G5FP_TRS(X)     \
    X(MFFS)             \
    X(MFFSCE)           \
    X(MFFSCRN)          \
    X(MFFSCDRN)         \
    X(MFFSCRNI)         \
    X(MFFSCDRNI)        \
    X(MFFSL)            \
    X(MCRFS)            \
    X(CR1)              \
    X(CR1MTFS)          \
    X(MTFSB0)           \
    X(MTFSB1)           \
    X(MTFSF)            \
    X(MTFSFI)

#define G5FP_TR_ID(name) G5FP_TR_##name,
enum { G5FP_TRS(G5FP_TR_ID) G5FP_NTR };
#undef G5FP_TR_ID

/* The argument of a G5FP_TR_* (field, bit, FLM, ...) fits in 9 bits. */
#define G5FP_TR_ARGS 512
#define G5FP_TR_CODE(kind, arg, pr) (((kind) << 10) | ((arg) << 1) | (pr))

void g5fp_entry(CPUPPCState *env);
void g5fp_op(CPUPPCState *env, int h, uint64_t a, uint64_t b, uint64_t c,
             uint64_t r, uint64_t mid);
/* An enabled FP exception: fe says whether MSR[FE0|FE1] lets it through. */
void g5fp_excreq(CPUPPCState *env, int cause, bool fe);
/* At delivery: delivered, or ignored because MSR[FE0|FE1] or FP is 0. */
void g5fp_exc(CPUPPCState *env, int cause, bool deliver);

#endif
