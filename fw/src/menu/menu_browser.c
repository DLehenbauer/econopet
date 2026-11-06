// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "menu_browser.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#include "diag/log/log.h"
#include "fatal.h"

// Records a recoverable filesystem error for the menu to display.
static bool browser_error(menu_browser_t* browser, const char* message) {
    snprintf(browser->error, sizeof(browser->error), "%s", message);
    log_warn("menu: %s (%s)", message, browser->path);
    return false;
}

// Orders directories first, then names without losing case-sensitive identity.
static int entry_compare(const menu_browser_entry_t* left, const menu_browser_entry_t* right) {
    if (left->directory != right->directory) return left->directory ? -1 : 1;
    const int insensitive = strcasecmp(left->name, right->name);
    return insensitive != 0 ? insensitive : strcmp(left->name, right->name);
}

// Recognizes disk formats supported by the mount facade.
static bool image_name(const char* name) {
    const char* const extension = strrchr(name, '.');
    return extension != NULL && (strcasecmp(extension, ".d64") == 0
        || strcasecmp(extension, ".d80") == 0 || strcasecmp(extension, ".d82") == 0);
}

// Uses FatFs stat on the board and refuses symlinks on host filesystems.
static int inspect_path(const char* path, struct stat* status) {
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
    return stat(path, status);
#else
    return lstat(path, status);
#endif
}

// Verifies every component before opening a directory, including mounted paths.
static bool safe_directory(menu_browser_t* browser) {
    if (strncmp(browser->path, MENU_BROWSER_ROOT, strlen(MENU_BROWSER_ROOT)) != 0
        || (browser->path[strlen(MENU_BROWSER_ROOT)] != '\0'
            && browser->path[strlen(MENU_BROWSER_ROOT)] != '/'))
        return browser_error(browser, "Directory is outside /disks");
    char prefix[SD_PATH_MAX];
    snprintf(prefix, sizeof(prefix), "%s", browser->path);
    char* component = prefix + 1;
    while (true) {
        char* const slash = strchr(component, '/');
        if (slash != NULL) *slash = '\0';
        if (component[0] == '\0' || strcmp(component, ".") == 0 || strcmp(component, "..") == 0)
            return browser_error(browser, "Invalid directory path");
        struct stat status;
        if (inspect_path(prefix, &status) != 0 || !S_ISDIR(status.st_mode))
            return browser_error(browser, "Directory is unavailable");
        if (slash == NULL) return true;
        *slash = '/';
        component = slash + 1;
    }
}

// Builds a full path without silently truncating a filename.
static bool entry_path(menu_browser_t* browser, const char* name, char output[SD_PATH_MAX]) {
    const int length = snprintf(output, SD_PATH_MAX, "%s/%s", browser->path, name);
    return length >= 0 && length < SD_PATH_MAX
        ? true : browser_error(browser, "Path is too long");
}

// Keeps the nearest bounded entries in scan order, ascending or descending.
static void insert_entry(menu_browser_t* browser, const menu_browser_entry_t* entry,
                         size_t limit, bool reverse) {
    size_t index = 0;
    while (index < browser->entry_count
        && (reverse ? entry_compare(&browser->entries[index], entry) > 0
                    : entry_compare(&browser->entries[index], entry) < 0)) index++;
    if (index >= limit) return;
    const size_t end = MIN(browser->entry_count, limit - 1);
    for (size_t i = end; i > index; i--) browser->entries[i] = browser->entries[i - 1];
    browser->entries[index] = *entry;
    if (browser->entry_count < limit) browser->entry_count++;
}

// Scans cooperatively, retaining only the nearest entries beyond an ordering cursor.
static bool scan_entries(menu_browser_t* browser, const menu_browser_entry_t* boundary,
                         size_t limit, bool reverse, size_t* total) {
    if (!safe_directory(browser)) return false;
    DIR* const dir = opendir(browser->path);
    if (dir == NULL) return browser_error(browser, "Cannot open directory");
    browser->entry_count = 0;
    *total = 0;
    bool ok = true;
    while (true) {
        errno = 0;
        const struct dirent* const entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) ok = browser_error(browser, "Cannot read directory");
            break;
        }
        if (browser->service != NULL) browser->service();
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char path[SD_PATH_MAX];
        if (!entry_path(browser, entry->d_name, path)) continue;
        bool directory = entry->d_type == DT_DIR;
        bool regular = entry->d_type == DT_REG;
        if (entry->d_type == DT_UNKNOWN) {
            struct stat status;
            if (inspect_path(path, &status) != 0) {
                ok = browser_error(browser, "Cannot inspect directory entry");
                break;
            }
            directory = S_ISDIR(status.st_mode);
            regular = S_ISREG(status.st_mode);
        }
        if (!directory && (!regular || !image_name(entry->d_name))) continue;
        (*total)++;
        menu_browser_entry_t item = {.directory = directory};
        snprintf(item.name, sizeof(item.name), "%s", entry->d_name);
        const int order = boundary != NULL ? entry_compare(&item, boundary) : 0;
        if (limit > 0 && (boundary == NULL || (reverse ? order <= 0 : order > 0)))
            insert_entry(browser, &item, limit, reverse);
    }
    if (closedir(dir) != 0) ok = browser_error(browser, "Cannot close directory");
    // Expose both scan directions in ascending display order.
    if (reverse) {
        for (size_t i = 0; i < browser->entry_count / 2; i++) {
            const menu_browser_entry_t item = browser->entries[i];
            browser->entries[i] = browser->entries[browser->entry_count - 1 - i];
            browser->entries[browser->entry_count - 1 - i] = item;
        }
    }
    return ok;
}

