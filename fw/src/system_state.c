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

#include "system_state.h"

system_state_t system_state = { 0 };

void system_state_set_video_ram_mask(system_state_t* state, uint8_t video_ram_mask) {
    state->video_ram_mask = video_ram_mask;
    state->video_ram_bytes = (size_t)(video_ram_mask + 1) * 1024u;
}
