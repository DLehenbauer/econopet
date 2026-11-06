// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "menu_ui.h"

#include "diag/log/log.h"
#include "display/char_encoding.h"
#include "fatal.h"
#include "input.h"
#include "ieee/ieee_drive.h"
#include "menu_config.h"
#include "version.h"

#define MENU_DISK_TITLE_ROW 0u
#define MENU_DISK_FRAME_TOP 1u
#define MENU_DISK_FRAME_BOTTOM 12u
#define MENU_DISK_ROW 3u
#define MENU_DISK_WARNING_ROW 20u
#define MENU_DISK_WARNING_UNDERLINE 0x63u
#define MENU_CONFIG_FRAME_TOP 1u
#define MENU_IMAGE_FRAME_TOP 0u
#define MENU_PICKER_FRAME_BOTTOM 23u
#define MENU_PICKER_TOP_PADDING 2u
#define MENU_PICKER_POSITION_INSET 2u
#define MENU_DISK_LABEL_COLUMN 6u
#define MENU_DISK_LABEL_WIDTH (MENU_UI_WIDTH - MENU_DISK_LABEL_COLUMN - 1u)
#define MENU_ACTION_ROW 11u
#define MENU_BUTTON_COLUMN 11u
#define MENU_BUTTON_WIDTH 17u
#define MENU_BACK_ROW 14u
#define MENU_BORDER_VERTICAL 0x5du
#define MENU_BORDER_HORIZONTAL 0x40u
#define MENU_FRAME_TOP_LEFT 0x70u
#define MENU_FRAME_TOP_RIGHT 0x6eu
#define MENU_BORDER_TOP_LEFT 0x61u
#define MENU_BORDER_TOP_RIGHT 0x76u
#define MENU_BORDER_BOTTOM_LEFT 0x6du
#define MENU_BORDER_BOTTOM_RIGHT 0x7du
#define MENU_SCROLLBAR_TOP 0x71u
#define MENU_SCROLLBAR_BOTTOM 0x72u
#define MENU_SCROLLBAR_TRACK 0x66u
#define MENU_SCROLLBAR_THUMB 0xa0u
#define MENU_DEVICE_DRIVE_TOP 0x72u
#define MENU_DEVICE_DRIVE_BOTTOM 0x6du
#define MENU_DISK_SELECTION_RIGHT 0x74u

// Counts the synthetic Eject and optional parent entries before files.
static size_t special_count(const menu_ui_t* menu);

// Writes clipped text without treating normal display clipping as a path error.
static void text_at(const window_t* window, unsigned int x, unsigned int y,
                    unsigned int width, const char* text) {
    const size_t length = MIN(strlen(text), width);
    for (size_t i = 0; i < length; i++)
        *window_xy(window, x + i, y) = ascii_to_vrom((uint8_t) text[i]);
}

// Uses the updated mockup's dashboard and paging legends.
static void paint_footer(const window_t* window, bool picker) {
    text_at(window, 0, 24, MENU_UI_WIDTH, picker
        ? "Up/Dn:Move Lt/Rt:Page RET:Selct DEL:Back"
        : "Up/Dn:Move      RET:Select      DEL:Back");
    window_reverse(window, window_xy(window, 0, 24), 2);
    window_reverse(window, window_xy(window, 3, 24), 2);
    if (picker) {
        window_reverse(window, window_xy(window, 11, 24), 2);
        window_reverse(window, window_xy(window, 14, 24), 2);
    }
    window_reverse(window, window_xy(window, picker ? 22 : 16, 24), 3);
    window_reverse(window, window_xy(window, 32, 24), 3);
}

// Reports browsing or media failure without dismissing the picker.
static bool menu_error(menu_ui_t* menu, const char* operation) {
    menu->browser.error[0] = '\0';
    snprintf(menu->error, sizeof(menu->error), "%s", operation);
    log_warn("menu: %s (%s)", operation, menu->browser.path);
    return false;
}

