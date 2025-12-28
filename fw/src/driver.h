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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "system_state.h"

void driver_init();

void spi_read(uint32_t addr, size_t byteLength, uint8_t* pDest);
void spi_read_seek(uint32_t addr);
uint8_t spi_read_at(uint32_t addr);
uint8_t spi_read_next();
uint8_t spi_read_prev();
uint8_t spi_read_same();

void spi_write(uint32_t addr, const uint8_t* const pSrc, size_t byteLength);
uint8_t spi_write_at(uint32_t addr, uint8_t data);
uint8_t spi_write_next(uint8_t data);
uint8_t spi_write_prev(uint8_t data);
uint8_t spi_write_same(uint8_t data);
void spi_fill(uint32_t addr, uint8_t byte, size_t byteLength);

void set_cpu(bool ready, bool reset, bool nmi);
void sync_state();

void read_pet_model(system_state_t* const system_state);
void write_pet_model(const system_state_t* const system_state);
