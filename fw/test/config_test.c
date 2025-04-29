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

#include "../src/menu/menu_config.h"

#define COLS 40
#define ROWS 25
#define BUFFER_SIZE (COLS * ROWS)
static uint8_t buffer[BUFFER_SIZE] = {0};

void config_test() {
    menu_config_show(buffer, COLS, ROWS);
}
