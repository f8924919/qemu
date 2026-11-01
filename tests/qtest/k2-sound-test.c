/*
 * QTest testcase for the K2 sound cell of powermac7_3
 *
 * The K2 mac-io carries a second Keywest i2c controller at BAR + 0x18000
 * with the TAS3004 equalizer (7-bit 0x35) and the CS8420 S/PDIF codec
 * (7-bit 0x10) behind it, the I2S-a cell at BAR + 0x10000, the feature
 * control registers at BAR + 0x24 and the I2S DBDMA channels 0 (out) and
 * 2 (in).
 *
 * Darwin's AppleOnboardAudio talks to the TAS3004 through PPCI2CInterface
 * and streams through DBDMA channel 0.  The two failure modes that matter
 * are a codec that never answers (the driver gives up and publishes no
 * output device) and a DMA program that never retires (the sound server
 * blocks), so the tests pin down a register round trip through the i2c,
 * the interrupt line the i2c raises, and descriptor retirement on both
 * DMA channels.
 *
 * A DBDMA channel window is 0x80 bytes but holds only 16 registers; the
 * dbdma tests check that the rest of the window neither reads back nor
 * reaches the channel state that follows the registers.
 *
 * With x-dbdma-devstat, a control write also sets s0-s7 in the channel
 * status through its mask and value; a real boot ROM raises s0 that way
 * to end a program that loops on a branch.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "hw/pci/pci_regs.h"

#define U3_HT_CFG_BASE    0xf2000000ULL

/* K2 HT-PCI bridge at bus 0 / dev 3 / fn 0, macio behind it at dev 7 */
#define K2_DEVFN          (3 << 3)
#define MACIO_DEVFN       (7 << 3)
#define CFA0(devfn, off)  (((devfn) << 8) | (off))
#define CFA1(bus, devfn, off)  (CFA0(devfn, off) + ((bus) << 16) + 0x01000000)

/* Where the test puts the macio BAR, inside the K2 bridge's window */
#define MACIO_BAR         0xfa000000ULL

/* K2 Keywest i2c */
#define K2_I2C_BASE       (MACIO_BAR + 0x18000)
#define KW_REG(n)         (K2_I2C_BASE + ((n) * 0x10))
#define KW_MODE           KW_REG(0)
#define KW_CONTROL        KW_REG(1)
#define KW_STATUS         KW_REG(2)
#define KW_ISR            KW_REG(3)
#define KW_IER            KW_REG(4)
#define KW_ADDR           KW_REG(5)
#define KW_SUBADDR        KW_REG(6)
#define KW_DATA           KW_REG(7)

#define KW_I2C_MODE_STANDARDSUB 0x08
#define KW_I2C_MODE_COMBINED    0x0c
#define KW_I2C_CTL_XADDR        0x02
#define KW_I2C_CTL_STOP         0x04
#define KW_I2C_STAT_LAST_AAK    0x02
#define KW_I2C_IRQ_DATA         0x01
#define KW_I2C_IRQ_ADDR         0x02
#define KW_I2C_IRQ_STOP         0x04

#define TAS3004_ADDR8     (0x35 << 1)
#define CS8420_ADDR8      (0x10 << 1)
#define TAS3004_VOL       0x04

/* Feature control registers; FCR1 gates the I2S-a clocks */
#define K2_FCR1           (MACIO_BAR + 0x3c)
#define K2_FCR1_I2S0_BITS 0x00003400   /* cell enable, clock enable, enable */

/* The fans' TACH counts a real ROM reads (docs/arch/macio-k2-sound.md) */
#define K2_FAN_TACH_A     (MACIO_BAR + 0x2c)
#define K2_FAN_TACH_B     (MACIO_BAR + 0x34)
#define K2_FAN_PWM_A      (MACIO_BAR + 0x30)
#define K2_FAN_PWM_B      (MACIO_BAR + 0x4c)
#define FAN_TACH_RATE     1000
#define NS_PER_SEC        1000000000LL

