# Diagnostics

## Available Diagnostics

| Test | Type | Notes |
| --- | --- | --- |
| [cbm-burnin-tests](https://github.com/fachat/cbm-burnin-tests/archive/refs/heads/master.zip) | RAM/ROM + 64K | Updated classic PET burn-in suite. ([README](https://github.com/fachat/cbm-burnin-tests#readme)) |
| [8032.mem.prg](https://www.zimmers.net/anonftp/pub/cbm/pet/utilities/8032.mem.prg) | 64K Only | Despite the name, this program tests the 64K expansion board only. |
| [testram9000.d80.gz](https://www.zimmers.net/anonftp/pub/cbm/pet/SuperPET/os9/testram9000.d80.gz) | SuperPET Only | SuperPET OS/9 disk with RAM tests. |
| [4.0 System RAM/ROM Test V300](https://www.zimmers.net/anonftp/pub/cbm/pet/utilities/cbm_technician_disk.d80.gz) | RAM/ROM | On the CBM technician disk, run `4.0 system 40c` or `40xx memory test` for 4.0 ROMs. |
| [RAM/ROM TT110 V250](https://www.zimmers.net/anonftp/pub/cbm/pet/utilities/cbm_technician_disk.d80.gz) | RAM/ROM | On the CBM technician disk, run `40c 2.0` for upgrade ROMs. |
| [System Burn-In V2007TT](https://www.zimmers.net/anonftp/pub/cbm/pet/utilities/cbm_technician_disk.d80.gz) | RAM/ROM | On the CBM technician disk, run `80sysburn1039` or `80xx memory test` for 80-column systems. |
| [sp9000](https://www.zimmers.net/anonftp/pub/cbm/pet/utilities/cbm_technician_disk.d80.gz) | SuperPET Only | On the CBM technician disk, run `sp9000` to test SuperPET banked memory. |

## EconoPET Firmware Diagnostics

The serial console offers the continuous `test memory` and `test sd` plans. The
memory plan runs an Extended March C- pass over EconoPET SRAM and BRAM.

The SD plan is destructive. It formats the SD card and exercises the block
device, FatFs, VFS, POSIX, and production-shaped standard I/O operations. Use a
sacrificial card. The guest stops and its storage is flushed and unmounted before the console
asks you to remove the original card and insert the sacrificial card. The
hardware design does not recommend powered card insertion. Once insertion is
detected, a ten-second countdown begins. Press any key to abort or power off.
Card removal during the countdown does not abort it. Do not change cards during
the countdown: the card present when the test starts will be destroyed. An abort
resets the MCU (the guest cannot be resumed). A storage preparation failure
halts with a fatal error. The inserted card may not contain the files needed to
boot.
The detection switch cannot verify card identity, so inserting the original card
will destroy its data.

The firmware SD diagnostic always uses the physical card and production storage
APIs. Host `sd_diag_tests` compile the same diagnostic source with test-only SDK,
VFS, and stdio fakes over a heap-backed FAT card. These tests check diagnostic
behavior and failure reporting, not physical SD or newlib integration.
The diagnostic uses only EconoPET's production SD SPI configuration (configured
speed and CRC setting), including checks that rebuild the transport.

The existing suites cover read-only fallback, independent configuration readers,
loader chunk boundaries, exact directory identities, and eight unbuffered image
streams with a ninth replacement candidate. Tagged D64/D80/D82-sized containers
are fully checked after a clean transport/root remount without formatting.
These are stream-operation tests, not native DOS-format validation or the
planned product-code I/O stress suite.

## References

* Burn-in Tests
  * [4.0 System RAM/ROM Test V300](https://www.zimmers.net/anonftp/pub/cbm/pet/utilities/cbm_technician_disk.d80.gz) - Run '4.0 system 40c' or '40xx memory test' - For 4.0 roms
  * [RAM/ROM TT110 V250](https://www.zimmers.net/anonftp/pub/cbm/pet/utilities/cbm_technician_disk.d80.gz) - Run '40c 2.0' -- For upgrade roms
  * [System Burn-In V2007TT](https://www.zimmers.net/anonftp/pub/cbm/pet/utilities/cbm_technician_disk.d80.gz) - Run '80sysburn1039' or '80xx memory test' -- For 80 col
* memory tests
  * [sp9000](https://www.zimmers.net/anonftp/pub/cbm/pet/utilities/cbm_technician_disk.d80.gz) - Run 'sp9000' -- SuperPET banked memory



* [cbm-burnin-tests](https://github.com/fachat/cbm-burnin-tests)
* Memory Test
  * SuperPET Memmory Tests: [testram9000.d80.gz](https://www.zimmers.net/anonftp/pub/cbm/pet/SuperPET/os9/testram9000.d80.gz)
* Diagnostic Clip
  * [Andre's PET Diagnostic Site](https://6502.org/users/andre/petindex/diag/index.html)
  * [diagnostics.txt](https://www.zimmers.net/anonftp/pub/cbm/schematics/computers/pet/diagnostics.txt)