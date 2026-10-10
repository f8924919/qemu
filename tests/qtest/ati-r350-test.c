/*
 * QTest testcase for the ATI R350 (Radeon 9800) 2D packets
 *
 * Copyright (c) 2026 The QEMU Project Developers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/libqos-spapr.h"
#include "libqos/pci.h"

#define R350_VENDOR_ID          0x1002
#define R350_DEVICE_ID          0x4e48

#define VRAM_BAR                0
#define MMIO_BAR                2

#define PM4_FIFO_DATA_EVEN      0x1000
#define DP_BRUSH_FRGD_CLR       0x147c
#define DEFAULT_OFFSET          0x16e0
#define DEFAULT_SC_BOTTOM_RIGHT 0x16e8

#define PKT3(op, n)     (0xc0000000u | (((n) - 1) << 16) | ((op) << 8))
#define OP_BITBLT_MULTI 0x9b

/* GUI_CONTROL (DP_GUI_MASTER_CNTL) */
#define GMC_SRC_PITCH_OFFSET    (1u << 0)
#define GMC_DST_PITCH_OFFSET    (1u << 1)
#define GMC_SRC_CLIPPING        (1u << 2)
#define GMC_DST_CLIPPING        (1u << 3)
#define GMC_BRUSH(t)            ((uint32_t)(t) << 4)
#define GMC_LD_BRUSH_Y_X        (1u << 31)
/* 32bpp colour source from video memory, SRCCOPY */
#define GMC_COPY                ((6u << 8) | (3u << 12) | (0xccu << 16) | \
                                 (2u << 24) | (1u << 28) | (1u << 30))

#define BRUSH_SOLID_COLOR       13
#define BRUSH_NONE              15

/* Radeon packed pitch/offset: pitch/64 in [29:22], offset/1K in [21:0] */
#define PITCH_OFFSET(off, pitch) ((((pitch) / 64) << 22) | ((off) >> 10))
#define XY(x, y)        (((uint32_t)(x) << 16) | (y))
/* the scissor registers put Y high and X low, unlike the rectangles */
#define SC(x, y)        (((uint32_t)(y) << 16) | (x))

/* Small 32bpp surfaces, 256 pixels across */
#define PITCH           1024
#define SRC_OFF         0x100000
#define DST_OFF         0x200000

typedef struct {
    QOSState *qs;
    QPCIDevice *dev;
    QPCIBar vram;
    QPCIBar mmio;
} R350;

static void find_r350(QPCIDevice *dev, int devfn, void *data)
{
    *(QPCIDevice **)data = dev;
}

static void r350_start(R350 *r)
{
    QPCIDevice *dev = NULL;

    r->qs = qtest_spapr_boot("-vga none -device ati-radeon9800,"
                             "async-engine=off,async-display=off");
    qpci_device_foreach(r->qs->pcibus, R350_VENDOR_ID, R350_DEVICE_ID,
                        find_r350, &dev);
    g_assert(dev);
    r->dev = dev;
    r->vram = qpci_iomap(dev, VRAM_BAR, NULL);
    r->mmio = qpci_iomap(dev, MMIO_BAR, NULL);
    qpci_device_enable(dev);

    qpci_io_writel(dev, r->mmio, DEFAULT_SC_BOTTOM_RIGHT, 0x1fff1fff);
}

static void r350_stop(R350 *r)
{
    g_free(r->dev);
    qtest_shutdown(r->qs);
}

static void push(R350 *r, const uint32_t *dw, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        qpci_io_writel(r->dev, r->mmio, PM4_FIFO_DATA_EVEN, dw[i]);
    }
}

static uint64_t px_off(uint32_t surf, uint32_t pitch, int x, int y)
{
    return surf + (uint64_t)y * pitch + x * 4;
}

static void put_px(R350 *r, uint32_t surf, uint32_t pitch, int x, int y,
                   uint32_t v)
{
    qpci_memwrite(r->dev, r->vram, px_off(surf, pitch, x, y), &v, 4);
}

static uint32_t get_px(R350 *r, uint32_t surf, uint32_t pitch, int x, int y)
{
    uint32_t v;

    qpci_memread(r->dev, r->vram, px_off(surf, pitch, x, y), &v, 4);
    return v;
}