/* DBDMA channels of I2S-a */
#define DBDMA_BASE        (MACIO_BAR + 0x8000)
#define DBDMA_CH(n)       (DBDMA_BASE + ((n) << 7))
#define DBDMA_CONTROL     0x00
#define DBDMA_STATUS      0x04
#define DBDMA_CMDPTR_HI   0x08
#define DBDMA_CMDPTR_LO   0x0c
#define DBDMA_INTR_SEL    0x10
#define DBDMA_BRANCH_SEL  0x14
#define DBDMA_WAIT_SEL    0x18
#define DBDMA_XFER_MODE   0x1c
#define DBDMA_DATA2PTR_HI 0x20
#define DBDMA_DATA2PTR_LO 0x24
#define DBDMA_RES1        0x28
#define DBDMA_ADDRESS_HI  0x2c
#define DBDMA_BR_ADDR_HI  0x30
#define DBDMA_RES2        0x34
#define DBDMA_RES3        0x38
#define DBDMA_RES4        0x3c
#define DBDMA_IMPL_END    0x40
#define DBDMA_WINDOW      0x80
#define DBDMA_NCHANNELS   32
#define DBDMA_RUN         0x8000
#define DBDMA_PAUSE       0x4000
#define DBDMA_WAKE        0x1000
#define DBDMA_ACTIVE      0x0400
#define DBDMA_BT          0x0100
#define DBDMA_DEVSTAT     0x00ff
#define OUTPUT_LAST       0x1000
#define INPUT_LAST        0x3000
#define INTR_ALWAYS       0x0030
#define DBDMA_STOP        0x7000
#define DBDMA_NOP         0x6000
#define BR_IFCLR          0x0008

/* How many status reads a running program gets to retire */
#define DBDMA_POLLS       1000

/* mpic inputs (docs/arch/macio-k2-sound.md, allocation table) */
#define IRQ_I2S_TX        0x01
#define IRQ_I2S_RX        0x08
#define IRQ_K2_I2C        0x1a

/* Guest RAM used for the descriptor programs and their buffers */
#define DESC_ADDR         0x100000
#define BUF_ADDR          0x200000
#define BUF_LEN           4096

static void le_writel(QTestState *qts, uint64_t addr, uint32_t val)
{
    qtest_writel(qts, addr, bswap32(val));
}

static uint32_t le_readl(QTestState *qts, uint64_t addr)
{
    return bswap32(qtest_readl(qts, addr));
}

static void cfg_writew(QTestState *qts, uint64_t addr, uint16_t val)
{
    qtest_writew(qts, addr, bswap16(val));
}

/*
 * Firmware does not run under qtest: bring up the K2 bridge window and
 * place the macio BAR by hand (same steps as u3-ht-test).
 */
static void k2_bring_up(QTestState *qts)
{
    qtest_writeb(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x18), 0);
    qtest_writeb(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x19), 1);
    qtest_writeb(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x1a), 1);
    cfg_writew(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x20), 0xfa00);
    cfg_writew(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 0x22), 0xfaf0);
    cfg_writew(qts, U3_HT_CFG_BASE + CFA0(K2_DEVFN, 4), PCI_COMMAND_MEMORY);

    le_writel(qts, U3_HT_CFG_BASE + CFA1(1, MACIO_DEVFN, PCI_BASE_ADDRESS_0),
              (uint32_t)MACIO_BAR);
    cfg_writew(qts, U3_HT_CFG_BASE + CFA1(1, MACIO_DEVFN, PCI_COMMAND),
               PCI_COMMAND_MEMORY);
}

static QTestState *k2_init(const char *extra)
{
    QTestState *qts = qtest_initf("-machine powermac7_3 %s", extra);

    k2_bring_up(qts);
    return qts;
}

static void i2c_wait_isr(QTestState *qts, uint8_t bit)
{
    int i;

    for (i = 0; i < 100; i++) {
        if (qtest_readb(qts, KW_ISR) & bit) {
            return;
        }
    }
    g_assert_cmphex(qtest_readb(qts, KW_ISR) & bit, ==, bit);
}

/* Start a transfer and return whether the address was acknowledged */
static bool i2c_start(QTestState *qts, uint8_t mode, uint8_t addr8,
                      uint8_t sub)
{
    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, mode);
    qtest_writeb(qts, KW_ADDR, addr8);
    qtest_writeb(qts, KW_SUBADDR, sub);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_XADDR);
    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);
    return qtest_readb(qts, KW_STATUS) & KW_I2C_STAT_LAST_AAK;
}

static void i2c_write_bytes(QTestState *qts, uint8_t addr8, uint8_t sub,
                            const uint8_t *val, int len)
{
    int i;

    g_assert_true(i2c_start(qts, KW_I2C_MODE_STANDARDSUB, addr8 & ~1, sub));
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_ADDR);
    for (i = 0; i < len; i++) {
        qtest_writeb(qts, KW_DATA, val[i]);
        i2c_wait_isr(qts, KW_I2C_IRQ_DATA);
        qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_DATA);
    }
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_STOP);
    i2c_wait_isr(qts, KW_I2C_IRQ_STOP);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_STOP);
}

