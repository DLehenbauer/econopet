# Virtual Tape Drive

This document describes how the MCU emulates a tape drive to load PRG files
from the SD card when the user types `LOAD`. Rather than emulating the tape
hardware signals, the MCU intercepts the KERNAL's LOAD dispatch, copies the
program directly into SRAM, and redirects execution to a stub that prints a
LOADING message and returns to the BASIC ready prompt.

## Overview

1. User types `LOAD "PROGRAM"` (or `LOAD "P*"` for wildcard prefix match)
2. BASIC calls the KERNAL LOAD routine, which parses the filename and device
   number into zero page variables (FNLEN, FNADR, FA)
3. MCU breakpoint fires just before the KERNAL dispatches to the
   device-specific load path (at `JSR LD15` for ROM 2/4, `LDA FA` for ROM 1)
4. MCU reads the device number. If not a tape device (1 or 2), the MCU
   resumes the KERNAL so it can handle disk/IEEE LOADs normally
5. MCU reads the filename from PET memory. If empty or just `*`, the MCU
   resumes the KERNAL (fall through to physical datasette)
6. MCU searches `/sd/prgs/` for a matching `.prg` file
7. If found:
   - MCU copies the PRG data into SRAM at the load address from the PRG file
   - MCU sets EAL/EAH to the end address
   - MCU saves the existing tape buffer contents (for later restoration)
   - MCU writes a stub to the tape buffer that prints
     `FOUND "/SD/PRGS/FILENAME.PRG"` then jumps to the KERNAL's LD210
     routine, which prints "READY.", sets VARTAB, relinks BASIC lines,
     clears variables, and enters the main loop
   - When the stub's `JMP LD210` fires a second breakpoint at LD210, the
     MCU restores the original tape buffer contents before resuming
8. If not found:
   - MCU resumes the KERNAL, which enters the tape path normally (prints
     "PRESS PLAY ON TAPE" and polls the sense pin)

## Background

When the user types `LOAD`, the KERNAL displays "PRESS PLAY ON TAPE" and
enters a tight loop polling PIA1 Port A bit 4 (the cassette sense pin),
waiting for it to go low. See [tape.md](PET/tape.md) for full details.

## Detection Strategy

The firmware uses its breakpoint subsystem (see [breakpoint.h](../../../fw/src/breakpoint.h))
to intercept the KERNAL's LOAD dispatch before it enters the device-specific
code path.

### LOAD Dispatch (ROM 4)

The KERNAL's LOAD routine parses the filename and device number into zero
page variables via PARS1, then dispatches to the appropriate device handler
via `JSR LD15`:

```
LOAD   F401  LDA #0
       F403  STA VERCK            ; set load (not verify)
LD10   F405  JSR PARS1            ; parse FNLEN, FNADR, FA, SA
LOADNP F408  JSR SV60             ; set STAL/STAH/EAL/EAH
       F40B  LDA #$FF
LD11   F40D  CMP STKEY            ; wait for all keys released
       F40F  BNE LD11
       F411  CMP STKEY
       F413  BNE LD11
  -->  F415  JSR LD15             ; <--- breakpoint here
```

At `$F415`, PARS1 and SV60 have both returned. FNLEN, FNADR, and FA (device
number) are fully initialized in zero page, and there are no stale KERNAL
subroutine frames on the stack. This is essential for program-mode LOADs
(e.g., a BASIC program that LOADs itself), because LD210's program-mode path
uses STKINI to pop the top return address from the stack and reset the stack
pointer. If stale KERNAL frames were present, STKINI would pop the wrong
address.

ROM 1.0 uses inline device dispatch instead of `JSR LD15`. The equivalent
clean breakpoint is `LDA FA` at `$F362`, just before the `BCC LD100` branch
to the tape path.

Since the breakpoint fires for all LOADs (tape, disk, IEEE), the callback
checks the device number first and immediately resumes if it is not a tape
device (1 or 2). This costs a single SPI read for non-tape LOADs.

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
| $C9     | EAL    | End address low (end of loaded program)  |
| $CA     | EAH    | End address high                         |
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
| $F42E   | LD210   | Post-load fixup (see below)              |

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

