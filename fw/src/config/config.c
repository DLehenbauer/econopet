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
#include "fatal.h"
#include "global.h"
#include "sd/sd.h"
#include "term.h"
#include "video/video.h"

typedef struct parser_s {
    yaml_parser_t parser;
    yaml_event_t event;
    const char* filename;
    const config_sink_t* const sink;
} parser_t;

typedef void (*parse_callback_t)(parser_t* parser);

static void fatal_parse_error(parser_t* parser, const char* format, ...) {
    const window_t* const window = parser->sink->window;

    window_fill(window, CH_SPACE);
    uint8_t* pOut = window_println(window, window->start, "Error parsing YAML file:");
    pOut = window_println(window, pOut, "'%s'", parser->filename);
    pOut = window_println(window, pOut, "");
    
    if (parser->parser.problem != NULL) {
        pOut = window_println(window, pOut,
            "Line %zu, Column %zu: ",
            parser->parser.problem_mark.line + 1,
            parser->parser.problem_mark.column + 1);
        pOut = window_println(window, pOut,
            parser->parser.problem);
    } else {
        pOut = window_println(window, pOut,
            "Line %zu, Column %zu: ",
            parser->event.start_mark.line + 1,
            parser->event.start_mark.column + 1);
    }

    va_list args;
    va_start(args, format);
    pOut = window_vprintln(window, pOut, format, args);
    va_end(args);

    term_display(window);
    abort();
}

static void parse_next(parser_t* parser) {
    yaml_event_delete(&parser->event);
    if (!yaml_parser_parse(&parser->parser, &parser->event)) {
        fatal_parse_error(parser, "(YAML is malformed)");
    }
}

static char* type_to_string(yaml_event_type_t type) {
    switch (type) {
        case YAML_NO_EVENT: return "(none)";
        case YAML_STREAM_START_EVENT: return "stream start";
        case YAML_STREAM_END_EVENT: return "stream end";
        case YAML_DOCUMENT_START_EVENT: return "document start";
        case YAML_DOCUMENT_END_EVENT: return "document end";
        case YAML_MAPPING_START_EVENT: return "mapping start";
        case YAML_MAPPING_END_EVENT: return "mapping end";
        case YAML_SEQUENCE_START_EVENT: return "sequence start";
        case YAML_SEQUENCE_END_EVENT: return "sequence end";
        case YAML_SCALAR_EVENT: return "scalar";
        case YAML_ALIAS_EVENT: return "alias";

        default: return "(unknown)";
    }
}

static void assert_yaml_type(parser_t* parser, yaml_event_type_t expected_type) {
    if (parser->event.type != expected_type) {
        fatal_parse_error(parser, "Expected %s, but got %s",
            type_to_string(expected_type),
            type_to_string(parser->event.type));
    }
}

static void parse_expect_type(parser_t* parser, yaml_event_type_t expected_type) {
    parse_next(parser);
    assert_yaml_type(parser, expected_type);
}

static const char* get_current_string(parser_t* state) {
    assert_yaml_type(state, YAML_SCALAR_EVENT);
    return (const char*) state->event.data.scalar.value;
}

