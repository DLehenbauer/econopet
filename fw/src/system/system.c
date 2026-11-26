// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "system/system.h"

#include "breakpoint.h"
#include "display/display.h"
#include "display/window.h"
#include "driver.h"
#include "fatal.h"
#include "hardware_contract.h"
#include "ieee/ieee_drive.h"
#include "ieee/ieee_protocol.h"
#include "input/pet_keyboard.h"
#include "pet.h"
#include "reset.h"
#include "roms/roms.h"
#include "system/state.h"

/** Settle delay (microseconds) after clearing CPU_READY before issuing an I/O bridge write. */
#define CPU_HALT_SETTLE_US 4u
/** Bounded per-phase wait (microseconds) when retiring external IEEE-488 devices on hot-mount. */
#define IEEE_ABORT_TIMEOUT_US 2000u
/** Allow interrupt-driven external adapters to respond to ATN without hardware ATNA. */
#define IEEE_ATN_SETTLE_US 100u
#define CONSOLE_SCREEN_BYTES VIDEO_PLANE_CAPACITY
#define CONSOLE_COLOR_BYTES VIDEO_COLOR_CAPACITY
#define CONSOLE_FONT_BYTES 0x800u

// Only initialized CRTC fields used for console geometry are borrowed.
static const uint8_t crtc_indices[] = {0, 1, 2, 3, 4, 5, 6, 7, 9, 12, 13};

typedef enum {
    CONSOLE_BORROW_INACTIVE,
    CONSOLE_BORROW_ENTERING,
    CONSOLE_BORROW_ACTIVE,
} console_borrow_state_t;

static system_operation_t mode = system_operation_boot;
/** Last non-IEEE VIA DDRB value written to physical VIA while virtual IEEE drives are enabled. */
static uint8_t parked_via_ddrb;
static struct {
    console_borrow_state_t state;
    uint8_t screen[CONSOLE_SCREEN_BYTES], colors[CONSOLE_COLOR_BYTES], font[CONSOLE_FONT_BYTES];
    uint8_t physical_colors[VIDEO_RAM_UNIT_BYTES];
    uint8_t crtc[count_of(crtc_indices)], host_crtc[CRTC_REG_COUNT];
    uint8_t cra, crb, ddra, ddrb, ora, pcr, video, audio, cpu, selection;
    /** Whether virtual IEEE emulation was active when system_suspend() borrowed the console. */
    bool ieee_enabled;
    video_source_t source;
    video_text_mode_t text;
    pet_display_columns_t columns;
    uint8_t video_mask;
} console_borrow;

/** Capture DDRs through their selected registers without driving input pins to inspect latches. */
static void save_pia(void) {
    console_borrow.cra = pet_io_read(ECONOPET_PIA1_CRA_ADDR) & ECONOPET_PIA_CONTROL_WRITABLE_MASK;
    console_borrow.crb = pet_io_read(ECONOPET_PIA1_CRB_ADDR) & ECONOPET_PIA_CONTROL_WRITABLE_MASK;
    pet_io_write(ECONOPET_PIA1_CRA_ADDR, console_borrow.cra & ~ECONOPET_PIA_CONTROL_DATA_SELECT_MASK);
    console_borrow.ddra = pet_io_read(ECONOPET_PIA1_PORTA_ADDR);
    pet_io_write(ECONOPET_PIA1_CRB_ADDR, console_borrow.crb & ~ECONOPET_PIA_CONTROL_DATA_SELECT_MASK);
    console_borrow.ddrb = pet_io_read(ECONOPET_PIA1_PORTB_ADDR);
    pet_io_write(ECONOPET_PIA1_CRA_ADDR, console_borrow.cra | ECONOPET_PIA_CONTROL_DATA_SELECT_MASK);
    console_borrow.ora = pet_io_read(ECONOPET_PIA1_PORTA_ADDR);
    pet_io_write(ECONOPET_PIA1_CRA_ADDR, console_borrow.cra);
    pet_io_write(ECONOPET_PIA1_CRB_ADDR, console_borrow.crb);
}

