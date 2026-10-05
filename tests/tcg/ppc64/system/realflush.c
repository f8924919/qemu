/*
 * Hypervisor real mode across the SLB and TLB invalidations.
 *
 * With MSR[HV] set and relocation off, the 970 turns an effective address
 * into a real one without the SLB or the hash table (there is no HRMOR on
 * this part).  slbia, slbie and tlbie only change those, so QEMU need not
 * drop what its softmmu TLB holds for hypervisor real mode when it flushes
 * on their behalf.  This test checks that code and data reached in that
 * mode are still right after each of them: code is rewritten in RAM and run
 * again, data is rewritten and read back, before and after the
 * invalidation.
 *
 * Nothing here can tell whether the TLB entries survived; it only shows
 * that keeping them does not hand out stale code or data.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <minilib.h>

/* RAM, reached in real mode */
#define CODE_PAGE 0x01000000UL
#define DATA_ADDR 0x01001000UL

/* An effective address for slbie and tlbie; nothing is mapped there */
#define EA_ANY 0x40000000UL

static int ok = 1;

/*
 * Real-mode stores to RAM that the tested code or the next load reads:
 * none may be dropped or reordered.
 */
static void wr64(unsigned long addr, unsigned long val)
{
    /* volatile: see above */
    *(volatile unsigned long *)addr = val;
}

static void wr32(unsigned long addr, unsigned int val)
{
    /* volatile: see above */
    *(volatile unsigned int *)addr = val;
}

static unsigned long rd64(unsigned long addr)
{
    /* volatile: see above */
    return *(volatile unsigned long *)addr;
}

/* li r3,mark; blr */
static void put_code(char mark)
{
    wr32(CODE_PAGE, 0x38600000 | mark);
    wr32(CODE_PAGE + 4, 0x4e800020);
    asm volatile("dcbst 0,%0; sync; icbi 0,%0; sync; isync"
                 : : "r"(CODE_PAGE) : "memory");
}

static unsigned long call_code(void)
{
    register unsigned long r3 asm("r3");

    asm volatile("mtctr %1; bctrl" : "=r"(r3) : "r"(CODE_PAGE)
                 : "ctr", "lr", "memory");
    return r3;
}

enum way { SLBIA, SLBIE, TLBIE };

static const char *way_name(enum way how)
{
    switch (how) {
    case SLBIA:
        return "slbia";
    case SLBIE:
        return "slbie";
    default:
        return "tlbie";
    }
}

static void invalidate(enum way how)
{
    switch (how) {
    case SLBIA:
        asm volatile("slbia; isync" : : : "memory");
        break;
    case SLBIE:
        asm volatile("slbie %0; isync" : : "r"(EA_ANY) : "memory");
        break;
    case TLBIE:
        asm volatile("tlbie %0,%1; eieio; tlbsync; ptesync"
                     : : "r"(EA_ANY), "r"(0UL) : "memory");
        break;
    }
}

static void expect(enum way how, const char *step, char want_code,
                   unsigned long want_data)
{
    unsigned long code = call_code();
    unsigned long data = rd64(DATA_ADDR);

    ml_printf("realflush %s %s: code %c data %lu\n", way_name(how), step,
              (char)code, data);
    if (code != (unsigned long)want_code) {
        ml_printf("FAIL: realflush %s %s code %c expected %c\n",
                  way_name(how), step, (char)code, want_code);
        ok = 0;
    }
    if (data != want_data) {
        ml_printf("FAIL: realflush %s %s data %lu expected %lu\n",
                  way_name(how), step, data, want_data);
        ok = 0;
    }
}

static void run_case(enum way how)
{
    /* fill the TLB for both addresses */
    put_code('A');
    wr64(DATA_ADDR, 1);
    expect(how, "before", 'A', 1);

    /* nothing changed: still the same */
    invalidate(how);
    expect(how, "same", 'A', 1);

    /* changed before the invalidation */
    put_code('B');
    wr64(DATA_ADDR, 2);
    invalidate(how);
    expect(how, "changed-before", 'B', 2);

    /* changed after the invalidation */
    invalidate(how);
    put_code('C');
    wr64(DATA_ADDR, 3);
    expect(how, "changed-after", 'C', 3);
}

int main(void)
{
    run_case(SLBIA);
    run_case(SLBIE);
    run_case(TLBIE);

    if (ok) {
        ml_printf("PASS: realflush\n");
    }

    return 0;
}
