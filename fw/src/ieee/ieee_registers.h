// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

// FPGA register block (see gw common_pkg.sv WB_IEEE_BASE = 5'b01110 and
// ieee.sv for semantics). Shared by the firmware transport and its host mocks.
#define ADDR_IEEE (0b01110 << 15)
#define IEEE_REGISTER_COUNT 8

#define IEEE_REG_CTRL     (ADDR_IEEE + 0)
#define IEEE_REG_STATUS   (ADDR_IEEE + 1)
#define IEEE_REG_RX       (ADDR_IEEE + 2)
#define IEEE_REG_TX       (ADDR_IEEE + 3)
#define IEEE_REG_TX_LAST  (ADDR_IEEE + 4)
#define IEEE_REG_SA       (ADDR_IEEE + 5)
#define IEEE_REG_TXS      (ADDR_IEEE + 6)   // status-channel TX
#define IEEE_REG_TXS_LAST (ADDR_IEEE + 7)   // status-channel TX, final byte (EOI)

#define IEEE_CTRL_ENABLE     0x01   // write bit
#define IEEE_CTRL_FLUSH      0x02   // write bit
#define IEEE_CTRL_DATA_FLUSH 0x04   // write bit: flush the data TX FIFO only

// CTRL register READ-back bits (asymmetric with the write bits above).
#define IEEE_CTRL_RD_TX_ROOM 0x02   // data TX FIFO has room for >= TX_BURST_CHUNK

// Data TX FIFO burst-fill chunk. Must match TX_BURST_CHUNK in ieee.sv: the
// fabric's tx_room watermark guarantees space for this many bytes, so a burst
// of up to this size never overflows.
#define TX_BURST_CHUNK 64

#define IEEE_ST_RX_AVAIL     0x01
#define IEEE_ST_RX_ATN       0x02
#define IEEE_ST_TX_FULL      0x04
#define IEEE_ST_TX_EMPTY     0x08
#define IEEE_ST_ATN          0x10
#define IEEE_ST_LISTENING    0x20
#define IEEE_ST_TALKING      0x40
