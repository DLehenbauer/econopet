# TODOs

* Before ordering:
  * Try removing current limiting resistor before crystal.
* Before release
  * Config
    * Business/Graphics Keyboard
    * 9"/12" (test with 15kHz VGA)
  * Keyboard
    * Fix USB keyboard bugs
    * Read back from PET keyboard for menu
  * Menu
    * 40/80 Column Switching
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
