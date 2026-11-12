// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "menu_ui_test.h"

#include <dirent.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diag/log/log.h"
#include "diskimage_test.h"
#include "display/char_encoding.h"
#include "display/display.h"
#include "driver.h"
#include "ieee/diskimage.h"
#include "ieee/ieee_drive.h"
#include "ieee/ieee_protocol.h"
#include "input/input.h"
#include "input/input_controls.h"
#include "input/input_uart.h"
#include "input/input_routing.h"
#include "menu/menu_config.h"
#include "menu/menu_screen.h"
#include "menu/menu_ui.h"
#include "menu_browser_test.h"
#include "mock.h"
#include "pet.h"
#include "test_support.h"
#include "ui/cli.h"
#include "usb/keyboard.h"
#include "version.h"

#define TEST_MODEL_COUNT 47u
#define TEST_IMAGE_COUNT 65u

static unsigned int service_count, setup_count;
static unsigned int mount_count;
static bool verify_blank_on_setup;
static char applied_names[MENU_UI_DRIVE_SLOT_COUNT][SD_PATH_MAX];
static system_state_t saved_state;
static uint8_t guarded_screen[MENU_UI_WIDTH * MENU_UI_HEIGHT + 2];
static menu_ui_t menu;

DIR* __real_opendir(const char* path);
int __real_lstat(const char* path, struct stat* status);
struct dirent* __real_readdir(DIR* dir);

// Confirms cooperative scanning invokes the caller's display/input service hook.
static void service(void) {
    service_count++;
}

// Executes configured model options into the actual shared system state.
static void set_options(void* context, options_t* options) {
    (void) context;
    if (verify_blank_on_setup) {
        ck_assert_int_eq(get_cpu(), CPU_HALT);
        for (size_t i = 0; i < 0x800; i++) {
            ck_assert_uint_eq(system_state.video_char_buffer[i], CH_SPACE);
            ck_assert_uint_eq(spi_read_at(0x8000 + i), CH_SPACE);
        }
    }
    setup_count++;
    system_state.pet_display_columns = options->columns == 80
        ? pet_display_columns_80 : pet_display_columns_40;
    system_state_set_video_ram_mask(&system_state, options->video_ram_mask);
}

// Applies configured mounts through the production drive facade.
static void mount_option(void* context, uint32_t device, uint32_t drive, const char* name) {
    (void) context;
    const unsigned int slot = (device - 8) * DOS_MEDIA_SLOTS_PER_UNIT + drive;
    ck_assert_int_eq(ieee_drive_try_mount(slot, name), DOS_MEDIA_OK);
    snprintf(applied_names[slot], SD_PATH_MAX, "%s", name);
    mount_count++;
}

static const setup_sink_t setup_sink = {
    .on_set_options = set_options, .on_mount = mount_option, .system_state = &system_state
};

// Creates a filename visible to libc directory scans.
static void make_file(const char* relative) {
    char path[PATH_MAX];
    const int length = snprintf(path, sizeof(path), "%s/%s", directory_root, relative);
    ck_assert_int_ge(length, 0);
    ck_assert_uint_lt((size_t) length, sizeof(path));
    FILE* const file = fopen(path, "wb");
    ck_assert_ptr_nonnull(file);
    ck_assert_int_eq(fclose(file), 0);
}

// Adds a valid mountable image to the shared FILE fixture registry.
static void register_disk(const char* relative) {
    char path[SD_PATH_MAX];
    snprintf(path, sizeof(path), "/disks/%s", relative);
    uint8_t* const image = diskimage_test_make_d64();
    test_register_binary_file(path, image, DISKIMAGE_D64_SIZE, true);
    free(image);
}

// Registers exact-size media independently of the filename's extension.
static void register_disk_type(const char* relative, diskimage_type_t type) {
    static const size_t sizes[] = {0, DISKIMAGE_D64_SIZE, DISKIMAGE_D80_SIZE, DISKIMAGE_D82_SIZE};
    char path[SD_PATH_MAX];
    snprintf(path, sizeof(path), "/disks/%s", relative);
    uint8_t* const bytes = calloc(1, sizes[type]);
    ck_assert_ptr_nonnull(bytes);
    test_register_binary_file(path, bytes, sizes[type], true);
    free(bytes);
}

// Installs more models than a screen can display, preserving source-order identity.
static void register_models(bool with_default) {
    char yaml[8192];
    size_t used = snprintf(yaml, sizeof(yaml), "%sconfigs:\n",
                           with_default ? "default: model1\n" : "");
    for (unsigned int index = 0; index < TEST_MODEL_COUNT; index++) {
        const int length = snprintf(yaml + used, sizeof(yaml) - used,
            "  - id: model%u\n    name: Model %02u\n    setup:\n"
            "      - action: set\n        columns: %u\n",
            index, index, index % 2 == 0 ? 40 : 80);
        ck_assert_int_ge(length, 0);
        ck_assert_uint_lt((size_t) length, sizeof(yaml) - used);
        used += length;
    }
    test_register_file("/config.yaml", yaml);
}

// Applies the UI's outcome through the configuration layer, as the session loop does.
static void apply_menu(void) {
    load_config(&setup_sink, menu.config_index, menu.config_changed);
    menu.config_changed = false;
}

// Initializes real model state and a guarded 40x25 framebuffer.
static void setup(void) {
    log_init();
    saved_state = system_state;
    mock_reset();
    test_clear_files();
    ieee_drive_init();
    fail_readdir = fail_next_readdir = false;
    unknown_entry_types = false;
    directory_scans = metadata_reads = 0;
    setup_count = 0;
    verify_blank_on_setup = false;
    mount_count = 0;
    memset(applied_names, 0, sizeof(applied_names));
    service_count = 0;
    system_state.video_text_mode = video_text_mode_lower;
    snprintf(directory_root, sizeof(directory_root), "/tmp/econopet-menu-XXXXXX");
    ck_assert_ptr_nonnull(mkdtemp(directory_root));
    register_models(true);
    set_cpu(CPU_READY);
    memset(guarded_screen, 0xa5, sizeof(guarded_screen));
    static const window_t window = {
        guarded_screen + 1, guarded_screen + sizeof(guarded_screen) - 1,
        MENU_UI_WIDTH, MENU_UI_HEIGHT
    };
    menu_ui_init(&menu, &window, service);
    apply_menu();
    setup_count = 0;
    set_cpu(CPU_READY);
}

