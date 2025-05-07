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
#include "sd/sd.h"

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

static bool assert_yaml_type(parser_state_t* state, yaml_event_type_t expected_type) {
    bool result = state->event.type == expected_type;

    if (!result) {
        print_parse_error(state, "Expected event type %d, but got %d", expected_type, state->event.type);
        goto cleanup;
    }

cleanup:
    return result;
}

static bool parse_expect_type(parser_state_t* state, yaml_event_type_t expected_type) {
    bool result;
    
    if (!(result = parse_next(state))) { goto cleanup; }
    if (!assert_yaml_type(state, expected_type)) { goto cleanup; }

cleanup:
    return result;
}

static const char* get_current_string(parser_state_t* state) {
    assert_yaml_type(state, YAML_SCALAR_EVENT);
    return (const char*) state->event.data.scalar.value;
}

// Helper function to parse a scalar value
static bool parse_string(parser_state_t* state, char* buffer, size_t buffer_size) {
    bool result;

    if (!(result = parse_expect_type(state, YAML_SCALAR_EVENT))) { goto cleanup;}

    strncpy(buffer, get_current_string(state), buffer_size - 1);
    buffer[buffer_size - 1] = '\0';

cleanup:
    return result;
}

static bool parse_as_string(parser_state_t* state, void* context, size_t context_size) {
    return parse_string(state, (char*) context, context_size);
}

static bool parse_uint32(parser_state_t* state, uint32_t* value) {
    bool result;
    char buffer[32] = { 0 };

    if (!(result = parse_string(state, buffer, sizeof(buffer)))) { goto cleanup; }

    char* endptr = NULL;
    *value = strtoul(buffer, &endptr, 0);
    if (!(result = *endptr == '\0')) {
        print_parse_error(state, "Expected unsigned integer, but got '%s'\n", buffer);
        goto cleanup;
    }

cleanup:
    return result;
}

static bool parse_as_uint32(parser_state_t* state, void* context, size_t context_size) {
    assert(context_size == sizeof(uint32_t));
    return parse_uint32(state, (uint32_t*) context);
}

typedef bool (*map_dispatch_fn_t)(parser_state_t* state, void* context, size_t context_size);

typedef struct map_dipatch_entry_s {
    const char* key;
    const map_dispatch_fn_t fn;
    void* context;
    size_t context_size;
} map_dispatch_entry_t;

static bool parse_mapping_continued(parser_state_t* state, const map_dispatch_entry_t dispatch[]) {
    bool result;

    while (true) {
        if (!(result = parse_next(state))) { goto cleanup; }
        else if (state->event.type == YAML_MAPPING_END_EVENT) { goto cleanup; }
        else if (state->event.type == YAML_SCALAR_EVENT) {
            const char* const key = (const char*) state->event.data.scalar.value;

            // Scan the dispatch table for a matching key
            const map_dispatch_entry_t* pEntry = &dispatch[0];
            while (true) {
                if (pEntry->key == NULL) {
                    print_parse_error(state, "Unknown key '%s'", key);
                    goto cleanup;
                } else if (strcmp(key, pEntry->key) == 0) {
                    if (!(result = pEntry->fn(state, pEntry->context, pEntry->context_size))) { goto cleanup; }
                    break;
                } else {
                    pEntry++;
                }
            }
        }
    }

cleanup:
    return result;
}


static bool parse_mapping(parser_state_t* state, const map_dispatch_entry_t dispatch[]) {
    assert_yaml_type(state, YAML_MAPPING_START_EVENT);
    return parse_mapping_continued(state, dispatch);
}

