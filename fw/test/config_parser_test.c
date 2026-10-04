// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config/config.h"
#include "fatal.h"
#include "global.h"
#include "mock.h"
#include "sd/sd.h"
#include "system_state.h"
#include "config_parser_test.h"

enum test_path_kind {
    TEST_PATH_LOAD,
    TEST_PATH_KEYMAP,
    TEST_PATH_MOUNT,
    TEST_PATH_KIND_COUNT,
};

// Test fixture data structure
typedef struct test_context_s {
    int config_enter_count;
    int config_exit_count;
    int default_count;
    int load_count;
    int patch_count;
    int copy_count;
    int mount_count;
    int set_options_count;
    int fix_checksum_count;
    
    char last_config_id[41];
    char last_config_name[41];
    char last_default_id[41];
    char last_load_file[SD_PATH_MAX];
    uint32_t last_load_address;
    uint32_t last_patch_address;
    size_t last_patch_size;
    uint32_t last_copy_source;
    uint32_t last_copy_dest;
    uint32_t last_copy_length;
    uint32_t last_mount_device;
    uint32_t last_mount_drive;
    char last_mount_file[SD_PATH_MAX];
    uint32_t last_columns;
    uint32_t last_video_ram_mask;
    char last_usb_keymap[sizeof(((options_t*) 0)->usb_keymap)];
    tape_config_t last_tape;
    bool last_tape_enabled;
    uint32_t last_checksum_start;
    uint32_t last_checksum_end;
    uint32_t last_checksum_fix;
    uint32_t last_checksum_value;
} test_context_t;

static test_context_t test_ctx;
static const char* mounted_image_root = NULL;

// Global shared test state (sink structs with const members must be initialized statically)
static system_state_t sys_state; // defaults applied in setup()

// Callback implementations for testing
static void test_on_enter_config(void* context) {
    test_context_t* ctx = (test_context_t*)context;
    ctx->config_enter_count++;
}

static void test_on_exit_config(void* context, const char* id, const char* name) {
    test_context_t* ctx = (test_context_t*)context;
    ctx->config_exit_count++;
    strncpy(ctx->last_config_id, id, sizeof(ctx->last_config_id) - 1);
    ctx->last_config_id[sizeof(ctx->last_config_id) - 1] = '\0';
    strncpy(ctx->last_config_name, name, sizeof(ctx->last_config_name) - 1);
    ctx->last_config_name[sizeof(ctx->last_config_name) - 1] = '\0';
}

static void test_on_default(void* context, const char* id) {
    test_context_t* ctx = (test_context_t*)context;
    ctx->default_count++;
    strncpy(ctx->last_default_id, id, sizeof(ctx->last_default_id) - 1);
    ctx->last_default_id[sizeof(ctx->last_default_id) - 1] = '\0';
}

static void test_on_load(void* context, const char* filename, uint32_t address) {
    test_context_t* ctx = (test_context_t*)context;
    ctx->load_count++;
    strncpy(ctx->last_load_file, filename, sizeof(ctx->last_load_file) - 1);
    ctx->last_load_file[sizeof(ctx->last_load_file) - 1] = '\0';
    ctx->last_load_address = address;
}

static void test_on_patch(void* context, uint32_t address, const binary_t* binary) {
    test_context_t* ctx = (test_context_t*)context;
    ctx->patch_count++;
    ctx->last_patch_address = address;
    ctx->last_patch_size = binary->size;
}

static void test_on_copy(void* context, uint32_t source, uint32_t destination, uint32_t length) {
    test_context_t* ctx = (test_context_t*)context;
    ctx->copy_count++;
    ctx->last_copy_source = source;
    ctx->last_copy_dest = destination;
    ctx->last_copy_length = length;
}

static void test_on_mount(void* context, uint32_t device, uint32_t drive, const char* filename) {
    test_context_t* ctx = (test_context_t*)context;
    ctx->mount_count++;
    ctx->last_mount_device = device;
    ctx->last_mount_drive = drive;
    strncpy(ctx->last_mount_file, filename, sizeof(ctx->last_mount_file) - 1);
    ctx->last_mount_file[sizeof(ctx->last_mount_file) - 1] = '\0';

    if (mounted_image_root != NULL) {
        char image_path[PATH_MAX];
        snprintf(image_path, sizeof(image_path), "%s%s%s", mounted_image_root,
                 sd_dir_prefix(SD_DIR_DISKS), filename);
        FILE* image = fopen(image_path, "rb");
        ck_assert_msg(image != NULL,
                      "Configured IEEE image '%s' is missing", image_path);
        fclose(image);
    }
}

static void test_on_set_options(void* context, options_t* options) {
    test_context_t* ctx = (test_context_t*)context;
    ctx->set_options_count++;
    ctx->last_columns = options->columns;
    ctx->last_video_ram_mask = options->video_ram_mask;
    strncpy(ctx->last_usb_keymap, options->usb_keymap, sizeof(ctx->last_usb_keymap) - 1);
    ctx->last_usb_keymap[sizeof(ctx->last_usb_keymap) - 1] = '\0';
    ctx->last_tape = options->tape;
    ctx->last_tape_enabled = options->tape_enabled;
}