/* A source pixel's value says where it came from */
static uint32_t pat(int x, int y)
{
    return 0xa5000000 | (y << 12) | x;
}

/* Source pixels at (x, y) for each point; the rest of VRAM stays zero */
static void seed(R350 *r, uint32_t surf, uint32_t pitch,
                 const int (*pts)[2], size_t n)
{
    for (size_t i = 0; i < n; i++) {
        put_px(r, surf, pitch, pts[i][0], pts[i][1],
               pat(pts[i][0], pts[i][1]));
    }
}

/* dst(x + dx, y + dy) == src(x, y) for each source point */
static void assert_copied(R350 *r, uint32_t surf, uint32_t pitch,
                          const int (*pts)[2], size_t n, int dx, int dy)
{
    for (size_t i = 0; i < n; i++) {
        int x = pts[i][0], y = pts[i][1];

        g_assert_cmphex(get_px(r, surf, pitch, x + dx, y + dy), ==,
                        pat(x, y));
    }
}

static void assert_untouched(R350 *r, uint32_t surf, uint32_t pitch,
                             const int (*pts)[2], size_t n, int dx, int dy)
{
    for (size_t i = 0; i < n; i++) {
        g_assert_cmphex(get_px(r, surf, pitch, pts[i][0] + dx,
                               pts[i][1] + dy), ==, 0);
    }
}

/*
 * (a) The packet Mac OS X 10.5's Quartz Composer "Shell" screen saver
 * sends every frame, verbatim: both pitch/offsets and DST_CLIPPING, so
 * SC_TOP_LEFT and SC_BOTTOM_RIGHT come before the one rectangle, a
 * 1024x768 copy from 0xed0000 to 0x11d0000.
 */
static void test_bitblt_multi_shell(void)
{
    static const uint32_t pkt[] = {
        PKT3(OP_BITBLT_MULTI, 8),
        0x52cc36fb, 0x50003b40, 0x50004740,
        0x00000000, 0x03000400,
        0x00000000, 0x00000000, 0x04000300,
    };
    static const int pts[][2] = {
        { 0, 0 }, { 1023, 0 }, { 0, 767 }, { 1023, 767 }, { 512, 384 },
    };
    R350 r;

    r350_start(&r);
    seed(&r, 0xed0000, 4096, pts, ARRAY_SIZE(pts));
    push(&r, pkt, ARRAY_SIZE(pkt));
    assert_copied(&r, 0x11d0000, 4096, pts, ARRAY_SIZE(pts), 0, 0);
    r350_stop(&r);
}

/* (b) The scissor the packet carries is the one the copy obeys */
static void test_bitblt_multi_scissor(void)
{
    static const uint32_t pkt[] = {
        PKT3(OP_BITBLT_MULTI, 8),
        GMC_COPY | GMC_SRC_PITCH_OFFSET | GMC_DST_PITCH_OFFSET |
        GMC_DST_CLIPPING | GMC_BRUSH(BRUSH_NONE),
        PITCH_OFFSET(SRC_OFF, PITCH), PITCH_OFFSET(DST_OFF, PITCH),
        SC(16, 8), SC(47, 23),          /* SC_TOP_LEFT, SC_BOTTOM_RIGHT */
        XY(0, 0), XY(0, 0), XY(64, 32),
    };
    static const int inside[][2] = { { 20, 10 }, { 40, 20 }, { 30, 15 } };
    static const int outside[][2] = {
        { 2, 2 }, { 60, 30 }, { 30, 2 }, { 2, 15 }, { 60, 15 }, { 30, 30 },
    };
    R350 r;

    r350_start(&r);
    seed(&r, SRC_OFF, PITCH, inside, ARRAY_SIZE(inside));
    seed(&r, SRC_OFF, PITCH, outside, ARRAY_SIZE(outside));
    push(&r, pkt, ARRAY_SIZE(pkt));
    assert_copied(&r, DST_OFF, PITCH, inside, ARRAY_SIZE(inside), 0, 0);
    assert_untouched(&r, DST_OFF, PITCH, outside, ARRAY_SIZE(outside), 0, 0);
    r350_stop(&r);
}

