// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

typedef struct {
    unsigned index;
} spi_inst_t;

extern spi_inst_t sd_diag_mock_spi[2];
#define spi0 (&sd_diag_mock_spi[0])
#define spi1 (&sd_diag_mock_spi[1])