// Removes only the named fixture root and its test-created descendants.
static void remove_fixture_directory(const char* path) {
    DIR* const dir = __real_opendir(path);
    ck_assert_ptr_nonnull(dir);
    struct dirent* entry;
    while ((entry = __real_readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        const int length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        ck_assert_int_ge(length, 0);
        ck_assert_uint_lt((size_t) length, sizeof(child));
        struct stat status;
        ck_assert_int_eq(__real_lstat(child, &status), 0);
        if (S_ISDIR(status.st_mode)) remove_fixture_directory(child);
        else ck_assert_int_eq(unlink(child), 0);
    }
    ck_assert_int_eq(closedir(dir), 0);
    ck_assert_int_eq(rmdir(path), 0);
}

// Closes facade-owned files before clearing fixtures or deleting their directory.
static void teardown(void) {
    fail_readdir = fail_next_readdir = false;
    ck_assert(ieee_drive_unmount_all());
    test_clear_files();
    ck_assert_uint_eq(guarded_screen[0], 0xa5);
    ck_assert_uint_eq(guarded_screen[sizeof(guarded_screen) - 1], 0xa5);
    remove_fixture_directory(directory_root);
    directory_root[0] = '\0';
    system_state = saved_state;
}

// Checks literal text and reverse-video bits at fixed mockup coordinates.
static void expect_text(unsigned int x, unsigned int y, const char* text, bool reverse) {
    for (size_t index = 0; index < strlen(text); index++) {
        const uint8_t expected = ascii_to_vrom(text[index]) ^ (reverse ? 0x80 : 0);
        ck_assert_msg(guarded_screen[1 + y * MENU_UI_WIDTH + x + index] == expected,
                      "screen (%u,%u) expected %u, got %u", x + (unsigned int) index, y,
                      expected, guarded_screen[1 + y * MENU_UI_WIDTH + x + index]);
    }
}

// Navigates through the on-demand root and drive chooser to the requested slot.
static void open_slot(unsigned int slot) {
    if (menu.controller.screen_count == 1) {
        menu.controller.focused_component = 1;
        tui_controller_dispatch(&menu.controller, tui_action_activate);
    }
    menu.controller.focused_component = 0;
    menu.drive_controls[0].list.selected = slot;
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_uint_eq(menu.controller.screen_count, 3);
}

// Selects a picker item using actual cursor navigation and page refreshes.
static void move_to(size_t index) {
    while (menu.picker_control.list.selected < index)
        tui_controller_dispatch(&menu.controller, tui_action_down);
    while (menu.picker_control.list.selected > index)
        tui_controller_dispatch(&menu.controller, tui_action_up);
}

// Verifies the new root and drive chooser against the mockup geometry.
START_TEST(test_dashboard_layout_and_navigation) {
    tui_controller_render(&menu.controller);
    expect_text(16, 0, "EconoPET", true);
    expect_text(11, 11, "[     Reset     ]", true);
    expect_text(11, 12, "[  Change Disk  ]", false);
    expect_text(11, 13, "[ Change Config ]", false);
    expect_text(31, 21, "Firmware", false);
    expect_text(32, 22, "v" FW_BUILD_DATE, false);
    expect_text(0, 24, "Up", true);
    expect_text(16, 24, "RET", true);
    expect_text(32, 24, "DEL", true);

    tui_controller_dispatch(&menu.controller, tui_action_down);
    ck_assert_uint_eq(menu.controller.focused_component, 1);
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_uint_eq(menu.controller.screen_count, 2);
    tui_controller_render(&menu.controller);
    expect_text(14, 0, "Change Disk", false);
    expect_text(1, 3, " 8", false);
    expect_text(4, 3, "0:", false);
    expect_text(4, 4, "1:", false);
    expect_text(1, 9, "11", false);
    expect_text(16, 14, "[ Back ]", false);
    ck_assert_uint_eq(guarded_screen[1 + 3 * MENU_UI_WIDTH + 3], 0x72);
    ck_assert_uint_eq(guarded_screen[1 + 4 * MENU_UI_WIDTH + 3], 0x6d);
    ck_assert_uint_eq(guarded_screen[1 + MENU_UI_WIDTH], 0x70);
    ck_assert_uint_eq(guarded_screen[1 + MENU_UI_WIDTH + 39], 0x6e);
    ck_assert_uint_eq(guarded_screen[1 + 12 * MENU_UI_WIDTH], 0x6d);
    ck_assert_uint_eq(guarded_screen[1 + 12 * MENU_UI_WIDTH + 39], 0x7d);
    expect_text(0, 20, "Note: Disk emulation is beta. Please use", false);
    for (unsigned int col = 0; col < strlen("Note"); col++)
        ck_assert_uint_eq(guarded_screen[1 + 21 * MENU_UI_WIDTH + col], 0x63);
    expect_text(4, 21, "  ", false);
    expect_text(6, 21, "backup images to avoid data loss.", false);
    expect_text(6, 10, "(Empty)", false);
    for (unsigned int index = 1; index < 8; index++) {
        tui_controller_dispatch(&menu.controller, tui_action_down);
        ck_assert_uint_eq(menu.drive_controls[0].list.selected, index);
    }
    tui_controller_render(&menu.controller);
    for (unsigned int col = 6; col < 39; col++)
        ck_assert(guarded_screen[1 + 10 * MENU_UI_WIDTH + col] & 0x80);
    tui_controller_dispatch(&menu.controller, tui_action_back);
    ck_assert_uint_eq(menu.controller.screen_count, 1);
    ck_assert_uint_eq(menu.controller.focused_component, 1);
    ck_assert(tui_controller_is_active(&menu.controller));
    tui_controller_dispatch(&menu.controller, tui_action_down);
    ck_assert_uint_eq(menu.controller.focused_component, 2);
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    tui_controller_render(&menu.controller);
    expect_text(11, 24, "Lt", true);
    expect_text(14, 24, "Rt", true);
    expect_text(22, 24, "RET", true);
    expect_text(32, 24, "DEL", true);
    ck_assert_uint_eq(guarded_screen[1 + 4 * MENU_UI_WIDTH + 38], 0x61);
    tui_controller_dispatch(&menu.controller, tui_action_back);
    open_slot(0);
    tui_controller_render(&menu.controller);
    ck_assert_uint_eq(guarded_screen[1 + 2 * MENU_UI_WIDTH + 38], 0x74);
}
END_TEST

// Both ends of the drive list reach Back, which preserves live mounts like Backspace.
START_TEST(test_drive_back_button) {
    register_disk("mounted.d64");
    ck_assert_int_eq(ieee_drive_try_mount(0, "mounted.d64"), DOS_MEDIA_OK);
    tui_controller_dispatch(&menu.controller, tui_action_down);
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    if (_i == 0) {
        for (size_t slot = 0; slot < MENU_UI_DRIVE_SLOT_COUNT; slot++)
            tui_controller_dispatch(&menu.controller, tui_action_down);
    } else {
        tui_controller_dispatch(&menu.controller, tui_action_up);
    }
    ck_assert_uint_eq(menu.controller.focused_component, 1);
    tui_controller_render(&menu.controller);
    expect_text(16, 14, "[ Back ]", true);
    expect_text(6, 3, "mounted.d64", false);
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_uint_eq(menu.controller.screen_count, 1);
    ck_assert_uint_eq(menu.controller.focused_component, 1);
    ck_assert(tui_controller_is_active(&menu.controller));
    ck_assert_str_eq(ieee_drive_filename(0), "mounted.d64");
    ck_assert_uint_eq(setup_count, 0);
    ck_assert_uint_eq(mount_count, 0);
}
END_TEST

// Boot opens the configuration picker, ignores Back, and exits immediately on selection.
START_TEST(test_boot_configuration_picker) {
    const window_t* const window = menu.controller.window;
    menu_ui_begin(&menu, window, true);
    ck_assert(menu.choosing_model);
    ck_assert_uint_eq(menu.controller.screen_count, 1);
    ck_assert_ptr_eq(menu.controller.screens[0], &menu.picker);
    tui_controller_render(&menu.controller);
    expect_text(10, 0, "Select Configuration", false);
    expect_text(22, 24, "RET", true);
    tui_controller_dispatch(&menu.controller, tui_action_back);
    ck_assert(tui_controller_is_active(&menu.controller));
    ck_assert_uint_eq(setup_count, 0);
    move_to(2);
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert(!tui_controller_is_active(&menu.controller));
    ck_assert_int_eq(menu.config_index, 2);
    ck_assert_uint_eq(menu.controller.screen_count, 1);
    apply_menu();
    ck_assert_uint_eq(setup_count, 1);
    ck_assert_int_eq(system_state.pet_display_columns, pet_display_columns_40);
}
END_TEST

// Configuration paging uses the same overlap and neighboring selection as disk paging.
START_TEST(test_configuration_overlapping_pages) {
    const window_t* const window = menu.controller.window;
    menu_ui_begin(&menu, window, true);
    const size_t rows = menu.picker_control.list.visible_rows;
    ck_assert_uint_eq(rows, 20);
    move_to(rows - 1);
    tui_controller_dispatch(&menu.controller, _i == 0 ? tui_action_down : tui_action_page_down);
    ck_assert_uint_eq(menu.picker_control.list.first_visible, rows - 1);
    ck_assert_uint_eq(menu.picker_control.list.selected, rows);
    ck_assert_str_eq(menu.labels[0], "Model 19");
    ck_assert_str_eq(menu.labels[1], "Model 20");
    move_to(rows - 1);
    tui_controller_dispatch(&menu.controller, _i == 0 ? tui_action_up : tui_action_page_up);
    ck_assert_uint_eq(menu.picker_control.list.first_visible, 0);
    ck_assert_uint_eq(menu.picker_control.list.selected, rows - 2);
    ck_assert_str_eq(menu.labels[rows - 1], "Model 19");
}
END_TEST

// Both pickers display paging hints and a scrollbar only when the list exceeds one screen.
START_TEST(test_picker_paging_artwork) {
    const bool config = _i < 2;
    const bool paged = _i % 2 != 0;
    const size_t count = (config ? 20u : 21u) + (paged ? 1 : 0);
    if (config) {
        char yaml[2048];
        size_t used = snprintf(yaml, sizeof(yaml), "configs:\n");
        for (size_t i = 0; i < count; i++) {
            const int length = snprintf(yaml + used, sizeof(yaml) - used,
                "  - name: Config %zu\n    setup: []\n", i);
            ck_assert_int_gt(length, 0);
            ck_assert_uint_lt((size_t) length, sizeof(yaml) - used);
            used += length;
        }
        test_unregister_file("/config.yaml");
        test_register_file("/config.yaml", yaml);
        const window_t* const window = menu.controller.window;
        menu_ui_begin(&menu, window, true);
    } else {
        for (size_t i = 1; i < count; i++) {
            char name[32];
            snprintf(name, sizeof(name), "disk%02zu.d64", i);
            make_file(name);
        }
        open_slot(0);
    }
    ck_assert_uint_eq(menu.picker_control.list.count, count);
    move_to(0);
    tui_controller_render(&menu.controller);
    if (config) {
        expect_text(10, 0, "Select Configuration", false);
        ck_assert_uint_eq(guarded_screen[1 + MENU_UI_WIDTH], 0x70);
        for (unsigned int col = 1; col < MENU_UI_WIDTH - 1; col++)
            ck_assert_uint_eq(guarded_screen[1 + MENU_UI_WIDTH + col], 0x40);
        ck_assert_uint_eq(guarded_screen[1 + MENU_UI_WIDTH + MENU_UI_WIDTH - 1], 0x6e);
        expect_text(2, 3, "Config 0", true);
        expect_text(2, 22, "Config 19", false);
    } else {
        ck_assert_uint_eq(guarded_screen[1], 0xe1);
        expect_text(1, 0, "/disks", true);
        expect_text(7, 0, "                                ", true);
        ck_assert_uint_eq(guarded_screen[1 + MENU_UI_WIDTH - 1], 0xf6);
        expect_text(2, 2, "[Eject]", true);
        expect_text(2, 22, "disk20.d64", false);
        expect_text(33, 23, paged ? "(1/2)" : "(1/1)", false);
        ck_assert_uint_eq(guarded_screen[1 + 23 * MENU_UI_WIDTH + 38], 0x40);
        ck_assert_uint_eq(guarded_screen[1 + 23 * MENU_UI_WIDTH + 39], 0x7d);
    }
    if (paged) {
        expect_text(11, 24, "Lt", true);
        expect_text(14, 24, "Rt", true);
        expect_text(22, 24, "RET", true);
        ck_assert_uint_eq(guarded_screen[1 + (config ? 2 : 1) * MENU_UI_WIDTH + 39], 0x71);
    } else {
        expect_text(11, 24, "    ", false);
        expect_text(16, 24, "RET", true);
        ck_assert_uint_eq(guarded_screen[1 + (config ? 2 : 1) * MENU_UI_WIDTH + 39], 0x5d);
    }
    expect_text(32, 24, "DEL", true);
}
END_TEST

// Page totals include overlap, partial pages, and multi-digit counters without hiding the frame.
START_TEST(test_disk_page_counter) {
    static const size_t counts[] = {1, 21, 22, 41, 42, 202};
    static const size_t pages[] = {1, 1, 2, 2, 3, 11};
    for (size_t index = 1; index < counts[_i]; index++) {
        char name[32];
        snprintf(name, sizeof(name), "disk%03zu.d64", index);
        make_file(name);
    }
    open_slot(0);
    for (size_t page = 1; page <= pages[_i]; page++) {
        tui_controller_render(&menu.controller);
        char expected[32];
        snprintf(expected, sizeof(expected), "(%zu/%zu)", page, pages[_i]);
        expect_text(MENU_UI_WIDTH - strlen(expected) - 2, 23, expected, false);
        ck_assert_uint_eq(guarded_screen[1 + 23 * MENU_UI_WIDTH + 38], 0x40);
        ck_assert_uint_eq(guarded_screen[1 + 23 * MENU_UI_WIDTH + 39], 0x7d);
        tui_controller_dispatch(&menu.controller, tui_action_page_down);
    }
    tui_controller_render(&menu.controller);
    char expected[32];
    snprintf(expected, sizeof(expected), "(%zu/%zu)", pages[_i], pages[_i]);
    expect_text(MENU_UI_WIDTH - strlen(expected) - 2, 23, expected, false);
    move_to(0);
    tui_controller_render(&menu.controller);
    snprintf(expected, sizeof(expected), "(1/%zu)", pages[_i]);
    expect_text(MENU_UI_WIDTH - strlen(expected) - 2, 23, expected, false);
}
END_TEST

// Only drive labels omit long prefixes, preserving full paths and boundary-length labels.
START_TEST(test_drive_label_suffixes) {
    const char* const path = "superpet/a/very/long/directory/os9-systemdisk.d80";
    register_disk(path);
    ck_assert_int_eq(ieee_drive_try_mount(0, path), DOS_MEDIA_OK);
    char boundary[34];
    memset(boundary, 'x', sizeof(boundary) - 1);
    boundary[sizeof(boundary) - 1] = '\0';
    register_disk(boundary);
    ck_assert_int_eq(ieee_drive_try_mount(1, boundary), DOS_MEDIA_OK);
    menu.controller.focused_component = 1;
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    char expected[34];
    snprintf(expected, sizeof(expected), "...%s", path + strlen(path) - 30);
    ck_assert_str_eq(menu.drive_labels[0], expected);
    ck_assert_str_eq(menu.drive_labels[1], boundary);
    ck_assert_str_eq(ieee_drive_filename(0), path);
    tui_controller_render(&menu.controller);
    expect_text(6, 3, expected, true);
    expect_text(6, 4, boundary, false);
}
END_TEST

// Back preserves live mounts, including a replacement applied before leaving the menu.
START_TEST(test_on_demand_back_and_reentry) {
    make_file("original.d64");
    make_file("replacement.d64");
    register_disk("replacement.d64");
    register_disk("original.d64");
    ck_assert_int_eq(ieee_drive_try_mount(0, "original.d64"), DOS_MEDIA_OK);
    open_slot(0);
    move_to(2);
    tui_controller_dispatch(&menu.controller, tui_action_back);
    ck_assert_uint_eq(menu.controller.screen_count, 2);
    ck_assert_str_eq(ieee_drive_filename(0), "original.d64");
    ck_assert_uint_eq(setup_count, 0);
    ck_assert_uint_eq(mount_count, 0);
    open_slot(0);
    move_to(2);
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_uint_eq(menu.controller.screen_count, 2);
    ck_assert_str_eq(ieee_drive_filename(0), "replacement.d64");
    ck_assert_uint_eq(mount_count, 0);
    tui_controller_dispatch(&menu.controller, tui_action_back);
    ck_assert_uint_eq(menu.controller.screen_count, 1);
    ck_assert(tui_controller_is_active(&menu.controller));
    tui_controller_dispatch(&menu.controller, tui_action_back);
    ck_assert(!tui_controller_is_active(&menu.controller));
    apply_menu();
    ck_assert_uint_eq(setup_count, 1);
    ck_assert_str_eq(ieee_drive_filename(0), "replacement.d64");
    ck_assert_uint_eq(mount_count, 0);
    const int selected = menu.config_index;
    const window_t* const window = menu.controller.window;
    menu_ui_begin(&menu, window, false);
    ck_assert_ptr_eq(menu.controller.screens[0], &menu.dashboard);
    ck_assert_int_eq(menu.config_index, selected);
    ck_assert_str_eq(ieee_drive_filename(0), "replacement.d64");
    ck_assert_uint_eq(menu.controller.focused_component, 0);
}
END_TEST

// Display clipping must never change the full filename used by mounting.
START_TEST(test_long_names_and_overlong_paths) {
    char long_name[181];
    memset(long_name, 'a', sizeof(long_name) - 5);
    memcpy(long_name + sizeof(long_name) - 5, ".d64", 5);
    make_file(long_name);
    register_disk(long_name);
    char invalid[256];
    memset(invalid, 'z', sizeof(invalid) - 5);
    memcpy(invalid + sizeof(invalid) - 5, ".d64", 5);
    make_file(invalid);
    open_slot(4);
    ck_assert_uint_eq(menu.picker_control.list.count, 2);
    ck_assert(menu.picker_control.list.page_valid);
    ck_assert_str_eq(menu.browser.error, "Path is too long");
    move_to(1);
    ck_assert_uint_lt(strlen(menu.labels[1]), strlen(long_name));
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_str_eq(ieee_drive_filename(4), long_name);
    ck_assert_uint_eq(mount_count, 0);
    apply_menu();
    ck_assert_str_eq(ieee_drive_filename(4), long_name);
}
END_TEST

// Directory mutation is a fatal violation of the browser's stable-listing contract.
START_TEST(test_directory_change_blocks_activation) {
    for (unsigned int index = 0; index < MENU_UI_PAGE_ROWS + 1; index++) {
        char name[32];
        snprintf(name, sizeof(name), "image%02u.d64", index);
        make_file(name);
    }
    open_slot(0);
    make_file("new-image.d64");
    test_expect_fatal_message("browser directory count changed");
    test_expect_fatal_callback(teardown);
    move_to(MENU_UI_PAGE_ROWS);
}
END_TEST

// A file replaced by a symlink after listing must not be followed by activation.
START_TEST(test_replaced_entry_does_not_escape_root) {
    make_file("disk.d64");
    open_slot(0);
    move_to(1);
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/disk.d64", directory_root);
    ck_assert_int_eq(unlink(path), 0);
    ck_assert_int_eq(symlink("/tmp", path), 0);
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_str_eq(menu.browser.error, "Entry changed - reopen picker");
    ck_assert_uint_eq(menu.controller.screen_count, 3);
}
END_TEST

// Choosing a configuration exits without reading disks until the configuration is applied.
START_TEST(test_model_change_loads_configured_disks) {
    make_file("old.d64");
    make_file("new.d64");
    register_disk("old.d64");
    ck_assert_int_eq(ieee_drive_try_mount(0, "old.d64"), DOS_MEDIA_OK);
    open_slot(0);
    tui_controller_dispatch(&menu.controller, tui_action_back);
    ck_assert_str_eq(menu.slot_paths[0], "/disks");
    tui_controller_dispatch(&menu.controller, tui_action_back);
    menu.controller.focused_component = 2;
    test_unregister_file("/config.yaml");
    test_register_file("/config.yaml",
        "configs:\n"
        "  - id: new\n    name: Replacement\n    setup:\n"
        "      - action: set\n        columns: 80\n"
        "      - action: mount\n        device: 11\n        drive: 1\n        file: new.d64\n"
        "  - id: old\n    name: Previous\n    setup: []\n");
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    move_to(0);
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert(!tui_controller_is_active(&menu.controller));
    ck_assert_uint_eq(menu.controller.screen_count, 2);
    ck_assert_int_eq(menu.config_index, 0);
    ck_assert_str_eq(ieee_drive_filename(0), "old.d64");
    ck_assert_str_eq(ieee_drive_filename(7), "");
    ck_assert_str_eq(menu.slot_paths[0], "");
    ck_assert_uint_eq(setup_count, 0);
    ck_assert_uint_eq(mount_count, 0);
    ck_assert_int_eq(get_cpu(), CPU_READY);
    register_disk("new.d64");
    apply_menu();
    ck_assert_str_eq(applied_names[7], "new.d64");
    ck_assert_uint_eq(mount_count, 1);
    ck_assert_str_eq(ieee_drive_filename(0), "");
    ck_assert_str_eq(ieee_drive_filename(7), "new.d64");
    ck_assert_str_eq(menu.slot_paths[0], "");
    ck_assert_int_eq(system_state.pet_display_columns, pet_display_columns_80);
    menu_ui_begin(&menu, menu.controller.window, false);
    open_slot(7);
    ck_assert_str_eq(ieee_drive_filename(7), "new.d64");
}
END_TEST

// Reset and root Back both reload the last selected configuration.
START_TEST(test_dashboard_exit_outcome) {
    tui_controller_dispatch(&menu.controller, _i == 0 ? tui_action_activate : tui_action_back);
    ck_assert(!tui_controller_is_active(&menu.controller));
    ck_assert_uint_eq(setup_count, 0);
    ck_assert_int_eq(get_cpu(), CPU_READY);
    apply_menu();
    ck_assert_uint_eq(setup_count, 1);
    ck_assert_int_eq(get_cpu(), CPU_HALT);
}
END_TEST

static unsigned int boot_keys;
static terminal_session_t expected_terminal_session;

static unsigned int charset_observations;
static unsigned int charset_ready_after;

// Model menu.rom selecting lowercase after a few CPU status observations.
static void select_menu_charset(void) {
    ck_assert_uint_eq(mock_display_task_count(), 0);
    for (size_t i = 0; i < 0x800; i++) {
        ck_assert_uint_eq(system_state.video_char_buffer[i], CH_SPACE);
        ck_assert_uint_eq(spi_read_at(0x8000 + i), CH_SPACE);
    }
    if (++charset_observations == charset_ready_after) {
        mock_set_text_mode_lower(true);
        mock_cpu_set_run_callback(NULL);
    }
}

// Model a visible menu on both displays before the user exits.
static tui_action_t exit_after_menu_render(void) {
    ck_assert_int_eq(system_state.video_text_mode, video_text_mode_lower);
    ck_assert_uint_eq(mock_display_task_count(), 1);
    memset(system_state.video_char_buffer, 0x41, 0x800);
    spi_write(0x8000, system_state.video_char_buffer, 0x800);
    return tui_action_activate;
}

// Both displays are blank before the character set changes and before config reload.
START_TEST(test_charset_transition_blanks_both_displays) {
    test_unregister_file("/config.yaml");
    register_models(false);
    memset(system_state.video_char_buffer, 0x41, 0x800);
    memset(system_state.video_char_buffer + 0x800, 0x5a, 0x800);
    spi_write(0x8000, system_state.video_char_buffer, 0x1000);
    set_cpu(CPU_HALT);
    menu_screen_clear();
    ck_assert_uint_eq(system_state.video_char_buffer[0x800], 0x5a);
    ck_assert_uint_eq(spi_read_at(0x8800), 0x5a);

    system_state.video_text_mode = video_text_mode_graphics;
    mock_set_text_mode_lower(_i != 0);
    charset_observations = 0;
    charset_ready_after = _i == 0 ? 3 : 1;
    mock_cpu_set_run_callback(select_menu_charset);
    mock_input_set_reader(exit_after_menu_render);
    verify_blank_on_setup = true;
    set_cpu(CPU_READY);
    const window_t window = window_create(guarded_screen + 1, MENU_UI_WIDTH, MENU_UI_HEIGHT);
    menu_config_show(&window, &setup_sink, true);
    ck_assert_uint_eq(charset_observations, _i == 0 ? 3 : 1);
    ck_assert_uint_eq(setup_count, 1);
    mock_cpu_set_run_callback(NULL);
}
END_TEST

// Exercises the real boot-menu loop while requiring the menu ROM to remain running.
static tui_action_t boot_input(void) {
    ck_assert_int_eq(get_cpu(), CPU_READY);
    ck_assert_uint_eq(setup_count, 0);
    ck_assert_int_eq(system_state.terminal_session, expected_terminal_session);
    ck_assert_uint_lt(boot_keys, 4);
    ck_assert_uint_eq(mock_display_task_count(), 1);
    if (boot_keys++ < 3) {
        return tui_action_none;
    }
    mock_input_set_reader(NULL);
    return tui_action_activate;
}

// Boot selects directly, and on-demand Reset restores routing and the selected configuration.
START_TEST(test_boot_loop_waits_for_reset) {
    test_unregister_file("/config.yaml");
    register_models(false);
    const terminal_session_t session = _i == 0 ? terminal_session_cli : terminal_session_remote;
    system_state.terminal_session = session;
    expected_terminal_session = session;
    boot_keys = 0;
    mock_input_set_reader(boot_input);
    const window_t window = window_create(guarded_screen + 1, MENU_UI_WIDTH, MENU_UI_HEIGHT);
    menu_config_show(&window, &setup_sink, _i == 0);
    ck_assert_uint_eq(boot_keys, 4);
    ck_assert_uint_eq(mock_pet_nmi_count(), _i == 0 ? 1 : 0);
    ck_assert_uint_eq(setup_count, 1);
    ck_assert_int_eq(get_cpu(), CPU_HALT);
    ck_assert_int_eq(system_state.terminal_session, session);
}
END_TEST

// Sends a terminal byte stream through the production routing path.
static void send_uart(const char* text) {
    while (*text != '\0') {
        input_uart_process((unsigned char)*text++);
    }
}

// Ordinary rendering and UART routing follow the same authoritative session.
START_TEST(test_terminal_session_rendering) {
    cli_init();
    ck_assert_int_eq(system_state.terminal_session, terminal_session_cli);
    ck_assert(!display_term_video_active());
    input_uart_process('a');
    ck_assert_uint_eq(mock_term_inject_count(), 0);
    cli_init();
    send_uart("remote\r\n");
    ck_assert_int_eq(system_state.terminal_session, terminal_session_remote);
    ck_assert(display_term_video_active());
    input_uart_process('a');
    ck_assert_uint_eq(mock_term_inject_count(), 1);
    input_uart_process(0x03);
    ck_assert_int_eq(system_state.terminal_session, terminal_session_cli);
    ck_assert(!display_term_video_active());
    ck_assert_uint_eq(mock_term_begin_count(), 1);
    ck_assert_uint_eq(mock_term_end_count(), 1);
}
END_TEST

// Terminal aliases have one typed navigation meaning and ordinary typing has none.
START_TEST(test_terminal_navigation_actions) {
    const int keys[] = {KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
        KEY_PGUP, KEY_PGDN, '\r', '\n', '\b', 0x7f, '\e', 't', EOF};
    const tui_action_t expected[] = {tui_action_up, tui_action_down,
        tui_action_page_up, tui_action_page_down, tui_action_none, tui_action_none,
        tui_action_page_up, tui_action_page_down, tui_action_activate,
        tui_action_activate, tui_action_back, tui_action_back, tui_action_back,
        tui_action_none, tui_action_none};
    static_assert(count_of(keys) == count_of(expected), "terminal navigation trace must be complete");
    for (size_t i = 0; i < count_of(keys); ++i)
        ck_assert_int_eq(input_menu_bind_terminal(keys[i]), expected[i]);
}
END_TEST

// Exercises both mode changes while the real menu loop remains active.
static tui_action_t switch_terminal_in_menu(void) {
    ck_assert_uint_eq(mock_display_task_count(), 1);
    if (expected_terminal_session == terminal_session_cli) {
        ck_assert_int_eq(system_state.terminal_session, terminal_session_cli);
        send_uart("remote\r\n");
        ck_assert(cli_in_remote_mode());
        ck_assert_uint_eq(mock_term_begin_count(), 1);
        ck_assert_uint_eq(mock_term_refresh_count(), 1);
        ck_assert_int_eq(input_routing_navigation(), tui_action_none);
        input_uart_process(KEY_DOWN);
        ck_assert_int_eq(input_routing_navigation(), tui_action_down);
    } else {
        ck_assert(cli_in_remote_mode());
        input_uart_process(KEY_DOWN);
        input_uart_process(0x03);
        ck_assert(!cli_in_remote_mode());
        ck_assert_int_eq(input_routing_navigation(), tui_action_none);
        ck_assert_uint_eq(mock_term_end_count(), 1);
        send_uart("log\r");
    }
    mock_input_set_reader(NULL);
    return tui_action_activate;
}

// The menu never overrides the user's mode, and mode changes survive menu exit.
START_TEST(test_menu_preserves_user_terminal_mode) {
    test_unregister_file("/config.yaml");
    register_models(false);
    cli_init();
    expected_terminal_session = (_i & 1) == 0 ? terminal_session_cli : terminal_session_remote;
    if (expected_terminal_session == terminal_session_remote) {
        send_uart("remote\r\n");
    }
    mock_input_set_reader(switch_terminal_in_menu);
    const window_t window = window_create(guarded_screen + 1, MENU_UI_WIDTH, MENU_UI_HEIGHT);
    menu_config_show(&window, &setup_sink, _i < 2);

    ck_assert_int_eq(system_state.terminal_session,
        expected_terminal_session == terminal_session_cli ? terminal_session_remote : terminal_session_cli);
    ck_assert_int_eq(cli_in_remote_mode(), expected_terminal_session == terminal_session_cli);
    ck_assert_uint_eq(mock_term_begin_count(), 1);
    ck_assert_uint_eq(mock_term_end_count(), expected_terminal_session == terminal_session_remote ? 1 : 0);
    if (cli_in_remote_mode()) {
        input_uart_process('x');
        ck_assert_uint_eq(mock_term_inject_count(), 1);
        ck_assert_int_eq(mock_term_inject_last(), 'x');
        cli_exit_remote();
    }
}
END_TEST

// Feeds remote navigation into the real boot-menu controller.
static tui_action_t remote_boot_input(void) {
    if (boot_keys++ == 0) {
        send_uart("remote\r\n");
        input_uart_process(KEY_DOWN);
        send_uart("\r\n");
    }
    return tui_action_none;
}

START_TEST(test_remote_controls_boot_menu) {
    test_unregister_file("/config.yaml");
    register_models(false);
    cli_init();
    boot_keys = 0;
    mock_input_set_reader(remote_boot_input);
    const window_t window = window_create(guarded_screen + 1, MENU_UI_WIDTH, MENU_UI_HEIGHT);
    menu_config_show(&window, &setup_sink, true);
    ck_assert_int_eq(system_state.pet_display_columns, pet_display_columns_80);
    ck_assert(cli_in_remote_mode());
    ck_assert_uint_eq(mock_term_inject_count(), 0);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
    cli_exit_remote();
}
END_TEST

// Remote transitions do not write a CLI prompt to video, and Ctrl-C restores log access.
START_TEST(test_uart_remote_cli_and_log) {
    cli_init();
    log_info("terminal routing test marker");
    FILE* const output = tmpfile();
    ck_assert_ptr_nonnull(output);
    fflush(stdout);
    const int saved_stdout = dup(STDOUT_FILENO);
    ck_assert_int_ge(saved_stdout, 0);
    ck_assert_int_ge(dup2(fileno(output), STDOUT_FILENO), 0);

    input_routing_set_menu_active(true);
    send_uart("remote\r\n");
    fflush(stdout);
    rewind(output);
    char remote_text[2048] = {0};
    const size_t remote_length = fread(remote_text, 1, sizeof(remote_text) - 1, output);
    const tui_action_t after_remote = input_routing_navigation();
    const unsigned int injected = mock_term_inject_count();

    input_uart_process(KEY_UP);
    const tui_action_t up = input_routing_navigation();
    input_uart_process('\r');
    input_uart_process('\n');
    const tui_action_t enter = input_routing_navigation();
    const tui_action_t after_enter = input_routing_navigation();
    input_uart_process(0x03);
    const bool remote_after_ctrl_c = cli_in_remote_mode();
    send_uart("log\r");
    fflush(stdout);
    rewind(output);
    char text[2048] = {0};
    const size_t length = fread(text, 1, sizeof(text) - 1, output);
    const int restored = dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    fclose(output);
    input_routing_set_menu_active(false);
    ck_assert_int_ge(restored, 0);
    ck_assert_uint_gt(remote_length, 0);
    ck_assert_ptr_null(strstr(remote_text, "econopet>"));
    ck_assert_int_eq(after_remote, tui_action_none);
    ck_assert_uint_eq(injected, 0);
    ck_assert_int_eq(up, tui_action_up);
    ck_assert_int_eq(enter, tui_action_activate);
    ck_assert_int_eq(after_enter, tui_action_none);
    ck_assert(!remote_after_ctrl_c);
    ck_assert_uint_gt(length, 0);
    ck_assert_ptr_nonnull(strstr(text, "terminal routing test marker"));
    ck_assert_ptr_nonnull(strstr(text, "[exited remote mode]"));
}
END_TEST

// CRLF and queued menu keys cannot leak to the PET across routing changes.
START_TEST(test_uart_transition_boundaries) {
    cli_init();
    send_uart("remote\r\n");
    ck_assert_uint_eq(mock_term_inject_count(), 0);
    input_uart_process('x');
    ck_assert_int_eq(mock_term_inject_last(), 'x');
    input_routing_set_menu_active(true);
    input_uart_process(KEY_DOWN);
    input_routing_set_menu_active(false);
    input_routing_set_menu_active(true);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
    input_uart_process('\r');
    input_routing_set_menu_active(false);
    input_uart_process('\n');
    ck_assert_uint_eq(mock_term_inject_count(), 1);
    cli_exit_remote();
}
END_TEST

// Holds the physical button without returning a synthetic activation key.
static tui_action_t hold_menu_button(void) {
    ck_assert_uint_lt(boot_keys, 2);
    if (boot_keys++ == 0) {
        mock_clock_set(100000);
        mock_input_button_level(true);
        input_button_update(true);
        mock_clock_advance(50000);
        input_button_update(true);
        mock_input_button_level(false);
        input_button_update(false);
        mock_clock_advance(50000);
        input_button_update(false);
        return tui_action_none;
    }
    mock_input_button_level(true);
    input_button_update(true);
    mock_clock_advance(50000);
    input_button_update(true);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    mock_input_set_reader(NULL);
    return tui_action_none;
}

// The menu interprets a release as Down and a subsequent hold as activation.
START_TEST(test_menu_loop_activates_held_button) {
    mock_clock_set(0);
    input_controls_consume();
    input_button_update(false);
    mock_clock_advance(50000);
    input_button_update(false);
    ck_assert_int_eq(mock_take_request().action, input_system_none);
    // Let the menu's charset-settle wait use real time before injecting the hold.
    mock_reset();
    test_unregister_file("/config.yaml");
    register_models(false);
    boot_keys = 0;
    mock_input_set_reader(hold_menu_button);
    const window_t window = window_create(guarded_screen + 1, MENU_UI_WIDTH, MENU_UI_HEIGHT);
    menu_config_show(&window, &setup_sink, true);
    ck_assert_uint_eq(boot_keys, 2);
    ck_assert_uint_eq(setup_count, 1);
    ck_assert_int_eq(system_state.pet_display_columns, pet_display_columns_80);
    mock_input_button_level(false);
    input_button_update(false);
    mock_clock_advance(50000);
    input_button_update(false);
    ck_assert_int_eq(mock_take_request().action, input_system_none);
}
END_TEST

// Injects a qualified chord, then requires a separate Enter to choose the config.
static tui_action_t chord_then_enter(void) {
    ck_assert_uint_lt(boot_keys, 2);
    if (boot_keys++ == 0) {
        mock_clock_set(100000);
        const hid_keyboard_report_t report = {
            .modifier = KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT
        };
        process_kbd_report(1, 0, &report);
        mock_clock_advance(INPUT_LONG_PRESS_US);
        return tui_action_none;
    }
    mock_input_set_reader(NULL);
    return tui_action_activate;
}

// Menu dispatch ignores the chord event and does not replay a held chord on exit.
START_TEST(test_menu_loop_ignores_shift_chord) {
    test_unregister_file("/config.yaml");
    register_models(false);
    memset(pet_key_matrix, 0xff, sizeof(pet_key_matrix));
    memset(usb_key_matrix, 0xff, sizeof(usb_key_matrix));
    const hid_keyboard_report_t released = {0};
    process_kbd_report(1, 0, &released);
    input_controls_consume();
    boot_keys = 0;
    mock_input_set_reader(chord_then_enter);
    const window_t window = window_create(guarded_screen + 1, MENU_UI_WIDTH, MENU_UI_HEIGHT);
    menu_config_show(&window, &setup_sink, true);
    ck_assert_uint_eq(boot_keys, 2);
    ck_assert_uint_eq(setup_count, 1);
    mock_input_set_reader(NULL);
    mock_clock_advance(INPUT_LONG_PRESS_US);
    ck_assert(mock_input_empty());
    process_kbd_report(1, 0, &released);
}
END_TEST

static const int* session_keys;
static size_t session_key_count, session_key_index;
static unsigned int prior_setup_count;

// Supplies one session's keys and rejects setup before its terminating choice.
static tui_action_t session_input(void) {
    ck_assert_uint_eq(setup_count, prior_setup_count);
    // Cooperative disk scans may service input after the last key arrives but before dispatch.
    if (session_key_index == session_key_count)
        return tui_action_none;
    ck_assert_uint_lt(session_key_index, session_key_count);
    return input_menu_bind_terminal(session_keys[session_key_index++]);
}

// Runs the production menu loop with a bounded input trace.
static void run_session(const int* keys, size_t count, bool is_boot) {
    session_keys = keys;
    session_key_count = count;
    session_key_index = 0;
    prior_setup_count = setup_count;
    mock_input_set_reader(session_input);
    set_cpu(CPU_READY);
    const window_t window = window_create(guarded_screen + 1, MENU_UI_WIDTH, MENU_UI_HEIGHT);
    menu_config_show(&window, &setup_sink, is_boot);
    ck_assert_uint_eq(session_key_index, count);
    ck_assert_uint_eq(setup_count, prior_setup_count + 1);
}

// The production loop remembers config changes across Reset and destructive root Back.
START_TEST(test_configuration_persists_across_sessions) {
    test_unregister_file("/config.yaml");
    register_models(false);
    const int boot[] = {KEY_DOWN, KEY_DOWN, '\r'};
    run_session(boot, count_of(boot), true);
    ck_assert_int_eq(system_state.pet_display_columns, pet_display_columns_40);
    const int back[] = {'\b'};
    run_session(back, count_of(back), false);
    ck_assert_int_eq(system_state.pet_display_columns, pet_display_columns_40);
    const int change[] = {KEY_DOWN, KEY_DOWN, '\r', KEY_UP, '\r'};
    run_session(change, count_of(change), false);
    ck_assert_int_eq(system_state.pet_display_columns, pet_display_columns_80);
    const int reset[] = {'\r'};
    run_session(reset, count_of(reset), false);
    ck_assert_int_eq(system_state.pet_display_columns, pet_display_columns_80);
}
END_TEST

// Session restoration preserves interactive media, while choosing a config reapplies its mounts.
START_TEST(test_session_disk_policy) {
    make_file("configured.d64");
    make_file("replacement.d64");
    register_disk("configured.d64");
    register_disk("replacement.d64");
    test_unregister_file("/config.yaml");
    test_register_file("/config.yaml",
        "default: model\nconfigs:\n  - id: model\n    name: Model\n    setup:\n"
        "      - action: set\n        columns: 40\n"
        "      - action: mount\n        device: 8\n        drive: 0\n"
        "        file: configured.d64\n");
    run_session(NULL, 0, true);
    ck_assert_uint_eq(mount_count, 1);
    ck_assert_str_eq(ieee_drive_filename(0), "configured.d64");
    for (unsigned int slot = 1; slot < MENU_UI_DRIVE_SLOT_COUNT; ++slot)
        ck_assert_int_eq(ieee_drive_try_mount(slot, "replacement.d64"), DOS_MEDIA_OK);

    const int replace[] = {
        KEY_DOWN, '\r', '\r', KEY_DOWN, KEY_DOWN, '\r', '\b', KEY_UP, '\r'
    };
    run_session(replace, count_of(replace), false);
    ck_assert_uint_eq(mount_count, 1);
    ck_assert_str_eq(ieee_drive_filename(0), "replacement.d64");

    const int reselect[] = {KEY_DOWN, KEY_DOWN, '\r', '\r'};
    run_session(reselect, count_of(reselect), false);
    ck_assert_uint_eq(mount_count, 2);
    ck_assert_str_eq(ieee_drive_filename(0), "configured.d64");
    for (unsigned int slot = 1; slot < MENU_UI_DRIVE_SLOT_COUNT; ++slot)
        ck_assert_str_eq(ieee_drive_filename(slot), "");
}
END_TEST

// A configured default boots without input, while no default still needs a selection.
START_TEST(test_default_autoboot_and_reentry) {
    test_unregister_file("/config.yaml");
    test_register_file("/config.yaml", _i == 0
        ? "default: second\nconfigs:\n  - id: first\n    name: First\n    setup:\n"
          "      - action: set\n        columns: 40\n"
          "  - id: second\n    name: Second\n    setup:\n"
          "      - action: set\n        columns: 80\n"
        : "configs:\n  - id: first\n    name: First\n    setup:\n"
          "      - action: set\n        columns: 40\n");
    const int select[] = {'\r'};
    session_keys = select;
    session_key_count = count_of(select);
    session_key_index = 0;
    prior_setup_count = setup_count;
    mock_input_set_reader(session_input);
    // Auto-boot must not wait for menu.rom to select a character set.
    if (_i == 0) mock_set_text_mode_lower(false);
    const window_t window = window_create(guarded_screen + 1, MENU_UI_WIDTH, MENU_UI_HEIGHT);
    menu_config_show(&window, &setup_sink, true);
    ck_assert_uint_eq(session_key_index, _i == 0 ? 0 : 1);
    ck_assert_uint_eq(mock_pet_nmi_count(), _i == 0 ? 0 : 1);
    ck_assert_uint_eq(mock_display_task_count(), _i == 0 ? 0 : 1);
    ck_assert_int_eq(get_cpu(), CPU_HALT);
    ck_assert_uint_eq(setup_count, 1);
    ck_assert_int_eq(system_state.pet_display_columns,
                     _i == 0 ? pet_display_columns_80 : pet_display_columns_40);

    // Reentry must remember the auto-booted configuration, not the first entry.
    mock_set_text_mode_lower(true);
    run_session(select, count_of(select), false);
    ck_assert_uint_eq(mock_pet_nmi_count(), _i == 0 ? 0 : 1);
    ck_assert_int_eq(system_state.pet_display_columns,
                     _i == 0 ? pet_display_columns_80 : pet_display_columns_40);
}
END_TEST

static jmp_buf ram_test_exit;

// Stops the nonreturning RAM diagnostic after observing the CPU halt.
static void observe_ram_test(void) {
    ck_assert_int_eq(get_cpu(), CPU_HALT);
    ck_assert_uint_eq(setup_count, 0);
    longjmp(ram_test_exit, 1);
}

// Enter the diagnostic directly while the menu is polling CLI input.
static tui_action_t request_ram_test_in_menu(void) {
    send_uart("test   \r");
    return tui_action_none;
}

// The CLI enters the existing nonreturning diagnostic synchronously.
START_TEST(test_cli_memory_test) {
    test_unregister_file("/config.yaml");
    register_models(false);
    prior_setup_count = setup_count;
    cli_init();
    if (_i != 0) mock_input_set_reader(request_ram_test_in_menu);
    mock_ram_test_set_handler(observe_ram_test);
    const window_t window = window_create(guarded_screen + 1, MENU_UI_WIDTH, MENU_UI_HEIGHT);
    if (setjmp(ram_test_exit) == 0) {
        if (_i == 0) send_uart("test\r");
        else menu_config_show(&window, &setup_sink, true);
        ck_abort_msg("CLI test returned instead of entering the diagnostic");
    }
    mock_ram_test_set_handler(NULL);
}
END_TEST

// Invalid arguments produce normal CLI usage without requesting the diagnostic.
START_TEST(test_cli_memory_test_arguments) {
    cli_init();
    send_uart("test invalid\r");
    // An unexpected invocation would fail through the fixture's diagnostic stub.
    ck_assert(!input_routing_menu_active());
}
END_TEST

// A newly selected configuration clears old slots even without mount actions.
START_TEST(test_config_without_mount_actions_clears_disks) {
    register_disk("live.d64");
    for (unsigned int slot = 0; slot < MENU_UI_DRIVE_SLOT_COUNT; ++slot)
        ck_assert_int_eq(ieee_drive_try_mount(slot, "live.d64"), DOS_MEDIA_OK);
    load_config(&setup_sink, 0, true);
    ck_assert_uint_eq(mount_count, 0);
    for (unsigned int slot = 0; slot < MENU_UI_DRIVE_SLOT_COUNT; ++slot)
        ck_assert_str_eq(ieee_drive_filename(slot), "");
}
END_TEST

// Restoring preserves media, while a new selection clears it without a mount callback.
START_TEST(test_config_disk_policy_without_mount_callback) {
    register_disk("live.d64");
    ck_assert_int_eq(ieee_drive_try_mount(0, "live.d64"), DOS_MEDIA_OK);
    const setup_sink_t no_mount = {
        .on_set_options = set_options, .system_state = &system_state
    };
    load_config(&no_mount, 0, false);
    ck_assert_str_eq(ieee_drive_filename(0), "live.d64");
    load_config(&no_mount, 0, true);
    ck_assert_str_eq(ieee_drive_filename(0), "");
}
END_TEST

// The filesystem browser can page, resolve, and find entries without a TUI controller.
START_TEST(test_browser_without_ui) {
    for (size_t i = 0; i < TEST_IMAGE_COUNT; i++) {
        char name[32];
        snprintf(name, sizeof(name), "disk%02zu.d64", i);
        make_file(name);
    }
    char folder[PATH_MAX];
    snprintf(folder, sizeof(folder), "%s/folder", directory_root);
    ck_assert_int_eq(mkdir(folder, 0700), 0);
    menu_browser_t browser = {.path = MENU_BROWSER_ROOT, .service = service};
    ck_assert(menu_browser_refresh(&browser));
    ck_assert_uint_eq(browser.count, TEST_IMAGE_COUNT + 1);
    ck_assert(menu_browser_page(&browser, MENU_BROWSER_PAGE_ROWS, MENU_BROWSER_PAGE_ROWS));
    ck_assert_str_eq(browser.entries[0].name, "disk20.d64");
    size_t index;
    ck_assert(menu_browser_find(&browser, "disk64.d64", false, &index));
    char path[SD_PATH_MAX];
    bool directory;
    ck_assert(menu_browser_resolve(&browser, index, path, &directory));
    ck_assert(!directory);
    ck_assert_str_eq(path, "/disks/disk64.d64");
    ck_assert(menu_browser_find(&browser, "folder", true, &index));
    ck_assert(menu_browser_resolve(&browser, index, path, &directory));
    ck_assert(directory);
    snprintf(browser.path, sizeof(browser.path), "%s", path);
    char child[SD_PATH_MAX];
    ck_assert(menu_browser_parent(&browser, child));
    ck_assert_str_eq(child, "folder");
    ck_assert_str_eq(browser.path, MENU_BROWSER_ROOT);
    ck_assert(!menu_browser_parent(&browser, child));
}
END_TEST

// RESET preserves applied disk overrides even if the YAML defaults later disappear.
START_TEST(test_reset_applies_disk_overrides) {
    register_disk("old.d64");
    register_disk("peer.d64");
    test_unregister_file("/config.yaml");
    test_register_file("/config.yaml",
        "configs:\n  - name: Configured\n    setup:\n"
        "      - action: set\n        columns: 80\n"
        "      - action: mount\n        device: 8\n        drive: 0\n        file: old.d64\n"
        "      - action: mount\n        device: 8\n        drive: 1\n        file: peer.d64\n");
    uint8_t* const image = diskimage_test_make_d80();
    test_register_binary_file("/disks/new.d80", image, DISKIMAGE_D80_SIZE, true);
    free(image);
    make_file("new.d80");
    const window_t* const window = menu.controller.window;
    menu_ui_init(&menu, window, service);
    ck_assert_str_eq(ieee_drive_filename(0), "");
    ck_assert_str_eq(ieee_drive_filename(1), "");
    apply_menu();
    setup_count = mount_count = 0;
    memset(applied_names, 0, sizeof(applied_names));
    menu_ui_begin(&menu, window, false);
    ck_assert_str_eq(ieee_drive_filename(0), "old.d64");
    ck_assert_str_eq(ieee_drive_filename(1), "peer.d64");
    open_slot(1);
    move_to(0);
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    open_slot(0);
    move_to(1);
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_str_eq(ieee_drive_filename(0), "new.d80");
    ck_assert_str_eq(ieee_drive_filename(1), "");
    ck_assert_uint_eq(mount_count, 0);
    if (_i != 0) {
        test_unregister_file("/disks/old.d64");
        test_unregister_file("/disks/peer.d64");
    }
    apply_menu();
    ck_assert_uint_eq(setup_count, 1);
    ck_assert_int_eq(system_state.pet_display_columns, pet_display_columns_80);
    ck_assert_uint_eq(mount_count, 0);
    ck_assert_str_eq(ieee_drive_filename(0), "new.d80");
    ck_assert_str_eq(ieee_drive_filename(1), "");
}
END_TEST

// Mounting a different format ejects its partner in either slot without an error screen.
START_TEST(test_mixed_disk_pair_ejects_partner) {
    const unsigned int slot = (unsigned int)_i % DOS_MEDIA_SLOTS_PER_UNIT;
    const unsigned int formats = (unsigned int)_i / DOS_MEDIA_SLOTS_PER_UNIT;
    const diskimage_type_t existing = diskimage_type_d64 + formats / 2u;
    diskimage_type_t replacement = diskimage_type_d64 + formats % 2u;
    if (replacement >= existing) replacement++;
    for (unsigned int index = 0; index < MENU_UI_PAGE_ROWS; index++) {
        char name[32];
        snprintf(name, sizeof(name), "aaa%02u.d64", index);
        make_file(name);
    }
    make_file("candidate.d64");
    register_disk_type("candidate.d64", replacement);
    register_disk_type("peer.d64", existing);
    ck_assert_int_eq(ieee_drive_try_mount(slot ^ 1u, "peer.d64"), DOS_MEDIA_OK);
    if (formats % 2u != 0)
        ck_assert_int_eq(ieee_drive_try_mount(slot, "peer.d64"), DOS_MEDIA_OK);
    const size_t open_files = test_open_file_count();
    open_slot(slot);
    move_to(MENU_UI_PAGE_ROWS + 1);
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_uint_eq(menu.controller.screen_count, 2);
    ck_assert_str_eq(ieee_drive_filename(slot), "candidate.d64");
    ck_assert_str_eq(ieee_drive_filename(slot ^ 1u), "");
    ck_assert_str_eq(menu.error, "");
    ck_assert_uint_eq(mount_count, 0);
    ck_assert_uint_eq(setup_count, 0);
    ck_assert_int_eq(get_cpu(), CPU_READY);
    ck_assert_uint_eq(test_open_file_count(), open_files - (formats % 2u != 0 ? 1 : 0));
}
END_TEST

// Unreadable and unsupported images remain recoverable without accepting the bad choice.
START_TEST(test_disk_probe_error_keeps_picker_open) {
    make_file("bad.d64");
    register_disk("previous.d64");
    ck_assert_int_eq(ieee_drive_try_mount(0, "previous.d64"), DOS_MEDIA_OK);
    if (_i != 0) {
        const uint8_t bytes[DISKIMAGE_SECTOR_SIZE] = {0};
        test_register_binary_file("/disks/bad.d64", bytes, sizeof(bytes), true);
    }
    open_slot(0);
    move_to(1);
    const size_t open_files = test_open_file_count();
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_uint_eq(menu.controller.screen_count, 3);
    ck_assert_str_eq(ieee_drive_filename(0), "previous.d64");
    ck_assert_str_eq(menu.error, _i == 0 ? "Cannot open disk image" : "Unsupported disk image");
    ck_assert_uint_eq(test_open_file_count(), open_files);
    ck_assert_uint_eq(mount_count, 0);
    ck_assert_uint_eq(setup_count, 0);
}
END_TEST

// Busy mounts and ejections retain live media and stay open for a successful retry.
START_TEST(test_busy_disk_change_retries_without_losing_mount) {
    register_disk("old.d64");
    register_disk("new.d64");
    make_file("new.d64");
    ck_assert_int_eq(ieee_drive_try_mount(0, "old.d64"), DOS_MEDIA_OK);
    open_slot(0);
    move_to(_i == 0 ? 0 : 1);
    const size_t open_files = test_open_file_count();
    mock_ieee_enqueue_rx(true, IEEE_CMD_UNLISTEN);
    mock_ieee_enqueue_atn_end();
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_uint_eq(menu.controller.screen_count, 3);
    ck_assert_str_eq(menu.error, "Disk is busy - retry");
    ck_assert_str_eq(ieee_drive_filename(0), "old.d64");
    ck_assert_uint_eq(test_open_file_count(), open_files);
    ieee_drive_task();
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_uint_eq(menu.controller.screen_count, 2);
    ck_assert_str_eq(ieee_drive_filename(0), _i == 0 ? "" : "new.d64");
    ck_assert_uint_eq(setup_count, 0);
    ck_assert_uint_eq(mount_count, 0);
}
END_TEST

// Exercises every image beyond the previous 24-file cutoff in both directions.
START_TEST(test_disk_paging_and_parent_focus) {
    for (unsigned int index = 0; index < TEST_IMAGE_COUNT; index++) {
        char name[32];
        snprintf(name, sizeof(name), "image%02u.d64", TEST_IMAGE_COUNT - 1 - index);
        make_file(name);
    }
    make_file("not-an-image.txt");
    open_slot(7);
    ck_assert_uint_eq(menu.picker_control.list.count, TEST_IMAGE_COUNT + 1);
    tui_controller_render(&menu.controller);
    ck_assert_uint_eq(guarded_screen[1], 0xe1);
    ck_assert_uint_eq(guarded_screen[1 + 39], 0xf6);
    ck_assert_uint_eq(guarded_screen[1 + 2 * MENU_UI_WIDTH + 39], 0xa0);
    ck_assert_uint_eq(guarded_screen[1 + 9 * MENU_UI_WIDTH + 39], 0x66);
    ck_assert_uint_eq(guarded_screen[1 + 23 * MENU_UI_WIDTH], 0x6d);
    ck_assert_uint_eq(guarded_screen[1 + 23 * MENU_UI_WIDTH + 39], 0x7d);
    for (size_t index = 1; index <= TEST_IMAGE_COUNT; index++) {
        move_to(index);
        const size_t row = index - menu.page_first;
        char expected[32];
        snprintf(expected, sizeof(expected), "image%02zu.d64", index - 1);
        ck_assert_str_eq(menu.labels[row], expected);
    }
    const unsigned int scanned = service_count;
    tui_controller_render(&menu.controller);
    tui_controller_render(&menu.controller);
    ck_assert_uint_eq(service_count, scanned);
    for (size_t index = TEST_IMAGE_COUNT; index > 0; index--) {
        move_to(index);
        char expected[32];
        snprintf(expected, sizeof(expected), "image%02zu.d64", index - 1);
        ck_assert_str_eq(menu.labels[index - menu.page_first], expected);
    }
    tui_controller_dispatch(&menu.controller, tui_action_back);
    ck_assert_uint_eq(menu.controller.focused_component, 0);
    ck_assert_uint_eq(menu.drive_controls[0].list.selected, 7);
    open_slot(7);
    ck_assert_uint_eq(menu.picker_control.list.selected, 0);
    ck_assert_uint_eq(menu.picker_control.list.first_visible, 0);
}
END_TEST

// Cursor edges and Left/Right expose the same overlapping disk pages in both directions.
START_TEST(test_disk_overlapping_pages) {
    for (unsigned int index = 0; index < TEST_IMAGE_COUNT; index++) {
        char name[32];
        snprintf(name, sizeof(name), "image%02u.d64", index);
        make_file(name);
    }
    open_slot(0);
    tui_list_t* const list = &menu.picker_control.list;
    const size_t stride = MENU_UI_PAGE_ROWS - 1;
    move_to(stride);
    char boundary[MENU_UI_WIDTH + 1];
    snprintf(boundary, sizeof(boundary), "%s", menu.labels[stride]);
    tui_controller_dispatch(&menu.controller, _i == 0 ? tui_action_down
        : tui_action_page_down);
    ck_assert_uint_eq(list->first_visible, stride);
    ck_assert_uint_eq(list->selected, stride + 1);
    ck_assert_str_eq(menu.labels[0], boundary);
    ck_assert_str_eq(menu.labels[1], "image20.d64");
    ck_assert(list->page_valid);

    move_to(stride);
    tui_controller_dispatch(&menu.controller, _i == 0 ? tui_action_up
        : tui_action_page_up);
    ck_assert_uint_eq(list->first_visible, 0);
    ck_assert_uint_eq(list->selected, stride - 1);
    ck_assert_str_eq(menu.labels[stride], boundary);
    ck_assert_str_eq(menu.labels[stride - 1], "image18.d64");

    // The final partial page keeps its leading overlap rather than sliding to fill the screen.
    move_to(3 * stride);
    tui_controller_dispatch(&menu.controller, _i == 0 ? tui_action_down : tui_action_page_down);
    ck_assert_uint_eq(list->first_visible, 3 * stride);
    ck_assert_uint_eq(list->selected, 3 * stride + 1);
    move_to(TEST_IMAGE_COUNT);
    tui_controller_dispatch(&menu.controller, tui_action_page_down);
    ck_assert_uint_eq(list->first_visible, 3 * stride);
    ck_assert_uint_eq(list->selected, TEST_IMAGE_COUNT);
    tui_controller_dispatch(&menu.controller, tui_action_page_up);
    ck_assert_uint_eq(list->first_visible, 2 * stride);
    ck_assert_uint_eq(list->selected, 3 * stride - 1);

    // Existing cursor wrap remains visible, while paging at the beginning stops there.
    move_to(0);
    tui_controller_dispatch(&menu.controller, tui_action_page_up);
    ck_assert_uint_eq(list->first_visible, 0);
    ck_assert_uint_eq(list->selected, 0);
    tui_controller_dispatch(&menu.controller, tui_action_up);
    ck_assert_uint_eq(list->first_visible, 3 * stride);
    ck_assert_uint_eq(list->selected, TEST_IMAGE_COUNT);
    tui_controller_dispatch(&menu.controller, tui_action_down);
    ck_assert_uint_eq(list->first_visible, 0);
    ck_assert_uint_eq(list->selected, 0);

    // Reopening a slot discards even a distant cursor and viewport.
    move_to(TEST_IMAGE_COUNT);
    tui_controller_dispatch(&menu.controller, tui_action_back);
    open_slot(0);
    ck_assert_uint_eq(list->first_visible, 0);
    ck_assert_uint_eq(list->selected, 0);
}
END_TEST

// Adjacent pages scan once in either direction, using directory types or stat fallback.
START_TEST(test_disk_scan_cost_and_types) {
    unknown_entry_types = _i != 0;
    for (unsigned int index = 0; index < TEST_IMAGE_COUNT; index++) {
        char name[32];
        snprintf(name, sizeof(name), "image%02u.d64", TEST_IMAGE_COUNT - 1 - index);
        make_file(name);
    }
    char folder[PATH_MAX];
    snprintf(folder, sizeof(folder), "%s/folder", directory_root);
    ck_assert_int_eq(mkdir(folder, 0700), 0);
    make_file("ignored.txt");
    char link[PATH_MAX];
    snprintf(link, sizeof(link), "%s/link.d64", directory_root);
    ck_assert_int_eq(symlink("/tmp", link), 0);
    open_slot(0);
    ck_assert_uint_eq(menu.picker_control.list.count, TEST_IMAGE_COUNT + 2);
    ck_assert_str_eq(menu.labels[1], "[folder]");
    for (unsigned int page = 0; page < 3; page++)
        tui_controller_dispatch(&menu.controller, tui_action_page_down);

    // Measure a deep backward page, then return forward across the same boundary.
    for (unsigned int direction = 0; direction < 2; direction++) {
        directory_scans = metadata_reads = 0;
        tui_controller_dispatch(&menu.controller,
            direction == 0 ? tui_action_page_up : tui_action_page_down);
        ck_assert(menu.picker_control.list.page_valid);
        ck_assert_uint_eq(directory_scans, 1);
        if (unknown_entry_types) ck_assert_uint_gt(metadata_reads, TEST_IMAGE_COUNT);
        else ck_assert_uint_eq(metadata_reads, 1); // Only the directory's root is checked.
        const size_t first = (direction == 0 ? 2 : 3) * (MENU_UI_PAGE_ROWS - 1);
        ck_assert_uint_eq(menu.picker_control.list.first_visible, first);
        for (size_t row = 0; row < MENU_UI_PAGE_ROWS && first + row < TEST_IMAGE_COUNT + 2; row++) {
            char expected[32];
            snprintf(expected, sizeof(expected), "image%02zu.d64", first + row - 2);
            ck_assert_str_eq(menu.labels[row], expected);
        }
    }
}
END_TEST

// Confirms folders sort first, nested Back selects the child, and mounts retain paths.
START_TEST(test_nested_browsing_mount_and_eject) {
    char nested[SD_PATH_MAX];
    snprintf(nested, sizeof(nested), "%s/superpet", directory_root);
    ck_assert_int_eq(mkdir(nested, 0700), 0);
    make_file("root.d64");
    make_file("superpet/os9.d64");
    register_disk("superpet/os9.d64");
    open_slot(3);
    move_to(1);
    ck_assert_str_eq(menu.labels[1], "[superpet]");
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_str_eq(menu.browser.path, "/disks/superpet");
    ck_assert_str_eq(menu.labels[0], "[Eject]");
    ck_assert_str_eq(menu.labels[1], "[..]");
    ck_assert_str_eq(menu.labels[2], "os9.d64");
    tui_controller_dispatch(&menu.controller, tui_action_back);
    ck_assert_str_eq(menu.browser.path, "/disks");
    ck_assert_uint_eq(menu.picker_control.list.selected, 1);
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    move_to(2);
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_uint_eq(menu.controller.screen_count, 2);
    ck_assert_str_eq(ieee_drive_filename(3), "superpet/os9.d64");
    ck_assert_uint_eq(mount_count, 0);
    open_slot(3);
    ck_assert_str_eq(menu.browser.path, "/disks/superpet");
    ck_assert_uint_eq(menu.picker_control.list.selected, 0);
    ck_assert_uint_eq(menu.picker_control.list.first_visible, 0);
    move_to(0);
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_str_eq(ieee_drive_filename(3), "");
    ck_assert(tui_controller_is_active(&menu.controller));
    apply_menu();
    ck_assert_uint_eq(mount_count, 0);
}
END_TEST

// A picked disk is live immediately and RESET neither reopens nor replaces it.
START_TEST(test_disk_choice_is_immediate) {
    make_file("good.d64");
    make_file("other.d64");
    register_disk("good.d64");
    ck_assert_int_eq(ieee_drive_try_mount(0, "good.d64"), DOS_MEDIA_OK);
    register_disk("other.d64");
    open_slot(0);
    ck_assert_uint_eq(menu.picker_control.list.selected, 0);
    move_to(2);
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_uint_eq(menu.controller.screen_count, 2);
    ck_assert_str_eq(ieee_drive_filename(0), "other.d64");
    ck_assert_uint_eq(mount_count, 0);
    const size_t open_files = test_open_file_count();
    apply_menu();
    ck_assert_str_eq(ieee_drive_filename(0), "other.d64");
    ck_assert_uint_eq(test_open_file_count(), open_files);
    ck_assert_uint_eq(mount_count, 0);
}
END_TEST

// Rejects directory read failures and host symlinks without hiding them as emptiness.
START_TEST(test_browser_errors_and_root_confinement) {
    char link[SD_PATH_MAX];
    snprintf(link, sizeof(link), "%s/outside", directory_root);
    ck_assert_int_eq(symlink("/tmp", link), 0);
    open_slot(0);
    ck_assert_uint_eq(menu.picker_control.list.count, 1);
    tui_controller_dispatch(&menu.controller, tui_action_back);
    ck_assert_uint_eq(menu.controller.screen_count, 2);
    fail_readdir = true;
    open_slot(0);
    ck_assert_uint_eq(menu.picker_control.list.count, 0);
    ck_assert(!menu.picker_control.list.page_valid);
    ck_assert_str_eq(menu.browser.error, "Cannot read directory");
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_uint_eq(menu.controller.screen_count, 3);
    tui_controller_dispatch(&menu.controller, tui_action_back);
    ck_assert_uint_eq(menu.controller.screen_count, 2);
}
END_TEST

// A transient scan failure stays recoverable on open, descent, and parent navigation.
START_TEST(test_transient_directory_scan_failure) {
    char folder[PATH_MAX];
    snprintf(folder, sizeof(folder), "%s/child", directory_root);
    ck_assert_int_eq(mkdir(folder, 0700), 0);
    make_file("child/disk.d64");
    if (_i == 0) {
        fail_next_readdir = true;
        open_slot(0);
    } else {
        open_slot(0);
        move_to(1);
        if (_i == 2) {
            tui_controller_dispatch(&menu.controller, tui_action_activate);
            ck_assert_str_eq(menu.browser.path, "/disks/child");
        }
        fail_next_readdir = true;
        tui_controller_dispatch(&menu.controller, _i == 1 ? tui_action_activate : tui_action_back);
    }

    ck_assert_uint_eq(menu.picker_control.list.count, 0);
    ck_assert(!menu.picker_control.list.page_valid);
    ck_assert_str_eq(menu.browser.error, "Cannot read directory");
    tui_controller_render(&menu.controller);
    tui_controller_dispatch(&menu.controller, tui_action_back);
    if (menu.controller.screen_count == 3)
        tui_controller_dispatch(&menu.controller, tui_action_back);
    ck_assert_uint_eq(menu.controller.screen_count, 2);
    open_slot(0);
    ck_assert_uint_gt(menu.picker_control.list.count, 0);
    ck_assert(menu.picker_control.list.page_valid);
    ck_assert_str_eq(menu.browser.error, "");
}
END_TEST

// UART CRLF must not eject a disk when its CR opens the picker.
START_TEST(test_uart_crlf_across_picker_transition) {
    make_file("disk.d64");
    register_disk("disk.d64");
    ck_assert_int_eq(ieee_drive_try_mount(0, "disk.d64"), DOS_MEDIA_OK);
    open_slot(0);
    tui_controller_dispatch(&menu.controller, tui_action_back);
    ck_assert_uint_eq(menu.controller.screen_count, 2);

    input_uart_filter_t filter = {0};
    ck_assert(input_uart_accept(&filter, '\r'));
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    if (input_uart_accept(&filter, '\n'))
        tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_uint_eq(menu.controller.screen_count, 3);
    ck_assert_str_eq(ieee_drive_filename(0), "disk.d64");

    // A separate keyboard Enter remains an independent activation.
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_uint_eq(menu.controller.screen_count, 2);
    ck_assert_str_eq(ieee_drive_filename(0), "");
}
END_TEST

// Filtering spans UART polls and routing changes with one Enter per CRLF pair.
START_TEST(test_uart_line_endings) {
    input_uart_filter_t filter = {0};
    ck_assert(input_uart_accept(&filter, '\n'));
    ck_assert(input_uart_accept(&filter, '\r'));
    ck_assert(!input_uart_accept(&filter, '\n'));
    ck_assert(input_uart_accept(&filter, '\r'));
    ck_assert(input_uart_accept(&filter, '\r'));
    ck_assert(!input_uart_accept(&filter, '\n'));
    ck_assert(input_uart_accept(&filter, '\r'));
    ck_assert(input_uart_accept(&filter, 'x'));
    ck_assert(input_uart_accept(&filter, '\n'));
    ck_assert(input_uart_accept(&filter, '\r'));
    ck_assert(!input_uart_accept(&filter, '\n'));
    ck_assert(input_uart_accept(&filter, '\r'));
    ck_assert(!input_uart_accept(&filter, '\n'));
}
END_TEST

// Picker filtering follows native media support, including case-insensitive D82.
START_TEST(test_native_formats_include_d82_and_exclude_hdd) {
    make_file("alpha.D64");
    make_file("beta.d80");
    make_file("gamma.D82");
    make_file("legacy.hdd");
    uint8_t* image = diskimage_test_make_d80();
    uint8_t* const expanded = realloc(image, DISKIMAGE_D82_SIZE);
    ck_assert_ptr_nonnull(expanded);
    image = expanded;
    memset(image + DISKIMAGE_D80_SIZE, 0, DISKIMAGE_D80_SIZE);
    test_register_binary_file("/disks/gamma.D82", image, DISKIMAGE_D82_SIZE, true);
    free(image);
    open_slot(0);
    ck_assert_uint_eq(menu.picker_control.list.count, 4);
    ck_assert_str_eq(menu.labels[1], "alpha.D64");
    ck_assert_str_eq(menu.labels[2], "beta.d80");
    ck_assert_str_eq(menu.labels[3], "gamma.D82");
    move_to(3);
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_uint_eq(menu.controller.screen_count, 2);
    ck_assert_str_eq(ieee_drive_filename(0), "gamma.D82");
    ck_assert_uint_eq(mount_count, 0);
    apply_menu();
    ck_assert_str_eq(ieee_drive_filename(0), "gamma.D82");
}
END_TEST

// Missing storage is reported separately from a successful empty directory.
START_TEST(test_empty_and_missing_directories) {
    open_slot(0);
    ck_assert_str_eq(menu.error, "");
    ck_assert_uint_eq(menu.picker_control.list.count, 1);
    tui_controller_render(&menu.controller);
    expect_text(1, 1, "No images or directories", false);
    tui_controller_dispatch(&menu.controller, tui_action_back);
    ck_assert_int_eq(rmdir(directory_root), 0);
    open_slot(0);
    ck_assert_str_eq(menu.browser.error, "Directory is unavailable");
    ck_assert_uint_eq(menu.picker_control.list.count, 0);
    ck_assert(!menu.picker_control.list.page_valid);
    tui_controller_dispatch(&menu.controller, tui_action_back);
    ck_assert_int_eq(mkdir(directory_root, 0700), 0);
}
END_TEST

// Directory depth must not consume the TUI's borrowed-screen stack.
START_TEST(test_deep_directories_do_not_grow_screen_stack) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", directory_root);
    const unsigned int depth = TUI_SCREEN_STACK_CAPACITY + 2;
    for (unsigned int level = 0; level < depth; level++) {
        strcat(path, "/child");
        ck_assert_int_eq(mkdir(path, 0700), 0);
    }
    open_slot(0);
    for (unsigned int level = 0; level < depth; level++) {
        move_to(level == 0 ? 1 : 2);
        tui_controller_dispatch(&menu.controller, tui_action_activate);
        ck_assert_uint_eq(menu.controller.screen_count, 3);
        ck_assert_str_eq(menu.error, "");
    }
    ck_assert_uint_eq(menu.picker_control.list.count, 2);
    for (unsigned int level = 0; level < depth; level++) {
        tui_controller_dispatch(&menu.controller, tui_action_back);
        ck_assert_uint_eq(menu.controller.screen_count, 3);
    }
    ck_assert_str_eq(menu.browser.path, "/disks");
    tui_controller_dispatch(&menu.controller, tui_action_back);
    ck_assert_uint_eq(menu.controller.screen_count, 2);
}
END_TEST

