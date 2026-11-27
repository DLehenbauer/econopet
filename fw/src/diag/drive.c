// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "diag/drive.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diag/diag.h"
#include "diag/target.h"
#include "drive-test/drive_test_catalog.h"
#include "driver.h"
#include "fatal.h"
#include "ieee/diskimage_file.h"
#include "ieee/ieee_drive.h"
#include "menu/menu.h"
#include "menu/menu_browser.h"
#include "pet.h"
#include "sd/sd.h"

/** Standard 32K 80-column PET ROM 4 configuration ID required by drive-test.prg. */
#define DRIVE_DIAG_CONFIG_ID "pet-80xx-60hz"

/** Staged SD-card path of the 6502 IEEE-488 drive qualification program. */
#define DRIVE_DIAG_PRG_PATH "/test/drive-test.prg"

/** Root SD-card directory reserved for diagnostic assets and scratch storage. */
#define DRIVE_DIAG_ROOT_DIR "/test"

/** Disposable SD-card tree, purged before and after each drive diagnostic run. */
#define DRIVE_DIAG_TMP_DIR "/test/tmp"

/**
 * Per-case host backstop timeout in microseconds (240 seconds).
 *
 * Exceeds the 6502 runner's ~200.5s FORMAT_BUDGET so real IEEE hangs report
 * DT_ERR_TIMEOUT from the target while 1 MiB 8250 D82 full-disk duplicates
 * (~45-60s over SPI SD) never trip the host backstop prematurely.
 */
#define DRIVE_DIAG_CASE_TIMEOUT_US UINT64_C(240000000)

_Static_assert(DT_TOTAL_CASES == 468u, "drive diagnostic catalog count drifted from 468 checks");

/** Suite indices in execution order, matching run_pass() in prgs/drive-test/drive-test.c. */
typedef enum {
    /** Full N<drive>:<label>,<id> format across all eight slots. */
    DRIVE_SUITE_FORMAT = 0,
    /** Per-slot directory, BAM, SEQ/PRG/USR, command, direct-block, and REL smoke checks. */
    DRIVE_SUITE_SMOKE,
    /** Cross-unit (U8 vs U11) and dual-drive (D0 vs D1) readback isolation checks. */
    DRIVE_SUITE_ISOLATION,
    /** Cross-drive copy and SEQ/PRG concatenation checks on each dual-drive unit. */
    DRIVE_SUITE_CONCAT,
    /** 512-byte multi-sector sequential stream checks on every slot. */
    DRIVE_SUITE_MULTI_SEQ,
    /** Representative per-geometry relative record length, growth, and side-sector checks. */
    DRIVE_SUITE_REL,
    /** Raw U1/U2 and UA/UB sector round-trips including D82's final sector. */
    DRIVE_SUITE_RAW_BLOCK,
    /** Sector-boundary lengths, append/replace, wildcard scratch, B-A/B-F, I/V, quick format, and zones. */
    DRIVE_SUITE_MAINT,
    /** Full-disk DUPLICATE checks in both directions on representative 4040/8050/8250 units. */
    DRIVE_SUITE_DUPLICATE,
    /** Total number of suites in diag_plan_drive. */
    DRIVE_SUITE_COUNT,
} drive_suite_id_t;

/** Ordered suite definitions also supply the counts used for ordinal decoding. */
static const diag_suite_t drive_suites[DRIVE_SUITE_COUNT];

/** Temporary scratch container paths mounted on slots 0..7 during drive qualification. */
static const char* const scratch_paths[DT_SLOT_COUNT] = {
    DRIVE_DIAG_TMP_DIR "/u8d0.d64",  DRIVE_DIAG_TMP_DIR "/u8d1.d64",  DRIVE_DIAG_TMP_DIR "/u9d0.d80",
    DRIVE_DIAG_TMP_DIR "/u9d1.d80",  DRIVE_DIAG_TMP_DIR "/u10d0.d82", DRIVE_DIAG_TMP_DIR "/u10d1.d82",
    DRIVE_DIAG_TMP_DIR "/u11d0.d64", DRIVE_DIAG_TMP_DIR "/u11d1.d64",
};

/** Commodore drive model labels indexed by DT_GEOM_D64/D80/D82. */
static const char* const model_names[] = {"4040", "8050", "8250"};