/*
 * (c) The iTunes form: only the source pitch/offset (the destination is
 * the default one) and a run of rectangles, every one of them copied.
 */
static void test_bitblt_multi_run(void)
{
    static const uint32_t pkt[] = {
        PKT3(OP_BITBLT_MULTI, 8),
        GMC_COPY | GMC_SRC_PITCH_OFFSET | GMC_BRUSH(BRUSH_NONE),
        PITCH_OFFSET(SRC_OFF, PITCH),
        XY(0, 0), XY(100, 0), XY(16, 4),
        XY(0, 10), XY(100, 50), XY(32, 8),
    };
    static const int first[][2] = { { 0, 0 }, { 15, 3 } };
    static const int second[][2] = { { 0, 10 }, { 31, 17 } };
    R350 r;

    r350_start(&r);
    qpci_io_writel(r.dev, r.mmio, DEFAULT_OFFSET,
                   PITCH_OFFSET(DST_OFF, PITCH));
    seed(&r, SRC_OFF, PITCH, first, ARRAY_SIZE(first));
    seed(&r, SRC_OFF, PITCH, second, ARRAY_SIZE(second));
    push(&r, pkt, ARRAY_SIZE(pkt));
    assert_copied(&r, DST_OFF, PITCH, first, ARRAY_SIZE(first), 100, 0);
    assert_copied(&r, DST_OFF, PITCH, second, ARRAY_SIZE(second), 100, 40);
    r350_stop(&r);
}

/* (d) Mac OS X 10.4's pointer save: both pitch/offsets, one rectangle */
static void test_bitblt_multi_pointer(void)
{
    static const uint32_t pkt[] = {
        PKT3(OP_BITBLT_MULTI, 6),
        GMC_COPY | GMC_SRC_PITCH_OFFSET | GMC_DST_PITCH_OFFSET |
        GMC_BRUSH(BRUSH_NONE),
        PITCH_OFFSET(SRC_OFF, PITCH), PITCH_OFFSET(DST_OFF, PITCH),
        XY(40, 20), XY(0, 0), XY(16, 16),
    };
    static const int pts[][2] = { { 40, 20 }, { 55, 35 }, { 47, 27 } };
    R350 r;

    r350_start(&r);
    seed(&r, SRC_OFF, PITCH, pts, ARRAY_SIZE(pts));
    push(&r, pkt, ARRAY_SIZE(pkt));
    assert_copied(&r, DST_OFF, PITCH, pts, ARRAY_SIZE(pts), -40, -20);
    r350_stop(&r);
}

/*
 * (e) Every optional SETUP_BODY dword at once, in the order of the
 * Radeon R5xx Acceleration guide (6.2.2): SRC_SC_BOT_RITE, SC_TOP_LEFT,
 * SC_BOT_RITE, the solid brush's colour and BRUSH_Y_X, then the
 * rectangle. The brush colour lands in its register.
 */
static void test_bitblt_multi_setup_body(void)
{
    static const uint32_t pkt[] = {
        PKT3(OP_BITBLT_MULTI, 11),
        GMC_COPY | GMC_SRC_PITCH_OFFSET | GMC_DST_PITCH_OFFSET |
        GMC_SRC_CLIPPING | GMC_DST_CLIPPING |
        GMC_BRUSH(BRUSH_SOLID_COLOR) | GMC_LD_BRUSH_Y_X,
        PITCH_OFFSET(SRC_OFF, PITCH), PITCH_OFFSET(DST_OFF, PITCH),
        SC(255, 255),                   /* SRC_SC_BOT_RITE */
        SC(0, 0), SC(255, 255),         /* SC_TOP_LEFT, SC_BOT_RITE */
        0x00ff00ff,                     /* FRGD_COLOR */
        0x00000000,                     /* BRUSH_Y_X */
        XY(8, 8), XY(64, 32), XY(16, 16),
    };
    static const int pts[][2] = { { 8, 8 }, { 23, 23 }, { 15, 15 } };
    R350 r;

    r350_start(&r);
    seed(&r, SRC_OFF, PITCH, pts, ARRAY_SIZE(pts));
    push(&r, pkt, ARRAY_SIZE(pkt));
    assert_copied(&r, DST_OFF, PITCH, pts, ARRAY_SIZE(pts), 56, 24);
    g_assert_cmphex(qpci_io_readl(r.dev, r.mmio, DP_BRUSH_FRGD_CLR), ==,
                    0x00ff00ff);
    r350_stop(&r);
}

