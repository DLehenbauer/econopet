// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "rom4040.h"

typedef enum {
    FIFO4040_IDLE, FIFO4040_RX_READY, FIFO4040_RX_ACCEPT, FIFO4040_RX_RELEASE,
} fifo4040_receive_t;

typedef struct {
    rom4040_t* machine;
    rom4040_bus_t peer;
    fifo4040_receive_t receive;
    bool transmitting, acknowledged, offered;
    uint8_t byte;
    bool eoi;
    uint32_t presented, consumed;
} fifo4040_t;

// Initialize the synthesized controller-side handshake, without advancing DOS.
void fifo4040_init(fifo4040_t* fifo, rom4040_t* machine);

// Start one ordered RX byte. The caller retains its FIFO head until rx_done.
void fifo4040_receive(fifo4040_t* fifo, uint8_t byte, bool atn, bool eoi);

// Apply an ordered ATN release token from the FPGA RX FIFO.
void fifo4040_atn_end(fifo4040_t* fifo);

// Apply actual PET NRFD-low GET or unread NDAC-high termination, never enqueue.
void fifo4040_consumed(fifo4040_t* fifo, bool get);

// Advance one ROM cycle and synthesize the next phase. Returns RX completion.
bool fifo4040_tick(fifo4040_t* fifo);

// Take the single ROM-presented byte for FPGA enqueue, without advancing GET.
bool fifo4040_transmit(fifo4040_t* fifo, uint8_t* byte, bool* eoi);
