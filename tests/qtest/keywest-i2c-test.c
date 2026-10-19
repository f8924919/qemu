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
#define KW_REG(n)         (kw_base + ((n) * KW_I2C_STEP))

/*
 * The real Boot ROM describes the controller at 0xf8001003 (AAPL,address)
 * with a step of 0x10, i.e. it uses byte 3 of each 16-byte slot, while
 * OpenBIOS says 0xf8001000.  The lane3 tests move the base to exercise the
 * same helpers through byte 3; everything else uses byte 0.
 */
#define KW_LANE3          3
static uint64_t kw_base = KW_I2C_BASE;

/*
 * The machine the byte-0 tests run on.  main() runs every one of them a
 * second time with x-u3-i2c-lane3=on: decoding byte 3 as well must not
 * change anything a guest does through byte 0.
 */
#define KW_MACHINE        "-machine powermac7_3"
#define KW_MACHINE_LANE3  "-machine powermac7_3,x-u3-i2c-lane3=on"
#define KW_MACHINE_SPD    "-machine powermac7_3,x-u3-i2c-spd=on"
static const char *kw_machine = KW_MACHINE;

/*
 * The channel i2c_read_sub() selects, already shifted into the top nibble
 * of the mode register.  The clock chip is on channel 0; the SPD tests
 * move it to channel 1.
 */
#define KW_CHAN1          0x10
static uint8_t kw_chan;

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
    qtest_writeb(qts, KW_MODE,
                 KW_I2C_MODE_COMBINED | KW_I2C_MODE_100KHZ | kw_chan);
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
    QTestState *qts = qtest_init(kw_machine);

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
    QTestState *qts = qtest_init(kw_machine);

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
    QTestState *qts = qtest_init(kw_machine);
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
    QTestState *qts = qtest_init(kw_machine);

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
    QTestState *qts = qtest_init(kw_machine);

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
    QTestState *qts = qtest_init(kw_machine);

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
    QTestState *qts = qtest_init(kw_machine);
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
    QTestState *qts = qtest_init(kw_machine);

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
    QTestState *qts = qtest_init(kw_machine);
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
    QTestState *qts = qtest_init(kw_machine);

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
    QTestState *qts = qtest_init(kw_machine);

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
 * Darwin's PPCI2CInterface (AppleI2C.kext) never writes the control
 * register outright: it reads it and ORs in AAK or STOP.  XADDR, written
 * to start the transfer, therefore has to read back clear once the start
 * has happened -- otherwise the STOP that ends a write comes back with
 * XADDR still set and restarts the transfer instead of finishing it.
 */
static void test_control_start_bit_self_clears(void)
{
    QTestState *qts = qtest_init(kw_machine);

    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_STANDARDSUB);
    qtest_writeb(qts, KW_ADDR, HWCLOCK_ADDR8 & ~1);
    qtest_writeb(qts, KW_SUBADDR, HWCLOCK_REG);
    qtest_writeb(qts, KW_CONTROL,
                 qtest_readb(qts, KW_CONTROL) | KW_I2C_CTL_XADDR);
    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);
    g_assert_cmphex(qtest_readb(qts, KW_CONTROL) & KW_I2C_CTL_XADDR, ==, 0);

    qtest_quit(qts);
}

/* A one-byte write the way Darwin ends it: STOP read-modify-written */
static void test_darwin_write_stop_rmw(void)
{
    QTestState *qts = qtest_init(kw_machine);

    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_STANDARDSUB);
    qtest_writeb(qts, KW_ADDR, HWCLOCK_ADDR8 & ~1);
    qtest_writeb(qts, KW_SUBADDR, HWCLOCK_REG);
    qtest_writeb(qts, KW_ISR, 0x0f);
    qtest_writeb(qts, KW_CONTROL,
                 qtest_readb(qts, KW_CONTROL) | KW_I2C_CTL_XADDR);
    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);

    /* ADDR: first byte into DATA, then one ack */
    qtest_writeb(qts, KW_DATA, 0x3c);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_ADDR);
    i2c_wait_isr(qts, KW_I2C_IRQ_DATA);

    /* DATA, nothing left: STOP by read-modify-write, then one ack */
    qtest_writeb(qts, KW_CONTROL,
                 qtest_readb(qts, KW_CONTROL) | KW_I2C_CTL_STOP);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_DATA);
    i2c_wait_isr(qts, KW_I2C_IRQ_STOP);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_STOP);

    g_assert_cmphex(i2c_read_sub(qts, HWCLOCK_ADDR8, HWCLOCK_REG), ==, 0x3c);

    qtest_quit(qts);
}

