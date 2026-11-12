// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "mock.h"

#include <check.h>

#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "tusb.h"

#include "driver.h"
#include "fatal.h"
#include "hardware_contract.h"
#include "hw.h"
#include "ieee/ieee_protocol.h"
#include "input/input_controls.h"
#include "input/input.h"
#include "input/input_routing.h"
#include "input/input_uart.h"
#include "pet.h"
#include "reset.h"
#include "roms/roms.h"
#include "system_state.h"
#include "input/term_inject.h"
#include "usb/keyboard.h"
#include "usb/keyscan.h"
#include "usb/usb.h"
#include "input/pet_keyboard.h"
#include "display/dvi/dvi.h"

// ---------------------------------------------------------------------------
// Driver mocks
// ---------------------------------------------------------------------------

uint8_t mock_ram[MOCK_RAM_SIZE];
static uint8_t mock_char_rom[1u << ECONOPET_BRAM_ADDR_WIDTH];
static tui_action_t (*input_reader)(void);
static unsigned int input_reader_display_count;
static bool button_pressed;
static bool fail_next_suspend;
static uint8_t physical_matrix[KEY_COL_COUNT];
static uint8_t published_matrix[KEY_COL_COUNT];
static uint8_t fpga_crtc[CRTC_REG_COUNT];
static unsigned int matrix_publish_count;
static unsigned int spi_read_count, spi_write_count;
static unsigned int usb_service_count, hid_service_count, button_sample_count;

#define MOCK_UART_LOG2_CAPACITY 8
#define MOCK_UART_CAPACITY (1u << MOCK_UART_LOG2_CAPACITY)
#define MOCK_UART_CAPACITY_MASK (MOCK_UART_CAPACITY - 1u)
#define MOCK_HID_LOG2_CAPACITY 5
#define MOCK_HID_CAPACITY (1u << MOCK_HID_LOG2_CAPACITY)
#define MOCK_HID_CAPACITY_MASK (MOCK_HID_CAPACITY - 1u)

static uint8_t uart_bytes[MOCK_UART_CAPACITY];
static size_t uart_head, uart_tail;
static struct {
    uint8_t device, instance;
    hid_keyboard_report_t report;
} hid_reports[MOCK_HID_CAPACITY];
static size_t hid_head, hid_tail;
struct uart_inst {
    uint8_t index;
};
static uart_inst_t mock_uart;
uart_inst_t* const uart0 = &mock_uart;
static void (*cpu_run_callback)(void);
static void (*cpu_state_observer)(cpu_state_t);
static cpu_state_t mock_cpu_state;
static cpu_type_t selected_cpu;
static bool suspended, display_transition, menu_attributes;
static uint8_t audio_control = ECONOPET_REG_AUDIO_ENABLE_MASK;
static uint8_t video_control, via_pcr;
static uint8_t pia_control[2], pia_ddr[2], pia_output[2];
static bool text_mode_lower = true;
static bool clock_overridden;
static uint64_t clock_us;
static unsigned int display_task_count;
static unsigned int term_begin_count, term_end_count, term_refresh_count;
static unsigned int term_inject_count;
static int term_inject_last;
static unsigned int pet_nmi_count;
static void (*ram_test_handler)(void);

uint64_t __real_time_us_64(void);

// Keep ordinary tests on monotonic time and allow deterministic breakpoint waits.
uint64_t __wrap_time_us_64(void) {
    return clock_overridden ? clock_us : __real_time_us_64();
}

void mock_clock_set(uint64_t now) {
    clock_overridden = true;
    clock_us = now;
}

void mock_clock_advance(uint64_t delta) {
    ck_assert(clock_overridden);
    clock_us += delta;
}

// Model the probe's minimum reset pulse without delaying host tests.
void sleep_us(uint64_t us) {
    if (clock_overridden) {
        clock_us += us;
    }
}

typedef struct {
    uint8_t byte;
    bool eoi;
} mock_ieee_tx_byte_t;

static struct {
    uint8_t ctrl;
    uint8_t prefetched;
    uint8_t status_prefetched;
    struct {
        uint8_t byte;
        bool atn;
        bool eoi;
        bool atn_end;
        bool talk_event;
    } rx[MOCK_IEEE_RX_CAPACITY];
    size_t rx_head;
    size_t rx_count;
    mock_ieee_tx_byte_t data[MOCK_IEEE_TX_CAPACITY];
    size_t data_count;
    mock_ieee_tx_byte_t status[MOCK_IEEE_TXS_CAPACITY];
    size_t status_count;
    size_t status_repeat_after;
    uint8_t tx_owner;
} mock_ieee;

