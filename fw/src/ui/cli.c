// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "cli.h"

#include "breakpoint.h"
#include "console.h"
#include "diag/log/log.h"
#include "diag/diag.h"
#include "display/display.h"
#include "driver.h"
#include "input/input_routing.h"
#include "system/system.h"
#include "pet.h"
#include "reset.h"
#include "system/state.h"
#include "ui/terminal.h"
#include "version.h"

// CLI state
static char line_buffer[CONSOLE_LINE_MAX];
static size_t line_length = 0;
static bool ignore_next_lf;

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
    { "test",   "Run diagnostic plan (test <plan> [count])", cmd_test },
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
    system_reset();
}

/**
 * Parse an optional CLI cycle count token in [1, 255], treating empty text as 1
 * and -1 as the maximum 255-cycle soak count.
 */
static bool parse_cycle_count(const char* text, uint8_t* count_out) {
    // Default an omitted count token to a single diagnostic cycle.
    if (*text == '\0') {
        *count_out = 1;
        return true;
    }

    // Parse a base-10 integer and reject empty digits, overflow, or trailing characters.
    errno = 0;
    char* end = NULL;
    const long parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }

    // Accept 1..255 directly and map -1 to 255 cycles.
    if (parsed == -1 || (parsed >= 1 && parsed <= UINT8_MAX)) {
        *count_out = (uint8_t)parsed;
        return true;
    }
    return false;
}

/** Print CLI usage for the 'test' command and list all registered diagnostic plans. */
static void print_test_usage(void) {
    // Emit command syntax followed by each registered plan's name and one-line summary.
    console_puts("Usage: test <plan> [count]\r\n\r\nAvailable tests:\r\n");
    size_t count;
    const diag_plan_t* const* const plans = diag_plans(&count);
    for (size_t index = 0; index < count; ++index) {
        printf("  %-8s - %s\r\n", plans[index]->name, plans[index]->description);
    }
    fflush(stdout);
}

/**
 * Validate 'test <plan> [count]' arguments before taking system ownership,
 * printing usage on invalid input without disturbing the running guest.
 */
static void cmd_test(const char* args) {
    // Copy the bounded console arguments once and trim trailing spaces in place.
    char tokens[CONSOLE_LINE_MAX];
    snprintf(tokens, sizeof(tokens), "%s", args);
    size_t length = strlen(tokens);
    while (length != 0 && tokens[length - 1] == ' ') tokens[--length] = '\0';

    // Split the plan token from the optional count, retaining extra tokens for count validation.
    char* name = tokens;
    while (*name == ' ') ++name;
    char* count_arg = name;
    while (*count_arg != '\0' && *count_arg != ' ') ++count_arg;
    if (*count_arg != '\0') *count_arg++ = '\0';
    while (*count_arg == ' ') ++count_arg;

    // Look up the requested plan before taking irreversible diagnostic ownership.
    const diag_plan_t* const plan = *name != '\0' ? diag_find_plan(name) : NULL;
    if (plan == NULL) {
        if (*name != '\0') printf("Unrecognized test name '%s'\r\n", name);
        print_test_usage();
        return;
    }

    // Validate the optional cycle count before running confirmation or hardware takeover.
    uint8_t max_cycles = 1;
    if (!parse_cycle_count(count_arg, &max_cycles)) {
        printf("Invalid cycle count '%s' (expected 1-255 or -1)\r\n", count_arg);
        print_test_usage();
        return;
    }

    // Hand off to the plan's interactive confirmation gate if present, otherwise launch directly.
    if (plan->confirm != NULL) plan->confirm(max_cycles);
    diag_run_plan(plan, max_cycles);
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
    ignore_next_lf = false;
    input_routing_cancel(INPUT_SOURCE_UART);

    system_state.terminal_session = terminal_session_cli;

    // Restore default attributes before clearing so blank cells match the banner background.
    fputs(TERM_CLI_RESET, stdout);
    fputs(TERM_CLEAR_SCREEN, stdout);
    fflush(stdout);
    
    // Print welcome banner and initial prompt
    printf("\r\n");
    printf("EconoPET Firmware v%s\r\n(Build: %s Commit: %s%s)\r\n", FW_VERSION_STRING, FW_BUILD_DATE, FW_GIT_HASH, FW_GIT_DIRTY ? "-dirty" : "");
    printf("\r\nType 'help' for available commands.\r\n");
    printf("\r\n");
    console_prompt();
}

void cli_process_char(int ch) {
    if (ignore_next_lf) {
        ignore_next_lf = false;
        if (ch == '\n') return;
    }

    if (console_process_char(ch, line_buffer, &line_length, CONSOLE_LINE_MAX)) {
        // A completed CR command must consume the following LF without treating it as input.
        ignore_next_lf = ch == '\r';
        execute_command(line_buffer);
        line_length = 0;
        if (!cli_in_remote_mode()) console_prompt();
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
        ignore_next_lf = false;
        
        console_puts("\r\n[exited remote mode]\r\n");
        console_prompt();
    }
}
