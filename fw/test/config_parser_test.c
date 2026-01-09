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

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config/config.h"
#include "mock.h"
#include "sd/sd.h"
#include "system_state.h"
#include "config_parser_test.h"

// Test fixture data structure
typedef struct test_context_s {
    int config_enter_count;
    int config_exit_count;
    int load_count;
    int patch_count;
    int copy_count;
    int set_options_count;
    int fix_checksum_count;
    
    char last_config_name[41];
    char last_load_file[PATH_MAX];
    uint32_t last_load_address;
    uint32_t last_patch_address;
    size_t last_patch_size;
    uint32_t last_copy_source;
    uint32_t last_copy_dest;
    uint32_t last_copy_length;
    uint32_t last_columns;
    uint32_t last_video_ram_mask;
    char last_usb_keymap[261];
    uint32_t last_checksum_start;
    uint32_t last_checksum_end;
    uint32_t last_checksum_fix;
    uint32_t last_checksum_value;
} test_context_t;

static test_context_t test_ctx;

// Global shared test state (sink structs with const members must be initialized statically)
static system_state_t sys_state; // defaults applied in setup()

// Callback implementations for testing
static void test_on_enter_config(void* context) {
    test_context_t* ctx = (test_context_t*)context;
    ctx->config_enter_count++;
}

static void test_on_exit_config(void* context, const char* name) {
    test_context_t* ctx = (test_context_t*)context;
    ctx->config_exit_count++;
    strncpy(ctx->last_config_name, name, sizeof(ctx->last_config_name) - 1);
    ctx->last_config_name[sizeof(ctx->last_config_name) - 1] = '\0';
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

static void test_on_set_options(void* context, options_t* options) {
    test_context_t* ctx = (test_context_t*)context;
    ctx->set_options_count++;
    ctx->last_columns = options->columns;
    ctx->last_video_ram_mask = options->video_ram_mask;
    strncpy(ctx->last_usb_keymap, options->usb_keymap, sizeof(ctx->last_usb_keymap) - 1);
    ctx->last_usb_keymap[sizeof(ctx->last_usb_keymap) - 1] = '\0';
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
    .on_set_options = test_on_set_options,
    .on_fix_checksum = test_on_fix_checksum,
    .system_state = &sys_state,
};

static const config_sink_t config_sink = {
    .context = &test_ctx,
    .on_enter_config = test_on_enter_config,
    .on_exit_config = test_on_exit_config,
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
    ck_assert_int_eq(num_configs, 7);  // Should have exactly 7 configs
    
    // Now load each config by index to verify they're all valid
    for (int i = 0; i < num_configs; i++) {
        memset(&test_ctx, 0, sizeof(test_ctx));
        parse_config_file("/config.yaml", &config_sink, i);
        
        // Should have entered and exited all configs up to and including target
        ck_assert_int_eq(test_ctx.config_enter_count, num_configs);
        ck_assert_int_eq(test_ctx.config_exit_count, num_configs);
        
        // Should have a valid config name
        ck_assert_int_gt(strlen(test_ctx.last_config_name), 0);
    }
    
    // Clean up
    free(config_contents);
    mock_unregister_file("/config.yaml");
}
END_TEST

Suite *config_parser_suite(void) {
    Suite *s;
    TCase *tc_core;

    s = suite_create("config_parser");
    tc_core = tcase_create("Core");

    tcase_add_checked_fixture(tc_core, setup, teardown);
    
    tcase_add_test(tc_core, test_parse_minimal_config);
    tcase_add_test(tc_core, test_parse_load_action);
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
    
    suite_add_tcase(s, tc_core);

    return s;
}
