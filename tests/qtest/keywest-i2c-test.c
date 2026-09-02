/*
 * QTest testcase for the Keywest i2c controller inside U3 (powermac7_3)
 *
 * The register layout and the transfer state machine are dictated by two
 * independent guests that must both work:
 *
 *  - Linux arch/powerpc/platforms/powermac/low_i2c.c drives the eight
 *    registers at AAPL,address + (reg << bsteps).  It polls when no
 *    interrupt is wired, and it does so with the timebase frozen, so the
 *    model must complete transfers synchronously inside the MMIO handler
 *    rather than through a timer.
 *  - Darwin's AppleI2C (PPCI2CInterface) drives the same registers for
 *    MacRISC4CPU::enableCPUTimeBase(), which read-modify-writes register
 *    0x2e of the clock chip at 8-bit address 0xd2.
 *
 * The failure mode that matters is a hang, not a wrong value: if a read
 * never completes, Darwin's MacRISC4CPU::startCPU() spins forever waiting
 * for gI2CTransactionComplete and the guest never finishes booting.  The
 * tests below therefore pin down the handshake (which write raises which
 * ISR bit, and that the ISR is write-1-to-clear) rather than just poking
 * registers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* U3-internal offset 0x1000; AAPL,address in the device tree is absolute */
#define KW_I2C_BASE       0xf8001000ULL
#define KW_I2C_STEP       0x10
#define KW_REG(n)         (KW_I2C_BASE + ((n) * KW_I2C_STEP))

#define KW_MODE           KW_REG(0)
#define KW_CONTROL        KW_REG(1)
#define KW_STATUS         KW_REG(2)
#define KW_ISR            KW_REG(3)
#define KW_IER            KW_REG(4)
#define KW_ADDR           KW_REG(5)
#define KW_SUBADDR        KW_REG(6)
#define KW_DATA           KW_REG(7)

/* Mode register (low_i2c.c) */
#define KW_I2C_MODE_100KHZ      0x00
#define KW_I2C_MODE_STANDARD    0x04
#define KW_I2C_MODE_STANDARDSUB 0x08
#define KW_I2C_MODE_COMBINED    0x0c

/* Control register */
#define KW_I2C_CTL_AAK          0x01
#define KW_I2C_CTL_XADDR        0x02
#define KW_I2C_CTL_STOP         0x04

/* Status register */
#define KW_I2C_STAT_BUSY        0x01
#define KW_I2C_STAT_LAST_AAK    0x02

/* ISR/IER */
#define KW_I2C_IRQ_DATA         0x01
#define KW_I2C_IRQ_ADDR         0x02
#define KW_I2C_IRQ_STOP         0x04

/*
 * The clock chip.  The device tree calls it i2c-hwclock@d2 and Linux reads
 * that reg value three different ways (smp.c as a switch, i2c-powermac as
 * (reg & 0xff) >> 1, low_i2c as reg >> 8 for the channel), so 0xd2 is the
 * 8-bit form and 0x69 is what the QEMU slave is addressed by.
 */
#define HWCLOCK_ADDR8     0xd2
#define HWCLOCK_REG       0x2e

/*
 * Both guests only ever read-modify-write this register, so the reset value
 * does not matter for correctness -- but a model that returns zero for every
 * read is indistinguishable from one whose reads never happen.  Requiring a
 * round trip is what separates the two.
 */

static void i2c_wait_isr(QTestState *qts, uint8_t bit)
{
    int i;

    /*
     * The model completes transfers inside the MMIO handler, so the bit is
     * expected to be set on the very first read.  Loop a bounded number of
     * times anyway: a model that (wrongly) defers to a timer would show up
     * as a timeout here instead of hanging the whole test run.
     */
    for (i = 0; i < 100; i++) {
        if (qtest_readb(qts, KW_ISR) & bit) {
            return;
        }
    }
    g_assert_cmphex(qtest_readb(qts, KW_ISR) & bit, ==, bit);
}

