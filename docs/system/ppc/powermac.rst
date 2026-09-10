PowerMac family boards (``g3beige``, ``mac99``, ...)
==================================================================

Use the executable ``qemu-system-ppc`` to simulate a complete PowerMac
PowerPC system.

- ``g3beige``              Heathrow based PowerMac
- ``mac99``                Mac99 based PowerMac
- ``powermac3_1``          Apple Power Mac G4 AGP (Sawtooth), fixed
  configuration equivalent to ``mac99,via=pmu``
- ``powerbook3_2``         Apple PowerBook G4 Titanium (Mercury), fixed
  configuration equivalent to ``mac99,via=pmu-adb``
- ``powermac7_3``          Apple Power Mac G5 (Niagara), 970FX based,
  only available in ``qemu-system-ppc64``

Supported devices
-----------------

QEMU emulates the following PowerMac peripherals:

 *  UniNorth or Grackle PCI Bridge
 *  PCI VGA compatible card with VESA Bochs Extensions
 *  2 PMAC IDE interfaces with hard disk and CD-ROM support
 *  NE2000 PCI adapters
 *  Non Volatile RAM
 *  VIA-CUDA with ADB keyboard and mouse.


Missing devices
---------------

 * To be identified

Non Volatile RAM
----------------

The NVRAM holds the Open Firmware variables, and the guest operating
systems keep their own settings there as well.  It starts out empty
unless a backing image is given, and its contents are lost when QEMU
exits.

To keep them, pass an image with ``-drive if=mtd,file=<filename>,format=raw``,
or give the drive an ID (``-drive if=none,file=<filename>,format=raw,id=nvid``)
and hand that to the device with ``-global macio-nvram.drive=nvid``.  The
image has to be the size of the part: 16 KiB on ``mac99`` and its
derivatives, 8 KiB on ``g3beige``.

An image that does not hold an NVRAM yet - an empty file of the right
size will do - is initialised on the first run and written back, so
``-prom-env`` still has its usual effect.  Once the image holds one it is
left alone, and the settings in it win over ``-prom-env``.  Note that
``-snapshot`` covers this drive too, so nothing is kept with it.

Migrating a machine hands its NVRAM to the image on the destination as
well.  That happens once the destination is running, so an image read
right after the migration has finished still holds what it did before, and
one on a destination started with ``-S`` is only brought up to date by
``cont``.  The size of the part is not part of what is migrated, so both
ends have to be the same machine.

Firmware
--------

Since version 0.9.1, QEMU uses OpenBIOS https://www.openbios.org/ for
the g3beige and mac99 PowerMac and the 40p machines. OpenBIOS is a free
(GPL v2) portable firmware implementation. The goal is to implement a
100% IEEE 1275-1994 (referred to as Open Firmware) compliant firmware.