// Identifies addresses belonging to the emulated FPGA IEEE register block.
static bool mock_ieee_addr(uint32_t addr) {
    return addr >= ECONOPET_WB_IEEE_BASE_ADDR && addr < ECONOPET_WB_IEEE_BASE_ADDR + ECONOPET_IEEE_REG_COUNT;
}

// Flushes captured data, optionally clearing receive and status queues too.
static void mock_ieee_flush(bool data_only) {
    mock_ieee.data_count = 0;
    if (!data_only) {
        mock_ieee.prefetched = 0;
        mock_ieee.status_prefetched = 0;
        mock_ieee.rx_head = 0;
        mock_ieee.rx_count = 0;
        mock_ieee.status_count = 0;
        mock_ieee.tx_owner = 0;
    }
}

// Appends one PET byte and ATN tag to the bounded receive FIFO.
void mock_ieee_enqueue_rx(bool atn, uint8_t byte) {
    mock_ieee_enqueue_rx_eoi(atn, byte, false);
}

// Appends explicit DAV-sampled metadata without inferring EOI from later commands.
void mock_ieee_enqueue_rx_eoi(bool atn, uint8_t byte, bool eoi) {
    ck_assert_uint_lt(mock_ieee.rx_count, MOCK_IEEE_RX_CAPACITY);
    size_t index = (mock_ieee.rx_head + mock_ieee.rx_count) % MOCK_IEEE_RX_CAPACITY;
    mock_ieee.rx[index].atn = atn;
    mock_ieee.rx[index].byte = byte;
    mock_ieee.rx[index].eoi = eoi;
    mock_ieee.rx[index].atn_end = false;
    mock_ieee.rx[index].talk_event = false;
    mock_ieee.rx_count++;
}

// Queue the native ATN50 boundary independently of command and payload bytes.
void mock_ieee_enqueue_atn_end(void) {
    mock_ieee_enqueue_rx(false, 0);
    const size_t index = (mock_ieee.rx_head + mock_ieee.rx_count - 1) % MOCK_IEEE_RX_CAPACITY;
    mock_ieee.rx[index].atn_end = true;
}

// Use the existing ordered-boundary tag without inventing a command or GET.
void mock_ieee_enqueue_listen_end(uint8_t units) {
    ck_assert_uint_gt(units, 0);
    ck_assert_uint_eq(units & ~ECONOPET_IEEE_RX_LISTEN_UNITS_MASK, 0);
    mock_ieee_enqueue_atn_end();
    const size_t index = (mock_ieee.rx_head + mock_ieee.rx_count - 1) % MOCK_IEEE_RX_CAPACITY;
    mock_ieee.rx[index].byte = units;
}

// Retain native source progress separately from controller-sent bytes.
void mock_ieee_enqueue_talk_event(uint8_t owner, uint8_t flags) {
    mock_ieee_enqueue_rx(false,
        (owner & ~(ECONOPET_IEEE_TX_OWNER_VALID_MASK | ECONOPET_IEEE_TX_OWNER_PAUSE_MASK)) | flags);
    const size_t index = (mock_ieee.rx_head + mock_ieee.rx_count - 1) % MOCK_IEEE_RX_CAPACITY;
    mock_ieee.rx[index].talk_event = true;
}

// Returns the captured file-channel byte count.
size_t mock_ieee_data_count(void) {
    return mock_ieee.data_count;
}

// Returns a captured file-channel byte after checking its index.
uint8_t mock_ieee_data_byte(size_t index) {
    ck_assert_uint_lt(index, mock_ieee.data_count);
    return mock_ieee.data[index].byte;
}

// Returns the EOI marker for a captured file-channel byte.
bool mock_ieee_data_eoi(size_t index) {
    ck_assert_uint_lt(index, mock_ieee.data_count);
    return mock_ieee.data[index].eoi;
}

// Models the controller consuming all captured file-channel bytes.
void mock_ieee_clear_data(void) {
    mock_ieee_consume_data(mock_ieee.data_count);
}

