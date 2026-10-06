/*
 * qemu-g5 #387 (measurement only, not for upstream): target/ppc's FP
 * counters register this to print G5FP lines at the end of "info jit".
 */

#ifndef EXEC_G5_FPDUMP_H
#define EXEC_G5_FPDUMP_H

extern void (*g5fp_dump_hook)(GString *buf);

#endif
