# TODOs

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
* Future
  * Parts
    * SMT Tactile Buttons (C318884)
    * SMT Dip for config (C2961921)
  * Relayout
    * These parts should be offset: CPU, RAM, 25-pin header
    * Route ground with signals
    * Check http://jlcdfm.com recommendations
      * Ideal SMD pad to THT is >= 3.05mm
      * Anular rings >0.15mm
