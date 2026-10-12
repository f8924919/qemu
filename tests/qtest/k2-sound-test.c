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

/* DBDMA channels of I2S-a */
#define DBDMA_BASE        (MACIO_BAR + 0x8000)
#define DBDMA_CH(n)       (DBDMA_BASE + ((n) << 7))
#define DBDMA_CONTROL     0x00
#define DBDMA_STATUS      0x04
#define DBDMA_CMDPTR_LO   0x0c
#define DBDMA_RUN         0x8000
#define DBDMA_ACTIVE      0x0400
#define OUTPUT_LAST       0x1000
#define INPUT_LAST        0x3000
#define INTR_ALWAYS       0x0030
#define DBDMA_STOP        0x7000

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
static QTestState *k2_init(const char *extra)
{
    QTestState *qts = qtest_initf("-machine powermac7_3 %s", extra);

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
static void dma_program(QTestState *qts, int ch, uint16_t cmd)
{
    uint8_t desc[32] = { 0 };

    stw_le_p(desc + 0, BUF_LEN);
    stw_le_p(desc + 2, cmd | INTR_ALWAYS);
    stl_le_p(desc + 4, BUF_ADDR);
    stw_le_p(desc + 16 + 2, DBDMA_STOP);
    qtest_memwrite(qts, DESC_ADDR, desc, sizeof(desc));

    le_writel(qts, DBDMA_CH(ch) + DBDMA_CMDPTR_LO, DESC_ADDR);
    le_writel(qts, DBDMA_CH(ch) + DBDMA_CONTROL,
              (DBDMA_RUN << 16) | DBDMA_RUN);
}

/* xfer_status is written back with the channel status once it retires */
static uint16_t dma_xfer_status(QTestState *qts)
{
    uint8_t desc[16];

    qtest_memread(qts, DESC_ADDR, desc, sizeof(desc));
    return lduw_le_p(desc + 14);
}

/*
 * Descriptors retire at the pace the audio would play, from a timer.
 * Without an audiodev the cell stays silent but must still retire them:
 * a guest whose output DMA never completes blocks its sound server.
 */
static void dma_retires(int ch, uint16_t cmd, int irq)
{
    QTestState *qts = k2_init("");

    qtest_irq_intercept_in(qts, "pic");
    le_writel(qts, K2_FCR1, K2_FCR1_I2S0_BITS);
    dma_program(qts, ch, cmd);

    /* 4096 bytes are 1024 16-bit stereo frames: well under 100ms */
    qtest_clock_step(qts, 100 * 1000 * 1000);

    g_assert_cmphex(dma_xfer_status(qts) & DBDMA_RUN, ==, DBDMA_RUN);
    g_assert_true(qtest_get_irq(qts, irq));

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
    qtest_add_func("/k2-sound/fcr-reads-back", test_fcr_reads_back);
    qtest_add_func("/k2-sound/dma-out-retires", test_dma_out_retires);
    qtest_add_func("/k2-sound/dma-in-retires", test_dma_in_retires);

    return g_test_run();
}
