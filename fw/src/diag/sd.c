// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "diag/sd.h"

#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>

// pico-vfs defines its own path bound.
#undef PATH_MAX
#include "blockdevice/blockdevice.h"
#include "blockdevice/sd.h"
#include "ff.h"
#include "filesystem/filesystem.h"
#include "filesystem/vfs.h"

#include "diag/diag.h"
#include "driver.h"
#include "fatal.h"
#include "global.h"
#include "hw.h"
#include "ieee/diskimage.h"
#include "ieee/dos_media.h"
#include "ieee/ieee_drive.h"
#include "menu/menu_browser.h"
#include "pet.h"
#include "sd/sd.h"

#define SD_DIAG_SECTOR_SIZE 512u
#define SD_DIAG_MAX_TRANSFER_SECTORS 64u
#define SD_DIAG_SWEEP_SECTORS 4096u
#define SD_DIAG_WINDOW_MIN_LBA 16u
#define SD_DIAG_RESERVED_LBAS 128u
#define SD_DIAG_PATTERN_COUNT 6u
#define SD_DIAG_PATTERN_BYTES 4099u
#define SD_DIAG_DIRECT_FILE_BYTES 20480u
#define SD_DIAG_FREE_FILE_BYTES 65536u
#define SD_DIAG_ROM_FILE_BYTES 65536u
#define SD_DIAG_IMAGE_OWNERS (DOS_MEDIA_UNIT_COUNT * DOS_MEDIA_SLOTS_PER_UNIT)
#define SD_DIAG_IMAGE_FILES (SD_DIAG_IMAGE_OWNERS + 1u)
#define SD_DIAG_OPEN_ATTEMPTS 64u
#define SD_DIAG_FAT_ATTRIBUTE_OFFSET 11u
#define SD_DIAG_RANDOM_IO_COUNT 128u
#define SD_DIAG_IMAGE_TAGS 256u
#define SD_DIAG_BROWSER_FILE_COUNT 68u
#define SD_DIAG_BROWSER_DIR_COUNT 2u
#define SD_DIAG_CHILD_NAME_MAX 32u
#define SD_DIAG_PRNG_MULTIPLIER UINT64_C(2685821657736338717)
#define SD_DIAG_BIT_MASK 7u
#define SD_DIAG_WORKSPACE_MAX_BYTES (40u * 1024u)

_Static_assert(SD_DIAG_SWEEP_SECTORS % SD_DIAG_MAX_TRANSFER_SECTORS == 0, "SD sweep must contain complete transfers");
_Static_assert(SD_DIAG_IMAGE_FILES + SD_DIAG_RANDOM_IO_COUNT < SD_DIAG_IMAGE_TAGS,
               "Image initial and update tags must remain distinct");

typedef struct {
    uint8_t tx_buf[SD_DIAG_MAX_TRANSFER_SECTORS * SD_DIAG_SECTOR_SIZE];
    // The attribute helper retires its temporary FIL before receive-buffer I/O resumes.
    union {
        uint8_t rx_buf[SD_DIAG_PATTERN_BYTES];
        FIL attribute_file;
    };
    uint8_t guard_before[SD_DIAG_SECTOR_SIZE];
    uint8_t guard_after[SD_DIAG_SECTOR_SIZE];
    char path_a[SD_PATH_MAX];
    char path_b[SD_PATH_MAX];
    char path_c[SD_PATH_MAX];
    fs_file_t fat_file;
    fs_dir_t fat_dir;
    uint64_t cycle_counter;
    fs_file_t* open_fat_file;
    fs_dir_t* open_fat_dir;
    bool fat_mounted_direct;
    int open_fds[2];
    DIR* open_dirs[2];
    FILE* open_files[SD_DIAG_IMAGE_FILES];
    bool browser_seen[SD_DIAG_BROWSER_FILE_COUNT];
    bool browser_dirs_seen[SD_DIAG_BROWSER_DIR_COUNT];
    blockdevice_t* temp_mode_device;
} sd_diag_workspace_t;

// Leave heap space for the display, FAT caches, and nine simultaneous image streams.
_Static_assert(sizeof(sd_diag_workspace_t) <= SD_DIAG_WORKSPACE_MAX_BYTES,
               "SD diagnostic workspace exceeds its heap budget");

typedef struct {
    size_t bytes;
    size_t expected_bytes;
    size_t calls;
    bool valid;
} sd_read_capture_t;

static sd_diag_workspace_t* workspace;

typedef enum {
    SD_IMAGE_FIRST,
    SD_IMAGE_LAST,
    SD_IMAGE_BEFORE_CLUSTER,
    SD_IMAGE_AFTER_CLUSTER,
    SD_IMAGE_SCATTERED,
    SD_IMAGE_POSITION_COUNT,
} sd_image_position_t;

static const size_t image_sizes[] = {DISKIMAGE_D64_SIZE, DISKIMAGE_D80_SIZE, DISKIMAGE_D82_SIZE};

/** Allocate the single large diagnostic workspace outside Core 0's stack. */
static sd_diag_workspace_t* ensure_workspace(void) {
    if (workspace == NULL) {
        workspace = calloc(1, sizeof(*workspace));
        vet(workspace != NULL, "out of memory for sd diagnostic");
        workspace->open_fds[0] = -1;
        workspace->open_fds[1] = -1;
    }
    return workspace;
}

/** Close all handles that can remain live after diag_fail() longjmps. */
static void sd_diag_cleanup_handles(void) {
    sd_diag_workspace_t* const state = workspace;
    if (state == NULL) {
        return;
    }
    filesystem_t* const fat = sd_fat_filesystem();
    if (state->open_fat_file != NULL && fat != NULL) {
        fat->file_close(fat, state->open_fat_file);
        state->open_fat_file = NULL;
    }
    if (state->open_fat_dir != NULL && fat != NULL) {
        fat->dir_close(fat, state->open_fat_dir);
        state->open_fat_dir = NULL;
    }
    for (size_t index = 0; index < count_of(state->open_fds); ++index) {
        if (state->open_fds[index] >= 0) {
            close(state->open_fds[index]);
        }
        state->open_fds[index] = -1;
    }
    for (size_t index = 0; index < count_of(state->open_dirs); ++index) {
        if (state->open_dirs[index] != NULL) {
            closedir(state->open_dirs[index]);
        }
        state->open_dirs[index] = NULL;
    }
    for (size_t index = 0; index < count_of(state->open_files); ++index) {
        if (state->open_files[index] != NULL) {
            fclose(state->open_files[index]);
        }
        state->open_files[index] = NULL;
    }
    if (state->temp_mode_device != NULL) {
        blockdevice_sd_free(state->temp_mode_device);
    }
    state->temp_mode_device = NULL;
}

/** Distinguish each directory read's EOF from its own error, not stale errno. */
static const struct dirent* read_diag_dir(DIR* directory) {
    errno = 0;
    return readdir(directory);
}

/** Retire a stream before reporting close failure so it cannot be closed twice. */
static void close_diag_stream(size_t index) {
    sd_diag_workspace_t* const state = ensure_workspace();
    FILE* const file = state->open_files[index];
    state->open_files[index] = NULL;
    diag_check(fclose(file) == 0, "stream %zu close failed: errno %d", index, errno);
}

/** Retire a descriptor before reporting close failure. */
static void close_diag_fd(size_t index) {
    sd_diag_workspace_t* const state = ensure_workspace();
    const int fd = state->open_fds[index];
    state->open_fds[index] = -1;
    diag_check(close(fd) == 0, "descriptor close failed: errno %d", errno);
}

/** Release direct FAT and root VFS mounts before raw card access. */
static void sd_diag_cleanup_mounts(void) {
    sd_diag_workspace_t* const state = workspace;
    if (state == NULL) {
        return;
    }
    filesystem_t* const fat = sd_fat_filesystem();
    if (state->fat_mounted_direct && fat != NULL) {
        fat->unmount(fat);
    }
    state->fat_mounted_direct = false;
    sd_unmount_root();
}

/** Begin a case with no resources surviving a prior aborted invocation. */
static void begin_case(void) {
    sd_diag_workspace_t* const state = ensure_workspace();
    sd_diag_cleanup_handles();
    if (state->temp_mode_device != NULL) {
        diag_fail("temporary mode device leaked");
    }
}

/** Build an absolute path on the SD root filesystem. */
static void make_vfs_path(char path[SD_PATH_MAX], const char* suffix) {
    const int written = snprintf(path, SD_PATH_MAX, "%s", suffix);
    diag_check(written >= 0 && (size_t)written < SD_PATH_MAX, "diagnostic path too long: %s", suffix);
}

/** Append a child name to a bounded absolute diagnostic path. */
static void append_path(char path[SD_PATH_MAX], const char* parent, const char* child) {
    const size_t parent_length = strlen(parent);
    const size_t child_length = strlen(child);
    diag_check(parent_length + 1u + child_length < SD_PATH_MAX, "diagnostic child path too long");
    memcpy(path, parent, parent_length);
    path[parent_length] = '/';
    memcpy(path + parent_length + 1, child, child_length + 1);
}

/** Advance a xorshift64* stream from a stable cycle and LBA seed. */
static uint64_t pattern_next(uint64_t* state) {
    uint64_t value = *state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * SD_DIAG_PRNG_MULTIPLIER;
}

/** Fill a sector with cycle- and LBA-specific deterministic bytes. */
static void fill_sector(uint8_t* buffer, uint64_t cycle, uint64_t lba) {
    uint64_t state = (cycle << 32) ^ (lba + UINT64_C(0x9e3779b97f4a7c15));
    for (size_t offset = 0; offset < SD_DIAG_SECTOR_SIZE; offset += sizeof(uint64_t)) {
        const uint64_t word = pattern_next(&state);
        memcpy(buffer + offset, &word, sizeof(word));
    }
}

/** Fill a contiguous range with independent sector patterns. */
static void fill_sectors(uint8_t* buffer, uint64_t cycle, uint64_t lba, size_t sectors) {
    for (size_t sector = 0; sector < sectors; ++sector) {
        fill_sector(buffer + sector * SD_DIAG_SECTOR_SIZE, cycle, lba + sector);
    }
}