/** Borrow only keyboard direction/data-selection fields, leaving ORB and C2 modes alone. */
static void setup_pia(void) {
    pet_io_write(ECONOPET_PIA1_CRA_ADDR, console_borrow.cra & ~ECONOPET_PIA_CONTROL_DATA_SELECT_MASK);
    pet_io_write(ECONOPET_PIA1_PORTA_ADDR, console_borrow.ddra | ECONOPET_PIA1_PORTA_KEY_MASK);
    pet_io_write(ECONOPET_PIA1_CRB_ADDR, console_borrow.crb & ~ECONOPET_PIA_CONTROL_DATA_SELECT_MASK);
    pet_io_write(ECONOPET_PIA1_PORTB_ADDR, 0);
    pet_io_write(ECONOPET_PIA1_CRA_ADDR, console_borrow.cra | ECONOPET_PIA_CONTROL_DATA_SELECT_MASK);
    pet_io_write(ECONOPET_PIA1_CRB_ADDR, console_borrow.crb | ECONOPET_PIA_CONTROL_DATA_SELECT_MASK);
}

/** Restore observable ORA before returning guest directions and register selection. */
static void restore_pia(void) {
    pet_io_write(ECONOPET_PIA1_CRA_ADDR, console_borrow.cra | ECONOPET_PIA_CONTROL_DATA_SELECT_MASK);
    pet_io_write(ECONOPET_PIA1_PORTA_ADDR, console_borrow.ora);
    pet_io_write(ECONOPET_PIA1_CRA_ADDR, console_borrow.cra & ~ECONOPET_PIA_CONTROL_DATA_SELECT_MASK);
    pet_io_write(ECONOPET_PIA1_PORTA_ADDR, console_borrow.ddra);
    pet_io_write(ECONOPET_PIA1_CRB_ADDR, console_borrow.crb & ~ECONOPET_PIA_CONTROL_DATA_SELECT_MASK);
    pet_io_write(ECONOPET_PIA1_PORTB_ADDR, console_borrow.ddrb);
    pet_io_write(ECONOPET_PIA1_CRA_ADDR, console_borrow.cra);
    pet_io_write(ECONOPET_PIA1_CRB_ADDR, console_borrow.crb);
}

/**
 * Park physical PIA2 Port A/B and CA2/CB2 as inputs with ORA/ORB selected.
 *
 * When precharge is true, briefly drives Port B and CA2/CB2 high first so the
 * W65C21N output latches hold released ('1') states and charge capacitive bus
 * nodes high before switching to passive pull-ups / input mode.
 */
static void park_pia2_inputs(const bool precharge) {
    // Pre-charge Port B pins and manual C2 outputs high before switching to input mode.
    if (precharge) {
        pet_io_write(ECONOPET_PIA2_CRB_ADDR, ECONOPET_PIA_CONTROL_DATA_SELECT_MASK);
        pet_io_write(ECONOPET_PIA2_PORTB_ADDR, 0xffu);
        pet_io_write(ECONOPET_PIA2_CRB_ADDR, 0x00u);
        pet_io_write(ECONOPET_PIA2_PORTB_ADDR, 0xffu);
        pet_io_write(ECONOPET_PIA2_CRA_ADDR, ECONOPET_PIA_CONTROL_C2_HIGH);
        pet_io_write(ECONOPET_PIA2_CRB_ADDR, ECONOPET_PIA_CONTROL_C2_HIGH);
    }

    // Clear DDRA and DDRB to all-inputs (leaving CA2/CB2 as inputs) and reselect ORA/ORB.
    pet_io_write(ECONOPET_PIA2_CRA_ADDR, 0x00u);
    pet_io_write(ECONOPET_PIA2_PORTA_ADDR, 0x00u);
    pet_io_write(ECONOPET_PIA2_CRA_ADDR, ECONOPET_PIA_CONTROL_DATA_SELECT_MASK);
    pet_io_write(ECONOPET_PIA2_CRB_ADDR, 0x00u);
    pet_io_write(ECONOPET_PIA2_PORTB_ADDR, 0x00u);
    pet_io_write(ECONOPET_PIA2_CRB_ADDR, ECONOPET_PIA_CONTROL_DATA_SELECT_MASK);
}

