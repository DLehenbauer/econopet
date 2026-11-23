// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"

#include <check.h>

#include "blockdevice/heap.h"
#include "blockdevice/sd.h"
#undef PATH_MAX
#include "ff.h"
#include "filesystem/fat.h"
#include "filesystem/vfs.h"

#include "fatal.h"
#include "hw.h"
#include "sd/sd.h"

#define TEST_CARD_BYTES (16u * 1024u * 1024u)

typedef enum {
    CS_UNCONFIGURED,
    CS_INITIALIZED,
    CS_OUTPUT,
    CS_DEASSERTED,
} cs_state_t;

static blockdevice_t* card;
static cs_state_t cs_state;
static unsigned int device_creations;
static unsigned int filesystem_creations;
static unsigned int mount_calls;
static unsigned int unmount_calls;
static unsigned int warnings;
static bool fail_device_creation;
static bool fail_filesystem_creation;
static bool led_on;
static unsigned int activity_completions;
static unsigned int initialization_calls;
static unsigned int fail_initialization_call;
static int (*card_init)(blockdevice_t*);
static int (*fat_unmount)(filesystem_t*);

/** Observe CS initialization before the output direction and level are set. */
void gpio_init(unsigned int gpio) {
    ck_assert_uint_eq(gpio, SD_CSN_GP);
    cs_state = CS_INITIALIZED;
}

/** Require CS to be configured as an output before deassertion. */
void gpio_set_dir(unsigned int gpio, bool output) {
    ck_assert_uint_eq(gpio, SD_CSN_GP);
    ck_assert_int_eq(cs_state, CS_INITIALIZED);
    ck_assert(output);
    cs_state = CS_OUTPUT;
}

/** Observe CS setup and permit the real activity wrapper's LED writes. */
void gpio_put(unsigned int gpio, bool value) {
    if (gpio == PICO_DEFAULT_LED_PIN) {
        ck_assert(value != led_on);
        led_on = value;
        if (!value) {
            ++activity_completions;
        }
        return;
    }
    ck_assert_uint_eq(gpio, SD_CSN_GP);
    ck_assert_int_eq(cs_state, CS_OUTPUT);
    ck_assert(value);
    cs_state = CS_DEASSERTED;
}

/** Accept expected device initialization and FAT validation warnings. */
void log_warn(const char* format, ...) {
    ck_assert(strcmp(format, "fs_mount error: %s") == 0 || strcmp(format, "sd init error: %d") == 0);
    ++warnings;
}

/** Model SD reinitialization failures without erasing the heap card's persistent bytes. */
static int observe_init(blockdevice_t* device) {
    ++initialization_calls;
    if (initialization_calls == fail_initialization_call) {
        device->is_initialized = false;
        return -EIO;
    }

    // The heap was allocated at construction, and card contents survive failed initialization.
    device->is_initialized = true;
    return card_init(device);
}

/** Supply the heap card after checking the actual board setup and SPI settings. */
blockdevice_t* blockdevice_sd_create(spi_inst_t* spi, uint8_t mosi, uint8_t miso, uint8_t clock, uint8_t cs,
                                     uint32_t hz, bool crc) {
    ck_assert_int_eq(cs_state, CS_DEASSERTED);
    ck_assert_ptr_eq(spi, SD_SPI_INSTANCE);
    ck_assert_uint_eq(mosi, SD_CMD_GP);
    ck_assert_uint_eq(miso, SD_DAT_GP);
    ck_assert_uint_eq(clock, SD_CLK_GP);
    ck_assert_uint_eq(cs, SD_CSN_GP);
    ck_assert_uint_eq(hz, SD_SPI_MHZ * UINT32_C(1000000));
    ck_assert_int_eq(crc, SD_SPI_ENABLE_CRC);
    ++device_creations;
    return fail_device_creation ? NULL : card;
}

/** Observe real FAT registration cleanup on rollback and normal unmount. */
static int observe_unmount(filesystem_t* filesystem) {
    ++unmount_calls;
    return fat_unmount(filesystem);
}

filesystem_t* __real_filesystem_fat_create(void);

