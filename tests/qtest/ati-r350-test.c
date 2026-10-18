/*
 * QTest testcase for the ATI R350 (Radeon 9800) 2D packets, the indexed
 * 3D draw packets, the hardware cursor and the EDID
 *
 * Copyright (c) 2026 The QEMU Project Developers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/libqos-spapr.h"
#include "libqos/pci.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "hw/display/edid.h"

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
#define OP_NOP3         0x10
#define OP_INDX_BUFFER  0x33
#define OP_DRAW_INDX_2  0x36

/* INDX_BUFFER names the port it feeds as a dword register index */
#define VAP_PORT_IDX0   (0x2040 >> 2)
/* VAP_VF_CNTL: triangle list, PRIM_WALK=1 (indices), NUM_VERTICES */
#define VF_IDX32        (1u << 11)
#define VF(n, idx32)    (((uint32_t)(n) << 16) | ((idx32) ? VF_IDX32 : 0) | \
                         (1u << 4) | 4)

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

/*
 * @async_display picks async-display=on / off; @hwcursor, when set, adds
 * guest_hwcursor=on (the property is left out otherwise, so a build without
 * it still runs the cases that do not need it).
 */
static void r350_start_cfg(R350 *r, bool async_display, bool hwcursor)
{
    QPCIDevice *dev = NULL;

    r->qs = qtest_spapr_boot("-vga none -device ati-radeon9800,"
                             "async-engine=off,async-display=%s%s",
                             async_display ? "on" : "off",
                             hwcursor ? ",guest_hwcursor=on" : "");
    qpci_device_foreach(r->qs->pcibus, R350_VENDOR_ID, R350_DEVICE_ID,
                        find_r350, &dev);
    g_assert(dev);
    r->dev = dev;
    r->vram = qpci_iomap(dev, VRAM_BAR, NULL);
    r->mmio = qpci_iomap(dev, MMIO_BAR, NULL);
    qpci_device_enable(dev);

    qpci_io_writel(dev, r->mmio, DEFAULT_SC_BOTTOM_RIGHT, 0x1fff1fff);
}

static void r350_start(R350 *r)
{
    r350_start_cfg(r, false, false);
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

/*
 * Hardware cursor drawn into the console surface (guest_hwcursor, #451).
 *
 * The scanout is a small 32bpp mode; the picture is read back with
 * screendump (PPM), so what is compared is the console surface itself, not
 * the cursor handed to a UI. Every case runs with async-display=off (the
 * conversion and the overlay on the main loop) and =on (the worker does
 * both).
 */
#define CRTC_GEN_CNTL           0x0050
#define CRTC_EXT_CNTL           0x0054
#define CRTC_H_TOTAL_DISP       0x0200
#define CRTC_V_TOTAL_DISP       0x0208
#define CRTC_OFFSET             0x0224
#define CRTC_PITCH              0x022c
#define CUR_OFFSET              0x0260
#define CUR_HORZ_VERT_POSN      0x0264
#define CUR_HORZ_VERT_OFF       0x0268

#define GEN_CRTC_EXT_DISP_EN    (1u << 24)
#define GEN_CRTC_EN             (1u << 25)
#define GEN_PIX_WIDTH_32BPP     (6u << 8)
#define GEN_CUR_EN              (1u << 16)
#define GEN_CUR_MODE(m)         ((uint32_t)(m) << 20)
#define CUR_LOCK                (1u << 31)

#define SCR_W           160
#define SCR_H           112
#define FB_OFF          0x100000
/* the cursor images sit outside the scanned-out range */
#define IMG_A           0x10000
#define IMG_B           0x18000
#define CUR_SIZE        64

/* mode 1 (ARGB coded): bit 31 set and the low 24 bits not all ones */
#define TRANSP1         0x80000000u
#define INVERT1         0x80ffffffu

typedef struct {
    bool async;
    bool hw;
} Cfg;

typedef struct {
    uint32_t px[SCR_W * SCR_H];         /* 0x00RRGGBB */
} Screen;

/* a cursor image dword at (x, y) of the 64x64 map, as the guest stores it */
typedef struct {
    int x, y;
    uint32_t v;
} CPx;

static void wr(R350 *r, uint32_t reg, uint32_t v)
{
    qpci_io_writel(r->dev, r->mmio, reg, v);
}

/*
 * R is always odd and G always odd, so no background pixel is pure red,
 * pure blue or any colour the cases draw with an even R
 */
static uint32_t bg_color(int x, int y)
{
    return ((((x * 2 + 1) & 0xff) << 16) | (((y * 2 + 3) & 0xff) << 8) |
            (((x ^ y) * 3 + 7) & 0xff));
}

static Screen *screen_new(void)
{
    Screen *s = g_new0(Screen, 1);

    for (int y = 0; y < SCR_H; y++) {
        for (int x = 0; x < SCR_W; x++) {
            s->px[y * SCR_W + x] = bg_color(x, y);
        }
    }
    return s;
}

static void scr_put(Screen *s, int x, int y, uint32_t c)
{
    if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H) {
        s->px[y * SCR_W + x] = c & 0xffffff;
    }
}

static void vram_write32(R350 *r, uint64_t off, const uint32_t *dw, size_t n)
{
    uint32_t *le = g_new(uint32_t, n);

    for (size_t i = 0; i < n; i++) {
        le[i] = cpu_to_le32(dw[i]);
    }
    qpci_memwrite(r->dev, r->vram, off, le, n * 4);
    g_free(le);
}

/* the whole scanned-out frame, from @s */
static void fb_write(R350 *r, const Screen *s)
{
    vram_write32(r, FB_OFF, s->px, SCR_W * SCR_H);
}

/* change one scanned-out pixel in VRAM and in the expectation */
static void bg_set(R350 *r, Screen *s, int x, int y, uint32_t c)
{
    vram_write32(r, FB_OFF + (uint64_t)(y * SCR_W + x) * 4, &c, 1);
    scr_put(s, x, y, c);
}

