/**
 * PET Clone - Open hardware implementation of the Commodore PET
 * by Daniel Lehenbauer and contributors.
 * 
 * https://github.com/DLehenbauer/commodore-pet-clone
 *
 * To the extent possible under law, I, Daniel Lehenbauer, have waived all
 * copyright and related or neighboring rights to this project. This work is
 * published from the United States.
 *
 * @copyright CC0 http://creativecommons.org/publicdomain/zero/1.0/
 * @author Daniel Lehenbauer <DLehenbauer@users.noreply.github.com> and contributors
 */

#include "keyboard.h"

uint8_t key_matrix[KEY_ROW_COUNT] = {
    /* row 0: */ 0xff,
    /* row 1: */ 0xff,
    /* row 2: */ 0xff,
    /* row 3: */ 0xff,
    /* row 4: */ 0xff,
    /* row 5: */ 0xff,
    /* row 6: */ 0xff,
    /* row 7: */ 0xff,
    /* row 8: */ 0xff,
    /* row 9: */ 0xff,
};

#define M_NONE { 0, 0 }
#define M(row, col) { row, (1 << col) }

// Map HID codes to corresponding row/col on PET key matrix.
// TODO: Key layout should be tied to ROM version
//
// (See https://usb.org/sites/default/files/hut1_3_0.pdf chapter 10)