// Selects distant configurations without disrupting menu.rom's display and keyboard scan.
START_TEST(test_model_paging_and_change) {
    make_file("old.d64");
    register_disk("old.d64");
    ck_assert_int_eq(ieee_drive_try_mount(0, "old.d64"), DOS_MEDIA_OK);
    menu.controller.focused_component = 2;
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert(menu.choosing_model);
    ck_assert_uint_eq(menu.picker_control.list.count, TEST_MODEL_COUNT);
    ck_assert_uint_eq(menu.picker_control.list.selected, 1);
    move_to(TEST_MODEL_COUNT - 1);
    ck_assert_str_eq(menu.labels[menu.picker_control.list.selected - menu.page_first], "Model 46");
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert(!tui_controller_is_active(&menu.controller));
    ck_assert_int_eq(menu.config_index, TEST_MODEL_COUNT - 1);
    ck_assert_uint_eq(setup_count, 0);
    ck_assert_int_eq(get_cpu(), CPU_READY);
    apply_menu();
    ck_assert_int_eq(system_state.pet_display_columns, pet_display_columns_40);
    ck_assert_uint_eq(mount_count, 0);
    ck_assert_uint_eq(setup_count, 1);
    ck_assert_int_eq(get_cpu(), CPU_HALT);
}
END_TEST