/** Poll physical VIA Port B until (IRB & mask) == expected or IEEE_ABORT_TIMEOUT_US elapses. */
static bool wait_via_ieee_mask(const uint8_t mask, const uint8_t expected) {
    // Sample physical VIA Port B until the masked inputs match expected or the deadline expires.
    const uint64_t start = time_us_64();
    for (;;) {
        if ((pet_io_read(ECONOPET_VIA_PORTB_ADDR) & mask) == expected) {
            return true;
        }
        if (time_us_64() - start >= IEEE_ABORT_TIMEOUT_US) {
            return false;
        }
    }
}

/**
 * Handshake one IEEE-488 command byte over physical PIA2 under already-asserted ATN.
 *
 * Mirrors PET KERNAL ISOUR (KERNAL_PET_4.0_REC/ob1src): returns false immediately
 * when no external acceptor holds NRFD or NDAC low (device not present), or if
 * an acceptor stalls past IEEE_ABORT_TIMEOUT_US.
 */
static bool send_physical_atn_command(const uint8_t command) {
    // Bail out immediately if neither NRFD nor NDAC is pulled low by an external acceptor.
    const uint8_t initial = pet_io_read(ECONOPET_VIA_PORTB_ADDR) & (ECONOPET_VIA_PORTB_NRFD_IN_MASK | ECONOPET_VIA_PORTB_NDAC_IN_MASK);
    if (initial == (ECONOPET_VIA_PORTB_NRFD_IN_MASK | ECONOPET_VIA_PORTB_NDAC_IN_MASK)) {
        return false;
    }

    // Drive active-low DIO on PIA2 Port B and wait for all acceptors to release NRFD.
    pet_io_write(ECONOPET_PIA2_PORTB_ADDR, (uint8_t)~command);
    if (!wait_via_ieee_mask(ECONOPET_VIA_PORTB_NRFD_IN_MASK, ECONOPET_VIA_PORTB_NRFD_IN_MASK)) {
        pet_io_write(ECONOPET_PIA2_PORTB_ADDR, 0xffu);
        return false;
    }

    // Assert DAV low until all acceptors release NDAC, then release DAV and DIO.
    pet_io_write(ECONOPET_PIA2_CRB_ADDR, ECONOPET_PIA_CONTROL_C2_LOW);
    const bool accepted = wait_via_ieee_mask(ECONOPET_VIA_PORTB_NDAC_IN_MASK, ECONOPET_VIA_PORTB_NDAC_IN_MASK);
    pet_io_write(ECONOPET_PIA2_CRB_ADDR, ECONOPET_PIA_CONTROL_C2_HIGH);
    pet_io_write(ECONOPET_PIA2_PORTB_ADDR, 0xffu);

    // Wait for acceptors to re-arm NDAC low before the next command or ATN release.
    return accepted && wait_via_ieee_mask(ECONOPET_VIA_PORTB_NDAC_IN_MASK, 0x00u);
}

/**
 * Retire any active external IEEE-488 talkers or listeners before hot-isolating
 * the physical port in the menu (where RESB/IFC was not pulsed).
 *
 * Per Commodore 4040/8050/8250 DOS (DOS_4040/ieee, DOS_8250/ieee ATNIRQ), merely
 * releasing control lines leaves an addressed listener holding NDAC low in LSN10
 * or advances an addressed talker into driving DIO/DAV low in TLK20. Asserting
 * ATN triggers ATNIRQ + ATNA to free peripheral DAV/EOI/DIO immediately, and
 * UNLISTEN ($3F) + UNTALK ($5F) clear LSNACT/TLKACT so ATN release returns all
 * external peripherals to IDLE with DAV, NRFD, and NDAC released.
 */
