#include "pch.hpp"
#pragma once

class CDisplay {
    public:
        CDisplay(
            const uint8_t* const pCharRom,
            const size_t charRomByteSize,
            const uint8_t* const pVideoMemory,
            const uint8_t* const pCrtcRegs,
            uint8_t keyMatrix[10]
        );
        
        void update();

        volatile static unsigned s_funcKey;

    private:
        void keyDown(SDL_Keysym key);
        void keyUp(SDL_Keysym key);
        void traceKey(const char* eventName, const unsigned char scanCode, const uint8_t row, const uint8_t colMask);

        unsigned get_crtc_horizontal_total();
        unsigned get_crtc_horizontal_displayed();
        unsigned get_crtc_horizontal_sync_position();
        unsigned get_crtc_horizontal_sync_width();
        unsigned get_crtc_vertical_total();
        unsigned get_crtc_vertical_total_adjust();
        unsigned get_crtc_vertical_displayed();
        unsigned get_crtc_vertical_sync_position();
        unsigned get_crtc_interlace_mode_and_skew();
        unsigned get_crtc_maximum_raster_address();
        unsigned get_crtc_display_start_address();

        SDL_Window* pWindow = nullptr;
        SDL_Renderer* pRenderer = nullptr;
        SDL_Texture* pTargetTex = nullptr;
        SDL_Texture* pCharTex = nullptr;
        
        const uint8_t* const pVideoMemory;
        const uint8_t* const pCrtcRegs;
        uint8_t* const keyMatrix;
};
