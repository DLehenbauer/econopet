// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "driver.h"

#include "fatal.h"
#include "input/keyboard_core.h"
#include "input/pet_keyboard.h"
#include "system/state.h"

/** Exchange input and video state each input iteration, using the driver's SPI boundary. */
void sync_state(void) {
    // Publish routed ownership before reading the CPU-observed physical matrix.
    spi_write(ECONOPET_WB_KBD_BASE_ADDR, usb_key_matrix, ECONOPET_KBD_COL_COUNT);
    pet_keyboard_sync();

    // Refresh display timing and status from the same FPGA observation cycle.
    spi_read(ECONOPET_WB_CRTC_BASE_ADDR, CRTC_REG_COUNT, system_state.pet_crtc_registers);
    const uint8_t status = spi_read_at(ECONOPET_WB_STATUS_ADDR);
    system_state.video_text_mode = (status & ECONOPET_REG_STATUS_TEXT_MODE_MASK) != 0
        ? video_text_mode_lower : video_text_mode_graphics;
    system_state.bp_halted = (status & ECONOPET_REG_STATUS_BP_HALT_MASK) != 0;
}

/** Read the physical PET display and keyboard model from the status register. */
void read_pet_model(system_state_t* const state) {
    const uint8_t status = spi_read_at(ECONOPET_WB_STATUS_ADDR);
    state->pet_video_type =
        (pet_video_type_t)((status & ECONOPET_REG_STATUS_CRT_MASK) != 0);
    state->pet_keyboard_model =
        (pet_keyboard_model_t)((status & ECONOPET_REG_STATUS_KEYBOARD_MASK) != 0);
}

/** Write validated display columns and video RAM geometry to the FPGA. */
void write_pet_model(const system_state_t* const state) {
    const unsigned int video_ram_mask_max =
        ECONOPET_REG_VIDEO_RAM_MASK >> ECONOPET_REG_VIDEO_RAM_MASK_LO_BIT;
    vet(state->video_ram_mask <= video_ram_mask_max,
        "system_state.video_ram_mask (%u) exceeds hardware field maximum (%u)",
        state->video_ram_mask, video_ram_mask_max);
    vet(state->video_ram_bytes == (size_t)(state->video_ram_mask + 1) * 1024u,
        "system_state.video_ram_bytes (%zu) inconsistent with video_ram_mask (%u)",
        state->video_ram_bytes, state->video_ram_mask);

    uint8_t value = (uint8_t)(state->video_ram_mask << ECONOPET_REG_VIDEO_RAM_MASK_LO_BIT);
    if (state->pet_display_columns == pet_display_columns_80) {
        value |= ECONOPET_REG_VIDEO_COL_80_MASK;
    }
    spi_write_at(ECONOPET_WB_VIDEO_ADDR, value);
}