// Back cancels configuration selection, but choosing even the current config exits for reset.
START_TEST(test_model_cancel_and_current_selection) {
    menu.controller.focused_component = 2;
    tui_controller_dispatch(&menu.controller, tui_action_activate);
    if (_i == 0) tui_controller_dispatch(&menu.controller, tui_action_back);
    else tui_controller_dispatch(&menu.controller, tui_action_activate);
    ck_assert_uint_eq(menu.controller.screen_count, _i == 0 ? 1 : 2);
    ck_assert_int_eq(tui_controller_is_active(&menu.controller), _i == 0);
    ck_assert_uint_eq(setup_count, 0);
    ck_assert_int_eq(get_cpu(), CPU_READY);
}
END_TEST

// Confirms PET, USB, and terminal keycodes share one navigation policy.
START_TEST(test_menu_key_actions) {
    ck_assert_int_eq(input_menu_bind_terminal('\b'), tui_action_back);
    ck_assert_int_eq(input_menu_bind_terminal(0x7f), tui_action_back);
    ck_assert_int_eq(input_menu_bind_terminal('\e'), tui_action_back);
    ck_assert_int_eq(input_menu_bind_terminal('\r'), tui_action_activate);
    ck_assert_int_eq(input_menu_bind_terminal(KEY_DOWN), tui_action_down);
    input_routing_set_menu_active(true);
    input_routing_gesture(INPUT_SOURCE_BUTTON, input_gesture_button_short);
    ck_assert_int_eq(input_routing_navigation(), tui_action_down);
    input_routing_gesture(INPUT_SOURCE_BUTTON, input_gesture_button_long);
    ck_assert_int_eq(input_routing_navigation(), tui_action_activate);
    input_routing_gesture(INPUT_SOURCE_PET, input_gesture_dual_shift);
    ck_assert_int_eq(input_routing_navigation(), tui_action_none);
    ck_assert_int_eq(input_routing_pending_request().action, input_system_none);
    input_routing_set_menu_active(false);
    ck_assert_int_eq(input_menu_bind_terminal(KEY_LEFT), tui_action_page_up);
    ck_assert_int_eq(input_menu_bind_terminal(KEY_RIGHT), tui_action_page_down);
}
END_TEST

