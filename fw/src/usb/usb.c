// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "usb.h"

#include "diag/log/log.h"

void usb_init() {
    // Something in 'board_init()' interrupts the UART, losing characters pending in the FIFO.
    // Wait for the FIFO to empty before continuing.
    fflush(stdout);
    uart_default_tx_wait_blocking();

    board_init();

    // init host stack on configured roothub port
    tuh_init(BOARD_TUH_RHPORT);

    log_info("USB initialized");
}

void tuh_mount_cb(uint8_t dev_addr) {
    log_info("USB: Mounted device with address %d", dev_addr);
}

void tuh_umount_cb(uint8_t dev_addr) {
    log_info("USB: Unmounted device with address %d", dev_addr);
}