/*
 * A two-byte read in Darwin's order: AAK is settled *before* the single
 * ack of each byte.  Dropping AAK while the first byte is acked means "the
 * next byte is the last", not "stop now" -- a controller that stops there
 * loses the second byte.
 */
static void test_darwin_multi_byte_read(void)
{
    QTestState *qts = qtest_init(kw_machine);
    uint8_t first, second;

    i2c_write_sub(qts, HWCLOCK_ADDR8, HWCLOCK_REG, 0x5a);
    i2c_write_sub(qts, HWCLOCK_ADDR8, HWCLOCK_REG + 1, 0xa5);

    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_COMBINED);
    qtest_writeb(qts, KW_ADDR, HWCLOCK_ADDR8 | 1);
    qtest_writeb(qts, KW_SUBADDR, HWCLOCK_REG);
    qtest_writeb(qts, KW_ISR, 0x0f);
    qtest_writeb(qts, KW_CONTROL,
                 qtest_readb(qts, KW_CONTROL) | KW_I2C_CTL_XADDR);
    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);

    /* ADDR: two bytes wanted, AAK armed, one ack */
    qtest_writeb(qts, KW_CONTROL,
                 qtest_readb(qts, KW_CONTROL) | KW_I2C_CTL_AAK);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_ADDR);

    /* DATA: read, one byte left so AAK is dropped, then one ack */
    i2c_wait_isr(qts, KW_I2C_IRQ_DATA);
    first = qtest_readb(qts, KW_DATA);
    qtest_writeb(qts, KW_CONTROL, 0);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_DATA);

    /* DATA: the last byte, then one ack ends the transfer */
    i2c_wait_isr(qts, KW_I2C_IRQ_DATA);
    second = qtest_readb(qts, KW_DATA);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_DATA);
    i2c_wait_isr(qts, KW_I2C_IRQ_STOP);

    g_assert_cmphex(first, ==, 0x5a);
    g_assert_cmphex(second, ==, 0xa5);

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

/*
 * x-u3-i2c-lane3 (off by default): byte 3 of each slot is a hole, exactly
 * as before, so nothing changes for guests that are not the real ROM.
 */
static void test_lane3_off_is_hole(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3");

    qtest_writeb(qts, KW_MODE, 0x21);
    g_assert_cmphex(qtest_readb(qts, KW_MODE + KW_LANE3), ==, 0);

    qtest_writeb(qts, KW_MODE + KW_LANE3, 0x42);
    g_assert_cmphex(qtest_readb(qts, KW_MODE), ==, 0x21);

    qtest_quit(qts);
}

/*
 * With the switch on, byte 3 is the same register as byte 0, in both
 * directions, for every register that keeps what is written.  The others
 * (control, status, isr, data) are covered by the transfers below.
 */
static void test_lane3_same_register(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3,x-u3-i2c-lane3=on");
    static const unsigned regs[] = { 0, 4, 5, 6 };  /* mode ier addr sub */
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(regs); i++) {
        uint64_t r = KW_REG(regs[i]);

        qtest_writeb(qts, r, 0x10 + i);
        g_assert_cmphex(qtest_readb(qts, r + KW_LANE3), ==, 0x10 + i);
        qtest_writeb(qts, r + KW_LANE3, 0x20 + i);
        g_assert_cmphex(qtest_readb(qts, r), ==, 0x20 + i);
    }

    /* AAK is the one control bit that is held */
    qtest_writeb(qts, KW_CONTROL + KW_LANE3, KW_I2C_CTL_AAK);
    g_assert_cmphex(qtest_readb(qts, KW_CONTROL), ==, KW_I2C_CTL_AAK);
    qtest_writeb(qts, KW_CONTROL, 0);
    g_assert_cmphex(qtest_readb(qts, KW_CONTROL + KW_LANE3), ==, 0);

    qtest_quit(qts);
}

/*
 * The transfer the ROM starts: through byte 3, a read of the clock chip
 * raises ADDR with the ack bit set (i2c_read_sub asserts both) and returns
 * what was written through byte 0.  Writing through byte 3 and reading
 * back through byte 0 shows it is the same chip, not a copy.
 */