// Refreshes labels from the live mounts after returning from a picker.
static void refresh_drives(tui_controller_t* controller, void* context) {
    (void) controller;
    menu_ui_t* const menu = context;
    for (size_t slot = 0; slot < MENU_UI_DRIVE_SLOT_COUNT; slot++) {
        const char* const name = ieee_drive_filename(slot);
        const size_t length = strlen(name);
        if (length > MENU_DISK_LABEL_WIDTH)
            snprintf(menu->drive_labels[slot], sizeof(menu->drive_labels[slot]), "...%s",
                     name + length - (MENU_DISK_LABEL_WIDTH - 3));
        else
            snprintf(menu->drive_labels[slot], sizeof(menu->drive_labels[slot]), "%.*s",
                     (int) MENU_DISK_LABEL_WIDTH, name[0] != '\0' ? name : "(Empty)");
        menu->drive_label_ptrs[slot] = menu->drive_labels[slot];
    }
}

// Paints the on-demand root and the firmware's actual build date.
static void paint_dashboard(const window_t* window, const void* context) {
    (void) context;
    text_at(window, (MENU_UI_WIDTH - strlen("EconoPET")) / 2, 0, MENU_UI_WIDTH, "EconoPET");
    window_reverse(window, window->start, MENU_UI_WIDTH);
    text_at(window, 31, 21, 9, "Firmware");
    text_at(window, 32, 22, 8, "v" FW_BUILD_DATE);
    paint_footer(window, false);
}

// Draws the drive chooser's frame, device/drive prefixes, and beta warning.
static void paint_drives(const window_t* window, const void* context) {
    (void) context;
    text_at(window, 14, MENU_DISK_TITLE_ROW, 11, "Change Disk");
    for (unsigned int row = MENU_DISK_FRAME_TOP; row <= MENU_DISK_FRAME_BOTTOM; row++) {
        *window_xy(window, 0, row) = MENU_BORDER_VERTICAL;
        *window_xy(window, MENU_UI_WIDTH - 1, row) = MENU_BORDER_VERTICAL;
    }
    window_hline(window, window_xy(window, 1, MENU_DISK_FRAME_TOP),
                 MENU_UI_WIDTH - 2, MENU_BORDER_HORIZONTAL);
    *window_xy(window, 0, MENU_DISK_FRAME_TOP) = MENU_FRAME_TOP_LEFT;
    *window_xy(window, MENU_UI_WIDTH - 1, MENU_DISK_FRAME_TOP) = MENU_FRAME_TOP_RIGHT;
    window_hline(window, window_xy(window, 1, MENU_DISK_FRAME_BOTTOM),
                 MENU_UI_WIDTH - 2, MENU_BORDER_HORIZONTAL);
    *window_xy(window, 0, MENU_DISK_FRAME_BOTTOM) = MENU_BORDER_BOTTOM_LEFT;
    *window_xy(window, MENU_UI_WIDTH - 1, MENU_DISK_FRAME_BOTTOM) = MENU_BORDER_BOTTOM_RIGHT;
    for (unsigned int slot = 0; slot < MENU_UI_DRIVE_SLOT_COUNT; slot++) {
        char prefix[MENU_DISK_LABEL_COLUMN];
        if (slot % DOS_MEDIA_SLOTS_PER_UNIT == 0)
            snprintf(prefix, sizeof(prefix), "%2u:0:", 8 + slot / DOS_MEDIA_SLOTS_PER_UNIT);
        else snprintf(prefix, sizeof(prefix), "  :1:");
        text_at(window, 1, MENU_DISK_ROW + slot, MENU_DISK_LABEL_COLUMN - 1, prefix);
        *window_xy(window, 3, MENU_DISK_ROW + slot) = slot % DOS_MEDIA_SLOTS_PER_UNIT == 0
            ? MENU_DEVICE_DRIVE_TOP : MENU_DEVICE_DRIVE_BOTTOM;
    }
    text_at(window, 0, MENU_DISK_WARNING_ROW, MENU_UI_WIDTH,
            "Note: Disk emulation is beta. Please use");
    window_hline(window, window_xy(window, 0, MENU_DISK_WARNING_ROW + 1),
                 strlen("Note"), MENU_DISK_WARNING_UNDERLINE);
    text_at(window, 6, MENU_DISK_WARNING_ROW + 1, MENU_UI_WIDTH - 6,
            "backup images to avoid data loss.");
    paint_footer(window, false);
}