static uint8_t i2c_read_byte(QTestState *qts, uint8_t addr8, uint8_t sub)
{
    uint8_t val;

    g_assert_true(i2c_start(qts, KW_I2C_MODE_COMBINED, addr8 | 1, sub));
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_ADDR);
    i2c_wait_isr(qts, KW_I2C_IRQ_DATA);
    val = qtest_readb(qts, KW_DATA);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_DATA);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_DATA);
    i2c_wait_isr(qts, KW_I2C_IRQ_STOP);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_STOP);
    return val;
}

/*
 * The volume register is six bytes (two 8.16 gains).  Reading back what
 * was written separates a codec that is really on the bus from a model
 * whose reads return zero.
 */
static void test_tas3004_roundtrip(void)
{
    QTestState *qts = k2_init("");
    static const uint8_t vol[6] = { 0x00, 0x8a, 0x31, 0x00, 0x45, 0x67 };

    i2c_write_bytes(qts, TAS3004_ADDR8, TAS3004_VOL, vol, sizeof(vol));
    g_assert_cmphex(i2c_read_byte(qts, TAS3004_ADDR8, TAS3004_VOL),
                    ==, vol[0]);

    qtest_quit(qts);
}

static void test_cs8420_answers(void)
{
    QTestState *qts = k2_init("");

    g_assert_true(i2c_start(qts, KW_I2C_MODE_STANDARDSUB,
                            CS8420_ADDR8, 0x00));
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_ADDR);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_STOP);
    i2c_wait_isr(qts, KW_I2C_IRQ_STOP);

    qtest_quit(qts);
}

/* Nothing sits at 0x20: the address phase must come back without AAK */
static void test_unanswered_address_naks(void)
{
    QTestState *qts = k2_init("");

    g_assert_false(i2c_start(qts, KW_I2C_MODE_STANDARDSUB, 0x40, 0x00));

    qtest_quit(qts);
}

/*
 * The U3 i2c is polled and has no interrupt wired; the K2 one is the first
 * instance whose ISR reaches the mpic, so check the line itself.
 */
static void test_i2c_irq_line(void)
{
    QTestState *qts = k2_init("");

    qtest_irq_intercept_in(qts, "pic");
    qtest_writeb(qts, KW_IER, KW_I2C_IRQ_ADDR | KW_I2C_IRQ_DATA |
                              KW_I2C_IRQ_STOP);
    g_assert_false(qtest_get_irq(qts, IRQ_K2_I2C));

    i2c_start(qts, KW_I2C_MODE_STANDARDSUB, TAS3004_ADDR8, 0x00);
    g_assert_true(qtest_get_irq(qts, IRQ_K2_I2C));

    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_ADDR);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_STOP);
    i2c_wait_isr(qts, KW_I2C_IRQ_STOP);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_STOP);
    g_assert_false(qtest_get_irq(qts, IRQ_K2_I2C));

    qtest_quit(qts);
}

static void test_fcr_reads_back(void)
{
    QTestState *qts = k2_init("");

    le_writel(qts, K2_FCR1, K2_FCR1_I2S0_BITS);
    g_assert_cmphex(le_readl(qts, K2_FCR1), ==, K2_FCR1_I2S0_BITS);

    qtest_quit(qts);
}

/* One descriptor followed by STOP; returns once the program is in place */
static void dma_program_len(QTestState *qts, int ch, uint16_t cmd,
                            uint16_t len)
{
    uint8_t desc[32] = { 0 };

    stw_le_p(desc + 0, len);
    stw_le_p(desc + 2, cmd | INTR_ALWAYS);
    stl_le_p(desc + 4, BUF_ADDR);
    stw_le_p(desc + 16 + 2, DBDMA_STOP);
    qtest_memwrite(qts, DESC_ADDR, desc, sizeof(desc));

    le_writel(qts, DBDMA_CH(ch) + DBDMA_CMDPTR_LO, DESC_ADDR);
    le_writel(qts, DBDMA_CH(ch) + DBDMA_CONTROL,
              (DBDMA_RUN << 16) | DBDMA_RUN);
}

static void dma_program(QTestState *qts, int ch, uint16_t cmd)
{
    dma_program_len(qts, ch, cmd, BUF_LEN);
}

/* xfer_status is written back with the channel status once it retires */
static uint16_t dma_xfer_status(QTestState *qts)
{
    uint8_t desc[16];

    qtest_memread(qts, DESC_ADDR, desc, sizeof(desc));
    return lduw_le_p(desc + 14);
}

