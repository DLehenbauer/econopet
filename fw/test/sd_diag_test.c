// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#define _XOPEN_SOURCE 700

#include "pch.h"
#include "sd_diag_mock.h"

#include <check.h>

#include "blockdevice/heap.h"
#include "blockdevice/sd.h"
#include "diag/diag.h"
#include "diag/sd.h"
#include "display/char_encoding.h"
#include "driver.h"
#undef PATH_MAX
#include "filesystem/fat.h"
#include "filesystem/vfs.h"
#include "fatal.h"
#include "hw.h"
#include "ieee/dos_media.h"
#include "sd/sd.h"
#include "system/state.h"

#define SD_DIAG_TEST_CARD_BYTES (16u * 1024u * 1024u)
#define SD_DIAG_TEST_SECTOR_SIZE 512u
#define SD_DIAG_TEST_ROM_BYTES (64u * 1024u)

typedef enum {
    SD_FAULT_NONE,
    SD_FAULT_READ_ERROR,
    SD_FAULT_CORRUPT_READ,
    SD_FAULT_WRITE_BLEED,
    SD_FAULT_SHORT_MULTIBLOCK_READ,
} sd_fault_t;

typedef struct {
    blockdevice_t* backend;
    sd_fault_t fault;
    bool mode_init_failure;
} sd_fixture_t;

static blockdevice_t device;
static sd_fixture_t fixture;
static blockdevice_t* backend;
static filesystem_t* fat;
static blockdevice_t* mode_device;
static diag_session_t session;
static size_t mode_calls;
static bool storage_idle;
static bool cpu_halted;
static bool images_unmounted;
static bool fail_images_unmount;
static bool fail_device_deinit;
static bool builtin_font;
const uint8_t rom_chars_e800[0x800] = {0};

/** The standalone storage fixture never enters a menu scan. */
bool menu_browser_abandon_scan(void) {
    ck_assert(cpu_halted && storage_idle);
    return true;
}

/** Reject hardware display paths in this local-renderer fixture. */
void video_init(void) { ck_abort_msg("unexpected video_init"); }
void spi_read(uint32_t addr, size_t length, uint8_t* dest) {
    (void)addr; (void)length; (void)dest;
    ck_abort_msg("unexpected spi_read");
}
void spi_write(uint32_t addr, const uint8_t* const src, size_t length) {
    (void)addr; (void)src; (void)length;
    ck_abort_msg("unexpected spi_write");
}
uint8_t spi_write_at(uint32_t addr, uint8_t value) {
    (void)addr; (void)value;
    ck_abort_msg("unexpected spi_write_at");
    __builtin_unreachable();
}
void write_pet_model(const system_state_t* const state) {
    (void)state;
    ck_abort_msg("unexpected write_pet_model");
}

/** Satisfy renderer font selection without FPGA SPI in the host harness. */
void roms_use_builtin_char_rom(void) {
    builtin_font = true;
}

/** Observe guest storage quiescence as immediately complete in the host fixture. */
void pet_wait_for_storage_idle(void) {
    ck_assert(cpu_halted);
    storage_idle = true;
}

/** Observe CPU halt requests without modeled PET registers. */
void set_cpu(cpu_state_t state) {
    ck_assert_int_eq(state, CPU_HALT);
    cpu_halted = true;
}

/** Report all IEEE-488 media as already unmounted in the host fixture. */
bool ieee_drive_unmount_all(void) {
    ck_assert(cpu_halted && storage_idle);
    if (fail_images_unmount) return false;
    images_unmounted = true;
    return true;
}

/** Reject the fallback reset when the modeled image unmount succeeded. */
void ieee_drive_init(void) {
    ck_abort_msg("unexpected IEEE reset after successful fake image unmount");
}

/** Reject warnings from paths not modeled by this focused fixture. */
void log_warn(const char* format, ...) {
    ck_abort_msg("unexpected storage warning: %s", format);
}

/** Initialize the shared heap once so mode deinitialization retains card data. */
static int fixture_init(blockdevice_t* wrapper) {
    sd_fixture_t* const state = wrapper->config;
    if (state->backend->is_initialized) {
        wrapper->is_initialized = true;
        return BD_ERROR_OK;
    }
    const int error = state->backend->init(state->backend);
    wrapper->is_initialized = error == BD_ERROR_OK;
    return error;
}

