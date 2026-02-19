// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "tape.h"

#include <ctype.h>
#include <dirent.h>

#include "breakpoint.h"
#include "diag/log/log.h"
#include "driver.h"
#include "global.h"
#include "sd/sd.h"

#define PRGS_DIR "/prgs"

// Fixed 6502 print loop: LDX #0 / LDA msg,X / BEQ done / JSR CHROUT / INX / BNE loop
static const uint8_t print_loop[] = {
    0xA2, 0x00,                     // LDX #$00
    0xBD, 0x00, 0x00,               // LDA $xxxx,X  (operand patched at runtime)
    0xF0, 0x06,                     // BEQ done
    0x20, 0xD2, 0xFF,               // JSR $FFD2 (CHROUT)
    0xE8,                           // INX
    0xD0, 0xF5                      // BNE loop
};

#define PRINT_LOOP_SIZE  sizeof(print_loop)     // 13 bytes
#define JMP_SIZE         3                      // JMP ld210
#define TAPE_BUFFER_CAPACITY 192

typedef struct {
    tape_config_t cfg;      // Parsed from config.yaml hex blob
    bool enabled;           // Virtual tape enabled (config had 'tape' key)
} tape_state_t;

static tape_state_t state;

// Convert a PETSCII character to lowercase ASCII for filesystem matching.
// PETSCII $41-$5A and $C1-$DA are uppercase letters.
static char petscii_to_lower(uint8_t ch) {
    if (ch >= 0xC1 && ch <= 0xDA) {
        ch -= 0x80;     // Shifted PETSCII uppercase -> $41-$5A range
    }
    if (ch >= 0x41 && ch <= 0x5A) {
        return (char)(ch + 0x20);   // Uppercase -> lowercase ASCII
    }
    return (char)ch;
}

// Check if 'pattern' (length pattern_len, already lowercase ASCII) matches
// 'filename' (a directory entry name like "game.prg"). Supports trailing '*'
// wildcard for prefix matching. Returns false for empty pattern or lone "*".
static bool match_filename(const char* pattern, uint8_t pattern_len,
                           const char* filename) {
    if (pattern_len == 0) return false;
    if (pattern_len == 1 && pattern[0] == '*') return false;

    bool has_wildcard = (pattern[pattern_len - 1] == '*');
    uint8_t match_len = has_wildcard ? (uint8_t)(pattern_len - 1) : pattern_len;

    // The filename must be at least as long as the prefix we're matching.
    size_t fname_len = strlen(filename);
    if (fname_len < match_len) return false;

    for (uint8_t i = 0; i < match_len; i++) {
        if (pattern[i] != tolower((unsigned char)filename[i])) return false;
    }

    if (!has_wildcard) {
        // Exact match: filename must be "pattern.prg" or just "pattern"
        return (filename[match_len] == '.' || filename[match_len] == '\0');
    }

    return true;
}

// Search PRGS_DIR for a .prg file matching 'pattern'. On success, writes the
// full path (e.g. "/prgs/game.prg") to 'path_out' and returns true.
static bool find_prg_file(const char* pattern, uint8_t pattern_len,
                          char* path_out, size_t path_out_size) {
    DIR* dir = opendir(PRGS_DIR);
    if (dir == NULL) {
        log_warn("tape: cannot open " PRGS_DIR);
        return false;
    }

    bool found = false;
    struct dirent* ent;

    while ((ent = readdir(dir)) != NULL) {
        const char* name = ent->d_name;

        // Skip entries that do not end with ".prg" (case-insensitive)
        size_t len = strlen(name);
        if (len < 5) continue;
        const char* ext = name + len - 4;
        if (tolower((unsigned char)ext[0]) != '.'
            || tolower((unsigned char)ext[1]) != 'p'
            || tolower((unsigned char)ext[2]) != 'r'
            || tolower((unsigned char)ext[3]) != 'g') {
            continue;
        }

        if (match_filename(pattern, pattern_len, name)) {
            int n = snprintf(path_out, path_out_size, "%s/%s", PRGS_DIR, name);
            if (n < 0 || (size_t)n >= path_out_size) {
                log_warn("tape: path too long for %s", name);
                continue;
            }
            found = true;
            break;
        }
    }

    closedir(dir);
    return found;
}

