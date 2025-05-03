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

#include "config.h"

void print_error(const char* format, ...) {
    char buffer[512];
    
    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (errno != 0 && written < (int) sizeof(buffer)) {
        snprintf(buffer + written, sizeof(buffer) - written, ": %s", strerror(errno));
        errno = 0;
    }

    fprintf(stderr, "Error: %s\n", buffer);
}

typedef struct parser_state_s {
    yaml_parser_t parser;
    yaml_event_t event;
    const char* filename;
    const config_sink_t* const sink;
} parser_state_t;

typedef bool (*parse_callback_t)(parser_state_t* state);

void print_parse_error(parser_state_t* state, const char* format, ...) {
    char buffer[512];
    va_list args;

    // Start building the error message
    int written = 0;

    // Prepend the file name, line, and column information
    written = snprintf(buffer, sizeof(buffer),
        "'%s' at line %zu, column %zu: ",
        state->filename,
        state->parser.problem_mark.line + 1,
        state->parser.problem_mark.column + 1);

    // Append the custom error message
    if (written < (int) sizeof(buffer)) {
        va_start(args, format);
        vsnprintf(buffer + written, sizeof(buffer) - written, format, args);
        va_end(args);
    }

    // Print the complete error message
    fprintf(stderr, "%s\n", buffer);
}

static bool parse_next(parser_state_t* state) {
    yaml_event_delete(&state->event);
    if (!yaml_parser_parse(&state->parser, &state->event)) {
        print_parse_error(state, "Malformed YAML");
        return false;
    }

    return true;
}

static bool parse_consume_type(parser_state_t* state, yaml_event_type_t expected_type) {
    bool result = parse_next(state);
    if (!result) {
        goto cleanup;
    }

    result = state->event.type == expected_type;
    if (!result) {
        print_parse_error(state, "Expected event type %d, but got %d", expected_type, state->event.type);
        goto cleanup;
    }

cleanup:
    return result;
}

// Helper function to parse a scalar value
static bool parse_string(parser_state_t* state, char* buffer, size_t buffer_size) {
    bool result = parse_next(state);
    if (!result) {
        goto cleanup;
    }

    result = state->event.type == YAML_SCALAR_EVENT;
    if (!result) {
        print_parse_error(state, "Expected a scalar value");
        goto cleanup;
    }

    strncpy(buffer, (const char*) state->event.data.scalar.value, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';

cleanup:
    return result;
}

static bool parse_uint32(parser_state_t* state, uint32_t* value) {
    char buffer[32] = { 0 };

    bool result = parse_string(state, buffer, sizeof(buffer));
    if (!result) {
        goto cleanup;
    }

    char* endptr = NULL;
    *value = strtoul(buffer, &endptr, 0);
    result = *endptr == '\0';
    if (!result) {
        print_parse_error(state, "Invalid integer in YAML file '%s'\n");
        goto cleanup;
    }

cleanup:
    return result;
}

// Helper function to parse a "load" action
static bool parse_load_action(parser_state_t* state) {
    char file[261] = { 0 };     // Windows OS max path length is 260 characters
    uint32_t address = 0;
    bool result;

    while (1) {
        result = parse_next(state);
        if (!result) {
            return false;
        }

        if (state->event.type == YAML_MAPPING_END_EVENT) {
            if (state->sink->on_action_load) {
                state->sink->on_action_load(state->sink->user_data, file, address);
            }
            goto cleanup;
        }

        if (state->event.type == YAML_SCALAR_EVENT) {
            const char* key = (const char*) state->event.data.scalar.value;
            if (strcmp(key, "file") == 0) {
                result = parse_string(state, file, sizeof(file));
            } else if (strcmp(key, "address") == 0) {
                result = parse_uint32(state, &address);
            }
        }

        if (!result) {
            goto cleanup;
        }
    }

cleanup:
    return result;
}

// Updated parse_action function
static bool parse_action(parser_state_t* state) {
    bool result;

    while (1) {
        result = parse_next(state);
        if (!result) {
            goto cleanup;
        }

        if (state->event.type == YAML_MAPPING_END_EVENT) {
            goto cleanup;
        }

        if (state->event.type == YAML_SCALAR_EVENT) {
            const char* key = (const char*) state->event.data.scalar.value;

            if (strcmp(key, "action") == 0) {
                char action_type[64];
                result = parse_string(state, action_type, sizeof(action_type));
                if (!result) {
                    goto cleanup;
                }

                if (strcmp(action_type, "load") == 0) {
                    result = parse_load_action(state);
                    goto cleanup;
                } else {
                    result = false;
                    print_parse_error(state, "Unknown action type '%s'.", action_type);
                    goto cleanup;
                }
            }
        }
    }

cleanup:
    return result;
}

// Helper function to parse a list of actions
static bool parse_sequence(parser_state_t* state, parse_callback_t on_item_fn) {
    bool result;

    while (1) {
        result = parse_next(state);
        if (!result) {
            goto cleanup;
        }

        if (state->event.type == YAML_SEQUENCE_END_EVENT) {
            goto cleanup;
        }

        if (state->event.type == YAML_MAPPING_START_EVENT) {
            result = on_item_fn(state);
            if (!result) {
                goto cleanup;
            }
        }
    }

cleanup:
    return true;
}

// Helper function to parse a list of actions
static bool parse_action_list(parser_state_t* state) {
    return parse_sequence(state, parse_action);
}

// Helper function to parse an individual config
static bool parse_config(parser_state_t* state) {
    if (state->sink->on_enter_config) {
        state->sink->on_enter_config(state->sink->user_data);
    }

    bool result;
    char name[80] = { 0 };

    while (true) {
        result = parse_next(state);
        if (!result) {
            goto cleanup;
        }

        if (state->event.type == YAML_MAPPING_END_EVENT) {
            if (state->sink->on_exit_config) {
                state->sink->on_exit_config(state->sink->user_data, name);
            }
            goto cleanup;
        }

        if (state->event.type == YAML_SCALAR_EVENT) {
            const char* key = (const char*) state->event.data.scalar.value;

            if (strcmp(key, "name") == 0) {
                result = parse_string(state, name, sizeof(name));
                if (!result) {
                    goto cleanup;
                }
            } else if (strcmp(key, "actions") == 0) {
                result = parse_action_list(state);
                if (!result) {
                    goto cleanup;
                }
            }
        }
    }

cleanup:
    return result;
}

// Helper function to parse a list of configs
static bool parse_config_list(parser_state_t* state) {
    return parse_sequence(state, parse_config);
}

void parse_config_file(const char* filename, const config_sink_t* const sink) {
    FILE* file = NULL;

    parser_state_t state = {
        .parser = { 0 },
        .event = { 0 },
        .filename = filename,
        .sink = sink,
    };

    if (!yaml_parser_initialize(&state.parser)) {
        print_error("Failed to initialize YAML parser");
        goto cleanup;
    }

    file = fopen(filename, "r");
    if (!file) {
        print_error("Failed to open file '%s'", filename);
        goto cleanup;
    }

    yaml_parser_set_input_file(&state.parser, file);

    parse_consume_type(&state, YAML_STREAM_START_EVENT);
    parse_consume_type(&state, YAML_DOCUMENT_START_EVENT);
    parse_consume_type(&state, YAML_MAPPING_START_EVENT);

    if (!parse_config_list(&state)) {
        print_parse_error(&state, "Failed to parse configs");
        goto cleanup;
    }
    
cleanup:
    yaml_event_delete(&state.event);
    yaml_parser_delete(&state.parser);

    if (file) {
        fclose(file);
    }
}