/*
 * A program that loops on itself until the branch condition holds, then
 * stops: a NOP that branches back to itself while the condition is clear,
 * and a STOP after it.
 */
static void dma_program_loop(QTestState *qts, int ch, uint32_t branch_sel)
{
    uint8_t desc[32] = { 0 };

    stw_le_p(desc + 2, DBDMA_NOP | BR_IFCLR);
    stl_le_p(desc + 8, DESC_ADDR);
    stw_le_p(desc + 16 + 2, DBDMA_STOP);
    qtest_memwrite(qts, DESC_ADDR, desc, sizeof(desc));

    le_writel(qts, DBDMA_CH(ch) + DBDMA_CONTROL, 0xffff0000);
    le_writel(qts, DBDMA_CH(ch) + DBDMA_BRANCH_SEL, branch_sel);
    le_writel(qts, DBDMA_CH(ch) + DBDMA_CMDPTR_LO, DESC_ADDR);
    le_writel(qts, DBDMA_CH(ch) + DBDMA_CONTROL,
              (DBDMA_RUN << 16) | DBDMA_RUN);
}

/*
 * The channel runs from a bottom half, so a single read may come before
 * the program has moved; read until ACTIVE drops or DBDMA_POLLS is spent.
 */
static uint32_t dbdma_poll_inactive(QTestState *qts, int ch)
{
    uint32_t status = 0;
    int i;

    for (i = 0; i < DBDMA_POLLS; i++) {
        status = le_readl(qts, DBDMA_CH(ch) + DBDMA_STATUS);
        if (!(status & DBDMA_ACTIVE)) {
            break;
        }
    }
    return status;
}

/* Fill the unimplemented part of a channel window, a distinct value a word */
static void dbdma_fill_unimplemented(QTestState *qts, int ch)
{
    uint32_t off;

    for (off = DBDMA_IMPL_END; off < DBDMA_WINDOW; off += 4) {
        le_writel(qts, DBDMA_CH(ch) + off, 0xa5000000 | off);
    }
}

/*
 * Descriptors retire at the pace the audio would play, from a timer.
 * Without an audiodev the cell stays silent but must still retire them:
 * a guest whose output DMA never completes blocks its sound server.
 */
static void dma_retires_after(int ch, uint16_t cmd, int irq, bool fill)
{
    QTestState *qts = k2_init("");

    qtest_irq_intercept_in(qts, "pic");
    le_writel(qts, K2_FCR1, K2_FCR1_I2S0_BITS);
    if (fill) {
        dbdma_fill_unimplemented(qts, ch);
    }
    dma_program(qts, ch, cmd);

    /* 4096 bytes are 1024 16-bit stereo frames: well under 100ms */
    qtest_clock_step(qts, 100 * 1000 * 1000);

    g_assert_cmphex(dma_xfer_status(qts) & DBDMA_RUN, ==, DBDMA_RUN);
    g_assert_true(qtest_get_irq(qts, irq));

    qtest_quit(qts);
}

static void dma_retires(int ch, uint16_t cmd, int irq)
{
    dma_retires_after(ch, cmd, irq, false);
}

/*
 * A reset while a descriptor is still pending must leave the channel usable.
 * The pending descriptor is dropped (completing it would write back through
 * a command pointer that the reset has cleared), so the channel has to be
 * told that it is no longer busy -- otherwise the next boot's first RUN is
 * ignored and the guest never hears anything again.
 */
static void test_dma_usable_after_reset(void)
{
    QTestState *qts = k2_init("");

    le_writel(qts, K2_FCR1, K2_FCR1_I2S0_BITS);
    dma_program(qts, 0, OUTPUT_LAST);
    /* Mid-descriptor: 1024 frames at 72 kHz take about 14 ms */
    qtest_clock_step(qts, 1 * 1000 * 1000);
    g_assert_cmphex(dma_xfer_status(qts) & DBDMA_RUN, ==, 0);

    qtest_system_reset(qts);
    k2_bring_up(qts);
    le_writel(qts, K2_FCR1, K2_FCR1_I2S0_BITS);
    dma_program(qts, 0, OUTPUT_LAST);
    qtest_clock_step(qts, 100 * 1000 * 1000);
    g_assert_cmphex(dma_xfer_status(qts) & DBDMA_RUN, ==, DBDMA_RUN);

    qtest_quit(qts);
}

/*
 * A stream starts on time: its first descriptor retires one descriptor's
 * playing time after it was taken, not at once.  Retiring the start of a
 * stream early runs the cell ahead of the guest, which then has not yet
 * written the buffers the cell reads.
 */
