// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "ieee_drive.h"

#include <strings.h>

#include "controller4040.h"
#include "diag/log/log.h"
#include "driver.h"
#include "fatal.h"
#include "fifo4040.h"
#include "hardware_contract.h"
#include "sd/sd.h"

#define SERVICE_CYCLES 4096u
#define SERVICE_US 250u
#define BOOT_CYCLES 2000000u
#define MACHINE_SLICE 64u
#define CRC32_POLYNOMIAL 0xedb88320u

static uint8_t rom[ROM4040_ROM_SIZE];
static bool rom_loaded, enabled, rx_pending, tx_status, tx_pending;
static uint8_t tx_count;
static FILE* files[CONTROLLER4040_DRIVES];
static rom4040_t machine;
static fifo4040_t fifo;
static controller4040_t controller = {.machine = &machine};
static ieee_drive_metrics_t metrics;
static uint64_t last_service;
static uint64_t measurement_start;

// Verify externally supplied ROM chips without modifying or embedding their bytes.
static uint32_t crc32(const uint8_t* bytes, size_t count) {
    uint32_t crc = UINT32_MAX;
    while (count--) {
        crc ^= *bytes++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? CRC32_POLYNOMIAL : 0);
    }
    return ~crc;
}

// Load only the approved DOS 2.0 revision-2 chips from the user's SD card.
static bool load_rom(void) {
    static const char* const names[] = {"901468-14.bin", "901468-15.bin", "901468-16.bin"};
    static const uint32_t crcs[] = {0xbc4d4872, 0xb6970533, 0x1f5eefb7};
    for (unsigned i = 0; i < count_of(names); ++i) {
        char path[SD_PATH_MAX];
        sd_make_path(path, SD_DIR_ROMS, names[i]);
        FILE* const file = fopen(path, "rb");
        if (file == NULL) {
            log_info("ieee-6502: missing ROM %s", path);
            return false;
        }
        uint8_t* const chip = rom + i * 0x1000;
        const bool valid = fread(chip, 1, 0x1000, file) == 0x1000
            && fgetc(file) == EOF && !ferror(file) && crc32(chip, 0x1000) == crcs[i];
        const bool closed = fclose(file) == 0;
        if (!valid || !closed) {
            log_info("ieee-6502: invalid ROM or I/O failure: %s", path);
            return false;
        }
    }
    rom_loaded = true;
    return true;
}

// Execute one raw-sector job, propagating exact I/O/flush failure to the ROM.
static bool disk_io(void* context, uint32_t offset, uint8_t sector[256], bool write) {
    FILE* const file = context;
    if (fseek(file, (long) offset, SEEK_SET) != 0) return false;
    if (write)
        return fwrite(sector, 1, ROM4040_SECTOR_SIZE, file) == ROM4040_SECTOR_SIZE
            && fflush(file) == 0;
    return fread(sector, 1, ROM4040_SECTOR_SIZE, file) == ROM4040_SECTOR_SIZE;
}

// Abort the experiment explicitly instead of inventing successful hardware behavior.
static void check_fault(void) {
    vet(machine.fault == ROM4040_FAULT_NONE,
        "ieee-6502: unsupported hardware %u at PC $%04x",
        machine.fault, m6502_pc(&machine.cpu));
}

// Start an unmodified ROM from RESET, including native RAM/ROM diagnostics.
static void boot(void) {
    const uint64_t start = time_us_64();
    controller.unsupported_code = false;
    rom4040_init(&machine, rom, controller4040_job, &controller);
    rom4040_run(&machine, BOOT_CYCLES);
    check_fault();
    vet(machine.running && machine.edge_enabled[1], "ieee-6502: ROM failed power-on diagnostics");
    fifo4040_init(&fifo, &machine);
    rx_pending = tx_pending = false;
    metrics = (ieee_drive_metrics_t) {0};
    last_service = time_us_64();
    measurement_start = last_service;
    log_info("ieee-6502: boot %lu cycles, %lu us",
        (unsigned long) machine.cycles, (unsigned long) (last_service - start));
    log_info("ieee-6502: state %u + ROM %u bytes",
        (unsigned) sizeof(machine), (unsigned) sizeof(rom));
}

void ieee_drive_init(void) {
    ieee_drive_unmount_all();
}