const static uint8_t s_hidToKeyMatrix[][2] = {
    /* 0x00: NONE                           */ M_NONE,
    /* 0x01: ERROR_ROLL_OVER                */ M_NONE,
    /* 0x02: POST_FAIL                      */ M_NONE,
    /* 0x03: ERROR_UNDEFINED                */ M_NONE,
    /* 0x04: A -> 'A'                       */ M(4, 0),
    /* 0x05: B -> 'B'                       */ M(6, 2),
    /* 0x06: C -> 'C'                       */ M(6, 1),
    /* 0x07: D -> 'D'                       */ M(4, 1),
    /* 0x08: E -> 'E'                       */ M(2, 1),
    /* 0x09: F -> 'F'                       */ M(5, 1),
    /* 0x0A: G -> 'G'                       */ M(4, 2),
    /* 0x0B: H -> 'H'                       */ M(5, 2),
    /* 0x0C: I -> 'I'                       */ M(3, 3),
    /* 0x0D: J -> 'J'                       */ M(4, 3),
    /* 0x0E: K -> 'K'                       */ M(5, 3),
    /* 0x0F: L -> 'L'                       */ M(4, 4),
    /* 0x10: M -> 'M'                       */ M(6, 3),
    /* 0x11: N -> 'N'                       */ M(7, 2),
    /* 0x12: O -> 'O'                       */ M(2, 4),
    /* 0x13: P -> 'P'                       */ M(3, 4),
    /* 0x14: Q -> 'Q'                       */ M(2, 0),
    /* 0x15: R -> 'R'                       */ M(3, 1),
    /* 0x16: S -> 'S'                       */ M(5, 0),
    /* 0x17: T -> 'T'                       */ M(2, 2),
    /* 0x18: U -> 'U'                       */ M(2, 3),
    /* 0x19: V -> 'V'                       */ M(7, 1),
    /* 0x1A: W -> 'W'                       */ M(3, 0),
    /* 0x1B: X -> 'X'                       */ M(7, 0),
    /* 0x1C: Y -> 'Y'                       */ M(3, 2),
    /* 0x1D: Z -> 'Z'                       */ M(6, 0),
    /* 0x1E: 1 -> '!'                       */ M(0, 0),
    /* 0x1F: 2 -> '"'                       */ M(1, 0),
    /* 0x20: 3 -> '#'                       */ M(0, 1),
    /* 0x21: 4 -> '$'                       */ M(1, 1),
    /* 0x22: 5 -> '%'                       */ M(0, 2),
    /* 0x23: 6 -> '''                       */ M(1, 2),
    /* 0x24: 7 -> '&'                       */ M(0, 3),
    /* 0x25: 8 -> '\'                       */ M(1, 3),
    /* 0x26: 9 -> '('                       */ M(0, 4),
    /* 0x27: 0 -> ')'                       */ M(1, 4),

    /* 0x28: ENTER     -> Return            */ M(6, 5),
    /* 0x29: ESCAPE    -> Run Stop          */ M(9, 4),
    /* 0x2A: BACKSPACE -> Inst Del          */ M(1, 7),
    /* 0x2B: TAB       -> RVS               */ M(9, 0),
    /* 0x2C: SPACE     -> Space             */ M(9, 2),

    /* 0x2D: MINUS ('-')         -> '-'     */ M(0, 5),
    /* 0x2E: EQUAL ('=')         -> '='     */ M(9, 7),
    /* 0x2F: BRACKET_LEFT ('[')  -> '['     */ M(9, 1),
    /* 0x30: BRACKET_RIGHT (']') -> ']'     */ M(8, 2),
    /* 0x31: BACKSLASH ('\')     -> '^'     */ M(2, 5),
    /* 0x32: EUROPE_1 ('#')      -> '#'     */ M(0, 1),
    /* 0x33: SEMICOLON (';')     -> ':'     */ M(5, 4),
    /* 0x34: APOSTROPHE (''')    -> R/S     */ M(9, 4),
    /* 0x35: GRAVE ('`')         -> '@'     */ M(8, 1),
    /* 0x36: COMMA (',')         -> ','     */ M(7, 3),
    /* 0x37: PERIOD ('.')        -> ';'     */ M(6, 4),
    /* 0x38: SLASH ('/')         -> '?'     */ M(7, 4),

    /* 0x39: CAPS_LOCK                      */ M_NONE,
    /* 0x3A: F1                             */ M_NONE,
    /* 0x3B: F2                             */ M_NONE,
    /* 0x3C: F3                             */ M_NONE,
    /* 0x3D: F4                             */ M_NONE,
    /* 0x3E: F5                             */ M_NONE,
    /* 0x3F: F6                             */ M_NONE,
    /* 0x40: F7                             */ M_NONE,
    /* 0x41: F8                             */ M_NONE,
    /* 0x42: F9                             */ M_NONE,
    /* 0x43: F10                            */ M_NONE,
    /* 0x44: F11                            */ M_NONE,
    /* 0x45: F12                            */ M_NONE,
    /* 0x46: PRINT_SCREEN                   */ M_NONE,
    /* 0x47: SCROLL_LOCK                    */ M_NONE,
    /* 0x48: PAUSE                          */ M_NONE,
    /* 0x49: INSERT                         */ M_NONE,
    /* 0x4A: HOME                           */ M(0, 6),
    /* 0x4B: PAGE_UP                        */ M_NONE,
    /* 0x4C: DELETE                         */ M_NONE,
    /* 0x4D: END                            */ M(9, 3),
    /* 0x4E: PAGE_DOWN                      */ M(8, 4),
    /* 0x4F: ARROW_RIGHT                    */ M(0, 7),
    /* 0x50: ARROW_LEFT                     */ M(0, 7),     // Needs shift
    /* 0x51: ARROW_DOWN                     */ M(1, 6),
    /* 0x52: ARROW_UP                       */ M(1, 6),     // Needs shift
    /* 0x53: NUM_LOCK                       */ M_NONE,
    /* 0x54: KEYPAD_DIVIDE ('/')            */ M(3, 7),
    /* 0x55: KEYPAD_MULTIPLY ('*')          */ M(5, 7),
    /* 0x56: KEYPAD_SUBTRACT ('-')          */ M(8, 7),
    /* 0x57: KEYPAD_ADD ('+')               */ M(7, 7),
    /* 0x58: KEYPAD_ENTER                   */ M(9, 7),
    /* 0x59: KEYPAD_1 ('1')                 */ M(6, 6),
    /* 0x5A: KEYPAD_2 ('2')                 */ M(7, 6),
    /* 0x5B: KEYPAD_3 ('3')                 */ M(6, 7),
    /* 0x5C: KEYPAD_4 ('4')                 */ M(4, 6),
    /* 0x5D: KEYPAD_5 ('5')                 */ M(5, 6),
    /* 0x5E: KEYPAD_6 ('6')                 */ M(4, 7),
    /* 0x5F: KEYPAD_7 ('7')                 */ M(2, 6),
    /* 0x60: KEYPAD_8 ('8')                 */ M(3, 6),
    /* 0x61: KEYPAD_9 ('9')                 */ M(2, 7),
    /* 0x62: KEYPAD_0 ('0')                 */ M(8, 6),
    /* 0x63: KEYPAD_DECIMAL ('.')           */ M(9, 6),
    /* 0x64: EUROPE_2                       */ M_NONE,
    /* 0x65: APPLICATION                    */ M_NONE,
    /* 0x66: POWER                          */ M_NONE,
    /* 0x67: KEYPAD_EQUAL                   */ M_NONE,
    /* 0x68: F13                            */ M_NONE,
    /* 0x69: F14                            */ M_NONE,
    /* 0x6A: F15                            */ M_NONE,
    /* 0x6B: F16                            */ M_NONE,
    /* 0x6C: F17                            */ M_NONE,
    /* 0x6D: F18                            */ M_NONE,
    /* 0x6E: F19                            */ M_NONE,
    /* 0x6F: F20                            */ M_NONE,
    /* 0x70: F21                            */ M_NONE,
    /* 0x71: F22                            */ M_NONE,
    /* 0x72: F23                            */ M_NONE,
    /* 0x73: F24                            */ M_NONE,
    /* 0x74: EXECUTE                        */ M_NONE,
    /* 0x75: HELP                           */ M_NONE,
    /* 0x76: MENU                           */ M_NONE,
    /* 0x77: SELECT                         */ M_NONE,
    /* 0x78: STOP                           */ M_NONE,
    /* 0x79: AGAIN                          */ M_NONE,
    /* 0x7A: UNDO                           */ M_NONE,
    /* 0x7B: CUT                            */ M_NONE,
    /* 0x7C: COPY                           */ M_NONE,
    /* 0x7D: PASTE                          */ M_NONE,
    /* 0x7E: FIND                           */ M_NONE,
    /* 0x7F: MUTE                           */ M_NONE,
    /* 0x80: VOLUME_UP                      */ M_NONE,
    /* 0x81: VOLUME_DOWN                    */ M_NONE,
    /* 0x82: LOCKING_CAPS_LOCK              */ M_NONE,
    /* 0x83: LOCKING_NUM_LOCK               */ M_NONE,
    /* 0x84: LOCKING_SCROLL_LOCK            */ M_NONE,
    /* 0x85: KEYPAD_COMMA                   */ M_NONE,
    /* 0x86: KEYPAD_EQUAL_SIGN              */ M_NONE,
    /* 0x87: KANJI1                         */ M_NONE,
    /* 0x88: KANJI2                         */ M_NONE,
    /* 0x89: KANJI3                         */ M_NONE,
    /* 0x8A: KANJI4                         */ M_NONE,
    /* 0x8B: KANJI5                         */ M_NONE,
    /* 0x8C: KANJI6                         */ M_NONE,
    /* 0x8D: KANJI7                         */ M_NONE,
    /* 0x8E: KANJI8                         */ M_NONE,
    /* 0x8F: KANJI9                         */ M_NONE,
    /* 0x90: LANG1                          */ M_NONE,
    /* 0x91: LANG2                          */ M_NONE,
    /* 0x92: LANG3                          */ M_NONE,
    /* 0x93: LANG4                          */ M_NONE,
    /* 0x94: LANG5                          */ M_NONE,
    /* 0x95: LANG6                          */ M_NONE,
    /* 0x96: LANG7                          */ M_NONE,
    /* 0x97: LANG8                          */ M_NONE,
    /* 0x98: LANG9                          */ M_NONE,
    /* 0x99: ALTERNATE_ERASE                */ M_NONE,
    /* 0x9A: SYSREQ_ATTENTION               */ M_NONE,
    /* 0x9B: CANCEL                         */ M_NONE,
    /* 0x9C: CLEAR                          */ M_NONE,
    /* 0x9D: PRIOR                          */ M_NONE,
    /* 0x9E: RETURN                         */ M_NONE,
    /* 0x9F: SEPARATOR                      */ M_NONE,
    /* 0xA0: OUT                            */ M_NONE,
    /* 0xA1: OPER                           */ M_NONE,
    /* 0xA2: CLEAR_AGAIN                    */ M_NONE,
    /* 0xA3: CRSEL_PROPS                    */ M_NONE,
    /* 0xA4: EXSEL                          */ M_NONE,
    /* 0xA5: RESERVED                       */ M_NONE,
    /* 0xA6: RESERVED                       */ M_NONE,
    /* 0xA7: RESERVED                       */ M_NONE,
    /* 0xA8: RESERVED                       */ M_NONE,
    /* 0xA9: RESERVED                       */ M_NONE,
    /* 0xAA: RESERVED                       */ M_NONE,
    /* 0xAB: RESERVED                       */ M_NONE,
    /* 0xAC: RESERVED                       */ M_NONE,
    /* 0xAD: RESERVED                       */ M_NONE,
    /* 0xAE: RESERVED                       */ M_NONE,
    /* 0xAF: RESERVED                       */ M_NONE,
    /* 0xB0: KP_00 ('00')                   */ M_NONE,
    /* 0xB1: KP_000 ('000')                 */ M_NONE,
    /* 0xB2: THOUSANDS_SEPARATOR            */ M_NONE,
    /* 0xB3: DECIMAL_SEPARATOR              */ M_NONE,
    /* 0xB4: CURRENCY_UNIT                  */ M_NONE,
    /* 0xB5: CURRENCY_SUBUNIT               */ M_NONE,
    /* 0xB6: KP_LEFT_PAREN ('(')            */ M_NONE,
    /* 0xB7: KP_RIGHT_PAREN (')')           */ M_NONE,
    /* 0xB8: KP_LEFT_BRACE ('{')            */ M_NONE,
    /* 0xB9: KP_RIGHT_BRACE ('}')           */ M_NONE,
    /* 0xBA: KP_TAB                         */ M_NONE,
    /* 0xBB: KP_BACKSPACE                   */ M_NONE,
    /* 0xBC: KP_A ('A')                     */ M_NONE,
    /* 0xBD: KP_B ('B')                     */ M_NONE,
    /* 0xBE: KP_C ('C')                     */ M_NONE,
    /* 0xBF: KP_D ('D')                     */ M_NONE,
    /* 0xC0: KP_E ('E')                     */ M_NONE,
    /* 0xC1: KP_F ('F')                     */ M_NONE,
    /* 0xC2: KP_XOR                         */ M_NONE,
    /* 0xC3: KP_POWER ('^')                 */ M_NONE,
    /* 0xC4: KP_PERCENT ('%')               */ M_NONE,
    /* 0xC5: KP_LESS ('<')                  */ M_NONE,
    /* 0xC6: KP_GREATER ('>')               */ M_NONE,
    /* 0xC7: KP_AMPERSAND ('&')             */ M_NONE,
    /* 0xC8: KP_DOUBLE_AMPERSAND ('&&')     */ M_NONE,
    /* 0xC9: KP_VERTICAL_BAR ('|')          */ M_NONE,
    /* 0xCA: KP_DOUBLE_VERTICAL_BAR ('||')  */ M_NONE,
    /* 0xCB: KP_COLON (':')                 */ M_NONE,
    /* 0xCC: KP_HASH ('#')                  */ M_NONE,
    /* 0xCD: KP_SPACE (' ')                 */ M_NONE,
    /* 0xCE: KP_AT ('@')                    */ M_NONE,
    /* 0xCF: KP_EXCLAM ('!')                */ M_NONE,
    /* 0xD0: KP_MEMORY_STORE                */ M_NONE,
    /* 0xD1: KP_MEMORY_RECALL               */ M_NONE,
    /* 0xD2: KP_MEMORY_CLEAR                */ M_NONE,
    /* 0xD3: KP_MEMORY_ADD                  */ M_NONE,
    /* 0xD4: KP_MEMORY_SUBTRACT             */ M_NONE,
    /* 0xD5: KP_MEMORY_MULTIPLY             */ M_NONE,
    /* 0xD6: KP_MEMORY_DIVIDE               */ M_NONE,
    /* 0xD7: KP_PLUS_MINUS ('+/-')          */ M_NONE,
    /* 0xD8: KP_CLEAR                       */ M_NONE,
    /* 0xD9: KP_CLEAR_ENTRY                 */ M_NONE,
    /* 0xDA: KP_BINARY                      */ M_NONE,
    /* 0xDB: KP_OCTAL                       */ M_NONE,
    /* 0xDC: KP_DECIMAL                     */ M_NONE,
    /* 0xDD: KP_HEXADECIMAL                 */ M_NONE,
    /* 0xDE: RESERVED                       */ M_NONE,
    /* 0xDF: RESERVED                       */ M_NONE,
    /* 0xE0: CONTROL_LEFT                   */ M_NONE,
    /* 0xE1: SHIFT_LEFT                     */ M(8,0),
    /* 0xE2: ALT_LEFT                       */ M_NONE,
    /* 0xE3: GUI_LEFT                       */ M_NONE,
    /* 0xE4: CONTROL_RIGHT                  */ M_NONE,
    /* 0xE5: SHIFT_RIGHT                    */ M(8,5),
    /* 0xE6: ALT_RIGHT                      */ M_NONE,
    /* 0xE7: GUI_RIGHT                      */ M_NONE,
};