Suite* menu_ui_suite(void) {
    Suite* const suite = suite_create("menu_ui");
    TCase* const cases = tcase_create("dashboard");
    tcase_add_checked_fixture(cases, setup, teardown);
    tcase_add_test(cases, test_dashboard_layout_and_navigation);
    tcase_add_loop_test(cases, test_drive_back_button, 0, 2);
    tcase_add_test(cases, test_boot_configuration_picker);
    tcase_add_loop_test(cases, test_configuration_overlapping_pages, 0, 2);
    tcase_add_loop_test(cases, test_picker_paging_artwork, 0, 4);
    tcase_add_loop_test(cases, test_disk_page_counter, 0, 6);
    tcase_add_test(cases, test_drive_label_suffixes);
    tcase_add_test(cases, test_on_demand_back_and_reentry);
    tcase_add_loop_test(cases, test_dashboard_exit_outcome, 0, 2);
    tcase_add_loop_test(cases, test_charset_transition_blanks_both_displays, 0, 2);
    tcase_add_loop_test(cases, test_boot_loop_waits_for_reset, 0, 2);
    tcase_add_loop_test(cases, test_menu_preserves_user_terminal_mode, 0, 4);
    tcase_add_test(cases, test_terminal_session_rendering);
    tcase_add_test(cases, test_terminal_navigation_actions);
    tcase_add_test(cases, test_remote_controls_boot_menu);
    tcase_add_test(cases, test_uart_remote_cli_and_log);
    tcase_add_test(cases, test_uart_transition_boundaries);
    tcase_add_test(cases, test_menu_loop_activates_held_button);
    tcase_add_test(cases, test_menu_loop_ignores_shift_chord);
    tcase_add_test(cases, test_configuration_persists_across_sessions);
    tcase_add_test(cases, test_session_disk_policy);
    tcase_add_loop_test(cases, test_default_autoboot_and_reentry, 0, 2);
    tcase_add_loop_test(cases, test_cli_memory_test, 0, 2);
    tcase_add_test(cases, test_cli_memory_test_arguments);
    tcase_add_test(cases, test_config_disk_policy_without_mount_callback);
    tcase_add_test(cases, test_config_without_mount_actions_clears_disks);
    tcase_add_test(cases, test_browser_without_ui);
    tcase_add_loop_test(cases, test_reset_applies_disk_overrides, 0, 2);
    tcase_add_loop_test(cases, test_mixed_disk_pair_ejects_partner, 0, 12);
    tcase_add_loop_test(cases, test_disk_probe_error_keeps_picker_open, 0, 2);
    tcase_add_loop_test(cases, test_busy_disk_change_retries_without_losing_mount, 0, 2);
    tcase_add_test(cases, test_disk_paging_and_parent_focus);
    tcase_add_loop_test(cases, test_disk_overlapping_pages, 0, 2);
    tcase_add_loop_test(cases, test_disk_scan_cost_and_types, 0, 2);
    tcase_add_test(cases, test_nested_browsing_mount_and_eject);
    tcase_add_test(cases, test_disk_choice_is_immediate);
    tcase_add_test(cases, test_browser_errors_and_root_confinement);
    tcase_add_loop_test(cases, test_transient_directory_scan_failure, 0, 3);
    tcase_add_test(cases, test_uart_crlf_across_picker_transition);
    tcase_add_test(cases, test_uart_line_endings);
    tcase_add_test(cases, test_native_formats_include_d82_and_exclude_hdd);
    tcase_add_test(cases, test_empty_and_missing_directories);
    tcase_add_test(cases, test_deep_directories_do_not_grow_screen_stack);
    tcase_add_test(cases, test_model_paging_and_change);
    tcase_add_loop_test(cases, test_model_cancel_and_current_selection, 0, 2);
    tcase_add_test(cases, test_menu_key_actions);
    tcase_add_test(cases, test_long_names_and_overlong_paths);
    tcase_add_test(cases, test_replaced_entry_does_not_escape_root);
    tcase_add_test(cases, test_model_change_loads_configured_disks);
    suite_add_tcase(suite, cases);
    return suite;
}

// Isolates fatal browser invariants exercised through the menu.
Suite* menu_ui_fatal_suite(void) {
    Suite* const suite = suite_create("menu_ui-fatal");
    TCase* const cases = tcase_create("dashboard");
    tcase_add_checked_fixture(cases, setup, teardown);
    tcase_add_test_raise_signal(cases, test_directory_change_blocks_activation, SIGABRT);
    suite_add_tcase(suite, cases);
    return suite;
}
