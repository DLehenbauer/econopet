// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "menu_session.h"

#include "display/dvi/dvi.h"
#include "driver.h"
#include "fatal.h"
#include "input/pet_keyboard.h"
#include "roms/roms.h"
#include "system_state.h"

#define PIA1_PORT_A 0xe810u
#define PIA1_CONTROL_A 0xe811u
#define PIA1_PORT_B 0xe812u
#define PIA1_CONTROL_B 0xe813u
#define PIA_CONTROL_WRITABLE 0x3fu
#define PIA_DATA_SELECT 0x04u
#define KEY_ROW_MASK 0x0fu
#define VIA_PCR 0xe84cu
#define VIA_CA2_MASK 0x0eu
#define VIA_CA2_HIGH 0x0eu

// Only initialized CRTC fields used for menu geometry are borrowed.
static const uint8_t crtc_indices[] = {0, 1, 2, 3, 4, 5, 6, 7, 9, 12, 13};
static const uint8_t menu_crtc[] = {0x31, 0x28, 0x29, 0x0f, 0x31, 0, 0x19, 0x25, 7, 0x10, 0};
static struct {
    bool active, suspended, saved;
    uint8_t screen[MENU_SCREEN_BYTES], font[MENU_FONT_BYTES];
    uint8_t crtc[sizeof(crtc_indices)], host_crtc[CRTC_REG_COUNT];
    uint8_t cra, crb, ddra, ddrb, ora, pcr, video, audio, cpu, selection;
    video_source_t source;
    video_text_mode_t text;
    pet_display_columns_t columns;
    uint8_t video_mask;
} session;

bool menu_session_active(void) { return session.active; }

/** Capture DDRs through their selected registers without driving input pins to inspect latches. */
static void save_pia(void) {
    session.cra = pet_io_read(PIA1_CONTROL_A) & PIA_CONTROL_WRITABLE;
    session.crb = pet_io_read(PIA1_CONTROL_B) & PIA_CONTROL_WRITABLE;
    pet_io_write(PIA1_CONTROL_A, session.cra & ~PIA_DATA_SELECT);
    session.ddra = pet_io_read(PIA1_PORT_A);
    pet_io_write(PIA1_CONTROL_B, session.crb & ~PIA_DATA_SELECT);
    session.ddrb = pet_io_read(PIA1_PORT_B);
    pet_io_write(PIA1_CONTROL_A, session.cra | PIA_DATA_SELECT);
    session.ora = pet_io_read(PIA1_PORT_A);
    pet_io_write(PIA1_CONTROL_A, session.cra);
    pet_io_write(PIA1_CONTROL_B, session.crb);
}

/** Borrow only keyboard direction/data-selection fields, leaving ORB and C2 modes alone. */
static void setup_pia(void) {
    pet_io_write(PIA1_CONTROL_A, session.cra & ~PIA_DATA_SELECT);
    pet_io_write(PIA1_PORT_A, session.ddra | KEY_ROW_MASK);
    pet_io_write(PIA1_CONTROL_B, session.crb & ~PIA_DATA_SELECT);
    pet_io_write(PIA1_PORT_B, 0);
    pet_io_write(PIA1_CONTROL_A, session.cra | PIA_DATA_SELECT);
    pet_io_write(PIA1_CONTROL_B, session.crb | PIA_DATA_SELECT);
}

/** Restore observable ORA before returning guest directions and register selection. */
static void restore_pia(void) {
    pet_io_write(PIA1_CONTROL_A, session.cra | PIA_DATA_SELECT);
    pet_io_write(PIA1_PORT_A, session.ora);
    pet_io_write(PIA1_CONTROL_A, session.cra & ~PIA_DATA_SELECT);
    pet_io_write(PIA1_PORT_A, session.ddra);
    pet_io_write(PIA1_CONTROL_B, session.crb & ~PIA_DATA_SELECT);
    pet_io_write(PIA1_PORT_B, session.ddrb);
    pet_io_write(PIA1_CONTROL_A, session.cra);
    pet_io_write(PIA1_CONTROL_B, session.crb);
}