/** Count root allocations while keeping the actual FAT implementation. */
filesystem_t* __wrap_filesystem_fat_create(void) {
    ++filesystem_creations;
    if (fail_filesystem_creation) {
        return NULL;
    }
    filesystem_t* const filesystem = __real_filesystem_fat_create();
    ck_assert_ptr_nonnull(filesystem);
    fat_unmount = filesystem->unmount;
    filesystem->unmount = observe_unmount;
    return filesystem;
}

/** Adapt the VFS boundary to the real FAT mount without replacing SD state. */
int fs_mount(const char* path, filesystem_t* filesystem, blockdevice_t* device) {
    ck_assert_str_eq(path, "/");
    ck_assert_ptr_eq(filesystem, sd_fat_filesystem());
    ck_assert_ptr_eq(device, card);

    // Match the VFS preflight before allowing FAT to reserve a logical drive.
    if (!device->is_initialized) {
        const int result = device->init(device);
        if (result != BD_ERROR_OK) {
            errno = -result;
            return -1;
        }
    }
    ++mount_calls;
    const int result = filesystem->mount(filesystem, device, false);
    errno = result < 0 ? -result : 0;
    return result < 0 ? -1 : result;
}

/** Unmount the production root's actual FAT registration. */
int fs_unmount(const char* path) {
    ck_assert_str_eq(path, "/");
    filesystem_t* const filesystem = sd_fat_filesystem();
    const int result = filesystem->unmount(filesystem);
    errno = result < 0 ? -result : 0;
    return result < 0 ? -1 : result;
}

/** Create one blank card for a fresh, fork-isolated firmware boot. */
static void setup(void) {
    card = blockdevice_heap_create(TEST_CARD_BYTES);
    ck_assert_ptr_nonnull(card);
    card_init = card->init;
    card->init = observe_init;
    ck_assert_ptr_null(sd_blockdevice());
    ck_assert_ptr_null(sd_fat_filesystem());
    ck_assert(!sd_is_mounted());
}

/** Format the card independently of the production root allocation. */
static void format_card(void) {
    filesystem_t* const filesystem = __real_filesystem_fat_create();
    ck_assert_ptr_nonnull(filesystem);
    ck_assert_int_eq(filesystem->format(filesystem, card), 0);
    filesystem_fat_free(filesystem);
}

/** Retire the real root registration before freeing fixture-owned storage. */
static void teardown(void) {
    ck_assert(sd_unmount_root());
    filesystem_t* const filesystem = sd_fat_filesystem();
    if (filesystem != NULL) {
        filesystem_fat_free(filesystem);
    }
    blockdevice_heap_free(card);
}

/** Repeated initialization and remount retain one device, filesystem, and activity hook. */
START_TEST(test_init_reuses_root) {
    format_card();
    ck_assert(sd_init());
    ck_assert(sd_is_mounted());
    ck_assert_ptr_eq(sd_blockdevice(), card);
    ck_assert_uint_gt(activity_completions, 0);
    ck_assert(!led_on);
    filesystem_t* const filesystem = sd_fat_filesystem();
    ck_assert(sd_init());
    ck_assert_uint_eq(mount_calls, 1);
    ck_assert(sd_unmount_root());
    ck_assert(!sd_is_mounted());
    ck_assert(sd_init());
    ck_assert_ptr_eq(sd_fat_filesystem(), filesystem);
    ck_assert_uint_eq(device_creations, 1);
    ck_assert_uint_eq(filesystem_creations, 1);
    ck_assert_uint_eq(mount_calls, 2);
    ck_assert_uint_eq(warnings, 0);
}
END_TEST

/** Failed reinitialization during FAT mounting must release every reserved drive slot. */
START_TEST(test_init_recovers_mount_initialization_failure) {
    format_card();
    for (unsigned int attempt = 0; attempt <= FF_VOLUMES; ++attempt) {
        // Fail FAT's initialization, not the preceding VFS preflight on retry.
        fail_initialization_call = initialization_calls + (card->is_initialized ? 1u : 2u);
        ck_assert(!sd_init());
        ck_assert(!sd_is_mounted());
        ck_assert(!card->is_initialized);
        ck_assert_uint_eq(unmount_calls, attempt + 1);
    }

    fail_initialization_call = 0;
    ck_assert(sd_init());
    ck_assert(sd_is_mounted());
    ck_assert_uint_eq(device_creations, 1);
    ck_assert_uint_eq(filesystem_creations, 1);
    FATFS* filesystem = NULL;
    DWORD free_clusters;
    ck_assert_int_eq(f_getfree("0:", &free_clusters, &filesystem), FR_OK);
    ck_assert_ptr_nonnull(filesystem);
    ck_assert_uint_gt(free_clusters, 0);
}
END_TEST

