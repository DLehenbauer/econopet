// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "system/system.h"

#include "breakpoint.h"
#include "diag/mem.h"
#include "display/display.h"
#include "display/window.h"
#include "driver.h"
#include "fatal.h"
#include "input/pet_keyboard.h"
#include "pet.h"
#include "reset.h"
#include "roms/roms.h"
#include "system/state.h"

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
#define CONSOLE_SCREEN_BYTES VIDEO_PLANE_CAPACITY
#define CONSOLE_COLOR_BYTES VIDEO_RAM_UNIT_BYTES
#define CONSOLE_FONT_BYTES 0x800u

// Only initialized CRTC fields used for console geometry are borrowed.
static const uint8_t crtc_indices[] = {0, 1, 2, 3, 4, 5, 6, 7, 9, 12, 13};
static const uint8_t console_crtc[] = {0x31, 0x28, 0x29, 0x0f, 0x31, 0, 0x19, 0x25, 7, 0x10, 0};

typedef enum {
    CONSOLE_BORROW_INACTIVE,
    CONSOLE_BORROW_ENTERING,
    CONSOLE_BORROW_ACTIVE,
} console_borrow_state_t;

static system_operation_t mode = system_operation_boot;
static struct {
    console_borrow_state_t state;
    uint8_t screen[CONSOLE_SCREEN_BYTES], colors[CONSOLE_COLOR_BYTES], font[CONSOLE_FONT_BYTES];
    uint8_t physical_colors[CONSOLE_COLOR_BYTES];
    uint8_t crtc[sizeof(crtc_indices)], host_crtc[CRTC_REG_COUNT];
    uint8_t cra, crb, ddra, ddrb, ora, pcr, video, audio, cpu, selection;
    video_source_t source;
    video_text_mode_t text;
    pet_display_columns_t columns;
    uint8_t video_mask;
} console_borrow;

/** Capture DDRs through their selected registers without driving input pins to inspect latches. */
static void save_pia(void) {
    console_borrow.cra = pet_io_read(PIA1_CONTROL_A) & PIA_CONTROL_WRITABLE;
    console_borrow.crb = pet_io_read(PIA1_CONTROL_B) & PIA_CONTROL_WRITABLE;
    pet_io_write(PIA1_CONTROL_A, console_borrow.cra & ~PIA_DATA_SELECT);
    console_borrow.ddra = pet_io_read(PIA1_PORT_A);
    pet_io_write(PIA1_CONTROL_B, console_borrow.crb & ~PIA_DATA_SELECT);
    console_borrow.ddrb = pet_io_read(PIA1_PORT_B);
    pet_io_write(PIA1_CONTROL_A, console_borrow.cra | PIA_DATA_SELECT);
    console_borrow.ora = pet_io_read(PIA1_PORT_A);
    pet_io_write(PIA1_CONTROL_A, console_borrow.cra);
    pet_io_write(PIA1_CONTROL_B, console_borrow.crb);
}

/** Borrow only keyboard direction/data-selection fields, leaving ORB and C2 modes alone. */
static void setup_pia(void) {
    pet_io_write(PIA1_CONTROL_A, console_borrow.cra & ~PIA_DATA_SELECT);
    pet_io_write(PIA1_PORT_A, console_borrow.ddra | KEY_ROW_MASK);
    pet_io_write(PIA1_CONTROL_B, console_borrow.crb & ~PIA_DATA_SELECT);
    pet_io_write(PIA1_PORT_B, 0);
    pet_io_write(PIA1_CONTROL_A, console_borrow.cra | PIA_DATA_SELECT);
    pet_io_write(PIA1_CONTROL_B, console_borrow.crb | PIA_DATA_SELECT);
}