// Shows the visible page's position using the mockup's PETSCII scrollbar.
static void paint_scrollbar(const menu_ui_t* menu, const window_t* window) {
    const tui_list_t* const list = &menu->picker_control.list;
    const unsigned int top = menu->picker_control.bounds.y;
    const size_t track_rows = list->visible_rows - 1;
    size_t thumb_rows = track_rows;
    size_t thumb_first = 0;
    if (list->count > list->visible_rows) {
        thumb_rows = track_rows * list->visible_rows / list->count;
        if (thumb_rows == 0) thumb_rows = 1;
        const size_t last = list->count - list->visible_rows;
        thumb_first = (track_rows - thumb_rows) * MIN(list->first_visible, last) / last;
    }
    *window_xy(window, MENU_UI_WIDTH - 1, top - 1) = MENU_SCROLLBAR_TOP;
    *window_xy(window, MENU_UI_WIDTH - 1, top + track_rows) = MENU_SCROLLBAR_BOTTOM;
    for (size_t row = 0; row < track_rows; row++)
        *window_xy(window, MENU_UI_WIDTH - 1, top + row) =
            row >= thumb_first && row - thumb_first < thumb_rows
                ? MENU_SCROLLBAR_THUMB : MENU_SCROLLBAR_TRACK;
}

// Paints the picker frame, path, scroll position, and explicit error message.
static void paint_picker(const window_t* window, const void* context) {
    const menu_ui_t* const menu = context;
    const tui_list_t* const list = &menu->picker_control.list;
    const unsigned int top = menu->choosing_model ? MENU_CONFIG_FRAME_TOP : MENU_IMAGE_FRAME_TOP;
    if (menu->choosing_model) {
        const char* const title = "Select Configuration";
        text_at(window, (MENU_UI_WIDTH - strlen(title)) / 2, 0, MENU_UI_WIDTH, title);
    }
    for (unsigned int row = top; row <= MENU_PICKER_FRAME_BOTTOM; row++) {
        *window_xy(window, 0, row) = MENU_BORDER_VERTICAL;
        *window_xy(window, MENU_UI_WIDTH - 1, row) = MENU_BORDER_VERTICAL;
    }
    if (menu->choosing_model) {
        window_hline(window, window_xy(window, 1, top), MENU_UI_WIDTH - 2, MENU_BORDER_HORIZONTAL);
        *window_xy(window, 0, top) = MENU_FRAME_TOP_LEFT;
        *window_xy(window, MENU_UI_WIDTH - 1, top) = MENU_FRAME_TOP_RIGHT;
    } else {
        text_at(window, 1, top, MENU_UI_WIDTH - 2, menu->browser.path);
        *window_xy(window, 0, top) = MENU_BORDER_TOP_LEFT;
        *window_xy(window, MENU_UI_WIDTH - 1, top) = MENU_BORDER_TOP_RIGHT;
        window_reverse(window, window_xy(window, 0, top), MENU_UI_WIDTH);
    }
    window_hline(window, window_xy(window, 1, MENU_PICKER_FRAME_BOTTOM),
                 MENU_UI_WIDTH - 2, MENU_BORDER_HORIZONTAL);
    *window_xy(window, 0, MENU_PICKER_FRAME_BOTTOM) = MENU_BORDER_BOTTOM_LEFT;
    *window_xy(window, MENU_UI_WIDTH - 1, MENU_PICKER_FRAME_BOTTOM) = MENU_BORDER_BOTTOM_RIGHT;
    if (list->count > list->visible_rows)
        paint_scrollbar(menu, window);
    if (list->count != 0 && list->page_valid) {
        const size_t row = menu->picker_control.bounds.y + list->selected - list->first_visible;
        *window_xy(window, 1, row) = MENU_BORDER_TOP_LEFT ^ 0x80;
        *window_xy(window, MENU_UI_WIDTH - 2, row) = menu->choosing_model
            ? MENU_BORDER_TOP_LEFT : MENU_DISK_SELECTION_RIGHT;
    }
    const char* const error = menu->browser.error[0] != '\0' ? menu->browser.error : menu->error;
    if (error[0] != '\0')
        text_at(window, 1, top + 1, MENU_UI_WIDTH - 2, error);
    else if (!menu->choosing_model && list->count == special_count(menu))
        text_at(window, 1, top + 1, MENU_UI_WIDTH - 2, "No images or directories");
    if (!menu->choosing_model && list->count != 0 && list->page_valid) {
        // Adjacent pages share one entry, so each new page adds one fewer row.
        const size_t stride = list->visible_rows - 1;
        const size_t pages = list->count <= list->visible_rows ? 1
            : 1 + (list->count - list->visible_rows + stride - 1) / stride;
        char position[MENU_UI_WIDTH];
        snprintf(position, sizeof(position), "(%zu/%zu)", list->first_visible / stride + 1, pages);
        text_at(window, MENU_UI_WIDTH - strlen(position) - MENU_PICKER_POSITION_INSET,
                MENU_PICKER_FRAME_BOTTOM, strlen(position), position);
    } else if (menu->choosing_model && error[0] == '\0' && list->count > list->visible_rows) {
        char position[MENU_UI_WIDTH];
        snprintf(position, sizeof(position), "%zu / %zu",
                 list->selected + 1, list->count);
        text_at(window, MENU_UI_WIDTH - strlen(position) - 1,
                MENU_PICKER_FRAME_BOTTOM, strlen(position), position);
    }
    paint_footer(window, list->count > list->visible_rows);
}

