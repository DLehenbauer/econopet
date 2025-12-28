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

#include "pch.h"
#include "keystate.h"

#define NUM_KEYSTATE_FLAGS 2
#define KEYSTATE_FLAG_MASK ((1 << NUM_KEYSTATE_FLAGS) - 1)
#define VECTOR_INDEX_SHIFT 4
#define BIT_INDEX_MASK 0xF

uint32_t keystate_vector[0x100 >> VECTOR_INDEX_SHIFT] = { 0 };

void get_location(uint32_t keycode, uint32_t* vector_index, uint32_t* bit_index) {
    *vector_index = (keycode >> VECTOR_INDEX_SHIFT);
    *bit_index    = (keycode & BIT_INDEX_MASK);
}

KeyStateFlags keystate_reset(uint8_t keycode) {
    uint32_t vector_index;
    uint32_t bit_index;
    get_location(keycode, &vector_index, &bit_index);

    uint32_t flags = keystate_vector[vector_index] >> (bit_index * NUM_KEYSTATE_FLAGS);
    keystate_vector[vector_index] &= ~(KEYSTATE_FLAG_MASK << (bit_index * NUM_KEYSTATE_FLAGS));

    return flags & KEYSTATE_FLAG_MASK;
}

void keystate_set(uint8_t keycode, KeyStateFlags flags) {
    uint32_t vector_index;
    uint32_t bit_index;
    get_location(keycode, &vector_index, &bit_index);

    keystate_vector[vector_index] &= ~(KEYSTATE_FLAG_MASK << (bit_index * NUM_KEYSTATE_FLAGS));
    keystate_vector[vector_index] |= flags << (bit_index * NUM_KEYSTATE_FLAGS);
}