// Advances only native GET progress, preserving the unread FIFO suffix.
void mock_ieee_consume_data(size_t count) {
    ck_assert_uint_le(count, mock_ieee.data_count);
    for (size_t index = 0; index < count; ++index)
        if (mock_ieee.data[index].eoi)
            mock_ieee_enqueue_talk_event(mock_ieee.tx_owner,
                ECONOPET_IEEE_RX_TALK_EOI_GET_MASK | ECONOPET_IEEE_RX_TALK_RECHECK_MASK);
    mock_ieee.prefetched = (uint8_t) (mock_ieee.prefetched + count);
    mock_ieee.data_count -= count;
    memmove(mock_ieee.data, mock_ieee.data + count,
        mock_ieee.data_count * sizeof(mock_ieee.data[0]));
}

// Returns the captured command-channel byte count.
size_t mock_ieee_status_count(void) {
    return mock_ieee.status_count;
}

// Returns a captured command-channel byte after checking its index.
uint8_t mock_ieee_status_byte(size_t index) {
    ck_assert_uint_lt(index, mock_ieee.status_count);
    return mock_ieee.status[index].byte;
}

// Returns the EOI marker for a captured command-channel byte.
bool mock_ieee_status_eoi(size_t index) {
    ck_assert_uint_lt(index, mock_ieee.status_count);
    return mock_ieee.status[index].eoi;
}

// Discards transport storage independently of status GET progress.
void mock_ieee_discard_status(void) {
    mock_ieee.status_count = 0;
}

// Injects one ordered hardware selection in the middle of firmware publication.
void mock_ieee_repeat_status_after(size_t writes) {
    ck_assert_uint_gt(writes, 0);
    mock_ieee.status_repeat_after = writes;
}

// Observes the firmware's source gate without treating SA readback as the owner.
bool mock_ieee_tx_paused(void) {
    return (mock_ieee.tx_owner & ECONOPET_IEEE_TX_OWNER_PAUSE_MASK) != 0;
}

// Models a prefix of status GETs, retaining its unread transport suffix.
void mock_ieee_consume_status(size_t count) {
    ck_assert_uint_le(count, mock_ieee.status_count);
    mock_ieee.status_prefetched = (uint8_t) (mock_ieee.status_prefetched + count);
    mock_ieee.status_count -= count;
    memmove(mock_ieee.status, mock_ieee.status + count,
        mock_ieee.status_count * sizeof(mock_ieee.status[0]));
}

// Models fetching the complete queued response.
void mock_ieee_clear_status(void) {
    mock_ieee_consume_status(mock_ieee.status_count);
}

// Reads an IEEE register without popping RX, or reads the mocked PET RAM.
uint8_t spi_read_at(uint32_t addr) {
    if (addr == ECONOPET_WB_CPU_SEL_ADDR) {
        return selected_cpu;
    }
    if (addr == ECONOPET_WB_AUDIO_ADDR) {
        return audio_control;
    }
    if (addr == ECONOPET_WB_VIDEO_ADDR) {
        return video_control;
    }
    if (addr == ECONOPET_WB_SUSPEND_ADDR) {
        return suspended ? ECONOPET_REG_SUSPEND_REQUEST_MASK | ECONOPET_REG_SUSPEND_ACK_MASK : 0;
    }
    if (addr >= ECONOPET_WB_CRTC_BASE_ADDR && addr - ECONOPET_WB_CRTC_BASE_ADDR < CRTC_REG_COUNT) {
        return fpga_crtc[addr - ECONOPET_WB_CRTC_BASE_ADDR];
    }
    if (addr == ECONOPET_WB_STATUS_ADDR) {
        if (mock_cpu_state == CPU_READY && !system_state.bp_halted && cpu_run_callback != NULL) {
            cpu_run_callback();
        }
        return (system_state.bp_halted ? ECONOPET_REG_STATUS_BP_HALT_MASK : 0)
            | (text_mode_lower ? ECONOPET_REG_STATUS_TEXT_MODE_MASK : 0);
    }
    // Synthesize FIFO readiness and peek at the pending receive byte.
    if (mock_ieee_addr(addr)) {
        if (addr == ECONOPET_WB_IEEE_TX_ADDR) return mock_ieee.prefetched;
        if (addr == ECONOPET_WB_IEEE_TXS_ADDR) return mock_ieee.status_prefetched;
        if (addr == ECONOPET_WB_IEEE_CTRL_ADDR) {
            uint8_t ctrl = (mock_ieee.ctrl & ECONOPET_IEEE_CTRL_UNIT_MASK)
                ? ECONOPET_IEEE_CTRL_RD_ENABLE_MASK : 0;
            if (mock_ieee.data_count <= MOCK_IEEE_TX_CAPACITY - ECONOPET_IEEE_TX_BURST_CHUNK) {
                ctrl |= ECONOPET_IEEE_CTRL_RD_TX_ROOM_MASK;
            }
            if (mock_ieee.rx_count != 0 && mock_ieee.rx[mock_ieee.rx_head].eoi) {
                ctrl |= ECONOPET_IEEE_CTRL_RD_RX_EOI_MASK;
            }
            if (mock_ieee.rx_count != 0 && mock_ieee.rx[mock_ieee.rx_head].atn_end)
                ctrl |= ECONOPET_IEEE_CTRL_RD_RX_ATN_END_MASK;
            if (mock_ieee.rx_count != 0 && mock_ieee.rx[mock_ieee.rx_head].talk_event)
                ctrl |= ECONOPET_IEEE_CTRL_RD_RX_TALK_EVENT_MASK;
            return ctrl;
        }
        if (addr == ECONOPET_WB_IEEE_STATUS_ADDR) {
            uint8_t status = 0;
            if (mock_ieee.rx_count != 0) {
                status |= ECONOPET_IEEE_ST_RX_AVAIL_MASK;
                if (mock_ieee.rx[mock_ieee.rx_head].atn) status |= ECONOPET_IEEE_ST_RX_ATN_MASK;
            }
            if (mock_ieee.data_count == MOCK_IEEE_TX_CAPACITY) status |= ECONOPET_IEEE_ST_TX_FULL_MASK;
            if (mock_ieee.data_count == 0) status |= ECONOPET_IEEE_ST_TX_EMPTY_MASK;
            return status;
        }
        if (addr == ECONOPET_WB_IEEE_RX_ADDR) {
            return mock_ieee.rx_count == 0 ? 0 : mock_ieee.rx[mock_ieee.rx_head].byte;
        }
    }
    if (addr >= ECONOPET_WB_BRAM_BASE_ADDR &&
        addr - ECONOPET_WB_BRAM_BASE_ADDR < sizeof(mock_char_rom)) {
        return mock_char_rom[addr - ECONOPET_WB_BRAM_BASE_ADDR];
    }
    // Enforce the address-space bound before accessing ordinary RAM.
    ck_assert_uint_lt(addr, MOCK_RAM_SIZE);
    return mock_ram[addr];
}

