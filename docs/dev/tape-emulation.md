# Virtual Tape Drive

This document describes how the MCU emulates a tape drive to load PRG files
from the SD card when the user types `LOAD`. Rather than emulating the tape
hardware signals, the MCU intercepts the KERNAL before it prints "PRESS PLAY
ON TAPE", copies the program directly into SRAM, and redirects execution to
a stub that prints a LOADING message and returns to the BASIC ready prompt.

## Overview

1. User types `LOAD "PROGRAM"` (or `LOAD "P*"` for wildcard prefix match)
2. KERNAL enters the tape load path and calls the CSTE1 routine
3. MCU breakpoint fires at the CSTE1 entry, before any message is printed
4. MCU reads the requested filename from PET memory
5. If the filename is empty or just `*`, the MCU resumes the KERNAL
   (fall through to physical datasette)
6. MCU searches `/sd/prgs/` for a matching `.prg` file
7. If found:
   - MCU copies the PRG data into SRAM at the load address from the PRG file
   - MCU sets VARTAB to the end address
   - MCU writes a stub to the tape buffer that prints
     `LOADING "/SD/PRGS/FILENAME.PRG"`, relinks BASIC lines, and returns
     to the READY prompt
8. If not found:
   - MCU resumes the KERNAL, which prints "PRESS PLAY ON TAPE" and polls
     the sense pin normally

## Background

When the user types `LOAD`, the KERNAL displays "PRESS PLAY ON TAPE" and
enters a tight loop polling PIA1 Port A bit 4 (the cassette sense pin),
waiting for it to go low. See [tape.md](PET/tape.md) for full details.

## Detection Strategy

The firmware uses its breakpoint subsystem (see [breakpoint.h](../../../fw/src/breakpoint.h))
to intercept the KERNAL before it prints "PRESS PLAY ON TAPE".

### CSTE1 Routine (ROM 4)

The KERNAL's CSTE1 routine at `$F857` checks the tape sense pin, prints the
"PRESS PLAY ON TAPE" prompt if the key is not pressed, and polls in a loop:

```
F857  CSTE1    JSR $F87A        ; Check Tape Status (test sense pin)
F85A           BEQ $F88B        ; Already pressed, return
F85C           LDY #$41
F85E           JSR $F185        ; Print "PRESS PLAY"
F861           LDY #$56
F863           JSR $F185        ; Print "ON TAPE "
F866           LDA $D4          ; Device number
F868           ORA #$30         ; Convert to ASCII digit
F86A           JSR $E202        ; Print device number
F86D           JSR $F935        ; Check for STOP key
F870           JSR $F87A        ; Check Tape Status
F873           BNE $F86D        ; Loop until sense pin asserted
F875           LDY #$AA
F877           JMP $F185        ; Print CR and return
```

Setting a breakpoint at `$F857` (the routine entry) halts the CPU before any
message is printed. If the MCU finds a matching file on the SD card, it loads
the program and redirects execution to a stub in the tape buffer. If no match
is found, the MCU resumes the original instruction and the KERNAL proceeds
normally.

## PRG File Format

A `.prg` file is the standard Commodore program format:

| Offset | Size | Description                              |
|--------|------|------------------------------------------|
| 0      | 2    | Load address (little-endian)             |
| 2      | n    | Program data (BASIC tokens or ML code)   |

For BASIC programs, the load address is typically `$0401` (start of BASIC
program area). The program data consists of linked BASIC lines.

### BASIC Line Format

Each BASIC line has:

| Offset | Size | Description                              |
|--------|------|------------------------------------------|
| 0      | 2    | Pointer to next line (or $0000 for end)  |
| 2      | 2    | Line number                              |
| 4      | n    | Tokenized BASIC text                     |
| 4+n    | 1    | Zero terminator ($00)                    |

The "next line" pointers form a linked list. These pointers are absolute
addresses that depend on where the program is loaded in memory.

## Memory Locations

### Zero Page Variables (ROM 4)

