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

| Variable/Address | Location/Value | Description |
|------------------|----------------|-------------|
| `COLOUR_RAM`     | `$8400`        | Colour RAM base if `COLOURVER=0` (beta) |
| `COLOUR_RAM`     | `$8800`        | Colour RAM base if `COLOURVER=1` (normal/uPET/VICE) |
| `ColourFG`       | `$BB`          | Zero page: Current foreground colour |
| `ColourBG`       | `$BC`          | Zero page: Current background colour |
| `ColourV`        | `$D7`          | Zero page: Combined colour byte to write to RAM |
| `ColourCount`    | `$D6`          | Zero page: Counter for colour codes |
| `ColourPNum`     | `$D0`          | Zero page: Analog palette selection (0-2) |
| `COLOURSTOR`     | `COLOUR_RAM` + (25 * Cols) | Start of hidden custom palette storage |
| **Storage Area** | | *(Defined in memcpet.asm)* |
| `COLOURSTOR`     | `COLOURSTOR` + 9 | Base of hidden variables (Offset +9) |
| `COLOURV`        | `COLOURSTOR` + 1 | Combined FG and BG value |
| `COLOURFG`       | `COLOURSTOR` + 2 | Foreground Colour |
| `COLOURBG`       | `COLOURSTOR` + 3 | Background Colour |
| `COLOURBORDER`   | `COLOURSTOR` + 4 | Border Colour |
| `COLOURCOUNT`    | `COLOURSTOR` + 5 | Count to track colour change codes |
| `COLOURREGBG`    | `COLOURSTOR` + 6 | Colour Background Register (dummy) |
| `COLOURREGBORDER`| `COLOURSTOR` + 7 | Colour Border Register (dummy) |
| `COLOURREGMODE`  | `COLOURSTOR` + 8 | Colour Mode Register (Future) |

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

## Custom Palette (Analog Mode)
- Files: `colourpet.asm`, `memzeropage.asm`

In Analog mode (`COLOURMODE=1`), a third palette option allows user-defined colours stored in "hidden" colour RAM—the portion of colour memory beyond the visible 25×COLUMNS screen area.

### Palette Selection
| `ColourPNum` | Palette | Source |
|--------------|---------|--------|
| 0 | C128/RGBI | `C128COLOURS` table in ROM |
| 1 | C64 | `C64COLOURS` table in ROM |
| 2 | **Custom** | `ColourStor` in hidden colour RAM |

- Select palette at runtime: `POKE 208, n` (where `n` = 0, 1, or 2)

### Custom Palette Memory Location
The custom palette is stored immediately after the visible screen area in colour RAM:

```
ColourStor = COLOUR_RAM + 25 * COLUMNS
```

| COLOURVER | COLOUR_RAM | ColourStor (40-col) | ColourStor (80-col) |
|-----------|------------|---------------------|---------------------|
| 0 (beta)  | `$8400`    | `$87E8` (34792)     | `$8BD0` (35792)     |
| 1 (normal)| `$8800`    | `$8BE8` (35816)     | `$8FD0` (36816)     |

### Custom Palette Format
The custom palette occupies **16 consecutive bytes** starting at `ColourStor`, one byte per colour index (0–15). Each byte is in **RRRGGGBB** format (RGB332):

| Bits | Component | Levels |
|------|-----------|--------|
| 7:5  | Red       | 8      |
| 4:2  | Green     | 8      |
| 1:0  | Blue      | 4      |

### BASIC Examples
```basic
REM Select custom palette
POKE 208, 2

REM Set colour 0 (black) - already 0
POKE 34792, 0

REM Set colour 15 (white)
POKE 34792 + 15, 255

REM Set colour 5 to bright green (R=0, G=7, B=0)
REM Binary: 00011100 = 28
POKE 34792 + 5, 28

REM Set colour 8 to bright red (R=7, G=0, B=0)
REM Binary: 11100000 = 224
POKE 34792 + 8, 224

REM Set colour 2 to bright blue (R=0, G=0, B=3)
REM Binary: 00000011 = 3
POKE 34792 + 2, 3
```

Note: The address `34792` assumes 40-column mode with `COLOURVER=0`. Adjust for your configuration using the table above.

### How It Works
When `SetColourValue` is called with `ColourPNum=2`, it reads directly from hidden colour RAM instead of the ROM tables:

```asm
LDA ColourPNum        ; Current Palette
CMP #2                ; Custom Palette?
BNE SCV_Set           ; No, use default
LDA ColourStor,X      ; Yes, read from hidden colour RAM
JMP SCV_exit
```

This allows runtime palette modification without ROM changes.

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