// Report bulk reads independently of their per-byte register accesses.
unsigned int mock_spi_read_count(void) { return spi_read_count; }

// Report bulk writes independently of their per-byte register accesses.
unsigned int mock_spi_write_count(void) { return spi_write_count; }

// Reads consecutive mocked addresses into the caller's buffer.
void spi_read(uint32_t addr, size_t byteLength, uint8_t* pDest) {
    ++spi_read_count;
    if (addr == ECONOPET_WB_KBD_BASE_ADDR) {
        ck_assert_uint_eq(byteLength, sizeof(physical_matrix));
        memcpy(pDest, physical_matrix, sizeof(physical_matrix));
        return;
    }
    if (addr == ECONOPET_WB_CRTC_BASE_ADDR) {
        ck_assert_uint_eq(byteLength, CRTC_REG_COUNT);
        memcpy(pDest, fpga_crtc, byteLength);
        return;
    }
    for (size_t i = 0; i < byteLength; i++) {
        pDest[i] = spi_read_at(addr + i);
    }
}

// Applies IEEE register side effects or writes RAM, returning the mock's zero reply.
uint8_t spi_write_at(uint32_t addr, uint8_t data) {
    if (addr == ECONOPET_WB_AUDIO_ADDR) {
        audio_control = data;
        return 0;
    }
    if (addr == ECONOPET_WB_VIDEO_ADDR) {
        video_control = data;
        return 0;
    }
    if (addr == ECONOPET_WB_SUSPEND_ADDR) {
        suspended = (data & ECONOPET_REG_SUSPEND_REQUEST_MASK) != 0;
        return 0;
    }
    if (addr >= ECONOPET_WB_CRTC_BASE_ADDR && addr - ECONOPET_WB_CRTC_BASE_ADDR < CRTC_REG_COUNT) {
        fpga_crtc[addr - ECONOPET_WB_CRTC_BASE_ADDR] = data;
        return 0;
    }
    // Apply FIFO flush/pop operations and capture outgoing bytes with EOI.
    if (mock_ieee_addr(addr)) {
        if (addr == ECONOPET_WB_IEEE_CTRL_ADDR) {
            mock_ieee.ctrl = data;
            if (data & ECONOPET_IEEE_CTRL_FLUSH_MASK) mock_ieee_flush(false);
            if (data & ECONOPET_IEEE_CTRL_DATA_FLUSH_MASK) mock_ieee_flush(true);
            if (data & ECONOPET_IEEE_CTRL_STATUS_FLUSH_MASK) mock_ieee_discard_status();
        } else if (addr == ECONOPET_WB_IEEE_SA_ADDR) {
            mock_ieee.tx_owner = data;
        } else if (addr == ECONOPET_WB_IEEE_RX_ADDR) {
            if (mock_ieee.rx_count != 0) {
                mock_ieee.rx_head = (mock_ieee.rx_head + 1) % MOCK_IEEE_RX_CAPACITY;
                mock_ieee.rx_count--;
            }
        } else if (addr == ECONOPET_WB_IEEE_TX_ADDR || addr == ECONOPET_WB_IEEE_TX_LAST_ADDR) {
            ck_assert_uint_lt(mock_ieee.data_count, MOCK_IEEE_TX_CAPACITY);
            mock_ieee.data[mock_ieee.data_count++] = (mock_ieee_tx_byte_t) {
                .byte = data,
                .eoi = addr == ECONOPET_WB_IEEE_TX_LAST_ADDR,
            };
        } else if (addr == ECONOPET_WB_IEEE_TXS_ADDR || addr == ECONOPET_WB_IEEE_TXS_LAST_ADDR) {
            ck_assert_uint_lt(mock_ieee.status_count, MOCK_IEEE_TXS_CAPACITY);
            mock_ieee.status[mock_ieee.status_count++] = (mock_ieee_tx_byte_t) {
                .byte = data,
                .eoi = addr == ECONOPET_WB_IEEE_TXS_LAST_ADDR,
            };
            if (mock_ieee.status_repeat_after != 0 && --mock_ieee.status_repeat_after == 0) {
                mock_ieee_discard_status();
                mock_ieee_enqueue_rx(true, IEEE_CMD_SECONDARY(IEEE_COMMAND_CHANNEL));
                mock_ieee_enqueue_atn_end();
            }
        }
        return 0;
    }
    if (addr >= ECONOPET_WB_BRAM_BASE_ADDR &&
        addr - ECONOPET_WB_BRAM_BASE_ADDR < sizeof(mock_char_rom)) {
        mock_char_rom[addr - ECONOPET_WB_BRAM_BASE_ADDR] = data;
        return 0;
    }
    // Preserve the existing no-op behavior for writes outside mocked RAM.
    if (addr < MOCK_RAM_SIZE) {
        mock_ram[addr] = data;
    }
    return 0;
}