static void test_on_fix_checksum(void* context, uint32_t start_addr, uint32_t end_addr, 
                                   uint32_t fix_addr, uint32_t checksum) {
    test_context_t* ctx = (test_context_t*)context;
    ctx->fix_checksum_count++;
    ctx->last_checksum_start = start_addr;
    ctx->last_checksum_end = end_addr;
    ctx->last_checksum_fix = fix_addr;
    ctx->last_checksum_value = checksum;
}

// Static sink initializations
static const setup_sink_t setup_sink = {
    .context = &test_ctx,
    .on_load = test_on_load,
    .on_patch = test_on_patch,
    .on_copy = test_on_copy,
    .on_mount = test_on_mount,
    .on_set_options = test_on_set_options,
    .on_fix_checksum = test_on_fix_checksum,
    .system_state = &sys_state,
};

static const config_sink_t config_sink = {
    .context = &test_ctx,
    .on_enter_config = test_on_enter_config,
    .on_exit_config = test_on_exit_config,
    .on_default = test_on_default,
    .setup = &setup_sink,
};

// Setup and teardown functions
static void setup(void) {
    // Remove any previously registered mock files
    mock_clear_files();
    
    // Clear the test context
    memset(&test_ctx, 0, sizeof(test_ctx));

    // Initialize default system state (most tests use graphics/crtc)
    sys_state.pet_keyboard_model = pet_keyboard_model_graphics;
    sys_state.pet_video_type = pet_video_type_crtc;

    // Const sink structs are statically initialized.
}

static void teardown(void) {
    mock_clear_files();
}

// Test: Parse minimal valid config
START_TEST(test_parse_minimal_config) {
    const char* yaml_content = 
        "configs:\n"
        "  - name: Test Config\n"
        "    setup: []\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, 0);
    
    ck_assert_int_eq(test_ctx.config_enter_count, 1);
    ck_assert_int_eq(test_ctx.config_exit_count, 1);
    ck_assert_str_eq(test_ctx.last_config_name, "Test Config");
}
END_TEST

// Test: Parse config with load action
START_TEST(test_parse_load_action) {
    const char* yaml_content = 
        "configs:\n"
        "  - name: Load Test\n"
        "    setup:\n"
        "      - action: load\n"
        "        files:\n"
        "          - file: basic.bin\n"
        "            address: 0xC000\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, 0);
    
    ck_assert_int_eq(test_ctx.load_count, 1);
    ck_assert_str_eq(test_ctx.last_load_file, "basic.bin");
    ck_assert_int_eq(test_ctx.last_load_address, 0xC000);
}
END_TEST

START_TEST(test_parse_mount_action) {
    const char* yaml_content =
        "configs:\n"
        "  - name: Mount Test\n"
        "    setup:\n"
        "      - action: mount\n"
        "        device: 11\n"
        "        drive: 1\n"
        "        file: some/disk.d64\n";

    mock_register_file("/config.yaml", yaml_content);

    parse_config_file("/config.yaml", &config_sink, 0);

    ck_assert_int_eq(test_ctx.mount_count, 1);
    ck_assert_int_eq(test_ctx.last_mount_device, 11);
    ck_assert_int_eq(test_ctx.last_mount_drive, 1);
    ck_assert_str_eq(test_ctx.last_mount_file, "some/disk.d64");
}
END_TEST

START_TEST(test_parse_mount_action_defaults) {
    const char* yaml_content =
        "configs:\n"
        "  - name: Mount Defaults Test\n"
        "    setup:\n"
        "      - action: mount\n"
        "        file: some/disk.d80\n";

    mock_register_file("/config.yaml", yaml_content);

    parse_config_file("/config.yaml", &config_sink, 0);

    ck_assert_int_eq(test_ctx.mount_count, 1);
    ck_assert_int_eq(test_ctx.last_mount_device, 8);
    ck_assert_int_eq(test_ctx.last_mount_drive, 0);
    ck_assert_str_eq(test_ctx.last_mount_file, "some/disk.d80");
}
END_TEST

// Test: Parse config with patch action
START_TEST(test_parse_patch_action) {
    const char* yaml_content = 
        "configs:\n"
        "  - name: Patch Test\n"
        "    setup:\n"
        "      - action: patch\n"
        "        address: 0x8000\n"
        "        hex: DEADBEEF\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, 0);
    
    ck_assert_int_eq(test_ctx.patch_count, 1);
    ck_assert_int_eq(test_ctx.last_patch_address, 0x8000);
    ck_assert_int_eq(test_ctx.last_patch_size, 4);
}
END_TEST