/* Read one byte from subaddress `sub` of the 8-bit address `addr8`. */
static uint8_t i2c_read_sub(QTestState *qts, uint8_t addr8, uint8_t sub)
{
    uint8_t val;

    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_COMBINED | KW_I2C_MODE_100KHZ);
    qtest_writeb(qts, KW_ADDR, addr8 | 1);
    qtest_writeb(qts, KW_SUBADDR, sub);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_XADDR);

    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);
    g_assert_cmphex(qtest_readb(qts, KW_STATUS) & KW_I2C_STAT_LAST_AAK,
                    ==, KW_I2C_STAT_LAST_AAK);

    /*
     * Acking the ADDR interrupt is what produces the first data byte.
     * AAK is deliberately left clear: low_i2c.c only arms it when more
     * than one byte is wanted, and both guests read this chip one byte
     * at a time.
     */
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_ADDR);

    i2c_wait_isr(qts, KW_I2C_IRQ_DATA);
    val = qtest_readb(qts, KW_DATA);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_DATA);
    /* low_i2c.c acks DATA a second time at the end of the block */
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_DATA);

    /*
     * No STOP is written here on purpose.  low_i2c.c ends a read by moving
     * to state_stop and waiting for the interrupt; the controller has to
     * finish the transfer by itself once AAK is clear.
     */
    i2c_wait_isr(qts, KW_I2C_IRQ_STOP);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_STOP);

    return val;
}

/* Write one byte to subaddress `sub` of the 8-bit address `addr8`. */
static void i2c_write_sub(QTestState *qts, uint8_t addr8, uint8_t sub,
                          uint8_t val)
{
    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_STANDARDSUB | KW_I2C_MODE_100KHZ);
    qtest_writeb(qts, KW_ADDR, addr8 & ~1);
    qtest_writeb(qts, KW_SUBADDR, sub);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_XADDR);

    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);
    g_assert_cmphex(qtest_readb(qts, KW_STATUS) & KW_I2C_STAT_LAST_AAK,
                    ==, KW_I2C_STAT_LAST_AAK);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_ADDR);

    qtest_writeb(qts, KW_DATA, val);
    i2c_wait_isr(qts, KW_I2C_IRQ_DATA);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_DATA);

    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_STOP);
    i2c_wait_isr(qts, KW_I2C_IRQ_STOP);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_STOP);
}

/*
 * The registers must be one step apart.  Getting the stride wrong makes
 * every register alias register 0, which still "works" for a while: the
 * guest writes mode, reads it back as status, and only fails later in a
 * way that looks like a transfer problem.
 */
static void test_register_stride(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    qtest_writeb(qts, KW_MODE, 0x21);
    qtest_writeb(qts, KW_SUBADDR, 0x5a);

    g_assert_cmphex(qtest_readb(qts, KW_MODE), ==, 0x21);
    g_assert_cmphex(qtest_readb(qts, KW_SUBADDR), ==, 0x5a);

    /* The window is 4KB (Linux ioremaps AAPL,address for 0x1000). */
    qtest_writeb(qts, KW_I2C_BASE + 0xff0, 0x00);

    qtest_quit(qts);
}

/*
 * The ISR is write-1-to-clear per bit.  low_i2c.c acks ADDR alone while a
 * DATA interrupt may already be pending; a model that stores the written
 * value would drop DATA and the guest would poll forever.
 */
static void test_isr_write_one_to_clear(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_STANDARDSUB);
    qtest_writeb(qts, KW_ADDR, HWCLOCK_ADDR8 & ~1);
    qtest_writeb(qts, KW_SUBADDR, HWCLOCK_REG);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_XADDR);
    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);

    /* Writing a bit that is not set must not disturb the ones that are. */
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_STOP);
    g_assert_cmphex(qtest_readb(qts, KW_ISR) & KW_I2C_IRQ_ADDR,
                    ==, KW_I2C_IRQ_ADDR);

    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_ADDR);
    g_assert_cmphex(qtest_readb(qts, KW_ISR) & KW_I2C_IRQ_ADDR, ==, 0);

    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_STOP);
    i2c_wait_isr(qts, KW_I2C_IRQ_STOP);
    qtest_quit(qts);
}

/*
 * The round trip both guests actually perform.  Reading back what was
 * written is what distinguishes a working transfer from a model that
 * returns zero for everything.
 */
