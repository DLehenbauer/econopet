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

#include "diag/log/log.h"
#include "diag/mem.h"
#include "display/display.h"
#include "display/dvi/dvi.h"
#include "driver.h"
#include "global.h"
#include "hw.h"
#include "input.h"
#include "menu/menu.h"
#include "pet.h"
#include "sd/sd.h"
#include "system_state.h"
#include "ui/cli.h"
#include "usb/usb.h"

void measure_freqs(uint fpga_div) {
    uint32_t f_pll_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_SYS_CLKSRC_PRIMARY);
    uint32_t f_pll_usb = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_USB_CLKSRC_PRIMARY);
    uint32_t f_rosc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_ROSC_CLKSRC);
    uint32_t f_clk_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
    uint32_t f_clk_peri = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_PERI);
    uint32_t f_clk_usb = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_USB);
    uint32_t f_clk_adc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_ADC);
    uint32_t f_clk_rtc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_RTC);

    log_debug("clk: pll_sys=%lu pll_usb=%lu rosc=%lu kHz", f_pll_sys, f_pll_usb, f_rosc);
    log_debug("clk: sys=%lu peri=%lu usb=%lu kHz", f_clk_sys, f_clk_peri, f_clk_usb);
    log_debug("clk: adc=%lu rtc=%lu fpga=%lu kHz", f_clk_adc, f_clk_rtc, f_clk_sys / fpga_div);
}

void fpga_write_zeros(size_t count) {
    assert(count <= TEMP_BUFFER_SIZE);
    
    uint8_t* temp_buffer = acquire_temp_buffer();
    memset(temp_buffer, 0, count);
    spi_write_blocking(FPGA_SPI_INSTANCE, temp_buffer, count);
    release_temp_buffer(&temp_buffer);
}

void fpga_read_bitstream_callback(size_t offset, uint8_t* buffer, size_t bytes_read, void* context) {
    (void)offset;
    (void)context;

    spi_write_blocking(FPGA_SPI_INSTANCE, buffer, bytes_read);
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

    log_debug("FLASH_SIZE=0x%08x XOSC_DELAY=%d", PICO_FLASH_SIZE_BYTES, PICO_XOSC_STARTUP_DELAY_MULTIPLIER);
    measure_freqs(fpga_div);

    // Initialize FPGA_SPI at 24 MHz (default format is to SPI mode 0).
    const int baudrate = spi_init(FPGA_SPI_INSTANCE, FPGA_SPI_MHZ * MHZ);
    gpio_set_function(FPGA_SPI_SCK_GP, GPIO_FUNC_SPI);
    gpio_set_function(FPGA_SPI_SDO_GP, GPIO_FUNC_SPI);
    gpio_set_function(FPGA_SPI_SDI_GP, GPIO_FUNC_SPI);
    log_debug("fpga_spi=%u Bd", baudrate);

    // Configure and deassert CS_N.  We configure CS_N as GPIO_OUT rather than GPIO_FUNC_SPI
    // so we can control via software.
    gpio_init(FPGA_SPI_CSN_GP);
    gpio_set_dir(FPGA_SPI_CSN_GP, GPIO_OUT);
    gpio_put(FPGA_SPI_CSN_GP, 1);

    // Configure STALL.  STALL is asserted by the FPGA while it is busy processing the last SPI command.
    gpio_init(SPI_STALL_GP);
    gpio_set_dir(SPI_STALL_GP, GPIO_IN);

    // When no programmer is attached, the onboard 100k pull-down will hold the FPGA in reset.
    // If CRESET_N is is high, we know a JTAG programmer is attached and skip FPGA configuration.
    if (gpio_get(FPGA_CRESET_GP)) {
        log_warn("FPGA config skipped: Programmer attached");
        return;
    }

    // Create a clean CRESET_N pulse to initiate FPGA configuration.
    gpio_set_dir(FPGA_CRESET_GP, GPIO_OUT);
    gpio_put(FPGA_CRESET_GP, 1);
    sleep_ms(1);  // t_CRESET_N = 320 ns
    gpio_put(FPGA_CRESET_GP, 0);
    sleep_ms(1);  // t_CRESET_N = 320 ns

    // Efinix requires SPI mode 3 for configuration.
    spi_set_format(FPGA_SPI_INSTANCE, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);

    // Changes in clock polarity do not seem to take effect until the next write.  Send
    // a single byte while CS_N is deasserted to transition SCK to high.
    fpga_write_zeros(/* count: */ 1);
    sleep_ms(1);  // t_CRESET_N = 320 ns

    // The Efinix FPGA samples CS_N on the positive edge of CRESET_N to select passive
    // vs. active SPI configuration.  (0 = Passive, 1 = Active)
    gpio_put(FPGA_SPI_CSN_GP, 0);

    sleep_ms(1);  // t_CRESET_N = 320 ns
    gpio_put(FPGA_CRESET_GP, 1);
    sleep_ms(1);  // t_CRESET_N = 320 ns

    // Send bitstream to FPGA. To generate a '*.hex.bin' file, you must enable 'Generate SPI Raw
    // Binary Configuration File' under ~File ~Edit Project ~Bitstream Generation.
    sd_read_file("/fpga/EconoPET.hex.bin", fpga_read_bitstream_callback, NULL, SIZE_MAX);

    // To ensure successful configuration, the microprocessor must continue to supply the
    // configuration clock to the Trion FPGA for at least 100 cycles after sending the last
    // configuration data.
    //
    // Efinix example clocks out 1000 zero bits to generate extra clock cycles.
    fpga_write_zeros(/* count: */ 125);

    // Deassert CS_N to signal end of configuration.
    sleep_ms(1);  // t_CRESET_N = 320 ns
    gpio_put(FPGA_SPI_CSN_GP, 1);

    // Restore SPI mode 0
    spi_set_format(FPGA_SPI_INSTANCE, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    // Changes in clock polarity do not seem to take effect until the next write.  Send
    // a single byte while CS_N is deasserted to transition SCK to low.
    fpga_write_zeros(/* count: */ 1);

    log_info("FPGA configured");
}

int main() {
    // Note: A prolonged period without a valid sync signal may cause the PET monitor to
    // display a "bright spot" which can damage the CRT phosphor. Therefore, we initialize
    // the FPGA as soon as possible to start generating a native video for the PET.
    //
    // We limit the work done before fpga_init() to tasks that are either very fast or
    // prerequisites for FPGA configuration.

    // Turn on LED to signal that the RP2040 has booted.  'sd_init()' will turn LED off.
    // If the LED is "stuck on" at boot, this typically means the sd card is missing.
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    log_init();     // Initialize logging subsystem early for boot diagnostics
    input_init();   // Begin charging debounce capacitor for menu button (if any)
    sd_init();      // SD card is required to read the FPGA's bitstream file
    fpga_init();    // Setup sys_clock, FPGA_SPI, and configure FPGA

    // We now are generating a valid video signal for the PET, so it's safe to proceed
    // with the rest of the initialization.

    display_init(); // Initialize firmware display subsystem
    usb_init();     // Initialize USB subsystem
    cli_init();     // Start CLI on UART serial
    
    // Enter boot menu
    menu_enter();

    // PET is configured and running.  Enter main loop to synchronize displays, service
    // input queues, and check for menu/reset button.
    while (true) {
        display_task(); // Sync video buffer, render to terminal if needed
        input_task();   // Poll inputs, dispatch based on mode
        menu_task();    // Check for button events to enter menu
    }

    __builtin_unreachable();
}
