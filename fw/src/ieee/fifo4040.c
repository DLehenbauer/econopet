// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "fifo4040.h"

void fifo4040_init(fifo4040_t* f, rom4040_t* machine) {
    *f = (fifo4040_t) {
        .machine = machine,
        .peer = {true, true, true, true, true, 0xff},
    };
}

void fifo4040_receive(fifo4040_t* f, uint8_t byte, bool atn, bool eoi) {
    assert(f->receive == FIFO4040_IDLE);
    if (atn) {
        // ATN aborts the presentation, but cannot undo a previously applied GET.
        f->transmitting = false;
        f->acknowledged = false;
        f->offered = false;
        f->peer.nrfd = true;
        f->peer.ndac = true;
    }
    f->byte = byte;
    f->eoi = eoi;
    f->peer.atn = !atn;
    f->receive = FIFO4040_RX_READY;
}

void fifo4040_atn_end(fifo4040_t* f) {
    assert(f->receive == FIFO4040_IDLE);
    f->peer.atn = true;
    f->peer.nrfd = true;
    f->peer.ndac = false;
}

void fifo4040_consumed(fifo4040_t* f, bool get) {
    assert(f->transmitting && f->offered && !f->acknowledged);
    f->peer.nrfd = !get;
    f->peer.ndac = true;
    f->acknowledged = true;
    f->consumed += get;
}

bool fifo4040_tick(fifo4040_t* f) {
    rom4040_input(f->machine, f->peer);
    rom4040_tick(f->machine);
    const rom4040_bus_t out = rom4040_output(f->machine);
    bool rx_done = false;
    switch (f->receive) {
        case FIFO4040_RX_READY:
            if (out.nrfd && !out.ndac) {
                f->peer.dio = (uint8_t) ~f->byte;
                f->peer.eoi = !f->eoi;
                f->peer.dav = false;
                f->receive = FIFO4040_RX_ACCEPT;
            }
            break;
        case FIFO4040_RX_ACCEPT:
            if (out.ndac) {
                f->peer.dav = true;
                f->receive = FIFO4040_RX_RELEASE;
            }
            break;
        case FIFO4040_RX_RELEASE:
            if (!out.ndac) {
                f->peer.dio = 0xff;
                f->peer.eoi = true;
                f->receive = FIFO4040_IDLE;
                rx_done = true;
            }
            break;
        case FIFO4040_IDLE:
            if (f->peer.atn && !out.dav && !f->transmitting) {
                f->transmitting = true;
                f->offered = false;
                ++f->presented;
            } else if (out.dav && f->transmitting && f->acknowledged) {
                f->peer.nrfd = true;
                f->peer.ndac = false;
                f->transmitting = false;
                f->acknowledged = false;
            }
            break;
    }
    return rx_done;
}

bool fifo4040_transmit(fifo4040_t* f, uint8_t* byte, bool* eoi) {
    if (!f->transmitting || f->offered) return false;
    const rom4040_bus_t out = rom4040_output(f->machine);
    *byte = (uint8_t) ~out.dio;
    *eoi = !out.eoi;
    f->offered = true;
    return true;
}
