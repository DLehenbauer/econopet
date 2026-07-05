#include "display.hpp"
#include "trace.hpp"

#define MNONE { 0, 0 }
#define M(row, col) { row, (1 << col) }

volatile unsigned CDisplay::s_funcKey = 0;

const static uint8_t s_KeyToRowCol[SDL_NUM_SCANCODES][2] = {
    /* 0x00: None */ MNONE,
    /* 0x01: None */ MNONE,
    /* 0x02: None */ MNONE,
    /* 0x03: None */ MNONE,

    /* 0x04: 'A' -> 'A'  */ M(4, 0),
    /* 0x05: 'B' -> 'B'  */ M(6, 2),
    /* 0x06: 'C' -> 'C'  */ M(6, 1),
    /* 0x07: 'D' -> 'D'  */ M(4, 1),
    /* 0x08: 'E' -> 'E'  */ M(2, 1),
    /* 0x09: 'F' -> 'F'  */ M(5, 1),
    /* 0x0A: 'G' -> 'G'  */ M(4, 2),
    /* 0x0B: 'H' -> 'H'  */ M(5, 2),
    /* 0x0C: 'I' -> 'I'  */ M(3, 3),
    /* 0x0D: 'J' -> 'J'  */ M(4, 3),
    /* 0x0E: 'K' -> 'K'  */ M(5, 3),
    /* 0x0F: 'L' -> 'L'  */ M(4, 4),
    /* 0x10: 'M' -> 'M'  */ M(6, 3),
    /* 0x11: 'N' -> 'N'  */ M(7, 2),
    /* 0x12: 'O' -> 'O'  */ M(2, 4),
    /* 0x13: 'P' -> 'P'  */ M(3, 4),
    /* 0x14: 'Q' -> 'Q'  */ M(2, 0),
    /* 0x15: 'R' -> 'R'  */ M(3, 1),
    /* 0x16: 'S' -> 'S'  */ M(5, 0),
    /* 0x17: 'T' -> 'T'  */ M(2, 2),
    /* 0x18: 'U' -> 'U'  */ M(2, 3),
    /* 0x19: 'V' -> 'V'  */ M(7, 1),
    /* 0x1A: 'W' -> 'W'  */ M(3, 0),
    /* 0x1B: 'X' -> 'X'  */ M(7, 0),
    /* 0x1C: 'Y' -> 'Y'  */ M(3, 2),
    /* 0x1D: 'Z' -> 'Z'  */ M(6, 0),
    /* 0x1E: '1' -> '!'  */ M(0, 0),
    /* 0x1F: '2' -> '"'  */ M(1, 0),
    /* 0x20: '3' -> '#'  */ M(0, 1),
    /* 0x21: '4' -> '$'  */ M(1, 1),
    /* 0x22: '5' -> '%'  */ M(0, 2),
    /* 0x23: '6' -> '''  */ M(1, 2),
    /* 0x24: '7' -> '&'  */ M(0, 3),
    /* 0x25: '8' -> '\'  */ M(1, 3),
    /* 0x26: '9' -> '('  */ M(0, 4),
    /* 0x27: '0' -> ')'  */ M(1, 4),

    /* 0x28: Return    -> Return     */ M(6, 5),
    /* 0x29: Escape    -> Run Stop   */ M(9, 4),
    /* 0x2A: Backspace -> Inst Del   */ M(1, 7),
    /* 0x2B: Tabulator -> RVS        */ M(9, 0),
    /* 0x2C: Space     -> Space      */ M(9, 2),

    /* 0x2D: '-' -> '-'  */ M(0, 5),
    /* 0x2E: '=' -> '='  */ M(9, 7),
    /* 0x2F: '[' -> '['  */ M(9, 1),
    /* 0x30: ']' -> ']'  */ M(8, 2),
    /* 0x31: '\' -> '^'  */ M(2, 5),
    /* 0x32: '#' -> '#'  */ M(0, 1),
    /* 0x33: ';' -> ':'  */ M(5, 4),
    /* 0x34: ''' -> R/S  */ M(9, 4),
    /* 0x35: '`' -> '@'  */ M(8, 1),
    /* 0x36: ',' -> ','  */ M(7, 3),
    /* 0x37: '.' -> ';'  */ M(6, 4),
    /* 0x38: '/' -> '?'  */ M(7, 4),

    /* 0x39: CapsLock   */ MNONE,
    /* 0x3A: F1         */ MNONE,
    /* 0x3B: F2         */ MNONE,
    /* 0x3C: F3         */ MNONE,
    /* 0x3D: F4         */ MNONE,
    /* 0x3E: F5         */ MNONE,
    /* 0x3F: F6         */ MNONE,
    /* 0x40: F7         */ MNONE,
    /* 0x41: F8         */ MNONE,
    /* 0x42: F9         */ MNONE,
    /* 0x43: F10        */ MNONE,
    /* 0x44: F11        */ MNONE,
    /* 0x45: F12        */ MNONE,

    /* 0x46: PRINTSCREEN    */ MNONE,
    /* 0x47: SCROLLLOCK     */ MNONE,
    /* 0x48: PAUSE          */ MNONE,
    /* 0x49: INSERT         */ MNONE,
    /* 0x4A: HOME           */ M(0, 6),
    /* 0x4B: PAGEUP         */ MNONE,
    /* 0x4C: DELETE         */ MNONE,
    /* 0x4D: END            */ M(9, 3),
    /* 0x4E: PAGEDOWN       */ M(8, 4),
    /* 0x4F: RIGHT          */ M(0, 7),
    /* 0x50: LEFT           */ M(0, 7),     // Needs shift
    /* 0x51: DOWN           */ M(1, 6),
    /* 0x52: UP             */ M(1, 6),     // Needs shift
    /* 0x53: NUMLOCKCLEAR   */ MNONE,

    /* 0x54: KP_Divide      */ M(3, 7),
    /* 0x55: KP_Multiply    */ M(5, 7),
    /* 0x56: KP_Subtract    */ M(8, 7),
    /* 0x57: KP_Add         */ M(7, 7),
    /* 0x58: KP_Enter       */ M(9, 7),
    /* 0x59: KP_1           */ M(6, 6),
    /* 0x5A: KP_2           */ M(7, 6),
    /* 0x5B: KP_3           */ M(6, 7),
    /* 0x5C: KP_4           */ M(4, 6),
    /* 0x5D: KP_5           */ M(5, 6),
    /* 0x5E: KP_6           */ M(4, 7),
    /* 0x5F: KP_7           */ M(2, 6),
    /* 0x60: KP_8           */ M(3, 6),
    /* 0x61: KP_9           */ M(2, 7),
    /* 0x62: KP_0           */ M(8, 6),
    /* 0x63: KP_PERIOD      */ M(9, 6),
    /* 0x64: NONUSBACKSLASH */ MNONE,
    /* 0x65: APPLICATION    */ MNONE,
    /* 0x66: POWER          */ MNONE,
    /* 0x67: KP_EQUALS      */ MNONE,
    /* 0x68: F13            */ MNONE,
    /* 0x69: F14            */ MNONE,
    /* 0x6A: F15            */ MNONE,
    /* 0x6B: F16            */ MNONE,
    /* 0x6C: F17            */ MNONE,
    /* 0x6D: F18            */ MNONE,
    /* 0x6E: F19            */ MNONE,
    /* 0x6F: F20            */ MNONE,
    /* 0x70: F21            */ MNONE,
    /* 0x71: F22            */ MNONE,
    /* 0x72: F23            */ MNONE,
    /* 0x73: F24            */ MNONE,
    /* 0x74: EXECUTE        */ MNONE,
    /* 0x75: HELP           */ MNONE,
    /* 0x76: MENU           */ MNONE,
    /* 0x77: SELECT         */ MNONE,
    /* 0x78: STOP           */ MNONE,
    /* 0x79: AGAIN          */ MNONE,
    /* 0x7A: UNDO           */ MNONE,
    /* 0x7B: CUT            */ MNONE,
    /* 0x7C: COPY           */ MNONE,
    /* 0x7D: PASTE          */ MNONE,
    /* 0x7E: FIND           */ MNONE,
    /* 0x7F: MUTE           */ MNONE,
    /* 0x80: VOLUMEUP       */ MNONE,
    /* 0x81: VOLUMEDOWN     */ MNONE,
    /* 0x82: LOCKINGCAPSLOCK    */ MNONE,
    /* 0x83: LOCKINGNUMLOCK     */ MNONE,
    /* 0x84: LOCKINGSCROLLLOCK  */ MNONE,
    /* 0x85: KP_COMMA           */ MNONE,
    /* 0x86: KP_EQUALSAS400     */ MNONE,
    /* 0x87: INTERNATIONAL1     */ MNONE,
    /* 0x88: INTERNATIONAL2     */ MNONE,
    /* 0x89: INTERNATIONAL3     */ MNONE,
    /* 0x8A: INTERNATIONAL4     */ MNONE,
    /* 0x8B: INTERNATIONAL5     */ MNONE,
    /* 0x8C: INTERNATIONAL6     */ MNONE,
    /* 0x8D: INTERNATIONAL7     */ MNONE,
    /* 0x8E: INTERNATIONAL8     */ MNONE,
    /* 0x8F: INTERNATIONAL9     */ MNONE,
    /* 0x90: LANG1              */ MNONE,
    /* 0x91: LANG2              */ MNONE,
    /* 0x92: LANG3              */ MNONE,
    /* 0x93: LANG4              */ MNONE,
    /* 0x94: LANG5              */ MNONE,
    /* 0x95: LANG6              */ MNONE,
    /* 0x96: LANG7              */ MNONE,
    /* 0x97: LANG8              */ MNONE,
    /* 0x98: LANG9              */ MNONE,
    /* 0x99: ALTERASE           */ MNONE,
    /* 0x9A: SYSREQ             */ MNONE,
    /* 0x9B: CANCEL             */ MNONE,
    /* 0x9C: CLEAR              */ MNONE,
    /* 0x9D: PRIOR              */ MNONE,
    /* 0x9E: RETURN2            */ MNONE,
    /* 0x9F: SEPARATOR          */ MNONE,
    /* 0xA0: OUT                */ MNONE,
    /* 0xA1: OPER               */ MNONE,
    /* 0xA2: CLEARAGAIN         */ MNONE,
    /* 0xA3: CRSEL              */ MNONE,
    /* 0xA4: EXSEL              */ MNONE,
    /* 0xA5: NONE               */ MNONE,
    /* 0xA6: NONE               */ MNONE,
    /* 0xA7: NONE               */ MNONE,
    /* 0xA8: NONE               */ MNONE,
    /* 0xA9: NONE               */ MNONE,
    /* 0xAA: NONE               */ MNONE,
    /* 0xAB: NONE               */ MNONE,
    /* 0xAC: NONE               */ MNONE,
    /* 0xAD: NONE               */ MNONE,
    /* 0xAE: NONE               */ MNONE,
    /* 0xAF: NONE               */ MNONE,
    /* 0xB0: KP_00              */ MNONE,
    /* 0xB1: KP_000             */ MNONE,
    /* 0xB2: THOUSANDSSEPARATOR */ MNONE,
    /* 0xB3: DECIMALSEPARATOR   */ MNONE,
    /* 0xB4: CURRENCYUNIT       */ MNONE,
    /* 0xB5: CURRENCYSUBUNIT    */ MNONE,
    /* 0xB6: KP_LEFTPAREN       */ MNONE,
    /* 0xB7: KP_RIGHTPAREN      */ MNONE,
    /* 0xB8: KP_LEFTBRACE       */ MNONE,
    /* 0xB9: KP_RIGHTBRACE      */ MNONE,
    /* 0xBA: KP_TAB             */ MNONE,
    /* 0xBB: KP_BACKSPACE       */ MNONE,
    /* 0xBC: KP_A               */ MNONE,
    /* 0xBD: KP_B               */ MNONE,
    /* 0xBE: KP_C               */ MNONE,
    /* 0xBF: KP_D               */ MNONE,
    /* 0xC0: KP_E               */ MNONE,
    /* 0xC1: KP_F               */ MNONE,
    /* 0xC2: KP_XOR             */ MNONE,
    /* 0xC3: KP_POWER           */ MNONE,
    /* 0xC4: KP_PERCENT         */ MNONE,
    /* 0xC5: KP_LESS            */ MNONE,
    /* 0xC6: KP_GREATER         */ MNONE,
    /* 0xC7: KP_AMPERSAND       */ MNONE,
    /* 0xC8: KP_DBLAMPERSAND    */ MNONE,
    /* 0xC9: KP_VERTICALBAR     */ MNONE,
    /* 0xCA: KP_DBLVERTICALBAR  */ MNONE,
    /* 0xCB: KP_COLON           */ MNONE,
    /* 0xCC: KP_HASH            */ MNONE,
    /* 0xCD: KP_SPACE           */ MNONE,
    /* 0xCE: KP_AT              */ MNONE,
    /* 0xCF: KP_EXCLAM          */ MNONE,
    /* 0xD0: KP_MEMSTORE        */ MNONE,
    /* 0xD1: KP_MEMRECALL       */ MNONE,
    /* 0xD2: KP_MEMCLEAR        */ MNONE,
    /* 0xD3: KP_MEMADD          */ MNONE,
    /* 0xD4: KP_MEMSUBTRACT     */ MNONE,
    /* 0xD5: KP_MEMMULTIPLY     */ MNONE,
    /* 0xD6: KP_MEMDIVIDE       */ MNONE,
    /* 0xD7: KP_PLUSMINUS       */ MNONE,
    /* 0xD8: KP_CLEAR           */ MNONE,
    /* 0xD9: KP_CLEARENTRY      */ MNONE,
    /* 0xDA: KP_BINARY          */ MNONE,
    /* 0xDB: KP_OCTAL           */ MNONE,
    /* 0xDC: KP_DECIMAL         */ MNONE,
    /* 0xDD: KP_HEXADECIMAL     */ MNONE,
    /* 0xDE: NONE               */ MNONE,
    /* 0xDF: NONE               */ MNONE,
    /* 0xE0: LCTRL              */ MNONE,
    /* 0xE1: LSHIFT             */ M(8,0),
    /* 0xE2: LALT               */ MNONE,
    /* 0xE3: LGUI               */ MNONE,
    /* 0xE4: RCTRL              */ MNONE,
    /* 0xE5: RSHIFT             */ M(8,5),
    /* 0xE6: RALT               */ MNONE,
    /* 0xE7: RGUI               */ MNONE,
};

