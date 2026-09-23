# SuperPET

## Memory Test

Memory tests are available in [testram9000.d80.gz](https://www.zimmers.net/anonftp/pub/cbm/pet/SuperPET/os9/testram9000.d80.gz):
  
* Mount as disk8/1
* Boot in 6809 mode
* Type `test.main`, `test.banks`, or `test.os9` from the Waterloo microSystems menu
* Use `edit` and `g README` to view the test instructions.

## OS-9

OS-9 path | IEEE device | Physical drive | REL file
---------:|:-----------:|:--------------:|:-----------
/d0       |           8 |              0 | OS9 DRIVE A
/d1       |           8 |              1 | OS9 DRIVE A
/d2       |           8 |              0 | OS9 DRIVE B
/d3       |           8 |              1 | OS9 DRIVE B

## References

* Hardware
  * [CommonPET](https://github.com/InsaneDruid/CommonPET/tree/main) replica of the SuperPET combo board.
  * MMU
    * [SuperPET OS/9 MMU](https://mikenaberezny.com/hardware/superpet/super-os9-mmu)
    * [Schematic](https://mikenaberezny.com/wp-content/uploads/2018/03/superos9-mmu-schematic-nils-eilers.pdf)
    * [Instructions](https://mikenaberezny.com/wp-content/uploads/2009/11/install-2-boards.txt)
  * 6702
    * [Patching Waterloo Disks](https://mikenaberezny.com/wp-content/uploads/2012/04/Waterloo-D64-Disk-Patches.pdf)
* Waterloo
  * [ROM Disassemblies](https://mikenaberezny.com/hardware/superpet/disassemblies/)
  * [Waterloo Languages on the SuperPET](https://mikenaberezny.com/hardware/superpet/waterloo-languages/)
* OS-9
  * [System Disk](https://mikenaberezny.com/wp-content/uploads/2009/11/os9-systemdisk.d80)
  * [OS-9 Operating System User's Guide](https://www.roug.org/retrocomputing/os/os9/os9guide.pdf)
  * [Commodore SuperPET MMU OS/9 Software Notes](https://retro-bobbel.de/zimmers/cbm/pet/SuperPET/os9/OS9_SW_Docs.pdf)