/** Compare a byte buffer and report the exact LBA and byte offset. */
static void verify_bytes(const uint8_t* actual, const uint8_t* expected, uint64_t first_lba, size_t size) {
    for (size_t offset = 0; offset < size; ++offset) {
        if (actual[offset] == expected[offset]) {
            continue;
        }
        diag_fail("LBA %" PRIu64 " +0x%03X: expected $%02X, got $%02X", first_lba + offset / SD_DIAG_SECTOR_SIZE,
                  (unsigned)(offset % SD_DIAG_SECTOR_SIZE), (unsigned)expected[offset], (unsigned)actual[offset]);
    }
}

/** Program and verify a complete sector transfer. */
static void program_verify(blockdevice_t* device, uint64_t lba, const uint8_t* buffer, size_t sectors) {
    const bd_size_t addr = (bd_size_t)lba * SD_DIAG_SECTOR_SIZE;
    const size_t bytes = sectors * SD_DIAG_SECTOR_SIZE;
    diag_check(bytes <= sizeof(ensure_workspace()->rx_buf), "small transfer buffer overflow");
    diag_check(device->program(device, buffer, addr, bytes) == BD_ERROR_OK,
               "program LBA %" PRIu64 " sectors %zu failed", lba, sectors);
    memset(ensure_workspace()->rx_buf, 0xa5, bytes);
    diag_check(device->read(device, ensure_workspace()->rx_buf, addr, bytes) == BD_ERROR_OK,
               "read LBA %" PRIu64 " sectors %zu failed", lba, sectors);
    verify_bytes(ensure_workspace()->rx_buf, buffer, lba, bytes);
}

/** Read a full multi-block request into the transfer buffer and regenerate its expected sectors. */
static void read_verify_patterns(blockdevice_t* device, uint64_t lba, size_t sectors) {
    sd_diag_workspace_t* const state = ensure_workspace();
    const size_t bytes = sectors * SD_DIAG_SECTOR_SIZE;
    diag_check(bytes <= sizeof(state->tx_buf), "pattern transfer buffer overflow");
    // Retained write data must not hide an incomplete read reported as successful.
    memset(state->tx_buf, 0xa5, bytes);
    diag_check(device->read(device, state->tx_buf, lba * SD_DIAG_SECTOR_SIZE, bytes) == BD_ERROR_OK,
               "read LBA %" PRIu64 " sectors %zu failed", lba, sectors);
    for (size_t sector = 0; sector < sectors; ++sector) {
        fill_sector(state->rx_buf, state->cycle_counter, lba + sector);
        verify_bytes(state->tx_buf + sector * SD_DIAG_SECTOR_SIZE, state->rx_buf, lba + sector, SD_DIAG_SECTOR_SIZE);
    }
}

/** Preserve large write/read request sizes without retaining a second full-size buffer. */
static void program_verify_patterns(blockdevice_t* device, uint64_t lba, size_t sectors) {
    sd_diag_workspace_t* const state = ensure_workspace();
    diag_check(sectors <= SD_DIAG_MAX_TRANSFER_SECTORS, "pattern write buffer overflow");
    fill_sectors(state->tx_buf, state->cycle_counter, lba, sectors);
    diag_check(device->program(device, state->tx_buf, lba * SD_DIAG_SECTOR_SIZE, sectors * SD_DIAG_SECTOR_SIZE) ==
                   BD_ERROR_OK,
               "program LBA %" PRIu64 " sectors %zu failed", lba, sectors);
    read_verify_patterns(device, lba, sectors);
}

/** Compute a bounded rotating raw-I/O base for the current burn-in cycle. */
static uint64_t window_base(blockdevice_t* device) {
    const bd_size_t sectors = device->size(device) / SD_DIAG_SECTOR_SIZE;
    const uint64_t reserved = SD_DIAG_RESERVED_LBAS + SD_DIAG_SWEEP_SECTORS;
    diag_check(sectors > reserved + SD_DIAG_WINDOW_MIN_LBA, "block device too small for diagnostic window");
    const uint64_t span = (uint64_t)sectors - reserved - SD_DIAG_WINDOW_MIN_LBA;
    return SD_DIAG_WINDOW_MIN_LBA + (ensure_workspace()->cycle_counter % span);
}

/** Validate initialization, geometry, and basic erase/sync behavior. */
static void case_init_geometry(const void* context) {
    (void)context;
    begin_case();
    blockdevice_t* const device = sd_blockdevice();
    sd_diag_cleanup_mounts();
    diag_check(device != NULL, "SD block device is unavailable");
    diag_check(device->deinit(device) == BD_ERROR_OK, "initial deinit failed");
    diag_check(device->init(device) == BD_ERROR_OK && device->is_initialized, "initial init failed");
    diag_check(device->read_size == SD_DIAG_SECTOR_SIZE && device->program_size == SD_DIAG_SECTOR_SIZE,
               "unexpected SD transfer size");
    diag_check(device->erase_size >= SD_DIAG_SECTOR_SIZE && device->erase_size % SD_DIAG_SECTOR_SIZE == 0,
               "unexpected SD erase size");
    const bd_size_t bytes = device->size(device);
    diag_check(bytes >= ((bd_size_t)SD_DIAG_SWEEP_SECTORS + SD_DIAG_MAX_TRANSFER_SECTORS) * SD_DIAG_SECTOR_SIZE &&
                   bytes % SD_DIAG_SECTOR_SIZE == 0,
               "SD size is too small or unaligned");
    diag_check(device->sync(device) == BD_ERROR_OK, "initial sync failed");
    diag_check(device->erase(device, 0, SD_DIAG_SECTOR_SIZE) == BD_ERROR_OK, "initial erase failed");
    ++ensure_workspace()->cycle_counter;
}

/** Verify repeat initialization and transport after full deinitialization. */
static void case_reinit(const void* context) {
    (void)context;
    begin_case();
    blockdevice_t* const device = sd_blockdevice();
    diag_check(device != NULL, "SD block device is unavailable");
    diag_check(device->init(device) == BD_ERROR_OK && device->is_initialized, "idempotent init failed");
    diag_check(device->deinit(device) == BD_ERROR_OK, "deinit failed");
    diag_check(device->deinit(device) == BD_ERROR_OK, "second deinit failed");
    diag_check(device->init(device) == BD_ERROR_OK && device->is_initialized, "reinit failed");
    fill_sectors(ensure_workspace()->tx_buf, ensure_workspace()->cycle_counter, 0, 1);
    program_verify(device, 0, ensure_workspace()->tx_buf, 1);
}

/** Verify data survives rebuilding the SD transport with production settings. */
static void case_spi_configuration(const void* context) {
    (void)context;
    begin_case();
    const uint32_t hz = SD_SPI_MHZ * UINT32_C(1000000);
    blockdevice_t* const primary = sd_blockdevice();
    fill_sectors(ensure_workspace()->tx_buf, ensure_workspace()->cycle_counter, 0, 2);
    program_verify(primary, 0, ensure_workspace()->tx_buf, 2);
    diag_check(primary->deinit(primary) == BD_ERROR_OK, "SPI pre-deinit failed");

    // Read the retained device's data through a newly constructed transport.
    blockdevice_t* const device =
        blockdevice_sd_create(SD_SPI_INSTANCE, SD_CMD_GP, SD_DAT_GP, SD_CLK_GP, SD_CSN_GP, hz, SD_SPI_ENABLE_CRC);
    ensure_workspace()->temp_mode_device = device;
    diag_check(device != NULL && device->is_initialized, "SPI %u Hz CRC=%u init failed", (unsigned)hz,
               (unsigned)SD_SPI_ENABLE_CRC);
    diag_check(device->read(device, ensure_workspace()->rx_buf, 0, 2u * SD_DIAG_SECTOR_SIZE) == BD_ERROR_OK,
               "SPI cross-read failed");
    verify_bytes(ensure_workspace()->rx_buf, ensure_workspace()->tx_buf, 0, 2u * SD_DIAG_SECTOR_SIZE);
    fill_sectors(ensure_workspace()->tx_buf, ensure_workspace()->cycle_counter, 1, 2);
    program_verify(device, 0, ensure_workspace()->tx_buf, 2);
    diag_check(device->deinit(device) == BD_ERROR_OK, "SPI deinit failed");
    blockdevice_sd_free(device);
    ensure_workspace()->temp_mode_device = NULL;

    // Restore the boot-owned transport and verify the new device's writes.
    diag_check(primary->init(primary) == BD_ERROR_OK && primary->is_initialized,
               "primary reinit after SPI test failed");
    memset(ensure_workspace()->rx_buf, 0xa5, 2u * SD_DIAG_SECTOR_SIZE);
    diag_check(primary->read(primary, ensure_workspace()->rx_buf, 0, 2u * SD_DIAG_SECTOR_SIZE) == BD_ERROR_OK,
               "primary SPI read failed");
    verify_bytes(ensure_workspace()->rx_buf, ensure_workspace()->tx_buf, 0, 2u * SD_DIAG_SECTOR_SIZE);
}

/** Run the standard single-sector patterns at LBA 0 and the rotating window. */
static void case_single_patterns(const void* context) {
    (void)context;
    begin_case();
    blockdevice_t* const device = sd_blockdevice();
    const uint64_t lbas[] = {0, window_base(device)};
    for (size_t lba_index = 0; lba_index < count_of(lbas); ++lba_index) {
        for (unsigned pattern = 0; pattern < SD_DIAG_PATTERN_COUNT; ++pattern) {
            switch (pattern) {
            case 0:
                memset(ensure_workspace()->tx_buf, 0x00, SD_DIAG_SECTOR_SIZE);
                break;
            case 1:
                memset(ensure_workspace()->tx_buf, 0xff, SD_DIAG_SECTOR_SIZE);
                break;
            case 2:
                memset(ensure_workspace()->tx_buf, 0xaa, SD_DIAG_SECTOR_SIZE);
                break;
            case 3:
                memset(ensure_workspace()->tx_buf, 0x55, SD_DIAG_SECTOR_SIZE);
                break;
            case 4:
                for (size_t byte = 0; byte < SD_DIAG_SECTOR_SIZE; ++byte) {
                    ensure_workspace()->tx_buf[byte] = (byte < SD_DIAG_SECTOR_SIZE / 2)
                                                           ? (uint8_t)(1u << (byte & SD_DIAG_BIT_MASK))
                                                           : (uint8_t)~(1u << (byte & SD_DIAG_BIT_MASK));
                }
                break;
            case 5:
                fill_sector(ensure_workspace()->tx_buf, ensure_workspace()->cycle_counter, lbas[lba_index]);
                break;
            }
            program_verify(device, lbas[lba_index], ensure_workspace()->tx_buf, 1);
        }
    }
}

