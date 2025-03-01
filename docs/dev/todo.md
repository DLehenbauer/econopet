# TODOs

* Before release
  * Config
    * Business/Graphics Keyboard
    * 9"/12" (test with 15kHz VGA)
  * Keyboard
    * USB keyboard control
    * PET keyboard control (via read-back)
  * Menu
    * 40/80 Column Switching
* Firmware
  * 8K Video RAM:
    * Move character rom to shadowed I/O region at $E000-$E800?
  * Menu:
    * Enter/Exit without reset?
  * Commit patched PicoDVI (?)
* Compat:
  * Echo last byte on bus when reading unmapped regions.
* HW
  * Parts
    * SMT Tactile Buttons (C318884)
    * SMT Dip for config (C2961921)
      * Maybe could use to switch on/off sound and 5V/Video as well (using diode)?
    * Insufficient 1x headers due to 6V and 5V/VIDEO selector
  * Silk
    * Recommended Assembly order: IDC header accidentally included in 2x header parts
  * Layout
    * These parts should be offset: CPU, RAM, 25-pin header
    * Route ground with signals
    * Check http://jlcdfm.com recommendations
      * Ideal SMD pad to THT is >= 3.05mm (Really?  This seems very large.)
      * Annular rings >0.15mm
