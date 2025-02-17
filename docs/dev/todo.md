# TODOs

* Before ordering:
  * Figure out NDAC/ATN report on 8296 burn-in
  * Try removing current limiting resistor before crystal.
  * Verify FPGA GPIO pin assignments in Efinity
* Before release
  * Config
    * Business/Graphics Keyboard
    * 9"/12" (test with 15kHz VGA)
  * Keyboard
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
* Future
  * Relayout
    * These parts should be offset: CPU, RAM, 25-pin header
    * Route ground with signals
    * Check http://jlcdfm.com recommendations
      * Ideal SMD pad to THT is >= 3.05mm
      * Anular rings >0.15mm


    