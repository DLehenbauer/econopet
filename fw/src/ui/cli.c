// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "cli.h"

#include "breakpoint.h"
#include "console.h"
#include "diag/log/log.h"
#include "diag/mem.h"
#include "display/display.h"
#include "driver.h"
#include "input/input_routing.h"
#include "menu/menu_session.h"
#include "pet.h"
#include "reset.h"
#include "system_state.h"
#include "version.h"

// CLI state
static char line_buffer[CONSOLE_LINE_MAX];
static size_t line_length = 0;

// Forward declarations for command handlers
static void cmd_bp(const char* args);
static void cmd_help(const char* args);
static void cmd_log(const char* args);
static void cmd_remote(const char* args);
static void cmd_reset(const char* args);
static void cmd_test(const char* args);

// Command table
typedef struct {
    const char* name;
    const char* description;
    void (*handler)(const char* args);
} cli_command_t;

static const cli_command_t commands[] = {
    { "bp",     "List active breakpoints",                   cmd_bp },
    { "help",   "Show this help message",                    cmd_help },
    { "log",    "Show log [debug|info|warn]",                cmd_log },
    { "remote", "Remote control PET (Ctrl+C to exit)",       cmd_remote },
    { "reset",  "Reset the RP2040",                          cmd_reset },
    { "test",   "Run the continuous memory self-test",       cmd_test },
    { NULL, NULL, NULL }  // Sentinel
};

static void cmd_bp(const char* args) {
    (void)args;

    int count = bp_count();
    if (count == 0) {
        console_puts("(no active breakpoints)\r\n");
        return;
    }

    printf("%d active breakpoint(s):\r\n", count);
    for (int i = 0; i < count; i++) {
        printf("  $%04X\r\n", bp_get(i)->addr);
    }
    fflush(stdout);
}

static void cmd_help(const char* args) {
    (void)args;
    
    console_puts("Commands:\r\n");
    for (const cli_command_t* cmd = commands; cmd->name != NULL; cmd++) {
        printf("  %-8s - %s\r\n", cmd->name, cmd->description);
    }
    fflush(stdout);
}

static log_level_t parse_log_level(const char* str) {
    if (str == NULL || *str == '\0') {
        return LOG_LEVEL_DEBUG;  // Default: show all
    }
    
    // Skip leading whitespace
    while (*str == ' ') str++;
    
    if (strncmp(str, "debug", 5) == 0) return LOG_LEVEL_DEBUG;
    if (strncmp(str, "info", 4) == 0) return LOG_LEVEL_INFO;
    if (strncmp(str, "warn", 4) == 0) return LOG_LEVEL_WARN;
    
    return LOG_LEVEL_DEBUG;  // Default on invalid input
}

static void cmd_log(const char* args) {
    log_level_t min_level = parse_log_level(args);

    const uint32_t displayed = log_dump(min_level);
    if (displayed == 0) {
        console_puts("(no log entries)\r\n");
        return;
    }

    printf("(%" PRIu32 " entries)\r\n", displayed);
    fflush(stdout);
}

static void cmd_remote(const char* args) {
    (void)args;
    
    console_puts("[entering remote mode - Ctrl+C to exit]\r\n");
    
    system_state.terminal_session = terminal_session_remote;
    display_term_begin();
    display_term_refresh();  // Immediately render video buffer to terminal
}

static void cmd_reset(const char* args) {
    (void)args;
    if (menu_session_active()) {
        console_puts("Close the menu before resetting the MCU.\r\n");
        return;
    }

    console_puts("Resetting...\r\n");
    system_reset();
}

// Finish active storage traffic before entering the continuous memory diagnostic.
static void cmd_test(const char* args) {
    if (menu_session_active()) {
        console_puts("Close the menu before running memory diagnostics.\r\n");
        return;
    }
    while (*args == ' ') ++args;
    if (*args != '\0') {
        console_puts("Usage: test\r\n");
        return;
    }
    pet_wait_for_storage_idle();
    set_cpu(CPU_HALT);
    test_ram();
}

static void execute_command(const char* line) {
    // Skip leading whitespace
    while (*line == ' ') line++;
    
    // Empty line - just show prompt again
    if (*line == '\0') {
        return;
    }
    
    // Find end of command name
    const char* cmd_end = line;
    while (*cmd_end && *cmd_end != ' ') cmd_end++;
    size_t cmd_len = cmd_end - line;
    
    // Find start of arguments
    const char* args = cmd_end;
    while (*args == ' ') args++;
    
    // Look up command
    for (const cli_command_t* cmd = commands; cmd->name != NULL; cmd++) {
        if (strncmp(line, cmd->name, cmd_len) == 0 && strlen(cmd->name) == cmd_len) {
            cmd->handler(args);
            return;
        }
    }
    
    // Unknown command
    printf("Unknown command: %.*s\r\n", (int)cmd_len, line);
    console_puts("Type 'help' for available commands.\r\n");
    fflush(stdout);
}

void cli_init(void) {
    line_length = 0;
    input_routing_cancel(INPUT_SOURCE_UART);

    system_state.terminal_session = terminal_session_cli;

    // Reset terminal to clean state using VT100/VT102 compatible sequences:
    // - Reset scroll region to full screen (DECSTBM with no args)
    // - Reset character attributes (SGR 0)
    // - Enable auto-wrap mode (DECAWM)
    printf("\e[r");        // Reset scroll region to full screen
    printf("\e[m");        // Reset character attributes
    printf("\e[?7h");      // Enable line wrap (DECAWM)
    fflush(stdout);
    
    // Print welcome banner and initial prompt
    printf("\r\n");
    printf("EconoPET Firmware v%s (%s%s)\r\n", FW_VERSION_STRING, FW_GIT_HASH, FW_GIT_DIRTY ? "-dirty" : "");
    printf("Type 'help' for available commands.\r\n");
    printf("\r\n");
    printf("econopet> ");
    fflush(stdout);
}

void cli_process_char(int ch) {
    if (console_process_char(ch, line_buffer, &line_length, CONSOLE_LINE_MAX)) {
        // Line complete - execute command
        execute_command(line_buffer);
        line_length = 0;
        if (!cli_in_remote_mode()) {
            console_prompt();
        }
    }
}

bool cli_in_remote_mode(void) {
    return system_state.terminal_session == terminal_session_remote;
}

void cli_exit_remote(void) {
    if (cli_in_remote_mode()) {
        input_routing_cancel(INPUT_SOURCE_UART);
        display_term_end();
        
        system_state.terminal_session = terminal_session_cli;
        
        console_puts("\r\n[exited remote mode]\r\n");
        console_prompt();
    }
}