static void test_dma_out_starts_on_time(void)
{
    QTestState *qts = k2_init("");

    le_writel(qts, K2_FCR1, K2_FCR1_I2S0_BITS);
    /* Well past boot, so that a start "in the past" would not be clamped */
    qtest_clock_step(qts, 100 * 1000 * 1000);
    /* 128 bytes are 32 frames: about 0.44 ms at 72 kHz */
    dma_program_len(qts, 0, OUTPUT_LAST, 128);
    qtest_clock_step(qts, 100 * 1000);
    g_assert_cmphex(dma_xfer_status(qts) & DBDMA_RUN, ==, 0);

    qtest_clock_step(qts, 1 * 1000 * 1000);
    g_assert_cmphex(dma_xfer_status(qts) & DBDMA_RUN, ==, DBDMA_RUN);

    qtest_quit(qts);
}

static void test_dma_out_retires(void)
{
    dma_retires(0, OUTPUT_LAST, IRQ_I2S_TX);
}

static void test_dma_in_retires(void)
{
    dma_retires(2, INPUT_LAST, IRQ_I2S_RX);
}

/* The unimplemented part of every channel window reads as zero */
static void test_dbdma_unimplemented_reads_zero(void)
{
    QTestState *qts = k2_init("");
    uint32_t off;
    int ch;

    for (ch = 0; ch < DBDMA_NCHANNELS; ch++) {
        dbdma_fill_unimplemented(qts, ch);
    }
    for (ch = 0; ch < DBDMA_NCHANNELS; ch++) {
        for (off = DBDMA_IMPL_END; off < DBDMA_WINDOW; off += 4) {
            g_assert_cmphex(le_readl(qts, DBDMA_CH(ch) + off), ==, 0);
        }
    }

    qtest_quit(qts);
}

/*
 * Writes past the registers leave the implemented ones alone, and the
 * registers QEMU does not use read as zero.
 */
static void test_dbdma_implemented_regs_kept(void)
{
    static const struct {
        uint32_t off;
        uint32_t val;
        uint32_t expect;
    } regs[] = {
        { DBDMA_STATUS,     0x000000a5, 0x000000a5 },
        { DBDMA_CMDPTR_LO,  DESC_ADDR | 0x7, DESC_ADDR },
        { DBDMA_INTR_SEL,   0x00110011, 0x00110011 },
        { DBDMA_BRANCH_SEL, 0x00220022, 0x00220022 },
        { DBDMA_WAIT_SEL,   0x00330033, 0x00330033 },
        { DBDMA_RES1,       0x5a5a0001, 0x5a5a0001 },
        { DBDMA_RES2,       0x5a5a0002, 0x5a5a0002 },
        { DBDMA_RES3,       0x5a5a0003, 0x5a5a0003 },
        { DBDMA_RES4,       0x5a5a0004, 0x5a5a0004 },
        { DBDMA_XFER_MODE,  0x12345678, 0 },
        { DBDMA_CMDPTR_HI,  0x12345678, 0 },
        { DBDMA_DATA2PTR_HI, 0x12345678, 0 },
        { DBDMA_DATA2PTR_LO, 0x12345678, 0 },
        { DBDMA_ADDRESS_HI, 0x12345678, 0 },
        { DBDMA_BR_ADDR_HI, 0x12345678, 0 },
    };
    QTestState *qts = k2_init("");
    int i;

    for (i = 0; i < ARRAY_SIZE(regs); i++) {
        le_writel(qts, DBDMA_CH(0) + regs[i].off, regs[i].val);
    }
    dbdma_fill_unimplemented(qts, 0);

    for (i = 0; i < ARRAY_SIZE(regs); i++) {
        g_assert_cmphex(le_readl(qts, DBDMA_CH(0) + regs[i].off), ==,
                        regs[i].expect);
    }
    /* CONTROL reads back the status */
    g_assert_cmphex(le_readl(qts, DBDMA_CH(0) + DBDMA_CONTROL), ==, 0xa5);

    qtest_quit(qts);
}

/* The channel still runs a program after writes past its registers */
static void test_dbdma_runs_after_unimplemented_writes(void)
{
    dma_retires_after(0, OUTPUT_LAST, IRQ_I2S_TX, true);
}

/*
 * x-u3-i2c-lane3 is for the U3 controller only: the K2 one keeps byte 0
 * and leaves byte 3 a hole.
 */