/** Catch writes leaking into neighboring sectors. */
static void case_neighbors(const void* context) {
    (void)context;
    begin_case();
    blockdevice_t* const device = sd_blockdevice();
    const uint64_t middle = window_base(device) + 2;
    fill_sectors(ensure_workspace()->guard_before, ensure_workspace()->cycle_counter, middle - 1, 1);
    fill_sectors(ensure_workspace()->guard_after, ensure_workspace()->cycle_counter, middle + 1, 1);
    program_verify(device, middle - 1, ensure_workspace()->guard_before, 1);
    program_verify(device, middle + 1, ensure_workspace()->guard_after, 1);
    for (unsigned pass = 0; pass < 2; ++pass) {
        memset(ensure_workspace()->tx_buf, pass == 0 ? 0 : 0xff, SD_DIAG_SECTOR_SIZE);
        program_verify(device, middle, ensure_workspace()->tx_buf, 1);
        diag_check(device->read(device, ensure_workspace()->rx_buf, (middle - 1) * SD_DIAG_SECTOR_SIZE,
                                SD_DIAG_SECTOR_SIZE) == BD_ERROR_OK,
                   "left neighbor read failed");
        verify_bytes(ensure_workspace()->rx_buf, ensure_workspace()->guard_before, middle - 1, SD_DIAG_SECTOR_SIZE);
        diag_check(device->read(device, ensure_workspace()->rx_buf, (middle + 1) * SD_DIAG_SECTOR_SIZE,
                                SD_DIAG_SECTOR_SIZE) == BD_ERROR_OK,
                   "right neighbor read failed");
        verify_bytes(ensure_workspace()->rx_buf, ensure_workspace()->guard_after, middle + 1, SD_DIAG_SECTOR_SIZE);
    }
}

/** Cross-check multi-block requests against separate single-block commands. */
static void case_multiblock_cross(const void* context) {
    (void)context;
    begin_case();
    blockdevice_t* const device = sd_blockdevice();
    const uint64_t base = window_base(device) + 8;
    fill_sectors(ensure_workspace()->tx_buf, ensure_workspace()->cycle_counter, base, 2);
    diag_check(device->program(device, ensure_workspace()->tx_buf, base * SD_DIAG_SECTOR_SIZE,
                               2u * SD_DIAG_SECTOR_SIZE) == BD_ERROR_OK,
               "CMD25 two-sector program failed");
    for (unsigned sector = 0; sector < 2; ++sector) {
        diag_check(device->read(device, ensure_workspace()->rx_buf, (base + sector) * SD_DIAG_SECTOR_SIZE,
                                SD_DIAG_SECTOR_SIZE) == BD_ERROR_OK,
                   "CMD17 sector %u failed", sector);
        verify_bytes(ensure_workspace()->rx_buf, ensure_workspace()->tx_buf + sector * SD_DIAG_SECTOR_SIZE,
                     base + sector, SD_DIAG_SECTOR_SIZE);
    }
    fill_sectors(ensure_workspace()->tx_buf, ensure_workspace()->cycle_counter + 1, base, 2);
    for (unsigned sector = 0; sector < 2; ++sector) {
        diag_check(device->program(device, ensure_workspace()->tx_buf + sector * SD_DIAG_SECTOR_SIZE,
                                   (base + sector) * SD_DIAG_SECTOR_SIZE, SD_DIAG_SECTOR_SIZE) == BD_ERROR_OK,
                   "CMD24 sector %u failed", sector);
    }
    diag_check(device->read(device, ensure_workspace()->rx_buf, base * SD_DIAG_SECTOR_SIZE, 2u * SD_DIAG_SECTOR_SIZE) ==
                   BD_ERROR_OK,
               "CMD18 two-sector read failed");
    verify_bytes(ensure_workspace()->rx_buf, ensure_workspace()->tx_buf, base, 2u * SD_DIAG_SECTOR_SIZE);
}

/** Exercise aligned and odd 8- and 64-sector multi-block transfers. */
static void case_multiblock_sizes(const void* context) {
    (void)context;
    begin_case();
    blockdevice_t* const device = sd_blockdevice();
    const uint64_t base = window_base(device);
    const size_t sizes[] = {8u, SD_DIAG_MAX_TRANSFER_SECTORS};
    const uint64_t offsets[] = {16, 25};
    size_t completed = 0;
    const size_t total = count_of(sizes) * count_of(offsets);
    for (size_t size_index = 0; size_index < count_of(sizes); ++size_index) {
        for (size_t offset_index = 0; offset_index < count_of(offsets); ++offset_index) {
            const uint64_t lba = base + offsets[offset_index];
            program_verify_patterns(device, lba, sizes[size_index]);
            diag_case_progress(++completed, total);
        }
    }
}

/** Check zero, sub-unit, and multi-unit trim followed by durable programming. */
static void case_trim(const void* context) {
    (void)context;
    begin_case();
    blockdevice_t* const device = sd_blockdevice();
    const uint64_t base = window_base(device);
    diag_check(device->trim(device, 0, 0) == BD_ERROR_OK, "zero trim failed");
    diag_check(device->trim(device, (base + 96) * SD_DIAG_SECTOR_SIZE, SD_DIAG_SECTOR_SIZE) == BD_ERROR_OK,
               "single-sector trim failed");
    const uint64_t trim_lba = base + SD_DIAG_RESERVED_LBAS;
    diag_check(device->trim(device, trim_lba * SD_DIAG_SECTOR_SIZE,
                            SD_DIAG_MAX_TRANSFER_SECTORS * SD_DIAG_SECTOR_SIZE) == BD_ERROR_OK,
               "multi-sector trim failed");
    program_verify_patterns(device, trim_lba, SD_DIAG_MAX_TRANSFER_SECTORS);
}

/** Write and then verify the complete sequential sweep region. */
static void case_sweep(const void* context) {
    (void)context;
    begin_case();
    blockdevice_t* const device = sd_blockdevice();
    const uint64_t base = window_base(device) + SD_DIAG_RESERVED_LBAS;
    const size_t total_progress = SD_DIAG_SWEEP_SECTORS * 2u;
    size_t completed = 0;
    for (uint64_t lba = base; lba < base + SD_DIAG_SWEEP_SECTORS; lba += SD_DIAG_MAX_TRANSFER_SECTORS) {
        fill_sectors(ensure_workspace()->tx_buf, ensure_workspace()->cycle_counter, lba, SD_DIAG_MAX_TRANSFER_SECTORS);
        diag_check(device->program(device, ensure_workspace()->tx_buf, lba * SD_DIAG_SECTOR_SIZE,
                                   sizeof(ensure_workspace()->tx_buf)) == BD_ERROR_OK,
                   "sweep write LBA %" PRIu64 " failed", lba);
        completed += SD_DIAG_MAX_TRANSFER_SECTORS;
        diag_case_progress(completed, total_progress);
    }
    for (uint64_t lba = base; lba < base + SD_DIAG_SWEEP_SECTORS; lba += SD_DIAG_MAX_TRANSFER_SECTORS) {
        read_verify_patterns(device, lba, SD_DIAG_MAX_TRANSFER_SECTORS);
        completed += SD_DIAG_MAX_TRANSFER_SECTORS;
        diag_case_progress(completed, total_progress);
    }
}

/** Format a corrupted raw device and leave FAT mounted directly. */
static void case_fat_remount(const void* context) {
    (void)context;
    begin_case();
    blockdevice_t* const device = sd_blockdevice();
    filesystem_t* const fat = sd_fat_filesystem();
    diag_check(fat != NULL, "FAT filesystem is unavailable");
    diag_check(fat->mount(fat, device, false) != 0, "corrupt raw LBA 0 mounted");
    (void)fat->unmount(fat);
    diag_check(fat->format(fat, device) == 0, "FAT format failed");
    diag_check(fat->mount(fat, device, false) == 0, "FAT mount after format failed");
    ensure_workspace()->fat_mounted_direct = true;
    diag_check(fat->unmount(fat) == 0, "FAT post-format unmount failed");
    ensure_workspace()->fat_mounted_direct = false;
    diag_check(fat->mount(fat, device, false) == 0, "FAT remount failed");
    ensure_workspace()->fat_mounted_direct = true;
}

/** Validate sd_free_bytes around a temporary 64 KiB allocation. */
static void case_free_space(const void* context) {
    (void)context;
    begin_case();
    filesystem_t* const fat = sd_fat_filesystem();
    const uint64_t free_before = sd_free_bytes();
    diag_check(free_before > 0 && free_before <= sd_blockdevice()->size(sd_blockdevice()),
               "invalid initial free-space report");
    diag_check(fat->file_open(fat, &ensure_workspace()->fat_file, "/_free.bin", O_WRONLY | O_CREAT | O_TRUNC) == 0,
               "free-space file open failed");
    ensure_workspace()->open_fat_file = &ensure_workspace()->fat_file;
    memset(ensure_workspace()->tx_buf, 0x5a, sizeof(ensure_workspace()->tx_buf));
    for (size_t written = 0; written < SD_DIAG_FREE_FILE_BYTES; written += sizeof(ensure_workspace()->tx_buf)) {
        diag_check(fat->file_write(fat, ensure_workspace()->open_fat_file, ensure_workspace()->tx_buf,
                                   sizeof(ensure_workspace()->tx_buf)) == (ssize_t)sizeof(ensure_workspace()->tx_buf),
                   "free-space file write failed");
    }
    diag_check(fat->file_close(fat, ensure_workspace()->open_fat_file) == 0, "free-space file close failed");
    ensure_workspace()->open_fat_file = NULL;
    const uint64_t free_during = sd_free_bytes();
    diag_check(free_during < free_before, "free space did not decrease");
    diag_check(fat->remove(fat, "/_free.bin") == 0, "free-space file remove failed");
    diag_check(sd_free_bytes() == free_before, "free space was not restored");
}