### Step 4: Update End-of-Load Pointer

Set EAL/EAH to point just past the end of the loaded program. The KERNAL's
LD210 routine copies these into VARTAB before calling FINI (which relinks
BASIC lines and clears variables). The EAL/EAH ZP addresses come from the
config blob:

```c
spi_write_at(cfg.eal, end_addr & 0xFF);
spi_write_at(cfg.eah, end_addr >> 8);
```

### Step 5: Write Stub to Tape Buffer

The MCU writes a small 6502 program to the tape buffer ($027A) that:

1. Prints a message with the SD card path using CHROUT ($FFD2)
2. Jumps to the KERNAL's LD210 routine

Before overwriting the tape buffer, the MCU saves its current contents
(192 bytes) in a local buffer. These are restored when the stub reaches
LD210 (see [Step 7](#step-7-restore-tape-buffer)).

CHROUT ($FFD2) is in the KERNAL jump table and stable across all ROM
versions. The LD210 address comes from the config blob.

The LD210 routine is the KERNAL's own post-tape-load fixup path. It:

1. Prints "READY." (from the KERNAL message table)
2. Copies EAL/EAH into VARTAB
3. Calls FINI, which calls RUNC (reset TXTPTR, clear variables), then
   falls through into LNKPRG (rechain BASIC line pointers), which ends
   by jumping to the BASIC main input loop

This reuses the exact same code path the KERNAL uses after a real tape load,
so all ROM versions (including ROM 1.0 where LNKPRG ends with `JMP MAIN`
instead of `RTS`) work correctly.

#### Tape Buffer Layout

The MCU constructs the following program at runtime (ROM 4 shown):

```
Offset  Addr   Hex          Asm
------  -----  -----------  ----------------------------------
 0      027A   A2 00        LDX #$00
 2      027C   BD 8A 02     LDA $028A,X      ; message address
 5      027F   F0 06        BEQ $0287        ; null terminator
 7      0281   20 D2 FF     JSR $FFD2        ; CHROUT
10      0284   E8           INX
11      0285   D0 F5        BNE $027C        ; loop
13      0287   4C 2E F4     JMP $F42E        ; LD210
16      028A   ...          message string (null-terminated)
```

Bytes 0-12 are a fixed print loop. The `LDA` operand at offset 3-4 is
computed as `TAPE_BUFFER + 16` (print loop size + JMP size). Bytes 13-15
are `JMP ld210` built at runtime from `cfg.ld210`. The message string
starts at byte 16, for example:

```
FOUND /PRGS/GAME.PRG\rLOADING
```

The `\r` ($0D, carriage return) moves the cursor to the next line. LD210
then prints "READY." on the following line. The path is converted to
uppercase for the PET's default character set. The tape buffer is 192
bytes, leaving 176 bytes for the message (more than enough for any path).

### Step 6: Resume Execution

The breakpoint callback returns the stub address ($027A) as the resume
target. The breakpoint system writes a JMP instruction to redirect execution.

### Step 7: Restore Tape Buffer

The `tape_load_callback` arms a one-shot breakpoint at LD210 (`cfg.ld210`).
When the stub's `JMP LD210` reaches this breakpoint, the MCU restores the
saved tape buffer contents, removes the breakpoint, and resumes LD210
normally. This ensures the tape buffer is left in its original state after
the virtual load completes.

Because the breakpoint only exists while a virtual load is in progress,
physical datasette loads never trigger the callback.

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
#define JMP_SIZE         3

static void tape_build_stub(uint8_t* buf, const char* path) {
    // Print loop (13 bytes)
    memcpy(buf, print_loop, PRINT_LOOP_SIZE);

    // Patch LDA operand with message address
    uint16_t msg_addr = TAPE_BUFFER + PRINT_LOOP_SIZE + JMP_SIZE;
    buf[3] = msg_addr & 0xFF;
    buf[4] = msg_addr >> 8;

    // JMP ld210 (3 bytes)
    buf[PRINT_LOOP_SIZE]     = 0x4C;    // JMP
    buf[PRINT_LOOP_SIZE + 1] = state.cfg.ld210 & 0xFF;
    buf[PRINT_LOOP_SIZE + 2] = state.cfg.ld210 >> 8;

    // Message string
    uint8_t off = PRINT_LOOP_SIZE + JMP_SIZE;
    const char prefix[] = "FOUND ";
    memcpy(buf + off, prefix, sizeof(prefix) - 1);
    off += sizeof(prefix) - 1;
    for (size_t i = 0; path[i]; i++)
        buf[off++] = toupper((unsigned char)path[i]);
    buf[off++] = '\r';      // CR (newline on PET)
    static const char loading[] = "LOADING";
    memcpy(buf + off, loading, sizeof(loading) - 1);
    off += sizeof(loading) - 1;
    buf[off++] = '\0';      // Null terminator
}

static bp_result_t tape_load_callback(uint16_t pc, void* context) {
    // ... check device number, read filename, search SD card ...

    if (load_successful) {
        // Set EAL/EAH so LD210 can update VARTAB
        spi_write_at(state.cfg.eal, end_addr & 0xFF);
        spi_write_at(state.cfg.eah, end_addr >> 8);

        // Save tape buffer before overwriting with stub
        spi_read(TAPE_BUFFER, TAPE_BUFFER_CAPACITY, state.saved_buf);

        // Arm a one-shot breakpoint at LD210 to restore the tape buffer
        bp_set(state.cfg.ld210, tape_ld210_callback, NULL);

        uint8_t buf[192];
        tape_build_stub(buf, path);     // e.g. "/prgs/game.prg"
        uint8_t total = PRINT_LOOP_SIZE + JMP_SIZE + msg_len;
        spi_write(TAPE_BUFFER, buf, total);
        return (bp_result_t){ .pc = TAPE_BUFFER, .rearm = true };
    }

    return (bp_result_t){ .pc = pc, .rearm = true };
}

// One-shot breakpoint at LD210: restore tape buffer after virtual load.
static bp_result_t tape_ld210_callback(uint16_t pc, void* context) {
    spi_write(TAPE_BUFFER, state.saved_buf, TAPE_BUFFER_CAPACITY);
    return (bp_result_t){ .pc = pc, .rearm = false };
}
```

The callback returns a `bp_result_t` with the resume address and a `rearm`
flag. Setting `rearm = false` tells the breakpoint system to remove the
breakpoint after resuming (one-shot behavior).

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
directly to a hex string in `config.yaml`. The struct contains the LD210
address (where the stub jumps after printing), the breakpoint address, and
the zero page locations the MCU reads/writes. At runtime, the MCU builds a
print loop, appends a `JMP ld210`, and writes the message (see
[Step 5](#step-5-write-stub-to-tape-buffer)):

```c
typedef struct __attribute__((packed)) {
    // KERNAL post-load fixup address (2 bytes, little-endian).
    // The stub jumps here after printing FOUND/LOADING. LD210 prints
    // "READY.", copies EAL/EAH into VARTAB, relinks BASIC lines,
    // clears variables, and enters the main loop.
    uint16_t ld210;

    // Breakpoint address (2 bytes, little-endian).
    // Set at the LOAD dispatch point: JSR LD15 (ROM 2/4) or LDA FA
    // (ROM 1). FNLEN/FNADR/FA are set up and the stack is clean.
    uint16_t bp_addr;

    // Zero page locations (5 bytes)
    // These vary by ROM version. The MCU reads/writes them to access
    // the filename, end address, and device number in PET memory.
    uint8_t eal;        // ZP addr of EAL (end address low)
    uint8_t eah;        // ZP addr of EAH (end address high)
    uint8_t fnlen;      // ZP addr of FNLEN (filename length)
    uint8_t devnum;     // ZP addr of DEVNUM (device number)
    uint8_t fnadr;      // ZP addr of FNADR (filename pointer)
} tape_config_t;        // 9 bytes total
```

The tape buffer address ($027A) is the same across all ROMs and is a
compile-time constant:

```c
#define TAPE_BUFFER 0x027A
```

#### ROM 4 Example

```
Offset  Bytes     Field    Value    Meaning
------  --------  -------  -------  ---------------------------
 0..1   2E F4     ld210    $F42E    KERNAL post-load fixup
 2..3   15 F4     bp_addr  $F415    JSR LD15 (LOAD dispatch)
 4      C9        eal      $C9      ZP: End address low
 5      CA        eah      $CA      ZP: End address high
 6      D1        fnlen    $D1      ZP: Filename length
 7      D4        devnum   $D4      ZP: Device number
 8      DA        fnadr    $DA      ZP: Filename pointer
```

`config.yaml` hex string: `"2ef415f4c9cad1d4da"`

#### ROM 1 Example

```
Offset  Bytes     Field    Value    Meaning
------  --------  -------  -------  ---------------------------
 0..1   E5 F3     ld210    $F3E5    KERNAL post-load fixup
 2..3   62 F3     bp_addr  $F362    LDA FA (inline dispatch)
 4      E5        eal      $E5      ZP: End address low
 5      E6        eah      $E6      ZP: End address high
 6      EE        fnlen    $EE      ZP: Filename length
 7      F1        devnum   $F1      ZP: Device number
 8      F9        fnadr    $F9      ZP: Filename pointer
```

`config.yaml` hex string: `"e5f362f3e5e6eef1f9"`

### State Structure

```c
typedef struct {
    tape_config_t cfg;      // Parsed from config.yaml hex blob
    bool enabled;           // Virtual tape enabled (config had 'tape' key)
    uint8_t saved_buf[192]; // Original tape buffer contents
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
routines, and the KERNAL LOAD dispatch point) moves between ROM versions.
The PET KERNAL jump table at `$FFC0`+ is stable across all versions, but it
only covers I/O vectors (OPEN, CLOSE, LOAD, SAVE, etc.). The routines we
need (LD210, the LOAD dispatch) are internal routines with no stable vectors.

### Address Table

All addresses below are derived from [memorymap-gen.csv](PET/memorymap-gen.csv):

| Symbol    | ROM 1.0 | ROM 2.0 | ROM 4.0 | Description                      |
|-----------|---------|---------|---------|----------------------------------|
| TXTTAB    | $7A     | $28     | $28     | ZP: Start of BASIC program       |
| VARTAB    | $7C     | $2A     | $2A     | ZP: Start of variables           |
| EAL       | $E5     | $C9     | $C9     | ZP: End address low              |
| EAH       | $E6     | $CA     | $CA     | ZP: End address high             |
| FNLEN     | $EE     | $D1     | $D1     | ZP: Filename length               |
| DEVNUM    | $F1     | $D4     | $D4     | ZP: Device number                 |
| FNADR     | $F9     | $DA     | $DA     | ZP: Filename pointer              |
| READY     | $C38B   | $C389   | $B3FF   | BASIC warm start (READY prompt)   |
| LINKPRG   | $C433   | $C442   | $B4B6   | Rechain BASIC line pointers       |
| RSTXCLR   | $C567   | $C572   | $B5E9   | Reset TXTPTR and CLR              |
| LD210     | $F3E5   | $F3EF   | $F42E   | Post-load fixup (prints READY, sets VARTAB) |
| CSTE1     | $F83B   | $F812   | $F857   | Press-play routine entry           |
| bp_addr   | $F362   | $F3D6   | $F415   | Breakpoint: LOAD dispatch (see above) |

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
        tape: "2ef415f4c9cad1d4da"
```

Pre-computed blobs for each ROM version:

| ROM Version | Hex Blob (9 bytes)               |
|-------------|----------------------------------|
| ROM 4.0     | `2ef415f4c9cad1d4da`           |
| ROM 2.0     | `eff3d6f3c9cad1d4da`           |
| ROM 1.0     | `e5f362f3e5e6eef1f9`           |

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