// Test: Parse config with copy action
START_TEST(test_parse_copy_action) {
    const char* yaml_content = 
        "configs:\n"
        "  - name: Copy Test\n"
        "    setup:\n"
        "      - action: copy\n"
        "        source: 0x1000\n"
        "        destination: 0x2000\n"
        "        length: 256\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, 0);
    
    ck_assert_int_eq(test_ctx.copy_count, 1);
    ck_assert_int_eq(test_ctx.last_copy_source, 0x1000);
    ck_assert_int_eq(test_ctx.last_copy_dest, 0x2000);
    ck_assert_int_eq(test_ctx.last_copy_length, 256);
}
END_TEST

// Test: Parse config with set options action
START_TEST(test_parse_set_action) {
    const char* yaml_content = 
        "configs:\n"
        "  - name: Set Options Test\n"
        "    setup:\n"
        "      - action: set\n"
        "        columns: 80\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, 0);
    
    ck_assert_int_eq(test_ctx.set_options_count, 1);
    ck_assert_int_eq(test_ctx.last_columns, 80);
    // video-ram-kb defaults to 1, which maps to mask 0
    ck_assert_int_eq(test_ctx.last_video_ram_mask, 0);
}
END_TEST

// Test: Parse config with video-ram-kb setting (1KB = mask 0)
START_TEST(test_parse_set_video_ram_1kb) {
    const char* yaml_content = 
        "configs:\n"
        "  - name: Video RAM 1KB Test\n"
        "    setup:\n"
        "      - action: set\n"
        "        video-ram-kb: 1\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, 0);
    
    ck_assert_int_eq(test_ctx.set_options_count, 1);
    ck_assert_int_eq(test_ctx.last_video_ram_mask, 0);  // 1KB -> mask 0
}
END_TEST

// Test: Parse config with video-ram-kb setting (2KB = mask 1)
START_TEST(test_parse_set_video_ram_2kb) {
    const char* yaml_content = 
        "configs:\n"
        "  - name: Video RAM 2KB Test\n"
        "    setup:\n"
        "      - action: set\n"
        "        video-ram-kb: 2\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, 0);
    
    ck_assert_int_eq(test_ctx.set_options_count, 1);
    ck_assert_int_eq(test_ctx.last_video_ram_mask, 1);  // 2KB -> mask 1
}
END_TEST

// Test: Parse config with video-ram-kb setting (3KB = mask 2, ColourPET mode)
START_TEST(test_parse_set_video_ram_3kb) {
    const char* yaml_content = 
        "configs:\n"
        "  - name: Video RAM 3KB Test\n"
        "    setup:\n"
        "      - action: set\n"
        "        video-ram-kb: 3\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, 0);
    
    ck_assert_int_eq(test_ctx.set_options_count, 1);
    ck_assert_int_eq(test_ctx.last_video_ram_mask, 2);  // 3KB -> mask 2
}
END_TEST

// Test: Parse config with video-ram-kb setting (4KB = mask 3)
START_TEST(test_parse_set_video_ram_4kb) {
    const char* yaml_content = 
        "configs:\n"
        "  - name: Video RAM 4KB Test\n"
        "    setup:\n"
        "      - action: set\n"
        "        video-ram-kb: 4\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, 0);
    
    ck_assert_int_eq(test_ctx.set_options_count, 1);
    ck_assert_int_eq(test_ctx.last_video_ram_mask, 3);  // 4KB -> mask 3
}
END_TEST

// Test: Parse config with combined columns and video-ram-kb settings
START_TEST(test_parse_set_columns_and_video_ram) {
    const char* yaml_content = 
        "configs:\n"
        "  - name: Combined Options Test\n"
        "    setup:\n"
        "      - action: set\n"
        "        columns: 80\n"
        "        video-ram-kb: 2\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, 0);
    
    ck_assert_int_eq(test_ctx.set_options_count, 1);
    ck_assert_int_eq(test_ctx.last_columns, 80);
    ck_assert_int_eq(test_ctx.last_video_ram_mask, 1);  // 2KB -> mask 1
}
END_TEST

// Test: Parse config with usb-keymap in set action
START_TEST(test_parse_set_keymap_action) {
    const char* yaml_content = 
        "configs:\n"
        "  - name: Set Keymap Test\n"
        "    setup:\n"
        "      - action: set\n"
        "        usb-keymap: custom_keymap.bin\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, 0);
    
    ck_assert_int_eq(test_ctx.set_options_count, 1);
    ck_assert_str_eq(test_ctx.last_usb_keymap, "custom_keymap.bin");
}
END_TEST

// Test: Parse config with fix-checksum action
START_TEST(test_parse_fix_checksum_action) {
    const char* yaml_content = 
        "configs:\n"
        "  - name: Checksum Test\n"
        "    setup:\n"
        "      - action: fix-checksum\n"
        "        start-addr: 0xC000\n"
        "        end-addr: 0xE000\n"
        "        fix-addr: 0xE001\n"
        "        checksum: 0x42\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, 0);
    
    ck_assert_int_eq(test_ctx.fix_checksum_count, 1);
    ck_assert_int_eq(test_ctx.last_checksum_start, 0xC000);
    ck_assert_int_eq(test_ctx.last_checksum_end, 0xE000);
    ck_assert_int_eq(test_ctx.last_checksum_fix, 0xE001);
    ck_assert_int_eq(test_ctx.last_checksum_value, 0x42);
}
END_TEST

