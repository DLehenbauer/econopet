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

int main() {
    stdio_init_all();

    gpio_init(SPI_CSN_PIN);
    gpio_set_dir(SPI_CSN_PIN, GPIO_OUT);
    gpio_put(SPI_CSN_PIN, 1);
    
    uint baudrate = spi_init(SPI_INSTANCE, SPI_MHZ * 1000 * 1000);
    gpio_set_function(SPI_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SPI_TX_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SPI_RX_PIN, GPIO_FUNC_SPI);

    printf("    spi1     = %d Bd\n", baudrate);

    uint8_t count = 0;

    while (1) {
        const uint8_t tx[] = { count++ };

        gpio_put(SPI_CSN_PIN, 0);
        spi_write_blocking(SPI_INSTANCE, tx, sizeof(tx));
        gpio_put(SPI_CSN_PIN, 1);
    }
}