static void test_i2c_lane3_not_on_k2(void)
{
    QTestState *qts = k2_init("-machine powermac7_3,x-u3-i2c-lane3=on");

    qtest_writeb(qts, KW_MODE, 0x21);
    g_assert_cmphex(qtest_readb(qts, KW_MODE + 3), ==, 0);
    qtest_writeb(qts, KW_MODE + 3, 0x42);
    g_assert_cmphex(qtest_readb(qts, KW_MODE), ==, 0x21);

    qtest_quit(qts);
}

/* So is x-u3-i2c-dumb: START in DUMB mode does nothing on K2 */
static void test_i2c_dumb_not_on_k2(void)
{
    QTestState *qts = k2_init("-machine powermac7_3,x-u3-i2c-dumb=on");

    qtest_writeb(qts, KW_MODE, 0x00);
    qtest_writeb(qts, KW_ISR, 0x0f);
    qtest_writeb(qts, KW_CONTROL, 0x08);
    g_assert_cmphex(qtest_readb(qts, KW_ISR), ==, 0);
    g_assert_cmphex(qtest_readb(qts, KW_CONTROL), ==, 0x08);

    qtest_quit(qts);
}

/* Off, the TACH registers are plain storage like the other FCRs */
static void test_fan_tach_off(void)
{
    QTestState *qts = k2_init("");

    le_writel(qts, K2_FAN_TACH_A, 0x12345678);
    le_writel(qts, K2_FAN_TACH_B, 0x9abcdef0);
    qtest_clock_step(qts, NS_PER_SEC);
    g_assert_cmphex(le_readl(qts, K2_FAN_TACH_A), ==, 0x12345678);
    g_assert_cmphex(le_readl(qts, K2_FAN_TACH_B), ==, 0x9abcdef0);

    qtest_quit(qts);
}

/*
 * On, both halves of both registers count with the virtual clock, which
 * qtest starts at zero and moves only on clock_step.
 */
static void fan_tach_counts(const char *extra)
{
    QTestState *qts = k2_init(extra);
    uint32_t one = FAN_TACH_RATE * 0x10001;

    g_assert_cmphex(le_readl(qts, K2_FAN_TACH_B), ==, 0);
    qtest_clock_step(qts, NS_PER_SEC);
    g_assert_cmphex(le_readl(qts, K2_FAN_TACH_B), ==, one);
    g_assert_cmphex(le_readl(qts, K2_FAN_TACH_A), ==, one);
    /* A two-byte read of the upper half reaches the same register */
    g_assert_cmphex(bswap16(qtest_readw(qts, K2_FAN_TACH_B + 2)), ==,
                    FAN_TACH_RATE);

    /* What the guest writes does not move the count */
    le_writel(qts, K2_FAN_TACH_B, 0x12345678);
    le_writel(qts, K2_FAN_TACH_A, 0x9abcdef0);
    g_assert_cmphex(le_readl(qts, K2_FAN_TACH_B), ==, one);
    g_assert_cmphex(le_readl(qts, K2_FAN_TACH_A), ==, one);

    /* The PWM registers next to them still read back */
    le_writel(qts, K2_FAN_PWM_A, 0x01010101);
    le_writel(qts, K2_FAN_PWM_B, 0x02020202);
    g_assert_cmphex(le_readl(qts, K2_FAN_PWM_A), ==, 0x01010101);
    g_assert_cmphex(le_readl(qts, K2_FAN_PWM_B), ==, 0x02020202);

    qtest_quit(qts);
}

static void test_fan_tach_on(void)
{
    fan_tach_counts("-machine powermac7_3,x-k2-fan-tach=on");
}

/* The machine leaves the device alone when off, so -global works too */
static void test_fan_tach_global(void)
{
    fan_tach_counts("-global k2-i2s.x-fan-tach=on");
}

/*
 * Across the 16-bit wrap a second still moves the count by the rate once
 * the 0x10000 AppleK2Fan adds back is taken into account.
 */
static void test_fan_tach_wraps(void)
{
    QTestState *qts = k2_init("-machine powermac7_3,x-k2-fan-tach=on");
    uint32_t a, b;

    qtest_clock_step(qts, 65 * NS_PER_SEC + NS_PER_SEC / 2);
    a = le_readl(qts, K2_FAN_TACH_B) & 0xffff;
    qtest_clock_step(qts, NS_PER_SEC);
    b = le_readl(qts, K2_FAN_TACH_B) & 0xffff;
    g_assert_cmpuint(b, <, a);
    g_assert_cmpuint((b - a + 0x10000) & 0xffff, ==, FAN_TACH_RATE);

    qtest_quit(qts);
}