static void test_hwclock_roundtrip(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");
    uint8_t orig, val;

    orig = i2c_read_sub(qts, HWCLOCK_ADDR8, HWCLOCK_REG);

    /* Pulsar: mask 0x77, freeze 0x22 -- the value Linux and Darwin write. */
    i2c_write_sub(qts, HWCLOCK_ADDR8, HWCLOCK_REG, (orig & ~0x77) | 0x22);
    val = i2c_read_sub(qts, HWCLOCK_ADDR8, HWCLOCK_REG);
    g_assert_cmphex(val & 0x77, ==, 0x22);

    /* ... and unfreeze, which is what leaves the chip running. */
    i2c_write_sub(qts, HWCLOCK_ADDR8, HWCLOCK_REG, (val & ~0x77) | 0x11);
    val = i2c_read_sub(qts, HWCLOCK_ADDR8, HWCLOCK_REG);
    g_assert_cmphex(val & 0x77, ==, 0x11);

    qtest_quit(qts);
}

/*
 * An address nobody answers must come back as a NAK, not as a stall.  This
 * is the guard against the hang: a model that leaves the guest waiting for
 * an interrupt that never arrives is worse than one that reports failure.
 */
static void test_unanswered_address_naks(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_STANDARD);
    qtest_writeb(qts, KW_ADDR, 0xa0);          /* nothing at 7-bit 0x50 */
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_XADDR);

    /* The interrupt must still be raised -- only the ack bit differs. */
    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);
    g_assert_cmphex(qtest_readb(qts, KW_STATUS) & KW_I2C_STAT_LAST_AAK,
                    ==, 0);

    /*
     * And STOP has to come without being asked for.  low_i2c.c records
     * -ENXIO, moves to state_stop and waits: it never writes STOP after a
     * NAK, so a controller that only raises ADDR leaves it spinning.
     */
    g_assert_cmphex(qtest_readb(qts, KW_ISR) & KW_I2C_IRQ_STOP,
                    ==, KW_I2C_IRQ_STOP);

    qtest_quit(qts);
}

/*
 * The clock chip hangs off channel 0.  Linux registers two channels because
 * the parent node is /u3, so the second one has to behave -- answering on
 * it would let a wrong channel selection pass unnoticed.
 */
static void test_channel_one_has_no_hwclock(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_STANDARD | 0x10);   /* channel 1 */
    qtest_writeb(qts, KW_ADDR, HWCLOCK_ADDR8 & ~1);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_XADDR);

    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);
    g_assert_cmphex(qtest_readb(qts, KW_STATUS) & KW_I2C_STAT_LAST_AAK,
                    ==, 0);

    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_ADDR);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_STOP);
    i2c_wait_isr(qts, KW_I2C_IRQ_STOP);
    qtest_quit(qts);
}

/*
 * A read has to end on its own.  low_i2c.c never writes STOP for reads: it
 * clears AAK before the final byte, moves to state_stop and waits.  A model
 * that waits for a STOP write instead leaves the guest spinning until it
 * reports "Timeout in i2c transfer on keywest" -- survivable for Linux,
 * which treats a timeout in state_stop as success, and therefore easy to
 * ship without noticing.
 */
static void test_read_stops_without_stop_write(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_COMBINED);
    qtest_writeb(qts, KW_ADDR, HWCLOCK_ADDR8 | 1);
    qtest_writeb(qts, KW_SUBADDR, HWCLOCK_REG);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_XADDR);
    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);

    /* Single byte read: AAK stays clear, as low_i2c.c leaves it. */
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_ADDR);
    i2c_wait_isr(qts, KW_I2C_IRQ_DATA);
    qtest_readb(qts, KW_DATA);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_DATA);

    /* Nothing else is written -- the STOP interrupt must appear anyway. */
    g_assert_cmphex(qtest_readb(qts, KW_ISR) & KW_I2C_IRQ_STOP,
                    ==, KW_I2C_IRQ_STOP);

    qtest_quit(qts);
}

/*
 * A two byte read.  Only one byte is ever needed for the timebase dance,
 * so this path is not exercised by either guest today -- but the whole
 * point of sitting on the I2CBus framework is that other slaves can be
 * added later, and they will read more than one byte.
 *
 * The handshake is subtle: low_i2c.c reads the data register and then
 * immediately acks the DATA interrupt, so a controller that raises the
 * next DATA while the register is being read has it cleared again by that
 * ack.  The next byte has to be produced by the ack itself.
 */