static bool find_key_in_report(hid_keyboard_report_t const* report, uint8_t keycode) {
    for (uint8_t i = 0; i < 6; i++) {
        if (report->keycode[i] == keycode) {
            return true;
        }
    }

    return false;
}

void key_down(uint8_t keycode) {
    uint8_t const* row_and_col = s_hidToKeyMatrix[keycode];
    uint8_t row = row_and_col[0];
    uint8_t col = row_and_col[1];

    if (col != 0 && (key_matrix[row] & col)) {
        key_matrix[row] &= ~col;
        printf("USB: Key down: %d=(%d,%d)\n", keycode, row, col);
    }
}

void key_up(uint8_t keycode) {
    uint8_t const* row_and_col = s_hidToKeyMatrix[keycode];
    uint8_t row = row_and_col[0];
    uint8_t col = row_and_col[1];

    if (col != 0 && !(key_matrix[row] & col)) {
        key_matrix[row] |= col;
        printf("USB: Key up: %d=(%d,%d)\n", keycode, row, col);
    }
}

void process_kbd_report(hid_keyboard_report_t const* report) {
    static hid_keyboard_report_t prev_report = {0, 0, {0}};

    uint8_t current_modifiers = report->modifier;
    uint8_t previous_modifiers = prev_report.modifier;

    for (uint8_t i = 0; i < 8; i++) {
        uint8_t current_modifier = current_modifiers & 0x01;
        uint8_t previous_modifier = previous_modifiers & 0x01;

        if (current_modifier) {
            if (!previous_modifier) {
                key_down(HID_KEY_CONTROL_LEFT + i);
            }
        } else if (previous_modifier) {
            key_up(HID_KEY_CONTROL_LEFT + i);
        }

        current_modifiers >>= 1;
        previous_modifiers >>= 1;
    }

    for (uint8_t i = 0; i < 6; i++) {
        uint8_t keycode = prev_report.keycode[i];
        if (keycode && !find_key_in_report(report, keycode)) {
            key_up(keycode);
        }
    }

    for (uint8_t i = 0; i < 6; i++) {
        uint8_t keycode = report->keycode[i];
        if (keycode && !find_key_in_report(&prev_report, keycode)) {
            key_down(keycode);
        }
    }

    prev_report = *report;
}
