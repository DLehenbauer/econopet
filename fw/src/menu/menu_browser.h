// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "sd/sd.h"

#define MENU_BROWSER_PAGE_ROWS 21u
#define MENU_BROWSER_ERROR_CAPACITY 41u
#define MENU_BROWSER_ROOT "/disks"

typedef struct {
    char name[SD_PATH_MAX];
    bool directory;
} menu_browser_entry_t;

typedef struct {
    char path[SD_PATH_MAX]; // Current directory path, rooted at MENU_BROWSER_ROOT.
    char error[MENU_BROWSER_ERROR_CAPACITY]; // Recoverable menu error; cleared by refresh.
    menu_browser_entry_t entries[MENU_BROWSER_PAGE_ROWS]; // Sorted entries in the cached page.
    size_t count; // Directories and supported disk images counted at refresh.
    size_t first; // Zero-based listing index of the first cached entry.
    size_t entry_count; // Number of entries stored in the page cache.
    size_t rows; // Requested row count for the cached page.
    bool cache_valid; // Whether the cached page and its metadata can be reused.
    void (*service)(void); // Optional callback invoked during directory scans.
} menu_browser_t;

// Recounts the current directory and invalidates its bounded page cache.
bool menu_browser_refresh(menu_browser_t* browser);
// Loads a sorted page of actual files and directories, without synthetic UI items.
bool menu_browser_page(menu_browser_t* browser, size_t first, size_t rows);
// Finds a full entry name with bounded storage, returning its sorted file index.
bool menu_browser_find(menu_browser_t* browser, const char* name, bool directory, size_t* index);
// Resolves a cached entry and rechecks its filesystem type before activation.
bool menu_browser_resolve(menu_browser_t* browser, size_t index, char path[SD_PATH_MAX],
                          bool* directory);
// Moves to the parent within /disks and returns the child name, or false at root.
bool menu_browser_parent(menu_browser_t* browser, char child[SD_PATH_MAX]);
