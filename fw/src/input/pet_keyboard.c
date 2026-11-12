// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "pet_keyboard.h"

#include "driver.h"
#include "usb/keyboard.h"
#include "usb/keyscan.h"

#define PIA1_ORA_ADDR 0xe810u
#define PIA1_CRA_ADDR 0xe811u
#define PIA1_ORB_ADDR 0xe812u
#define PIA1_CRB_ADDR 0xe813u
#define BOOT_PIA_CONTROL 0x3cu
#define BOOT_RETRACE_ENABLE 0x01u
#define PIA_DATA_SELECT 0x04u
#define ROW_MASK 0x0fu
// Conservative initial dwell, to be confirmed on fitted rev-a keyboards.
#define ROW_SETTLE_US 100u
#define STABLE_RETRIES 3u

static struct {
    bool active, selected, sampled, ready, unstable, released;
    uint8_t row, base, previous, retries;
    uint8_t matrix[KEY_COL_COUNT];
    uint64_t deadline;
} scanner;

void pet_keyboard_sync(void) {
    if (scanner.active) {
        return;
    }

    spi_read(ECONOPET_WB_KBD_BASE_ADDR, KEY_COL_COUNT, pet_key_matrix);
}

void pet_keyboard_begin(const uint8_t saved_ora) {
    scanner = (typeof(scanner)){.active = true, .base = saved_ora & ~ROW_MASK};
    memset(scanner.matrix, 0xff, sizeof(scanner.matrix));
}

void pet_keyboard_begin_boot(void) {
    pet_io_write(PIA1_CRA_ADDR, BOOT_PIA_CONTROL & ~PIA_DATA_SELECT);
    pet_io_write(PIA1_ORA_ADDR, ROW_MASK);
    pet_io_write(PIA1_CRB_ADDR, BOOT_PIA_CONTROL & ~PIA_DATA_SELECT);
    pet_io_write(PIA1_ORB_ADDR, 0);
    pet_io_write(PIA1_CRA_ADDR, BOOT_PIA_CONTROL);
    pet_io_write(PIA1_CRB_ADDR, BOOT_PIA_CONTROL | BOOT_RETRACE_ENABLE);
    pet_keyboard_begin(pet_io_read(PIA1_ORA_ADDR));
}

void pet_keyboard_task(void) {
    if (!scanner.active) {
        return;
    }
    // Select one physical row without changing unrelated observable port-A outputs.
    if (!scanner.selected) {
        pet_io_write(PIA1_ORA_ADDR, scanner.base | scanner.row);
        scanner.selected = true;
        scanner.deadline = time_us_64() + ROW_SETTLE_US;
        return;
    }
    if (time_us_64() < scanner.deadline) {
        return;
    }

    // Require two matching samples and bound unstable-row retries.
    const uint8_t sample = pet_io_read(PIA1_ORB_ADDR);
    if (!scanner.sampled || sample != scanner.previous) {
        scanner.previous = sample;
        scanner.sampled = true;
        scanner.deadline = time_us_64() + ROW_SETTLE_US;
        if (++scanner.retries < STABLE_RETRIES) {
            return;
        }
        scanner.unstable = true;
    } else {
        scanner.matrix[scanner.row] = sample;
    }
    scanner.selected = scanner.sampled = false;
    scanner.retries = 0;

    // Publish only complete matrices, never a mixture of different scan rounds.
    if (++scanner.row == KEY_COL_COUNT) {
        memcpy(pet_key_matrix, scanner.matrix, sizeof(scanner.matrix));
        scanner.row = 0;
        scanner.ready = true;
        scanner.released = !scanner.unstable;
        for (size_t row = 0; row < KEY_COL_COUNT; ++row) {
            scanner.released &= scanner.matrix[row] == 0xff;
        }
        scanner.unstable = false;
    }
}

void pet_keyboard_end(void) { scanner.active = false; }
bool pet_keyboard_active(void) { return scanner.active; }
bool pet_keyboard_ready(void) { return scanner.ready; }
void pet_keyboard_wait_release_begin(void) {
    // Restart at row zero so no pre-exit observation can release the guest.
    scanner.selected = scanner.sampled = scanner.ready = scanner.released = scanner.unstable = false;
    scanner.row = scanner.retries = 0;
}
bool pet_keyboard_released(void) { return scanner.ready && scanner.released; }