/** Exercise production FAT cursor, sync, live metadata, and complete readback. */
static void case_fat_file(const void* context) {
    (void)context;
    begin_case();
    filesystem_t* const fat = sd_fat_filesystem();
    fs_file_t* const file = &ensure_workspace()->fat_file;
    diag_check(fat->file_open(fat, file, "/fat_io.bin", O_RDWR | O_CREAT | O_TRUNC) == 0, "direct FAT open failed");
    ensure_workspace()->open_fat_file = file;
    fill_sectors(ensure_workspace()->tx_buf, ensure_workspace()->cycle_counter, 700,
                 SD_DIAG_DIRECT_FILE_BYTES / SD_DIAG_SECTOR_SIZE + 1);
    const size_t odd_bytes = SD_DIAG_DIRECT_FILE_BYTES + 137;
    diag_check(fat->file_write(fat, file, ensure_workspace()->tx_buf, odd_bytes) == (ssize_t)odd_bytes,
               "direct FAT write failed");
    diag_check(fat->file_sync(fat, file) == 0, "direct FAT sync failed");
    diag_check(fat->file_tell(fat, file) == (off_t)odd_bytes && fat->file_size(fat, file) == (off_t)odd_bytes,
               "direct FAT cursor/size mismatch");
    diag_check(fat->file_seek(fat, file, 137, SEEK_SET) == 137, "direct FAT SEEK_SET failed");
    diag_check(fat->file_seek(fat, file, 19, SEEK_CUR) == 156, "direct FAT SEEK_CUR failed");
    diag_check(fat->file_seek(fat, file, -137, SEEK_END) == SD_DIAG_DIRECT_FILE_BYTES, "direct FAT SEEK_END failed");
    memset(ensure_workspace()->rx_buf, 0xa5, 137);
    diag_check(fat->file_read(fat, file, ensure_workspace()->rx_buf, 137) == 137, "direct FAT read failed");
    diag_check(memcmp(ensure_workspace()->rx_buf, ensure_workspace()->tx_buf + SD_DIAG_DIRECT_FILE_BYTES, 137) == 0,
               "direct FAT readback mismatch");
    struct stat info = {0};
    diag_check(fat->stat(fat, "/fat_io.bin", &info) == 0 && info.st_size == (off_t)odd_bytes,
               "direct FAT live metadata mismatch");
    diag_check(fat->file_close(fat, file) == 0, "direct FAT close failed");
    ensure_workspace()->open_fat_file = NULL;
    diag_check(fat->file_open(fat, file, "/fat_io.bin", O_RDONLY) == 0, "direct FAT reopen failed");
    ensure_workspace()->open_fat_file = file;
    diag_check(fat->file_size(fat, file) == (off_t)odd_bytes, "direct FAT reopened size mismatch");
    for (size_t offset = 0; offset < odd_bytes; offset += TEMP_BUFFER_SIZE) {
        const size_t length = MIN(odd_bytes - offset, TEMP_BUFFER_SIZE);
        diag_check(fat->file_read(fat, file, ensure_workspace()->rx_buf, length) == (ssize_t)length &&
                       memcmp(ensure_workspace()->rx_buf, ensure_workspace()->tx_buf + offset, length) == 0,
                   "direct FAT readback at %zu mismatch", offset);
    }
    diag_check(fat->file_close(fat, file) == 0, "direct FAT final close failed");
    ensure_workspace()->open_fat_file = NULL;
    diag_check(fat->remove(fat, "/fat_io.bin") == 0, "direct FAT remove failed");
}

/** Change fixture permissions on disk without enabling nonproduction FatFs chmod. */
static void set_fat_readonly(const char* path, bool readonly) {
    sd_diag_workspace_t* const state = ensure_workspace();
    filesystem_t* const fat = sd_fat_filesystem();
    blockdevice_t* const device = sd_blockdevice();
    FIL* const file = &state->attribute_file;
    diag_check(f_open(file, path, FA_READ) == FR_OK, "attribute fixture open failed");
    const LBA_t sector = file->dir_sect;
    const size_t attribute = (size_t)(file->dir_ptr - file->obj.fs->win) + SD_DIAG_FAT_ATTRIBUTE_OFFSET;
    const FRESULT closed = f_close(file);
    diag_check(closed == FR_OK && attribute < SD_DIAG_SECTOR_SIZE, "attribute fixture location failed");

    // Raw metadata access requires every filesystem owner and cache to be retired.
    diag_check(fat->unmount(fat) == 0, "attribute fixture unmount failed");
    state->fat_mounted_direct = false;
    const bd_size_t address = sector * SD_DIAG_SECTOR_SIZE;
    diag_check(device->read(device, state->tx_buf, address, SD_DIAG_SECTOR_SIZE) == 0, "attribute sector read failed");
    if (readonly) {
        state->tx_buf[attribute] |= AM_RDO;
    } else {
        state->tx_buf[attribute] &= (uint8_t)~AM_RDO;
    }
    diag_check(device->erase(device, address, SD_DIAG_SECTOR_SIZE) == 0 &&
                   device->program(device, state->tx_buf, address, SD_DIAG_SECTOR_SIZE) == 0,
               "attribute sector write failed");
    diag_check(fat->mount(fat, device, false) == 0, "attribute fixture remount failed");
    state->fat_mounted_direct = true;
}

/** Repeated writable-open rejection must permit the product's read-only fallback. */
static void case_fat_readonly(const void* context) {
    (void)context;
    begin_case();
    sd_diag_workspace_t* const state = ensure_workspace();
    filesystem_t* const fat = sd_fat_filesystem();
    fs_file_t* const file = &state->fat_file;
    const char* const path = "/readonly.bin";
    diag_check(fat->file_open(fat, file, path, O_CREAT | O_RDWR) == 0, "read-only fixture create failed");
    state->open_fat_file = file;
    fill_sector(state->tx_buf, state->cycle_counter, 0);
    diag_check(fat->file_write(fat, file, state->tx_buf, SD_DIAG_SECTOR_SIZE) == SD_DIAG_SECTOR_SIZE,
               "read-only fixture write failed");
    const int closed = fat->file_close(fat, file);
    state->open_fat_file = NULL;
    diag_check(closed == 0, "read-only fixture close failed");
    set_fat_readonly(path, true);

    for (size_t attempt = 0; attempt < SD_DIAG_OPEN_ATTEMPTS; ++attempt) {
        const int rejected = fat->file_open(fat, file, path, O_RDWR);
        if (rejected == 0) {
            state->open_fat_file = file;
        }
        diag_check(rejected == -EACCES && file->context == NULL, "read-only update open: rc %d", rejected);
        diag_check(fat->file_open(fat, file, path, O_RDONLY) == 0, "read-only fallback open failed");
        state->open_fat_file = file;
        fill_sector(state->tx_buf, state->cycle_counter, 0);
        diag_check(fat->file_read(fat, file, state->rx_buf, SD_DIAG_SECTOR_SIZE) == SD_DIAG_SECTOR_SIZE,
                   "read-only fallback read failed");
        verify_bytes(state->rx_buf, state->tx_buf, 0, SD_DIAG_SECTOR_SIZE);
        const int result = fat->file_close(fat, file);
        state->open_fat_file = NULL;
        diag_check(result == 0, "read-only fallback close failed");
    }

    // Repeat the real stdio fallback through VFS/newlib, not just the FAT adapter.
    diag_check(fat->unmount(fat) == 0, "read-only direct unmount failed");
    state->fat_mounted_direct = false;
    diag_check(sd_mount_root(), "read-only VFS mount failed");
    for (size_t attempt = 0; attempt < SD_DIAG_OPEN_ATTEMPTS; ++attempt) {
        errno = 0;
        state->open_files[0] = fopen(path, "r+b");
        diag_check(state->open_files[0] == NULL && errno == EACCES, "read-only stdio update open: errno %d", errno);
        state->open_files[0] = fopen(path, "rb");
        diag_check(state->open_files[0] != NULL && setvbuf(state->open_files[0], NULL, _IONBF, 0) == 0,
                   "read-only stdio fallback failed");
        diag_check(fread(state->rx_buf, 1, SD_DIAG_SECTOR_SIZE, state->open_files[0]) == SD_DIAG_SECTOR_SIZE,
                   "read-only stdio read failed");
        fill_sector(state->tx_buf, state->cycle_counter, 0);
        verify_bytes(state->rx_buf, state->tx_buf, 0, SD_DIAG_SECTOR_SIZE);
        close_diag_stream(0);
    }
    diag_check(sd_unmount_root(), "read-only VFS unmount failed");
    diag_check(fat->mount(fat, sd_blockdevice(), false) == 0, "read-only direct remount failed");
    state->fat_mounted_direct = true;
    set_fat_readonly(path, false);
    diag_check(fat->remove(fat, path) == 0, "read-only fixture cleanup failed");
}