static void scr_mode(R350 *r)
{
    /* the reset value has CRTC_DISPLAY_DIS set; the mode is not valid so */
    wr(r, CRTC_EXT_CNTL, 0);
    wr(r, CRTC_H_TOTAL_DISP, ((SCR_W / 8 - 1) << 16) | (SCR_W / 8 - 1));
    wr(r, CRTC_V_TOTAL_DISP, ((SCR_H - 1) << 16) | (SCR_H - 1));
    wr(r, CRTC_PITCH, SCR_W / 8);
    wr(r, CRTC_OFFSET, FB_OFF);
    wr(r, CRTC_GEN_CNTL, GEN_CRTC_EN | GEN_CRTC_EXT_DISP_EN |
                         GEN_PIX_WIDTH_32BPP);
}

/* the 1ms cursor timer runs on the virtual clock */
static void settle(R350 *r)
{
    qtest_clock_step(r->qs->qts, 2 * 1000 * 1000);
}

static void screendump(R350 *r, uint32_t *out)
{
    g_autofree char *name = NULL;
    g_autofree char *data = NULL;
    GError *err = NULL;
    QDict *rsp;
    gsize len;
    int w, h, hdr = 0, fd;

    fd = g_file_open_tmp("ati-r350-XXXXXX.ppm", &name, &err);
    g_assert_no_error(err);
    close(fd);
    rsp = qtest_qmp(r->qs->qts, "{'execute': 'screendump',"
                    " 'arguments': {'filename': %s}}", name);
    g_assert(!qdict_haskey(rsp, "error"));
    qobject_unref(rsp);
    g_assert(g_file_get_contents(name, &data, &len, &err));
    g_assert_no_error(err);
    unlink(name);

    g_assert_cmpint(sscanf(data, "P6\n%d %d\n255\n%n", &w, &h, &hdr), ==, 2);
    g_assert_cmpint(w, ==, SCR_W);
    g_assert_cmpint(h, ==, SCR_H);
    g_assert_cmpuint(len, ==, hdr + (gsize)w * h * 3);
    for (int i = 0; i < w * h; i++) {
        const uint8_t *p = (const uint8_t *)data + hdr + i * 3;

        out[i] = (p[0] << 16) | (p[1] << 8) | p[2];
    }
}

static bool near(uint32_t a, uint32_t b, int tol)
{
    for (int sh = 0; sh < 24; sh += 8) {
        int d = (int)((a >> sh) & 0xff) - (int)((b >> sh) & 0xff);

        if (d > tol || d < -tol) {
            return false;
        }
    }
    return true;
}

/* the picture is exactly @exp (each channel within @tol) */
static void assert_screen(R350 *r, const Screen *exp, int tol)
{
    uint32_t *got = g_new(uint32_t, SCR_W * SCR_H);
    int bad = 0;

    screendump(r, got);
    for (int y = 0; y < SCR_H; y++) {
        for (int x = 0; x < SCR_W; x++) {
            uint32_t g = got[y * SCR_W + x], e = exp->px[y * SCR_W + x];

            if (!near(g, e, tol)) {
                if (bad++ < 8) {
                    g_printerr("screen (%d,%d): got 0x%06x want 0x%06x\n",
                               x, y, g, e);
                }
            }
        }
    }
    g_assert_cmpint(bad, ==, 0);
    g_free(got);
}

/* the 64x64 map: every dword @fill, then the listed ones */
static void cur_image(R350 *r, uint32_t vram_off, uint32_t fill,
                      const CPx *pts, size_t n)
{
    uint32_t img[CUR_SIZE * CUR_SIZE];

    for (int i = 0; i < CUR_SIZE * CUR_SIZE; i++) {
        img[i] = fill;
    }
    for (size_t i = 0; i < n; i++) {
        img[pts[i].y * CUR_SIZE + pts[i].x] = pts[i].v;
    }
    vram_write32(r, vram_off, img, CUR_SIZE * CUR_SIZE);
}

/*
 * Program the cursor as the guest does: image, hidden columns/rows,
 * position (CUR_HORZ_VERT_POSN carries the position *before* the hidden
 * columns are taken off), then the enable and format in CRTC_GEN_CNTL.
 * @lock is OR-ed into every cursor register: CUR_LOCK is one bit shared by
 * the three, set or cleared by the last write to any of them.
 */
static void cur_prog(R350 *r, unsigned mode, uint32_t vram_off, int hx, int y,
                     unsigned hoff, unsigned voff, bool en, uint32_t lock)
{
    wr(r, CUR_OFFSET, vram_off | lock);
    wr(r, CUR_HORZ_VERT_OFF, ((hoff << 16) | voff) | lock);
    wr(r, CUR_HORZ_VERT_POSN, (((uint32_t)hx << 16) | y) | lock);
    wr(r, CRTC_GEN_CNTL, GEN_CRTC_EN | GEN_CRTC_EXT_DISP_EN |
                         GEN_PIX_WIDTH_32BPP | GEN_CUR_MODE(mode) |
                         (en ? GEN_CUR_EN : 0));
    settle(r);
}

/*
 * Opaque cursor pixels onto the expected picture. @x is where map column 0
 * lands (position minus the hidden columns); the first @hoff columns and
 * the last @voff rows are not part of the cursor.
 */
static void exp_cur(Screen *e, int x, int y, const CPx *pts, size_t n,
                    unsigned hoff, unsigned voff)
{
    for (size_t i = 0; i < n; i++) {
        if (pts[i].x >= hoff && pts[i].y < CUR_SIZE - voff) {
            scr_put(e, x + pts[i].x, y + pts[i].y, pts[i].v);
        }
    }
}

/* mode 1 cursor shape used by (a), (b), (c), (d), (h), (i) */
static const CPx shape_a[] = {
    { 0, 0, 0x00402010 }, { 5, 3, 0x00806040 },
    { 10, 20, 0x00c8d8e8 }, { 63, 63, 0x00a0b0c0 },
};
static const CPx shape_b[] = {
    { 1, 1, 0x00e04020 }, { 30, 2, 0x00204060 }, { 62, 40, 0x0060a0e0 },
};
static const CPx shape_c[] = {         /* what (h) leaves in the old image */
    { 0, 0, 0x00020406 }, { 4, 4, 0x0008090a }, { 63, 0, 0x000c0e10 },
};
static const CPx shape_d[] = {
    { 3, 60, 0x00504030 }, { 20, 20, 0x00907050 }, { 40, 5, 0x00b0c0d0 },
};

