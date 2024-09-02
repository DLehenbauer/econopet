# Rev B. TODOs

* HW Design
  * YSX321SL requires XOSC workaround:
    * Most designs using YSX321L use 33pF
    * Try removing/shorting resistor to see if value too large?
    * Typical guidelines are 100 ohm + 1M parallel resistor to assist startup.
    * Other recommendations include a "guard ring" of vias.
    * [This project](https://github.com/Swyter/psdaptwor/tree/master) claims that 100 ohm + 33pF is "known good".
  * GPIO15 for fpga_clk conflicts USB reset fix for old RP2040 silicon
* Silkscreen
  * Front:
    * Consider adding boxes for SSN (`[] [] [] [] [] - [] []`).
  * Back:
    * Label remaining ports (IEEE, USER, KEYBOARD, etc.)
    * Label remaining pins (C[7:0], R[9:0], etc)
