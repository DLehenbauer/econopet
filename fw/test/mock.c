// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "mock.h"

#include <check.h>

#include "driver.h"
#include "ieee/ieee_registers.h"
#include "system_state.h"

// ---------------------------------------------------------------------------
// Driver mocks
// ---------------------------------------------------------------------------

uint8_t mock_ram[MOCK_RAM_SIZE];

typedef struct {
    uint8_t byte;
    bool eoi;
} mock_ieee_tx_byte_t;

static struct {
    uint8_t ctrl;
    struct {
        uint8_t byte;
        bool atn;
    } rx[MOCK_IEEE_RX_CAPACITY];
    size_t rx_head;
    size_t rx_count;
    mock_ieee_tx_byte_t data[MOCK_IEEE_TX_CAPACITY];
    size_t data_count;
    mock_ieee_tx_byte_t status[MOCK_IEEE_TXS_CAPACITY];
    size_t status_count;
} mock_ieee;

// Identifies addresses belonging to the emulated FPGA IEEE register block.
static bool mock_ieee_addr(uint32_t addr) {
    return addr >= ADDR_IEEE && addr < ADDR_IEEE + IEEE_REGISTER_COUNT;
}

// Flushes captured data, optionally clearing receive and status queues too.
static void mock_ieee_flush(bool data_only) {
    mock_ieee.data_count = 0;
    if (!data_only) {
        mock_ieee.rx_head = 0;
        mock_ieee.rx_count = 0;
        mock_ieee.status_count = 0;
    }
}

// Appends one PET byte and ATN tag to the bounded receive FIFO.
void mock_ieee_enqueue_rx(bool atn, uint8_t byte) {
    ck_assert_uint_lt(mock_ieee.rx_count, MOCK_IEEE_RX_CAPACITY);
    size_t index = (mock_ieee.rx_head + mock_ieee.rx_count) % MOCK_IEEE_RX_CAPACITY;
    mock_ieee.rx[index].atn = atn;
    mock_ieee.rx[index].byte = byte;
    mock_ieee.rx_count++;
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
    mock_ieee.data_count = 0;
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

// Models the controller consuming all captured command-channel bytes.
void mock_ieee_clear_status(void) {
    mock_ieee.status_count = 0;
}

// Reads an IEEE register without popping RX, or reads the mocked PET RAM.
uint8_t spi_read_at(uint32_t addr) {
    // Synthesize FIFO readiness and peek at the pending receive byte.
    if (mock_ieee_addr(addr)) {
        if (addr == IEEE_REG_CTRL) {
            return mock_ieee.data_count <= MOCK_IEEE_TX_CAPACITY - TX_BURST_CHUNK
                ? IEEE_CTRL_RD_TX_ROOM : 0;
        }
        if (addr == IEEE_REG_STATUS) {
            uint8_t status = 0;
            if (mock_ieee.rx_count != 0) {
                status |= IEEE_ST_RX_AVAIL;
                if (mock_ieee.rx[mock_ieee.rx_head].atn) status |= IEEE_ST_RX_ATN;
            }
            if (mock_ieee.data_count == MOCK_IEEE_TX_CAPACITY) status |= IEEE_ST_TX_FULL;
            if (mock_ieee.data_count == 0) status |= IEEE_ST_TX_EMPTY;
            return status;
        }
        if (addr == IEEE_REG_RX) {
            return mock_ieee.rx_count == 0 ? 0 : mock_ieee.rx[mock_ieee.rx_head].byte;
        }
    }
    // Enforce the address-space bound before accessing ordinary RAM.
    ck_assert_uint_lt(addr, MOCK_RAM_SIZE);
    return mock_ram[addr];
}

// Reads consecutive mocked addresses into the caller's buffer.
void spi_read(uint32_t addr, size_t byteLength, uint8_t* pDest) {
    for (size_t i = 0; i < byteLength; i++) {
        pDest[i] = spi_read_at(addr + i);
    }
}

// Applies IEEE register side effects or writes RAM, returning the mock's zero reply.
uint8_t spi_write_at(uint32_t addr, uint8_t data) {
    // Apply FIFO flush/pop operations and capture outgoing bytes with EOI.
    if (mock_ieee_addr(addr)) {
        if (addr == IEEE_REG_CTRL) {
            mock_ieee.ctrl = data;
            if (data & IEEE_CTRL_FLUSH) mock_ieee_flush(false);
            if (data & IEEE_CTRL_DATA_FLUSH) mock_ieee_flush(true);
        } else if (addr == IEEE_REG_RX) {
            if (mock_ieee.rx_count != 0) {
                mock_ieee.rx_head = (mock_ieee.rx_head + 1) % MOCK_IEEE_RX_CAPACITY;
                mock_ieee.rx_count--;
            }
        } else if (addr == IEEE_REG_TX || addr == IEEE_REG_TX_LAST) {
            ck_assert_uint_lt(mock_ieee.data_count, MOCK_IEEE_TX_CAPACITY);
            mock_ieee.data[mock_ieee.data_count++] = (mock_ieee_tx_byte_t) {
                .byte = data,
                .eoi = addr == IEEE_REG_TX_LAST,
            };
        } else if (addr == IEEE_REG_TXS || addr == IEEE_REG_TXS_LAST) {
            ck_assert_uint_lt(mock_ieee.status_count, MOCK_IEEE_TXS_CAPACITY);
            mock_ieee.status[mock_ieee.status_count++] = (mock_ieee_tx_byte_t) {
                .byte = data,
                .eoi = addr == IEEE_REG_TXS_LAST,
            };
        }
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

static cpu_state_t mock_cpu_state;

// Records CPU control flags for subsequent host-side reads.
void set_cpu(cpu_state_t state) {
    mock_cpu_state = state;
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
    // Restore the RAM fixture and empty all emulated IEEE queues.
    memset(mock_ram, 0xEA, sizeof(mock_ram));
    memset(&mock_ieee, 0, sizeof(mock_ieee));
    // Clear breakpoint observations and hold the CPU in reset.
    system_state.bp_halted = false;
    mock_bp_addr = 0;
    mock_bp_cleared = false;
    mock_cpu_state = CPU_RESET;
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
void display_window_begin(const void* window) {
    (void)window;
}

// Flushes host output instead of presenting a hardware display window.
void display_window_show(const void* window) {
    (void)window;
    fflush(stdout);
}

// Leaves display servicing inactive in host tests.
void display_task(void) { }

// ---------------------------------------------------------------------------
// Input mocks
// ---------------------------------------------------------------------------

// Leaves hardware input initialization inactive in host tests.
void input_init(void) { }

// Leaves hardware input polling inactive in host tests.
void input_task(void) { }

// Reports that no host input character is available.
int input_getch(void) {
    return EOF;
}

// ---------------------------------------------------------------------------
// ROM, PET, and IEEE mocks
// ---------------------------------------------------------------------------

// Leaves character-ROM refresh inactive in host tests.
void roms_refresh_char_rom() { }
// Leaves PET NMI generation inactive in host tests.
void pet_nmi() { }

// ---------------------------------------------------------------------------
// Diagnostic mocks
// ---------------------------------------------------------------------------

// Leaves hardware RAM diagnostics inactive in host tests.
void test_ram() { }