/** Keep the shared host card initialized across simulated SPI mode boundaries. */
static int fixture_deinit(blockdevice_t* wrapper) {
    if (fail_device_deinit) return BD_ERROR_DEVICE_ERROR;
    wrapper->is_initialized = false;
    return BD_ERROR_OK;
}

/** Inject read errors or one byte of corruption before returning the heap read. */
static int fixture_read(blockdevice_t* wrapper, const void* buffer,
                        bd_size_t addr, bd_size_t size) {
    sd_fixture_t* const state = wrapper->config;
    if (state->fault == SD_FAULT_READ_ERROR) return BD_ERROR_DEVICE_ERROR;
    if (state->fault == SD_FAULT_SHORT_MULTIBLOCK_READ &&
        size == 64u * SD_DIAG_TEST_SECTOR_SIZE)
        return state->backend->read(state->backend, buffer, addr, SD_DIAG_TEST_SECTOR_SIZE);
    const int error = state->backend->read(state->backend, buffer, addr, size);
    if (error == BD_ERROR_OK && state->fault == SD_FAULT_CORRUPT_READ && size != 0) {
        uint8_t* const bytes = (uint8_t*)buffer;
        bytes[size - 1] ^= 1u;
    }
    return error;
}

/** Optionally model a one-sector write leaking into the following sector. */
static int fixture_program(blockdevice_t* wrapper, const void* buffer,
                           bd_size_t addr, bd_size_t size) {
    sd_fixture_t* const state = wrapper->config;
    const int error = state->backend->program(state->backend, buffer, addr, size);
    if (error == BD_ERROR_OK && state->fault == SD_FAULT_WRITE_BLEED &&
        size == SD_DIAG_TEST_SECTOR_SIZE) {
        const uint8_t byte = 0x5a;
        const bd_size_t neighbor = addr + size;
        (void)state->backend->program(state->backend, &byte, neighbor, 1);
    }
    return error;
}

/** Delegate erase while preserving the host card's initialized transport. */
static int fixture_erase(blockdevice_t* wrapper, bd_size_t addr, bd_size_t size) {
    return ((sd_fixture_t*)wrapper->config)->backend->erase(
        ((sd_fixture_t*)wrapper->config)->backend, addr, size);
}

/** Delegate trim to the heap card. */
static int fixture_trim(blockdevice_t* wrapper, bd_size_t addr, bd_size_t size) {
    return ((sd_fixture_t*)wrapper->config)->backend->trim(
        ((sd_fixture_t*)wrapper->config)->backend, addr, size);
}

/** Delegate synchronization to the backing card. */
static int fixture_sync(blockdevice_t* wrapper) {
    sd_fixture_t* const state = wrapper->config;
    return state->backend->sync(state->backend);
}

/** Report the shared in-memory card's byte capacity. */
static bd_size_t fixture_size(blockdevice_t* wrapper) {
    sd_fixture_t* const state = wrapper->config;
    return state->backend->size(state->backend);
}

/** Fake board-level SD construction while requiring the production configuration. */
blockdevice_t* blockdevice_sd_create(spi_inst_t* spi, uint8_t mosi, uint8_t miso,
                                     uint8_t clock, uint8_t cs, uint32_t hz, bool crc) {
    ck_assert_ptr_eq(spi, SD_SPI_INSTANCE);
    ck_assert_uint_eq(mosi, SD_CMD_GP);
    ck_assert_uint_eq(miso, SD_DAT_GP);
    ck_assert_uint_eq(clock, SD_CLK_GP);
    ck_assert_uint_eq(cs, SD_CSN_GP);
    ++mode_calls;
    ck_assert_uint_eq(hz, SD_SPI_MHZ * UINT32_C(1000000));
    ck_assert_int_eq(crc, SD_SPI_ENABLE_CRC);
    ck_assert_ptr_null(mode_device);
    ck_assert(!device.is_initialized);
    mode_device = calloc(1, sizeof(*mode_device));
    ck_assert_ptr_nonnull(mode_device);
    *mode_device = device;
    mode_device->is_initialized = !fixture.mode_init_failure;
    return mode_device;
}