static void test_lane3_hwclock(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3,x-u3-i2c-lane3=on");
    uint8_t val;

    i2c_write_sub(qts, HWCLOCK_ADDR8, HWCLOCK_REG, 0x22);
    kw_base = KW_I2C_BASE + KW_LANE3;
    val = i2c_read_sub(qts, HWCLOCK_ADDR8, HWCLOCK_REG);
    g_assert_cmphex(val, ==, 0x22);
    i2c_write_sub(qts, HWCLOCK_ADDR8, HWCLOCK_REG, 0x11);
    kw_base = KW_I2C_BASE;
    val = i2c_read_sub(qts, HWCLOCK_ADDR8, HWCLOCK_REG);
    g_assert_cmphex(val, ==, 0x11);

    qtest_quit(qts);
}

/* Same NAK as test_unanswered_address_naks, through byte 3. */
static void test_lane3_unanswered_address_naks(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3,x-u3-i2c-lane3=on");

    kw_base = KW_I2C_BASE + KW_LANE3;
    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_STANDARD);
    qtest_writeb(qts, KW_ADDR, 0xa0);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_XADDR);

    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);
    g_assert_cmphex(qtest_readb(qts, KW_STATUS) & KW_I2C_STAT_LAST_AAK,
                    ==, 0);
    g_assert_cmphex(qtest_readb(qts, KW_ISR) & KW_I2C_IRQ_STOP,
                    ==, KW_I2C_IRQ_STOP);
    kw_base = KW_I2C_BASE;

    qtest_quit(qts);
}

/*
 * Only byte 3 is added.  The other bytes of a slot, and the slots past the
 * last register, stay holes with the switch on.
 */
static void test_lane3_other_bytes_are_holes(void)
{
    QTestState *qts = qtest_init("-machine powermac7_3,x-u3-i2c-lane3=on");
    static const unsigned lanes[] = { 1, 2, 4, 0xf };
    unsigned i;

    qtest_writeb(qts, KW_MODE, 0x21);
    for (i = 0; i < ARRAY_SIZE(lanes); i++) {
        g_assert_cmphex(qtest_readb(qts, KW_MODE + lanes[i]), ==, 0);
        qtest_writeb(qts, KW_MODE + lanes[i], 0x42);
        g_assert_cmphex(qtest_readb(qts, KW_MODE), ==, 0x21);
    }

    /* Past the last register (data is 7), byte 0 and byte 3 alike */
    qtest_writeb(qts, KW_REG(8), 0x42);
    g_assert_cmphex(qtest_readb(qts, KW_REG(8)), ==, 0);
    qtest_writeb(qts, KW_REG(8) + KW_LANE3, 0x42);
    g_assert_cmphex(qtest_readb(qts, KW_REG(8) + KW_LANE3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, KW_MODE), ==, 0x21);

    qtest_quit(qts);
}

static const struct {
    const char *name;
    void (*fn)(void);
} byte0_tests[] = {
    { "/keywest-i2c/register-stride", test_register_stride },
    { "/keywest-i2c/isr-write-one-to-clear", test_isr_write_one_to_clear },
    { "/keywest-i2c/hwclock-roundtrip", test_hwclock_roundtrip },
    { "/keywest-i2c/unanswered-address-naks", test_unanswered_address_naks },
    { "/keywest-i2c/channel-one-has-no-hwclock",
      test_channel_one_has_no_hwclock },
    { "/keywest-i2c/read-stops-without-stop-write",
      test_read_stops_without_stop_write },
    { "/keywest-i2c/multi-byte-read", test_multi_byte_read },
    { "/keywest-i2c/multi-byte-write", test_multi_byte_write },
    { "/keywest-i2c/subaddr-read-has-no-side-effect",
      test_subaddr_read_has_no_side_effect },
    { "/keywest-i2c/status-write-is-accepted", test_status_write_is_accepted },
    { "/keywest-i2c/stop-ends-the-bus-transfer",
      test_stop_ends_the_bus_transfer },
    { "/keywest-i2c/control-start-bit-self-clears",
      test_control_start_bit_self_clears },
    { "/keywest-i2c/darwin-write-stop-rmw", test_darwin_write_stop_rmw },
    { "/keywest-i2c/darwin-multi-byte-read", test_darwin_multi_byte_read },
};

/* Run one of the byte-0 tests with the lane3 switch on. */
static void test_byte0_with_lane3(gconstpointer data)
{
    kw_machine = KW_MACHINE_LANE3;
    byte0_tests[GPOINTER_TO_UINT(data)].fn();
    kw_machine = KW_MACHINE;
}

/*
 * ... and with the SPD switch on.  The EEPROMs sit on channel 1, so the
 * clock chip on channel 0 and every NAK the byte-0 tests expect (0xa0 is
 * asked for on channel 0) must be unchanged.
 */
