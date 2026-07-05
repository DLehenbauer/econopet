#include "pch.hpp"
#include "clock.hpp"

#include <cstdlib>   // std::getenv (ROM directory via ECONOPET_ROMS_DIR)
#include <string>

#define CHIPS_IMPL
#include "m6502.h"

#include "display.hpp"
#include "roms.hpp"
#include "trace.hpp"

uint8_t memory[0x10000] = { 0 };

// PET keyboard matrix is 10 rows x 8 cols.
uint8_t keyMatrix[10] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// Default register values for CRTC
// (See https://github.com/sjgray/cbm-edit-rom/blob/master/docs/CRTC%20Registers.txt)
constexpr uint8_t crtc_reg_defaults[10] = { 0x31, 0x28, 0x29, 0x0f, 0x28, 0x05, 0x19, 0x21, 0x00, 0x07 };

constexpr uint16_t vramStart      = 0x8000;
constexpr uint16_t vramEnd        = vramStart + 1024;

// CRTC registers are copied to E8Fx;
constexpr uint16_t crtcStart = 0xE8F0;
constexpr uint16_t crtcEnd   = 0xE900;

m6502_t cpu;
m6502_desc_t cpu_desc = {
    /* bool bcd_disabled            */ false,       /* set to true if BCD mode is disabled              */
    /* m6510_in_t m6510_in_cb       */ nullptr,     /* optional port IO input callback (only on m6510)  */
    /* m6510_out_t m6510_out_cb     */ nullptr,     /* optional port IO output callback (only on m6510) */
    /* void* m6510_user_data        */ nullptr,     /* optional callback user data                      */
    /* uint8_t m6510_io_pullup      */ 0,           /* IO port bits that are 1 when reading             */
    /* uint8_t m6510_io_floating    */ 0            /* unconnected IO port pins                         */
};

uint64_t pins = m6502_init(&cpu, &cpu_desc);

bool loadRom(const char* file, uint8_t* pDest, std::streamsize byteSize) {
    std::ifstream input(file, std::ios::binary);
    if (!input.good()) {
        trace("loadRom(): Failed to open '%s'.\n", file);
        return false;
    }

    input.read(reinterpret_cast<char*>(pDest), byteSize);
    if (!input) {
        trace("loadRom(): '%s' too short (expected %u bytes, but got %u bytes).\n", file, byteSize, input.gcount());
        return false;
    }

    input.peek();
    if (!input.eof()) {
        trace("loadRom(): '%s' too long (expected %u bytes).\n", file, byteSize);
        return false;
    }

    input.close();
    trace("ROM Loaded: '%s' (%u Kb)\n", file, byteSize / 1024);
    return true;
}

// Directory to load PET ROM images from. The EconoPET repo provides ROMs via its
// existing solution (see .devcontainer/download-roms.sh); point the emulator at
// that directory with the ECONOPET_ROMS_DIR environment variable. Falls back to a
// local "roms/" directory when the variable is not set.
std::string romDir() {
    const char* dir = std::getenv("ECONOPET_ROMS_DIR");
    std::string base = (dir != nullptr && dir[0] != '\0') ? dir : "roms";
    if (!base.empty() && base.back() != '/') {
        base += '/';
    }
    return base;
}

bool loadRomSet(unsigned index) {
    const RomEntry* pRom = (romSets + index)->roms;

    while (pRom->file != nullptr) {
        std::string path = romDir();
        std::string file = pRom->file;
        std::string fullPath = path + file;

        if (!loadRom(fullPath.c_str(), memory + pRom->addr, pRom->byteLength)) {
            return false;
        }
        pRom++;
    }

    return true;
}

std::streampos getFileSize(const char* file) {
    std::streampos fsize = 0;
    std::ifstream input(file, std::ios::binary);

    fsize = input.tellg();
    input.seekg(0, std::ios::end);
    fsize = input.tellg() - fsize;
    input.close();

    return fsize;
}