// Writes the caller's bytes to consecutive mocked addresses.
void spi_write(uint32_t addr, const uint8_t* pSrc, size_t byteLength) {
    ++spi_write_count;
    if (addr == ECONOPET_WB_KBD_BASE_ADDR) {
        ck_assert_uint_eq(byteLength, sizeof(published_matrix));
        memcpy(published_matrix, pSrc, sizeof(published_matrix));
        ++matrix_publish_count;
        return;
    }
    for (size_t i = 0; i < byteLength; i++) {
        spi_write_at(addr + i, pSrc[i]);
    }
}

// Writes all bytes to one register, as required by FIFO burst transfers.
void spi_write_same_block(uint32_t addr, const uint8_t* pSrc, size_t byteLength) {
    for (size_t i = 0; i < byteLength; i++) spi_write_at(addr, pSrc[i]);
}

// Preserves the legacy zero reply for unused sequential SPI reads.
uint8_t spi_read_next(void) { return 0; }
// Preserves the legacy zero reply for unused reverse SPI reads.
uint8_t spi_read_prev(void) { return 0; }

// Ignores RAM fills in this legacy host mock.
void spi_fill(uint32_t addr, uint8_t byte, size_t byteLength) {
    (void)addr;
    (void)byte;
    (void)byteLength;
}

// The register fixture has no transport pipeline or adjacent-read side effect.
uint8_t spi_read_pop(void) { return 0; }
uint8_t spi_read_once(uint32_t address) { return spi_read_at(address); }

// Records CPU control flags for subsequent host-side reads.
void set_cpu(cpu_state_t state) {
    if (cpu_state_observer != NULL)
        cpu_state_observer(state);
    mock_cpu_state = state;
}

void mock_cpu_set_state_observer(void (*observer)(cpu_state_t)) {
    cpu_state_observer = observer;
}

// Supplies CPU execution effects independently of the breakpoint probe.
void mock_cpu_set_run_callback(void (*callback)(void)) {
    cpu_run_callback = callback;
}