/** Exercise direct FAT directory iteration, stat, rename, and cleanup. */
static void case_fat_dirs(const void* context) {
    (void)context;
    begin_case();
    filesystem_t* const fat = sd_fat_filesystem();
    const char* const roots[] = {"/rom", "/disk", "/prg"};
    for (size_t index = 0; index < count_of(roots); ++index) {
        (void)fat->mkdir(fat, roots[index], 0777);
    }
    diag_check(fat->mkdir(fat, "/disk/Diagnostic Suite Dir", 0777) == 0, "FAT LFN directory create failed");
    const char* const names[] = {"alpha.lfn", "middle.lfn", "omega.lfn"};
    for (size_t index = 0; index < count_of(names); ++index) {
        snprintf(ensure_workspace()->path_a, SD_PATH_MAX, "/disk/Diagnostic Suite Dir/%s", names[index]);
        diag_check(fat->file_open(fat, &ensure_workspace()->fat_file, ensure_workspace()->path_a,
                                  O_WRONLY | O_CREAT | O_TRUNC) == 0,
                   "FAT directory file open failed");
        ensure_workspace()->open_fat_file = &ensure_workspace()->fat_file;
        diag_check(fat->file_write(fat, ensure_workspace()->open_fat_file, names[index], strlen(names[index])) ==
                       (ssize_t)strlen(names[index]),
                   "FAT directory file write failed");
        diag_check(fat->file_close(fat, ensure_workspace()->open_fat_file) == 0, "FAT directory file close failed");
        ensure_workspace()->open_fat_file = NULL;
    }
    struct stat info;
    diag_check(fat->stat(fat, "/disk/Diagnostic Suite Dir", &info) == 0 && S_ISDIR(info.st_mode),
               "FAT directory stat failed");
    diag_check(fat->stat(fat, "/disk/Diagnostic Suite Dir/alpha.lfn", &info) == 0 && S_ISREG(info.st_mode) &&
                   info.st_size == (off_t)strlen(names[0]),
               "FAT file stat failed");
    diag_check(fat->dir_open(fat, &ensure_workspace()->fat_dir, "/disk/Diagnostic Suite Dir") == 0,
               "FAT directory open failed");
    ensure_workspace()->open_fat_dir = &ensure_workspace()->fat_dir;
    bool seen[count_of(names)] = {false};
    struct dirent entry;
    int directory_result;
    while ((directory_result = fat->dir_read(fat, ensure_workspace()->open_fat_dir, &entry)) == 0) {
        bool found = false;
        for (size_t index = 0; index < count_of(names); ++index) {
            if (strcmp(entry.d_name, names[index]) != 0) {
                continue;
            }
            diag_check(!seen[index] && entry.d_type == DT_REG, "FAT directory duplicate/type mismatch: %s",
                       entry.d_name);
            seen[index] = found = true;
            break;
        }
        diag_check(found, "FAT unexpected directory entry: %s", entry.d_name);
    }
    diag_check(directory_result == -ENOENT, "FAT directory EOF: rc %d", directory_result);
    diag_check(fat->dir_close(fat, ensure_workspace()->open_fat_dir) == 0, "FAT directory close failed");
    ensure_workspace()->open_fat_dir = NULL;
    for (size_t index = 0; index < count_of(names); ++index) {
        diag_check(seen[index], "FAT directory entry missing: %s", names[index]);
    }
    diag_check(fat->rename(fat, "/disk/Diagnostic Suite Dir/alpha.lfn", "/disk/Diagnostic Suite Dir/renamed.lfn") == 0,
               "FAT rename failed");
    diag_check(fat->stat(fat, "/disk/Diagnostic Suite Dir/alpha.lfn", &info) == -ENOENT,
               "FAT old rename path still exists");
    diag_check(fat->stat(fat, "/disk/Diagnostic Suite Dir/renamed.lfn", &info) == 0, "FAT renamed path missing");
    const char* const cleanup[] = {"renamed.lfn", "middle.lfn", "omega.lfn"};
    for (size_t index = 0; index < count_of(cleanup); ++index) {
        snprintf(ensure_workspace()->path_b, SD_PATH_MAX, "/disk/Diagnostic Suite Dir/%s", cleanup[index]);
        diag_check(fat->remove(fat, ensure_workspace()->path_b) == 0, "FAT cleanup file remove failed");
    }
    diag_check(fat->rmdir(fat, "/disk/Diagnostic Suite Dir") == 0, "FAT temporary directory remove failed");
}

/** Resolve a case-insensitive LFN and reject an absent path. */
static void case_resolve(const void* context) {
    (void)context;
    begin_case();
    filesystem_t* const fat = sd_fat_filesystem();
    const int disk_result = fat->mkdir(fat, "/disk", 0777);
    diag_check(disk_result == 0 || disk_result == -EEXIST, "resolve disk directory failed");
    diag_check(fat->mkdir(fat, "/disk/Mixed Case Folder", 0777) == 0, "resolve LFN directory failed");
    diag_check(fat->file_open(fat, &ensure_workspace()->fat_file, "/disk/Mixed Case Folder/Super PET Test.d64",
                              O_WRONLY | O_CREAT | O_TRUNC) == 0,
               "resolve LFN file open failed");
    ensure_workspace()->open_fat_file = &ensure_workspace()->fat_file;
    diag_check(fat->file_close(fat, ensure_workspace()->open_fat_file) == 0, "resolve LFN file close failed");
    ensure_workspace()->open_fat_file = NULL;
    strcpy(ensure_workspace()->path_a, "/DISK/mixed case folder/SUPER pet TEST.D64");
    diag_check(sd_resolve_path(ensure_workspace()->path_a), "case-insensitive resolve failed");
    diag_check(strcmp(ensure_workspace()->path_a, "/disk/Mixed Case Folder/Super PET Test.d64") == 0,
               "resolved canonical LFN mismatch");
    strcpy(ensure_workspace()->path_a, "/disk/Mixed Case Folder/missing.d64");
    diag_check(!sd_resolve_path(ensure_workspace()->path_a), "missing resolve path unexpectedly succeeded");
    diag_check(fat->remove(fat, "/disk/Mixed Case Folder/Super PET Test.d64") == 0 &&
                   fat->rmdir(fat, "/disk/Mixed Case Folder") == 0,
               "resolve cleanup failed");
    diag_check(fat->unmount(fat) == 0, "direct FAT final unmount failed");
    ensure_workspace()->fat_mounted_direct = false;
}

/** Mount the VFS root, reformat it, and recreate standard directories. */
static void case_vfs_mount(const void* context) {
    (void)context;
    begin_case();
    diag_check(sd_mount_root(), "VFS root mount failed");
    diag_check(fs_reformat(sd_dir_prefix(SD_DIR_ROOT)) == 0, "VFS reformat failed");
    const char* const directories[] = {"/rom", "/disk", "/prg", "/_diag"};
    for (size_t index = 0; index < count_of(directories); ++index) {
        make_vfs_path(ensure_workspace()->path_a, directories[index]);
        const int created = mkdir(ensure_workspace()->path_a, 0777);
        diag_check(created == 0, "VFS mkdir failed: %s", directories[index]);
    }
}

/** Exercise POSIX descriptors and cursor operations through production VFS. */
static void case_posix_files(const void* context) {
    (void)context;
    begin_case();
    make_vfs_path(ensure_workspace()->path_a, "/_diag/posix.bin");
    errno = 0;
    const int missing = open(ensure_workspace()->path_a, O_RDONLY);
    if (missing >= 0) {
        close(missing);
    }
    diag_check(missing == -1 && errno == ENOENT, "missing POSIX open did not report ENOENT");
    int* const fd = &ensure_workspace()->open_fds[0];
    *fd = open(ensure_workspace()->path_a, O_CREAT | O_TRUNC | O_RDWR, 0666);
    diag_check(*fd >= 0, "POSIX create failed");
    fill_sectors(ensure_workspace()->tx_buf, ensure_workspace()->cycle_counter, 0,
                 (SD_DIAG_PATTERN_BYTES + SD_DIAG_SECTOR_SIZE - 1) / SD_DIAG_SECTOR_SIZE);
    diag_check(write(*fd, ensure_workspace()->tx_buf, SD_DIAG_PATTERN_BYTES) == SD_DIAG_PATTERN_BYTES,
               "POSIX write failed");
    diag_check(lseek(*fd, 137, SEEK_SET) == 137 && lseek(*fd, 19, SEEK_CUR) == 156 &&
                   lseek(*fd, -137, SEEK_END) == SD_DIAG_PATTERN_BYTES - 137,
               "POSIX seek failed");
    diag_check(read(*fd, ensure_workspace()->rx_buf, 137) == 137 &&
                   memcmp(ensure_workspace()->rx_buf, ensure_workspace()->tx_buf + SD_DIAG_PATTERN_BYTES - 137, 137) ==
                       0,
               "POSIX readback failed");
    struct stat info = {0};
    errno = 0;
    const int stat_result = fstat(*fd, &info);
    const int stat_errno = errno;
    diag_check(stat_result == 0 && info.st_size == SD_DIAG_PATTERN_BYTES,
               "POSIX fstat: rc %d errno %d size %jd (expected %u)", stat_result, stat_errno, (intmax_t)info.st_size,
               SD_DIAG_PATTERN_BYTES);
    close_diag_fd(0);
    *fd = open(ensure_workspace()->path_a, O_RDONLY);
    diag_check(*fd >= 0 && fstat(*fd, &info) == 0 && info.st_size == SD_DIAG_PATTERN_BYTES,
               "POSIX final size mismatch");
    diag_check(read(*fd, ensure_workspace()->rx_buf, SD_DIAG_PATTERN_BYTES) == SD_DIAG_PATTERN_BYTES,
               "POSIX complete readback failed");
    verify_bytes(ensure_workspace()->rx_buf, ensure_workspace()->tx_buf, 0, SD_DIAG_PATTERN_BYTES);
    close_diag_fd(0);
}

