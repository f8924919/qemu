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

Firmware
--------

Since version 0.9.1, QEMU uses OpenBIOS https://www.openbios.org/ for
the g3beige and mac99 PowerMac and the 40p machines. OpenBIOS is a free
(GPL v2) portable firmware implementation. The goal is to implement a
100% IEEE 1275-1994 (referred to as Open Firmware) compliant firmware.