/** Release only the temporary mode device, never the retained primary card. */
void blockdevice_sd_free(blockdevice_t* mode) {
    ck_assert_ptr_eq(mode, mode_device);
    ck_assert_ptr_ne(mode, &device);
    free(mode);
    mode_device = NULL;
}

/** Prepare the unmodified production plan using a fake SD card and VFS. */
static void setup(void) {
    mode_calls = 0;
    storage_idle = false;
    cpu_halted = false;
    images_unmounted = false;
    fail_images_unmount = false;
    fail_device_deinit = false;
    builtin_font = false;
    backend = blockdevice_heap_create(SD_DIAG_TEST_CARD_BYTES);
    ck_assert_ptr_nonnull(backend);
    fixture = (sd_fixture_t){ .backend = backend, .fault = SD_FAULT_NONE };
    device = (blockdevice_t){
        .init = fixture_init,
        .deinit = fixture_deinit,
        .sync = fixture_sync,
        .read = fixture_read,
        .program = fixture_program,
        .erase = fixture_erase,
        .trim = fixture_trim,
        .size = fixture_size,
        .read_size = SD_DIAG_TEST_SECTOR_SIZE,
        .erase_size = SD_DIAG_TEST_SECTOR_SIZE,
        .program_size = SD_DIAG_TEST_SECTOR_SIZE,
        .name = "faulting heap",
        .config = &fixture,
        .is_initialized = true,
    };
    fat = filesystem_fat_create();
    ck_assert_ptr_nonnull(fat);
    sd_diag_mock_bind(&device, fat);
    ck_assert(diag_sd_prepare_swap());
    ck_assert(storage_idle && cpu_halted && images_unmounted);
    ck_assert(!device.is_initialized);
    sd_diag_mock_capture_workspace();
    diag_plan_sd.prepare();
    diag_session_init(&session, &diag_plan_sd);
    ck_assert(builtin_font);
}

/** Failed media flush never invalidates the transport needed by retained owners. */
START_TEST(test_swap_rejects_unmount_failure) {
    device.is_initialized = true;
    images_unmounted = false;
    fail_images_unmount = true;
    ck_assert(!diag_sd_prepare_swap());
    ck_assert(!images_unmounted);
    ck_assert(device.is_initialized);
}
END_TEST

/** A mounted original filesystem is detached before retiring its transport. */
START_TEST(test_swap_retires_mounted_root) {
    ck_assert_int_eq(fat->format(fat, &device), 0);
    ck_assert(sd_mount_root());
    ck_assert(diag_sd_prepare_swap());
    ck_assert(!device.is_initialized);
    ck_assert(sd_mount_root());
    ck_assert(device.is_initialized);
    ck_assert(sd_unmount_root());
}
END_TEST

/** A transport retirement failure prevents a swap prompt. */
START_TEST(test_swap_rejects_deinit_failure) {
    device.is_initialized = true;
    fail_device_deinit = true;
    ck_assert(!diag_sd_prepare_swap());
    ck_assert(device.is_initialized);
}
END_TEST

/** Retire resources at the end of one simulated firmware process. */
static void teardown(void) {
    if (mode_device != NULL) blockdevice_sd_free(mode_device);
    sd_diag_mock_cleanup();
    ck_assert_uint_eq(sd_diag_mock_open_handles(), 0);
    if (fat != NULL) filesystem_fat_free(fat);
    if (backend != NULL) blockdevice_heap_free(backend);
    fat = NULL;
    backend = NULL;
}