static void test_byte0_with_spd(gconstpointer data)
{
    kw_machine = KW_MACHINE_SPD;
    byte0_tests[GPOINTER_TO_UINT(data)].fn();
    kw_machine = KW_MACHINE;
}

/*
 * The real Boot ROM reads the memory SPD on channel 1 at 7-bit 0x50,
 * 0x52, 0x54 and 0x56 with combined transfers.  x-u3-i2c-spd=on puts one
 * pair of DIMMs there, at 0x50 and 0x51, each holding half of the RAM.
 */
#define SPD_ADDR8(a7)     ((a7) << 1)
#define SPD_LEN           64

/* A combined read of `addr8` on channel 1 that must come back as a NAK. */
static void spd_assert_nak(QTestState *qts, uint8_t addr8)
{
    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_COMBINED | KW_CHAN1);
    qtest_writeb(qts, KW_ADDR, addr8 | 1);
    qtest_writeb(qts, KW_SUBADDR, 0);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_XADDR);

    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);
    g_assert_cmphex(qtest_readb(qts, KW_STATUS) & KW_I2C_STAT_LAST_AAK,
                    ==, 0);
    g_assert_cmphex(qtest_readb(qts, KW_ISR) & KW_I2C_IRQ_STOP,
                    ==, KW_I2C_IRQ_STOP);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_ADDR | KW_I2C_IRQ_STOP);
}

/* Read the first SPD_LEN bytes of the EEPROM at `addr8`, one at a time. */
static void spd_read(QTestState *qts, uint8_t addr8, uint8_t *buf)
{
    unsigned i;

    kw_chan = KW_CHAN1;
    for (i = 0; i < SPD_LEN; i++) {
        /* i2c_read_sub() asserts the address was acked */
        buf[i] = i2c_read_sub(qts, addr8, i);
    }
    kw_chan = 0;
}

/*
 * What a DDR SPD from spd_data_generate() looks like, whatever the size:
 * 128 bytes used, 256 bytes in the part, memory type 7, and byte 63 the
 * checksum of bytes 0-62.
 */
static void spd_assert_ddr(const uint8_t *buf)
{
    unsigned i;
    uint8_t sum = 0;

    g_assert_cmphex(buf[0], ==, 128);
    g_assert_cmphex(buf[1], ==, 8);
    g_assert_cmphex(buf[2], ==, 0x07);
    for (i = 0; i < 63; i++) {
        sum += buf[i];
    }
    g_assert_cmphex(buf[63], ==, sum);
}

/* Off by default: channel 1 stays empty, 0x50 included. */
static void test_spd_off_is_empty(void)
{
    QTestState *qts = qtest_init(KW_MACHINE);

    spd_assert_nak(qts, SPD_ADDR8(0x50));
    spd_assert_nak(qts, SPD_ADDR8(0x51));
    qtest_quit(qts);
}

/* Both halves of the pair answer, with the same DDR SPD. */
static void test_spd_pair(void)
{
    QTestState *qts = qtest_init(KW_MACHINE_SPD);
    uint8_t lo[SPD_LEN], hi[SPD_LEN];

    spd_read(qts, SPD_ADDR8(0x50), lo);
    spd_read(qts, SPD_ADDR8(0x51), hi);
    spd_assert_ddr(lo);
    g_assert_cmpmem(lo, sizeof(lo), hi, sizeof(hi));

    qtest_quit(qts);
}

/*
 * A multi-byte read moves through the EEPROM.  This is the handshake of
 * test_multi_byte_read, on channel 1, from subaddress 0.
 */
static void test_spd_multi_byte_read(void)
{
    QTestState *qts = qtest_init(KW_MACHINE_SPD);
    uint8_t one[SPD_LEN], seq[4];
    unsigned i;

    spd_read(qts, SPD_ADDR8(0x50), one);

    qtest_writeb(qts, KW_STATUS, 0);
    qtest_writeb(qts, KW_MODE, KW_I2C_MODE_COMBINED | KW_CHAN1);
    qtest_writeb(qts, KW_ADDR, SPD_ADDR8(0x50) | 1);
    qtest_writeb(qts, KW_SUBADDR, 0);
    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_XADDR);
    i2c_wait_isr(qts, KW_I2C_IRQ_ADDR);
    g_assert_cmphex(qtest_readb(qts, KW_STATUS) & KW_I2C_STAT_LAST_AAK,
                    ==, KW_I2C_STAT_LAST_AAK);

    qtest_writeb(qts, KW_CONTROL, KW_I2C_CTL_AAK);
    qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_ADDR);
    for (i = 0; i < ARRAY_SIZE(seq); i++) {
        i2c_wait_isr(qts, KW_I2C_IRQ_DATA);
        seq[i] = qtest_readb(qts, KW_DATA);
        qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_DATA);
        if (i == ARRAY_SIZE(seq) - 2) {
            /* One byte left */
            qtest_writeb(qts, KW_CONTROL, 0);
        }
        qtest_writeb(qts, KW_ISR, KW_I2C_IRQ_DATA);
    }
    i2c_wait_isr(qts, KW_I2C_IRQ_STOP);

    g_assert_cmpmem(seq, sizeof(seq), one, sizeof(seq));
    qtest_quit(qts);
}