#define N(a) ARRAY_SIZE(a)
#define CX 10
#define CY 6

static void scr_start(R350 *r, const Cfg *c, Screen **exp)
{
    r350_start_cfg(r, c->async, c->hw);
    scr_mode(r);
    *exp = screen_new();
    fb_write(r, *exp);
    /* the mode is valid and the background is what was written */
    assert_screen(r, *exp, 0);
}

static void scr_stop(R350 *r, Screen *exp)
{
    g_free(exp);
    r350_stop(r);
}

/* nothing but the background: the part that needs no guest_hwcursor */
static void cur_bg(const Cfg *c)
{
    R350 r;
    Screen *exp;

    scr_start(&r, c, &exp);
    scr_stop(&r, exp);
}

/*
 * (a) opaque pixels land at (X+px, Y+py); transparent ones show the picture;
 * a full conversion of the picture (a scanned-out pixel written) does not
 * take the cursor off again
 */
static void cur_a(const Cfg *c)
{
    R350 r;
    Screen *exp;

    scr_start(&r, c, &exp);
    cur_image(&r, IMG_A, TRANSP1, shape_a, N(shape_a));
    cur_prog(&r, 1, IMG_A, CX, CY, 0, 0, true, 0);
    exp_cur(exp, CX, CY, shape_a, N(shape_a), 0, 0);
    assert_screen(&r, exp, 0);

    bg_set(&r, exp, 2, 100, 0x445566);
    settle(&r);
    assert_screen(&r, exp, 0);
    scr_stop(&r, exp);
}

/* (b) moving restores everything under the old 64x64 and draws anew */
static void cur_b(const Cfg *c)
{
    R350 r;
    Screen *exp, *moved;

    scr_start(&r, c, &exp);
    cur_image(&r, IMG_A, TRANSP1, shape_a, N(shape_a));
    cur_prog(&r, 1, IMG_A, CX, CY, 0, 0, true, 0);
    exp_cur(exp, CX, CY, shape_a, N(shape_a), 0, 0);
    assert_screen(&r, exp, 0);

    moved = screen_new();
    /* 10..73 and 90..153 do not overlap */
    cur_prog(&r, 1, IMG_A, 90, 40, 0, 0, true, 0);
    exp_cur(moved, 90, 40, shape_a, N(shape_a), 0, 0);
    assert_screen(&r, moved, 0);
    g_free(moved);
    scr_stop(&r, exp);
}

/* (c) CUR_EN down: the whole picture is the background again */
static void cur_c(const Cfg *c)
{
    R350 r;
    Screen *exp, *bare;

    scr_start(&r, c, &exp);
    cur_image(&r, IMG_A, TRANSP1, shape_a, N(shape_a));
    cur_prog(&r, 1, IMG_A, CX, CY, 0, 0, true, 0);
    exp_cur(exp, CX, CY, shape_a, N(shape_a), 0, 0);
    assert_screen(&r, exp, 0);

    bare = screen_new();
    cur_prog(&r, 1, IMG_A, CX, CY, 0, 0, false, 0);
    assert_screen(&r, bare, 0);
    g_free(bare);
    scr_stop(&r, exp);
}

/* (d) guest_hwcursor off (the default): the cursor is never drawn */
static void cur_d(const Cfg *c)
{
    R350 r;
    Screen *exp;

    g_assert(!c->hw);
    scr_start(&r, c, &exp);
    cur_image(&r, IMG_A, TRANSP1, shape_a, N(shape_a));
    cur_prog(&r, 1, IMG_A, CX, CY, 0, 0, true, 0);
    assert_screen(&r, exp, 0);
    scr_stop(&r, exp);
}

/* (e) red and blue come out as red and blue, in mode 1 and in mode 3 */
static void cur_e(const Cfg *c)
{
    static const CPx m1[] = { { 1, 1, 0x00ff0000 }, { 2, 1, 0x000000ff } };
    static const CPx m3[] = { { 1, 3, 0xffff0000 }, { 2, 3, 0xff0000ff } };
    R350 r;
    Screen *exp, *two;

    scr_start(&r, c, &exp);
    cur_image(&r, IMG_A, TRANSP1, m1, N(m1));
    cur_prog(&r, 1, IMG_A, CX, CY, 0, 0, true, 0);
    scr_put(exp, CX + 1, CY + 1, 0xff0000);
    scr_put(exp, CX + 2, CY + 1, 0x0000ff);
    assert_screen(&r, exp, 0);

    two = screen_new();
    cur_image(&r, IMG_B, 0, m3, N(m3));
    cur_prog(&r, 3, IMG_B, CX, CY, 0, 0, true, 0);
    scr_put(two, CX + 1, CY + 3, 0xff0000);
    scr_put(two, CX + 2, CY + 3, 0x0000ff);
    assert_screen(&r, two, 0);
    g_free(two);
    scr_stop(&r, exp);
}

/*
 * (f) mode 3 alpha 0x80 black over white is mid grey (not an inversion);
 * a mode 1 inverting code over (0x12,0x34,0x56) is 0xedcba9. Mac OS X's
 * arrow has edge pixels such as 0x5fe4e4e4 whose colour exceeds their
 * alpha, so mode 3 is straight alpha: over black that is 228 * 95 / 255
 * = 0x55, where a premultiplied blend would give 0xe4.
 */
