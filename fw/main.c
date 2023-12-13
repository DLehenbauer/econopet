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
#include "driver.h"

void measure_freqs(uint fpga_div) {
    uint32_t f_pll_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_SYS_CLKSRC_PRIMARY);
    uint32_t f_pll_usb = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_USB_CLKSRC_PRIMARY);
    uint32_t f_rosc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_ROSC_CLKSRC);
    uint32_t f_clk_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
    uint32_t f_clk_peri = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_PERI);
    uint32_t f_clk_usb = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_USB);
    uint32_t f_clk_adc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_ADC);
    uint32_t f_clk_rtc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_RTC);

    printf("    pll_sys  = %d kHz\n", f_pll_sys);
    printf("    pll_usb  = %d kHz\n", f_pll_usb);
    printf("    rosc     = %d kHz\n", f_rosc);
    printf("    clk_sys  = %d kHz\n", f_clk_sys);
    printf("    clk_peri = %d kHz\n", f_clk_peri);
    printf("    clk_usb  = %d kHz\n", f_clk_usb);
    printf("    clk_adc  = %d kHz\n", f_clk_adc);
    printf("    clk_rtc  = %d kHz\n", f_clk_rtc);
    printf("    clk_fpga = %d kHz\n", f_clk_sys / fpga_div);
}

void fpga_init() {
    gpio_init(FPGA_CRESET_GP);

    // Setup 270 MHz system clock
	vreg_set_voltage(VREG_VOLTAGE_1_20);
	sleep_ms(10);
 	set_sys_clock_khz(270000, true);

    // FPGA CLK: 270 MHz / 6 = 45 MHz
    const uint16_t fpga_div = 6;

    const uint slice = pwm_gpio_to_slice_num(FPGA_CLK_GP);
    const uint channel = pwm_gpio_to_channel(FPGA_CLK_GP);
    pwm_config config = pwm_get_default_config();
    pwm_config_set_wrap(&config, fpga_div - 1);
    pwm_init(slice, &config, /* start: */ true);
    pwm_set_chan_level(slice, channel, 2);
    gpio_set_drive_strength(FPGA_CLK_GP, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_function(FPGA_CLK_GP, GPIO_FUNC_PWM);

    // Setting 'sys_clk' causes 'peri_clk' to revert to 48 MHz.  (Re)initialize UART.
    //
    // See: https://github.com/Bodmer/TFT_eSPI/discussions/2432
    // See: https://github.com/raspberrypi/pico-examples/blob/master/clocks/hello_48MHz/hello_48MHz.c
    stdio_init_all();
    printf("\e[2J");
    printf("Clocks initialized:\n");
    measure_freqs(fpga_div);
}

int main() {
    fpga_init();

    gpio_init(SPI_CS_GP);
    gpio_set_dir(SPI_CS_GP, GPIO_OUT);
    gpio_put(SPI_CS_GP, 1);

    gpio_init(SPI_STALL_GP);
    gpio_set_dir(SPI_STALL_GP, GPIO_IN);

    uint baudrate = spi_init(SPI_INSTANCE, SPI_MHZ * 1000 * 1000);
    gpio_set_function(SPI_SCK_GP, GPIO_FUNC_SPI);
    gpio_set_function(SPI_SDO_GP, GPIO_FUNC_SPI);
    gpio_set_function(SPI_SDI_GP, GPIO_FUNC_SPI);

    printf("    spi1     = %d Bd\n", baudrate);

    uint8_t expected = 0x10;

    while (1) {
        spi_write_at(0, expected);
        spi_read_at(0);
        const uint8_t actual = spi_read_next();
        assert(actual == expected);
        expected++;
    }
}
