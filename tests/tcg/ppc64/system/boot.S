/*
 * Bare-metal boot code for ppc64 system tests on the PowerMac7,3.
 *
 * The image is passed with -bios, so it is placed in the ROM region and the
 * 970 starts executing at PROM_BASE + 0x100 with MSR = 0x8000000000000000
 * (64-bit mode, MMU off).  No firmware runs at all, which means we get a
 * clean and fully deterministic machine state, but also that we have to
 * bring up the little bit of PCI we need ourselves.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* U3 HT configuration space is a flat, memory mapped window. */
#define U3_CFA0		0xf2000000	/* bus 0:  + (devfn << 8) + off */
#define U3_CFA1		0xf3000000	/* bus n: + (bus<<16) + (devfn<<8) */

#define BRIDGE_DEVFN	0x18		/* K2 HT-PCI bridge, bus 0 dev 3 fn 0 */
#define MACIO_DEVFN	0x38		/* K2 mac-io,        bus 1 dev 7 fn 0 */

#define MACIO_BAR	0xfa200000	/* inside the 16MB HT memory window */
#define ESCC_CTRL	(MACIO_BAR + 0x13020)	/* channel A control */
#define ESCC_DATA	(MACIO_BAR + 0x13030)	/* channel A data */
#define VIA_PMU		(MACIO_BAR + 0x16000)

#define SPR_HIOR	311		/* exception vector base on the 970 */

/*
 * Load a constant address.  lis sign extends, so in 64-bit mode the top
 * half has to be cleared explicitly.
 */
	.macro	LOAD32 reg, val
	lis	\reg, (\val) >> 16
	ori	\reg, \reg, (\val) & 0xffff
	clrldi	\reg, \reg, 32
	.endm

/*
 * Same for a link-time symbol.  @h would be rejected here: the linker
 * checks that the value fits in a sign extended 32-bit field, which
 * addresses in the ROM region do not.
 */
	.macro	LOADSYM reg, sym
	lis	\reg, (\sym)@highest
	ori	\reg, \reg, (\sym)@higher
	rldicr	\reg, \reg, 32, 31
	oris	\reg, \reg, (\sym)@h
	ori	\reg, \reg, (\sym)@l
	.endm

/*
 * Configuration space is little endian (u3_ht_cfg_ops is
 * DEVICE_LITTLE_ENDIAN), so byte accesses are used throughout: they are
 * the only width that is not byte swapped on the way through.
 */
	.macro	CFGB val, off		/* r12 = config base for the function */
	li	7, \val
	stb	7, \off(12)
	.endm

	.macro	PMUB byte		/* one byte to the PMU (r6 = VIA) */
	li	7, \byte
	stb	7, 0x1400(6)		/* SR */
	li	7, 0x00
	stb	7, 0x0000(6)		/* B: TREQ low  -> byte latched */
	li	7, 0x10
	stb	7, 0x0000(6)		/* B: TREQ high -> back to idle */
	.endm

/*
 * _start has to be the very first byte of the image: the reset vector is
 * a fixed address, nothing looks at e_entry.  Its own section keeps it
 * ahead of the test object, which the link line puts first.
 */
	.section .text.boot, "ax", @progbits
	.globl _start
_start:
	/* ---- bring up the K2 HT-PCI bridge (bus 0, dev 3, fn 0) ---- */
	LOAD32	12, (U3_CFA0 + (BRIDGE_DEVFN << 8))
	CFGB	0x00, 0x18		/* primary bus     = 0 */
	CFGB	0x01, 0x19		/* secondary bus   = 1 */
	CFGB	0x01, 0x1a		/* subordinate bus = 1 */
	CFGB	0x00, 0x20		/* memory base  = 0xfa000000 */
	CFGB	0xfa, 0x21
	CFGB	0xf0, 0x22		/* memory limit = 0xfaffffff */
	CFGB	0xfa, 0x23
	/*
	 * The bridge window alias stays zero sized until memory decoding is
	 * enabled, no matter what base/limit say.  QEMU is supposed to set
	 * this up itself but the generic PCI reset clears it again, so do it
	 * here (see the qemu-g5 issue about u3_ht_pci_bridge_setup).
	 */
	CFGB	0x02, 0x04		/* COMMAND = memory space enable */

	/* ---- give mac-io a BAR and enable it (bus 1, dev 7, fn 0) ---- */
	LOAD32	12, (U3_CFA1 + (1 << 16) + (MACIO_DEVFN << 8))
	CFGB	0x00, 0x10		/* BAR0 = MACIO_BAR */
	CFGB	0x00, 0x11
	CFGB	0x20, 0x12
	CFGB	0xfa, 0x13
	CFGB	0x02, 0x04		/* COMMAND = memory space enable */

	/*
	 * ---- enable the ESCC transmitter ----
	 * escc_mem_write() gates every character on TXCTRL2_TXEN, so without
	 * this the console is silent even though everything else works.
	 */
	LOAD32	3, ESCC_CTRL
	li	4, 5			/* select WR5 */
	stb	4, 0(3)
	li	4, 0x68			/* Tx 8 bits | TxEnable */
	stb	4, 0(3)

	/*
	 * Writable sections are linked *and loaded* into RAM (the ROM only
	 * holds the text), so there is nothing to relocate here, and guest
	 * RAM starts out zeroed so .bss needs no clearing either.
	 */
	/*
	 * ---- install catch-all exception handlers ----
	 * Without these an unexpected exception just wanders off and the test
	 * dies by timeout, with nothing on the console to say why.  Tests that
	 * want to take an exception on purpose overwrite the vector they need.
	 */
	li	3, 0
	mtspr	SPR_HIOR, 3		/* vectors at real 0x000 */
	isync

	LOADSYM	10, vec_table