bool menu_session_begin(void) {
    if (session.active) {
        return false;
    }
    session.active = true;
    session.saved = session.suspended = false;

    // Acquire CPU ownership without rewriting the guest control or breakpoint latch.
    session.cpu = get_cpu();
    session.selection = spi_read_at(ECONOPET_WB_CPU_SEL_ADDR);
    session.audio = spi_read_at(ECONOPET_WB_AUDIO_ADDR) & ECONOPET_REG_AUDIO_ENABLE_MASK;
    spi_write_at(ECONOPET_WB_AUDIO_ADDR, 0);
    if (!cpu_suspend(true)) {
        vet(cpu_suspend(false), "could not unwind CPU suspension");
        spi_write_at(ECONOPET_WB_AUDIO_ADDR, session.audio);
        session.active = false;
        return false;
    }
    session.suspended = true;
    vet(video_transition_begin(), "DVI did not acknowledge menu ownership");

    // Save physical bytes, not stale host pixels or a built-in replacement font.
    spi_read(MENU_SCREEN_ADDRESS, sizeof(session.screen), session.screen);
    spi_read(ECONOPET_WB_BRAM_BASE_ADDR, sizeof(session.font), session.font);
    session.video = spi_read_at(ECONOPET_WB_VIDEO_ADDR);
    session.pcr = pet_io_read(VIA_PCR);
    save_pia();
    session.source = system_state.video_source;
    session.text = system_state.video_text_mode;
    session.columns = system_state.pet_display_columns;
    session.video_mask = system_state.video_ram_mask;
    memcpy(session.host_crtc, system_state.pet_crtc_registers, sizeof(session.host_crtc));
    for (size_t index = 0; index < sizeof(crtc_indices); ++index) {
        session.crtc[index] = spi_read_at(ECONOPET_WB_CRTC_BASE_ADDR + crtc_indices[index]);
    }
    session.saved = true;

    // Set bounded menu geometry/font and borrow PIA1 only after the snapshot is valid.
    setup_pia();
    pet_io_write(VIA_PCR, (session.pcr & ~VIA_CA2_MASK) | VIA_CA2_HIGH);
    spi_write(ECONOPET_WB_BRAM_BASE_ADDR, rom_chars_e800, MENU_FONT_BYTES);
    roms_refresh_char_rom();
    spi_write_at(ECONOPET_WB_VIDEO_ADDR, 0);
    for (size_t index = 0; index < sizeof(crtc_indices); ++index) {
        spi_write_at(ECONOPET_WB_CRTC_BASE_ADDR + crtc_indices[index], menu_crtc[index]);
        system_state.pet_crtc_registers[crtc_indices[index]] = menu_crtc[index];
    }
    system_state.pet_display_columns = pet_display_columns_40;
    system_state_set_video_ram_mask(&system_state, pet_video_ram_mask_1kb);
    system_state.video_text_mode = video_text_mode_lower;
    system_state.video_source = video_source_firmware;
    video_set_menu_attributes(true);
    memset(system_state.video_char_buffer, 0x20, MENU_SCREEN_BYTES);
    pet_keyboard_begin(session.ora);
    vet(video_transition_end(), "DVI did not observe menu publication");
    return true;
}

void menu_session_restore(const bool resume) {
    vet(session.active && session.saved && session.suspended, "invalid menu restoration");
    pet_keyboard_end();
    vet(video_transition_begin(), "DVI did not acknowledge restoration");

    // Retain suspension/mute until physical bytes, borrowed I/O and renderer state agree.
    system_state.video_source = video_source_pet;
    spi_write(MENU_SCREEN_ADDRESS, session.screen, sizeof(session.screen));
    spi_write(ECONOPET_WB_BRAM_BASE_ADDR, session.font, sizeof(session.font));
    roms_refresh_char_rom();
    for (size_t index = 0; index < sizeof(crtc_indices); ++index) {
        spi_write_at(ECONOPET_WB_CRTC_BASE_ADDR + crtc_indices[index], session.crtc[index]);
    }
    spi_write_at(ECONOPET_WB_VIDEO_ADDR, session.video);
    restore_pia();
    pet_io_write(VIA_PCR, session.pcr);
    memcpy(system_state.video_char_buffer, session.screen, sizeof(session.screen));
    memcpy(system_state.pet_crtc_registers, session.host_crtc, sizeof(session.host_crtc));
    system_state.pet_display_columns = session.columns;
    system_state_set_video_ram_mask(&system_state, session.video_mask);
    system_state.video_text_mode = session.text;
    system_state.video_source = session.source;
    video_set_menu_attributes(false);
    vet(video_transition_end(), "DVI did not observe restored publication");
    vet(get_cpu() == session.cpu && spi_read_at(ECONOPET_WB_CPU_SEL_ADDR) == session.selection,
        "guest control changed during menu");

    // Retire resumability before destructive exits, never transiently executing the old guest.
    session.saved = false;
    if (resume) {
        vet(cpu_suspend(false), "CPU did not release menu suspension");
        spi_write_at(ECONOPET_WB_AUDIO_ADDR, session.audio);
        session.active = session.suspended = false;
    } else {
        spi_write_at(ECONOPET_WB_AUDIO_ADDR, session.audio);
    }
}

void menu_session_abandon(void) {
    if (!session.active) {
        return;
    }
    session.saved = false;
    pet_keyboard_end();
    set_cpu(CPU_RESET);
    vet(cpu_suspend(false), "could not hand menu suspension to reset");
    video_set_menu_attributes(false);
    session.active = session.suspended = false;
}