// Returns the count of virtual entries preceding actual files.
static size_t special_count(const menu_ui_t* menu) {
    return strcmp(menu->browser.path, MENU_BROWSER_ROOT) == 0 ? 1 : 2;
}

// Adds presentation-only Eject and parent rows to a bounded filesystem page.
static bool disk_page(void* context, size_t first, size_t rows) {
    menu_ui_t* const menu = context;
    const size_t specials = special_count(menu);
    const size_t file_first = first > specials ? first - specials : 0;
    const size_t virtual_rows = first < specials ? specials - first : 0;
    const size_t wanted = rows - MIN(rows, virtual_rows);
    if (!menu_browser_page(&menu->browser, file_first, wanted)) return false;
    menu->page_first = first;
    for (size_t row = 0; row < rows; row++) {
        const size_t index = first + row;
        if (index >= menu->picker_control.list.count) break;
        if (index == 0) snprintf(menu->labels[row], sizeof(menu->labels[row]), "[Eject]");
        else if (index < specials) snprintf(menu->labels[row], sizeof(menu->labels[row]), "[..]");
        else {
            const menu_browser_entry_t* const item =
                &menu->browser.entries[index - specials - file_first];
            snprintf(menu->labels[row], sizeof(menu->labels[row]), item->directory ? "[%.*s]" : "%.*s",
                     (int) MENU_UI_WIDTH - 4, item->name);
        }
    }
    return true;
}

// Enumerates a bounded page of configuration names in source order.
static bool model_page(void* context, size_t first, size_t rows) {
    menu_ui_t* const menu = context;
    char names[MENU_UI_PAGE_ROWS][CONFIG_TEXT_CAPACITY];
    const menu_config_catalog_t catalog = menu_config_enumerate(first, rows, names);
    if (catalog.count != menu->picker_control.list.count)
        return menu_error(menu, "Models changed - reopen picker");
    for (size_t row = 0; row < rows && first + row < catalog.count; row++)
        snprintf(menu->labels[row], sizeof(menu->labels[row]), "%.*s",
                 CONFIG_TEXT_CAPACITY - 1, names[row]);
    menu->page_first = first;
    return true;
}