/* (f) A dword past the last whole rectangle does not lose the rectangle */
static void test_bitblt_multi_trailing(void)
{
    static const uint32_t pkt[] = {
        PKT3(OP_BITBLT_MULTI, 7),
        GMC_COPY | GMC_SRC_PITCH_OFFSET | GMC_DST_PITCH_OFFSET |
        GMC_BRUSH(BRUSH_NONE),
        PITCH_OFFSET(SRC_OFF, PITCH), PITCH_OFFSET(DST_OFF, PITCH),
        XY(40, 20), XY(0, 0), XY(16, 16),
        0xdeadbeef,
    };
    static const int pts[][2] = { { 40, 20 }, { 55, 35 } };
    R350 r;

    r350_start(&r);
    seed(&r, SRC_OFF, PITCH, pts, ARRAY_SIZE(pts));
    push(&r, pkt, ARRAY_SIZE(pkt));
    assert_copied(&r, DST_OFF, PITCH, pts, ARRAY_SIZE(pts), -40, -20);
    r350_stop(&r);
}

/*
 * (g) A brush type whose packet size the guide does not give (4 is
 * reserved there): the rectangles cannot be located, so nothing is
 * drawn -- and the packet after it, in the same stream, is still
 * parsed from its own header.
 */
static void test_bitblt_multi_unsized_brush(void)
{
    static const uint32_t pkt[] = {
        PKT3(OP_BITBLT_MULTI, 6),
        GMC_COPY | GMC_SRC_PITCH_OFFSET | GMC_DST_PITCH_OFFSET |
        GMC_BRUSH(4),
        PITCH_OFFSET(SRC_OFF, PITCH), PITCH_OFFSET(DST_OFF, PITCH),
        XY(40, 20), XY(0, 0), XY(16, 16),
        PKT3(OP_BITBLT_MULTI, 6),
        GMC_COPY | GMC_SRC_PITCH_OFFSET | GMC_DST_PITCH_OFFSET |
        GMC_BRUSH(BRUSH_NONE),
        PITCH_OFFSET(SRC_OFF, PITCH), PITCH_OFFSET(DST_OFF, PITCH),
        XY(40, 20), XY(100, 50), XY(16, 16),
    };
    static const int pts[][2] = { { 40, 20 }, { 55, 35 } };
    R350 r;

    r350_start(&r);
    seed(&r, SRC_OFF, PITCH, pts, ARRAY_SIZE(pts));
    push(&r, pkt, ARRAY_SIZE(pkt));
    assert_untouched(&r, DST_OFF, PITCH, pts, ARRAY_SIZE(pts), -40, -20);
    assert_copied(&r, DST_OFF, PITCH, pts, ARRAY_SIZE(pts), 60, 30);
    r350_stop(&r);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_device("ati-radeon9800")) {
        return g_test_run();
    }
    qtest_add_func("/ati-r350/bitblt-multi/shell", test_bitblt_multi_shell);
    qtest_add_func("/ati-r350/bitblt-multi/scissor",
                   test_bitblt_multi_scissor);
    qtest_add_func("/ati-r350/bitblt-multi/run", test_bitblt_multi_run);
    qtest_add_func("/ati-r350/bitblt-multi/pointer",
                   test_bitblt_multi_pointer);
    qtest_add_func("/ati-r350/bitblt-multi/setup-body",
                   test_bitblt_multi_setup_body);
    qtest_add_func("/ati-r350/bitblt-multi/trailing",
                   test_bitblt_multi_trailing);
    qtest_add_func("/ati-r350/bitblt-multi/unsized-brush",
                   test_bitblt_multi_unsized_brush);
    return g_test_run();
}
