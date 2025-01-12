# TODOs

* Before ordering:
  * Config DIPs
  * 40/80 Column Switching
  * 96K RAM (?)
  * Try removing current limiting resistor before crystal.
* Firmware
  * USB keyboard bug
  * Menu:
    * PET keyboard control
    * USB keyboard control
    * Enter/Exit without reset?
  * Commit patched PicoDVI (?)
* HW Design
  * Swap HOLD and +5V on IDC so Pico programmer can power board?
  * GPIO15 for fpga_clk conflicts USB reset fix for old RP2040 silicon
* Silkscreen
  * Front:
    * Consider adding boxes for SSN (`[] [] [] [] [] - [] []`).
  * Back:
    * Label remaining ports (IEEE, USER, KEYBOARD, etc.)
    * Label remaining pins (C[7:0], R[9:0], etc)
