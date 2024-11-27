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

#include <dirent.h>
#include "filesystem/vfs.h"

#include "../driver.h"
#include "../hw.h"
#include "../sd/sd.h"
#include "../video/video.h"
#include "menu.h"

const uint8_t screen_width  = 40;
const uint8_t screen_height = 25;

#define CH_SPACE 0x20

// Lower-case (POKE 59468,14)
// TODO: _`{}|~
const uint8_t ascii_to_vrom[128] = {
    /* NUL */ 0x00, /* SOH */ 0x01, /* STX */ 0x02, /* ETX */ 0x03, /* EOT */ 0x04, /* ENQ */ 0x05, /* ACK */ 0x06, /* BEL */ 0x07,
    /*  BS */ 0x08, /*  HT */ 0x09, /*  LF */ 0x0A, /*  VT */ 0x0B, /*  FF */ 0x0C, /*  CR */ 0x0D, /*  SO */ 0x0E, /*  SI */ 0x0F,
    /* DLE */ 0x10, /* DC1 */ 0x11, /* DC2 */ 0x12, /* DC3 */ 0x13, /* DC4 */ 0x14, /* NAK */ 0x15, /* SYN */ 0x16, /* ETB */ 0x17,
    /* CAN */ 0x18, /*  EM */ 0x19, /* SUB */ 0x1A, /* ESC */ 0x1B, /*  FS */ 0x1C, /*  GS */ 0x1D, /*  RS */ 0x1E, /*  US */ 0x1F,
    /*  SP */ 0x20, /*   ! */ 0x21, /*   " */ 0x22, /*   # */ 0x23, /*   $ */ 0x24, /*   % */ 0x25, /*   & */ 0x26, /*   ' */ 0x27,
    /*   ( */ 0x28, /*   ) */ 0x29, /*   * */ 0x2A, /*   + */ 0x2B, /*   , */ 0x2C, /*   - */ 0x2D, /*   . */ 0x2E, /*   / */ 0x2F,
    /*   0 */ 0x30, /*   1 */ 0x31, /*   2 */ 0x32, /*   3 */ 0x33, /*   4 */ 0x34, /*   5 */ 0x35, /*   6 */ 0x36, /*   7 */ 0x37,
    /*   8 */ 0x38, /*   9 */ 0x39, /*   : */ 0x3A, /*   ; */ 0x3B, /*   < */ 0x3C, /*   = */ 0x3D, /*   > */ 0x3E, /*   ? */ 0x3F,
    /*   @ */ 0x00, /*   A */ 0x41, /*   B */ 0x42, /*   C */ 0x43, /*   D */ 0x44, /*   E */ 0x45, /*   F */ 0x46, /*   G */ 0x47,
    /*   H */ 0x48, /*   I */ 0x49, /*   J */ 0x4A, /*   K */ 0x4B, /*   L */ 0x4C, /*   M */ 0x4D, /*   N */ 0x4E, /*   O */ 0x4F,
    /*   P */ 0x50, /*   Q */ 0x51, /*   R */ 0x52, /*   S */ 0x53, /*   T */ 0x54, /*   U */ 0x55, /*   V */ 0x56, /*   W */ 0x57,
    /*   X */ 0x58, /*   Y */ 0x59, /*   Z */ 0x5A, /*   [ */ 0x1B, /*   \ */ 0x1C, /*   ] */ 0x1D, /*   ^ */ 0x1E, /*   _ */ 0x64,
    /*   ` */ 0x27, /*   a */ 0x01, /*   b */ 0x02, /*   c */ 0x03, /*   d */ 0x04, /*   e */ 0x05, /*   f */ 0x06, /*   g */ 0x07,
    /*   h */ 0x08, /*   i */ 0x09, /*   j */ 0x0A, /*   k */ 0x0B, /*   l */ 0x0C, /*   m */ 0x0D, /*   n */ 0x0E, /*   o */ 0x0F,
    /*   p */ 0x10, /*   q */ 0x11, /*   r */ 0x12, /*   s */ 0x13, /*   t */ 0x14, /*   u */ 0x15, /*   v */ 0x16, /*   w */ 0x17,
    /*   x */ 0x18, /*   y */ 0x19, /*   z */ 0x1A, /*   { */ 0x6B, /*   | */ 0x5B, /*   } */ 0x73, /*   ~ */ 0x71, /* DEL */ 0x7F,
};

uint16_t screen_offset(uint8_t x, uint8_t y) {
    uint16_t offset = y;
    offset *= screen_width;
    offset += x;
    return offset;
}

void screen_clear() {
    for (uint16_t i = 0; i < screen_width * screen_height; i++) {
        video_char_buffer[i] = CH_SPACE;
    }
}

void screen_print(uint8_t x, uint8_t y, const char* str, bool reverse) {
    uint16_t offset = screen_offset(x, y);
    const uint8_t* pCh = (uint8_t*) str;
    if (reverse) {
        while (*pCh != 0) {
            video_char_buffer[offset++] = (ascii_to_vrom[*(pCh++)] | 0x80);
        }
    } else {
        while (*pCh != 0) {
            video_char_buffer[offset++] = ascii_to_vrom[*(pCh++)];
        }
    }
}

void directory() {
    screen_clear();

    DIR* dir = opendir("/");

    printf("opendir\n");
    fflush(stdout);
    uart_default_tx_wait_blocking();

    if (dir == NULL) {
        screen_print(0, 0, "Error opening directory", false);
        return;
    }

    uint y = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        printf("%s\n", entry->d_name);
        fflush(stdout);
        uart_default_tx_wait_blocking();

        screen_print(0, y++, entry->d_name, false);
    }

    closedir(dir);
    printf("closedir\n");
    fflush(stdout);
    uart_default_tx_wait_blocking();
}

bool menu_is_pressed() {
    return !gpio_get(MENU_BTN_GP);
}

void menu_init_start() {
    gpio_init(MENU_BTN_GP);

    // To accelerate charging the debouncing capacitor, we briefly drive the pin as an output
    gpio_set_dir(MENU_BTN_GP, GPIO_OUT);
    gpio_put(MENU_BTN_GP, 1);
}

void menu_init_end() {
    // Enable pull-up resistor as the button is active low
    gpio_pull_up(MENU_BTN_GP);
    gpio_set_dir(MENU_BTN_GP, GPIO_IN);
}

bool menu_task() {
    if (!menu_is_pressed()) {
        return false;
    }

    // Wait for menu release
    while (menu_is_pressed());

    // CPU is suspended/reset while handling menu
    set_cpu(/* ready */ false, /* reset */ true);

    directory();

    // Wait for another press/release before returning
    while (!menu_is_pressed());
    while (menu_is_pressed());

    return true;
}