enum class CrtcReg {
    HorizontalTotal         = 0,        // (-1) > 40
    HorizontalDisplayed     = 1,        // Always 40 in standard 40 or 80 column machines due to the way memory is configured
    HorizontalSyncPosition  = 2,
    HorizontalSyncWidth     = 3,
    VerticalTotal           = 4,        // (-1) (7 bit) 39 or 49
    VerticalTotalAdjust     = 5,        // Additional scalelines (5 bit)
    VerticalDisplayed       = 6,        // (7-bit) Always 25 for PET/CBM
    VerticalSyncPosition    = 7,        // (7 bit) Must be less than R4
    InterlaceModeAndSkew    = 8,        // Always 0. Some variations allow interlace mode
    MaximumRasterAddress    = 9,        // 7 (for Graphics Mode) or 9 (for Text Mode) - One less than actual # of rasters.
    DisplayStartAddressHigh = 12,       // Only lower 4-bits are used. Upper 4-bits control additional features.
    DisplayStartAddressLow  = 13,
};

unsigned CDisplay::get_crtc_horizontal_total()           { return this->pCrtcRegs[0]; }
unsigned CDisplay::get_crtc_horizontal_displayed()       { return this->pCrtcRegs[1]; }
unsigned CDisplay::get_crtc_horizontal_sync_position()   { return this->pCrtcRegs[2]; }
unsigned CDisplay::get_crtc_horizontal_sync_width()      { return this->pCrtcRegs[3] & 0x0f; /* 4 bit */ }
unsigned CDisplay::get_crtc_vertical_total()             { return this->pCrtcRegs[4] & 0x7f; /* 7 bit */ }
unsigned CDisplay::get_crtc_vertical_total_adjust()      { return this->pCrtcRegs[5] & 0x1f; /* 5 bit */ }
unsigned CDisplay::get_crtc_vertical_displayed()         { return this->pCrtcRegs[6] & 0x7f; /* 7 bit */ }
unsigned CDisplay::get_crtc_vertical_sync_position()     { return this->pCrtcRegs[7] & 0x7f; /* 7 bit */ }
unsigned CDisplay::get_crtc_interlace_mode_and_skew()    { return this->pCrtcRegs[8]; }
unsigned CDisplay::get_crtc_maximum_raster_address()     { return this->pCrtcRegs[9] & 0x1f; /* 5 bit */ }

