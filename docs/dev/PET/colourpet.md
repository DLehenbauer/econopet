# ColourPET Palette Overview

This document summarizes how the `cbm-edit-rom` project implements colour handling (palette, storage, and usage) for PET/CBM systems, with pointers to the relevant source files in `rom/external/cbm-edit-rom`.

## Modes and Formats
- **Digital mode (RGBI, 16 colours):**
  - One byte per screen cell in colour RAM.
  - Lower nibble = foreground; upper nibble = background.
  - Each nibble encodes RGBI: 1 bit each for Red, Green, Blue, and Intensity.
  - Combined value is referred to as `ColourV`.
- **Analog mode (RRRGGGBB, 256 colours):**
  - Foreground uses all 8 bits: R (3), G (3), B (2) → RRRGGGBB.
  - Background/border planned as global registers (not per cell); analog mode ignores per-cell BG in the combination step.
  - Palette selection controls how PETSCII colour codes map to RRRGGGBB.

## Configuration
- File: `config-custom/!colourpet40.asm`
  - `COLOURPET`: enable colour features.
  - `COLOURMODE`: `0`=Digital, `1`=Analog.
  - `COLOURPAL`: default analog palette (`0`=C128/RGBI, `1`=C64); can be changed at runtime via `POKE208,n` (`ColourPNum`).
  - `DEFAULTFG`, `DEFAULTBG`, `DEFAULTBO`: defaults set by init.
  - `COLOURVER`: selects colour RAM base (`$8400` beta vs `$8800` normal/uPET).

## Memory Layout and Variables
- File: `memzeropage.asm` (Zero Page and colour RAM base)
  - Colour RAM base (`COLOUR_RAM`):
    - `COLOURVER=0`: `$8400` (beta; colour tables shifted)
    - `COLOURVER=1`: `$8800` (normal/uPET; used by VICE)
  - Zero page variables:
    - `ColourFG=$BB`, `ColourBG=$BC`, `ColourV=$D7`, `ColourCount=$D6`
    - `ColourPNum=$D0` (analog palette number)
    - Colour RAM pointers: `ColourPtr=$CE/$CF`, `ColourPtr2=$C2/$C3`, `ColourSAL=$DD/$DE`
  - Hidden/custom palette storage start: `ColourStor = COLOUR_RAM + 25 * COLUMNS`

## PETSCII Colour Codes
- File: `colourpet.asm` (`COLOURS` table and handler)
  - PETSCII codes are intercepted by `CheckColourCodes`:
    - First colour code sets foreground (`ColourFG`).
    - Second code sets background (`ColourBG`).
    - Third code is reserved for border (future hardware).
    - Quote mode (`QuoteMode`) disables processing.
  - Notes: Some PETSCII colour codes conflict with PET editor control keys (e.g., `$96` ERASE END, `$99` SCROLL UP, `$95` INSERT LINE).

## Combining Colours (SetColourValue)
- File: `colourpet.asm`
  - Digital mode (`COLOURMODE=0`):
    - `ColourBG` shifted left 4 times to upper nibble.
    - Added to `ColourFG` to form `ColourV` (RGBIRGBI).
    - Result stored to `ColourV` (`STA ColourV`).
  - Analog mode (`COLOURMODE=1`):
    - `ColourFG` used as index into conversion table based on `ColourPNum`:
      - `0`: `C128COLOURS` (RGBI-to-RRRGGGBB)
      - `1`: `C64COLOURS` (C64 NTSC/PAL → RRRGGGBB)
      - `2`: `ColourStor` (custom palette residing in hidden colour RAM)
    - Fetched RRRGGGBB value stored in `ColourV`.

## Conversion Tables (Analog Mode)
- File: `colourpet.asm`
  - `C128COLOURS`: native RGBI mapping (same as C128 VDC) to RRRGGGBB.
  - `C64COLOURS`: approximations of C64 NTSC/PAL colours to RRRGGGBB.
  - Indexing uses PETSCII colour code positions defined in `COLOURS`.

## Rendering and Screen Operations
- File: `editrom40.asm`
  - Colour-aware output:
    - `ColourPET_PutChar_at_Cursor`: writes both character and `ColourV` to screen and colour RAM.
    - Routines like `ColourPET_Scroll_Left`, `ColourPET_Erase_To_EOL`: maintain colour RAM in sync during scroll/erase.
  - Pointer sync: `ColourPET_SyncPointers` aligns colour RAM pointers with screen pointers.

## Emulator/Hardware Notes
- VICE supports ColourPET in 40/80 columns, digital and analog (RRRGGGBB) modes.
- Analog background/border are planned as registers; per-cell BG is ignored in analog `SetColourValue`.

## Quick References
- Configuration: `config-custom/!colourpet40.asm`
- PETSCII colours + handler: `colourpet.asm` (`COLOURS`, `CheckColourCodes`)
- Colour combination logic: `colourpet.asm` (`SetColourValue`)
- Analog conversion tables: `colourpet.asm` (`C128COLOURS`, `C64COLOURS`)
- Colour RAM base and ZP: `memzeropage.asm`
- Screen ops using colour: `editrom40.asm`
- Shifted colour line tables (beta): `screen1cshift.asm`
- Normal colour line tables (normal/uPET): `screen2c.asm`

## Integration Hint (EconoPET Firmware)
- Digital RGBI: decode `ColourV` nibbles per cell; FG in low nibble, BG in high nibble.
- Analog RRRGGGBB: treat `ColourV` byte as packed 3/3/2 bits; map to your display pipeline. BG/border likely global.
- Honour `COLOURVER` base when reading colour RAM (`$8400` vs `$8800`).