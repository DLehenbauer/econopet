# TODOs

* Before ordering:
  * Try removing current limiting resistor before crystal.
* Before release
  * Config
    * Business/Graphics Keyboard
    * 9"/12" (test with 15kHz VGA)
  * Keyboard
    * Fix USB keyboard bugs
    * USB keyboard control
    * PET keyboard control (via readback)
  * Menu
    * 40/80 Column Switching
* Firmware
  * 8K Video RAM:
    * Move character rom to shadowed I/O region at $E000-$E800?
  * Menu:
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