static void cur_f(const Cfg *c)
{
    static const CPx m3[] = { { 2, 2, 0x80000000 }, { 6, 2, 0x5fe4e4e4 } };
    static const CPx m1[] = { { 4, 4, INVERT1 } };
    R350 r;
    Screen *exp;

    scr_start(&r, c, &exp);
    bg_set(&r, exp, CX + 2, CY + 2, 0xffffff);
    bg_set(&r, exp, CX + 4, CY + 4, 0x123456);
    bg_set(&r, exp, CX + 6, CY + 2, 0x000000);
    assert_screen(&r, exp, 0);

    cur_image(&r, IMG_A, 0, m3, N(m3));
    cur_prog(&r, 3, IMG_A, CX, CY, 0, 0, true, 0);
    scr_put(exp, CX + 2, CY + 2, 0x7f7f7f);
    scr_put(exp, CX + 6, CY + 2, 0x555555);
    assert_screen(&r, exp, 1);

    cur_image(&r, IMG_B, TRANSP1, m1, N(m1));
    cur_prog(&r, 1, IMG_B, CX, CY, 0, 0, true, 0);
    scr_put(exp, CX + 2, CY + 2, 0xffffff);
    scr_put(exp, CX + 6, CY + 2, 0x000000);
    scr_put(exp, CX + 4, CY + 4, 0xedcba9);
    assert_screen(&r, exp, 0);
    scr_stop(&r, exp);
}

/*
 * (g) hidden columns/rows and the screen edges. The first arm is the shape
 * the guest writes to push the cursor off the top-left: position (0,0),
 * CUR_HORZ_OFF = 4 columns, CUR_VERT_OFF = hidden rows + 1 with CUR_OFFSET
 * advanced by 256 bytes per hidden row (the map then starts at the first
 * visible row and its last CUR_VERT_OFF rows are not part of the cursor).
 */
static void cur_g(const Cfg *c)
{
    /* map coordinates, counted from the advanced CUR_OFFSET */
    static const CPx tl[] = {
        { 4, 0, 0x00402010 },   /* first visible column and row -> (0,0) */
        { 3, 2, 0x00806040 },   /* hidden column, would land at x = -1 */
        { 10, 5, 0x00c8d8e8 },
        { 63, 58, 0x00a0b0c0 }, /* last visible row */
        { 20, 58, 0x00e0d0c0 },
        { 20, 59, 0x00101010 }, /* the last CUR_VERT_OFF rows are not drawn */
        { 20, 63, 0x00303030 },
    };
    static const CPx br[] = {
        { 0, 0, 0x00402010 }, { 9, 9, 0x00806040 },
        { 10, 10, 0x00c8d8e8 }, { 30, 2, 0x00a0b0c0 }, { 63, 63, 0x00e0d0c0 },
    };
    R350 r;
    Screen *exp, *e2, *e3;

    scr_start(&r, c, &exp);
    cur_image(&r, IMG_A + 0x400, TRANSP1, tl, N(tl));
    cur_prog(&r, 1, IMG_A + 0x400, 0, 0, 4, 5, true, 0);
    exp_cur(exp, -4, 0, tl, N(tl), 4, 5);
    assert_screen(&r, exp, 0);

    /* the same shape further in: the hidden columns land on the screen */
    e2 = screen_new();
    cur_prog(&r, 1, IMG_A + 0x400, 30, 20, 4, 5, true, 0);
    exp_cur(e2, 26, 20, tl, N(tl), 4, 5);
    assert_screen(&r, e2, 0);
    g_free(e2);

    /* only a corner is on the screen */
    e3 = screen_new();
    cur_image(&r, IMG_B, TRANSP1, br, N(br));
    cur_prog(&r, 1, IMG_B, SCR_W - 10, SCR_H - 10, 0, 0, true, 0);
    exp_cur(e3, SCR_W - 10, SCR_H - 10, br, N(br), 0, 0);
    assert_screen(&r, e3, 0);
    g_free(e3);
    scr_stop(&r, exp);
}

/*
 * (h) under CUR_LOCK a new position and image are not taken up, not even
 * by a full conversion of the picture; unlocking takes them up.
 */
static void cur_h(const Cfg *c)
{
    R350 r;
    Screen *exp, *locked, *after;

    scr_start(&r, c, &exp);
    cur_image(&r, IMG_A, TRANSP1, shape_a, N(shape_a));
    cur_prog(&r, 1, IMG_A, CX, CY, 0, 0, true, 0);
    exp_cur(exp, CX, CY, shape_a, N(shape_a), 0, 0);
    assert_screen(&r, exp, 0);

    /* lock, write a new image and position, overwrite the old image too */
    cur_image(&r, IMG_B, TRANSP1, shape_b, N(shape_b));
    cur_prog(&r, 1, IMG_B, 90, 40, 0, 0, true, CUR_LOCK);
    cur_image(&r, IMG_A, TRANSP1, shape_c, N(shape_c));
    settle(&r);
    assert_screen(&r, exp, 0);

    /* a write into the scanned-out range converts the whole picture */
    locked = screen_new();
    bg_set(&r, locked, 2, 100, 0x445566);
    exp_cur(locked, CX, CY, shape_a, N(shape_a), 0, 0);
    settle(&r);
    assert_screen(&r, locked, 0);
    g_free(locked);

    /* unlock: one write without the bit */
    wr(&r, CUR_HORZ_VERT_POSN, (90u << 16) | 40);
    settle(&r);
    after = screen_new();
    bg_set(&r, after, 2, 100, 0x445566);
    exp_cur(after, 90, 40, shape_b, N(shape_b), 0, 0);
    assert_screen(&r, after, 0);
    g_free(after);
    scr_stop(&r, exp);
}

/* (i) the image changes in place, the position does not */
static void cur_i(const Cfg *c)
{
    R350 r;
    Screen *exp, *changed;

    scr_start(&r, c, &exp);
    cur_image(&r, IMG_A, TRANSP1, shape_a, N(shape_a));
    cur_prog(&r, 1, IMG_A, CX, CY, 0, 0, true, 0);
    exp_cur(exp, CX, CY, shape_a, N(shape_a), 0, 0);
    assert_screen(&r, exp, 0);

    cur_image(&r, IMG_A, TRANSP1, shape_d, N(shape_d));
    wr(&r, CUR_HORZ_VERT_POSN, ((uint32_t)CX << 16) | CY);
    settle(&r);
    changed = screen_new();
    exp_cur(changed, CX, CY, shape_d, N(shape_d), 0, 0);
    assert_screen(&r, changed, 0);
    g_free(changed);
    scr_stop(&r, exp);
}

typedef struct {
    const char *name;
    void (*fn)(const Cfg *);
    bool hw;
} CurCase;

