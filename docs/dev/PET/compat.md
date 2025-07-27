# PET/CBM Compatibility Notes

## Bus Holding Behavior in Later PET/CBM Models

In later PET/CBM models, reading from an unmapped address holds the previous byte transferred on the data bus. For example, `LDA $E800` (AD 00 E8) will return `$E8` since `$E800` is unmapped and `$E8` was the last byte previously transferred.

This has the effect of making it appear that unmapped regions are filled with the high byte of the corresponding memory address:

```text
.m 90f0 9108
.:  90f0  90 90 90 90 90 90 90 90
.:  90f8  90 90 90 90 90 90 90 90
.:  9100  91 91 91 91 91 91 91 91
.:  9108  91 91 91 91 91 91 91 91
```

### Testing and Detection

The CBM 64KB Memory Expansion test (`mem.8032.prg`) includes an IO peek-through test that expects to see $E8 at $E800:

```
.C:f974  AD 00 E8    LDA $E800
.C:f977  C9 E8       CMP #$E8
```

The VICE 3.9 emulator approximates the bus holding behavior by returning the high byte of any address read.  This is imperfect and can be detected by using indirect addressing:

```assembly
A9 00       LDA #$00
85 FB       STA $FB
A9 E8       LDA #$E8
85 FC       STA $FC
A0 00       LDY #$00
B1 FB       LDA ($FB),Y
```

This should return `$FB` (the last byte transferred before the read cycle), but will return `$E8` on VICE 3.9.
