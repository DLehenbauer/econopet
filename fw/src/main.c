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

// Run 'fpga/bitstream.sh' to generate 'fpga/bitstream.h', or use a JTAG programmer to 
// manually configure the FPGA after power on for a faster development loop.
#if __has_include("fpga/bitstream.h")
    #define FPGA_PROG
#else
    #warning "'fpga/bitstream.h' not found.  Use JTAG programmer to configure FPGA."
#endif

#include "pch.h"
#include "driver.h"
#include "global.h"
#include "hw.h"
#include "menu/menu.h"
#include "pet.h"
#include "sd/sd.h"
#include "term.h"
#include "diag/mem.h"
#include "usb/usb.h"
#include "usb/keyboard.h"
#include "video/video.h"

static bool isBusinessKeyboard = false;
static bool hasCRTC = true;
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

#ifdef FPGA_PROG
static const uint8_t __in_flash(".fpga_bitstream") bitstream[] = {
    #include "./fpga/bitstream.h"
};
#endif

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
    printf("PICO_FLASH_SIZE_BYTES: 0x%08x\n", PICO_FLASH_SIZE_BYTES);
    printf("PICO_XOSC_STARTUP_DELAY_MULTIPLIER: %d\n", PICO_XOSC_STARTUP_DELAY_MULTIPLIER);
    printf("Clocks initialized:\n");
    measure_freqs(fpga_div);

    // Initialize FPGA_SPI at 24 MHz (default format is to SPI mode 0).
    uint baudrate = spi_init(FPGA_SPI_INSTANCE, FPGA_SPI_MHZ * MHZ);
    gpio_set_function(FPGA_SPI_SCK_GP, GPIO_FUNC_SPI);
    gpio_set_function(FPGA_SPI_SDO_GP, GPIO_FUNC_SPI);
    gpio_set_function(FPGA_SPI_SDI_GP, GPIO_FUNC_SPI);
    printf("    fpga_spi = %u Bd\n", baudrate);

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
        printf("FPGA config skipped: Programmer attached.\n");
        return;
    }

    #ifdef FPGA_PROG

    // Create a clean CRESET_N pulse to initiate FPGA configuration.
    gpio_init(FPGA_CRESET_GP);
    gpio_set_dir(FPGA_CRESET_GP, GPIO_OUT);
    gpio_put(FPGA_CRESET_GP, 1);
    sleep_ms(1);  // t_CRESET_N = 320 ns
    gpio_put(FPGA_CRESET_GP, 0);
    sleep_ms(1);  // t_CRESET_N = 320 ns

    // Efinix requires SPI mode 3 for configuration.
    spi_set_format(FPGA_SPI_INSTANCE, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);

    // Later, we will use this buffer of 1,000 zero bits to generate extra clock cycles
    // at the end of configuration.  We clear the 125 bytes now 
    _Static_assert(sizeof(temp_buffer) >= 125, "'temp_buffer' must be at least 125 bytes.");

    // _Static_assert above ensures that sizeof(temp_buffer) >= 125 bytes.
    memset(temp_buffer, 0, 125);
    
    // Changes in clock polarity do not seem to take effect until the next write.  Send
    // a single byte while CS_N is deasserted to transition SCK to high.
    spi_write_blocking(FPGA_SPI_INSTANCE, temp_buffer, /* len: */ 1);
    sleep_ms(1);  // t_CRESET_N = 320 ns

    // The Efinix FPGA samples CS_N on the positive edge of CRESET_N to select passive
    // vs. active SPI configuration.  (0 = Passive, 1 = Active)
    gpio_put(FPGA_SPI_CSN_GP, 0);

    sleep_ms(1);  // t_CRESET_N = 320 ns
    gpio_put(FPGA_CRESET_GP, 1);

    printf("FPGA: Sending %d bytes\n", sizeof(bitstream));
    spi_write_blocking(FPGA_SPI_INSTANCE, bitstream, sizeof(bitstream));

    // Efinix example clocks out 1000 zero bits to generate extra clock cycles.
    // _Static_assert above ensures that sizeof(temp_buffer) >= 125 bytes.
    spi_write_blocking(FPGA_SPI_INSTANCE, temp_buffer, 125);

    // Deassert CS_N to signal end of configuration.
    sleep_ms(1);  // t_CRESET_N = 320 ns
    gpio_put(FPGA_SPI_CSN_GP, 1);

    // Restore SPI mode 0
    spi_set_format(FPGA_SPI_INSTANCE, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    // Changes in clock polarity do not seem to take effect until the next write.  Send
    // a single byte while CS_N is deasserted to transition SCK to low.
    spi_write_blocking(FPGA_SPI_INSTANCE, temp_buffer, /* len: */ 1);

    printf("FPGA: DONE\n");

    #endif
}

int main() {
    // Turn on LED at start of config to signal that the RP2040 has booted and FPGA
    // configuration has started (sd_init will turn LED off.)
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    menu_init();    // Begin charging debouncing capacitor.
    fpga_init();    // Setup sys_clock, FPGA_SPI, and configure FPGA.

    // Deassert SD CS
    gpio_init(SD_CSN_GP);
    gpio_set_dir(SD_CSN_GP, GPIO_OUT);
    gpio_put(SD_CSN_GP, 1);
    
    sd_init();
    video_init();
    usb_init();

    get_model(&hasCRTC, &isBusinessKeyboard);
    printf("Model: %s / %s\n", hasCRTC ? "CRTC" : "No CRTC", isBusinessKeyboard ? "Business Keyboard" : "Graphics Keyboard");

    pet_init_roms(video_is_80_col, isBusinessKeyboard, is50Hz);

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
