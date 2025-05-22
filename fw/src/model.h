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

#pragma once

typedef enum {
    model_flag_none     = 0,
    model_flag_crtc     = 1 << 0,   // Flag set if the model has a CRTC (12"/20kHz display)
    model_flag_business = 1 << 1,   // Flag set if the model has a business keyboard
} model_flags_t;
