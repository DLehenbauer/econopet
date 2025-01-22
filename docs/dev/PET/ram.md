# RAM

## 8032.mem.prg

Despite the name, [8032.mem.prg]() tests the 64K expansion of the 8096.  It is described in the [listing-demo-instructions](https://mikenaberezny.com/wp-content/uploads/2016/11/listings-demo-instructions.pdf).

'8032.mem.prg' includes an IO peek-through test that expects to see $E8 at $E800:

```
.C:f974  AD 00 E8    LDA $E800
.C:f977  C9 E8       CMP #$E8
.C:f979  F0 07       BEQ $F982
.C:f97b  A9 50       LDA #$50
.C:f97d  85 02       STA $02
.C:f97f  4C 86 F9    JMP $F986
```

VICE shows that $E800-$E80F are E8 at power-on:

```
(C:$d000) m e800 f000
>C:e800  e8 e8 e8 e8  e8 e8 e8 e8  e8 e8 e8 e8  e8 e8 e8 e8   ................
>C:e810  f9 3c ff 3d  f9 3c ff 3d  f9 3c ff 3d  f9 3c ff 3d   .<.=.<.=.<.=.<.=
>C:e820  ff 3c ff 3c  ff 3c ff 3c  ff 3c ff 3c  ff 3c ff 3c   .<.<.<.<.<.<.<.<
>C:e830  f9 3c ff 3c  f9 3c ff 3c  f9 3c ff 3c  f9 3c ff 3c   .<.<.<.<.<.<.<.<
>C:e840  ff ff 1e 00  82 9f ff ff  47 8a 00 00  0c 40 80 ff   ........G....@..
>C:e850  f9 3c 1e 00  80 1c ff 3d  41 08 00 00  08 00 80 3d   .<.....=A......=
>C:e860  ff 3c 1e 00  82 1c ff 3c  47 08 00 00  0c 00 80 3c   .<.....<G......<
>C:e870  f9 3c 1e 00  80 1c ff 3c  41 08 00 00  08 00 80 3c   .<.....<A......<
```

Note that on non-CRTC model, E880-E8FF mirrors E800-E87F:

```
>C:e880  e8 e8 e8 e8  e8 e8 e8 e8  e8 e8 e8 e8  e8 e8 e8 e8   ................
>C:e890  f9 3c ff 3d  f9 3c ff 3d  f9 3c ff 3d  f9 3c ff 3d   .<.=.<.=.<.=.<.=
>C:e8a0  ff 3c ff 3c  ff 3c ff 3c  ff 3c ff 3c  ff 3c ff 3c   .<.<.<.<.<.<.<.<
>C:e8b0  f9 3c ff 3c  f9 3c ff 3c  f9 3c ff 3c  f9 3c ff 3c   .<.<.<.<.<.<.<.<
>C:e8c0  ff ff 1e 00  82 9f ff ff  47 8a 00 00  0c 40 80 ff   ........G....@..
>C:e8d0  f9 3c 1e 00  80 1c ff 3d  41 08 00 00  08 00 80 3d   .<.....=A......=
>C:e8e0  ff 3c 1e 00  82 1c ff 3c  47 08 00 00  0c 00 80 3c   .<.....<G......<
>C:e8f0  f9 3c 1e 00  80 1c ff 3c  41 08 00 00  08 00 80 3c   .<.....<A......<
>C:e900  07 07 07 07  07 07 07 07  07 07 07 07  07 07 07 07   ................
>C:e910  07 07 07 07  07 07 07 07  07 07 07 07  07 07 07 07   ................
```

On CRTC models:

* E800-E80F: E8
* E880-E8FF: 00
* E900-E9FF: E9
* EA00-EAFF: EA
* EB00-EBFF: EB
* EC00-ECFF: EC
* ED00-EDFF: ED
* EE00-EEFF: EE
* EF00-EFFF: EF

## References

* [CBM 64K RAM Expansion](https://mikenaberezny.com/hardware/pet-cbm/cbm-64k-ram-expansion/)
  * [Overview](https://www.vintagecomputer.net/commodore/8096/CBMPET64KExpansion.pdf)
  * [PET index - 8x96](http://6502.org/users/andre/petindex/8x96.html)
  * [Schematics (Reverse Engineered)](https://www.zimmers.net/anonftp/pub/cbm/schematics/computers/pet/8096/pet_64k.pdf)

