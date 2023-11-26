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
#include "hw.h"

//                           WA
#define SPI_CMD_READ_AT    0b01000000
#define SPI_CMD_READ_NEXT  0b00000000
#define SPI_CMD_WRITE_AT   0b11000000
#define SPI_CMD_WRITE_NEXT 0b10000000

int main() {
    stdio_init_all();

    gpio_init(SPI_CS_PIN);
    gpio_set_dir(SPI_CS_PIN, GPIO_OUT);
    gpio_put(SPI_CS_PIN, 1);

    gpio_init(SPI_STALL_PIN);
    gpio_set_dir(SPI_STALL_PIN, GPIO_IN);

    uint baudrate = spi_init(SPI_INSTANCE, SPI_MHZ * 1000 * 1000);
    gpio_set_function(SPI_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SPI_SDO_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SPI_SDI_PIN, GPIO_FUNC_SPI);

    printf("    spi1     = %d Bd\n", baudrate);

    uint8_t count = 0;

    while (1) {
        const uint8_t tx[] = { SPI_CMD_WRITE_NEXT, count++ };

        while (gpio_get(SPI_STALL_PIN));
        gpio_put(SPI_CS_PIN, 0);

        spi_write_blocking(SPI_INSTANCE, tx, sizeof(tx));

        while (gpio_get(SPI_STALL_PIN));
        gpio_put(SPI_CS_PIN, 1);
        
        sleep_ms(1);
    }
}