/** Resolve a 0-based global check ordinal into its catalog case descriptor. */
static void resolve_ordinal(const uint16_t ordinal, diag_drive_case_t* const out) {
    // Subtract preceding suite counts to locate the suite-relative index.
    vet(ordinal < DT_TOTAL_CASES, "invalid drive diagnostic ordinal %u", (unsigned)ordinal);
    size_t local = ordinal;
    for (size_t s = 0; s < DRIVE_SUITE_COUNT; ++s) {
        if (local < drive_suites[s].case_count) {
            diag_drive_resolve_case(s, local, out);
            return;
        }
        local -= drive_suites[s].case_count;
    }
    fatal("drive diagnostic suite counts do not cover ordinal %u", (unsigned)ordinal);
}

/** Verify that the mailbox's self-reported (phase, slot) matches the catalog ordinal. */
static bool drive_verify_snapshot(const diag_target_snapshot_t* const snap, const uint16_t check_ordinal,
                                  char* const out, const size_t out_size) {
    // Reject ordinals past the end of the 468-check catalog.
    if (check_ordinal >= DT_TOTAL_CASES) {
        snprintf(out, out_size, "target check ordinal %u exceeds catalog (%u)", (unsigned)check_ordinal + 1u,
                 (unsigned)DT_TOTAL_CASES);
        return false;
    }

    // Compare the target's published phase and slot against the host's expected descriptor.
    diag_drive_case_t expected;
    resolve_ordinal(check_ordinal, &expected);
    // Cross-copy verifies source retention within the destination's check, so a failure can name either drive.
    const bool source_failure = snap->state == TARGET_DIAG_STATE_FAILED && expected.phase == DT_PHASE_CROSS_COPY &&
                                snap->slot == (expected.slot ^ 1u);
    if (snap->phase != expected.phase || (snap->slot != expected.slot && !source_failure)) {
        snprintf(out, out_size, "mailbox drift at check #%u: expected phase %u slot %u, got phase %u slot %u",
                 (unsigned)check_ordinal + 1u, (unsigned)expected.phase, (unsigned)expected.slot, (unsigned)snap->phase,
                 (unsigned)snap->slot);
        return false;
    }
    return true;
}

/** Map catalog geometry ID to the firmware container type. */
static diskimage_type_t slot_image_type(const uint8_t geom) {
    // Translate the shared CC65/MCU geometry code into diskimage_t's enum.
    switch (geom) {
    case DT_GEOM_D64:
        return diskimage_type_d64;
    case DT_GEOM_D80:
        return diskimage_type_d80;
    case DT_GEOM_D82:
        return diskimage_type_d82;
    default:
        return diskimage_type_none;
    }
}

/** Purge the disposable scratch tree without recursion or retaining directory handles across mutations. */
static bool remove_scratch_files(char* const error_out, const size_t error_size) {
    // FatFs maps a missing ancestor to ENOTDIR, so inspect parents before their children.
    const char* const directories[] = {DRIVE_DIAG_ROOT_DIR, DRIVE_DIAG_TMP_DIR};
    for (size_t index = 0; index < count_of(directories); ++index) {
        struct stat info;
        if (stat(directories[index], &info) != 0) {
            if (errno == ENOENT) {
                return true;
            }
            snprintf(error_out, error_size, "could not inspect '%s' (errno %d)", directories[index], errno);
            return false;
        }
        if (!S_ISDIR(info.st_mode)) {
            snprintf(error_out, error_size, "'%s' is not a directory", directories[index]);
            return false;
        }
    }
    // Reopen after each mutation so traversal needs only one bounded path and one directory handle.
    char path[SD_PATH_MAX] = DRIVE_DIAG_TMP_DIR;
    while (true) {
        DIR* const directory = opendir(path);
        if (directory == NULL) {
            snprintf(error_out, error_size, "could not open '%s' (errno %d)", path, errno);
            return false;
        }
        const struct dirent* entry;
        do {
            errno = 0;
            entry = readdir(directory);
        } while (entry != NULL && (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0));

        // Copy the selected entry before closing its owner, preserving the first error.
        bool ok = true;
        const bool has_child = entry != NULL;
        const bool descend = has_child && entry->d_type == DT_DIR;
        if (has_child) {
            const size_t length = strlen(path);
            const int added = snprintf(path + length, sizeof(path) - length, "/%s", entry->d_name);
            if (added < 0 || (size_t)added >= sizeof(path) - length) {
                snprintf(error_out, error_size, "scratch path exceeds %u bytes", (unsigned)sizeof(path) - 1u);
                ok = false;
            }
        } else if (errno != 0) {
            snprintf(error_out, error_size, "could not read '%s' (errno %d)", path, errno);
            ok = false;
        }
        if (closedir(directory) != 0 && ok) {
            snprintf(error_out, error_size, "could not close scratch directory (errno %d)", errno);
            ok = false;
        }
        if (!ok) {
            return false;
        }

        // Descend only into directories reported by FAT, then remove each emptied directory on ascent.
        if (descend) {
            continue;
        }
        if ((has_child ? remove(path) : rmdir(path)) != 0) {
            snprintf(error_out, error_size, "could not remove '%s' (errno %d)", path, errno);
            return false;
        }
        if (!has_child && strcmp(path, DRIVE_DIAG_TMP_DIR) == 0) {
            return true;
        }
        *strrchr(path, '/') = '\0';
    }
}