static void test_multi_byte_read(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");
    uint8_t first, second;

    /* Seed two adjacent registers so the two bytes are distinguishable */
    i2c_write_sub(qts, HWCLOCK_ADDR8, HWCLOCK_REG, 0x5a);
    i2c_write_sub(qts, HWCLOCK_ADDR8, HWCLOCK_REG + 1, 0xa5);

    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_COMBINED);
    qtest_writeb(qts, KW_ADDR, HWCLOCK_ADDR8 | 1);
    qtest_writeb(qts, KW_SUBADDR, HWCLOCK_REG);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_XADDR);
    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);

    /* More than one byte wanted, so AAK is armed before the ack */
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_AAK);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_ADDR);

    i2c_wait_isr(qts, KW_I2C_IRQ_DATA);
    first = qtest_readb(qts, KW_DATA);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_DATA);
    /* One byte left: AAK is dropped so the controller finishes after it */
    qtest_writeb(qts, KW_CONTROL, 0);
    /*
     * And only now the second acknowledge, exactly as low_i2c.c does it:
     * the AAK change sits between the two.  A controller that decides what
     * to do next on every acknowledge loses the byte it just fetched.
     */
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_DATA);

    i2c_wait_isr(qts, KW_I2C_IRQ_DATA);
    second = qtest_readb(qts, KW_DATA);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_DATA);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_DATA);

    i2c_wait_isr(qts, KW_I2C_IRQ_STOP);

    g_assert_cmphex(first, ==, 0x5a);
    g_assert_cmphex(second, ==, 0xa5);

    qtest_quit(qts);
}

/*
 * A two byte write.  The same double acknowledge applies: low_i2c.c writes
 * the next byte from inside the data-interrupt handler and then acks DATA
 * at the end of the block, so a controller that raises the interrupt when
 * the register is written has it cleared again straight away.
 */
static void test_multi_byte_write(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_STANDARDSUB);
    qtest_writeb(qts, KW_ADDR, HWCLOCK_ADDR8 & ~1);
    qtest_writeb(qts, KW_SUBADDR, HWCLOCK_REG);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_XADDR);
    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);

    /* First byte goes out from the address handler */
    qtest_writeb(qts, KW_DATA, 0x11);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_ADDR);

    i2c_wait_isr(qts, KW_I2C_IRQ_DATA);
    qtest_writeb(qts, KW_DATA, 0x22);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_DATA);

    /* The interrupt for the second byte has to survive that ack */
    i2c_wait_isr(qts, KW_I2C_IRQ_DATA);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_STOP);
    i2c_wait_isr(qts, KW_I2C_IRQ_STOP);

    /* Both bytes landed in consecutive registers */
    g_assert_cmphex(i2c_read_sub(qts, HWCLOCK_ADDR8, HWCLOCK_REG), ==, 0x11);
    g_assert_cmphex(i2c_read_sub(qts, HWCLOCK_ADDR8, HWCLOCK_REG + 1),
                    ==, 0x22);

    qtest_quit(qts);
}

/*
 * low_i2c.c reads the subaddress register after *every* register write as
 * a write barrier (__kw_write_reg).  Giving that read a side effect breaks
 * both guests while leaving every other test happy.
 */
static void test_subaddr_read_has_no_side_effect(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");
    uint8_t before, after;

    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_COMBINED);
    qtest_writeb(qts, KW_ADDR, HWCLOCK_ADDR8 | 1);
    qtest_writeb(qts, KW_SUBADDR, HWCLOCK_REG);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_XADDR);
    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);

    before = qtest_readb(qts, KW_ISR);
    /* Read it the way the barrier does, several times */
    g_assert_cmphex(qtest_readb(qts, KW_SUBADDR), ==, HWCLOCK_REG);
    g_assert_cmphex(qtest_readb(qts, KW_SUBADDR), ==, HWCLOCK_REG);
    after = qtest_readb(qts, KW_ISR);

    g_assert_cmphex(after, ==, before);

    qtest_quit(qts);
}