// Sets the character-set status returned by the FPGA mock.
void mock_set_text_mode_lower(bool lower) {
    text_mode_lower = lower;
}

// Record socket/soft-core routing independently of execution flags.
void set_cpu_type(cpu_type_t cpu) {
    selected_cpu = cpu;
}

/** Record the independent SuperPET machine bit used by configuration activation. */
void set_cpu_type_machine(cpu_type_t cpu, bool superpet_io) {
    selected_cpu = (cpu_type_t)((uint8_t)cpu | (superpet_io ? ECONOPET_CPU_SEL_SUPERPET_IO_MASK : 0));
}

/** Model readable PIA direction/data selection and the separate hidden output latches. */
uint8_t pet_io_read(const uint16_t address) {
    ck_assert(suspended || !(mock_cpu_state & CPU_READY));
    if (address == 0xe84c) {
        return via_pcr;
    }
    ck_assert(address >= 0xe810 && address <= 0xe813);
    const unsigned port = (address - 0xe810) / 2;
    if (address & 1) {
        return pia_control[port];
    }
    if (!(pia_control[port] & 4)) {
        return pia_ddr[port];
    }
    const unsigned row = pia_output[0] & 15;
    const uint8_t input = port == 0 || row >= KEY_COL_COUNT ? 0xff : physical_matrix[row];
    return (input & ~pia_ddr[port]) | (pia_output[port] & pia_ddr[port]);
}

/** Record physical PIA writes in the register selected by the current control byte. */
void pet_io_write(const uint16_t address, const uint8_t data) {
    ck_assert(suspended || !(mock_cpu_state & CPU_READY));
    if (address == 0xe84c) {
        via_pcr = data;
        text_mode_lower = (data & 0x0e) == 0x0e;
        return;
    }
    ck_assert(address >= 0xe810 && address <= 0xe813);
    const unsigned port = (address - 0xe810) / 2;
    if (address & 1) {
        pia_control[port] = data & 0x3f;
    } else if (pia_control[port] & 4) {
        pia_output[port] = data;
    } else {
        pia_ddr[port] = data;
    }
}

/** Complete mock suspension synchronously while retaining guest control flags. */
bool cpu_suspend(const bool requested) {
    if (requested && fail_next_suspend) {
        fail_next_suspend = false;
        return false;
    }
    suspended = requested;
    return true;
}
void mock_cpu_fail_next_suspend(void) { fail_next_suspend = true; }
bool cpu_suspended(void) { return suspended; }

/** Model exclusive renderer publication ownership in fixtures without a DVI core. */
bool video_transition_begin(void) {
    ck_assert(!display_transition);
    display_transition = true;
    return true;
}
bool video_transition_end(void) {
    ck_assert(display_transition);
    display_transition = false;
    return true;
}
void video_set_menu_attributes(const bool enabled) { menu_attributes = enabled; }

cpu_type_t mock_cpu_type(void) {
    return selected_cpu;
}

// Returns the most recently recorded CPU control flags.
cpu_state_t get_cpu(void) {
    return mock_cpu_state;
}

// ---------------------------------------------------------------------------
// Breakpoint mocks
// ---------------------------------------------------------------------------

static uint16_t mock_bp_addr;
static bool mock_bp_cleared;

// Returns the configured breakpoint-hit address.
uint16_t bp_hit_addr(void) {
    return mock_bp_addr;
}

// Records halt acknowledgement and clears the shared halted state.
void bp_clear_halt(void) {
    mock_bp_cleared = true;
    system_state.bp_halted = false;
}

// ---------------------------------------------------------------------------
// Test fixture helpers
// ---------------------------------------------------------------------------