// Helper function to parse a scalar value
static void parse_string(parser_t* parser, char* buffer, size_t buffer_size) {
    parse_expect_type(parser, YAML_SCALAR_EVENT);
    strncpy(buffer, get_current_string(parser), buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
}

static void parse_as_string(parser_t* parser, void* context, size_t context_size) {
    return parse_string(parser, (char*) context, context_size);
}

static void parse_uint32(parser_t* parser, uint32_t* value) {
    char buffer[32] = { 0 };

    parse_string(parser, buffer, sizeof(buffer));

    char* endptr = NULL;
    *value = strtoul(buffer, &endptr, 0);
    if (*endptr != '\0') {
        fatal_parse_error(parser, "'%s' is not an unsigned integer\n", buffer);
    }
}

static void parse_as_uint32(parser_t* parser, void* context, size_t context_size) {
    assert(context_size == sizeof(uint32_t));
    return parse_uint32(parser, (uint32_t*) context);
}

typedef void (*map_dispatch_fn_t)(parser_t* parser, void* context, size_t context_size);

typedef struct map_dipatch_entry_s {
    const char* key;
    const map_dispatch_fn_t fn;
    void* context;
    size_t context_size;
} map_dispatch_entry_t;

static void parse_mapping_continued(parser_t* parser, const map_dispatch_entry_t dispatch[]) {
    while (true) {
        parse_next(parser);

        if (parser->event.type == YAML_MAPPING_END_EVENT) { return; }
        else if (parser->event.type == YAML_SCALAR_EVENT) {
            const char* const key = (const char*) parser->event.data.scalar.value;

            // Scan the dispatch table for a matching key
            const map_dispatch_entry_t* pEntry = &dispatch[0];
            while (true) {
                if (pEntry->key == NULL) {
                    fatal_parse_error(parser, "Unknown key '%s'", key);
                } else if (strcmp(key, pEntry->key) == 0) {
                    pEntry->fn(parser, pEntry->context, pEntry->context_size);
                    break;
                } else {
                    pEntry++;
                }
            }
        }
    }
}


static void parse_mapping(parser_t* parser, const map_dispatch_entry_t dispatch[]) {
    assert_yaml_type(parser, YAML_MAPPING_START_EVENT);
    parse_mapping_continued(parser, dispatch);
}

static void parse_sequence(parser_t* parser, parse_callback_t on_item_fn) {
    assert_yaml_type(parser, YAML_SEQUENCE_START_EVENT);

    while (true) {
        parse_next(parser);
        
        if (parser->event.type == YAML_SEQUENCE_END_EVENT) { return; }
        else { on_item_fn(parser); }
    }
}

static void parse_action_load_file_entry(parser_t* parser) {
    char file[261] = { 0 };     // Windows OS max path length is 260 characters
    uint32_t address = 0;
    
    parse_mapping(parser, (const map_dispatch_entry_t[]) {
        { "file", parse_as_string, &file, sizeof(file) },
        { "address", parse_as_uint32, &address, sizeof(address) },
        { NULL, NULL, NULL, 0 }
    });

    if (parser->sink->on_action_load) {
        parser->sink->on_action_load(parser->sink->user_data, file, address);
    }
}

static void parse_action_load_files(parser_t* parser, void* context, size_t context_size) {
    (void)context;
    (void)context_size;

    parse_expect_type(parser, YAML_SEQUENCE_START_EVENT);
    parse_sequence(parser, parse_action_load_file_entry);
}

static void parse_action_load(parser_t* parser, void* context, size_t context_size) {
    (void)context;
    (void)context_size;
    
    parse_mapping_continued(parser, (const map_dispatch_entry_t[]) {
        { "files", parse_action_load_files, NULL, 0 },
        { NULL, NULL, NULL, 0 }
    });
}

static void parse_action(parser_t* parser, void* context, size_t context_size) {
    (void)context;
    (void)context_size;
    
    parse_next(parser);
    const char* action = get_current_string(parser);

    if (strcmp(action, "load") == 0) {
        parse_action_load(parser, NULL, 0);
    } else {
        fatal_parse_error(parser, "Unknown action '%s'", action);
    }
}

static void parse_action_list_entry(parser_t* parser) {
    char action[32] = { 0 };

    assert_yaml_type(parser, YAML_MAPPING_START_EVENT);
    parse_string(parser, action, sizeof(action));
    if (strcmp(action, "action") == 0) {
        parse_action(parser, NULL, 0);
    } else {
        fatal_parse_error(parser, "Unknown action '%s'", action);
    }
}

// Helper function to parse a list of actions
static void parse_action_list(parser_t* parser, void* context, size_t context_size) {
    (void)context;
    (void)context_size;

    parse_expect_type(parser, YAML_SEQUENCE_START_EVENT);
    parse_sequence(parser, parse_action_list_entry);
}

typedef struct parse_config_context_s {
    char name[80];
} parse_config_context_t;

// Helper function to parse an individual config
static void parse_config(parser_t* parser) {
    if (parser->sink->on_enter_config) {
        parser->sink->on_enter_config(parser->sink->user_data);
    }

    char name[41] = { 0 };

    parse_mapping(parser, (const map_dispatch_entry_t[]) {
        { "name", parse_as_string, &name, sizeof(name) },
        { "setup", parse_action_list, NULL, 0 },
        { NULL, NULL, NULL, 0 }
    });

    if (parser->sink->on_exit_config) {
        parser->sink->on_exit_config(parser->sink->user_data, name);
    }
}

// Helper function to parse a list of configs
static void parse_config_list(parser_t* parser, void* context, size_t context_size) {
    (void)context;
    (void)context_size;

    parse_expect_type(parser, YAML_SEQUENCE_START_EVENT);
    parse_sequence(parser, parse_config);
}

void parse_config_file(const char* filename, const config_sink_t* const sink) {
    FILE* file = NULL;

    parser_t parser = {
        .parser = { 0 },
        .event = { 0 },
        .filename = filename,
        .sink = sink,
    };

    if (!yaml_parser_initialize(&parser.parser)) {
        fatal("Failed to initialize YAML parser");
    }

    file = sd_open(filename, "r");
    if (!file) {
        fatal("Failed to open '%s'", filename);
    }

    yaml_parser_set_input_file(&parser.parser, file);

    parse_expect_type(&parser, YAML_STREAM_START_EVENT);
    parse_expect_type(&parser, YAML_DOCUMENT_START_EVENT);
    parse_expect_type(&parser, YAML_MAPPING_START_EVENT);
    
    parse_mapping(&parser, (const map_dispatch_entry_t[]) {
        { "configs", parse_config_list, NULL, 0 },
        { NULL, NULL, NULL, 0 }
    });

    yaml_event_delete(&parser.event);
    yaml_parser_delete(&parser.parser);

    if (file) {
        fclose(file);
    }
}
