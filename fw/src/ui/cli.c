/**
 * PET Clone - Open hardware implementation of the Commodore PET
 * by Daniel Lehenbauer and contributors.
 * 
 * https://github.com/DLehenbauer/commodore-pet-clone
 *
 * To the extent possible under law, I, Daniel Lehenbauer, have waived all
 * copyright and related or neighboring rights to this project. This work is
 * published from the United States.
 *
 * @copyright CC0 http://creativecommons.org/publicdomain/zero/1.0/
 * @author Daniel Lehenbauer <DLehenbauer@users.noreply.github.com> and contributors
 */

#include "cli.h"
#include "console.h"
#include "diag/log/log.h"
#include "display/display.h"
#include "reset.h"
#include "system_state.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// CLI state
static char line_buffer[CONSOLE_LINE_MAX];
static size_t line_length = 0;
static bool in_remote_mode = false;

// Forward declarations for command handlers
static void cmd_help(const char* args);
static void cmd_log(const char* args);
static void cmd_remote(const char* args);
static void cmd_reset(const char* args);

// Command table
typedef struct {
    const char* name;
    const char* description;
    void (*handler)(const char* args);
} cli_command_t;

static const cli_command_t commands[] = {
    { "help",   "Show this help message",                    cmd_help },
    { "log",    "Show log [debug|info|warn]",                cmd_log },
    { "remote", "Remote control PET (Ctrl+C to exit)",       cmd_remote },
    { "reset",  "Reset the RP2040",                          cmd_reset },
    { NULL, NULL, NULL }  // Sentinel
};

// Level name prefixes for log output
static const char* const level_prefixes[] = {
    "D",  // DEBUG
    "I",  // INFO
    "W"   // WARN
};

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
    
    uint32_t count = log_entry_count(min_level);
    if (count == 0) {
        console_puts("(no log entries)\r\n");
        return;
    }
    
    // Get boot time for calculating relative timestamps
    uint64_t boot_time = log_boot_time();
    
    log_iterator_t iter;
    log_iter_init(&iter, min_level);
    
    const log_entry_t* entry;
    log_level_t level;
    uint32_t displayed = 0;
    
    while ((entry = log_iter_next(&iter, &level)) != NULL) {
        // Calculate relative time from boot
        uint64_t relative_us = entry->timestamp_us - boot_time;
        uint32_t seconds = (uint32_t)(relative_us / 1000000);
        uint32_t millis = (uint32_t)((relative_us % 1000000) / 1000);
        
        printf("[%4" PRIu32 ".%03" PRIu32 "] %s: %s\r\n", 
               seconds, millis, level_prefixes[level], entry->message);
        displayed++;
    }
    
    printf("(%" PRIu32 " entries)\r\n", displayed);
    fflush(stdout);
}

static void cmd_remote(const char* args) {
    (void)args;
    
    console_puts("[entering remote mode - Ctrl+C to exit]\r\n");
    
    in_remote_mode = true;
    system_state.term_mode = term_mode_video;
    system_state.term_input_dest = term_input_to_pet;
    display_term_begin();
    display_term_refresh();  // Immediately render video buffer to terminal
}

static void cmd_reset(const char* args) {
    (void)args;
    
    console_puts("Resetting...\r\n");
    system_reset();
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
    in_remote_mode = false;

    system_state.term_mode = term_mode_cli;
    system_state.term_input_dest = term_input_to_firmware;

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
    printf("EconoPET Debug Console\r\n");
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
        console_prompt();
    }
}

bool cli_in_remote_mode(void) {
    return in_remote_mode;
}

void cli_exit_remote(void) {
    if (in_remote_mode) {
        in_remote_mode = false;
        display_term_end();
        
        system_state.term_mode = term_mode_cli;
        system_state.term_input_dest = term_input_to_firmware;
        
        console_puts("\r\n[exited remote mode]\r\n");
        console_prompt();
    }
}
