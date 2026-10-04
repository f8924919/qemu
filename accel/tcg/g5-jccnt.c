/*
 * qemu-g5 #369: throwaway jump cache recheck counters.  NOT FOR UPSTREAM.
 * See include/exec/g5-jccnt.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "exec/g5-jccnt.h"

int g5jc_skip_recheck;
int g5jc_old;

static uint64_t cnt[G5JC_MAXCPU][G5JC_N];
static const char *const names[G5JC_N] = {
    "lookup", "hit_checked", "enter", "pass", "cmpfail", "minus1",
    "skipped", "x_same", "x_diff", "x_null", "qht",
};

static void __attribute__((constructor)) g5jc_init(void)
{
    const char *s = getenv("G5JC_SKIP_RECHECK");
    const char *o = getenv("G5JC_OLD");

    g5jc_skip_recheck = s && *s == '1';
    g5jc_old = o && *o == '1';
}

void g5jc_count(int cpu, int what)
{
    cnt[cpu & (G5JC_MAXCPU - 1)][what]++;
}

void g5jc_dump(void *opaque)
{
    GString *buf = opaque;

    g_string_append_printf(buf, "G5JC mode skip_recheck=%d old=%d\n",
                           g5jc_skip_recheck, g5jc_old);
    for (int c = 0; c < G5JC_MAXCPU; c++) {
        if (!cnt[c][G5JC_LOOKUP]) {
            continue;
        }
        g_string_append_printf(buf, "G5JC cpu=%d", c);
        for (int i = 0; i < G5JC_N; i++) {
            g_string_append_printf(buf, " %s=%" PRIu64, names[i], cnt[c][i]);
        }
        g_string_append_c(buf, '\n');
    }
}