/* Without a K2 (a G4 CPU) the switch has nothing to reach */
static void test_fan_tach_no_k2(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3,x-k2-fan-tach=on "
                                 "-cpu 7447a");

    qtest_quit(qts);
}

/*
 * The real ROM's sequence on channel 0: clear, select s0 for the branch,
 * run, then set s0 from the control register.  The loop ends and the
 * STOP after it drops ACTIVE.
 */
static void test_devstat_ends_loop(void)
{
    QTestState *qts = k2_init("-machine powermac7_3,x-dbdma-devstat=on");
    uint32_t status;
    int i;

    dma_program_loop(qts, 0, 0x00010001);
    for (i = 0; i < 10; i++) {
        status = le_readl(qts, DBDMA_CH(0) + DBDMA_STATUS);
        g_assert_cmphex(status & (DBDMA_ACTIVE | DBDMA_DEVSTAT), ==,
                        DBDMA_ACTIVE);
    }

    le_writel(qts, DBDMA_CH(0) + DBDMA_CONTROL, 0x00010001);
    status = dbdma_poll_inactive(qts, 0);
    g_assert_cmphex(status, ==, DBDMA_RUN | 0x01);
    g_assert_cmphex(le_readl(qts, DBDMA_CH(0) + DBDMA_CMDPTR_LO), ==,
                    DESC_ADDR + 16);
    /* The NOP is written back with the status it ran under */
    g_assert_cmphex(dma_xfer_status(qts) & DBDMA_DEVSTAT, ==, 0x01);

    qtest_quit(qts);
}

/*
 * Without the switch the same writes leave s0 clear and the loop keeps
 * branching for as long as it is read.
 */
static void test_devstat_off_keeps_looping(void)
{
    QTestState *qts = k2_init("");
    uint32_t status = 0;
    int i;

    dma_program_loop(qts, 0, 0x00010001);
    /* Wait for the first branch, so that every read below sees the loop */
    for (i = 0; i < DBDMA_POLLS; i++) {
        status = le_readl(qts, DBDMA_CH(0) + DBDMA_STATUS);
        if (status & DBDMA_BT) {
            break;
        }
    }
    g_assert_cmphex(status, ==, DBDMA_RUN | DBDMA_ACTIVE | DBDMA_BT);

    le_writel(qts, DBDMA_CH(0) + DBDMA_CONTROL, 0x00010001);
    for (i = 0; i < DBDMA_POLLS; i++) {
        status = le_readl(qts, DBDMA_CH(0) + DBDMA_STATUS);
        if (status != (DBDMA_RUN | DBDMA_ACTIVE | DBDMA_BT)) {
            break;
        }
    }
    g_assert_cmphex(status, ==, DBDMA_RUN | DBDMA_ACTIVE | DBDMA_BT);

    qtest_quit(qts);
}

/* Without the switch a control write never reaches s0-s7 */
static void test_devstat_off_ignored(void)
{
    QTestState *qts = k2_init("");

    le_writel(qts, DBDMA_CH(0) + DBDMA_CONTROL, 0x00ff00a5);
    g_assert_cmphex(le_readl(qts, DBDMA_CH(0) + DBDMA_STATUS), ==, 0);

    qtest_quit(qts);
}

/* Only the bits in the mask change, whatever else the write does */
static void devstat_mask(const char *extra)
{
    QTestState *qts = k2_init(extra);
    uint32_t st = DBDMA_CH(0) + DBDMA_STATUS;

    le_writel(qts, DBDMA_CH(0) + DBDMA_CONTROL, 0x00ff00a5);
    g_assert_cmphex(le_readl(qts, st), ==, 0xa5);
    le_writel(qts, DBDMA_CH(0) + DBDMA_CONTROL, 0x000000ff);
    g_assert_cmphex(le_readl(qts, st), ==, 0xa5);
    le_writel(qts, DBDMA_CH(0) + DBDMA_CONTROL,
              (DBDMA_RUN | DBDMA_PAUSE | DBDMA_WAKE) << 16 | 0x00ff);
    g_assert_cmphex(le_readl(qts, st), ==, 0xa5);
    le_writel(qts, DBDMA_CH(0) + DBDMA_CONTROL, 0x00010000);
    g_assert_cmphex(le_readl(qts, st), ==, 0xa4);
    le_writel(qts, DBDMA_CH(0) + DBDMA_CONTROL, 0x00820082);
    g_assert_cmphex(le_readl(qts, st), ==, 0xa6);

    /* Stopping a running channel keeps the bits outside the mask */
    dma_program(qts, 0, DBDMA_STOP);
    dbdma_poll_inactive(qts, 0);
    le_writel(qts, DBDMA_CH(0) + DBDMA_CONTROL, DBDMA_RUN << 16);
    g_assert_cmphex(le_readl(qts, st), ==, 0xa6);

    le_writel(qts, DBDMA_CH(0) + DBDMA_CONTROL, 0xffff0000);
    g_assert_cmphex(le_readl(qts, st), ==, 0);

    qtest_quit(qts);
}