// Borrows a label from the page that navigation has already populated.
static const char* page_label(void* context, size_t index) {
    menu_ui_t* const menu = context;
    vet(index >= menu->page_first && index - menu->page_first < MENU_UI_PAGE_ROWS,
        "menu label outside cached page");
    return menu->labels[index - menu->page_first];
}

// Refreshes picker counts while preserving an existing selection and viewport.
static void refresh_picker(tui_controller_t* controller, void* context) {
    (void) controller;
    menu_ui_t* const menu = context;
    tui_list_t* const list = &menu->picker_control.list;
    const unsigned int top = menu->choosing_model ? MENU_CONFIG_FRAME_TOP : MENU_IMAGE_FRAME_TOP;
    menu->picker_control.bounds.y = top + MENU_PICKER_TOP_PADDING;
    menu->picker_control.bounds.height = MENU_PICKER_FRAME_BOTTOM - menu->picker_control.bounds.y;
    list->visible_rows = menu->picker_control.bounds.height;
    menu->error[0] = '\0';
    menu->browser.error[0] = '\0';
    if (menu->choosing_model) {
        list->count = menu_config_enumerate(0, 0, NULL).count;
        list->load_page = model_page;
    } else {
        list->count = menu_browser_refresh(&menu->browser)
            ? menu->browser.count + special_count(menu) : 0;
        list->load_page = disk_page;
    }
}

// Restores the parent picker selection using the browser's full-name lookup.
static void locate_entry(menu_ui_t* menu, const char* name) {
    tui_list_t* const list = &menu->picker_control.list;
    size_t index;
    list->selected = menu_browser_find(&menu->browser, name, true, &index)
        ? special_count(menu) + index : 0;
    const size_t stride = list->visible_rows - 1;
    list->first_visible = list->selected == 0 ? 0 : (list->selected - 1) / stride * stride;
    list->page_valid = disk_page(menu, list->first_visible, list->visible_rows);
}

// Retains only the directory, never the cursor or page, for the next visit.
static void remember_slot(menu_ui_t* menu) {
    snprintf(menu->slot_paths[menu->slot], SD_PATH_MAX, "%s", menu->browser.path);
}

// Reopens the current picker after changing directories, without growing the screen stack.
static void change_directory(menu_ui_t* menu) {
    menu->picker_control.list.selected = 0;
    menu->picker_control.list.first_visible = 0;
    refresh_picker(&menu->controller, menu);
    tui_list_t* const list = &menu->picker_control.list;
    list->page_valid = list->load_page(menu, 0, list->visible_rows);
}

// Goes to the parent within /disks, or returns to the dashboard at the root.
static void picker_back(tui_controller_t* controller, void* context) {
    menu_ui_t* const menu = context;
    if (menu->choosing_model && menu->is_boot) return;
    if (menu->choosing_model || strcmp(menu->browser.path, MENU_BROWSER_ROOT) == 0) {
        if (!menu->choosing_model) remember_slot(menu);
        tui_controller_pop(controller);
        return;
    }
    char child[SD_PATH_MAX];
    if (!menu_browser_parent(&menu->browser, child)) return;
    change_directory(menu);
    locate_entry(menu, child);
}

// Applies a picker mount or eject, keeping failures recoverable without changing live media.
static void mount_disk(tui_controller_t* controller, menu_ui_t* menu, const char* name) {
    const dos_media_result_t result = ieee_drive_try_mount(menu->slot, name);
    if (result != DOS_MEDIA_OK) {
        menu_error(menu, result == DOS_MEDIA_BUSY ? "Disk is busy - retry"
            : name == NULL ? "Cannot eject disk image"
            : result == DOS_MEDIA_UNSUPPORTED ? "Unsupported disk image" : "Cannot open disk image");
        return;
    }
    remember_slot(menu);
    tui_controller_pop(controller);
}

