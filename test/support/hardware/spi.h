// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

// Match the Pico SDK frequency unit for host-side SPI setup.
#define MHZ 1000000u

typedef struct {
    unsigned index;
} spi_inst_t;

#ifdef __cplusplus
extern "C" {
#endif

extern spi_inst_t test_spi[2];
#define spi0 (&test_spi[0])
#define spi1 (&test_spi[1])

#ifdef __cplusplus
}
#endif