/* kw_i2c_xfer() clears the status register before every transfer. */
static void test_status_write_is_accepted(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_STANDARD);
    qtest_writeb(qts, KW_ADDR, HWCLOCK_ADDR8 & ~1);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_XADDR);
    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);
    g_assert_cmphex(qtest_readb(qts, KW_STATUS) & KW_I2C_STAT_BUSY,
                    ==, KW_I2C_STAT_BUSY);

    /*
     * Clear it while the transfer is still busy -- that is the state the
     * guest leaves behind, and only an honoured write can drop the bit
     * here.  Checking after STOP would pass either way, because STOP
     * clears BUSY on its own.
     */
    qtest_writeb(qts, KW_STATUS, 0);
    g_assert_cmphex(qtest_readb(qts, KW_STATUS) & KW_I2C_STAT_BUSY, ==, 0);

    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_STOP);
    i2c_wait_isr(qts, KW_I2C_IRQ_STOP);
    qtest_quit(qts);
}

/*
 * Writing STOP has to end the bus transfer, not just raise the interrupt.
 * QEMU's i2c core keeps the addressed device on the bus until
 * i2c_end_transfer(), and skips the address scan while one is held -- so a
 * controller that only raises the interrupt leaves the previous slave
 * answering for an address that nothing is listening on.
 */
static void test_stop_ends_the_bus_transfer(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    /* Address the clock chip, then abandon the transfer with STOP */
    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_STANDARD);
    qtest_writeb(qts, KW_ADDR, HWCLOCK_ADDR8 & ~1);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_XADDR);
    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);
    g_assert_cmphex(qtest_readb(qts, KW_STATUS) & KW_I2C_STAT_LAST_AAK,
                    ==, KW_I2C_STAT_LAST_AAK);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_ADDR);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_STOP);
    i2c_wait_isr(qts, KW_I2C_IRQ_STOP);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_STOP);

    /* Nothing lives here, so the next address phase has to come back NAK */
    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_STANDARD);
    qtest_writeb(qts, KW_ADDR, 0xa0);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_XADDR);
    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);
    g_assert_cmphex(qtest_readb(qts, KW_STATUS) & KW_I2C_STAT_LAST_AAK,
                    ==, 0);

    qtest_quit(qts);
}

/*
 * The controller belongs to U3, so it must not appear on mac99.
 *
 * Reading the region is not enough to tell: unassigned memory reads back as
 * zero here, and the mode register also resets to zero.  Writing first and
 * checking that nothing stuck is what actually distinguishes the two.
 */
static void test_mac99_has_no_i2c(void)
{
    QTestState *qts = qtest_init("-machine mac99");

    qtest_writeb(qts, KW_MODE, 0x21);
    g_assert_cmphex(qtest_readb(qts, KW_MODE), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine("powermac7_3")) {
        g_test_skip("powermac7_3 machine not available");
        return 0;
    }

    qtest_add_func("/keywest-i2c/register-stride", test_register_stride);
    qtest_add_func("/keywest-i2c/isr-write-one-to-clear",
                   test_isr_write_one_to_clear);
    qtest_add_func("/keywest-i2c/hwclock-roundtrip", test_hwclock_roundtrip);
    qtest_add_func("/keywest-i2c/unanswered-address-naks",
                   test_unanswered_address_naks);
    qtest_add_func("/keywest-i2c/channel-one-has-no-hwclock",
                   test_channel_one_has_no_hwclock);
    qtest_add_func("/keywest-i2c/read-stops-without-stop-write",
                   test_read_stops_without_stop_write);
    qtest_add_func("/keywest-i2c/multi-byte-read", test_multi_byte_read);
    qtest_add_func("/keywest-i2c/multi-byte-write", test_multi_byte_write);
    qtest_add_func("/keywest-i2c/subaddr-read-has-no-side-effect",
                   test_subaddr_read_has_no_side_effect);
    qtest_add_func("/keywest-i2c/status-write-is-accepted",
                   test_status_write_is_accepted);
    qtest_add_func("/keywest-i2c/stop-ends-the-bus-transfer",
                   test_stop_ends_the_bus_transfer);
    qtest_add_func("/keywest-i2c/mac99-has-no-i2c", test_mac99_has_no_i2c);

    return g_test_run();
}