unsigned CDisplay::get_crtc_display_start_address() {
    unsigned addr = this->pCrtcRegs[12];
    addr &= 0x0f;   // 4 bit
    addr <<= 8;
    addr |= this->pCrtcRegs[13];
    return addr;
}

void set_pixel(SDL_Surface* pSurface, unsigned x, unsigned y, uint32_t pixel)
{
    uint32_t* const target_pixel = reinterpret_cast<uint32_t*>(
      static_cast<uint8_t*>(pSurface->pixels)
        + y * pSurface->pitch
        + x * pSurface->format->BytesPerPixel);

    *target_pixel = pixel;
}

constexpr int screen_width  = 48 * 8;
constexpr int screen_height = 34 * 8;

CDisplay::CDisplay(
    const uint8_t* const pCharRom,
    const size_t charRomByteSize,
    const uint8_t* const pVideoMemory,
    const uint8_t* pCrtcRegs,
    uint8_t keyMatrix[10]
): pVideoMemory(pVideoMemory), pCrtcRegs(pCrtcRegs), keyMatrix(keyMatrix) {
    if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
        trace("[Error] SDL Init : %s\n", SDL_GetError());
    } else {
        SDL_DisplayMode dm;
        SDL_GetCurrentDisplayMode(0, &dm);

        trace("Display mode is %dx%dpx @ %dhz\n", dm.w, dm.h, dm.refresh_rate);
    }
    
    // SDL_ShowCursor(SDL_DISABLE);
    
    if (SDL_CreateWindowAndRenderer(screen_width, screen_height, SDL_WINDOW_SHOWN, &pWindow, &pRenderer) != 0) {
        trace("[Error] Creating Window and Renderer: %s\n", SDL_GetError());
        exit(0);
    }

    SDL_CreateWindowAndRenderer(screen_width, screen_height, SDL_WINDOW_SHOWN, &pWindow, &pRenderer);

    SDL_Surface* pCharSrc = SDL_CreateRGBSurfaceWithFormat(
        /* flags: */ 0,
        /* width: */ 8,
        /* height: */ charRomByteSize,
        /* depth: */ 32,
        /* format: */ SDL_PIXELFORMAT_RGB888);

    assert(pCharSrc->format->BytesPerPixel == 4);

    if (pCharSrc == nullptr) {
        SDL_Log("SDL_CreateRGBSurfaceWithFormat() failed: %s", SDL_GetError());
        exit(1);
    }

    for (size_t y = 0; y < (charRomByteSize / 2); y++) {
        constexpr uint32_t on = 0xFF44FF88;
        constexpr uint32_t off = 0xFF000000;

        uint8_t row = pCharRom[y];

        for (int x = 0; x < 8; x++) {
            if (row & 0x80) {
                set_pixel(pCharSrc, x, y, on);
                set_pixel(pCharSrc, x, y + (128 * 8), off);
            } else {
                set_pixel(pCharSrc, x, y, off);
                set_pixel(pCharSrc, x, y + (128 * 8), on);
            }
            row <<= 1;
        }
    }

    pCharTex = SDL_CreateTextureFromSurface(pRenderer, pCharSrc);
    if (pCharTex == nullptr) {
        SDL_Log("SDL_CreateTextureFromSurface() failed: %s", SDL_GetError());
        exit(1);
    }
    SDL_FreeSurface(pCharSrc);

    pTargetTex = SDL_CreateTexture(pRenderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_TARGET, screen_width, screen_height);

    SDL_SetRenderDrawColor(pRenderer, 0, 0, 0, 0);
    SDL_RenderClear(pRenderer);
    SDL_RenderPresent(pRenderer);
}

