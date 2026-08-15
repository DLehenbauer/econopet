# ROM and disk images

`config.yaml` expects ROM images in `/roms` on the SD card. The directory is
not committed; release archives include it, and `tools/fetch-roms.sh`
downloads the zimmers.net images into `ECONOPET_ROMS_DIR`. The remaining
images are listed below with their sources.

## Stock PET ROMs

From <https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/>:

- `rom-1-*.901439-*.bin` -- PET 2001 (ROM 1.0) series
- `basic-2-*.901465-01/02.bin`, `kernal-2.901465-03.bin`, `edit-2-n.901447-24.bin`,
  `edit-2-b.901474-01.bin` -- BASIC 2 machines
- `basic-4-*.901465-20/21/23.bin`, `kernal-4.901465-22.bin` -- BASIC 4 machines
- `edit-4-40-n-50Hz.901498-01.bin`, `edit-4-40-n-60Hz.901499-01.bin`,
  `edit-4-80-b-50Hz.901474-04-3681.bin`, `edit-4-80-b-60Hz.901474-03.bin` -- editors
- `characters-1.901447-08.bin`, `characters-2.901447-10.bin` -- character generators

## Other

- `rom1diskrom_v15.bin` -- ROM 1.0 disk support,
  <https://hub.inktada.com/channel/rom1diskmagic>
- `colourpet-n40.bin`, `colourpet-b40.bin` -- ColourPET preview, from the
  EconoPET project releases