/* Through byte 3, the way the real Boot ROM drives it, the bytes match. */
static void test_spd_lane3(void)
{
    QTestState *qts =
        qtest_init("-machine powermac7_3,x-u3-i2c-lane3=on,x-u3-i2c-spd=on");
    uint8_t b0[SPD_LEN], b3[SPD_LEN];

    spd_read(qts, SPD_ADDR8(0x50), b0);
    kw_base = KW_I2C_BASE + KW_LANE3;
    spd_read(qts, SPD_ADDR8(0x50), b3);
    spd_assert_nak(qts, SPD_ADDR8(0x52));
    kw_base = KW_I2C_BASE;
    spd_assert_ddr(b0);
    g_assert_cmpmem(b0, sizeof(b0), b3, sizeof(b3));

    qtest_quit(qts);
}

/* Only the pair is added: the rest of 0x50-0x57 stays unanswered. */
static void test_spd_other_addresses_nak(void)
{
    QTestState *qts = qtest_init(KW_MACHINE_SPD);
    uint8_t a7;

    for (a7 = 0x52; a7 <= 0x57; a7++) {
        spd_assert_nak(qts, SPD_ADDR8(a7));
    }
    qtest_quit(qts);
}

/* The smallest RAM the pair accepts: 32 MiB per DIMM. */
static void test_spd_smallest_ram(void)
{
    QTestState *qts = qtest_init(KW_MACHINE_SPD " -m 64");
    uint8_t buf[SPD_LEN];

    spd_read(qts, SPD_ADDR8(0x50), buf);
    spd_assert_ddr(buf);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    unsigned i;

    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine("powermac7_3")) {
        g_test_skip("powermac7_3 machine not available");
        return 0;
    }

    for (i = 0; i < ARRAY_SIZE(byte0_tests); i++) {
        g_autofree char *path = NULL;

        qtest_add_func(byte0_tests[i].name, byte0_tests[i].fn);
        path = g_strdup_printf("/keywest-i2c/lane3-on/%s",
                               byte0_tests[i].name + strlen("/keywest-i2c/"));
        qtest_add_data_func(path, GUINT_TO_POINTER(i), test_byte0_with_lane3);
        g_free(path);
        path = g_strdup_printf("/keywest-i2c/spd-on/%s",
                               byte0_tests[i].name + strlen("/keywest-i2c/"));
        qtest_add_data_func(path, GUINT_TO_POINTER(i), test_byte0_with_spd);
    }
    qtest_add_func("/keywest-i2c/mac99-has-no-i2c", test_mac99_has_no_i2c);
    qtest_add_func("/keywest-i2c/lane3-off-is-hole", test_lane3_off_is_hole);
    qtest_add_func("/keywest-i2c/lane3-same-register",
                   test_lane3_same_register);
    qtest_add_func("/keywest-i2c/lane3-hwclock", test_lane3_hwclock);
    qtest_add_func("/keywest-i2c/lane3-unanswered-address-naks",
                   test_lane3_unanswered_address_naks);
    qtest_add_func("/keywest-i2c/lane3-other-bytes-are-holes",
                   test_lane3_other_bytes_are_holes);
    qtest_add_func("/keywest-i2c/spd-off-is-empty", test_spd_off_is_empty);
    qtest_add_func("/keywest-i2c/spd-pair", test_spd_pair);
    qtest_add_func("/keywest-i2c/spd-multi-byte-read",
                   test_spd_multi_byte_read);
    qtest_add_func("/keywest-i2c/spd-lane3", test_spd_lane3);
    qtest_add_func("/keywest-i2c/spd-other-addresses-nak",
                   test_spd_other_addresses_nak);
    qtest_add_func("/keywest-i2c/spd-smallest-ram", test_spd_smallest_ram);

    return g_test_run();
}