// Records a configuration choice for the caller to apply after the menu exits.
static void select_config(menu_ui_t* menu, int index) {
    menu->config_index = index;
    menu->config_changed = true;
    memset(menu->slot_paths, 0, sizeof(menu->slot_paths));
}

// Applies disk choices immediately, or exits to load a selected machine configuration.
static void select_item(tui_controller_t* controller, void* context, size_t index) {
    menu_ui_t* const menu = context;
    if (menu->choosing_model) {
        select_config(menu, (int) index);
        controller->active = false;
        return;
    }
    if (index == 0) {
        mount_disk(controller, menu, NULL);
        return;
    }
    if (index < special_count(menu)) {
        picker_back(controller, menu);
        return;
    }
    char path[SD_PATH_MAX];
    bool directory;
    if (!menu_browser_resolve(&menu->browser, index - special_count(menu), path, &directory)) return;
    if (directory) {
        snprintf(menu->browser.path, sizeof(menu->browser.path), "%s", path);
        change_directory(menu);
    } else {
        mount_disk(controller, menu, path + strlen("/disks/"));
    }
}

// Opens the configuration picker with the currently loaded model selected.
static void show_models(tui_controller_t* controller, void* context) {
    menu_ui_t* const menu = context;
    menu->choosing_model = true;
    menu->picker_control.list.overlapping_pages = true;
    menu->picker_control.list.selected = menu->config_index;
    menu->picker_control.list.first_visible = 0;
    tui_controller_push(controller, &menu->picker);
}

// Opens the drive chooser showing the current mounts.
static void show_drives(tui_controller_t* controller, void* context) {
    menu_ui_t* const menu = context;
    tui_controller_push(controller, &menu->drives);
}

// Opens the slot's directory at its beginning, without preselecting the mounted image.
static void show_disks(tui_controller_t* controller, void* context, size_t index) {
    menu_ui_t* const menu = context;
    menu->choosing_model = false;
    menu->picker_control.list.overlapping_pages = true;
    menu->slot = index;
    snprintf(menu->browser.path, sizeof(menu->browser.path), "%s", MENU_BROWSER_ROOT);
    const char* const mounted = ieee_drive_filename(index);
    const bool remembered = menu->slot_paths[index][0] != '\0';
    if (remembered)
        snprintf(menu->browser.path, sizeof(menu->browser.path), "%s", menu->slot_paths[index]);
    else if (mounted[0] != '\0') {
        const char* const slash = strrchr(mounted, '/');
        if (slash != NULL) {
            const int length = snprintf(menu->browser.path, sizeof(menu->browser.path), "/disks/%.*s",
                                        (int) (slash - mounted), mounted);
            vet(length >= 0 && length < SD_PATH_MAX, "mounted menu path is too long");
        }
    }
    menu->picker_control.list.selected = 0;
    menu->picker_control.list.first_visible = 0;
    tui_controller_push(controller, &menu->picker);
}

// Finishes choosing so the caller can load the selected configuration and reset.
static void reset_machine(tui_controller_t* controller, void* context) {
    (void) context;
    controller->active = false;
}

// The ROM-based menu cannot resume. Back at its root reloads the last configuration.
static void dashboard_back(tui_controller_t* controller, void* context) {
    (void) context;
    controller->active = false;
}

// Gives the drive chooser's Back button the same behavior as its Back key.
static void drives_back(tui_controller_t* controller, void* context) {
    (void) context;
    tui_controller_dispatch(controller, tui_action_back);
}

