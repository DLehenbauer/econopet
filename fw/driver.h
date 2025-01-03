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

void driver_init();

void spi_read(uint32_t addr, size_t byteLength, uint8_t* pDest);
void spi_read_seek(uint32_t addr);
uint8_t spi_read_at(uint32_t addr);
uint8_t spi_read_next();

void spi_write(uint32_t addr, const uint8_t* const pSrc, size_t byteLength);
void spi_write_at(uint32_t addr, uint8_t data);
void spi_write_next(uint8_t data);

void set_cpu(bool ready, bool reset, bool nmi);
void set_video(bool col80);
void sync_state();