/** Run the full production workload twice through fake SD and VFS boundaries. */
START_TEST(test_two_complete_cycles) {
    ck_assert(diag_run_iteration(&session));
    ck_assert(diag_run_iteration(&session));
    ck_assert_int_eq(session.active_status, DIAG_STATUS_PASS);
    ck_assert_uint_eq(session.iteration, 2);
    ck_assert_uint_eq(mode_calls, 2);
    ck_assert_uint_eq(sd_diag_mock_reformats(), 2);
    ck_assert_uint_eq(sd_diag_mock_mounts(), 8);
    ck_assert_uint_eq(sd_diag_mock_peak_handles(),
                      DOS_MEDIA_UNIT_COUNT * DOS_MEDIA_SLOTS_PER_UNIT + 1u);
    ck_assert_uint_eq(sd_diag_mock_open_handles(), 0);
    char banner[DIAG_COLS + 1] = { 0 };
    for (size_t column = 0; column < DIAG_COLS; ++column)
        banner[column] = (char)system_state.video_char_buffer[column];
    const uint8_t completed_two = ascii_to_vrom((uint8_t)'2') | 0x80;
    ck_assert_ptr_nonnull(memchr(banner, completed_two, sizeof(banner)));

    // Golden cycle-two samples catch misplaced sectors and reads past the TX buffer.
    static const uint8_t expected[][8] = {
        { 0x3a, 0x01, 0x62, 0xb2, 0xc3, 0x3d, 0x09, 0xea },
        { 0x77, 0x10, 0xd9, 0x6b, 0x4f, 0x56, 0xfc, 0xe8 },
    };
    ck_assert_int_eq(fat->mount(fat, &device, false), 0);
    fs_file_t file = { 0 };
    ck_assert_int_eq(fat->file_open(fat, &file, "/_diag/rom64k.bin", O_RDONLY), 0);
    struct stat info;
    ck_assert_int_eq(fat->stat(fat, "/_diag/rom64k.bin", &info), 0);
    ck_assert_int_eq(info.st_size, SD_DIAG_TEST_ROM_BYTES);
    for (size_t index = 0; index < count_of(expected); ++index) {
        const off_t offset = index == 0 ? 0 :
            SD_DIAG_TEST_ROM_BYTES - SD_DIAG_TEST_SECTOR_SIZE;
        uint8_t actual[sizeof(expected[0])];
        ck_assert_int_eq(fat->file_seek(fat, &file, offset, SEEK_SET), offset);
        ck_assert_int_eq(fat->file_read(fat, &file, actual, sizeof(actual)), sizeof(actual));
        ck_assert_int_eq(memcmp(actual, expected[index], sizeof(actual)), 0);
    }
    ck_assert_int_eq(fat->file_close(fat, &file), 0);
    ck_assert_int_eq(fat->unmount(fat), 0);
}
END_TEST

/** Read errors escape the case with an explicit LBA-oriented diagnostic. */
START_TEST(test_read_fault) {
    fixture.fault = SD_FAULT_READ_ERROR;
    ck_assert(!diag_run_iteration(&session));
    ck_assert_ptr_nonnull(strstr(session.failure_detail, "read LBA"));
    ck_assert_uint_eq(sd_diag_mock_open_handles(), 0);
}
END_TEST

/** Reusing the large write buffer must not mask a partially filled successful read. */
START_TEST(test_short_multiblock_read) {
    fixture.fault = SD_FAULT_SHORT_MULTIBLOCK_READ;
    ck_assert(!diag_run_iteration(&session));
    ck_assert_ptr_nonnull(strstr(session.failure_summary, "Multi-Block 8 & 64 Sectors"));
    ck_assert_ptr_nonnull(strstr(session.failure_detail, "expected $"));
}
END_TEST

/** A single-bit readback corruption is reported at the exact sector boundary. */
START_TEST(test_corrupt_read) {
    fixture.fault = SD_FAULT_CORRUPT_READ;
    ck_assert(!diag_run_iteration(&session));
    ck_assert_ptr_nonnull(strstr(session.failure_detail, "expected $"));
    ck_assert_ptr_nonnull(strstr(session.failure_detail, "got $"));
    ck_assert_uint_eq(sd_diag_mock_open_handles(), 0);
}
END_TEST

/** A neighboring-sector write leak is caught by the isolation case. */
START_TEST(test_neighbor_bleed) {
    fixture.fault = SD_FAULT_WRITE_BLEED;
    ck_assert(!diag_run_iteration(&session));
    ck_assert_ptr_nonnull(strstr(session.failure_summary, "Neighbor Isolation"));
    ck_assert_uint_eq(sd_diag_mock_open_handles(), 0);
}
END_TEST

/** Failed production-configured SD construction remains a recoverable diag failure. */
START_TEST(test_mode_init_failure) {
    fixture.mode_init_failure = true;
    ck_assert(!diag_run_iteration(&session));
    ck_assert_ptr_nonnull(strstr(session.failure_summary, "Production SPI Configuration"));
    ck_assert_uint_eq(sd_diag_mock_open_handles(), 0);
}
END_TEST

