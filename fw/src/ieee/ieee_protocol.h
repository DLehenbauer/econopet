// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdint.h>

// Commodore's IEEE-488 layer-3 command encodings. LISTEN and TALK use a
// five-bit primary address. SECOND uses a five-bit secondary address, while
// Commodore's OPEN and CLOSE extensions support channels 0 through 15.
enum {
    IEEE_CMD_ADDRESS_MASK = 0x1f,
    IEEE_CMD_CHANNEL_MASK = 0x0f,
    IEEE_CMD_CLASS_MASK = 0xe0,
    IEEE_CMD_OPEN_CLOSE_MASK = 0xf0,

    IEEE_CMD_LISTEN_BASE = 0x20,
    IEEE_CMD_UNLISTEN = 0x3f,
    IEEE_CMD_TALK_BASE = 0x40,
    IEEE_CMD_UNTALK = 0x5f,
    IEEE_CMD_SECONDARY_BASE = 0x60,
    IEEE_CMD_CLOSE_BASE = 0xe0,
    IEEE_CMD_OPEN_BASE = 0xf0,
};

#define IEEE_CMD_LISTEN(device) \
    (IEEE_CMD_LISTEN_BASE | ((device) & IEEE_CMD_ADDRESS_MASK))
#define IEEE_CMD_TALK(device) \
    (IEEE_CMD_TALK_BASE | ((device) & IEEE_CMD_ADDRESS_MASK))
#define IEEE_CMD_SECONDARY(channel) \
    (IEEE_CMD_SECONDARY_BASE | ((channel) & IEEE_CMD_ADDRESS_MASK))
#define IEEE_CMD_CLOSE(channel) \
    (IEEE_CMD_CLOSE_BASE | ((channel) & IEEE_CMD_CHANNEL_MASK))
#define IEEE_CMD_OPEN(channel) \
    (IEEE_CMD_OPEN_BASE | ((channel) & IEEE_CMD_CHANNEL_MASK))

#define IEEE_CMD_CLASS(command) \
    ((command) & IEEE_CMD_CLASS_MASK)
#define IEEE_CMD_ADDRESS(command) \
    ((command) & IEEE_CMD_ADDRESS_MASK)
#define IEEE_CMD_CHANNEL(command) \
    ((command) & IEEE_CMD_CHANNEL_MASK)
#define IEEE_CMD_IS_OPEN(command) \
    (IEEE_CMD_CLASS(command) == IEEE_CMD_CLOSE_BASE \
     && ((command) & IEEE_CMD_OPEN_CLOSE_MASK) == IEEE_CMD_OPEN_BASE)