| Address | Name   | Description                              |
|---------|--------|------------------------------------------|
| $28-29  | TXTTAB | Pointer: Start of BASIC program          |
| $2A-2B  | VARTAB | Pointer: Start of BASIC variables        |
| $D1     | FNLEN  | Length of current filename (0-16)        |
| $D4     | DEVNUM | Current device number (1=Cass#1, 2=Cass#2) |
| $DA-DB  | FNADR  | Pointer to filename string               |

### BASIC/KERNAL Entry Points (ROM 4)

| Address | Name    | Description                              |
|---------|---------|------------------------------------------|
| $B3FF   | READY   | BASIC warm start (prints "READY" prompt) |
| $B4B6   | LINKPRG | Rechain BASIC lines (fix link pointers)  |
| $B5E9   | RSTXCLR | Reset TXTPTR and perform CLR             |
| $B5F0   | CLR     | Clear variables, reset stack             |

## Load Sequence

When the breakpoint fires and a matching PRG file is found:

### Step 1: Read Filename from PET Memory

```c
uint8_t fnlen = spi_read_at(FNLEN);      // $D1
uint16_t fnadr = spi_read16_at(FNADR);   // $DA-DB
char filename[17];
spi_read(fnadr, filename, fnlen);
filename[fnlen] = '\0';
```

### Step 2: Search SD Card

Search `/sd/prgs/` for a file matching the requested name. Support the `*`
wildcard for prefix matching:

- `LOAD "*"` or `LOAD ""` - fall through to physical datasette
- `LOAD "GAME*"` - load first file starting with "game"
- `LOAD "ADVENTURE"` - load exact match "adventure.prg"

Filename matching is case-insensitive. PETSCII is converted to ASCII for
filesystem operations.

### Step 3: Copy PRG to SRAM

```c
// Read PRG header (2-byte load address)
uint16_t load_addr;
f_read(&file, &load_addr, 2, &bytes_read);

// Use the load address from the PRG file (typically $0401 for BASIC).
// This matches KERNAL behavior: the tape header's start address (not
// TXTTAB) determines where data is loaded.
uint16_t dest = load_addr;

// Copy program data to SRAM
while (f_read(&file, buffer, sizeof(buffer), &bytes_read) == FR_OK 
       && bytes_read > 0) {
    spi_write(dest, buffer, bytes_read);
    dest += bytes_read;
}

uint16_t end_addr = dest;
```

### Step 4: Update BASIC Pointers

Set VARTAB to point just past the end of the program. LINKPRG does not
update VARTAB, but CLR (called by RSTXCLR) reads it, so the MCU must
write it before the stub runs. The VARTAB ZP address comes from the
config blob:

```c
spi_write16_at(cfg.vartab, end_addr);
```

### Step 5: Write Stub to Tape Buffer

The MCU writes a small 6502 program to the tape buffer ($027A) that:

1. Prints a LOADING message with the full SD card path using CHROUT ($FFD2)
2. Calls `LINKPRG` to rechain BASIC line pointers
3. Calls `RSTXCLR` to reset TXTPTR and clear variables
4. Jumps to `READY` to print the prompt and enter the main loop

CHROUT ($FFD2) is in the KERNAL jump table and stable across all ROM
versions. The LINKPRG, RSTXCLR, and READY addresses come from the config
blob's `stub[9]`, which has them baked into the opcodes.

#### Tape Buffer Layout

The MCU constructs the following program at runtime (ROM 4 shown):

```
Offset  Addr   Hex          Asm
------  -----  -----------  ----------------------------------
 0      027A   A2 00        LDX #$00
 2      027C   BD 90 02     LDA $0290,X      ; message address
 5      027F   F0 06        BEQ $0287        ; null terminator
 7      0281   20 D2 FF     JSR $FFD2        ; CHROUT
10      0284   E8           INX
11      0285   D0 F5        BNE $027C        ; loop
13      0287   20 B6 B4     JSR $B4B6        ; LINKPRG
16      028A   20 E9 B5     JSR $B5E9        ; RSTXCLR
19      028D   4C FF B3     JMP $B3FF        ; READY
22      0290   ...          message string (null-terminated)
```

Bytes 0-12 are a fixed print loop. The `LDA` operand at offset 3-4 is
computed as `TAPE_BUFFER + 22` (print loop size + stub size). Bytes 13-21
are copied from `cfg.stub[9]`. The message string starts at byte 22,
for example:

```
LOADING "/SD/PRGS/GAME.PRG"\r
```

The trailing $0D (carriage return) moves the cursor to the next line before
READY prints its prompt. The path is converted to uppercase for the PET's
default character set. The tape buffer is 192 bytes, leaving 170 bytes for
the message (more than enough for any path).

### Step 6: Resume Execution

The breakpoint callback returns the stub address ($027A) as the resume
target. The breakpoint system writes a JMP instruction to redirect execution:

```c
// Fixed print loop: LDX #0 / LDA msg,X / BEQ done / JSR CHROUT / INX / BNE loop
static const uint8_t print_loop[] = {
    0xA2, 0x00,                     // LDX #$00
    0xBD, 0x00, 0x00,               // LDA $xxxx,X  (patched below)
    0xF0, 0x06,                     // BEQ done
    0x20, 0xD2, 0xFF,               // JSR $FFD2 (CHROUT)
    0xE8,                           // INX
    0xD0, 0xF5                      // BNE loop
};

#define PRINT_LOOP_SIZE  sizeof(print_loop)  // 13
#define STUB_SIZE        9

static void tape_build_stub(uint8_t* buf, const char* path) {
    // Print loop (13 bytes)
    memcpy(buf, print_loop, PRINT_LOOP_SIZE);

    // Patch LDA operand with message address
    uint16_t msg_addr = TAPE_BUFFER + PRINT_LOOP_SIZE + STUB_SIZE;
    buf[3] = msg_addr & 0xFF;
    buf[4] = msg_addr >> 8;

    // Fixup code from config blob (9 bytes)
    memcpy(buf + PRINT_LOOP_SIZE, state.cfg.stub, STUB_SIZE);

    // Message string
    uint8_t off = PRINT_LOOP_SIZE + STUB_SIZE;
    const char prefix[] = "LOADING \"";
    memcpy(buf + off, prefix, sizeof(prefix) - 1);
    off += sizeof(prefix) - 1;
    for (size_t i = 0; path[i]; i++)
        buf[off++] = toupper((unsigned char)path[i]);
    buf[off++] = '"';
    buf[off++] = '\r';      // CR (newline on PET)
    buf[off++] = '\0';      // Null terminator
}

static uint16_t tape_cste1_callback(uint16_t addr, void* context) {
    // ... read filename, search SD card, copy PRG to SRAM ...

    if (load_successful) {
        uint8_t buf[192];
        tape_build_stub(buf, path);     // e.g. "/sd/prgs/game.prg"
        uint8_t total = PRINT_LOOP_SIZE + STUB_SIZE + msg_len;
        spi_write(TAPE_BUFFER, buf, total);
        return TAPE_BUFFER;
    }

    return 0;  // Resume CSTE1: KERNAL handles physical tape
}
```

Returning `0` resumes at the breakpoint address with the original instruction
restored, so the KERNAL proceeds normally (printing "PRESS PLAY ON TAPE" and
polling the sense pin).

## Wildcard Matching

The `*` wildcard matches zero or more characters at the end of the pattern.
However, `LOAD ""` and `LOAD "*"` fall through to the physical datasette
since the user did not specify a particular file:

| User Types      | Behavior                         |
|-----------------|----------------------------------|
| `LOAD ""`       | Physical datasette (no file specified) |
| `LOAD "*"`      | Physical datasette (no file specified) |
| `LOAD "A*"`     | First file starting with "a"     |
| `LOAD "GAME"`   | Exactly "game.prg"               |
| `LOAD "GAME*"`  | "game.prg", "game2.prg", etc.    |

Implementation:

```c
bool match_filename(const char* pattern, uint8_t pattern_len, 
                    const char* filename) {
    // Empty pattern or "*" alone: fall through to physical tape
    if (pattern_len == 0) return false;
    if (pattern_len == 1 && pattern[0] == '*') return false;
    
    // Check for trailing wildcard
    bool has_wildcard = (pattern[pattern_len - 1] == '*');
    uint8_t match_len = has_wildcard ? pattern_len - 1 : pattern_len;
    
    // Compare prefix (case-insensitive, PETSCII to ASCII)
    for (uint8_t i = 0; i < match_len; i++) {
        char p = petscii_to_ascii(pattern[i]);
        char f = tolower(filename[i]);
        if (p != f) return false;
    }
    
    // If no wildcard, filename must match exactly (minus .prg extension)
    if (!has_wildcard) {
        // filename should be "pattern.prg" or just end here
        return (filename[match_len] == '.' || filename[match_len] == '\0');
    }
    
    return true;
}
```

## Gateware Changes

**None required.** This approach reuses the existing breakpoint infrastructure
which relies on the FPGA's halt mechanism (triggered by the `STP` opcode).

## Firmware Implementation Summary

### New Files

- `fw/src/tape.h` - Public interface
- `fw/src/tape.c` - Implementation

### Tape Configuration Blob

All ROM-specific addresses are encoded as a single packed struct that maps
directly to a hex string in `config.yaml`. The first 9 bytes are the 6502
fixup sequence (with LINKPRG, RSTXCLR, and READY addresses baked into the
opcodes), followed by the breakpoint address and zero page locations. At
runtime, the MCU prepends a print loop and appends the LOADING message to
build the complete tape buffer program (see [Step 5](#step-5-write-stub-to-tape-buffer)):

```c
typedef struct __attribute__((packed)) {
    // 6502 fixup code (9 bytes) - embedded in the tape buffer stub.
    // Contains: JSR linkprg; JSR rstxclr; JMP ready
    // The BASIC/KERNAL addresses are embedded in the opcodes.
    uint8_t stub[9];

    // Breakpoint address (2 bytes, little-endian)
    // CSTE1 entry: "press play" check + sense polling.
    uint16_t bp_addr;

    // Zero page locations (4 bytes)
    // These vary by ROM version. The MCU reads/writes them to access
    // the filename and BASIC pointers in PET memory.
    uint8_t vartab;     // ZP addr of VARTAB (end of program)
    uint8_t fnlen;      // ZP addr of FNLEN (filename length)
    uint8_t devnum;     // ZP addr of DEVNUM (device number)
    uint8_t fnadr;      // ZP addr of FNADR (filename pointer)
} tape_config_t;        // 15 bytes total
```

The tape buffer address ($027A) is the same across all ROMs and is a
compile-time constant:

```c
#define TAPE_BUFFER 0x027A
```

#### ROM 4 Example

```
Offset  Bytes          Field        Value    Meaning
------  -------------  -----------  -------  ---------------------------
 0..2   20 B6 B4       stub[0..2]            JSR $B4B6 (LINKPRG)
 3..5   20 E9 B5       stub[3..5]            JSR $B5E9 (RSTXCLR)
 6..8   4C FF B3       stub[6..8]            JMP $B3FF (READY)
 9..10  57 F8          bp_addr      $F857    CSTE1 entry
11      2A             vartab       $2A      ZP: Start of variables
12      D1             fnlen        $D1      ZP: Filename length
13      D4             devnum       $D4      ZP: Device number
14      DA             fnadr        $DA      ZP: Filename pointer
```

`config.yaml` hex string: `"20b6b420e9b54cffb357f82ad1d4da"`

### State Structure

```c
typedef struct {
    tape_config_t cfg;      // Parsed from config.yaml hex blob
    bool enabled;           // Virtual tape enabled (config had 'tape' key)
    bool pending;           // Breakpoint fired, waiting to process
} tape_state_t;
```

### Public Interface

```c
// Initialize with config blob from config.yaml. If cfg is NULL, virtual
// tape is disabled and all LOADs go to the physical datasette.
void tape_init(const tape_config_t* cfg);

void tape_task(void);       // Process pending loads (call from main loop)
void tape_deinit(void);     // Remove breakpoint
```

## Testing

### Unit Tests

1. **Filename matching**: Test wildcard patterns against various filenames
2. **PETSCII conversion**: Verify correct ASCII conversion for filesystem ops
3. **PRG parsing**: Test reading load address and program data

### Integration Tests

1. **Basic load**: `LOAD "TEST"` loads `/sd/prgs/test.prg`
2. **Empty name**: `LOAD`, `LOAD ""`, and `LOAD "*"` falls through to physical datasette
3. **Prefix match**: `LOAD "GA*"` loads "game.prg"
4. **No match**: `LOAD "NOEXIST"` falls through to tape polling
5. **BASIC execution**: Loaded program runs correctly (line links valid)
6. **Variables cleared**: No stale data from previous programs

### Manual Testing

1. Copy test PRG files to `/sd/prgs/`
2. Boot PET, type `LOAD "filename"`
3. Verify program loads and `LIST` shows correct line numbers
4. Run program to verify execution

## ROM Compatibility

Every address used by the virtual tape drive (zero page variables, BASIC
routines, and the KERNAL CSTE1 routine) moves between ROM versions. The PET
KERNAL jump table at `$FFC0`+ is stable across all versions, but it only
covers I/O vectors (OPEN, CLOSE, LOAD, SAVE, etc.). The routines we need (
LINKPRG, READY, RSTXCLR) are internal BASIC routines with no stable vectors.

### Address Table

All addresses below are derived from [memorymap-gen.csv](PET/memorymap-gen.csv):

| Symbol    | ROM 1.0 | ROM 2.0 | ROM 4.0 | Description                      |
|-----------|---------|---------|---------|----------------------------------|
| TXTTAB    | $7A     | $28     | $28     | ZP: Start of BASIC program       |
| VARTAB    | $7C     | $2A     | $2A     | ZP: Start of variables (= TXTTAB + 2) |
| FNLEN     | $EE     | $D1     | $D1     | ZP: Filename length               |
| DEVNUM    | $F1     | $D4     | $D4     | ZP: Device number                 |
| FNADR     | $F9     | $DA     | $DA     | ZP: Filename pointer              |
| READY     | $C38B   | $C389   | $B3FF   | BASIC warm start (READY prompt)   |
| LINKPRG   | $C433   | $C442   | $B4B6   | Rechain BASIC line pointers       |
| RSTXCLR   | $C567   | $C572   | $B5E9   | Reset TXTPTR and CLR              |
| CSTE1     | $F83B   | $F812   | $F857   | Press-play routine entry           |

### Configuration via `config.yaml`

Since none of these addresses have stable vectors, the correct values must be
provided per configuration. The `config.yaml` already has a per-config `set`
action. We extend it with a `tape` key whose value is a hex string encoding
the `tape_config_t` blob (see [Firmware Implementation Summary](#firmware-implementation-summary)):

```yaml
configs:
  - name: "PET 40xx (40 Col 60 Hz)"
    setup:
      - action: "load"
        # ... ROM files ...
      - action: "set"
        usb-keymap: "/ukm/us.bin"
        video-ram-kb: 1
        tape: "20b6b420e9b54cffb357f82ad1d4da"
```

Pre-computed blobs for each ROM version:

| ROM Version | Hex Blob (15 bytes)                        |
|-------------|--------------------------------------------|
| ROM 4.0     | `20b6b420e9b54cffb357f82ad1d4da`         |
| ROM 2.0     | `2042c42072c54c89c312f82ad1d4da`         |
| ROM 1.0     | `2033c42067c54c8bc33bf87ceef1f9`         |

When the `tape` key is present, the firmware casts the decoded bytes to
`tape_config_t` and enables the virtual tape drive. When absent, the virtual
tape drive is disabled (all LOADs go to the physical datasette).

This makes ROM compatibility explicit and per-config without requiring
auto-detection. Custom or patched ROMs can supply their own blob.

## Limitations

- Only supports `.prg` files (not `.tap` tape images)
- No VERIFY support (would require different interception point)
- No SAVE support (separate feature)
- Single directory (`/sd/prgs/`) - no subdirectory navigation

## Future Enhancements

- Support subdirectories via special filename syntax
- IEEE-488 device emulation for disk-style commands
- SAVE to SD card