static const CurCase cur_cases[] = {
    { "bg", cur_bg, false },
    { "a", cur_a, true },
    { "b", cur_b, true },
    { "c", cur_c, true },
    { "d", cur_d, false },
    { "e", cur_e, true },
    { "f", cur_f, true },
    { "g", cur_g, true },
    { "h", cur_h, true },
    { "i", cur_i, true },
};

typedef struct {
    const CurCase *cc;
    bool async;
} CurArg;

static void run_cur(gconstpointer data)
{
    const CurArg *a = data;
    Cfg cfg = { .async = a->async, .hw = a->cc->hw };

    a->cc->fn(&cfg);
}

static void add_cursor_tests(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(cur_cases); i++) {
        for (int async = 0; async < 2; async++) {
            CurArg *a = g_new0(CurArg, 1);
            g_autofree char *path = g_strdup_printf(
                "/ati-r350/cursor/%s/async-%s", cur_cases[i].name,
                async ? "on" : "off");

            a->cc = &cur_cases[i];
            a->async = async;
            qtest_add_data_func(path, a, run_cur);
        }
    }
}

/*
 * EDID (#454). The fixed default is an Apple-style 17" CRT; any of the
 * standard EDID properties switches to qemu_edid_generate(), presented as
 * an analog monitor. Read back
 * through the hardware I2C engine (DDC address 0xA0/0xA1).
 */
#define I2C_CNTL_0              0x0090
#define I2C_CNTL_1              0x0094
#define I2C_DATA                0x0098
#define I2C_DONE                (1u << 0)
#define I2C_NACK                (1u << 1)
#define I2C_SOFT_RST            (1u << 5)
#define I2C_RECEIVE             (1u << 10)
#define I2C_GO                  (1u << 12)

static const uint8_t r350_fixed_edid[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
    0x06, 0x10, 0x75, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x0c, 0x09, 0x01, 0x01, 0x08, 0x20, 0x18, 0x78,
    0x0a, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26,
    0x0f, 0x50, 0x54, 0x31, 0x6b, 0x80, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x10, 0x27,
    0x80, 0x30, 0x41, 0x66, 0x2d, 0x30, 0x40, 0x80,
    0x13, 0x00, 0x40, 0xf0, 0x10, 0x00, 0x00, 0x1e,
    0x00, 0x00, 0x00, 0xfd, 0x00, 0x32, 0x4b, 0x1e,
    0x46, 0x0a, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x4d,
    0x75, 0x6c, 0x74, 0x69, 0x70, 0x6c, 0x65, 0x20,
    0x53, 0x63, 0x61, 0x6e, 0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x76,
};

static void r350_start_extra(R350 *r, const char *extra)
{
    QPCIDevice *dev = NULL;

    r->qs = qtest_spapr_boot("-vga none -device ati-radeon9800,"
                             "async-engine=off,async-display=off%s", extra);
    qpci_device_foreach(r->qs->pcibus, R350_VENDOR_ID, R350_DEVICE_ID,
                        find_r350, &dev);
    g_assert(dev);
    r->dev = dev;
    r->vram = qpci_iomap(dev, VRAM_BAR, NULL);
    r->mmio = qpci_iomap(dev, MMIO_BAR, NULL);
    qpci_device_enable(dev);
}

static void i2c_go(R350 *r, uint8_t addr, uint8_t count, uint32_t extra)
{
    uint32_t cntl0;

    qpci_io_writel(r->dev, r->mmio, I2C_CNTL_1, ((uint32_t)addr << 8) | count);
    qpci_io_writel(r->dev, r->mmio, I2C_CNTL_0, I2C_GO | extra);
    cntl0 = qpci_io_readl(r->dev, r->mmio, I2C_CNTL_0);
    g_assert_cmphex(cntl0 & (I2C_DONE | I2C_NACK), ==, I2C_DONE);
}

static void read_edid(R350 *r, uint8_t *edid)
{
    qpci_io_writel(r->dev, r->mmio, I2C_CNTL_0, I2C_SOFT_RST);
    qpci_io_writel(r->dev, r->mmio, I2C_DATA, 0);       /* word offset 0 */
    i2c_go(r, 0xa0, 1, 0);
    for (int off = 0; off < 128; off += 16) {
        i2c_go(r, 0xa1, 16, I2C_RECEIVE);
        for (int i = 0; i < 16; i++) {
            edid[off + i] = qpci_io_readl(r->dev, r->mmio, I2C_DATA) & 0xff;
        }
    }
}

static void test_edid_default(void)
{
    R350 r;
    uint8_t edid[128];

    r350_start_extra(&r, "");
    read_edid(&r, edid);
    r350_stop(&r);
    g_assert_cmpmem(edid, sizeof(edid), r350_fixed_edid,
                    sizeof(r350_fixed_edid));
}

typedef struct {
    const char *name;
    const char *props;          /* appended to -device */
    qemu_edid_info info;        /* the same values, for the generator */
} EdidCase;

static const EdidCase edid_cases[] = {
    { "xres-yres", ",xres=1920,yres=1200", { .prefx = 1920, .prefy = 1200 } },
    { "xmax", ",xmax=1024", { .maxx = 1024 } },
    { "ymax", ",ymax=768", { .maxy = 768 } },
    { "refresh-rate", ",refresh_rate=60000", { .refresh_rate = 60000 } },
};

/* The R350 turns the generated (digital) EDID into an analog RGB one */
static void edid_make_analog(uint8_t *edid)
{
    unsigned sum = 0;

    edid[20] = 0x08;
    edid[24] = (edid[24] & 0xe7) | 0x08;
    for (int i = 0; i < 127; i++) {
        sum += edid[i];
    }
    edid[127] = (256 - (sum & 0xff)) & 0xff;
}