static void retire_physical_ieee_bus(const uint8_t parked_ddrb) {
    // Preserve non-IEEE VIA ORB outputs while releasing NRFD_OUT and asserting ATN_OUT low.
    // Note: VIA Port B reads return external pin levels on input bits rather than
    // the output latch, so reconstruct the guest ORB latch from ieee.sv's snoop.
    const uint8_t preserved_orb =
        (uint8_t)(spi_read_at(ECONOPET_WB_IEEE_VIA_ORB_ADDR) & ~ECONOPET_VIA_PORTB_IEEE_OUT_MASK);
    // Release any active external ATN first so already-asserted ATN (e.g. guest halted
    // mid-ISOUR) still generates a fresh falling ATN edge for external ATNIRQ.
    pet_io_write(ECONOPET_PIA2_CRA_ADDR, ECONOPET_PIA_CONTROL_C2_HIGH);
    pet_io_write(ECONOPET_PIA2_CRB_ADDR, ECONOPET_PIA_CONTROL_DATA_SELECT_MASK);
    pet_io_write(ECONOPET_PIA2_PORTB_ADDR, 0xffu);
    pet_io_write(ECONOPET_PIA2_CRB_ADDR, 0x00u);
    pet_io_write(ECONOPET_PIA2_PORTB_ADDR, 0xffu);
    pet_io_write(ECONOPET_PIA2_CRB_ADDR, ECONOPET_PIA_CONTROL_C2_HIGH);
    pet_io_write(ECONOPET_VIA_PORTB_ADDR, (uint8_t)(preserved_orb | ECONOPET_VIA_PORTB_IEEE_OUT_MASK));
    pet_io_write(ECONOPET_VIA_DDRB_ADDR, (uint8_t)(parked_ddrb | ECONOPET_VIA_PORTB_IEEE_OUT_MASK));
    pet_io_write(ECONOPET_VIA_PORTB_ADDR, (uint8_t)(preserved_orb | ECONOPET_VIA_PORTB_NRFD_OUT_MASK));

    // DAV may already be released before an interrupt-driven adapter responds.
    // Give it a bounded settling interval before deciding no acceptor is present.
    if (wait_via_ieee_mask(ECONOPET_VIA_PORTB_DAV_IN_MASK, ECONOPET_VIA_PORTB_DAV_IN_MASK)) {
        sleep_us(IEEE_ATN_SETTLE_US);
        if (send_physical_atn_command(IEEE_CMD_UNLISTEN)) {
            (void)send_physical_atn_command(IEEE_CMD_UNTALK);
        }
    }

    // Release ATN_OUT high and wait up to IEEE_ABORT_TIMEOUT_US for peripherals to enter IDLE.
    pet_io_write(ECONOPET_VIA_PORTB_ADDR, (uint8_t)(preserved_orb | ECONOPET_VIA_PORTB_IEEE_OUT_MASK));
    (void)wait_via_ieee_mask(ECONOPET_VIA_PORTB_IEEE_IN_MASK, ECONOPET_VIA_PORTB_IEEE_IN_MASK);
}

/**
 * Isolate external IEEE-488 outputs on physical VIA and PIA2 after reset while
 * leaving non-IEEE VIA directions at their 0x00 hardware reset state until the
 * guest writes VIA DDRB.
 */
void system_park_ieee_io(void) {
    // Drive VIA PB2:1 high before clearing DDRB to 0x00 so the W65C22N output
    // latches hold released ('1') states without energizing Cassette #2 motor (PB4).
    pet_io_write(ECONOPET_VIA_PORTB_ADDR, ECONOPET_VIA_PORTB_IEEE_OUT_MASK);
    pet_io_write(ECONOPET_VIA_DDRB_ADDR, ECONOPET_VIA_PORTB_IEEE_OUT_MASK);
    pet_io_write(ECONOPET_VIA_DDRB_ADDR, 0x00u);
    parked_via_ddrb = 0x00u;

    // Pre-charge and park PIA2 DIO, NDAC, and DAV pins as inputs.
    park_pia2_inputs(true);
}

/**
 * Propagate non-IEEE guest VIA DDRB changes (such as PET CINT enabling PB4:3
 * cassette outputs) to physical VIA when virtual IEEE emulation suppresses $E842.
 */