void menu_ui_init(menu_ui_t* menu, const window_t* window, void (*service)(void)) {
    vet(window->width == MENU_UI_WIDTH && window->height == MENU_UI_HEIGHT,
        "menu dashboard requires a 40x25 window");
    *menu = (menu_ui_t) {.browser.service = service};
    const menu_config_catalog_t catalog = menu_config_enumerate(0, 0, NULL);
    select_config(menu, catalog.default_index >= 0 ? catalog.default_index : 0);
    menu->drive_controls[0] = (tui_component_t) {
        .kind = tui_component_list,
        .bounds = {MENU_DISK_LABEL_COLUMN, MENU_DISK_ROW, MENU_DISK_LABEL_WIDTH,
                   MENU_UI_DRIVE_SLOT_COUNT},
        .list = {.labels = menu->drive_label_ptrs, .count = MENU_UI_DRIVE_SLOT_COUNT,
                 .visible_rows = MENU_UI_DRIVE_SLOT_COUNT,
                 .on_activate = show_disks, .context = menu}
    };
    menu->drive_controls[1] = (tui_component_t) {
        .kind = tui_component_button,
        .bounds = {0, MENU_BACK_ROW, MENU_UI_WIDTH, 1},
        .button = {.label = "Back", .on_activate = drives_back}
    };
    const char* const labels[] = {"    Reset    ", " Change Disk ", "Change Config"};
    const tui_screen_fn actions[] = {reset_machine, show_drives, show_models};
    for (size_t i = 0; i < count_of(menu->controls); i++)
        menu->controls[i] = (tui_component_t) {
            .kind = tui_component_button,
            .bounds = {MENU_BUTTON_COLUMN, MENU_ACTION_ROW + i, MENU_BUTTON_WIDTH, 1},
            .button = {.label = labels[i], .on_activate = actions[i], .context = menu}
        };
    menu->picker_control = (tui_component_t) {
        .kind = tui_component_list,
        .bounds = {2, MENU_CONFIG_FRAME_TOP + MENU_PICKER_TOP_PADDING,
                   MENU_UI_WIDTH - 4, MENU_UI_PAGE_ROWS},
        .list = {.visible_rows = MENU_UI_PAGE_ROWS, .on_activate = select_item,
                 .context = menu, .label_at = page_label}
    };
    menu->dashboard = (tui_screen_t) {
        .components = menu->controls, .component_count = count_of(menu->controls),
        .context = menu, .on_paint = paint_dashboard,
        .on_back = dashboard_back
    };
    menu->drives = (tui_screen_t) {
        .components = menu->drive_controls, .component_count = count_of(menu->drive_controls),
        .context = menu, .on_show = refresh_drives, .on_paint = paint_drives
    };
    menu->picker = (tui_screen_t) {
        .components = &menu->picker_control, .component_count = 1,
        .context = menu, .on_show = refresh_picker, .on_paint = paint_picker,
        .on_back = picker_back
    };
    tui_controller_init(&menu->controller, window, &menu->dashboard);
}

void menu_ui_begin(menu_ui_t* menu, const window_t* window, bool is_boot) {
    menu->is_boot = is_boot;
    menu->choosing_model = is_boot;
    menu->picker_control.list.overlapping_pages = true;
    menu->picker_control.list.selected = menu->config_index;
    menu->picker_control.list.first_visible = 0;
    menu->drive_controls[0].list.selected = 0;
    menu->drive_controls[0].list.first_visible = 0;
    tui_controller_init(&menu->controller, window, is_boot ? &menu->picker : &menu->dashboard);
}

tui_action_t menu_ui_action_from_key(int key) {
    switch (key) {
        case KEY_UP: return tui_action_up;
        case KEY_DOWN: return tui_action_down;
        case KEY_BTN_SHORT: return tui_action_down;
        case KEY_LEFT: return tui_action_page_up;
        case KEY_RIGHT: return tui_action_page_down;
        case KEY_PGUP: return tui_action_page_up;
        case KEY_PGDN: return tui_action_page_down;
        case '\r':
        case '\n': return tui_action_activate;
        case KEY_BTN_LONG: return tui_action_activate;
        case '\b':
        case 0x7f:
        case '\e': return tui_action_back;
        default:
            // Note that KEY_SHIFT_SHIFT is intentionally ignored
            // inside the menu.
            return tui_action_none;
    }
}