/** Initialization failure before FAT registration needs no rollback and remains retryable. */
START_TEST(test_init_retries_before_registration) {
    format_card();
    card->is_initialized = false;
    fail_initialization_call = initialization_calls + 1;
    ck_assert(!sd_init());
    ck_assert(!sd_is_mounted());
    ck_assert_uint_eq(mount_calls, 0);
    ck_assert_uint_eq(unmount_calls, 0);

    fail_initialization_call = 0;
    ck_assert(sd_init());
    ck_assert(sd_is_mounted());
    ck_assert_uint_eq(mount_calls, 1);
    ck_assert_uint_eq(unmount_calls, 0);
}
END_TEST

/** Failed FAT validation releases its drive slot so the same root can recover. */
START_TEST(test_init_recovers_unformatted_card) {
    ck_assert(!sd_init());
    ck_assert(!sd_is_mounted());
    ck_assert_uint_eq(unmount_calls, 1);
    ck_assert(!sd_init());
    ck_assert_uint_eq(unmount_calls, 2);
    ck_assert_uint_eq(warnings, 2);
    format_card();
    ck_assert(sd_init());
    ck_assert(sd_is_mounted());
    ck_assert_uint_eq(device_creations, 1);
    ck_assert_uint_eq(filesystem_creations, 1);
    FATFS* filesystem = NULL;
    DWORD free_clusters;
    ck_assert_int_eq(f_getfree("0:", &free_clusters, &filesystem), FR_OK);
    ck_assert_ptr_nonnull(filesystem);
    ck_assert_uint_gt(free_clusters, 0);
}
END_TEST

/** Device allocation failure remains retryable without recreating the filesystem. */
START_TEST(test_init_retries_device_creation) {
    format_card();
    fail_device_creation = true;
    ck_assert(!sd_init());
    ck_assert_ptr_null(sd_blockdevice());
    ck_assert_uint_eq(mount_calls, 0);
    fail_device_creation = false;
    ck_assert(sd_init());
    ck_assert_uint_eq(device_creations, 2);
    ck_assert_uint_eq(filesystem_creations, 1);
}
END_TEST

/** FAT allocation failure retains the already wrapped device for a later retry. */
START_TEST(test_init_retries_filesystem_creation) {
    format_card();
    fail_filesystem_creation = true;
    ck_assert(!sd_init());
    ck_assert_ptr_eq(sd_blockdevice(), card);
    ck_assert_ptr_null(sd_fat_filesystem());
    ck_assert_uint_eq(mount_calls, 0);
    fail_filesystem_creation = false;
    ck_assert(sd_init());
    ck_assert_uint_eq(device_creations, 1);
    ck_assert_uint_eq(filesystem_creations, 2);
}
END_TEST

/** Run root lifecycle checks in independent firmware-lifetime processes. */
int main(void) {
    Suite* const suite = suite_create("sd-init");
    TCase* const root = tcase_create("root");
    tcase_add_checked_fixture(root, setup, teardown);
    tcase_add_test(root, test_init_reuses_root);
    tcase_add_test(root, test_init_recovers_unformatted_card);
    tcase_add_test(root, test_init_recovers_mount_initialization_failure);
    tcase_add_test(root, test_init_retries_before_registration);
    tcase_add_test(root, test_init_retries_device_creation);
    tcase_add_test(root, test_init_retries_filesystem_creation);
    suite_add_tcase(suite, root);
    SRunner* const runner = srunner_create(suite);
    srunner_set_fork_status(runner, CK_FORK);
    srunner_run_all(runner, CK_NORMAL);
    const int failures = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failures == 0 ? 0 : 1;
}