void system_sync_ieee_io(void) {
    if (mode != system_operation_guest || !ieee_drive_enabled()) {
        return;
    }

    // Fast path: return immediately when physical VIA DDRB already matches the masked snoop.
    const uint8_t target_ddrb =
        (uint8_t)(spi_read_at(ECONOPET_WB_IEEE_VIA_DDRB_ADDR) & ~ECONOPET_VIA_PORTB_IEEE_OUT_MASK);
    if (target_ddrb == parked_via_ddrb) {
        return;
    }

    // Only borrow a bus slot when the guest CPU is actively running outside a breakpoint halt.
    const cpu_state_t cpu = get_cpu();
    if (cpu != CPU_READY ||
        (spi_read_at(ECONOPET_WB_STATUS_ADDR) & ECONOPET_REG_STATUS_BP_HALT_MASK) != 0) {
        return;
    }

    // Pause CPU_READY, let any in-flight 6502 write sequence finish, and apply the settled mask.
    set_cpu(CPU_HALT);
    sleep_us(CPU_HALT_SETTLE_US);
    const uint8_t settled_ddrb =
        (uint8_t)(spi_read_at(ECONOPET_WB_IEEE_VIA_DDRB_ADDR) & ~ECONOPET_VIA_PORTB_IEEE_OUT_MASK);
    pet_io_write(ECONOPET_VIA_DDRB_ADDR, settled_ddrb);
    parked_via_ddrb = settled_ddrb;
    set_cpu(cpu);
}

/**
 * Synchronize physical VIA and PIA2 IEEE pin directions with virtual-drive
 * presence when returning from the firmware menu to the guest.
 */
static void restore_ieee_io(void) {
    const bool enabled = ieee_drive_enabled();
    if (enabled) {
        // Restore guest non-IEEE directions, including cassette inputs, while parking PB2:1.
        const uint8_t guest_ddrb = spi_read_at(ECONOPET_WB_IEEE_VIA_DDRB_ADDR);
        const uint8_t parked_ddrb = (uint8_t)(guest_ddrb & ~ECONOPET_VIA_PORTB_IEEE_OUT_MASK);

        // Retire any active external IEEE-488 peripheral session before parking on hot-mount.
        if (!console_borrow.ieee_enabled) {
            retire_physical_ieee_bus(parked_ddrb);
        }
        pet_io_write(ECONOPET_VIA_DDRB_ADDR, parked_ddrb);
        parked_via_ddrb = parked_ddrb;
        park_pia2_inputs(!console_borrow.ieee_enabled);
    } else if (console_borrow.ieee_enabled) {
        // Read the contiguous guest PIA2 and VIA shadow block snooped by ieee.sv.
        enum {
            SHADOW_PIA2_ORA = 0,
            SHADOW_PIA2_DDRA,
            SHADOW_PIA2_CRA,
            SHADOW_PIA2_ORB,
            SHADOW_PIA2_DDRB,
            SHADOW_PIA2_CRB,
            SHADOW_VIA_ORB,
            SHADOW_VIA_DDRB,
            SHADOW_COUNT,
        };
        static_assert(
            ECONOPET_WB_IEEE_PIA2_DDRA_ADDR == ECONOPET_WB_IEEE_PIA2_ORA_ADDR + SHADOW_PIA2_DDRA &&
                ECONOPET_WB_IEEE_PIA2_CRA_ADDR == ECONOPET_WB_IEEE_PIA2_ORA_ADDR + SHADOW_PIA2_CRA &&
                ECONOPET_WB_IEEE_PIA2_ORB_ADDR == ECONOPET_WB_IEEE_PIA2_ORA_ADDR + SHADOW_PIA2_ORB &&
                ECONOPET_WB_IEEE_PIA2_DDRB_ADDR == ECONOPET_WB_IEEE_PIA2_ORA_ADDR + SHADOW_PIA2_DDRB &&
                ECONOPET_WB_IEEE_PIA2_CRB_ADDR == ECONOPET_WB_IEEE_PIA2_ORA_ADDR + SHADOW_PIA2_CRB &&
                ECONOPET_WB_IEEE_VIA_ORB_ADDR == ECONOPET_WB_IEEE_PIA2_ORA_ADDR + SHADOW_VIA_ORB &&
                ECONOPET_WB_IEEE_VIA_DDRB_ADDR == ECONOPET_WB_IEEE_PIA2_ORA_ADDR + SHADOW_VIA_DDRB,
            "IEEE guest I/O shadow registers must remain contiguous");
        uint8_t shadow[SHADOW_COUNT];
        spi_read(ECONOPET_WB_IEEE_PIA2_ORA_ADDR, sizeof(shadow), shadow);

        // Program PIA2 Port A/B output latches before enabling DDRs and control modes to avoid bus glitches.
        pet_io_write(ECONOPET_PIA2_CRA_ADDR, ECONOPET_PIA_CONTROL_DATA_SELECT_MASK);
        pet_io_write(ECONOPET_PIA2_PORTA_ADDR, shadow[SHADOW_PIA2_ORA]);
        pet_io_write(ECONOPET_PIA2_CRA_ADDR, 0x00u);
        pet_io_write(ECONOPET_PIA2_PORTA_ADDR, shadow[SHADOW_PIA2_DDRA]);
        pet_io_write(ECONOPET_PIA2_CRA_ADDR, (uint8_t)(shadow[SHADOW_PIA2_CRA] & ECONOPET_PIA_CONTROL_WRITABLE_MASK));

        pet_io_write(ECONOPET_PIA2_CRB_ADDR, ECONOPET_PIA_CONTROL_DATA_SELECT_MASK);
        pet_io_write(ECONOPET_PIA2_PORTB_ADDR, shadow[SHADOW_PIA2_ORB]);
        pet_io_write(ECONOPET_PIA2_CRB_ADDR, 0x00u);
        pet_io_write(ECONOPET_PIA2_PORTB_ADDR, shadow[SHADOW_PIA2_DDRB]);
        pet_io_write(ECONOPET_PIA2_CRB_ADDR, (uint8_t)(shadow[SHADOW_PIA2_CRB] & ECONOPET_PIA_CONTROL_WRITABLE_MASK));

        // Restore physical VIA ORB before VIA DDRB so external ATN/NRFD resume at their guest levels.
        pet_io_write(ECONOPET_VIA_PORTB_ADDR, shadow[SHADOW_VIA_ORB]);
        pet_io_write(ECONOPET_VIA_DDRB_ADDR, shadow[SHADOW_VIA_DDRB]);
    }
}