static void test_devstat_mask(void)
{
    devstat_mask("-machine powermac7_3,x-dbdma-devstat=on");
}

/* The machine leaves the device alone when off, so -global works too */
static void test_devstat_global(void)
{
    devstat_mask("-global mac-dbdma.x-devstat=on");
}

/*
 * s4-s7 show in the status, but conditions still look at s0-s3 only: a
 * branch on s4 sees an empty mask, so its condition always holds and the
 * loop falls through to the STOP before s4 is set.  This pins down a known
 * difference from the DBDMA documentation (s7..s0); widening the condition
 * to eight bits turns it around.
 */
static void test_devstat_s4_not_a_condition(void)
{
    QTestState *qts = k2_init("-machine powermac7_3,x-dbdma-devstat=on");

    le_writel(qts, DBDMA_CH(0) + DBDMA_CONTROL, 0x00100010);
    g_assert_cmphex(le_readl(qts, DBDMA_CH(0) + DBDMA_STATUS), ==, 0x10);

    dma_program_loop(qts, 0, 0x00100010);
    g_assert_cmphex(dbdma_poll_inactive(qts, 0), ==, DBDMA_RUN);
    g_assert_cmphex(le_readl(qts, DBDMA_CH(0) + DBDMA_CMDPTR_LO), ==,
                    DESC_ADDR + 16);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine("powermac7_3")) {
        g_test_skip("powermac7_3 machine not available");
        return 0;
    }

    qtest_add_func("/k2-sound/tas3004-roundtrip", test_tas3004_roundtrip);
    qtest_add_func("/k2-sound/cs8420-answers", test_cs8420_answers);
    qtest_add_func("/k2-sound/unanswered-address-naks",
                   test_unanswered_address_naks);
    qtest_add_func("/k2-sound/i2c-irq-line", test_i2c_irq_line);
    qtest_add_func("/k2-sound/i2c-lane3-not-on-k2", test_i2c_lane3_not_on_k2);
    qtest_add_func("/k2-sound/i2c-dumb-not-on-k2", test_i2c_dumb_not_on_k2);
    qtest_add_func("/k2-sound/fcr-reads-back", test_fcr_reads_back);
    qtest_add_func("/k2-sound/fan-tach-off", test_fan_tach_off);
    qtest_add_func("/k2-sound/fan-tach-on", test_fan_tach_on);
    qtest_add_func("/k2-sound/fan-tach-global", test_fan_tach_global);
    qtest_add_func("/k2-sound/fan-tach-wraps", test_fan_tach_wraps);
    qtest_add_func("/k2-sound/fan-tach-no-k2", test_fan_tach_no_k2);
    qtest_add_func("/k2-sound/dma-out-retires", test_dma_out_retires);
    qtest_add_func("/k2-sound/dma-in-retires", test_dma_in_retires);
    qtest_add_func("/k2-sound/dma-out-starts-on-time",
                   test_dma_out_starts_on_time);
    qtest_add_func("/k2-sound/dma-usable-after-reset",
                   test_dma_usable_after_reset);
    qtest_add_func("/k2-sound/dbdma-unimplemented-reads-zero",
                   test_dbdma_unimplemented_reads_zero);
    qtest_add_func("/k2-sound/dbdma-implemented-regs-kept",
                   test_dbdma_implemented_regs_kept);
    qtest_add_func("/k2-sound/dbdma-runs-after-unimplemented-writes",
                   test_dbdma_runs_after_unimplemented_writes);
    qtest_add_func("/k2-sound/devstat-ends-loop", test_devstat_ends_loop);
    qtest_add_func("/k2-sound/devstat-off-keeps-looping",
                   test_devstat_off_keeps_looping);
    qtest_add_func("/k2-sound/devstat-off-ignored", test_devstat_off_ignored);
    qtest_add_func("/k2-sound/devstat-mask", test_devstat_mask);
    qtest_add_func("/k2-sound/devstat-global", test_devstat_global);
    qtest_add_func("/k2-sound/devstat-s4-not-a-condition",
                   test_devstat_s4_not_a_condition);

    return g_test_run();
}