/** Detect corrupted previous image bytes before overwriting the sector. */
START_TEST(test_image_previous_read_corruption) {
    sd_diag_mock_corrupt_image_read(false);
    ck_assert(!diag_run_iteration(&session));
    ck_assert_ptr_nonnull(strstr(session.failure_summary, "Disk Image Stream I/O"));
    ck_assert_ptr_nonnull(strstr(session.failure_detail, "previous bytes mismatch"));
}
END_TEST

/** Require complete image readback after a clean remount, never a second format. */
START_TEST(test_image_remount_corruption) {
    sd_diag_mock_corrupt_image_read(true);
    ck_assert(!diag_run_iteration(&session));
    ck_assert_ptr_nonnull(strstr(session.failure_summary, "Disk Image Stream I/O"));
    ck_assert_ptr_nonnull(strstr(session.failure_detail, "remount bytes"));
    ck_assert_uint_eq(sd_diag_mock_reformats(), 1);
    ck_assert_uint_eq(sd_diag_mock_mounts(), 4);
}
END_TEST

/** Catch a wrong descriptor size at the mocked VFS boundary. */
START_TEST(test_stat_size_failure) {
    sd_diag_mock_break_stat();
    ck_assert(!diag_run_iteration(&session));
    ck_assert_ptr_nonnull(strstr(session.failure_detail, "POSIX fstat"));
    ck_assert_ptr_nonnull(strstr(session.failure_detail, "size 4100"));
}
END_TEST

/** Build a bounded Check suite for complete cycles and injected failures. */
Suite* sd_diag_suite(void) {
    Suite* const suite = suite_create("sd-diag");
    TCase* const cycles = tcase_create("cycles");
    tcase_add_checked_fixture(cycles, setup, teardown);
    tcase_add_test(cycles, test_swap_rejects_unmount_failure);
    tcase_add_test(cycles, test_swap_rejects_deinit_failure);
    tcase_add_test(cycles, test_swap_retires_mounted_root);
    tcase_add_test(cycles, test_two_complete_cycles);
    tcase_add_test(cycles, test_read_fault);
    tcase_add_test(cycles, test_short_multiblock_read);
    tcase_add_test(cycles, test_corrupt_read);
    tcase_add_test(cycles, test_neighbor_bleed);
    tcase_add_test(cycles, test_mode_init_failure);
    tcase_add_test(cycles, test_image_previous_read_corruption);
    tcase_add_test(cycles, test_image_remount_corruption);
    tcase_add_test(cycles, test_stat_size_failure);
    suite_add_tcase(suite, cycles);
    return suite;
}

/** Run the standalone storage diagnostic suite. */
int main(void) {
    Suite* const suite = sd_diag_suite();
    SRunner* const runner = srunner_create(suite);
    // Each test models a firmware boot, including its process-lifetime workspace.
    srunner_set_fork_status(runner, CK_FORK);
    srunner_run_all(runner, CK_NORMAL);
    const int failures = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failures == 0 ? 0 : 1;
}

/** Reject accidentally executing the unrelated memory-plan placeholder. */
static void memory_case(const void* context) {
    (void)context;
    ck_abort_msg("unexpected memory diagnostic in SD fixture");
}

static const diag_case_t memory_cases[] = {
    { "Host fixture", memory_case },
};
static const diag_suite_t memory_suites[] = {
    DIAG_SUITE("Memory host fixture", memory_cases, NULL),
};
const diag_plan_t diag_plan_bup = {
    .name = "bup",
    .description = "Host fixture",
    .confirm = NULL,
    .prepare = NULL,
    .suites = memory_suites,
    .suite_count = count_of(memory_suites),
};

const diag_plan_t diag_plan_memory = {
    .name = "memory",
    .description = "Host fixture",
    .confirm = NULL,
    .prepare = NULL,
    .suites = memory_suites,
    .suite_count = count_of(memory_suites),
};

/** Confirmation is exercised by the CLI fixture, not by storage-only case execution. */
void diag_sd_confirm(uint8_t max_cycles) {
    (void)max_cycles;
    ck_abort_msg("unexpected SD confirmation in storage fixture");
    abort();
}

/** Finite completion halt is exercised by the diagnostic framework fixture. */
void diag_complete_halt(void) {
    ck_abort_msg("unexpected diag_complete_halt in storage fixture");
    abort();
}
