# RAM

## 8032.mem.prg

Despite the name, [8032.mem.prg](https://www.zimmers.net/anonftp/pub/cbm/pet/utilities/8032.mem.prg) tests the 64K expansion of the 8096.  It is described in the [listing-demo-instructions](https://mikenaberezny.com/wp-content/uploads/2016/11/listings-demo-instructions.pdf).

'8032.mem.prg' includes an IO peek-through test that relies on the bus holding behavior of later CBM/PET models (it expects to see $E8 at $E800):

```assembly
.C:f974  AD 00 E8    LDA $E800
.C:f977  C9 E8       CMP #$E8
```

There is also 64k addon test in [cbmeqtest.d64](https://www.zimmers.net/anonftp/pub/cbm/pet/utilities/cbmeqtest.d64.gz).

## References

* [CBM 64K RAM Expansion](https://mikenaberezny.com/hardware/pet-cbm/cbm-64k-ram-expansion/)
  * [Overview](https://www.vintagecomputer.net/commodore/8096/CBMPET64KExpansion.pdf)
  * [PET index - 8x96](http://6502.org/users/andre/petindex/8x96.html)
  * [Schematics (Reverse Engineered)](https://www.zimmers.net/anonftp/pub/cbm/schematics/computers/pet/8096/pet_64k.pdf)
