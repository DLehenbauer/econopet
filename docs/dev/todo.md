# TODOs

* Before release
  * Hang on reset?
* Firmware
  * 8K Video RAM:
    * Move character rom to shadowed I/O region at $E000-$E800?
    * (Will require gw support for "ghost byte" readback for compat.)
  * Menu:
    * Enter/Exit without reset?
  * Commit patched PicoDVI (?)
* Gateware:
  * Handle reset for:
    * Keyboard
  * Consider CPU addr_strobe signal?
  * Use writes to VIA/CA2 to cache Gfx state instead of physical pin?
  * Remove unused *.sv files (cpu, arbiter, bram, vsync, io, ??)
  * Generalize IO shadowing and interception for keyboard and (future) IEEE, etc.
* Compat:
  * Echo last byte on bus when reading unmapped regions. ("ghost byte")
* HW
  * Straighten bumped traces below User Port header (front)
  * Parts
    * SMT Tactile Buttons (C318884)
    * SMT Dip for config (C2961921)
      * Maybe could use to switch on/off sound and 5V/Video as well (using diode)?
    * Insufficient 1x headers due to 6V and 5V/VIDEO selector (but maybe we can use scrap from 2x header?)
  * Silk
    * Recommended Assembly order: IDC header accidentally included in 2x header parts
    * Reorder so that we do large headers first and in an order that ensures scraps will have sufficient parts.
  * Layout
    * These parts should be offset: CPU, RAM, 25-pin header
    * Route ground with signals
    * Check http://jlcdfm.com recommendations
      * Ideal SMD pad to THT is >= 3.05mm (Really? This seems very large.)
      * Annular rings >0.15mm