/* Structure checks that do not lean on the generator */
static void check_edid_structure(const uint8_t *edid, uint32_t w, uint32_t h)
{
    static const uint8_t header[8] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
    };
    const uint8_t *dtd = edid + 54;
    unsigned sum = 0;
    bool ranges = false;

    g_assert_cmpmem(edid, 8, header, 8);
    /* analog input (bit 7 clear) and an RGB colour display */
    g_assert_cmphex(edid[20] & 0x80, ==, 0);
    g_assert_cmphex(edid[24] & 0x18, ==, 0x08);
    for (int i = 0; i < 128; i++) {
        sum += edid[i];
    }
    g_assert_cmpuint(sum & 0xff, ==, 0);
    /* the first descriptor is the preferred mode's detailed timing */
    g_assert_cmpuint(dtd[0] | dtd[1], !=, 0);
    g_assert_cmpuint(dtd[2] | ((dtd[4] & 0xf0) << 4), ==, w);
    g_assert_cmpuint(dtd[5] | ((dtd[7] & 0xf0) << 4), ==, h);
    for (int d = 72; d < 126; d += 18) {
        if (!edid[d] && !edid[d + 1] && edid[d + 3] == 0xfd) {
            ranges = true;
        }
    }
    g_assert_true(ranges);
}

static void run_edid_case(const void *opaque)
{
    const EdidCase *c = opaque;
    qemu_edid_info info = c->info;
    uint8_t want[128] = { 0 };
    uint8_t edid[128];
    R350 r;

    qemu_edid_generate(want, sizeof(want), &info);
    edid_make_analog(want);
    r350_start_extra(&r, c->props);
    read_edid(&r, edid);
    r350_stop(&r);
    g_assert_cmpmem(edid, sizeof(edid), want, sizeof(want));
    check_edid_structure(edid, c->info.prefx ? c->info.prefx : 1280,
                         c->info.prefy ? c->info.prefy : 800);
}

/* xres or yres alone would give e.g. 1920x800: realize refuses it */
static void test_edid_half_pref(void)
{
    if (g_test_subprocess()) {
        R350 r;

        r350_start_extra(&r, g_getenv("R350_EDID_PROPS"));
        r350_stop(&r);
        return;
    }
    g_setenv("R350_EDID_PROPS", ",xres=1920", true);
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*xres and yres*");
    g_setenv("R350_EDID_PROPS", ",yres=1200", true);
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*xres and yres*");
    g_unsetenv("R350_EDID_PROPS");
}

static void add_edid_tests(void)
{
    qtest_add_func("/ati-r350/edid/default", test_edid_default);
    for (size_t i = 0; i < G_N_ELEMENTS(edid_cases); i++) {
        g_autofree char *path = g_strdup_printf("/ati-r350/edid/%s",
                                                edid_cases[i].name);
        qtest_add_data_func(path, &edid_cases[i], run_edid_case);
    }
    qtest_add_func("/ati-r350/edid/half-pref", test_edid_half_pref);
}

/*
 * Indexed 3D draws. The tests do not set up the vertex arrays or a
 * render target, so nothing is rasterized; what they compare is what
 * ati_r350_r300_draw_indx() was handed, as its trace event reports it
 * (VAP_VF_CNTL, the index count and the first four indices).
 */
#define IDX_OFF         0x300000        /* card address: the FB is at 0 */

static char *r350_start_traced(R350 *r)
{
    QPCIDevice *dev = NULL;
    char *log = NULL;
    int fd = g_file_open_tmp("ati-r350-test-XXXXXX.log", &log, NULL);

    g_assert(fd >= 0);
    close(fd);
    r->qs = qtest_spapr_boot("-vga none -device ati-radeon9800,"
                             "async-engine=off,async-display=off "
                             "-d trace:ati_r350_3d_indx*,"
                             "trace:ati_r350_3d_skip -D %s", log);
    qpci_device_foreach(r->qs->pcibus, R350_VENDOR_ID, R350_DEVICE_ID,
                        find_r350, &dev);
    g_assert(dev);
    r->dev = dev;
    r->vram = qpci_iomap(dev, VRAM_BAR, NULL);
    r->mmio = qpci_iomap(dev, MMIO_BAR, NULL);
    qpci_device_enable(dev);
    return log;
}

#define DRAWN           "R300 indexed draw "
#define SKIPPED         "R300 draw skipped "

/*
 * The indexed draws and the skipped draws the log holds, in the order
 * they were traced; the log is removed.
 */
static GPtrArray *traced_draws(const char *log)
{
    GPtrArray *ev = g_ptr_array_new_with_free_func(g_free);
    g_autofree char *text = NULL;
    g_auto(GStrv) lines = NULL;

    g_assert(g_file_get_contents(log, &text, NULL, NULL));
    unlink(log);
    lines = g_strsplit(text, "\n", -1);
    for (size_t i = 0; lines[i]; i++) {
        const char *d = strstr(lines[i], DRAWN);

        if (!d) {
            d = strstr(lines[i], SKIPPED);
        }
        if (d) {
            g_ptr_array_add(ev, g_strdup(d));
        }
    }
    return ev;
}

/* The indexed draws alone */
static GPtrArray *indexed_draws(const char *log)
{
    g_autoptr(GPtrArray) ev = traced_draws(log);
    GPtrArray *draws = g_ptr_array_new_with_free_func(g_free);

    for (guint i = 0; i < ev->len; i++) {
        if (g_str_has_prefix(g_ptr_array_index(ev, i), DRAWN)) {
            g_ptr_array_add(draws, g_strdup(g_ptr_array_index(ev, i)));
        }
    }
    return draws;
}

/* Indices packed as the packet carries them: two per dword or one */
static size_t pack_indices(uint32_t *dw, const uint16_t *idx, size_t n,
                           bool idx32)
{
    size_t ndw = idx32 ? n : (n + 1) / 2;

    for (size_t i = 0; i < ndw; i++) {
        uint32_t hi = 2 * i + 1 < n ? idx[2 * i + 1] : 0;

        dw[i] = idx32 ? idx[i] : idx[2 * i] | hi << 16;
    }
    return ndw;
}

static void draw_inline(R350 *r, const uint16_t *idx, size_t n, bool idx32)
{
    uint32_t pkt[2 + 16];
    size_t ndw = pack_indices(pkt + 2, idx, n, idx32);

    pkt[0] = PKT3(OP_DRAW_INDX_2, 1 + ndw);
    pkt[1] = VF(n, idx32);
    push(r, pkt, 2 + ndw);
}