/** Retire a completed or partial console borrow without restoring its snapshot. */
static void abandon_console(void) {
    if (console_borrow.state == CONSOLE_BORROW_INACTIVE) {
        return;
    }
    pet_keyboard_end();
    // Leave coordinated fabric/firmware reset to the terminal path.
    set_cpu(CPU_HALT);
    console_borrow.state = CONSOLE_BORROW_INACTIVE;
}

void system_init(void) {
    mode = system_operation_boot;
    parked_via_ddrb = 0x00u;
    console_borrow.state = CONSOLE_BORROW_INACTIVE;
}

system_operation_t system_mode(void) {
    return mode;
}

bool system_suspend(void) {
    if (mode == system_operation_diagnostic ||
        mode == system_operation_console ||
        console_borrow.state != CONSOLE_BORROW_INACTIVE) {
        return false;
    }
    console_borrow.state = CONSOLE_BORROW_ENTERING;

    // Acquire CPU ownership without rewriting the guest control or breakpoint latch.
    console_borrow.cpu = get_cpu();
    console_borrow.selection = spi_read_at(ECONOPET_WB_CPU_SEL_ADDR);
    console_borrow.audio =
        spi_read_at(ECONOPET_WB_AUDIO_ADDR) & ECONOPET_REG_AUDIO_ENABLE_MASK;
    spi_write_at(ECONOPET_WB_AUDIO_ADDR, 0);
    set_cpu((cpu_state_t)(console_borrow.cpu & ~CPU_READY));

    // Save physical bytes, not stale host pixels or the replacement font.
    spi_read(VIDEO_RAM_ADDRESS, sizeof(console_borrow.screen), console_borrow.screen);
    memcpy(console_borrow.colors,
           system_state.video_char_buffer + VIDEO_COLOR_OFFSET,
           sizeof(console_borrow.colors));
    spi_read(ECONOPET_WB_BRAM_BASE_ADDR, sizeof(console_borrow.font), console_borrow.font);
    console_borrow.video = spi_read_at(ECONOPET_WB_VIDEO_ADDR);
    console_borrow.ieee_enabled = ieee_drive_enabled();
    console_borrow.pcr = pet_io_read(ECONOPET_VIA_PCR_ADDR);
    save_pia();
    console_borrow.source = system_state.video_source;
    console_borrow.text = system_state.video_text_mode;
    console_borrow.columns = system_state.pet_display_columns;
    console_borrow.video_mask = system_state.video_ram_mask;
    if (console_borrow.video_mask == pet_video_ram_mask_colourpet) {
        spi_read(VIDEO_COLOR_ADDRESS,
                 sizeof(console_borrow.physical_colors), console_borrow.physical_colors);
    }
    memcpy(console_borrow.host_crtc,
           system_state.pet_crtc_registers, sizeof(console_borrow.host_crtc));
    for (size_t index = 0; index < count_of(crtc_indices); ++index) {
        console_borrow.crtc[index] =
            spi_read_at(ECONOPET_WB_CRTC_BASE_ADDR + crtc_indices[index]);
    }

    // Install the console policy only after the complete snapshot is valid.
    setup_pia();
    pet_io_write(ECONOPET_VIA_PCR_ADDR, (console_borrow.pcr & ~ECONOPET_VIA_PCR_CA2_MASK) | ECONOPET_VIA_PCR_CA2_HIGH);
    (void)display_init_firmware(pet_display_columns_40, VIDEO_DEFAULT_ATTRIBUTE);
    pet_keyboard_begin(console_borrow.ora);
    console_borrow.state = CONSOLE_BORROW_ACTIVE;
    mode = system_operation_console;
    return true;
}