/** Populate and scan a directory browser workload with regular and LFN entries. */
static void case_posix_dirs(const void* context) {
    (void)context;
    begin_case();
    sd_diag_workspace_t* const state = ensure_workspace();
    memset(state->browser_seen, 0, sizeof(state->browser_seen));
    memset(state->browser_dirs_seen, 0, sizeof(state->browser_dirs_seen));
    make_vfs_path(ensure_workspace()->path_a, "/_diag/browser");
    diag_check(mkdir(ensure_workspace()->path_a, 0777) == 0, "browser directory create failed");
    for (size_t index = 0; index < SD_DIAG_BROWSER_FILE_COUNT; ++index) {
        char name[SD_DIAG_CHILD_NAME_MAX];
        snprintf(name, sizeof(name), "Long File %02zu.txt", index);
        append_path(ensure_workspace()->path_b, ensure_workspace()->path_a, name);
        int* const fd = &ensure_workspace()->open_fds[0];
        *fd = open(ensure_workspace()->path_b, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        diag_check(*fd >= 0, "browser file open failed");
        const uint8_t byte = (uint8_t)index;
        diag_check(write(*fd, &byte, sizeof(byte)) == sizeof(byte), "browser file write failed");
        close_diag_fd(0);
    }
    for (size_t index = 0; index < SD_DIAG_BROWSER_DIR_COUNT; ++index) {
        char name[SD_DIAG_CHILD_NAME_MAX];
        snprintf(name, sizeof(name), "Subdirectory %02zu", index);
        append_path(ensure_workspace()->path_b, ensure_workspace()->path_a, name);
        diag_check(mkdir(ensure_workspace()->path_b, 0777) == 0, "browser subdirectory create failed");
    }
    DIR* const directory = opendir(ensure_workspace()->path_a);
    diag_check(directory != NULL, "browser opendir failed");
    ensure_workspace()->open_dirs[0] = directory;
    size_t regular = 0;
    size_t dirs = 0;
    const struct dirent* entry;
    while ((entry = read_diag_dir(directory)) != NULL) {
        const char* const entry_name = entry->d_name;
        if (strcmp(entry_name, ".") == 0 || strcmp(entry_name, "..") == 0) {
            continue;
        }
        append_path(ensure_workspace()->path_b, ensure_workspace()->path_a, entry_name);
        struct stat info;
        diag_check(stat(ensure_workspace()->path_b, &info) == 0, "browser stat failed");
        bool found = false;
        if (S_ISREG(info.st_mode)) {
            diag_check(entry->d_type == DT_UNKNOWN || entry->d_type == DT_REG, "browser regular entry type mismatch");
            for (size_t index = 0; index < SD_DIAG_BROWSER_FILE_COUNT; ++index) {
                char expected[SD_DIAG_CHILD_NAME_MAX];
                snprintf(expected, sizeof(expected), "Long File %02zu.txt", index);
                if (strcmp(entry_name, expected) != 0) {
                    continue;
                }
                diag_check(!state->browser_seen[index] && info.st_size == 1, "browser duplicate or size mismatch: %s",
                           entry_name);
                state->browser_seen[index] = true;
                state->open_fds[0] = open(state->path_b, O_RDONLY);
                uint8_t byte;
                diag_check(state->open_fds[0] >= 0 && read(state->open_fds[0], &byte, 1) == 1 && byte == index,
                           "browser file payload mismatch: %s", entry_name);
                close_diag_fd(0);
                found = true;
                ++regular;
                break;
            }
        } else if (S_ISDIR(info.st_mode)) {
            diag_check(entry->d_type == DT_UNKNOWN || entry->d_type == DT_DIR, "browser directory entry type mismatch");
            for (size_t index = 0; index < SD_DIAG_BROWSER_DIR_COUNT; ++index) {
                char expected[SD_DIAG_CHILD_NAME_MAX];
                snprintf(expected, sizeof(expected), "Subdirectory %02zu", index);
                if (strcmp(entry_name, expected) != 0) {
                    continue;
                }
                diag_check(!state->browser_dirs_seen[index], "browser duplicate directory");
                state->browser_dirs_seen[index] = true;
                found = true;
                ++dirs;
                break;
            }
        }
        diag_check(found, "browser unexpected entry: %s", entry_name);
    }
    diag_check(errno == 0, "browser readdir failed");
    diag_check(regular == SD_DIAG_BROWSER_FILE_COUNT && dirs == SD_DIAG_BROWSER_DIR_COUNT,
               "browser entry count mismatch");
    ensure_workspace()->open_dirs[0] = NULL;
    diag_check(closedir(directory) == 0, "browser closedir failed");
    append_path(ensure_workspace()->path_b, ensure_workspace()->path_a, "Long File 00.txt");
    append_path(ensure_workspace()->path_c, ensure_workspace()->path_a, "Renamed File.txt");
    diag_check(rename(ensure_workspace()->path_b, ensure_workspace()->path_c) == 0, "browser rename failed");
    diag_check(unlink(ensure_workspace()->path_c) == 0, "browser unlink failed");
    for (size_t index = 1; index < SD_DIAG_BROWSER_FILE_COUNT; ++index) {
        char name[SD_DIAG_CHILD_NAME_MAX];
        snprintf(name, sizeof(name), "Long File %02zu.txt", index);
        append_path(ensure_workspace()->path_b, ensure_workspace()->path_a, name);
        diag_check(unlink(ensure_workspace()->path_b) == 0, "browser file cleanup failed");
    }
    for (size_t index = 0; index < SD_DIAG_BROWSER_DIR_COUNT; ++index) {
        char name[SD_DIAG_CHILD_NAME_MAX];
        snprintf(name, sizeof(name), "Subdirectory %02zu", index);
        append_path(ensure_workspace()->path_b, ensure_workspace()->path_a, name);
        diag_check(rmdir(ensure_workspace()->path_b) == 0, "browser directory cleanup failed");
    }
    diag_check(rmdir(ensure_workspace()->path_a) == 0, "browser root cleanup failed");
}

/** Keep two file descriptors and a directory stream active independently. */
static void case_concurrent(const void* context) {
    (void)context;
    begin_case();
    make_vfs_path(ensure_workspace()->path_a, "/_diag/concurrent-a.bin");
    make_vfs_path(ensure_workspace()->path_b, "/_diag/concurrent-b.bin");
    make_vfs_path(ensure_workspace()->path_c, "/_diag");
    ensure_workspace()->open_fds[0] = open(ensure_workspace()->path_a, O_RDWR | O_CREAT | O_TRUNC, 0666);
    ensure_workspace()->open_fds[1] = open(ensure_workspace()->path_b, O_RDWR | O_CREAT | O_TRUNC, 0666);
    ensure_workspace()->open_dirs[0] = opendir(ensure_workspace()->path_c);
    diag_check(ensure_workspace()->open_fds[0] >= 0 && ensure_workspace()->open_fds[1] >= 0 &&
                   ensure_workspace()->open_dirs[0] != NULL,
               "concurrent open failed");
    diag_check(write(ensure_workspace()->open_fds[0], "alpha", 5) == 5 &&
                   write(ensure_workspace()->open_fds[1], "bravo", 5) == 5,
               "concurrent write failed");
    const struct dirent* const entry = read_diag_dir(ensure_workspace()->open_dirs[0]);
    diag_check(entry != NULL && entry->d_name[0] != '\0', "concurrent directory cursor failed");
    diag_check(lseek(ensure_workspace()->open_fds[0], 0, SEEK_SET) == 0 &&
                   lseek(ensure_workspace()->open_fds[1], 0, SEEK_SET) == 0,
               "concurrent seek failed");
    char a[6], b[6];
    diag_check(read(ensure_workspace()->open_fds[0], a, 5) == 5 && read(ensure_workspace()->open_fds[1], b, 5) == 5 &&
                   memcmp(a, "alpha", 5) == 0 && memcmp(b, "bravo", 5) == 0,
               "concurrent file cursor mismatch");
    diag_check(close(ensure_workspace()->open_fds[0]) == 0 && close(ensure_workspace()->open_fds[1]) == 0,
               "concurrent close failed");
    ensure_workspace()->open_fds[0] = -1;
    ensure_workspace()->open_fds[1] = -1;
    diag_check(closedir(ensure_workspace()->open_dirs[0]) == 0, "concurrent dir close failed");
    ensure_workspace()->open_dirs[0] = NULL;
}

/** Write bounded streaming fixtures without retaining whole files in RAM. */
static void write_file_pattern(FILE* file, size_t bytes, uint64_t key) {
    sd_diag_workspace_t* const state = ensure_workspace();
    for (size_t offset = 0; offset < bytes;) {
        const size_t length = MIN(bytes - offset, sizeof(state->tx_buf));
        fill_sectors(state->tx_buf, key, offset / SD_DIAG_SECTOR_SIZE,
                     (length + SD_DIAG_SECTOR_SIZE - 1) / SD_DIAG_SECTOR_SIZE);
        diag_check(fwrite(state->tx_buf, 1, length, file) == length, "fixture write at %zu failed: errno %d", offset,
                   errno);
        offset += length;
        diag_refresh();
    }
}

/** Compare a range with independently generated file bytes without failing in a callback. */
static bool matches_file_pattern(const uint8_t* bytes, size_t offset, size_t length, uint64_t key) {
    sd_diag_workspace_t* const state = ensure_workspace();
    for (size_t consumed = 0; consumed < length;) {
        const size_t position = offset + consumed;
        const size_t within = position % SD_DIAG_SECTOR_SIZE;
        const size_t chunk = MIN(length - consumed, SD_DIAG_SECTOR_SIZE - within);
        fill_sector(state->tx_buf, key, position / SD_DIAG_SECTOR_SIZE);
        if (memcmp(bytes + consumed, state->tx_buf + within, chunk) != 0) {
            return false;
        }
        consumed += chunk;
    }
    return true;
}

/** Exercise independent default-buffered readers as used by configuration aliases. */
static void case_text(const void* context) {
    (void)context;
    begin_case();
    make_vfs_path(ensure_workspace()->path_a, "/_diag/config.yaml");
    FILE* file = fopen(ensure_workspace()->path_a, "w");
    diag_check(file != NULL, "text fopen failed");
    ensure_workspace()->open_files[0] = file;
    const char* const expected[] = {
        "format-version: 1\n",
        "configs:\n",
        "  - name: Diagnostic\n",
        "    setup: []\n",
    };
    for (size_t index = 0; index < count_of(expected); ++index) {
        diag_check(fwrite(expected[index], 1, strlen(expected[index]), file) == strlen(expected[index]),
                   "text fixture write failed");
    }
    close_diag_stream(0);

    // Opening the same read-only file must not share the parent's cursor or EOF state.
    for (size_t reader = 0; reader < 2; ++reader) {
        ensure_workspace()->open_files[reader] = fopen(ensure_workspace()->path_a, "r");
        diag_check(ensure_workspace()->open_files[reader] != NULL, "text reader open failed");
    }
    char line[64];
    FILE* const parent = ensure_workspace()->open_files[0];
    FILE* const alias = ensure_workspace()->open_files[1];
    diag_check(fgets(line, sizeof(line), parent) != NULL && strcmp(line, expected[0]) == 0,
               "text parent header mismatch");
    diag_check(fgetc(alias) == expected[0][0] && ftell(alias) == 1 && ftell(parent) == (long)strlen(expected[0]),
               "text reader cursors collided");
    diag_check(fseek(alias, 0, SEEK_SET) == 0, "text alias seek failed");
    for (size_t reader = 0; reader < 2; ++reader) {
        file = ensure_workspace()->open_files[reader];
        for (size_t index = reader == 0 ? 1u : 0u; index < count_of(expected); ++index) {
            diag_check(fgets(line, sizeof(line), file) != NULL && strcmp(line, expected[index]) == 0,
                       "text reader %zu line %zu mismatch", reader, index);
        }
        diag_check(fgetc(file) == EOF && feof(file) != 0 && ferror(file) == 0, "text reader %zu EOF mismatch", reader);
        clearerr(file);
        diag_check(!feof(file) && !ferror(file), "text reader error reset failed");
        close_diag_stream(reader);
    }
}

/** Capture and verify each callback chunk emitted by sd_read_file(). */
static void capture_sd_read(size_t offset, uint8_t* buffer, size_t bytes_read, void* context) {
    sd_read_capture_t* const capture = context;
    capture->valid &= capture->bytes == offset;
    // Defer failure until sd_read_file releases its stream and shared buffer.
    const size_t remaining = offset <= capture->expected_bytes ? capture->expected_bytes - offset : 0;
    capture->valid &= bytes_read == MIN(remaining, TEMP_BUFFER_SIZE);
    capture->valid &= matches_file_pattern(buffer, offset, bytes_read, ensure_workspace()->cycle_counter);
    capture->bytes += bytes_read;
    capture->calls++;
}

/** Exercise empty files, exact loader chunks, short tails, EOF, and bounded reads. */
static void case_chunked_read(const void* context) {
    (void)context;
    begin_case();
    make_vfs_path(ensure_workspace()->path_a, "/_diag/rom64k.bin");
    static const size_t lengths[] = {
        0, TEMP_BUFFER_SIZE, TEMP_BUFFER_SIZE + 1, 2u * TEMP_BUFFER_SIZE - 1, SD_DIAG_ROM_FILE_BYTES,
    };
    for (size_t index = 0; index < count_of(lengths); ++index) {
        FILE* const file = fopen(ensure_workspace()->path_a, "wb");
        diag_check(file != NULL, "chunked fopen failed");
        ensure_workspace()->open_files[0] = file;
        write_file_pattern(file, lengths[index], ensure_workspace()->cycle_counter);
        close_diag_stream(0);
        sd_read_capture_t capture = {.valid = true, .expected_bytes = lengths[index]};
        sd_read_file(ensure_workspace()->path_a, capture_sd_read, &capture, lengths[index] + TEMP_BUFFER_SIZE);
        diag_check(capture.valid && capture.bytes == lengths[index] &&
                       capture.calls == (lengths[index] + TEMP_BUFFER_SIZE - 1) / TEMP_BUFFER_SIZE,
                   "sd_read_file size %zu mismatch: bytes %zu chunks %zu", lengths[index], capture.bytes,
                   capture.calls);
    }
    sd_read_capture_t capture = {.valid = true, .expected_bytes = 5000};
    sd_read_file(ensure_workspace()->path_a, capture_sd_read, &capture, 5000);
    diag_check(capture.valid && capture.bytes == 5000, "sd_read_file truncation mismatch");
}

/** Exercise only default and image buffering, with seeks across actual FAT clusters. */
static void case_buffered(const void* context) {
    (void)context;
    begin_case();
    make_vfs_path(ensure_workspace()->path_a, "/_diag/buffer.bin");
    FATFS* volume;
    DWORD free_clusters;
    diag_check(f_getfree("", &free_clusters, &volume) == FR_OK, "stream geometry query failed");
    const size_t cluster = volume->csize * SD_DIAG_SECTOR_SIZE;
    const size_t bytes = 3u * cluster + 3u;
    const size_t positions[] = {0, cluster - 1, 2u * cluster - 137u, bytes - 1};
    for (size_t mode = 0; mode < 2; ++mode) {
        FILE* file = fopen(ensure_workspace()->path_a, "wb");
        diag_check(file != NULL, "buffer fopen failed");
        ensure_workspace()->open_files[0] = file;
        write_file_pattern(file, bytes, ensure_workspace()->cycle_counter);
        close_diag_stream(0);
        file = fopen(ensure_workspace()->path_a, "r+b");
        diag_check(file != NULL, "buffer reopen failed");
        ensure_workspace()->open_files[0] = file;
        if (mode != 0) {
            diag_check(setvbuf(file, NULL, _IONBF, 0) == 0, "image buffering selection failed");
        }
        diag_check(fseek(file, 0, SEEK_END) == 0 && ftell(file) == (long)bytes, "stream EOF size mismatch");
        diag_check(fgetc(file) == EOF && feof(file) && !ferror(file), "stream EOF state mismatch");
        for (size_t index = 0; index < count_of(positions); ++index) {
            const size_t length = MIN(SD_DIAG_SECTOR_SIZE + 1, bytes - positions[index]);
            clearerr(file);
            diag_check(!feof(file) && !ferror(file), "stream clearerr failed");
            diag_check(fseek(file, (long)positions[index], SEEK_SET) == 0 && ftell(file) == (long)positions[index] &&
                           fread(ensure_workspace()->rx_buf, 1, length, file) == length,
                       "stream seek/read at %zu failed", positions[index]);
            diag_check(matches_file_pattern(ensure_workspace()->rx_buf, positions[index], length,
                                            ensure_workspace()->cycle_counter),
                       "stream mode %zu data at %zu mismatch", mode, positions[index]);
        }
        close_diag_stream(0);
    }
}

/** Select compatible native-size pairs, plus a same-format replacement candidate. */
static size_t image_format(size_t image) {
    return image == SD_DIAG_IMAGE_OWNERS ? 0u : (image / DOS_MEDIA_SLOTS_PER_UNIT) % count_of(image_sizes);
}

/** Return the exact size of a supported raw container, not a filesystem inside it. */
static size_t image_bytes(size_t image) { return image_sizes[image_format(image)]; }

/** Name each distinct stream fixture using its supported container extension. */
static void image_path(char path[SD_PATH_MAX], size_t image) {
    static const char* const extensions[] = {"d64", "d80", "d82"};
    const int written = snprintf(path, SD_PATH_MAX, "/disk/diag_%02zu.%s", image, extensions[image_format(image)]);
    diag_check(written > 0 && written < SD_PATH_MAX, "image fixture path overflow");
}

/** Tag initial and updated data independently of any observed readback. */
static uint64_t image_key(size_t tag) { return ensure_workspace()->cycle_counter * SD_DIAG_IMAGE_TAGS + tag + 1u; }

/** Choose first/last, cluster-adjacent, and scattered native-sector offsets. */
static size_t image_update_sector(size_t image, size_t update, size_t cluster) {
    const size_t sectors = image_bytes(image) / DISKIMAGE_SECTOR_SIZE;
    switch ((sd_image_position_t)((update / SD_DIAG_IMAGE_OWNERS) % SD_IMAGE_POSITION_COUNT)) {
    case SD_IMAGE_FIRST:
        return 0;
    case SD_IMAGE_LAST:
        return sectors - 1;
    case SD_IMAGE_BEFORE_CLUSTER:
        return MIN(cluster / DISKIMAGE_SECTOR_SIZE - 1, sectors - 1);
    case SD_IMAGE_AFTER_CLUSTER:
        return MIN(cluster / DISKIMAGE_SECTOR_SIZE, sectors - 1);
    default:
        return (size_t)((update * UINT64_C(1103515245) + 18u) % sectors);
    }
}

/** Reconstruct a native sector's last successful update without using stored bytes. */
static void expected_image_sector(size_t image, size_t sector, size_t updates, size_t cluster) {
    sd_diag_workspace_t* const state = ensure_workspace();
    fill_sector(state->tx_buf, image_key(image), sector / 2u);
    if (sector % 2u != 0) {
        memmove(state->tx_buf, state->tx_buf + DISKIMAGE_SECTOR_SIZE, DISKIMAGE_SECTOR_SIZE);
    }
    for (size_t update = 0; update < updates; ++update) {
        if (update % SD_DIAG_IMAGE_OWNERS == image && image_update_sector(image, update, cluster) == sector) {
            fill_sector(state->tx_buf, image_key(SD_DIAG_IMAGE_FILES + update), sector);
        }
    }
}

/** Check size metadata through a separate path lookup while a stream remains live. */
static void check_image_size(FILE* file, size_t image) {
    sd_diag_workspace_t* const state = ensure_workspace();
    image_path(state->path_a, image);
    struct stat info = {0};
    diag_check(fseek(file, 0, SEEK_END) == 0 && ftell(file) == (long)image_bytes(image) &&
                   stat(state->path_a, &info) == 0 && info.st_size == (off_t)image_bytes(image),
               "image %zu live size mismatch: errno %d", image, errno);
}

/** Verify every image byte after clean transport/root reconstruction. */
static void verify_image(FILE* file, size_t image, size_t cluster, size_t* verified, size_t total) {
    sd_diag_workspace_t* const state = ensure_workspace();
    check_image_size(file, image);
    diag_check(fseek(file, 0, SEEK_SET) == 0, "image %zu verification seek failed", image);
    for (size_t offset = 0; offset < image_bytes(image); offset += SD_DIAG_SECTOR_SIZE) {
        const size_t length = MIN(SD_DIAG_SECTOR_SIZE, image_bytes(image) - offset);
        diag_check(fread(state->rx_buf, 1, length, file) == length, "image %zu remount read at %zu failed: errno %d",
                   image, offset, errno);
        for (size_t within = 0; within < length; within += DISKIMAGE_SECTOR_SIZE) {
            expected_image_sector(image, (offset + within) / DISKIMAGE_SECTOR_SIZE, SD_DIAG_RANDOM_IO_COUNT, cluster);
            memcpy(state->guard_after + within, state->tx_buf, DISKIMAGE_SECTOR_SIZE);
        }
        diag_check(memcmp(state->rx_buf, state->guard_after, length) == 0, "image %zu remount bytes at %zu mismatch",
                   image, offset);
        *verified += length;
        if (offset % sizeof(state->tx_buf) == 0 || *verified == total) {
            diag_case_progress(total + *verified, 2u * total);
        } else {
            diag_refresh();
        }
    }
    diag_check(fgetc(file) == EOF && feof(file) && !ferror(file), "image %zu verification EOF mismatch", image);
}

/** Exercise production-shaped stdio streams without invoking the product I/O stress code. */
static void case_images(const void* context) {
    (void)context;
    begin_case();
    sd_diag_workspace_t* const state = ensure_workspace();
    FATFS* volume;
    DWORD free_clusters;
    diag_check(f_getfree("", &free_clusters, &volume) == FR_OK, "image cluster query failed");
    const size_t cluster = volume->csize * SD_DIAG_SECTOR_SIZE;
    size_t total = 0;

    // Create fixed-size tagged containers, not native DOS-format fixtures.
    for (size_t image = 0; image < SD_DIAG_IMAGE_FILES; ++image) {
        total += image_bytes(image);
        image_path(state->path_a, image);
        state->open_files[0] = fopen(state->path_a, "wb");
        diag_check(state->open_files[0] != NULL, "image %zu create failed", image);
        write_file_pattern(state->open_files[0], image_bytes(image), image_key(image));
        close_diag_stream(0);
        diag_case_progress(image + 1u, 2u * SD_DIAG_IMAGE_FILES);
    }
    for (size_t image = 0; image < SD_DIAG_IMAGE_OWNERS; ++image) {
        image_path(state->path_a, image);
        state->open_files[image] = fopen(state->path_a, "r+b");
        diag_check(state->open_files[image] != NULL && setvbuf(state->open_files[image], NULL, _IONBF, 0) == 0,
                   "image %zu production stream open failed", image);
        check_image_size(state->open_files[image], image);
    }

    // Validate the previous bytes before each update, with independent cursors and flushes.
    for (size_t update = 0; update < SD_DIAG_RANDOM_IO_COUNT; ++update) {
        const size_t image = update % SD_DIAG_IMAGE_OWNERS;
        const size_t sector = image_update_sector(image, update, cluster);
        FILE* const file = state->open_files[image];
        clearerr(file);
        diag_check(fseek(file, (long)(sector * DISKIMAGE_SECTOR_SIZE), SEEK_SET) == 0 &&
                       fread(state->rx_buf, 1, DISKIMAGE_SECTOR_SIZE, file) == DISKIMAGE_SECTOR_SIZE,
                   "image %zu sector %zu read failed: errno %d", image, sector, errno);
        expected_image_sector(image, sector, update, cluster);
        diag_check(memcmp(state->rx_buf, state->tx_buf, DISKIMAGE_SECTOR_SIZE) == 0,
                   "image %zu sector %zu previous bytes mismatch", image, sector);
        fill_sector(state->tx_buf, image_key(SD_DIAG_IMAGE_FILES + update), sector);
        diag_check(fseek(file, (long)(sector * DISKIMAGE_SECTOR_SIZE), SEEK_SET) == 0 &&
                       fwrite(state->tx_buf, 1, DISKIMAGE_SECTOR_SIZE, file) == DISKIMAGE_SECTOR_SIZE &&
                       fflush(file) == 0,
                   "image %zu sector %zu write/flush failed: errno %d", image, sector, errno);
        check_image_size(file, image);
        diag_refresh();
    }

    // A browser directory may overlap eight image owners, but not mutate their names.
    state->open_dirs[0] = opendir("/disk");
    diag_check(state->open_dirs[0] != NULL, "image directory open failed");
    bool seen[SD_DIAG_IMAGE_FILES] = {0};
    size_t entries = 0;
    const struct dirent* entry;
    while ((entry = read_diag_dir(state->open_dirs[0])) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        bool found = false;
        for (size_t image = 0; image < SD_DIAG_IMAGE_FILES; ++image) {
            image_path(state->path_a, image);
            if (strcmp(entry->d_name, strrchr(state->path_a, '/') + 1) != 0) {
                continue;
            }
            struct stat info = {0};
            diag_check(!seen[image] && stat(state->path_a, &info) == 0 && S_ISREG(info.st_mode) &&
                           info.st_size == (off_t)image_bytes(image) &&
                           (entry->d_type == DT_UNKNOWN || entry->d_type == DT_REG),
                       "image directory identity/type/size mismatch");
            seen[image] = found = true;
            entries++;
            break;
        }
        diag_check(found, "unexpected image directory entry: %s", entry->d_name);
    }
    diag_check(errno == 0 && entries == SD_DIAG_IMAGE_FILES, "image directory EOF/count mismatch");
    DIR* const directory = state->open_dirs[0];
    state->open_dirs[0] = NULL;
    diag_check(closedir(directory) == 0, "image directory close failed");

    // Expected missing candidates must not consume the ninth available owner.
    for (size_t attempt = 0; attempt < SD_DIAG_OPEN_ATTEMPTS; ++attempt) {
        errno = 0;
        state->open_files[SD_DIAG_IMAGE_OWNERS] = fopen("/disk/missing-image.d64", "r+b");
        diag_check(state->open_files[SD_DIAG_IMAGE_OWNERS] == NULL && errno == ENOENT,
                   "missing image candidate: errno %d", errno);
    }
    image_path(state->path_a, SD_DIAG_IMAGE_OWNERS);
    state->open_files[SD_DIAG_IMAGE_OWNERS] = fopen(state->path_a, "r+b");
    diag_check(state->open_files[SD_DIAG_IMAGE_OWNERS] != NULL &&
                   setvbuf(state->open_files[SD_DIAG_IMAGE_OWNERS], NULL, _IONBF, 0) == 0,
               "ninth image candidate open failed");
    check_image_size(state->open_files[SD_DIAG_IMAGE_OWNERS], SD_DIAG_IMAGE_OWNERS);
    FILE* const replacement = state->open_files[SD_DIAG_IMAGE_OWNERS];
    close_diag_stream(0);
    state->open_files[0] = replacement;
    state->open_files[SD_DIAG_IMAGE_OWNERS] = NULL;
    for (size_t image = 0; image < SD_DIAG_IMAGE_OWNERS; ++image) {
        close_diag_stream(image);
    }

    // Preserve media contents through complete root and transport retirement, without formatting.
    diag_check(sd_unmount_root(), "image persistence root unmount failed");
    blockdevice_t* const device = sd_blockdevice();
    diag_check(device->deinit(device) == 0 && device->init(device) == 0,
               "image persistence transport reconstruction failed");
    diag_check(sd_mount_root(), "image persistence root remount failed");
    size_t verified = 0;
    for (size_t image = 0; image < SD_DIAG_IMAGE_FILES; ++image) {
        image_path(state->path_a, image);
        state->open_files[0] = fopen(state->path_a, "rb");
        diag_check(state->open_files[0] != NULL, "image %zu verification open failed", image);
        verify_image(state->open_files[0], image, cluster, &verified, total);
        close_diag_stream(0);
        image_path(state->path_a, image);
        diag_check(remove(state->path_a) == 0, "image %zu cleanup failed", image);
    }
    diag_check(sd_unmount_root(), "VFS final unmount failed");
}

bool diag_sd_prepare_swap(void) {
    // Stop new guest requests before draining and flushing the existing owners.
    set_cpu(CPU_HALT);
    pet_wait_for_storage_idle();
    if (!menu_browser_abandon_scan() || !ieee_drive_unmount_all() || !sd_unmount_root()) {
        return false;
    }
    blockdevice_t* const device = sd_blockdevice();
    return device != NULL && device->deinit(device) == BD_ERROR_OK;
}

/** Allocate workspace after the replacement card has survived the countdown. */
static void sd_prepare(void) { ensure_workspace(); }

static const diag_case_t block_init_cases[] = {
    {"Init & Geometry", case_init_geometry},
    {"Re-init Idempotency", case_reinit},
    {"Production SPI Configuration", case_spi_configuration},
};
static const diag_case_t block_io_cases[] = {
    {"Single Block Patterns", case_single_patterns},
    {"Neighbor Isolation", case_neighbors},
    {"Multi-Block Cross-Check", case_multiblock_cross},
    {"Multi-Block 8 & 64 Sectors", case_multiblock_sizes},
    {"Trim & Re-Program", case_trim},
    {"Sequential 2 MiB Sweep", case_sweep},
};
static const diag_case_t fat_cases[] = {
    {"Format & Remount", case_fat_remount},        {"Free Space (sd_free_bytes)", case_free_space},
    {"Direct FAT File Ops", case_fat_file},        {"Read-Only Open Fallback", case_fat_readonly},
    {"Directories, Rename & Stat", case_fat_dirs}, {"Case-Insensitive Resolve", case_resolve},
};
static const diag_case_t vfs_cases[] = {
    {"VFS Mount & Reformat", case_vfs_mount},
    {"POSIX File Descriptors", case_posix_files},
    {"POSIX Directory Browser", case_posix_dirs},
    {"Concurrent Handles", case_concurrent},
};
static const diag_case_t stdio_cases[] = {
    {"Independent Config Streams", case_text},
    {"Chunked Binary (sd_read_file)", case_chunked_read},
    {"Production Buffering & Seek", case_buffered},
    {"Disk Image Stream I/O", case_images},
};

static const diag_suite_t sd_suites[] = {
    {"Block Device (Init & Modes)", block_init_cases, count_of(block_init_cases), NULL},
    {"Block Device (Sector I/O)", block_io_cases, count_of(block_io_cases), NULL},
    {"FAT Filesystem (FatFs)", fat_cases, count_of(fat_cases), NULL},
    {"VFS & POSIX I/O", vfs_cases, count_of(vfs_cases), NULL},
    {"Standard I/O & Workloads", stdio_cases, count_of(stdio_cases), NULL},
};

const diag_plan_t diag_plan_sd = {
    .name = "sd",
    .description = "Destructive full-stack SD-card diagnostic",
    .confirm = diag_sd_confirm,
    .prepare = sd_prepare,
    .suites = sd_suites,
    .suite_count = count_of(sd_suites),
};