void CDisplay::update() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                // Window closed (or Cmd/Alt+F4): shut down SDL and exit.
                SDL_Quit();
                exit(0);
                break;
            case SDL_WINDOWEVENT:
                // Some window managers deliver a close request as a window
                // event rather than SDL_QUIT.
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                    SDL_Quit();
                    exit(0);
                }
                break;
            case SDL_KEYDOWN:
                // Escape quits (reliable across display environments where the
                // window-close button does not generate SDL_QUIT).
                if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                    SDL_Quit();
                    exit(0);
                }
                keyDown(event.key.keysym);
                break;
            case SDL_KEYUP:
                keyUp(event.key.keysym);
                break;
        }
    }

    SDL_SetRenderDrawColor(pRenderer, 0, 0, 0, 255);
    SDL_SetRenderTarget(pRenderer, pTargetTex);
    SDL_RenderFillRect(pRenderer, nullptr);

    const int charWidth    = 8;
    const int charCountX   = this->get_crtc_horizontal_displayed();
    const int videoStartX  = (screen_width - (charWidth * 40)) >> 1;
    const int videoEndX    = videoStartX + charCountX * charWidth;

    const int charHeight   = this->get_crtc_maximum_raster_address() + 1;
    const int charCountY   = this->get_crtc_vertical_displayed();
    const int videoHeight  = charHeight * charCountY;
    const int videoStartY  = (screen_height - videoHeight) >> 1;
    const int videoEndY    = videoStartY + charCountY * charHeight;

    const int addrStart  = this->get_crtc_display_start_address();

    for (int addr = addrStart, y = videoStartY; y < videoEndY; y += charHeight) {
        for (int x = videoStartX; x < videoEndX; x += charWidth) {
            const int value = pVideoMemory[addr];

            addr++;
            addr &= 0x3ff;     // VRAM wraps at 1KB

            const SDL_Rect src = {
                /* x: */ 0,
                /* y: */ value * 8,
                /* w: */ 8,
                /* h: */ charHeight
            };

            const SDL_Rect dest = {
                /* x: */ x,
                /* y: */ y,
                /* w: */ 8,
                /* h: */ charHeight
            };

            SDL_RenderCopy(pRenderer, pCharTex, &src, &dest);
        }
    }

    SDL_SetRenderTarget(pRenderer, nullptr);
    SDL_RenderCopy(pRenderer, pTargetTex, nullptr, nullptr);
    SDL_RenderPresent(pRenderer);
}