static bool parse_sequence(parser_state_t* state, parse_callback_t on_item_fn) {
    bool result;

    assert_yaml_type(state, YAML_SEQUENCE_START_EVENT);

    while (true) {
        if (!(result = parse_next(state))) { goto cleanup; }
        if (state->event.type == YAML_SEQUENCE_END_EVENT) { goto cleanup; }

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

static bool parse_action_load_file_entry(parser_state_t* state) {
    bool result;

    char file[261] = { 0 };     // Windows OS max path length is 260 characters
    uint32_t address = 0;
    
    if (!(result = parse_mapping(state, (const map_dispatch_entry_t[]) {
        { "file", parse_as_string, &file, sizeof(file) },
        { "address", parse_as_uint32, &address, sizeof(address) },
        { NULL, NULL, NULL, 0 }
    }))) { goto cleanup; }

    if (state->sink->on_action_load) {
        state->sink->on_action_load(state->sink->user_data, file, address);
    }

cleanup:
    return result;
}

static bool parse_action_load_files(parser_state_t* state, void* context, size_t context_size) {
    (void)context;
    (void)context_size;

    bool result;
    
    if (!(result = parse_expect_type(state, YAML_SEQUENCE_START_EVENT))) { goto cleanup; }
    if (!(result = parse_sequence(state, parse_action_load_file_entry))) { goto cleanup; }

cleanup:
    return result;
}

static bool parse_action_load(parser_state_t* state, void* context, size_t context_size) {
    (void)context;
    (void)context_size;
    
    bool result;

    if (!(result = parse_mapping_continued(state, (const map_dispatch_entry_t[]) {
        { "files", parse_action_load_files, NULL, 0 },
        { NULL, NULL, NULL, 0 }
    }))) { goto cleanup; }

cleanup:
    return result;
}

static bool parse_action(parser_state_t* state, void* context, size_t context_size) {
    (void)context;
    (void)context_size;
    
    bool result;

    if (!(result = parse_next(state))) { goto cleanup; }
    const char* action = get_current_string(state);

    if (strcmp(action, "load") == 0) {
        if (!(result = parse_action_load(state, NULL, 0))) { goto cleanup; }
    } else {
        print_parse_error(state, "Unknown action '%s'", action);
        result = false;
        goto cleanup;
    }

cleanup:
    return result;
}

static bool parse_action_list_entry(parser_state_t* state) {
    bool result;
    char action[32] = { 0 };

    assert_yaml_type(state, YAML_MAPPING_START_EVENT);
    if (!(result = parse_string(state, action, sizeof(action)))) { goto cleanup; }
    if (strcmp(action, "action") == 0) {
        if (!(result = parse_action(state, NULL, 0))) { goto cleanup; }
    } else {
        print_parse_error(state, "Unknown action '%s'", action);
        result = false;
        goto cleanup;
    }

cleanup:
    return result;
}

// Helper function to parse a list of actions
static bool parse_action_list(parser_state_t* state, void* context, size_t context_size) {
    (void)context;
    (void)context_size;

    bool result;
    
    if (!(result = parse_expect_type(state, YAML_SEQUENCE_START_EVENT))) { goto cleanup; }
    if (!(result = parse_sequence(state, parse_action_list_entry))) { goto cleanup; }
    
cleanup:
    return result;
}

typedef struct parse_config_context_s {
    char name[80];
} parse_config_context_t;

// Helper function to parse an individual config
static bool parse_config(parser_state_t* state) {
    if (state->sink->on_enter_config) {
        state->sink->on_enter_config(state->sink->user_data);
    }

    bool result;
    char name[41] = { 0 };

    if (!(result = parse_mapping(state, (const map_dispatch_entry_t[]) {
        { "name", parse_as_string, &name, sizeof(name) },
        { "setup", parse_action_list, NULL, 0 },
        { NULL, NULL, NULL, 0 }
    }))) { goto cleanup; }

    if (state->sink->on_exit_config) {
        state->sink->on_exit_config(state->sink->user_data, name);
    }

cleanup:
    return result;
}

// Helper function to parse a list of configs
static bool parse_config_list(parser_state_t* state, void* context, size_t context_size) {
    (void)context;
    (void)context_size;

    bool result;
    
    if (!(result = parse_expect_type(state, YAML_SEQUENCE_START_EVENT))) { goto cleanup; }
    if (!(result = parse_sequence(state, parse_config))) { goto cleanup; }
    
cleanup:
    return result;
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

    file = sd_open(filename, "r");
    if (!file) {
        print_error("Failed to open file '%s'", filename);
        goto cleanup;
    }

    yaml_parser_set_input_file(&state.parser, file);

    if (!parse_expect_type(&state, YAML_STREAM_START_EVENT)) { goto cleanup; }
    if (!parse_expect_type(&state, YAML_DOCUMENT_START_EVENT)) { goto cleanup; }
    if (!parse_expect_type(&state, YAML_MAPPING_START_EVENT)) { goto cleanup; }
    
    parse_mapping(&state, (const map_dispatch_entry_t[]) {
        { "configs", parse_config_list, NULL, 0 },
        { NULL, NULL, NULL, 0 }
    });

cleanup:
    yaml_event_delete(&state.event);
    yaml_parser_delete(&state.parser);

    if (file) {
        fclose(file);
    }
}
