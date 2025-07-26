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
    FILE* file;
    const config_sink_t* sink;
    struct parser_s* previous;
    int depth;
} parser_t;

typedef void (*parse_callback_t)(parser_t* parser);

static void vfatal_parse_error(parser_t* parser, const char* format, va_list args) {
    const window_t window = window_create(video_char_buffer, 40, 25);
    term_begin(&window);
    window_fill(&window, CH_SPACE);

    uint8_t* pOut = window_println(&window, window.start, "E: error parsing file:");
    window_reverse(&window, window.start, 2);
    pOut = window_println(&window, pOut, "'%s'", parser->filename);
    pOut = window_println(&window, pOut, "");
    
    if (parser->parser.problem != NULL) {
        pOut = window_println(&window, pOut,
            "Line %zu, Column %zu: ",
            parser->parser.problem_mark.line + 1,
            parser->parser.problem_mark.column + 1);
        pOut = window_println(&window, pOut,
            parser->parser.problem);
    } else {
        pOut = window_println(&window, pOut,
            "Line %zu, Column %zu: ",
            parser->event.start_mark.line + 1,
            parser->event.start_mark.column + 1);
    }

    pOut = window_vprintln(&window, pOut, format, args);

    term_display(&window);
    abort();
}

void fatal_parse_error(parser_t* parser, const char* const format, ...) {
    va_list args;
    va_start(args, format);
    vfatal_parse_error(parser, format, args);
    va_end(args);
}

void vet_parser(parser_t* parser, bool condition, const char* const format, ...) {
    if (!condition) {
        va_list args;
        va_start(args, format);
        vfatal_parse_error(parser, format, args);
        va_end(args);
    }
}

static void on_action_load(const parser_t* const parser, const char* file, uint32_t address) {
    if (parser->sink->setup && parser->sink->setup->on_load) {
        parser->sink->setup->on_load(parser->sink->setup->context, file, address);
    }
}

static void on_action_patch(const parser_t* const parser, uint32_t address, const binary_t* binary) {
    if (parser->sink->setup && parser->sink->setup->on_patch) {
        parser->sink->setup->on_patch(parser->sink->setup->context, address, binary);
    }
}

static void on_action_copy(const parser_t* const parser, uint32_t source, uint32_t destination, uint32_t length) {
    if (parser->sink->setup && parser->sink->setup->on_copy) {
        parser->sink->setup->on_copy(parser->sink->setup->context, source, destination, length);
    }
}

static void on_action_fix_checksum(const parser_t* const parser, uint32_t start_addr, uint32_t end_addr, uint32_t fix_addr, uint32_t checksum) {
    if (parser->sink->setup && parser->sink->setup->on_fix_checksum) {
        parser->sink->setup->on_fix_checksum(parser->sink->setup->context, start_addr, end_addr, fix_addr, checksum);
    }
}

static model_t* get_model(const parser_t* const parser) {
    return parser->sink->setup ? parser->sink->setup->model : NULL;
}

static model_flags_t get_model_flags(const parser_t* const parser) {
    const model_t* model = get_model(parser);
    return model ? model->flags : model_flag_none;
}