void ieee_drive_reset(void) {
    if (!enabled) return;
    spi_write_at(ECONOPET_WB_IEEE_CTRL_ADDR, 0);
    boot();
    spi_write_at(ECONOPET_WB_IEEE_CTRL_ADDR,
        ECONOPET_IEEE_CTRL_ENABLE_MASK | ECONOPET_IEEE_CTRL_FLUSH_MASK);
}

void ieee_drive_unmount_all(void) {
    spi_write_at(ECONOPET_WB_IEEE_CTRL_ADDR, 0);
    enabled = false;
    for (unsigned i = 0; i < CONTROLLER4040_DRIVES; ++i) {
        if (files[i] != NULL) {
            const int result = fclose(files[i]);
            files[i] = NULL;
            vet(result == 0, "ieee-6502: disk close failed");
        }
        controller.disk[i] = (controller4040_disk_t) {0};
    }
}

bool ieee_drive_mount(unsigned slot, const char* filename) {
    if (slot >= CONTROLLER4040_DRIVES) {
        log_info("ieee-6502: only device 8, drives 0/1 are supported");
        return false;
    }
    if (!rom_loaded && !load_rom()) return false;
    const char* const extension = strrchr(filename, '.');
    if (extension == NULL || strcasecmp(extension, ".d64") != 0) {
        log_info("ieee-6502: only 35-track .d64 images are supported: %s", filename);
        return false;
    }
    char path[SD_PATH_MAX];
    sd_make_path(path, SD_DIR_DISKS, filename);
    bool writable = true;
    FILE* file = fopen(path, "r+b");
    if (file == NULL) {
        writable = false;
        file = fopen(path, "rb");
    }
    if (file == NULL) {
        log_info("ieee-6502: cannot open %s", path);
        return false;
    }
    uint8_t bam[ROM4040_SECTOR_SIZE];
    uint32_t offset;
    const bool coordinate = controller4040_offset(18, 0, &offset);
    vet(coordinate, "ieee-6502: invalid BAM coordinates");
    const bool valid = fseek(file, 0, SEEK_END) == 0
        && ftell(file) == CONTROLLER4040_IMAGE_SIZE && disk_io(file, offset, bam, false);
    if (!valid) {
        log_info("ieee-6502: invalid D64 size or I/O error: %s", path);
        const int result = fclose(file);
        vet(result == 0, "ieee-6502: rejected disk close failed");
        return false;
    }
    if (files[slot] != NULL) {
        const int result = fclose(files[slot]);
        vet(result == 0, "ieee-6502: replaced disk close failed");
    }
    files[slot] = file;
    controller.disk[slot] = (controller4040_disk_t) {
        .io = disk_io, .context = file, .writable = writable, .id = {bam[0xa2], bam[0xa3]},
    };
    log_info("ieee-6502: device 8 drive %u = %s (%s)", slot, path,
        writable ? "read/write" : "read-only");
    enabled = true;
    ieee_drive_reset();
    return true;
}

// Reconcile the sole offered byte before any ATN, owner change, or FIFO discard.
static void consumption(void) {
    if (!tx_pending) return;
    const uint8_t count = spi_read_at(tx_status
        ? ECONOPET_WB_IEEE_TXS_ADDR : ECONOPET_WB_IEEE_TX_ADDR);
    const uint8_t delta = (uint8_t) (count - tx_count);
    vet(delta <= 1, "ieee-6502: consumption counter advanced by %u", delta);
    if (delta) {
        fifo4040_consumed(&fifo, true);
        tx_pending = false;
    }
}

