# RAM

## Video Memory

The 8296 and 8296D do not have dedicated video ram, the CRTC controller accesses 8KB of main RAM at `$8000-9FFF`.

Need to confirm: when ROM is enabled at $9000, writes still end up in RAM, in which case the upper 4K of the 8K is in practice write only.

## 64KB Expansion

The Commodore Memory Expansion Board adds 64KB of RAM to the Commodore PET/CBM 8032 computer (for a total of 96KB).

### Control Register

The 64K RAM expansion uses a control register at $FFF0.

Bit | Function
----|---------
7   | Enable expansion memory
6   | Enable I/O peek through (`$E800-$E8FF`)
5   | Enable screen peek through (`$8000-$8FFF`)
4   | Reserved
3   | Select bank 2 or 3 (`$C000-$FFFF`)
2   | Select bank 0 or 1 (`$8000-$BFFF`)
1   | Write protect `$8000-$BFFF` (excludes screen peekthrough)
0   | Write protect `$C000-$FFFF` (excludes I/O peekthrough)

### 8032.mem.prg

Despite the name, [8032.mem.prg](https://www.zimmers.net/anonftp/pub/cbm/pet/utilities/8032.mem.prg) tests the 64K expansion of the 8096.  It is described in the [listing-demo-instructions](https://mikenaberezny.com/wp-content/uploads/2016/11/listings-demo-instructions.pdf).

'8032.mem.prg' includes an IO peek-through test that relies on the bus holding behavior of later CBM/PET models (it expects to see `$E8` at `$E800`):

```assembly
.C:f974  AD 00 E8    LDA $E800
.C:f977  C9 E8       CMP #$E8
```

## 8296

The 8296 includes the 8096-style expansion mapping, plus an extra 32KB of general RAM (for 128KB total).  This extra 32KB RAM is mapped as follows:

* `$8000-$8FFF` 4KB of read/write video RAM
* `$9000-$FFFF` sits under ROM decode and is normally "write-only".

The region `$9000-$9FFF` is addressable by the CRTC, bringing the total amount of video RAM to 8KB, provided you don't need readback from the upper 4KB.

Otherwise, the 28KB of RAM sits under ROM decode (`$9000-$FFFF`) can only
be read by the CPU by reconfiguring the machine with jumpers so that user
port pins can be used to unmap the ROM overlay in these regions.

## References

* [CBM 64K RAM Expansion](https://mikenaberezny.com/hardware/pet-cbm/cbm-64k-ram-expansion/)
  * [Overview](https://www.vintagecomputer.net/commodore/8096/CBMPET64KExpansion.pdf)
  * [PET index - 8x96](http://6502.org/users/andre/petindex/8x96.html)
  * [Schematics (Reverse Engineered)](https://www.zimmers.net/anonftp/pub/cbm/schematics/computers/pet/8096/pet_64k.pdf)
* [Commodore PET Programming Model](http://6502.org/users/andre/petindex/progmod.html)
* Diagnostics
  * [8032.mem.prg](https://www.zimmers.net/anonftp/pub/cbm/pet/utilities/8032.mem.prg)
  * [cbmeqtest.d64](https://www.zimmers.net/anonftp/pub/cbm/pet/utilities/cbmeqtest.d64.gz)
  * [cbm-burnin-tests](https://github.com/fachat/cbm-burnin-tests)
