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

#include "pch.h"

typedef uint8_t key_event_t;

// Sentinel for "no key event". (Note that column '0xF' exceeds KEY_COL_COUNT)
const extern key_event_t key_event_none;

key_event_t key_event(bool pressed, uint8_t row, uint8_t col);

key_event_t next_key_event(const uint8_t* matrix);
