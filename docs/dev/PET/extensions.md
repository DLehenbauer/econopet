## Memory Mapped Extensions
 
Range         | Description
--------------|-------------
`$8000-$9FFF` | 8KB video RAM
`$8400-$87FF` | ColourPET (40 col - color data at `$8400`)
`$8800-$8FFF` | ColourPET (80 col - color data at `$8800`)
`$8F00-$8FFF` | SID cartridge (emulated by VICE)
`$?-$?`       | Future: Writable character bitmaps (TBD)
`$?-$?`       | Future: Some way to identify EconoPET revision (TBD)

## Desired Control Bits

* 1 bit: toggle 40/80 column mode
* 2 bits: Video RAM mask (00 = 1KB, 01 = 2KB, 10 = 4KB, 11 = 8KB)
* 2 bit: ColourPET mode (00 = off, 01 = RGBI, 10 = Palette, 11 = Analog)
* 1 bit: ColourPET color data address (`$8400` or `$8800`)
* 1 bit: SID at $8F00
* 1 bit: Writable character bitmaps (at what address?)
* 7 bits: Write enable "ROMs" (1 bit each for `$9xxx`, `$Axxx`, etc.)

## Potential Control Register Locations

* 64KB RAM expansion control register at `$FFF0` has an unused/reserved bit (bit 4)
* CRTC registers 18-31 are writable, but unused
* There are 16 unmapped bytes at `$E800-$E80F` (accessible with I/O peek-through enabled)

## Thoughts

* For maximum compatibility, use $FFF0 bit 4 to enable/disable all extensions
* Consider reserving all or part of `$E800-$E80F` for ring-buffer communication with FPGA/MCU
* Potential issues with changing currently selected CRTC register?
* ColourPET RGBI mode seems redundant with palette mode. Is it worth keeping?
