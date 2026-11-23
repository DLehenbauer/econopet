// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

// Terminal control sequences
// (See: https://invisible-island.net/xterm/ctlseqs/ctlseqs.html)

// VT100 compatible controls
#define TERM_HOME "\e[H"
#define TERM_ATTRIBUTES_RESET "\e[m"
#define TERM_REVERSE_ON "\e[7m"
#define TERM_CLEAR_SCREEN "\e[2J" TERM_HOME
// SRM set disables terminal-local echo, reset enables it.
#define TERM_ECHO_OFF "\e[12h"
#define TERM_ECHO_ON "\e[12l"
#define TERM_ORIGIN_RESET "\e[?6l"
#define TERM_SCROLL_REGION_RESET "\e[r"
#define TERM_WRAP_ON "\e[?7h"
#define TERM_CHARSET_ASCII "\e(B"
#define TERM_CHARSET_LINE_DRAWING "\e(0"
#define TERM_SELECT_G0 "\x0f"

// VT220 compatible controls
#define TERM_CURSOR_HIDE "\e[?25l"
#define TERM_CURSOR_SHOW "\e[?25h"

// xterm extensions
#define TERM_ALTERNATE_ENTER "\e[?1049h"
#define TERM_ALTERNATE_EXIT "\e[?1049l"

// Restore the primary screen and reset its modes, without erasing its contents.
#define TERM_CLI_RESET \
    TERM_ALTERNATE_EXIT TERM_CURSOR_SHOW TERM_ORIGIN_RESET TERM_SCROLL_REGION_RESET \
    TERM_ATTRIBUTES_RESET TERM_WRAP_ON TERM_CHARSET_ASCII TERM_SELECT_G0 TERM_ECHO_OFF

// Normalize prompt rendering and disable local echo because firmware echoes CLI input.
#define TERM_CLI_PROMPT_NORMALIZE \
    TERM_CURSOR_SHOW TERM_ATTRIBUTES_RESET TERM_WRAP_ON TERM_CHARSET_ASCII TERM_SELECT_G0 TERM_ECHO_OFF