unsigned loadPrg(const char* file) {
    const auto actualBytes = getFileSize(file);
    std::ifstream input(file, std::ios::binary);

    // .PRG files begin with a 2B LE header containing load address.
    uint16_t loadAddr;
    input.read(reinterpret_cast<char*>(&loadAddr), 2);
    if (!input) {
        trace("loadPrg(): Expected 2 byte PRG header, but got %u bytes.\n", file, input.gcount());
        return 0;
    }

    if (!input.good()) {
        trace("loadPrg(): Failed to open '%s'.\n", file);
        return 0;
    }

    char* const pDest   = reinterpret_cast<char*>(&memory[loadAddr]);
    const std::streamsize maxSize = 0x8000 - loadAddr;

    input.read(pDest, maxSize);

    input.peek();
    if (!input.eof()) {
        trace("loadPrg(): '%s' out of memory (%u bytes available, but got %u bytes).\n", maxSize, actualBytes);
        return 0;
    }

    input.close();

    unsigned size = static_cast<unsigned>(actualBytes) + 0x3FF;
    memory[0x2a] = memory[0xc9] = size & 0xFF;
    memory[0x2b] = memory[0xca] = size >> 8;

    return actualBytes;
}

void reset() {
    memset(memory, 0, 0x10000);

    bool ok = loadRomSet(6);
    assert(ok);

    // No keys currently pressed
    memset(keyMatrix, 0xFF, sizeof(keyMatrix));

    // Initialize memory w/expected IO values
    memory[0xE810] = 0x00;      // Diagonstic sense on bit 7: 0 = TIM, 1 = BASIC
    memory[0xE812] = 0xFF;      // No keys pressed
    memory[0xE813] = 0x80;      // PIA1 CB1
    memory[0xE840] = 0xDF;      // VIA Port B
    memory[0xE84E] = 0x80;

    // Initialize CRTC registers w/default values
    memcpy(&memory[crtcStart], crtc_reg_defaults, sizeof(crtc_reg_defaults));
    
    pins |= M6502_RES;
}

uint8_t charRom[256 * 8];

int main() {
    CClock clock;

    bool ok = loadRom((romDir() + "characters-2.901447-10.bin").c_str(), charRom, /* byteSize: */ sizeof(charRom));
    assert(ok);

    CDisplay disp(
        charRom,
        sizeof(charRom),
        &memory[vramStart],
        &memory[crtcStart],
        keyMatrix);

    reset();
    
    TimePoint start = clock.now();
    int64_t executed_cycles = 0;
    int64_t next_jiffy      = 16640;

    while (true) {
        const int64_t total_cycles   = clock.elapsed(start, clock.now());
        int64_t remaining_cycles = total_cycles - executed_cycles;
        next_jiffy -= remaining_cycles;
        
        if (next_jiffy <= 0) {
            remaining_cycles += next_jiffy;
            next_jiffy        = 16640;
            pins |= M6502_IRQ;
            disp.update();
        }

        executed_cycles += remaining_cycles;

        while (remaining_cycles-- != 0) {
            pins = m6502_tick(&cpu, pins);
            
            // extract 16-bit address from pin mask
            const uint16_t addr = M6502_GET_ADDR(pins);
            
            // perform memory access
            uint8_t data;
            if (pins & M6502_RW) {
                switch (addr) {
                    case 0xE810:
                        data = 0xF0 | memory[0xE810];
                        break;
                    case 0xE812:
                        // Low nibble of PIA1 Port A (E810) selects row to read.
                        data = keyMatrix[memory[0xE810] & 0xF];
                        break;
                    case 0xE813:
                        data = 0x80;
                        break;
                    case 0xE840:
                        data = 0xDF;
                        break;
                    default:
                        data = memory[addr];
                        break;
                }

                // trace("R: %04x -> %02x\n", addr, data);
                M6502_SET_DATA(pins, data);
            }
            else {
                data = M6502_GET_DATA(pins);
                // trace("W: %04x <- %02x\n", addr, data);

                if (addr < 0x9000 || (0xE800 < addr && addr < 0xf000)) {
                    memory[addr] = data;
                } else {
                    trace("W: %04x <- %02x (ILLEGAL)\n", addr, data);
                    assert(false);
                }
            }

            pins &= ~M6502_IRQ;
        }

        if (CDisplay::s_funcKey) {
            const unsigned key = 31 - __builtin_clz(CDisplay::s_funcKey);
            
            switch (key + 1) {
                case 1: reset(); break;
                case 2: loadPrg("prgs/SpaceInvaders.prg"); break;
                case 3: loadPrg("prgs/PetDraw4032.prg"); break;
                case 4: loadPrg("prgs/DiamondHuntII.prg"); break;
                case 5: loadPrg("prgs/NoPetsAllowed.prg"); break;
                case 6: loadPrg("prgs/npa-patch.prg"); break;
                case 7: loadPrg("prgs/crtcx-pet-v1.prg"); break;
            }
            
            CDisplay::s_funcKey &= ~(1 << key);
        }
    }
}
