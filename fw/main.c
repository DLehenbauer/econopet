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
#include "driver.h"
#include "hw.h"
#include "menu/menu.h"
#include "pet.h"
#include "sd/sd.h"
#include "term.h"
#include "test/mem.h"
#include "usb/usb.h"
#include "usb/keyboard.h"
#include "video/video.h"

static bool isBusinessKeyboard = false;
static bool is50Hz = true;

void measure_freqs(uint fpga_div) {
    uint32_t f_pll_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_SYS_CLKSRC_PRIMARY);
    uint32_t f_pll_usb = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_USB_CLKSRC_PRIMARY);
    uint32_t f_rosc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_ROSC_CLKSRC);
    uint32_t f_clk_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
    uint32_t f_clk_peri = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_PERI);
    uint32_t f_clk_usb = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_USB);
    uint32_t f_clk_adc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_ADC);
    uint32_t f_clk_rtc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_RTC);

    printf("    pll_sys  = %lu kHz\n", f_pll_sys);
    printf("    pll_usb  = %lu kHz\n", f_pll_usb);
    printf("    rosc     = %lu kHz\n", f_rosc);
    printf("    clk_sys  = %lu kHz\n", f_clk_sys);
    printf("    clk_peri = %lu kHz\n", f_clk_peri);
    printf("    clk_usb  = %lu kHz\n", f_clk_usb);
    printf("    clk_adc  = %lu kHz\n", f_clk_adc);
    printf("    clk_rtc  = %lu kHz\n", f_clk_rtc);
    printf("    clk_fpga = %lu kHz\n", f_clk_sys / fpga_div);
}

void fpga_init() {
    gpio_init(FPGA_CRESET_GP);

    // Setup 270 MHz system clock
	vreg_set_voltage(VREG_VOLTAGE_1_20);
	sleep_ms(10);
 	set_sys_clock_khz(270000, true);

    // Use PWM to generate 18 MHz clock for FPGA PLL input:
    //     270 MHz / 15 = 18 MHz
    const uint16_t fpga_div = 15;

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

void reset() {
    // Per the W65C02S datasheet (sections 3.10 - 3.11):
    //
    //  * The CPU requires a minimum of 2 clock cycles to reset.  The CPU clock is 1 MHz (1 us period).
    //  * Deasserting RDY prevents the CPU from advancing it's state on negative PHI2 edges.
    // 
    // (See: https://www.westerndesigncenter.com/wdc/documentation/w65c02s.pdf)

    // Out of paranoia, deassert CPU 'reset' to ensure the CPU observes a clean reset pulse.
    // (We set 'ready' to false to prevent the CPU from executing instructions.)
    set_cpu(/* ready: */ false, /* reset: */ false);
    sleep_us(4);
    
    // Assert CPU 'reset'.  Execution continues to be suspended by deasserting 'ready'.
    set_cpu(/* ready: */ false, /* reset: */ true);
    sleep_us(4);
    
    // While the CPU is held in reset:
    // * Perform a RAM test
    // * Initialize the ROM region of our SRAM

    // test_ram();
    pet_init_roms(video_is_80_col, isBusinessKeyboard, is50Hz);

    // Finally, deassert CPU 'reset' and assert 'ready' to allow the CPU to execute instructions.
    set_cpu(/* ready: */ true,  /* reset: */ false);
}

int main() {
    // Turn on LED at start of config to signal that the RP2040 has booted and FPGA
    // configuration has started (sd_init will turn LED off.)
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    // Begin charging the debouncing capacitor for the menu button.
    menu_init_start();

    fpga_init();

    // Deassert SD CS
    gpio_init(SD_CSN_GP);
    gpio_set_dir(SD_CSN_GP, GPIO_OUT);
    gpio_put(SD_CSN_GP, 1);
    
    // Deassert SPI CS
    gpio_init(FPGA_SPI_CSN_GP);
    gpio_set_dir(FPGA_SPI_CSN_GP, GPIO_OUT);
    gpio_put(FPGA_SPI_CSN_GP, 1);

    gpio_init(SPI_STALL_GP);
    gpio_set_dir(SPI_STALL_GP, GPIO_IN);

    // Initialize SPI1 at 24 MHz.
    uint baudrate = spi_init(FPGA_SPI_INSTANCE, SPI_MHZ * MHZ);
    gpio_set_function(FPGA_SPI_SCK_GP, GPIO_FUNC_SPI);
    gpio_set_function(FPGA_SPI_SDO_GP, GPIO_FUNC_SPI);
    gpio_set_function(FPGA_SPI_SDI_GP, GPIO_FUNC_SPI);
    printf("    spi1     = %u Bd\n", baudrate);

    sd_init();
    video_init();
    usb_init();
    reset();
    menu_init_end();

    while (true) {
        // TODO: Reconfigure SPI/Wishbone address space so we can read video ram and register file
        //       in a single SPI transaction?  Maybe even read/write simultaneously?
        spi_read(/* src: */ 0x8000, /* byteLength: */ sizeof(video_char_buffer), /* pDest: */ (uint8_t*) video_char_buffer);

        tuh_task();
        cdc_app_task();     // TODO: USB serial console unused.  Remove?
        hid_app_task();     // TODO: Remove empty HID task or merge with dispatch_key_events?
        dispatch_key_events();

        // Write USB keyboard state and read video graphics.
        sync_state();
        menu_task();
    }

    __builtin_unreachable();
}