/** Restore observable ORA before returning guest directions and register selection. */
static void restore_pia(void) {
    pet_io_write(PIA1_CONTROL_A, console_borrow.cra | PIA_DATA_SELECT);
    pet_io_write(PIA1_PORT_A, console_borrow.ora);
    pet_io_write(PIA1_CONTROL_A, console_borrow.cra & ~PIA_DATA_SELECT);
    pet_io_write(PIA1_PORT_A, console_borrow.ddra);
    pet_io_write(PIA1_CONTROL_B, console_borrow.crb & ~PIA_DATA_SELECT);
    pet_io_write(PIA1_PORT_B, console_borrow.ddrb);
    pet_io_write(PIA1_CONTROL_A, console_borrow.cra);
    pet_io_write(PIA1_CONTROL_B, console_borrow.crb);
}

/** Retire a completed or partial console borrow without restoring its snapshot. */
static void abandon_console(void) {
    if (console_borrow.state == CONSOLE_BORROW_INACTIVE) {
        return;
    }
    pet_keyboard_end();
    // Leave coordinated fabric/firmware reset to the terminal path.
    set_cpu(CPU_HALT);
    console_borrow.state = CONSOLE_BORROW_INACTIVE;
}

void system_init(void) {
    mode = system_operation_boot;
    console_borrow.state = CONSOLE_BORROW_INACTIVE;
}

system_operation_t system_mode(void) {
    return mode;
}

bool system_suspend(void) {
    if (mode == system_operation_diagnostic ||
        mode == system_operation_console ||
        console_borrow.state != CONSOLE_BORROW_INACTIVE) {
        return false;
    }
    console_borrow.state = CONSOLE_BORROW_ENTERING;

    // Acquire CPU ownership without rewriting the guest control or breakpoint latch.
    console_borrow.cpu = get_cpu();
    console_borrow.selection = spi_read_at(ECONOPET_WB_CPU_SEL_ADDR);
    console_borrow.audio =
        spi_read_at(ECONOPET_WB_AUDIO_ADDR) & ECONOPET_REG_AUDIO_ENABLE_MASK;
    spi_write_at(ECONOPET_WB_AUDIO_ADDR, 0);
    set_cpu((cpu_state_t)(console_borrow.cpu & ~CPU_READY));

    // Save physical bytes, not stale host pixels or the replacement font.
    spi_read(VIDEO_RAM_ADDRESS, sizeof(console_borrow.screen), console_borrow.screen);
    memcpy(console_borrow.colors,
           system_state.video_char_buffer + VIDEO_COLOR_OFFSET,
           sizeof(console_borrow.colors));
    spi_read(ECONOPET_WB_BRAM_BASE_ADDR, sizeof(console_borrow.font), console_borrow.font);
    console_borrow.video = spi_read_at(ECONOPET_WB_VIDEO_ADDR);
    console_borrow.pcr = pet_io_read(VIA_PCR);
    save_pia();
    console_borrow.source = system_state.video_source;
    console_borrow.text = system_state.video_text_mode;
    console_borrow.columns = system_state.pet_display_columns;
    console_borrow.video_mask = system_state.video_ram_mask;
    if (console_borrow.video_mask == pet_video_ram_mask_colourpet) {
        spi_read(VIDEO_COLOR_ADDRESS,
                 sizeof(console_borrow.physical_colors), console_borrow.physical_colors);
    }
    memcpy(console_borrow.host_crtc,
           system_state.pet_crtc_registers, sizeof(console_borrow.host_crtc));
    for (size_t index = 0; index < sizeof(crtc_indices); ++index) {
        console_borrow.crtc[index] =
            spi_read_at(ECONOPET_WB_CRTC_BASE_ADDR + crtc_indices[index]);
    }

    // Install the console policy only after the complete snapshot is valid.
    setup_pia();
    pet_io_write(VIA_PCR, (console_borrow.pcr & ~VIA_CA2_MASK) | VIA_CA2_HIGH);
    spi_write(ECONOPET_WB_BRAM_BASE_ADDR, rom_chars_e800, CONSOLE_FONT_BYTES);
    roms_refresh_char_rom();
    spi_write_at(ECONOPET_WB_VIDEO_ADDR, 0);
    for (size_t index = 0; index < sizeof(crtc_indices); ++index) {
        spi_write_at(ECONOPET_WB_CRTC_BASE_ADDR + crtc_indices[index],
                     console_crtc[index]);
        system_state.pet_crtc_registers[crtc_indices[index]] = console_crtc[index];
    }
    system_state.pet_display_columns = pet_display_columns_40;
    system_state_set_video_ram_mask(&system_state, pet_video_ram_mask_1kb);
    system_state.video_text_mode = video_text_mode_lower;
    system_state.video_source = video_source_firmware;
    memset(system_state.video_char_buffer, CH_SPACE, CONSOLE_SCREEN_BYTES);
    memset(system_state.video_char_buffer + VIDEO_COLOR_OFFSET,
           VIDEO_DEFAULT_ATTRIBUTE, CONSOLE_COLOR_BYTES);
    pet_keyboard_begin(console_borrow.ora);
    console_borrow.state = CONSOLE_BORROW_ACTIVE;
    mode = system_operation_console;
    return true;
}