// Restores deterministic RAM contents and clears register/control observations.
void mock_reset(void) {
    pet_keyboard_end();
    suspended = display_transition = menu_attributes = false;
    audio_control = ECONOPET_REG_AUDIO_ENABLE_MASK;
    video_control = via_pcr = 0;
    memset(pia_control, 0, sizeof(pia_control));
    memset(pia_ddr, 0, sizeof(pia_ddr));
    memset(pia_output, 0, sizeof(pia_output));
    // Restore the RAM fixture and empty all emulated IEEE queues.
    memset(mock_ram, 0xEA, sizeof(mock_ram));
    memset(mock_char_rom, 0, sizeof(mock_char_rom));
    memset(&mock_ieee, 0, sizeof(mock_ieee));
    // Clear breakpoint observations and hold the CPU in reset.
    system_state.bp_halted = false;
    mock_bp_addr = 0;
    mock_bp_cleared = false;
    mock_cpu_state = CPU_RESET;
    input_reader = NULL;
    uart_head = uart_tail = hid_head = hid_tail = 0;
    button_pressed = false;
    fail_next_suspend = false;
    memset(physical_matrix, UINT8_MAX, sizeof(physical_matrix));
    memset(published_matrix, UINT8_MAX, sizeof(published_matrix));
    memset(fpga_crtc, 0, sizeof(fpga_crtc));
    matrix_publish_count = usb_service_count = hid_service_count = button_sample_count = 0;
    spi_read_count = spi_write_count = 0;
    cpu_run_callback = NULL;
    cpu_state_observer = NULL;
    display_task_count = 0;
    term_begin_count = term_end_count = term_refresh_count = 0;
    term_inject_count = 0;
    term_inject_last = EOF;
    input_init();
    pet_nmi_count = 0;
    ram_test_handler = NULL;
    text_mode_lower = true;
    selected_cpu = CPU_PHYS_6502;
    clock_overridden = false;
    clock_us = 0;
}

// Configures the address observed by breakpoint handling.
void mock_breakpoint_set_hit_addr(uint16_t addr) {
    mock_bp_addr = addr;
}

// Reports whether breakpoint handling acknowledged a halt.
bool mock_breakpoint_halt_was_cleared(void) {
    return mock_bp_cleared;
}

// ---------------------------------------------------------------------------
// Display mocks
// ---------------------------------------------------------------------------

// Accepts a display window without rendering it in host tests.
void __wrap_display_window_begin(const void* window) {
    (void)window;
}

// Flushes host output instead of presenting a hardware display window.
void __wrap_display_window_show(const void* window) {
    (void)window;
    fflush(stdout);
}

// Counts presentations without writing to hardware or a terminal.
void __wrap_display_task(void) { display_task_count++; }

// Reports presentations since the last fixture reset.
unsigned int mock_display_task_count(void) { return display_task_count; }

// Avoids ANSI rendering in host tests.
void __wrap_display_term_begin(void) {
    term_begin_count++;
    fflush(stdout);
}
void __wrap_display_term_end(void) {
    term_end_count++;
    fflush(stdout);
}
void __wrap_display_term_refresh(void) { term_refresh_count++; }

/** Reject hardware video startup in the host input fixture. */
void video_init(void) { fatal("unexpected hardware video initialization in host test"); }

unsigned int mock_term_begin_count(void) { return term_begin_count; }
unsigned int mock_term_end_count(void) { return term_end_count; }
unsigned int mock_term_refresh_count(void) { return term_refresh_count; }

bool __real_term_inject_char(int ch);

// Observe accepted runtime characters while retaining the real injection backend.
bool __wrap_term_inject_char(int ch) {
    const bool accepted = __real_term_inject_char(ch);
    if (accepted) {
        ++term_inject_count;
        term_inject_last = ch;
    }
    return accepted;
}

unsigned int mock_term_inject_count(void) { return term_inject_count; }
int mock_term_inject_last(void) { return term_inject_last; }

void system_reset(void) { fatal("unexpected system reset in host test"); }

// ---------------------------------------------------------------------------
// Input mocks
// ---------------------------------------------------------------------------

// Supplies component-level menu actions during USB servicing, never during a queue read.
void mock_input_set_reader(tui_action_t (*reader)(void)) {
    input_reader = reader;
    input_reader_display_count = display_task_count;
}

void mock_input_uart_bytes(const uint8_t* bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        const size_t next = (uart_head + 1u) & MOCK_UART_CAPACITY_MASK;
        ck_assert_msg(next != uart_tail, "mock UART queue full");
        uart_bytes[uart_head] = bytes[i];
        uart_head = next;
    }
}

void mock_input_hid_report(uint8_t device, uint8_t instance, const hid_keyboard_report_t* report) {
    const size_t next = (hid_head + 1u) & MOCK_HID_CAPACITY_MASK;
    ck_assert_msg(next != hid_tail, "mock HID queue full");
    hid_reports[hid_head].device = device;
    hid_reports[hid_head].instance = instance;
    hid_reports[hid_head].report = *report;
    hid_head = next;
}

void mock_input_button_level(bool pressed) { button_pressed = pressed; }

void mock_input_pet_snapshot(const uint8_t matrix[KEY_COL_COUNT]) {
    memcpy(physical_matrix, matrix, sizeof(physical_matrix));
}