static yaml_char_t* get_current_anchor(parser_t* parser) {
    switch (parser->event.type) {
        case YAML_MAPPING_START_EVENT: return parser->event.data.mapping_start.anchor;
        case YAML_SEQUENCE_START_EVENT: return parser->event.data.sequence_start.anchor;
        case YAML_SCALAR_EVENT: return parser->event.data.scalar.anchor;
        default: return NULL;
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

static void push_parser(parser_t* parser);
static void pop_parser(parser_t* parser);

static void mark_parser_depth(parser_t* parser) {
    switch (parser->event.type) {
        case YAML_STREAM_START_EVENT:
        case YAML_DOCUMENT_START_EVENT:
        case YAML_MAPPING_START_EVENT:
        case YAML_SEQUENCE_START_EVENT: {
            parser->depth = 0;
            break;
        }

        default: {
            parser->depth = -1;
            break;
        }
    }
}

static void vetted_yaml_parse_next(parser_t* parser) {
    if (!yaml_parser_parse(&parser->parser, &parser->event)) {
        fatal_parse_error(parser, "(YAML is malformed)");
    }
}

static void parse_next(parser_t* parser) {
    if (parser->depth < 0) {
        pop_parser(parser);
        assert_yaml_type(parser, YAML_ALIAS_EVENT);
    }

    yaml_event_delete(&parser->event);
    vetted_yaml_parse_next(parser);

    switch (parser->event.type) {
        case YAML_STREAM_START_EVENT: { parser->depth++; break; }
        case YAML_STREAM_END_EVENT: { parser->depth--; break; }

        case YAML_DOCUMENT_START_EVENT: { parser->depth++; break; }
        case YAML_DOCUMENT_END_EVENT: { parser->depth--; break; }

        case YAML_MAPPING_START_EVENT: { parser->depth++; break; }
        case YAML_MAPPING_END_EVENT: { parser->depth--; break; }

        case YAML_SEQUENCE_START_EVENT: { parser->depth++; break; }
        case YAML_SEQUENCE_END_EVENT: { parser->depth--; break; }

        case YAML_ALIAS_EVENT: {
            const yaml_char_t* target_anchor = parser->event.data.alias.anchor;
            
            push_parser(parser);
            
            while (true) {
                const yaml_char_t* current_anchor = get_current_anchor(parser);
                
                if (current_anchor != NULL &&
                    strcmp((const char*) target_anchor, (const char*) current_anchor) == 0) {
                    mark_parser_depth(parser);
                    break;
                }
                
                parse_next(parser);
            }
            break;
        }

        default: break;
    }
}

static void parse_expect_type(parser_t* parser, yaml_event_type_t expected_type) {
    parse_next(parser);
    assert_yaml_type(parser, expected_type);
}

static void init_parser(parser_t* parser, const char* filename, const config_sink_t* const sink) {
    memset(parser, 0, sizeof(parser_t));

    parser->filename = filename;
    parser->sink = sink;
    parser->depth = 0;

    vet_parser(parser, yaml_parser_initialize(&parser->parser), "yaml_parser_initialize failed");

    parser->file = sd_open(filename, "r");
    vet_parser(parser, parser->file != NULL, "file '%s' not found", filename);

    yaml_parser_set_input_file(&parser->parser, parser->file);

    parse_expect_type(parser, YAML_STREAM_START_EVENT);
    parse_expect_type(parser, YAML_DOCUMENT_START_EVENT);
}

static void deinit_parser(parser_t* parser) {
    vet_parser(parser, parser->previous == NULL, "deinit_parser with non-empty stack");
    vet_parser(parser, parser->depth == 0, "deinit_parser depth=%d", parser->depth);
    yaml_event_delete(&parser->event);
    yaml_parser_delete(&parser->parser);
    fclose((FILE*) parser->file);
}

static void push_parser(parser_t* parser) {
    parser_t* saved_parser = vetted_malloc(sizeof(parser_t));
    memcpy(/* dest: */ saved_parser, /* src: */ parser, sizeof(parser_t));
    init_parser(parser, saved_parser->filename, saved_parser->sink);
    parser->previous = saved_parser;
}

static void pop_parser(parser_t* parser) {
    vet_parser(parser->previous, parser->depth == -1, "unbalanced yaml (depth=%d)", parser->depth);

    parser_t* previous_parser = parser->previous;
    vet_parser(parser, previous_parser != NULL, "pop_parser with empty stack");
    parser->previous = NULL;
    
    parser->depth = 0;
    deinit_parser(parser);
    memcpy(/* dest: */ parser, /* src: */ previous_parser, sizeof(parser_t));
    free(previous_parser);
}

static void parse_skip(parser_t* parser, void* context, size_t context_size) {
    (void) context;
    (void) context_size;

    int start_depth = parser->depth;
    parse_next(parser);

    switch (parser->event.type) {
        case YAML_MAPPING_START_EVENT:
        case YAML_SEQUENCE_START_EVENT: {
            assert (parser->depth > start_depth);
            
            while (parser->depth > start_depth) {
                parse_next(parser);
            }
            break;
        }

        case YAML_ALIAS_EVENT:
        case YAML_SCALAR_EVENT:
            break;

        default:
            fatal_parse_error(parser, "cannot skip %s",
                type_to_string(parser->event.type));
    }
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
    parse_expect_type(parser, YAML_SCALAR_EVENT);
    const char* buffer = get_current_string(parser);

    char* endptr = NULL;
    *value = strtoul(buffer, &endptr, 0);
    if (endptr == buffer || *endptr != '\0') {
        fatal_parse_error(parser, "not an unsigned integer: '%s'", buffer);
    }
}

static void parse_as_uint32(parser_t* parser, void* context, size_t context_size) {
    assert(context_size == sizeof(uint32_t));
    return parse_uint32(parser, (uint32_t*) context);
}

static void parse_as_hex(parser_t* parser, void* context, size_t context_size) {
    parse_expect_type(parser, YAML_SCALAR_EVENT);
    assert(context_size == sizeof(binary_t));

    binary_t* binary = (binary_t*) context;
    if (binary->size != 0) {
        fatal_parse_error(parser, "duplicate binary/hex entry");
    }

    const char* str = get_current_string(parser);

    size_t len = strlen(str);
    if (binary->expected != 0 && len != binary->expected * 2) {
        fatal_parse_error(parser, "expected %zu chars, got %zu", binary->expected, len);
    } else if (len % 2 != 0) {
        fatal_parse_error(parser, "hex string must have even length");
    }

    binary->size = len / 2;
    if (binary->size > binary->capacity) {
        fatal_parse_error(parser, "hex string exceeds expected size");
    }

    for (size_t write_index = 0, read_index = 0; write_index < binary->size; write_index++) {
        char hex_byte[3] = { str[read_index++], str[read_index++], '\0' };
        char* endptr = NULL;
        long value = strtol(hex_byte, &endptr, 16);

        if (*endptr != '\0') {
            fatal_parse_error(parser, "Invalid hex '%s'", hex_byte);
        }

        binary->data[write_index] = (uint8_t) value;
    }
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
        else {
            assert_yaml_type(parser, YAML_SCALAR_EVENT);

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

    on_action_load(parser, file, address);
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

static void parse_action_patch(parser_t* parser, void* context, size_t context_size) {
    (void)context;
    (void)context_size;

    uint32_t address = 0;

    uint8_t* binary_data = acquire_temp_buffer();
    binary_t binary = {
        .data = binary_data,
        .size = 0,
        .expected = 0,
        .capacity = TEMP_BUFFER_SIZE
    };

    parse_mapping_continued(parser, (const map_dispatch_entry_t[]) {
        { "address", parse_as_uint32, &address, sizeof(uint32_t) },
        { "hex", parse_as_hex, &binary, sizeof(binary) },
        { NULL, NULL, NULL, 0 }
    });

    on_action_patch(parser, address, &binary);
    release_temp_buffer(&binary_data);
}

static void parse_action_copy(parser_t* parser, void* context, size_t context_size) {
    (void)context;
    (void)context_size;

    uint32_t source = 0;
    uint32_t destination = 0;
    uint32_t length = 0;

    parse_mapping_continued(parser, (const map_dispatch_entry_t[]) {
        { "source", parse_as_uint32, &source, sizeof(uint32_t) },
        { "destination", parse_as_uint32, &destination, sizeof(uint32_t) },
        { "length", parse_as_uint32, &length, sizeof(uint32_t) },
        { NULL, NULL, NULL, 0 }
    });

    on_action_copy(parser, source, destination, length);
}

static void parse_action_set_usb_keymap(parser_t* parser, void* context, size_t context_size) {
    (void)context;
    (void)context_size;

    char filename[261] = { 0 };  // Windows OS max path length is 260 characters

    parse_mapping_continued(parser, (const map_dispatch_entry_t[]) {
        { "file", parse_as_string, &filename, sizeof(filename) },
        { NULL, NULL, NULL, 0 }
    });

    if (parser->sink->setup && parser->sink->setup->on_set_keymap) {
        parser->sink->setup->on_set_keymap(parser->sink->setup->context, filename);
    }
}

static void parse_action_set(parser_t* parser, void* context, size_t context_size) {
    (void)context;
    (void)context_size;

    options_t options = {
        .columns = 40,  // Default value
    };

    parse_mapping_continued(parser, (const map_dispatch_entry_t[]) {
        { "columns", parse_as_uint32, &options.columns, sizeof(options.columns) },
        { NULL, NULL, NULL, 0 }
    });

    if (options.columns != 40 && options.columns != 80) {
        fatal_parse_error(parser, "Invalid number of columns: %u (must be 40 or 80)", options.columns);
    }

    if (parser->sink->setup && parser->sink->setup->on_set_options) {
        parser->sink->setup->on_set_options(parser->sink->setup->context, &options);
    }
}

static void parse_action_fix_checksum(parser_t* parser, void* context, size_t context_size) {
    (void)context;
    (void)context_size;

    uint32_t start_addr = 0;
    uint32_t end_addr = 0;
    uint32_t fix_addr = 0;
    uint32_t checksum = 0;

    parse_mapping_continued(parser, (const map_dispatch_entry_t[]) {
        { "start-addr", parse_as_uint32, &start_addr, sizeof(uint32_t) },
        { "end-addr", parse_as_uint32, &end_addr, sizeof(uint32_t) },
        { "fix-addr", parse_as_uint32, &fix_addr, sizeof(uint32_t) },
        { "checksum", parse_as_uint32, &checksum, sizeof(uint32_t) },
        { NULL, NULL, NULL, 0 }
    });

    on_action_fix_checksum(parser, start_addr, end_addr, fix_addr, checksum);
}

static void parse_action(parser_t* parser, void* context, size_t context_size) {
    (void)context;
    (void)context_size;
    
    parse_next(parser);
    const char* action = get_current_string(parser);

    if (strcmp(action, "load") == 0) {
        parse_action_load(parser, NULL, 0);
    } else if (strcmp(action, "patch") == 0) {
        parse_action_patch(parser, NULL, 0);
    } else if (strcmp(action, "copy") == 0) {
        parse_action_copy(parser, NULL, 0);
    } else if (strcmp(action, "set-usb-keymap") == 0) {
        parse_action_set_usb_keymap(parser, NULL, 0);
    } else if (strcmp(action, "set") == 0) {
        parse_action_set(parser, NULL, 0);
    } else if (strcmp(action, "fix-checksum") == 0) {
        parse_action_fix_checksum(parser, NULL, 0);
    } else {
        fatal_parse_error(parser, "Unknown action '%s'", action);
    }
}

static void parse_action_list(parser_t* parser, void* context, size_t context_size);

static void parse_then_else(parser_t* parser, void* context, size_t context_size) {
    assert(context_size == sizeof(bool));
    bool condition = *((bool*)context);

    if (condition) {
        parse_action_list(parser, NULL, 0);
    } else {
        //assert_yaml_type(parser, YAML_SEQUENCE_START_EVENT);
        parse_skip(parser, NULL, 0);
    }
}

static void parse_if(parser_t* parser, void* context, size_t context_size) {
    (void)context;
    (void)context_size;
    
    parse_next(parser);
    const char* condition = get_current_string(parser);
    bool then_value = false;

    if (strcmp(condition, "graphics") == 0) {
        then_value = (get_model_flags(parser) & model_flag_business) == 0;
    } else {
        fatal_parse_error(parser, "Unknown if-cond '%s'", condition);
    }

    bool else_value = !then_value;

    parse_mapping_continued(parser, (const map_dispatch_entry_t[]) {
        { "then", parse_then_else, &then_value, sizeof(then_value) },
        { "else", parse_then_else, &else_value, sizeof(else_value) },
        { NULL, NULL, NULL, 0 }
    });
}

static void parse_action_list_entry(parser_t* parser) {
    assert_yaml_type(parser, YAML_MAPPING_START_EVENT);

    parse_next(parser);
    const char* action = get_current_string(parser);

    if (strcmp(action, "action") == 0) {
        parse_action(parser, NULL, 0);
    } else if (strcmp(action, "if") == 0) {
        parse_if(parser, NULL, 0);
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
        parser->sink->on_enter_config(parser->sink->context);
    }

    char name[41] = { 0 };

    parse_mapping(parser, (const map_dispatch_entry_t[]) {
        { "name", parse_as_string, &name, sizeof(name) },
        { "setup", parse_action_list, NULL, 0 },
        { NULL, NULL, NULL, 0 }
    });

    if (parser->sink->on_exit_config) {
        parser->sink->on_exit_config(parser->sink->context, name);
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
    parser_t parser;

    init_parser(&parser, filename, sink);

    parse_expect_type(&parser, YAML_MAPPING_START_EVENT);
    
    parse_mapping(&parser, (const map_dispatch_entry_t[]) {
        { "configs", parse_config_list, NULL, 0 },
        { "data", parse_skip, NULL, 0 },
        { NULL, NULL, NULL, 0 }
    });

    parse_expect_type(&parser, YAML_DOCUMENT_END_EVENT);
    parse_expect_type(&parser, YAML_STREAM_END_EVENT);

    deinit_parser(&parser);
}