// Build the tape buffer stub: print loop + JMP ld210 + message string.
// Returns the total number of bytes written.
static size_t tape_build_stub(uint8_t* buf, const char* path) {
    // Print loop (13 bytes)
    memcpy(buf, print_loop, PRINT_LOOP_SIZE);

    // Patch LDA operand with the message address
    uint16_t msg_addr = TAPE_BUFFER + PRINT_LOOP_SIZE + JMP_SIZE;
    buf[3] = (uint8_t)(msg_addr & 0xFF);
    buf[4] = (uint8_t)(msg_addr >> 8);

    // JMP ld210 (3 bytes) - jump to KERNAL post-load fixup
    buf[PRINT_LOOP_SIZE]     = 0x4C;    // JMP
    buf[PRINT_LOOP_SIZE + 1] = (uint8_t)(state.cfg.ld210 & 0xFF);
    buf[PRINT_LOOP_SIZE + 2] = (uint8_t)(state.cfg.ld210 >> 8);

    // Message string: FOUND /PRGS/GAME.PRG\rLOADING\0
    size_t off = PRINT_LOOP_SIZE + JMP_SIZE;

    static const char found_prefix[] = "FOUND ";
    memcpy(buf + off, found_prefix, sizeof(found_prefix) - 1);
    off += sizeof(found_prefix) - 1;

    // Path in uppercase for PET display
    for (size_t i = 0; path[i] != '\0'; i++) {
        buf[off++] = (uint8_t)toupper((unsigned char)path[i]);
    }

    buf[off++] = '\r';      // CR (newline on PET)

    static const char loading[] = "LOADING";
    memcpy(buf + off, loading, sizeof(loading) - 1);
    off += sizeof(loading) - 1;
    buf[off++] = '\0';      // Null terminator for print loop

    return off;
}

static uint16_t tape_load_callback(uint16_t pc, void* context) {
    (void)context;

    // The breakpoint fires for every LOAD (tape, disk, etc.), so check
    // the device number first. Only intercept tape devices (1 or 2).
    uint8_t devnum = spi_read_at(state.cfg.devnum);
    if (devnum != 1 && devnum != 2) {
        return pc;
    }

    // Read the requested filename from PET memory.
    uint8_t fnlen = spi_read_at(state.cfg.fnlen);
    if (fnlen > 16) fnlen = 16;

    uint8_t fnadr_lo = spi_read_at(state.cfg.fnadr);
    uint8_t fnadr_hi = spi_read_at(state.cfg.fnadr + 1);
    uint16_t fnadr = (uint16_t)(fnadr_lo | (fnadr_hi << 8));

    // Convert filename from PETSCII to lowercase ASCII.
    char pattern[17];
    for (uint8_t i = 0; i < fnlen; i++) {
        pattern[i] = petscii_to_lower(spi_read_at(fnadr + i));
    }
    pattern[fnlen] = '\0';

    log_info("tape: LOAD \"%s\" (len=%d)", pattern, fnlen);

    // Check for empty name or lone "*" (fall through to datasette).
    if (fnlen == 0 || (fnlen == 1 && pattern[0] == '*')) {
        log_info("tape: fall through to datasette");
        return pc;
    }

    // Search SD card for a matching .prg file.
    char path[PATH_MAX];
    if (!find_prg_file(pattern, fnlen, path, sizeof(path))) {
        log_info("tape: no match for \"%s\"", pattern);
        return pc;
    }

    log_info("tape: found %s", path);

    // Open the PRG file and read the 2-byte load address.
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        log_warn("tape: cannot open %s", path);
        return pc;
    }

    uint8_t hdr[2];
    if (fread(hdr, 1, 2, file) != 2) {
        log_warn("tape: short read on header of %s", path);
        fclose(file);
        return pc;
    }
    uint16_t load_addr = (uint16_t)(hdr[0] | (hdr[1] << 8));
    uint16_t dest = load_addr;

    // Step 5: Copy program data into SRAM via SPI.
    uint8_t* temp = acquire_temp_buffer();
    size_t bytes_read;

    while ((bytes_read = fread(temp, 1, TEMP_BUFFER_SIZE, file)) > 0) {
        spi_write(dest, temp, bytes_read);
        dest += bytes_read;
    }

    release_temp_buffer(&temp);
    fclose(file);

    uint16_t end_addr = dest;
    log_info("tape: loaded $%04X-$%04X from %s", load_addr, end_addr, path);

    // Set EAL/EAH to the end address. The KERNAL's LD210 routine
    // copies these into VARTAB before calling FINI to relink and clear.
    spi_write_at(state.cfg.eal, (uint8_t)(end_addr & 0xFF));
    spi_write_at(state.cfg.eah, (uint8_t)(end_addr >> 8));

    // Build the stub in the tape buffer and write it to SRAM.
    uint8_t stub_buf[TAPE_BUFFER_CAPACITY];
    size_t stub_len = tape_build_stub(stub_buf, path);
    spi_write(TAPE_BUFFER, stub_buf, stub_len);

    // Redirect execution to the tape buffer.
    return TAPE_BUFFER;
}

void tape_init(const tape_config_t* cfg) {
    memset(&state, 0, sizeof(state));

    if (cfg == NULL) {
        log_info("tape: disabled (no config blob)");
        return;
    }

    state.cfg = *cfg;
    state.enabled = true;
    bp_set(state.cfg.bp_addr, tape_load_callback, NULL);
}

void tape_deinit(void) {
    if (state.enabled) {
        bp_remove(state.cfg.bp_addr);
        state.enabled = false;
        log_info("tape: disabled");
    }
}
