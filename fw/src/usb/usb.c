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

#include "usb.h"

void usb_init() {
    // Something in 'board_init()' interrupts the UART, losing characters pending in the FIFO.
    // Wait for the FIFO to empty before continuing.
    fflush(stdout);
    uart_default_tx_wait_blocking();

    board_init();

    // init host stack on configured roothub port
    tuh_init(BOARD_TUH_RHPORT);

    printf("USB initialized\n");
}

void tuh_mount_cb(uint8_t dev_addr) {
    printf("USB: Mounted device with address %d\n", dev_addr);
}

void tuh_umount_cb(uint8_t dev_addr) {
    printf("USB: Unmounted device with address %d\n", dev_addr);
}
