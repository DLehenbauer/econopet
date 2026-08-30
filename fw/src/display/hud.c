// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "hud.h"

#include "char_encoding.h"
#include "driver.h"
#include "system_state.h"

// HUD device address map. Must match WB_HUD_BASE / the control-register layout
// in gw/EconoPET/src/common_pkg.sv + hud.sv. The 5-bit base (0b01111) occupies
// wishbone address bits [19:15]; bit 8 selects the control registers, and bits
// [7:0] index the 256-byte text buffer.
#define ADDR_HUD        (0x0Fu << 15)          // 0x78000
#define HUD_CTRL_FLAG   0x100u
#define HUD_REG_CTRL    (ADDR_HUD | HUD_CTRL_FLAG | 0u)   // bit0 = enable overlay
#define HUD_REG_OFF_LO  (ADDR_HUD | HUD_CTRL_FLAG | 1u)   // VRAM offset [7:0]
#define HUD_REG_OFF_HI  (ADDR_HUD | HUD_CTRL_FLAG | 2u)   // VRAM offset [10:8]
#define HUD_REG_LEN     (ADDR_HUD | HUD_CTRL_FLAG | 3u)   // characters to overlay

static bool     active = false;
// Copy of the current overlay, so the HDMI path can paint the same content
// (the fabric substitution is CRT-scan-out only).
static uint8_t  cur_codes[HUD_MAX_LEN];
static uint16_t cur_off = 0;
static size_t   cur_len = 0;

void hud_init(void) {
    active = false;
    cur_len = 0;
    spi_write_at(HUD_REG_CTRL, 0x00);   // ensure the overlay starts disabled
}

bool hud_is_active(void) {
    return active;
}

void hud_show_codes(uint16_t off, const uint8_t* codes, size_t len) {
    if (len == 0) {
        hud_hide();
        return;
    }
    if (len > HUD_MAX_LEN) {
        len = HUD_MAX_LEN;
    }

    // Load the buffer, set placement, then enable.
    spi_write(ADDR_HUD, codes, len);
    spi_write_at(HUD_REG_OFF_LO, (uint8_t)(off & 0xFF));
    spi_write_at(HUD_REG_OFF_HI, (uint8_t)((off >> 8) & 0x07));
    spi_write_at(HUD_REG_LEN, (uint8_t)len);
    spi_write_at(HUD_REG_CTRL, 0x01);

    // Keep a copy so the HDMI path can paint the same overlay.
    memcpy(cur_codes, codes, len);
    cur_off = off;
    cur_len = len;
    active = true;
}

void hud_paint_hdmi(uint8_t* buf, size_t buf_bytes) {
    // Composite the current overlay into the firmware's HDMI video buffer,
    // which mirrors VRAM linearly (buf[i] == VRAM byte i). The fabric handles
    // the CRT; this gives HDMI the same overlay. No-op when inactive.
    if (!active) {
        return;
    }
    for (size_t i = 0; i < cur_len && (size_t)(cur_off + i) < buf_bytes; i++) {
        buf[cur_off + i] = cur_codes[i];
    }
}

size_t hud_show_text(uint8_t row, uint8_t col, const char* s) {
    const size_t cols = (size_t)system_state.pet_display_columns;   // 40 or 80
    if (col >= cols) {
        return 0;
    }

    size_t maxlen = cols - col;
    if (maxlen > HUD_MAX_LEN) {
        maxlen = HUD_MAX_LEN;
    }

    uint8_t codes[HUD_MAX_LEN];
    size_t n = 0;
    while (s[n] != '\0' && n < maxlen) {
        // Charset-aware, matching display.c: the SuperPET charset keeps
        // lowercase at ASCII positions; everything else via ascii_to_vrom.
        // Bit 7 = reverse video so the overlay stands out.
        uint8_t ch = (uint8_t)s[n];
        uint8_t code = (system_state.superpet_charset && ch >= 'a' && ch <= 'z')
            ? ch
            : ascii_to_vrom(ch);
        codes[n] = code | 0x80;
        n++;
    }

    if (n == 0) {
        return 0;
    }

    uint16_t off = (uint16_t)(row * cols + col);
    hud_show_codes(off, codes, n);
    return n;
}

void hud_hide(void) {
    if (!active) {
        return;
    }
    spi_write_at(HUD_REG_CTRL, 0x00);
    active = false;
}