void system_restore(const bool resume) {
    vet(mode == system_operation_console,
        "restoring firmware console outside console operation");
    vet(console_borrow.state == CONSOLE_BORROW_ACTIVE,
        "restoring incomplete firmware console borrow");
    pet_keyboard_end();

    // Keep the guest suspended until physical bytes and renderer state agree.
    system_state.video_source = video_source_pet;
    spi_write(VIDEO_RAM_ADDRESS,
              console_borrow.screen, sizeof(console_borrow.screen));
    if (console_borrow.video_mask == pet_video_ram_mask_colourpet) {
        spi_write(VIDEO_COLOR_ADDRESS,
                  console_borrow.physical_colors,
                  sizeof(console_borrow.physical_colors));
    }
    spi_write(ECONOPET_WB_BRAM_BASE_ADDR,
              console_borrow.font, sizeof(console_borrow.font));
    roms_refresh_char_rom();
    for (size_t index = 0; index < count_of(crtc_indices); ++index) {
        spi_write_at(ECONOPET_WB_CRTC_BASE_ADDR + crtc_indices[index],
                     console_borrow.crtc[index]);
    }
    spi_write_at(ECONOPET_WB_VIDEO_ADDR, console_borrow.video);
    restore_pia();
    restore_ieee_io();
    pet_io_write(ECONOPET_VIA_PCR_ADDR, console_borrow.pcr);
    memcpy(system_state.video_char_buffer,
           console_borrow.screen, sizeof(console_borrow.screen));
    memcpy(system_state.video_char_buffer + VIDEO_COLOR_OFFSET,
           console_borrow.colors, sizeof(console_borrow.colors));
    memcpy(system_state.pet_crtc_registers,
           console_borrow.host_crtc, sizeof(console_borrow.host_crtc));
    system_state.pet_display_columns = console_borrow.columns;
    system_state_set_video_ram_mask(&system_state, console_borrow.video_mask);
    system_state.video_text_mode = console_borrow.text;
    system_state.video_source = console_borrow.source;
    vet(get_cpu() == (cpu_state_t)(console_borrow.cpu & ~CPU_READY) &&
            spi_read_at(ECONOPET_WB_CPU_SEL_ADDR) == console_borrow.selection,
        "guest control changed during firmware console");

    if (resume) {
        set_cpu((cpu_state_t)console_borrow.cpu);
        mode = system_operation_guest;
    }
    spi_write_at(ECONOPET_WB_AUDIO_ADDR, console_borrow.audio);
    console_borrow.state = CONSOLE_BORROW_INACTIVE;
}

void system_reset_guest(void) {
    system_state.video_source = video_source_pet;
    pet_reset();
    mode = system_operation_guest;
}

void system_takeover(void) {
    if (mode == system_operation_diagnostic) {
        return;
    }
    mode = system_operation_diagnostic;
    abandon_console();
}

void __attribute__((noreturn)) system_reset(void) {
    system_takeover();
    mcu_reset();
    __builtin_unreachable();
}

void system_task(void) {
    if (mode != system_operation_guest) {
        return;
    }
    bp_task();
    pet_task();
    system_sync_ieee_io();
}
