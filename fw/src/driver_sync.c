// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "driver.h"

#include "input/keyboard_core.h"
#include "system_state.h"

/** Exchange input and video state each input iteration, using the driver's SPI boundary. */
void sync_state(void) {
    // Publish routed ownership before reading the CPU-observed physical matrix.
    spi_write(ECONOPET_WB_KBD_BASE_ADDR, usb_key_matrix, ECONOPET_KBD_COL_COUNT);
    spi_read(ECONOPET_WB_KBD_BASE_ADDR, ECONOPET_KBD_COL_COUNT, pet_key_matrix);

    // Refresh display timing and status from the same FPGA observation cycle.
    spi_read(ECONOPET_WB_CRTC_BASE_ADDR, CRTC_REG_COUNT, system_state.pet_crtc_registers);
    const uint8_t status = spi_read_at(ECONOPET_WB_STATUS_ADDR);
    system_state.video_text_mode = (status & ECONOPET_REG_STATUS_TEXT_MODE_MASK) != 0
        ? video_text_mode_lower : video_text_mode_graphics;
    system_state.bp_halted = (status & ECONOPET_REG_STATUS_BP_HALT_MASK) != 0;
}