/* A 3D_DRAW_INDX_2 that carries only VAP_VF_CNTL */
static void draw_vf_only(R350 *r, size_t n, bool idx32)
{
    const uint32_t pkt[] = { PKT3(OP_DRAW_INDX_2, 1), VF(n, idx32) };

    push(r, pkt, ARRAY_SIZE(pkt));
}

static void indx_buffer(R350 *r, uint32_t port, uint32_t addr, uint32_t ndw)
{
    const uint32_t pkt[] = { PKT3(OP_INDX_BUFFER, 3), port, addr, ndw };

    push(r, pkt, ARRAY_SIZE(pkt));
}

/* The packed indices into VRAM at IDX_OFF; returns their dword count */
static size_t put_indices(R350 *r, const uint16_t *idx, size_t n, bool idx32)
{
    uint32_t dw[16];
    size_t ndw = pack_indices(dw, idx, n, idx32);

    for (size_t i = 0; i < ndw; i++) {
        dw[i] = cpu_to_le32(dw[i]);
    }
    qpci_memwrite(r->dev, r->vram, IDX_OFF, dw, ndw * 4);
    return ndw;
}

typedef struct {
    const char *name;
    uint16_t idx[6];
    size_t n;
    bool idx32;
} IndxCase;

static const IndxCase indx_cases[] = {
    { "three-16", { 4, 8, 15 }, 3, false },
    { "six-16", { 7, 3, 9, 1, 5, 2 }, 6, false },
    { "three-32", { 4, 8, 15 }, 3, true },
    { "six-32", { 7, 3, 9, 1, 5, 2 }, 6, true },
};

/*
 * (a) The same indices, inline and fetched by INDX_BUFFER after a
 * 3D_DRAW_INDX_2 that carries none, reach the draw the same way.
 */
static void run_indx_case(const void *data)
{
    const IndxCase *c = data;
    g_autoptr(GPtrArray) draws = NULL;
    g_autofree char *log = NULL;
    size_t ndw;
    R350 r;

    log = r350_start_traced(&r);
    draw_inline(&r, c->idx, c->n, c->idx32);
    ndw = put_indices(&r, c->idx, c->n, c->idx32);
    draw_vf_only(&r, c->n, c->idx32);
    indx_buffer(&r, VAP_PORT_IDX0, IDX_OFF, ndw);
    r350_stop(&r);

    draws = indexed_draws(log);
    g_assert_cmpuint(draws->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(draws, 0), ==,
                    g_ptr_array_index(draws, 1));
}

/*
 * The indexed draws a packet sequence produced, and how many times its
 * events before the closing draw hold the line @skip (when not NULL).
 * An inline draw of indices 1 2 3 closes every sequence and must be the
 * last indexed draw seen, so that a count of zero cannot come from a log
 * that recorded nothing; what was traced after it is its own.
 */
static guint count_draws_skips(void (*seq)(R350 *r), const char *skip,
                               guint *nskip)
{
    static const uint16_t ctl[] = { 1, 2, 3 };
    g_autoptr(GPtrArray) ev = NULL;
    g_autofree char *log = NULL;
    guint draws = 0, last = 0;
    R350 r;

    log = r350_start_traced(&r);
    seq(&r);
    draw_inline(&r, ctl, ARRAY_SIZE(ctl), false);
    r350_stop(&r);
    ev = traced_draws(log);
    for (guint i = 0; i < ev->len; i++) {
        if (g_str_has_prefix(g_ptr_array_index(ev, i), DRAWN)) {
            draws++;
            last = i;
        }
    }
    g_assert_cmpuint(draws, >=, 1);
    g_assert_nonnull(strstr(g_ptr_array_index(ev, last), "first 1 2 3 0"));
    if (nskip) {
        *nskip = 0;
        for (guint i = 0; i < last; i++) {
            *nskip += !strcmp(g_ptr_array_index(ev, i), skip);
        }
    }
    return draws - 1;
}

static guint count_draws(void (*seq)(R350 *r))
{
    return count_draws_skips(seq, NULL, NULL);
}

/*
 * (b) An INDX_BUFFER that is dropped draws nothing and says so once,
 * with the VAP_VF_CNTL it was waiting with (0 if none) and its count.
 */
static void assert_dropped(void (*seq)(R350 *r), uint32_t vf, uint32_t ndw,
                           guint draws)
{
    g_autofree char *skip = g_strdup_printf(SKIPPED "vf_cntl=0x%08x "
                                            "vsize=0 info=0x%x", vf, ndw);
    guint nskip;

    g_assert_cmpuint(count_draws_skips(seq, skip, &nskip), ==, draws);
    g_assert_cmpuint(nskip, ==, 1);
}

static const uint16_t idx3[] = { 4, 8, 15 };

static void seq_wrong_port(R350 *r)
{
    put_indices(r, idx3, 3, false);
    draw_vf_only(r, 3, false);
    indx_buffer(r, VAP_PORT_IDX0 + 1, IDX_OFF, 2);
}

static void seq_no_draw(R350 *r)
{
    put_indices(r, idx3, 3, false);
    indx_buffer(r, VAP_PORT_IDX0, IDX_OFF, 2);
}

static void seq_zero_dwords(R350 *r)
{
    put_indices(r, idx3, 3, false);
    draw_vf_only(r, 3, false);
    indx_buffer(r, VAP_PORT_IDX0, IDX_OFF, 0);
}

static void seq_over_limit(R350 *r)
{
    put_indices(r, idx3, 3, false);
    draw_vf_only(r, 3, false);
    indx_buffer(r, VAP_PORT_IDX0, IDX_OFF, 0x10001);
}

/* the limit itself is taken: the draw uses the first two dwords */
static void seq_at_limit(R350 *r)
{
    put_indices(r, idx3, 3, false);
    draw_vf_only(r, 3, false);
    indx_buffer(r, VAP_PORT_IDX0, IDX_OFF, 0x10000);
}

/* one INDX_BUFFER uses up the VAP_VF_CNTL it was waiting with */
static void seq_twice(R350 *r)
{
    put_indices(r, idx3, 3, false);
    draw_vf_only(r, 3, false);
    indx_buffer(r, VAP_PORT_IDX0, IDX_OFF, 2);
    indx_buffer(r, VAP_PORT_IDX0, IDX_OFF, 2);
}