/** Ensure a directory exists on the mounted SD filesystem. */
static bool ensure_dir(const char* const path) {
    // Create the directory if absent, then validate an existing entry's type.
    if (mkdir(path, 0777) == 0) {
        return true;
    }
    if (errno != EEXIST) {
        return false;
    }
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

/** Format a target failure snapshot into a human-readable diagnostic detail string. */
static void drive_format_failure(const diag_target_snapshot_t* const snap, char* const out, const size_t out_size) {
    // Resolve the human-readable failure category from the shared X-macro table.
    const char* const label =
        snap->detail_id < DT_ERR_COUNT ? dt_detail_hdmi_names[snap->detail_id] : "unexpected failure";

    // Handshake timeouts report the assembly wait stage in actual and the raw VIA port B byte in aux1.
    if (snap->detail_id == DT_ERR_TIMEOUT) {
        snprintf(out, out_size, "%s (stage %u, VIA $%02X)", label, (unsigned)snap->actual, (unsigned)snap->aux1);
        return;
    }

    // Include physical track/sector coordinates when aux0 carries a non-zero 1-based track.
    if (snap->aux0 != 0) {
        snprintf(out, out_size, "%s at +%u (T%u/S%u): expected $%02X, got $%02X", label, (unsigned)snap->offset,
                 (unsigned)snap->aux0, (unsigned)snap->aux1, (unsigned)snap->expected, (unsigned)snap->actual);
        return;
    }

    // Format ordinary stream or status mismatches with byte offset and hex expected/actual bytes.
    snprintf(out, out_size, "%s at +%u: expected $%02X, got $%02X", label, (unsigned)snap->offset,
             (unsigned)snap->expected, (unsigned)snap->actual);
}

/** Target runner policy binding diag_plan_drive to ieee_drive_task() and catalog drift checks. */
static const diag_target_runner_t drive_runner = {
    .total_cases = DT_TOTAL_CASES,
    .case_timeout_us = DRIVE_DIAG_CASE_TIMEOUT_US,
    .service = ieee_drive_task,
    .verify_snapshot = drive_verify_snapshot,
    .format_failure = drive_format_failure,
};

/**
 * Prepare scratch images, mount all eight slots, and stage /test/drive-test.prg in SRAM.
 */
static bool drive_prepare(char* const error_out, const size_t error_size) {
    // Drain any prior guest disk activity and release mounted user media first.
    pet_wait_for_storage_idle();
    set_cpu(CPU_HALT);
    if (!menu_browser_abandon_scan()) {
        snprintf(error_out, error_size, "could not close the active disk scan");
        return false;
    }
    if (!ieee_drive_unmount_all()) {
        snprintf(error_out, error_size, "could not unmount existing IEEE disks");
        return false;
    }

    // Purge all stale scratch contents before loading the new profile.
    if (!remove_scratch_files(error_out, error_size)) {
        return false;
    }

    // Required ROM errors are fatal, so load the profile before allocating scratch resources.
    if (!menu_apply_diag_config(DRIVE_DIAG_CONFIG_ID)) {
        snprintf(error_out, error_size, "missing config '%s' in /config.yaml", DRIVE_DIAG_CONFIG_ID);
        return false;
    }

    // Create fresh scratch containers only after all profile actions have completed.
    if (!ensure_dir(DRIVE_DIAG_ROOT_DIR)) {
        snprintf(error_out, error_size, "could not create '%s' (errno %d)", DRIVE_DIAG_ROOT_DIR, errno);
        return false;
    }
    if (!ensure_dir(DRIVE_DIAG_TMP_DIR)) {
        snprintf(error_out, error_size, "could not create '%s' (errno %d)", DRIVE_DIAG_TMP_DIR, errno);
        return false;
    }
    for (unsigned slot = 0; slot < DT_SLOT_COUNT; ++slot) {
        if (!diskimage_create_file(scratch_paths[slot], slot_image_type(dt_slots[slot].geom))) {
            snprintf(error_out, error_size, "failed to create '%s'", scratch_paths[slot]);
            return false;
        }
    }

    // Mount all eight scratch slots before resetting the PET so IEEE pins park cleanly.
    for (unsigned slot = 0; slot < DT_SLOT_COUNT; ++slot) {
        const dos_media_result_t mounted = ieee_drive_mount_scratch(slot, scratch_paths[slot]);
        if (mounted != DOS_MEDIA_OK) {
            snprintf(error_out, error_size, "failed to mount '%s' on slot %u (err %u)", scratch_paths[slot], slot,
                     (unsigned)mounted);
            return false;
        }
    }

    // Boot the 8032 profile to BASIC READY, load /test/drive-test.prg, and arm RUN.
    if (!diag_target_prepare_pet_prg(DRIVE_DIAG_PRG_PATH, error_out, error_size)) {
        return false;
    }

    // Verify that resetting and booting the target left all eight scratch mounts untouched.
    for (unsigned slot = 0; slot < DT_SLOT_COUNT; ++slot) {
        vet(strcmp(ieee_drive_filename(slot), scratch_paths[slot]) == 0, "diagnostic setup displaced scratch slot %u",
            slot);
    }
    return true;
}

/** Halt the target, then unmount and delete scratch images only after confirmed detachment. */
static bool drive_cleanup(char* const error_out, const size_t error_size) {
    // Stop the 6502 before asking the IEEE facade to detach its mounted owners.
    set_cpu(CPU_HALT);
    if (!ieee_drive_unmount_all()) {
        snprintf(error_out, error_size, "could not unmount diagnostic IEEE disks; scratch files were retained");
        return false;
    }
    return remove_scratch_files(error_out, error_size);
}

/** Decode a (suite_index, case_index) pair into *out matching run_pass() in drive-test.c. */
void diag_drive_resolve_case(const size_t suite_index, const size_t case_index, diag_drive_case_t* const out) {
    // Validate inputs and zero optional length/track/sector fields before suite-specific decoding.
    vet(suite_index < DRIVE_SUITE_COUNT && out != NULL, "invalid drive diagnostic suite index");
    vet(case_index < drive_suites[suite_index].case_count, "invalid drive diagnostic case index");
    memset(out, 0, sizeof(*out));

    size_t slot = 0;
    uint8_t phase = 0;

    // Mirror the exact nested loop order of run_pass() in prgs/drive-test/drive-test.c.
    switch ((drive_suite_id_t)suite_index) {
    case DRIVE_SUITE_FORMAT:
        // One full format per slot in slot order 0..7.
        slot = case_index;
        phase = DT_PHASE_FULL_FORMAT;
        break;

    case DRIVE_SUITE_SMOKE:
        // Seventeen fixed smoke steps executed sequentially on each slot 0..7.
        slot = case_index / DT_SMOKE_STEPS_PER_SLOT;
        phase = dt_smoke_phases[case_index % DT_SMOKE_STEPS_PER_SLOT];
        break;

    case DRIVE_SUITE_ISOLATION:
        // Four alternating unit-isolation reads (U8/U11) followed by four alternating drive passes per unit.
        if (case_index < DT_UNIT_ISO_COUNT) {
            slot = dt_unit_iso_slots[case_index];
            phase = DT_PHASE_UNIT_ISO;
        } else {
            const size_t rem = case_index - DT_UNIT_ISO_COUNT;
            const size_t unit = rem / DT_DRIVE_ISO_PASSES;
            const size_t pass = rem % DT_DRIVE_ISO_PASSES;
            slot = unit * DT_DRIVES_PER_DEVICE + (pass & 1u);
            phase = DT_PHASE_DRIVE_ISO;
        }
        break;

    case DRIVE_SUITE_CONCAT: {
        // Per unit: D0->D1 copy (target D1), D1->D0 copy (target D0), 7 concat steps on D1, then D0/D1 retention.
        const size_t unit = case_index / DT_CONCAT_STEPS_PER_UNIT;
        const size_t step = case_index % DT_CONCAT_STEPS_PER_UNIT;
        const size_t drive = (step == 1u || step == 9u) ? 0u : 1u;
        slot = unit * DT_DRIVES_PER_DEVICE + drive;
        phase = dt_concat_phases[step];
        break;
    }

    case DRIVE_SUITE_MULTI_SEQ:
        // One 512-byte multi-sector sequential write/read check per slot.
        slot = case_index;
        phase = DT_PHASE_MULTI_SEQ;
        break;

    case DRIVE_SUITE_REL: {
        // Nineteen relative-file checks repeated across representative D64, D80, and D82 slots.
        const size_t rep = case_index / DT_REL_STEPS_PER_REP;
        const size_t step = case_index % DT_REL_STEPS_PER_REP;
        slot = dt_rep_slots[rep];
        phase = dt_rel_phases[step];
        break;
    }

    case DRIVE_SUITE_RAW_BLOCK:
        // One raw-block U1/U2 (plus per-geometry UA/UB and D82 end-of-disk) check per slot.
        slot = case_index;
        phase = DT_PHASE_RAW_BLOCK;
        break;

    case DRIVE_SUITE_MAINT: {
        // Partition the flat index into the D64, D80, or D82 representative slot's variable-length block.
        size_t rep;
        size_t local;
        if (case_index < DT_SUITE_MAINT_D64_COUNT) {
            rep = 0u;
            local = case_index;
        } else if (case_index < DT_SUITE_MAINT_D64_COUNT + DT_SUITE_MAINT_D80_COUNT) {
            rep = 1u;
            local = case_index - DT_SUITE_MAINT_D64_COUNT;
        } else {
            rep = 2u;
            local = case_index - (DT_SUITE_MAINT_D64_COUNT + DT_SUITE_MAINT_D80_COUNT);
        }
        slot = dt_rep_slots[rep];

        // Decode boundary lengths (with append/replace interleaved after 253B), maintenance commands, and zone sweeps.
        if (local < 3u) {
            phase = (uint8_t)(DT_PHASE_SEQ_BOUNDARY + local);
            out->has_length = true;
            out->length = (uint16_t)dt_boundary_lengths[0];
        } else if (local == 3u) {
            phase = DT_PHASE_SEQ_APPEND;
        } else if (local == 4u) {
            phase = DT_PHASE_SEQ_REPLACE;
        } else if (local < 26u) {
            const size_t rem = local - 5u;
            phase = (uint8_t)(DT_PHASE_SEQ_BOUNDARY + (rem % DT_BOUNDARY_TYPE_COUNT));
            out->has_length = true;
            out->length = (uint16_t)dt_boundary_lengths[1u + rem / DT_BOUNDARY_TYPE_COUNT];
        } else if (local < DT_MAINT_FIXED_STEPS) {
            phase = dt_maint_phases[local - 26u];
        } else {
            const drive_geom_t* const geom = &dt_geoms[dt_slots[slot].geom];
            const size_t zrem = local - DT_MAINT_FIXED_STEPS;
            const size_t z = zrem % geom->zone_count;
            phase = zrem < geom->zone_count ? DT_PHASE_GEOM_WRITE : DT_PHASE_GEOM_RET;
            out->track = geom->zones[z][0];
            out->sector = geom->zones[z][1];
        }
        break;
    }

    case DRIVE_SUITE_DUPLICATE: {
        // Seed source, duplicate to target, verify target, and verify source in both directions on units 8..10.
        const size_t dir_idx = case_index / DT_DUP_STEPS_PER_DIR;
        const size_t step = case_index % DT_DUP_STEPS_PER_DIR;
        const size_t unit = dir_idx / DT_DRIVES_PER_DEVICE;
        const size_t direction = dir_idx % DT_DRIVES_PER_DEVICE;
        const size_t source = unit * DT_DRIVES_PER_DEVICE + direction;
        const size_t target = unit * DT_DRIVES_PER_DEVICE + (direction ^ 1u);
        static const uint8_t dup_phases[DT_DUP_STEPS_PER_DIR] = {
            DT_PHASE_DUP_SEED,
            DT_PHASE_DUP_DISK,
            DT_PHASE_DUP_TARGET,
            DT_PHASE_DUP_SOURCE,
        };
        slot = (step == 0u || step == 3u) ? source : target;
        phase = dup_phases[step];
        break;
    }

    default:
        fatal("invalid drive suite %zu", suite_index);
    }

    // Populate the resolved phase and slot attributes in *out.
    out->phase = phase;
    out->slot = (uint8_t)slot;
    out->device = dt_slots[slot].device;
    out->drive = dt_slots[slot].drive;
    out->geom = dt_slots[slot].geom;
}

/** Format one generated drive check's human-readable case name for HDMI/UART output. */
static const char* drive_case_name(const diag_suite_t* const suite, const size_t index, char* const buf,
                                   const size_t size) {
    // Resolve the case's coordinates and select the matching byte-length, track/sector, or plain label format.
    const diag_suite_t* const original = suite->context;
    diag_drive_case_t desc;
    diag_drive_resolve_case((size_t)(original - drive_suites), index, &desc);

    const char* const label = dt_phase_hdmi_names[desc.phase];
    const char* const model = model_names[desc.geom];
    if (desc.has_length) {
        snprintf(buf, size, "%s %uB (U%u/D%u %s)", label, (unsigned)desc.length, (unsigned)desc.device,
                 (unsigned)desc.drive, model);
    } else if (desc.track != 0) {
        snprintf(buf, size, "%s T%u/S%u (U%u/D%u %s)", label, (unsigned)desc.track, (unsigned)desc.sector,
                 (unsigned)desc.device, (unsigned)desc.drive, model);
    } else {
        snprintf(buf, size, "%s (U%u/D%u %s)", label, (unsigned)desc.device, (unsigned)desc.drive, model);
    }
    return buf;
}

/** Wait for the running 6502 target program to complete this suite case's ordinal. */
static void drive_run_case(const diag_suite_t* const suite, const size_t index) {
    // Convert the suite-local index into a global check ordinal and poll the target mailbox.
    const diag_suite_t* const original = suite->context;
    size_t ordinal = index;
    for (const diag_suite_t* preceding = drive_suites; preceding != original; ++preceding) {
        ordinal += preceding->case_count;
    }
    diag_target_await_case(&drive_runner, ordinal);
}

/** Bind each generated suite to its original table entry, preserving identity when callers copy a suite. */
#define DRIVE_SUITE_ENTRY(suite_id, suite_name, suite_count_val)                                                       \
    {                                                                                                                  \
        .name = (suite_name),                                                                                          \
        .cases = NULL,                                                                                                 \
        .case_count = (suite_count_val),                                                                               \
        .context = &drive_suites[suite_id],                                                                            \
        .case_name = drive_case_name,                                                                                  \
        .run_case = drive_run_case,                                                                                    \
    }

/** Nine generated suites covering all 468 virtual IEEE-488 drive checks. */
static const diag_suite_t drive_suites[DRIVE_SUITE_COUNT] = {
    DRIVE_SUITE_ENTRY(DRIVE_SUITE_FORMAT, "Disk Format", DT_SUITE_FORMAT_COUNT),
    DRIVE_SUITE_ENTRY(DRIVE_SUITE_SMOKE, "Single-Drive Smoke", DT_SUITE_SMOKE_COUNT),
    DRIVE_SUITE_ENTRY(DRIVE_SUITE_ISOLATION, "Unit & Drive Isolation", DT_SUITE_ISOLATION_COUNT),
    DRIVE_SUITE_ENTRY(DRIVE_SUITE_CONCAT, "Cross-Drive Copy & Concat", DT_SUITE_CONCAT_COUNT),
    DRIVE_SUITE_ENTRY(DRIVE_SUITE_MULTI_SEQ, "Multi-Sector Streams", DT_SUITE_MULTI_SEQ_COUNT),
    DRIVE_SUITE_ENTRY(DRIVE_SUITE_REL, "Relative Files", DT_SUITE_REL_COUNT),
    DRIVE_SUITE_ENTRY(DRIVE_SUITE_RAW_BLOCK, "Raw Block Access", DT_SUITE_RAW_BLOCK_COUNT),
    DRIVE_SUITE_ENTRY(DRIVE_SUITE_MAINT, "Boundaries & Maintenance", DT_SUITE_MAINT_COUNT),
    DRIVE_SUITE_ENTRY(DRIVE_SUITE_DUPLICATE, "Disk Duplicate", DT_SUITE_DUPLICATE_COUNT),
};

const diag_plan_t diag_plan_drive = {
    .name = "drive",
    .description = "Virtual IEEE-488 4040/8050/8250 drive qualification",
    .confirm = NULL,
    .prepare = drive_prepare,
    .begin_cycle = diag_target_begin_cycle,
    .cleanup = drive_cleanup,
    .suites = drive_suites,
    .suite_count = count_of(drive_suites),
};
