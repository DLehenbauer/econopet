// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

// EconoPET hardware contract. Names match common_pkg.sv after removing the
// ECONOPET_ prefix. *_BIT is an index, *_MASK a mask, and *_ADDR a byte
// address.
//
// IMPORTANT: Keep contract definitions in sync with common_pkg.sv. The
//            generated 'system.HardwareContract.MatchesCommonPackage'
//            suite checks every exported constant against the RTL.
//
//            Use unsigned decimal or hexadecimal literals with a u suffix here.
//            Literals keep C macro expansion simple and hardware values easy
//            to inspect. Keep derivations in common_pkg.sv, where the consistency
//            test checks these compiled values against elaborated RTL without
//            duplicating construction logic.

// Data and address widths (See common_pkg.sv for more details)
#define ECONOPET_DATA_WIDTH 8u
#define ECONOPET_CPU_ADDR_WIDTH 16u
#define ECONOPET_RAM_ADDR_WIDTH 17u
#define ECONOPET_BRAM_ADDR_WIDTH 12u
#define ECONOPET_WB_ADDR_WIDTH 20u

// Wishbone window addresses (See common_pkg.sv for more details)
#define ECONOPET_WB_RAM_BASE_ADDR 0x00000u
#define ECONOPET_WB_CRTC_BASE_ADDR 0x50000u
#define ECONOPET_WB_KBD_BASE_ADDR 0x60000u
#define ECONOPET_WB_BRAM_BASE_ADDR 0x68000u
#define ECONOPET_WB_IEEE_BASE_ADDR 0x70000u

// Register-file Wishbone byte addresses (See common_pkg.sv for more details)
#define ECONOPET_WB_STATUS_ADDR 0x40000u
#define ECONOPET_WB_CPU_ADDR 0x40001u
#define ECONOPET_WB_VIDEO_ADDR 0x40002u
#define ECONOPET_WB_BP_CTL_ADDR 0x40003u
#define ECONOPET_WB_BP_LO_ADDR 0x40003u
#define ECONOPET_WB_BP_HI_ADDR 0x40004u
#define ECONOPET_WB_CPU_SEL_ADDR 0x40005u

// Status register masks (See common_pkg.sv for more details)
#define ECONOPET_REG_STATUS_TEXT_MODE_MASK 0x01u
#define ECONOPET_REG_STATUS_CRT_MASK 0x02u
#define ECONOPET_REG_STATUS_KEYBOARD_MASK 0x04u
#define ECONOPET_REG_STATUS_BP_HALT_MASK 0x08u
#define ECONOPET_REG_STATUS_PHYS_CPU_MASK 0x10u

// CPU control masks (See common_pkg.sv for more details)
#define ECONOPET_REG_CPU_READY_MASK 0x01u
#define ECONOPET_REG_CPU_RESET_MASK 0x02u
#define ECONOPET_REG_CPU_NMI_MASK 0x04u
#define ECONOPET_REG_CPU_MASK 0x07u

// CPU selection and SuperPET I/O enable (See common_pkg.sv for more details)
#define ECONOPET_CPU_SEL_PHYS_6502 0u
#define ECONOPET_CPU_SEL_SOFT_6809 1u
#define ECONOPET_CPU_SEL_SOFT_6502 2u
#define ECONOPET_CPU_SEL_SUPERPET_IO_MASK 0x04u

// Video column mode and RAM size field (See common_pkg.sv for more details)
#define ECONOPET_REG_VIDEO_RAM_MASK_LO_BIT 1u
#define ECONOPET_REG_VIDEO_RAM_MASK 0x06u
#define ECONOPET_REG_VIDEO_COL_80_MASK 0x01u

// Breakpoint halt-clear control (See common_pkg.sv for more details)
#define ECONOPET_REG_BP_CTL_CLEAR_MASK 0x01u

// Configuration pin levels (See common_pkg.sv for more details)
#define ECONOPET_CONFIG_CRT_CRTC 0u
#define ECONOPET_CONFIG_CRT_FIXED 1u
#define ECONOPET_CONFIG_KEYBOARD_BUSINESS 0u
#define ECONOPET_CONFIG_KEYBOARD_GRAPHICS 1u

// SPI command masks and opcode encodings (See common_pkg.sv for more details)
#define ECONOPET_SPI_CMD_OPCODE_MASK 0xe0u
#define ECONOPET_SPI_CMD_RESERVED_MASK 0x10u
#define ECONOPET_SPI_CMD_ADDRESS_HIGH_MASK 0x0fu
#define ECONOPET_SPI_CMD_READ_AT 0x40u
#define ECONOPET_SPI_CMD_READ_NEXT 0x20u
#define ECONOPET_SPI_CMD_READ_PREV 0x60u
#define ECONOPET_SPI_CMD_READ_SAME 0x00u
#define ECONOPET_SPI_CMD_WRITE_AT 0xc0u
#define ECONOPET_SPI_CMD_WRITE_NEXT 0xa0u
#define ECONOPET_SPI_CMD_WRITE_PREV 0xe0u
#define ECONOPET_SPI_CMD_WRITE_SAME 0x80u

// IEEE register indices and count (See common_pkg.sv for more details)
#define ECONOPET_IEEE_REG_CTRL 0u
#define ECONOPET_IEEE_REG_TXS_LAST 7u
#define ECONOPET_IEEE_REG_COUNT 8u

// IEEE Wishbone byte addresses (See common_pkg.sv for more details)
#define ECONOPET_WB_IEEE_CTRL_ADDR 0x70000u
#define ECONOPET_WB_IEEE_STATUS_ADDR 0x70001u
#define ECONOPET_WB_IEEE_RX_ADDR 0x70002u
#define ECONOPET_WB_IEEE_TX_ADDR 0x70003u
#define ECONOPET_WB_IEEE_TX_LAST_ADDR 0x70004u
#define ECONOPET_WB_IEEE_SA_ADDR 0x70005u
#define ECONOPET_WB_IEEE_TXS_ADDR 0x70006u
#define ECONOPET_WB_IEEE_TXS_LAST_ADDR 0x70007u

// IEEE control-write masks (See common_pkg.sv for more details)
#define ECONOPET_IEEE_CTRL_ENABLE_MASK 0x01u
#define ECONOPET_IEEE_CTRL_FLUSH_MASK 0x02u
#define ECONOPET_IEEE_CTRL_DATA_FLUSH_MASK 0x04u

// IEEE control-read masks (See common_pkg.sv for more details)
#define ECONOPET_IEEE_CTRL_RD_ENABLE_MASK 0x01u
#define ECONOPET_IEEE_CTRL_RD_TX_ROOM_MASK 0x02u

// IEEE status masks (See common_pkg.sv for more details)
#define ECONOPET_IEEE_ST_RX_AVAIL_MASK 0x01u
#define ECONOPET_IEEE_ST_RX_ATN_MASK 0x02u
#define ECONOPET_IEEE_ST_TX_FULL_MASK 0x04u
#define ECONOPET_IEEE_ST_TX_EMPTY_MASK 0x08u
#define ECONOPET_IEEE_ST_ATN_MASK 0x10u
#define ECONOPET_IEEE_ST_LISTENING_MASK 0x20u
#define ECONOPET_IEEE_ST_TALKING_MASK 0x40u
#define ECONOPET_IEEE_ST_TALK_STARVED_MASK 0x80u

// IEEE data FIFO capacity guaranteed by TX room (See common_pkg.sv for more details)
#define ECONOPET_IEEE_TX_BURST_CHUNK 64u