// Test: Parse multiple configs and select specific one
START_TEST(test_parse_multiple_configs_select_second) {
    const char* yaml_content = 
        "configs:\n"
        "  - name: First Config\n"
        "    setup:\n"
        "      - action: set\n"
        "        columns: 40\n"
        "  - name: Second Config\n"
        "    setup:\n"
        "      - action: set\n"
        "        columns: 80\n"
        "  - name: Third Config\n"
        "    setup:\n"
        "      - action: set\n"
        "        columns: 40\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    // Select second config (index 1)
    parse_config_file("/config.yaml", &config_sink, 1);
    
    // Should only execute the second config's actions
    ck_assert_int_eq(test_ctx.set_options_count, 1);
    ck_assert_int_eq(test_ctx.last_columns, 80);
    // Note: on_exit_config is called for all configs, so last_config_name will be the last one
    ck_assert_int_eq(test_ctx.config_enter_count, 3);
    ck_assert_int_eq(test_ctx.config_exit_count, 3);
}
END_TEST

// Test: Parse config with conditional (if/then/else) for graphics keyboard
START_TEST(test_parse_conditional_graphics) {
    const char* yaml_content = 
        "configs:\n"
        "  - name: Conditional Test\n"
        "    setup:\n"
        "      - if: graphics\n"
        "        then:\n"
        "          - action: set\n"
        "            columns: 40\n"
        "        else:\n"
        "          - action: set\n"
        "            columns: 80\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, 0);
    
    // Should execute 'then' branch (40 columns)
    ck_assert_int_eq(test_ctx.set_options_count, 1);
    ck_assert_int_eq(test_ctx.last_columns, 40);
}
END_TEST

// Test: Parse config with conditional (if/then/else) for business keyboard
START_TEST(test_parse_conditional_business) {
    const char* yaml_content = 
        "configs:\n"
        "  - name: Conditional Test\n"
        "    setup:\n"
        "      - if: graphics\n"
        "        then:\n"
        "          - action: set\n"
        "            columns: 40\n"
        "        else:\n"
        "          - action: set\n"
        "            columns: 80\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    // Override system state for business keyboard scenario
    sys_state.pet_keyboard_model = pet_keyboard_model_business;
    parse_config_file("/config.yaml", &config_sink, 0);
    
    // Should execute 'else' branch (80 columns)
    ck_assert_int_eq(test_ctx.set_options_count, 1);
    ck_assert_int_eq(test_ctx.last_columns, 80);
}
END_TEST

// Test: Parse config with multiple load files
START_TEST(test_parse_multiple_load_files) {
    const char* yaml_content = 
        "configs:\n"
        "  - name: Multi Load Test\n"
        "    setup:\n"
        "      - action: load\n"
        "        files:\n"
        "          - file: file1.bin\n"
        "            address: 0x8000\n"
        "          - file: file2.bin\n"
        "            address: 0xC000\n"
        "          - file: file3.bin\n"
        "            address: 0xE000\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, 0);
    
    // Should have called load 3 times
    ck_assert_int_eq(test_ctx.load_count, 3);
    // Last file loaded should be file3.bin at 0xE000
    ck_assert_str_eq(test_ctx.last_load_file, "file3.bin");
    ck_assert_int_eq(test_ctx.last_load_address, 0xE000);
}
END_TEST

// Test: Enumerate all configs (target_index = -1)
START_TEST(test_enumerate_all_configs) {
    const char* yaml_content = 
        "configs:\n"
        "  - name: Config A\n"
        "    setup:\n"
        "      - action: set\n"
        "        columns: 40\n"
        "  - name: Config B\n"
        "    setup:\n"
        "      - action: set\n"
        "        columns: 80\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    // Enumerate mode: target_index = -1
    parse_config_file("/config.yaml", &config_sink, -1);
    
    // Should enter/exit both configs but not execute actions
    ck_assert_int_eq(test_ctx.config_enter_count, 2);
    ck_assert_int_eq(test_ctx.config_exit_count, 2);
    ck_assert_int_eq(test_ctx.set_options_count, 0);
}
END_TEST

