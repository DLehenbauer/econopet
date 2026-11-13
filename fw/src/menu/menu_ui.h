// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "config/config.h"
#include "input/input_actions.h"
#include "ieee/dos_media.h"
#include "menu_browser.h"
#include "ui/tui.h"

#define MENU_UI_WIDTH 40u
#define MENU_UI_HEIGHT 25u
#define MENU_UI_PAGE_ROWS MENU_BROWSER_PAGE_ROWS
#define MENU_UI_DRIVE_SLOT_COUNT (DOS_MEDIA_UNIT_COUNT * DOS_MEDIA_SLOTS_PER_UNIT)

typedef struct {
    tui_controller_t controller;
    tui_screen_t dashboard, drives, picker;
    tui_component_t controls[3], drive_controls[2], picker_control;
    int config_index;
    // The session owner applies the chosen configuration and clears this result.
    bool config_changed;
    bool choosing_model;
    bool is_boot;
    unsigned int slot;
    menu_browser_t browser;
    char error[MENU_UI_WIDTH + 1];
    char drive_labels[MENU_UI_DRIVE_SLOT_COUNT][MENU_UI_WIDTH];
    const char* drive_label_ptrs[MENU_UI_DRIVE_SLOT_COUNT];
    char labels[MENU_UI_PAGE_ROWS][MENU_UI_WIDTH + 1];
    size_t page_first;
    char slot_paths[MENU_UI_DRIVE_SLOT_COUNT][SD_PATH_MAX];
} menu_ui_t;

// Initializes the hierarchy with the default configuration selected but not applied.
// The service callback keeps menu.rom's display and inputs moving during directory scans.
void menu_ui_init(menu_ui_t* menu, const window_t* window, void (*service)(void));
// Starts a session, retaining browser history and opening the boot picker or on-demand root.
void menu_ui_begin(menu_ui_t* menu, const window_t* window, bool is_boot);
