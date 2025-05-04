#include "roms.h"

const uint8_t __in_flash(".rom_chars_8800") rom_chars_8800[] = {
    #include "901447_10.h"
};

const uint8_t __in_flash(".rom_menu_ff00") rom_menu_ff00[] = {
    #include "menu_rom.h"
};

const uint8_t* const p_video_font_000 = rom_chars_8800;
const uint8_t* const p_video_font_400 = rom_chars_8800 + 0x400;