20:	lwz	11, 0(10)		/* vector address; 0 ends the list */
	cmpwi	11, 0
	beq	23f
	addi	10, 10, 4
	LOADSYM	5, vec_stub
	mr	6, 11
	li	7, (vec_stub_end - vec_stub) / 4
	mtctr	7
21:	lwz	8, 0(5)
	stw	8, 0(6)
	dcbst	0, 6
	sync
	icbi	0, 6
	addi	5, 5, 4
	addi	6, 6, 4
	bdnz	21b
	b	20b
23:	isync

	/* ---- C environment: stack and TOC ---- */
	LOADSYM	1, stack_end
	subi	1, 1, 256		/* leave a bit of headroom */
	LOADSYM	2, .TOC.		/* the linker placed the TOC */

	bl	main
	b	_exit			/* the return value is not passed on */

/*
 * Ask the PMU to power the machine down.  This is the only way a guest can
 * make QEMU exit on this board, and it always exits with status 0 -- the
 * pass/fail decision is made by grepping the console output instead.
 */
	.section .text
	.globl _exit
_exit:
	LOAD32	6, VIA_PMU
	li	7, 0x10
	stb	7, 0x0400(6)		/* DIRB: TREQ out, TACK an input */
	li	7, 0x10
	stb	7, 0x1600(6)		/* ACR: shift register out */
	PMUB	0x7e			/* PMU_SHUTDOWN, 4 argument bytes */
	PMUB	'M'
	PMUB	'A'
	PMUB	'T'
	PMUB	'T'
5:	b	5b			/* shutdown is asynchronous */

	.globl __sys_outc
__sys_outc:
	LOAD32	4, ESCC_DATA
	stb	3, 0(4)
	blr

/*
 * The stub is copied verbatim to every vector, so it has to work at any
 * address: it recovers its own location instead of being patched.
 */
	.section .text
vec_stub:
	bl	30f
30:	mflr	3
	subi	3, 3, 4			/* r3 = address of this vector */
	lis	0, unexpected_exception@highest
	ori	0, 0, unexpected_exception@higher
	rldicr	0, 0, 32, 31
	oris	0, 0, unexpected_exception@h
	ori	0, 0, unexpected_exception@l
	mtctr	0
	bctr
vec_stub_end:

/*
 * Report and give up.  Nothing here needs a stack: an exception can land
 * us anywhere, including before the stack pointer has been set up.
 */
unexpected_exception:
	mr	31, 3
	LOADSYM	3, str_unexpected
	bl	puts_asm
	mr	3, 31
	bl	put_hex4
	LOADSYM	3, str_nl
	bl	puts_asm
	b	_exit

/* r3 = NUL terminated string */
puts_asm:
	mflr	30
	mr	29, 3
31:	lbz	3, 0(29)
	cmpwi	3, 0
	beq	32f
	bl	__sys_outc
	addi	29, 29, 1
	b	31b
32:	mtlr	30
	blr

/* r3 = value, printed as four hex digits */
put_hex4:
	mflr	28
	mr	27, 3
	li	26, 3
33:	slwi	25, 26, 2
	srw	3, 27, 25
	andi.	3, 3, 0xf
	cmpwi	3, 9
	ble	34f
	addi	3, 3, 'a' - '0' - 10
34:	addi	3, 3, '0'
	bl	__sys_outc
	addic.	26, 26, -1
	bge	33b
	mtlr	28
	blr

	.section .rodata
str_unexpected:
	.string	"FAIL: unexpected exception at 0x"
str_nl:
	.string	"\n"

vec_table:
	.long	0x100, 0x200, 0x300, 0x380, 0x400, 0x480
	.long	0x500, 0x600, 0x700, 0x800, 0x900
	.long	0xc00, 0xd00, 0xf00
	.long	0