void mock_input_crtc_snapshot(const uint8_t registers[CRTC_REG_COUNT]) {
    memcpy(fpga_crtc, registers, sizeof(fpga_crtc));
}

const uint8_t* mock_input_published_matrix(void) { return published_matrix; }
unsigned int mock_input_publish_count(void) { return matrix_publish_count; }
unsigned int mock_input_usb_service_count(void) { return usb_service_count; }
unsigned int mock_input_hid_service_count(void) { return hid_service_count; }
unsigned int mock_input_button_sample_count(void) { return button_sample_count; }

// Validate the production input initializer's menu-button hardware configuration.
void __real_gpio_init(uint gpio);
void __real_gpio_set_dir(uint gpio, bool out);

void __wrap_gpio_init(uint gpio) {
    if (gpio != MENU_BTN_GP)
        __real_gpio_init(gpio);
}
void gpio_pull_up(uint gpio) { ck_assert_uint_eq(gpio, MENU_BTN_GP); }
void __wrap_gpio_set_dir(uint gpio, bool out) {
    if (gpio == MENU_BTN_GP)
        ck_assert(!out);
    else
        __real_gpio_set_dir(gpio, out);
}

// Publish the active-low button level at the GPIO sampling boundary.
bool gpio_get(uint gpio) {
    ck_assert_uint_eq(gpio, MENU_BTN_GP);
    ++button_sample_count;
    return !button_pressed;
}

// Expose queued UART bytes through the SDK acquisition boundary.
bool uart_is_readable(uart_inst_t* uart) {
    ck_assert_ptr_eq(uart, uart_default);
    return uart_tail != uart_head;
}

char uart_getc(uart_inst_t* uart) {
    ck_assert_ptr_eq(uart, uart_default);
    ck_assert_msg(uart_tail != uart_head, "UART read without available data");
    const uint8_t byte = uart_bytes[uart_tail];
    uart_tail = (uart_tail + 1u) & MOCK_UART_CAPACITY_MASK;
    return (char)byte;
}

// Deliver reports during the real orchestrator's USB-host service call.
void tuh_task_ext(uint32_t timeout_ms, bool in_isr) {
    ck_assert_uint_eq(timeout_ms, UINT32_MAX);
    ck_assert(!in_isr);
    ++usb_service_count;
    while (hid_tail != hid_head) {
        process_kbd_report(hid_reports[hid_tail].device, hid_reports[hid_tail].instance,
            &hid_reports[hid_tail].report);
        hid_tail = (hid_tail + 1u) & MOCK_HID_CAPACITY_MASK;
    }
}

// Component menu fixtures begin observations only after the menu's first render.
void hid_app_task(void) {
    ++hid_service_count;
    if (input_reader != NULL && input_routing_menu_active()
        && display_task_count > input_reader_display_count) {
        (void)input_routing_submit(INPUT_SOURCE_PET, input_reader());
    }
}

input_system_request_t mock_take_request(void) {
    input_controls_poll();
    return input_routing_take_request();
}

bool mock_menu_requested(void) {
    input_controls_poll();
    return input_routing_pending_request().action == input_system_request_menu;
}

bool mock_input_empty(void) {
    const input_system_request_t request = mock_take_request();
    const tui_action_t navigation = input_routing_navigation();
    return request.action == input_system_none && navigation == tui_action_none;
}

void mock_consume_requests(void) {
    (void)mock_take_request();
    input_controls_consume();
}

// ---------------------------------------------------------------------------
// ROM, PET, and IEEE mocks
// ---------------------------------------------------------------------------

// Counts asynchronous NMI requests without emulating the ROM's tune.
void __wrap_pet_nmi(void) {
    pet_nmi_count++;
    spi_write_at(MENU_ROM_COMPLETION_ADDRESS, MENU_ROM_COMPLETION_VALUE);
}

// Reports NMI requests since the last fixture reset.
unsigned int mock_pet_nmi_count(void) { return pet_nmi_count; }

// ---------------------------------------------------------------------------
// Diagnostic mocks
// ---------------------------------------------------------------------------

// Installs an observer that must escape the nonreturning RAM diagnostic.
void mock_ram_test_set_handler(void (*handler)(void)) { ram_test_handler = handler; }

// Delegates the destructive hardware diagnostic to an explicit test observer.
void test_ram(void) {
    vet(ram_test_handler != NULL, "unexpected RAM diagnostic in host test");
    ram_test_handler();
    fatal("RAM diagnostic observer returned");
}