/*
 * (c) As in the cat7 G5-openbios build this was taken from: the waiting
 * VAP_VF_CNTL outlives other packets until an INDX_BUFFER or a reset.
 */
static void seq_kept_across_packet(R350 *r)
{
    const uint32_t nop[] = { PKT3(OP_NOP3, 1), 0 };

    put_indices(r, idx3, 3, false);
    draw_vf_only(r, 3, false);
    push(r, nop, ARRAY_SIZE(nop));
    indx_buffer(r, VAP_PORT_IDX0, IDX_OFF, 2);
}

static void test_indx_wrong_port(void)
{
    assert_dropped(seq_wrong_port, VF(3, false), 2, 0);
}

static void test_indx_no_draw(void)
{
    assert_dropped(seq_no_draw, 0, 2, 0);
}

static void test_indx_zero_dwords(void)
{
    assert_dropped(seq_zero_dwords, VF(3, false), 0, 0);
}

static void test_indx_over_limit(void)
{
    assert_dropped(seq_over_limit, VF(3, false), 0x10001, 0);
}

static void test_indx_at_limit(void)
{
    g_assert_cmpuint(count_draws(seq_at_limit), ==, 1);
}

/* the first one draws, the second has no VAP_VF_CNTL left to use */
static void test_indx_twice(void)
{
    assert_dropped(seq_twice, 0, 2, 1);
}

static void test_indx_kept_across_packet(void)
{
    g_assert_cmpuint(count_draws(seq_kept_across_packet), ==, 1);
}

/* A reset drops the VAP_VF_CNTL an INDX_BUFFER would have used */
static void test_indx_reset(void)
{
    static const uint16_t ctl3[] = { 1, 2, 3 };
    g_autoptr(GPtrArray) draws = NULL;
    g_autofree char *log = NULL;
    R350 r;

    log = r350_start_traced(&r);
    put_indices(&r, idx3, 3, false);
    draw_vf_only(&r, 3, false);
    qtest_system_reset(r.qs->qts);
    r.vram = qpci_iomap(r.dev, VRAM_BAR, NULL);
    r.mmio = qpci_iomap(r.dev, MMIO_BAR, NULL);
    qpci_device_enable(r.dev);
    put_indices(&r, idx3, 3, false);
    indx_buffer(&r, VAP_PORT_IDX0, IDX_OFF, 2);
    draw_inline(&r, ctl3, ARRAY_SIZE(ctl3), false);
    r350_stop(&r);
    draws = traced_draws(log);
    /* the INDX_BUFFER is dropped with no VAP_VF_CNTL; then the inline draw */
    g_assert_cmpuint(draws->len, >=, 2);
    g_assert_cmpstr(g_ptr_array_index(draws, 0), ==,
                    SKIPPED "vf_cntl=0x00000000 vsize=0 info=0x2");
    g_assert_true(g_str_has_prefix(g_ptr_array_index(draws, 1), DRAWN));
    g_assert_nonnull(strstr(g_ptr_array_index(draws, 1), "first 1 2 3 0"));
}

/*
 * (d) The packet that follows an INDX_BUFFER is parsed from its header,
 * whether the INDX_BUFFER payload is the three dwords it uses or longer.
 */
static void indx_then_blit(bool extra)
{
    static const uint32_t blit[] = {
        PKT3(OP_BITBLT_MULTI, 6),
        GMC_COPY | GMC_SRC_PITCH_OFFSET | GMC_DST_PITCH_OFFSET |
        GMC_BRUSH(BRUSH_NONE),
        PITCH_OFFSET(SRC_OFF, PITCH), PITCH_OFFSET(DST_OFF, PITCH),
        XY(0, 0), XY(0, 0), XY(16, 4),
    };
    const uint32_t ib[] = {
        PKT3(OP_INDX_BUFFER, extra ? 4 : 3), VAP_PORT_IDX0, IDX_OFF, 2,
        0xdeadbeef,
    };
    static const int pts[][2] = { { 0, 0 }, { 15, 3 }, { 7, 1 } };
    R350 r;

    r350_start(&r);
    seed(&r, SRC_OFF, PITCH, pts, ARRAY_SIZE(pts));
    put_indices(&r, idx3, 3, false);
    draw_vf_only(&r, 3, false);
    push(&r, ib, extra ? 5 : 4);
    push(&r, blit, ARRAY_SIZE(blit));
    assert_copied(&r, DST_OFF, PITCH, pts, ARRAY_SIZE(pts), 0, 0);
    r350_stop(&r);
}

static void test_indx_then_blit(void)
{
    indx_then_blit(false);
}

static void test_indx_extra_then_blit(void)
{
    indx_then_blit(true);
}

static void add_indx_tests(void)
{
    for (size_t i = 0; i < G_N_ELEMENTS(indx_cases); i++) {
        g_autofree char *path = g_strdup_printf("/ati-r350/indx-buffer/%s",
                                                indx_cases[i].name);
        qtest_add_data_func(path, &indx_cases[i], run_indx_case);
    }
    qtest_add_func("/ati-r350/indx-buffer/wrong-port", test_indx_wrong_port);
    qtest_add_func("/ati-r350/indx-buffer/no-draw", test_indx_no_draw);
    qtest_add_func("/ati-r350/indx-buffer/zero-dwords",
                   test_indx_zero_dwords);
    qtest_add_func("/ati-r350/indx-buffer/over-limit", test_indx_over_limit);
    qtest_add_func("/ati-r350/indx-buffer/at-limit", test_indx_at_limit);
    qtest_add_func("/ati-r350/indx-buffer/twice", test_indx_twice);
    qtest_add_func("/ati-r350/indx-buffer/kept-across-packet",
                   test_indx_kept_across_packet);
    qtest_add_func("/ati-r350/indx-buffer/reset", test_indx_reset);
    qtest_add_func("/ati-r350/indx-buffer/then-blit", test_indx_then_blit);
    qtest_add_func("/ati-r350/indx-buffer/extra-then-blit",
                   test_indx_extra_then_blit);
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
    add_cursor_tests();
    add_edid_tests();
    add_indx_tests();
    return g_test_run();
}