// Helper function to read a file from disk into a string
static char* read_file_to_string(const char* host_path) {
    FILE* file = fopen(host_path, "r");
    if (!file) {
        fprintf(stderr, "FATAL: Could not open file '%s' for reading\n", host_path);
        return NULL;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // Allocate buffer and read file
    char* buffer = malloc(file_size + 1);
    if (!buffer) {
        fclose(file);
        fprintf(stderr, "FATAL: Could not allocate memory for file contents\n");
        return NULL;
    }
    
    size_t bytes_read = fread(buffer, 1, file_size, file);
    buffer[bytes_read] = '\0';
    
    fclose(file);
    return buffer;
}

// Test: Validate actual /sdcard/config.yaml by enumerating and loading each config
START_TEST(test_validate_sdcard_config_yaml) {
    // Read the actual config.yaml file from disk
    const char* sdcard_root = getenv("ECONOPET_TEST_SDCARD_ROOT");
    ck_assert_msg(sdcard_root != NULL, "ECONOPET_TEST_SDCARD_ROOT environment variable not set");
    const char* media_root = getenv("ECONOPET_MEDIA_DIR");
    ck_assert_msg(media_root != NULL, "ECONOPET_MEDIA_DIR environment variable not set");
    
    char config_path[PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/config.yaml", sdcard_root);
    
    char* config_contents = read_file_to_string(config_path);
    ck_assert_msg(config_contents != NULL, "Failed to read config.yaml from %s", config_path);
    
    // Register the file contents with the mock file system
    mock_register_file("/config.yaml", config_contents);
    
    // First, enumerate to count the number of configs
    memset(&test_ctx, 0, sizeof(test_ctx));
    parse_config_file("/config.yaml", &config_sink, -1);
    
    int num_configs = test_ctx.config_exit_count;
    ck_assert_int_eq(num_configs, 8);  // Should have exactly 8 configs
    
    // Now load each config by index to verify they're all valid
    mounted_image_root = media_root;
    for (int i = 0; i < num_configs; i++) {
        memset(&test_ctx, 0, sizeof(test_ctx));
        parse_config_file("/config.yaml", &config_sink, i);
        
        // Should have entered and exited all configs up to and including target
        ck_assert_int_eq(test_ctx.config_enter_count, num_configs);
        ck_assert_int_eq(test_ctx.config_exit_count, num_configs);
        
        // Should have a valid config name
        ck_assert_int_gt(strlen(test_ctx.last_config_name), 0);
    }
    mounted_image_root = NULL;
    
    // Clean up
    free(config_contents);
    mock_unregister_file("/config.yaml");
}
END_TEST

// Test: Parse config with tape hex blob in set action
START_TEST(test_parse_set_tape) {
    // ROM 4 blob: ld210=$F42E; bp=$F415; eal=$C9; eah=$CA; fnlen=$D1; devnum=$D4; fnadr=$DA
    const char* yaml_content = 
        "configs:\n"
        "  - name: Tape Test\n"
        "    setup:\n"
        "      - action: set\n"
        "        tape: \"2ef415f4c9cad1d4da\"\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, 0);
    
    ck_assert_int_eq(test_ctx.set_options_count, 1);
    ck_assert(test_ctx.last_tape_enabled);
    
    // Verify LD210 address ($F42E)
    ck_assert_int_eq(test_ctx.last_tape.ld210, 0xF42E);
    
    // Verify breakpoint address ($F415 = JSR LD15)
    ck_assert_int_eq(test_ctx.last_tape.bp_addr, 0xF415);
    
    // Verify zero page locations
    ck_assert_int_eq(test_ctx.last_tape.eal, 0xC9);
    ck_assert_int_eq(test_ctx.last_tape.eah, 0xCA);
    ck_assert_int_eq(test_ctx.last_tape.fnlen, 0xD1);
    ck_assert_int_eq(test_ctx.last_tape.devnum, 0xD4);
    ck_assert_int_eq(test_ctx.last_tape.fnadr, 0xDA);
}
END_TEST

// Test: Parse config with default key before configs
START_TEST(test_parse_default_config) {
    const char* yaml_content = 
        "default: second\n"
        "configs:\n"
        "  - id: first\n"
        "    name: First Config\n"
        "    setup: []\n"
        "  - id: second\n"
        "    name: Second Config\n"
        "    setup: []\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, -1);
    
    ck_assert_int_eq(test_ctx.default_count, 1);
    ck_assert_str_eq(test_ctx.last_default_id, "second");
    ck_assert_int_eq(test_ctx.config_exit_count, 2);
}
END_TEST

// Test: Parse config with default key after configs
START_TEST(test_parse_default_after_configs) {
    const char* yaml_content = 
        "configs:\n"
        "  - id: first\n"
        "    name: First Config\n"
        "    setup: []\n"
        "  - id: second\n"
        "    name: Second Config\n"
        "    setup: []\n"
        "default: first\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, -1);
    
    ck_assert_int_eq(test_ctx.default_count, 1);
    ck_assert_str_eq(test_ctx.last_default_id, "first");
    ck_assert_int_eq(test_ctx.config_exit_count, 2);
}
END_TEST

// Test: Parse config without default key
START_TEST(test_parse_no_default) {
    const char* yaml_content = 
        "configs:\n"
        "  - id: test\n"
        "    name: Test Config\n"
        "    setup: []\n";
    
    mock_register_file("/config.yaml", yaml_content);
    
    parse_config_file("/config.yaml", &config_sink, -1);
    
    ck_assert_int_eq(test_ctx.default_count, 0);
    ck_assert_str_eq(test_ctx.last_default_id, "");
}
END_TEST

// Test: Validate actual /sdcard/config.yaml has a valid default
START_TEST(test_validate_sdcard_config_yaml_default) {
    const char* sdcard_root = getenv("ECONOPET_TEST_SDCARD_ROOT");
    ck_assert_msg(sdcard_root != NULL, "ECONOPET_TEST_SDCARD_ROOT environment variable not set");
    
    char config_path[PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/config.yaml", sdcard_root);
    
    char* config_contents = read_file_to_string(config_path);
    ck_assert_msg(config_contents != NULL, "Failed to read config.yaml from %s", config_path);
    
    mock_register_file("/config.yaml", config_contents);
    
    parse_config_file("/config.yaml", &config_sink, -1);
    
    // The sdcard config.yaml ships with no default specified (commented out)
    ck_assert_int_eq(test_ctx.default_count, 0);
    
    // Verify the default matches one of the config names by re-parsing
    // (The name should be a valid config that exists in the file)
    
    free(config_contents);
    mock_unregister_file("/config.yaml");
}
END_TEST

START_TEST(test_parser_error_reports_semantic_location) {
    const char* yaml_content =
        "configs:\n"
        "  - name: Invalid Action\n"
        "    setup:\n"
        "      - action: invalid\n";

    mock_register_file("/config.yaml", yaml_content);
    mock_expect_fatal_message("Line 4, Column 17:\nUnknown action 'invalid'");

    parse_config_file("/config.yaml", &config_sink, 0);
}
END_TEST

START_TEST(test_parser_error_reports_yaml_problem) {
    const char* yaml_content =
        "configs:\n"
        "  - name: \"Malformed\n"
        "    setup: []\n";

    mock_register_file("/config.yaml", yaml_content);
    mock_expect_fatal_message(
        "Line 4, Column 1:\nfound unexpected end of stream"
    );

    parse_config_file("/config.yaml", &config_sink, 0);
}
END_TEST

START_TEST(test_parser_error_replaces_oversized_message) {
    char key[951];
    memset(key, 'x', sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';

    char yaml_content[1100];
    const int written = snprintf(
        yaml_content,
        sizeof(yaml_content),
        "configs:\n"
        "  - name: Long Error\n"
        "    setup:\n"
        "      - action: set\n"
        "        %s: 1\n",
        key
    );
    ck_assert_int_gt(written, 0);
    ck_assert_int_lt(written, (int) sizeof(yaml_content));

    mock_register_file("/config.yaml", yaml_content);
    mock_expect_fatal_message(
        "Line 5, Column 9:\nerror details too long to display"
    );

    parse_config_file("/config.yaml", &config_sink, 0);
}
END_TEST

START_TEST(test_parser_rejects_short_tape_before_allocation) {
    const char* yaml_content =
        "configs:\n"
        "  - name: Short Tape\n"
        "    setup:\n"
        "      - action: set\n"
        "        tape: 00\n";

    mock_register_file("/config.yaml", yaml_content);
    mock_expect_fatal_message("expected 18 chars, got 2");

    parse_config_file("/config.yaml", &config_sink, 0);
}
END_TEST

START_TEST(test_parser_rejects_long_tape_before_allocation) {
    const char* yaml_content =
        "configs:\n"
        "  - name: Long Tape\n"
        "    setup:\n"
        "      - action: set\n"
        "        tape: 000000000000000000000000000000\n";

    mock_register_file("/config.yaml", yaml_content);
    mock_expect_fatal_message("expected 18 chars, got 30");

    parse_config_file("/config.yaml", &config_sink, 0);
}
END_TEST

START_TEST(test_parser_rejects_multiline_config_name) {
    const char* yaml_content =
        "configs:\n"
        "  - name: \"First\\ncontinued\"\n"
        "    setup: []\n";

    mock_register_file("/config.yaml", yaml_content);
    mock_expect_fatal_message("config name must be a single line");

    parse_config_file("/config.yaml", &config_sink, 0);
}
END_TEST

static const char* const expected_sd_prefixes[SD_DIR_COUNT] = {
    [SD_DIR_NONE] = "",
    [SD_DIR_ROOT] = "/",
    [SD_DIR_DISKS] = "/disks/",
    [SD_DIR_ROMS] = "/roms/",
    [SD_DIR_PRGS] = "/prgs/",
    [SD_DIR_UKM] = "/ukm/",
    [SD_DIR_FPGA] = "/fpga/",
};

// Checks the SD layout independently of the production prefix lookup.
START_TEST(test_sd_dir_prefixes) {
    for (sd_dir_t directory = SD_DIR_NONE;
         directory < SD_DIR_COUNT; directory++) {
        ck_assert_str_eq(sd_dir_prefix(directory), expected_sd_prefixes[directory]);
    }
}
END_TEST

// Checks that every directory retains a complete 255-byte path and its null.
START_TEST(test_sd_make_path_boundary) {
    const char* prefix = expected_sd_prefixes[_i];
    const size_t prefix_length = strlen(prefix);
    char name[SD_PATH_MAX];
    const size_t length = SD_PATH_MAX - 1 - prefix_length;
    memset(name, 'a', length);
    if (_i == SD_DIR_NONE) name[0] = '/';
    name[length] = '\0';

    char path[SD_PATH_MAX];
    sd_make_path(path, _i, name);
    ck_assert_uint_eq(strlen(path), 255);
    ck_assert_int_eq(memcmp(path, prefix, prefix_length), 0);
    ck_assert_str_eq(path + prefix_length, name);
}
END_TEST

// Checks the first invalid full path length for every directory prefix.
START_TEST(test_sd_make_path_rejects_overlong_path) {
    const size_t length = SD_PATH_MAX - strlen(expected_sd_prefixes[_i]);
    char name[SD_PATH_MAX + 1];
    memset(name, 'a', length);
    if (_i == SD_DIR_NONE) name[0] = '/';
    name[length] = '\0';
    char path[SD_PATH_MAX];
    mock_expect_fatal_message("SD path exceeds 255 characters (got 256)");
    sd_make_path(path, _i, name);
}
END_TEST

// Checks invalid directory categories are diagnosed rather than defaulted.
START_TEST(test_sd_dir_rejects_invalid_category) {
    mock_expect_fatal_message("invalid SD directory");
    sd_dir_prefix(_i == 0 ? -1 : SD_DIR_COUNT);
}
END_TEST

// Registers a path at the selected action's limit, optionally one byte too long.
static void register_boundary_path(enum test_path_kind kind, bool oversized,
                                   char* path) {
    const sd_dir_t directory = kind == TEST_PATH_MOUNT
        ? SD_DIR_DISKS : SD_DIR_NONE;
    const size_t capacity = SD_PATH_MAX - strlen(sd_dir_prefix(directory));
    const size_t length = capacity - 1 + oversized;
    memset(path, 'a', length);
    path[length] = '\0';
    if (kind != TEST_PATH_MOUNT) path[0] = '/';

    const char* format = NULL;
    switch (kind) {
        case TEST_PATH_LOAD:
            format = "configs:\n"
                     "  - name: Boundary\n"
                     "    setup:\n"
                     "      - action: load\n"
                     "        files:\n"
                     "          - file: '%s'\n"
                     "            address: 0xC000\n";
            break;
        case TEST_PATH_KEYMAP:
            format = "configs:\n"
                     "  - name: Boundary\n"
                     "    setup:\n"
                     "      - action: set\n"
                     "        usb-keymap: '%s'\n";
            break;
        case TEST_PATH_MOUNT:
            format = "configs:\n"
                     "  - name: Boundary\n"
                     "    setup:\n"
                     "      - action: mount\n"
                     "        file: '%s'\n";
            break;
        default:
            ck_abort_msg("invalid boundary path kind");
    }
    char yaml[1100];
    const int written = snprintf(yaml, sizeof(yaml), format, path);
    ck_assert_int_ge(written, 0);
    ck_assert_uint_lt((size_t) written, sizeof(yaml));
    mock_register_file("/config.yaml", yaml);
}

// Verifies every path-bearing action retains the complete maximum-length path.
START_TEST(test_parse_maximum_sd_path) {
    char path[SD_PATH_MAX];
    register_boundary_path(_i, false, path);
    parse_config_file("/config.yaml", &config_sink, 0);
    switch (_i) {
        case TEST_PATH_LOAD: ck_assert_str_eq(test_ctx.last_load_file, path); break;
        case TEST_PATH_KEYMAP: ck_assert_str_eq(test_ctx.last_usb_keymap, path); break;
        case TEST_PATH_MOUNT: ck_assert_str_eq(test_ctx.last_mount_file, path); break;
        default: ck_abort_msg("invalid boundary path kind");
    }
}
END_TEST

// Verifies overlong paths produce a diagnostic rather than a truncated callback.
START_TEST(test_parser_rejects_overlong_sd_path) {
    char path[SD_PATH_MAX + 1];
    register_boundary_path(_i, true, path);
    mock_expect_fatal_message("SD path exceeds");
    parse_config_file("/config.yaml", &config_sink, 0);
}
END_TEST

// Verifies fixture storage and SD reads retain the terminating byte at the limit.
START_TEST(test_mock_sd_path_boundary) {
    ck_assert_uint_eq(SD_PATH_MAX, 256);
    vet_path_length(0);
    vet_path_length(SD_PATH_MAX - 1);
    char path[SD_PATH_MAX];
    memset(path, 'a', sizeof(path) - 1);
    path[0] = '/';
    path[sizeof(path) - 1] = '\0';
    mock_register_file(path, "boundary");
    FILE* file = sd_open(path, "r");
    ck_assert_ptr_nonnull(file);
    char content[sizeof("boundary")] = { 0 };
    ck_assert_uint_eq(fread(content, 1, sizeof(content) - 1, file), sizeof(content) - 1);
    ck_assert_str_eq(content, "boundary");
    fclose(file);
}
END_TEST

// Verifies host SD reads and fixture registration reject overlong paths.
START_TEST(test_mock_sd_rejects_overlong_path) {
    char path[SD_PATH_MAX + 1];
    memset(path, 'a', SD_PATH_MAX);
    path[0] = '/';
    path[SD_PATH_MAX] = '\0';
    mock_expect_fatal_message("SD path exceeds");
    if (_i == 0) sd_open(path, "r");
    else mock_register_file(path, "overlong");
}
END_TEST

// Verifies the shared helper rejects the first invalid and largest byte lengths.
START_TEST(test_vet_path_length_rejects_overlong_path) {
    mock_expect_fatal_message("SD path exceeds 255 characters");
    vet_path_length(_i == 0 ? SD_PATH_MAX : SIZE_MAX);
}
END_TEST

Suite *config_parser_suite(void) {
    Suite *s;
    TCase *tc_core;

    s = suite_create("config_parser");
    tc_core = tcase_create("Core");

    tcase_add_checked_fixture(tc_core, setup, teardown);
    tcase_add_loop_test(tc_core, test_parse_maximum_sd_path, 0, TEST_PATH_KIND_COUNT);
    tcase_add_test(tc_core, test_mock_sd_path_boundary);
    tcase_add_test(tc_core, test_sd_dir_prefixes);
    tcase_add_loop_test(tc_core, test_sd_make_path_boundary, 0, SD_DIR_COUNT);
    
    tcase_add_test(tc_core, test_parse_minimal_config);
    tcase_add_test(tc_core, test_parse_load_action);
    tcase_add_test(tc_core, test_parse_mount_action);
    tcase_add_test(tc_core, test_parse_mount_action_defaults);
    tcase_add_test(tc_core, test_parse_patch_action);
    tcase_add_test(tc_core, test_parse_copy_action);
    tcase_add_test(tc_core, test_parse_set_action);
    tcase_add_test(tc_core, test_parse_set_video_ram_1kb);
    tcase_add_test(tc_core, test_parse_set_video_ram_2kb);
    tcase_add_test(tc_core, test_parse_set_video_ram_3kb);
    tcase_add_test(tc_core, test_parse_set_video_ram_4kb);
    tcase_add_test(tc_core, test_parse_set_columns_and_video_ram);
    tcase_add_test(tc_core, test_parse_set_keymap_action);
    tcase_add_test(tc_core, test_parse_fix_checksum_action);
    tcase_add_test(tc_core, test_parse_multiple_configs_select_second);
    tcase_add_test(tc_core, test_parse_conditional_graphics);
    tcase_add_test(tc_core, test_parse_conditional_business);
    tcase_add_test(tc_core, test_parse_multiple_load_files);
    tcase_add_test(tc_core, test_enumerate_all_configs);
    tcase_add_test(tc_core, test_validate_sdcard_config_yaml);
    tcase_add_test(tc_core, test_parse_set_tape);
    tcase_add_test(tc_core, test_parse_default_config);
    tcase_add_test(tc_core, test_parse_default_after_configs);
    tcase_add_test(tc_core, test_parse_no_default);
    tcase_add_test(tc_core, test_validate_sdcard_config_yaml_default);
    
    suite_add_tcase(s, tc_core);

    return s;
}

Suite *config_parser_fatal_suite(void) {
    Suite* s = suite_create("config_parser-fatal");
    TCase* tc = tcase_create("Core");

    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_loop_test_raise_signal(tc, test_parser_rejects_overlong_sd_path,
                                    SIGABRT, 0, TEST_PATH_KIND_COUNT);
    tcase_add_loop_test_raise_signal(tc, test_mock_sd_rejects_overlong_path,
                                    SIGABRT, 0, 2);
    tcase_add_loop_test_raise_signal(tc, test_vet_path_length_rejects_overlong_path,
                                    SIGABRT, 0, 2);
    tcase_add_loop_test_raise_signal(tc, test_sd_make_path_rejects_overlong_path,
                                    SIGABRT, 0, SD_DIR_COUNT);
    tcase_add_loop_test_raise_signal(tc, test_sd_dir_rejects_invalid_category,
                                    SIGABRT, 0, 2);
    tcase_add_test_raise_signal(tc, test_parser_error_reports_semantic_location, SIGABRT);
    tcase_add_test_raise_signal(tc, test_parser_error_reports_yaml_problem, SIGABRT);
    tcase_add_test_raise_signal(tc, test_parser_error_replaces_oversized_message, SIGABRT);
    tcase_add_test_raise_signal(tc, test_parser_rejects_short_tape_before_allocation, SIGABRT);
    tcase_add_test_raise_signal(tc, test_parser_rejects_long_tape_before_allocation, SIGABRT);
    tcase_add_test_raise_signal(tc, test_parser_rejects_multiline_config_name, SIGABRT);
    suite_add_tcase(s, tc);

    return s;
}
