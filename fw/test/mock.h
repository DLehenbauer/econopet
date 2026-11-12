// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware_contract.h"
#include "driver.h"
#include "input/input_actions.h"
#include "input/keyboard_core.h"
#include "test_support.h"

#define MOCK_RAM_SIZE (1u << ECONOPET_CPU_ADDR_WIDTH)
#define MOCK_IEEE_RX_CAPACITY 32
#define MOCK_IEEE_TX_CAPACITY 1024
#define MOCK_IEEE_TXS_CAPACITY 32

extern uint8_t mock_ram[MOCK_RAM_SIZE];

// Resets RAM, register FIFOs, CPU flags, breakpoint observations, and input.
void mock_reset(void);
// Count bulk SPI transactions since the last reset.
unsigned int mock_spi_read_count(void);
unsigned int mock_spi_write_count(void);
// Invokes the callback on status polls while the mocked CPU is running.
void mock_cpu_set_run_callback(void (*callback)(void));
/** Observe real CPU control transitions while testing machine lifecycle sequencing. */
void mock_cpu_set_state_observer(void (*observer)(cpu_state_t state));
/** Inject one failed hold acquisition so session ownership unwind can be tested. */
void mock_cpu_fail_next_suspend(void);
/** Authorize display setup only for an explicit runtime configuration-exit fixture. */
void mock_allow_configuration_model_writes(bool enabled);
// Selects the character-set status returned by the FPGA mock.
void mock_set_text_mode_lower(bool lower);
// Returns the last CPU selected through the driver mock.
cpu_type_t mock_cpu_type(void);
// Switches the firmware clock to deterministic time until mock_reset().
void mock_clock_set(uint64_t now);
// Advances the deterministic firmware clock without sleeping.
void mock_clock_advance(uint64_t delta);
// Supplies component menu actions during USB servicing after the first render.
void mock_input_set_reader(tui_action_t (*reader)(void));
/** Queue bytes for the production UART adapter's SDK reads. */
void mock_input_uart_bytes(const uint8_t* bytes, size_t size);
/** Queue a report for delivery at the USB host boundary. */
void mock_input_hid_report(uint8_t device, uint8_t instance, const hid_keyboard_report_t* report);
/** Supply the active-low button GPIO's next physical level. */
void mock_input_button_level(bool pressed);
/** Publish a freshly scanned physical PET matrix at the FPGA boundary. */
void mock_input_pet_snapshot(const uint8_t matrix[KEY_COL_COUNT]);
/** Supply CRTC bytes independently of the firmware's cached display state. */
void mock_input_crtc_snapshot(const uint8_t registers[CRTC_REG_COUNT]);
/** Inspect the most recently published runtime matrix, independent of physical input. */
const uint8_t* mock_input_published_matrix(void);
/** Count FPGA exchanges and individual hardware service calls. */
unsigned int mock_input_publish_count(void);
unsigned int mock_input_usb_service_count(void);
unsigned int mock_input_hid_service_count(void);
unsigned int mock_input_button_sample_count(void);
/** Poll gesture adapters without consuming their latched request. */
bool mock_menu_requested(void);
/** Require both independent input groups to be empty, consuming observed input. */
bool mock_input_empty(void);
/** Consume machine requests and suppress current gesture holds in a fixture. */
void mock_consume_requests(void);
/** Explicitly poll fixture gestures, then read the request latch without application dispatch. */
input_system_request_t mock_take_request(void);
// Returns the number of display presentations since the last reset.
unsigned int mock_display_task_count(void);
// Reports terminal screen transitions and refreshes since the last reset.
unsigned int mock_term_begin_count(void);
unsigned int mock_term_end_count(void);
unsigned int mock_term_refresh_count(void);
// Reports PET keystrokes received from terminal routing.
unsigned int mock_term_inject_count(void);
int mock_term_inject_last(void);
// Returns the number of PET NMI requests since the last reset.
unsigned int mock_pet_nmi_count(void);
// Observes entry to the nonreturning RAM diagnostic.
void mock_ram_test_set_handler(void (*handler)(void));
// Sets the address returned by the mocked breakpoint register.
void mock_breakpoint_set_hit_addr(uint16_t addr);
// Reports whether the breakpoint halt was cleared since reset.
bool mock_breakpoint_halt_was_cleared(void);

// Queues a receive byte with its IEEE command/data tag.
void mock_ieee_enqueue_rx(bool atn, uint8_t byte);
// Queues a receive byte with explicit ATN and EOI metadata.
void mock_ieee_enqueue_rx_eoi(bool atn, uint8_t byte, bool eoi);
// Queues an ordered ATN-release event without inventing a received byte.
void mock_ieee_enqueue_atn_end(void);
// Queues the first data DAV-release checkpoint for the specified listener units.
void mock_ieee_enqueue_listen_end(uint8_t units);
// Queue ordered source progress using the hardware owner unit/channel layout.
void mock_ieee_enqueue_talk_event(uint8_t owner, uint8_t flags);
// Returns the number of captured file-channel bytes.
size_t mock_ieee_data_count(void);
// Returns a captured file-channel byte, asserting the index is valid.
uint8_t mock_ieee_data_byte(size_t index);
// Returns a captured file-channel byte's EOI marker.
bool mock_ieee_data_eoi(size_t index);
// Models NRFD-triggered advancement through a prefix of captured data.
void mock_ieee_consume_data(size_t count);
// Models NRFD-triggered advancement through all captured file-channel bytes.
void mock_ieee_clear_data(void);
// Returns the number of captured command-channel bytes.
size_t mock_ieee_status_count(void);
// Returns a captured command-channel byte, asserting the index is valid.
uint8_t mock_ieee_status_byte(size_t index);
// Returns a captured command-channel byte's EOI marker.
bool mock_ieee_status_eoi(size_t index);
// Models NRFD-triggered advancement through a prefix of captured status.
void mock_ieee_consume_status(size_t count);
// Discards status transport storage without fetching any response bytes.
void mock_ieee_discard_status(void);
// Accepts a repeated status secondary after this many TXS writes, flushing storage.
void mock_ieee_repeat_status_after(size_t writes);
// Reports whether firmware has paused new data/status presentations.
bool mock_ieee_tx_paused(void);
// Models fetching every captured command-channel byte.
void mock_ieee_clear_status(void);