void system_restore(const bool resume) {
    vet(mode == system_operation_console,
        "restoring firmware console outside console operation");
    vet(console_borrow.state == CONSOLE_BORROW_ACTIVE,
        "restoring incomplete firmware console borrow");
    pet_keyboard_end();

    // Keep the guest suspended until physical bytes and renderer state agree.
    system_state.video_source = video_source_pet;
    spi_write(VIDEO_RAM_ADDRESS,
              console_borrow.screen, sizeof(console_borrow.screen));
    if (console_borrow.video_mask == pet_video_ram_mask_colourpet) {
        spi_write(VIDEO_COLOR_ADDRESS,
                  console_borrow.physical_colors,
                  sizeof(console_borrow.physical_colors));
    }
    spi_write(ECONOPET_WB_BRAM_BASE_ADDR,
              console_borrow.font, sizeof(console_borrow.font));
    roms_refresh_char_rom();
    for (size_t index = 0; index < sizeof(crtc_indices); ++index) {
        spi_write_at(ECONOPET_WB_CRTC_BASE_ADDR + crtc_indices[index],
                     console_borrow.crtc[index]);
    }
    spi_write_at(ECONOPET_WB_VIDEO_ADDR, console_borrow.video);
    restore_pia();
    pet_io_write(VIA_PCR, console_borrow.pcr);
    memcpy(system_state.video_char_buffer,
           console_borrow.screen, sizeof(console_borrow.screen));
    memcpy(system_state.video_char_buffer + VIDEO_COLOR_OFFSET,
           console_borrow.colors, sizeof(console_borrow.colors));
    memcpy(system_state.pet_crtc_registers,
           console_borrow.host_crtc, sizeof(console_borrow.host_crtc));
    system_state.pet_display_columns = console_borrow.columns;
    system_state_set_video_ram_mask(&system_state, console_borrow.video_mask);
    system_state.video_text_mode = console_borrow.text;
    system_state.video_source = console_borrow.source;
    vet(get_cpu() == (cpu_state_t)(console_borrow.cpu & ~CPU_READY) &&
            spi_read_at(ECONOPET_WB_CPU_SEL_ADDR) == console_borrow.selection,
        "guest control changed during firmware console");

    if (resume) {
        set_cpu((cpu_state_t)console_borrow.cpu);
        mode = system_operation_guest;
    }
    spi_write_at(ECONOPET_WB_AUDIO_ADDR, console_borrow.audio);
    console_borrow.state = CONSOLE_BORROW_INACTIVE;
}

void system_reset_guest(void) {
    system_state.video_source = video_source_pet;
    pet_reset();
    mode = system_operation_guest;
}

void system_takeover(void) {
    if (mode == system_operation_diagnostic) {
        return;
    }
    mode = system_operation_diagnostic;
    abandon_console();
}

void __attribute__((noreturn)) system_reset(void) {
    system_takeover();
    mcu_reset();
    __builtin_unreachable();
}

void __attribute__((noreturn)) system_memory_diagnostic(void) {
    system_takeover();
    pet_wait_for_storage_idle();
    set_cpu(CPU_HALT);
    test_ram();
    __builtin_unreachable();
}

void system_task(void) {
    if (mode != system_operation_guest) {
        return;
    }
    bp_task();
    pet_task();
}