bool menu_browser_refresh(menu_browser_t* browser) {
    browser->error[0] = '\0';
    browser->cache_valid = false;
    browser->count = 0;
    size_t total;
    if (!scan_entries(browser, NULL, 0, false, &total)) return false;
    browser->count = total;
    return true;
}

bool menu_browser_page(menu_browser_t* browser, size_t first, size_t rows) {
    vet(rows <= MENU_BROWSER_PAGE_ROWS, "browser page exceeds cache capacity");
    if (browser->cache_valid && browser->first == first && browser->rows == rows) return true;
    menu_browser_entry_t cursor;
    bool have_cursor = false;
    bool reverse = false;
    size_t skip = first;

    // Reuse a cached ordering anchor for adjacent forward or overlapping backward pages.
    if (browser->cache_valid && first > browser->first
        && first <= browser->first + browser->entry_count) {
        cursor = browser->entries[first - browser->first - 1];
        have_cursor = true;
        skip = 0;
    }
    if (browser->cache_valid && first < browser->first && rows > 0
        && first + rows > browser->first
        && first + rows <= browser->first + browser->entry_count) {
        cursor = browser->entries[first + rows - browser->first - 1];
        have_cursor = true;
        reverse = true;
        skip = 0;
    }
    browser->cache_valid = false;
    while (skip > 0) {
        const size_t chunk = MIN(skip, MENU_BROWSER_PAGE_ROWS);
        size_t total;
        if (!scan_entries(browser, have_cursor ? &cursor : NULL, chunk, false, &total)) return false;
        if (browser->entry_count != chunk)
            return browser_error(browser, "Directory changed - reopen picker");
        cursor = browser->entries[chunk - 1];
        have_cursor = true;
        skip -= chunk;
    }
    size_t total;
    if (!scan_entries(browser, have_cursor ? &cursor : NULL, rows, reverse, &total)) return false;
    if (total != browser->count
        || browser->entry_count != MIN(rows, total - MIN(total, first)))
        return browser_error(browser, "Directory changed - reopen picker");
    browser->first = first;
    browser->rows = rows;
    browser->cache_valid = true;
    return true;
}

bool menu_browser_find(menu_browser_t* browser, const char* name, bool directory, size_t* index) {
    for (size_t first = 0; first < browser->count; first += MENU_BROWSER_PAGE_ROWS) {
        if (!menu_browser_page(browser, first, MENU_BROWSER_PAGE_ROWS)) return false;
        for (size_t i = 0; i < browser->entry_count; i++) {
            if (browser->entries[i].directory == directory
                && strcmp(browser->entries[i].name, name) == 0) {
                *index = first + i;
                return true;
            }
        }
    }
    return false;
}

bool menu_browser_resolve(menu_browser_t* browser, size_t index, char path[SD_PATH_MAX],
                          bool* directory) {
    vet(browser->cache_valid && index >= browser->first
        && index - browser->first < browser->entry_count, "selected image outside cached page");
    const menu_browser_entry_t* const item = &browser->entries[index - browser->first];
    if (!entry_path(browser, item->name, path) || !safe_directory(browser)) return false;
    struct stat status;
    if (inspect_path(path, &status) != 0
        || (item->directory ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode)))
        return browser_error(browser, "Entry changed - reopen picker");
    *directory = item->directory;
    return true;
}

bool menu_browser_parent(menu_browser_t* browser, char child[SD_PATH_MAX]) {
    if (strcmp(browser->path, MENU_BROWSER_ROOT) == 0) return false;
    char* const slash = strrchr(browser->path, '/');
    vet(slash != NULL && slash >= browser->path + strlen(MENU_BROWSER_ROOT),
        "menu path escaped /disks");
    snprintf(child, SD_PATH_MAX, "%s", slash + 1);
    *slash = '\0';
    browser->cache_valid = false;
    return true;
}
