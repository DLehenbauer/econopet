// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

// Clear the character plane on native video, HDMI, and the video terminal.
// The caller must halt the PET CPU before clearing.
void menu_screen_clear(void);