void CDisplay::traceKey(const char* eventName, const unsigned char scanCode, const uint8_t row, const uint8_t colMask) {
        trace("key%s: 0x%02x [%d, %d] %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
            eventName,
            scanCode,
            row,
            31 - __builtin_clz(colMask),
            keyMatrix[0],
            keyMatrix[1],
            keyMatrix[2],
            keyMatrix[3],
            keyMatrix[4],
            keyMatrix[5],
            keyMatrix[6],
            keyMatrix[7],
            keyMatrix[8],
            keyMatrix[9]);
}

void CDisplay::keyDown(SDL_Keysym key) {
    const unsigned char scanCode = key.scancode;

    if (SDL_SCANCODE_F1 <= scanCode && scanCode <= SDL_SCANCODE_F12) {
        CDisplay::s_funcKey |= 1 << (scanCode - SDL_SCANCODE_F1);
    }

    const auto [row, col] = s_KeyToRowCol[scanCode];
    if (col != 0 && (keyMatrix[row] & col)) {
        keyMatrix[row] &= ~col;
        traceKey("Down", scanCode, row, col);
    }
}

void CDisplay::keyUp(SDL_Keysym key) {
    const unsigned char scanCode = key.scancode;
    const auto [row, col] = s_KeyToRowCol[scanCode];
    if (col != 0 && !(keyMatrix[row] & col)) {
        keyMatrix[row] |= col;
        traceKey("Up  ", scanCode, row, col);
    }
}