void ieee_drive_task(void) {
    if (!enabled) return;
    const uint64_t start = time_us_64();
    const uint64_t gap = start - last_service;
    if (gap > metrics.max_service_gap_us) metrics.max_service_gap_us = gap;
    ++metrics.calls;
    const uint32_t before = machine.cycles;
    for (unsigned slice = 0; slice < SERVICE_CYCLES / MACHINE_SLICE; ++slice) {
        consumption();
        if (!rx_pending) {
            const uint8_t status = spi_read_at(ECONOPET_WB_IEEE_STATUS_ADDR);
            if (status & ECONOPET_IEEE_ST_RX_AVAIL_MASK) {
                // Pause future DAVs, then sample progress again after active-byte
                // protection. RX ordering can never discard a late native GET.
                spi_write_at(ECONOPET_WB_IEEE_SA_ADDR, ECONOPET_IEEE_TX_OWNER_PAUSE_MASK);
                const uint8_t control = spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR);
                consumption();
                if (!(control & (ECONOPET_IEEE_CTRL_RD_DATA_ACTIVE_MASK
                    | ECONOPET_IEEE_CTRL_RD_STATUS_ACTIVE_MASK))) {
                    // Finish any acknowledged virtual source phase before ATN.
                    if (fifo.acknowledged) {
                        for (unsigned i = 0; i < MACHINE_SLICE; ++i) fifo4040_tick(&fifo);
                    } else {
                        const uint8_t head = spi_read_at(ECONOPET_WB_IEEE_STATUS_ADDR);
                        const uint8_t byte = spi_read_at(ECONOPET_WB_IEEE_RX_ADDR);
                        if (control & ECONOPET_IEEE_CTRL_RD_RX_ATN_END_MASK) {
                            if (byte == 0) fifo4040_atn_end(&fifo);
                            spi_write_at(ECONOPET_WB_IEEE_RX_ADDR, 0);
                        } else if (control & ECONOPET_IEEE_CTRL_RD_RX_TALK_EVENT_MASK) {
                            if (!(byte & ECONOPET_IEEE_RX_TALK_EOI_GET_MASK) && tx_pending) {
                                fifo4040_consumed(&fifo, false);
                                tx_pending = false;
                            }
                            spi_write_at(ECONOPET_WB_IEEE_RX_ADDR, 0);
                        } else {
                            const bool atn = (head & ECONOPET_IEEE_ST_RX_ATN_MASK) != 0;
                            if (atn) {
                                spi_write_at(ECONOPET_WB_IEEE_CTRL_ADDR,
                                    ECONOPET_IEEE_CTRL_ENABLE_MASK
                                    | ECONOPET_IEEE_CTRL_DATA_FLUSH_MASK
                                    | ECONOPET_IEEE_CTRL_STATUS_FLUSH_MASK);
                                tx_pending = false;
                            }
                            fifo4040_receive(&fifo, byte, atn,
                                (control & ECONOPET_IEEE_CTRL_RD_RX_EOI_MASK) != 0);
                            rx_pending = true;
                        }
                    }
                }
            }
        }
        for (unsigned i = 0; i < MACHINE_SLICE; ++i) {
            if (fifo4040_tick(&fifo) && rx_pending) {
                spi_write_at(ECONOPET_WB_IEEE_RX_ADDR, 0);
                rx_pending = false;
            }
            if (machine.fault) break;
        }
        check_fault();
        const uint8_t status = spi_read_at(ECONOPET_WB_IEEE_STATUS_ADDR);
        if (!rx_pending && !(status & ECONOPET_IEEE_ST_RX_AVAIL_MASK)) {
            uint8_t byte;
            bool eoi;
            if (fifo4040_transmit(&fifo, &byte, &eoi)) {
                const uint8_t channel = spi_read_at(ECONOPET_WB_IEEE_SA_ADDR) & 0x0f;
                tx_status = channel == 15;
                tx_count = spi_read_at(tx_status
                    ? ECONOPET_WB_IEEE_TXS_ADDR : ECONOPET_WB_IEEE_TX_ADDR);
                spi_write_at(tx_status
                    ? (eoi ? ECONOPET_WB_IEEE_TXS_LAST_ADDR : ECONOPET_WB_IEEE_TXS_ADDR)
                    : (eoi ? ECONOPET_WB_IEEE_TX_LAST_ADDR : ECONOPET_WB_IEEE_TX_ADDR), byte);
                tx_pending = true;
            }
            spi_write_at(ECONOPET_WB_IEEE_SA_ADDR, 0);
        }
        // The sole source byte cannot progress until the PET consumes or aborts it.
        if (tx_pending && !rx_pending) break;
#if defined(PICO_RP2040)
        if (time_us_64() - start >= SERVICE_US) break;
#endif
    }
    const uint64_t end = time_us_64();
    metrics.cycles += (uint32_t) (machine.cycles - before);
    metrics.service_us += end - start;
    if (end - start > metrics.max_service_us) metrics.max_service_us = end - start;
    last_service = end;
}

ieee_drive_metrics_t ieee_drive_metrics(void) {
    ieee_drive_metrics_t result = metrics;
    if (enabled) result.elapsed_us = time_us_64() - measurement_start;
    result.presented = fifo.presented;
    result.consumed = fifo.consumed;
    return result;
}
