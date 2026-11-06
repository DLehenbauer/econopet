// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "tui_test.h"

#include "ui/tui.h"
#include "display/char_encoding.h"

static uint8_t screen_buffer[40 * 25];

static window_t init_window(void) {
    return window_create(screen_buffer, 40, 25);
}

START_TEST(test_tui_list_navigation) {
    static const char* const labels[] = { "ONE", "TWO", "THREE", "FOUR" };
    tui_component_t components[] = {
        {
            .kind = tui_component_list,
            .bounds = {1, 3, 38, 2},
            .list = {
                .labels = labels,
                .count = count_of(labels),
                .visible_rows = 2,
            },
        },
    };
    tui_screen_t screen = {
        .components = components,
        .component_count = count_of(components),
    };
    tui_controller_t controller;
    const window_t window = init_window();

    tui_controller_init(&controller, &window, &screen);
    tui_controller_dispatch(&controller, tui_action_down);
    tui_controller_dispatch(&controller, tui_action_down);
    ck_assert_uint_eq(components[0].list.selected, 2);
    ck_assert_uint_eq(components[0].list.first_visible, 1);

    tui_controller_dispatch(&controller, tui_action_page_down);
    ck_assert_uint_eq(components[0].list.selected, 3);
    tui_controller_dispatch(&controller, tui_action_down);
    ck_assert_uint_eq(components[0].list.selected, 0);
    ck_assert_uint_eq(components[0].list.first_visible, 0);
}
END_TEST

// Idle and no-op navigation preserve the painted screen without another clear.
START_TEST(test_tui_repaint_invalidation) {
    static const char* const labels[] = {"ONE", "TWO"};
    tui_component_t component = {
        .kind = tui_component_list,
        .bounds = {1, 3, 38, 2},
        .list = {.labels = labels, .count = count_of(labels), .visible_rows = 2}
    };
    tui_screen_t screen = {.components = &component, .component_count = 1};
    const window_t window = init_window();
    tui_controller_t controller;
    tui_controller_init(&controller, &window, &screen);
    ck_assert(tui_controller_needs_render(&controller));
    tui_controller_render(&controller);
    ck_assert(!tui_controller_needs_render(&controller));
    uint8_t painted[sizeof(screen_buffer)];
    memcpy(painted, screen_buffer, sizeof(painted));
    tui_controller_dispatch(&controller, tui_action_none);
    tui_controller_dispatch(&controller, tui_action_page_up);
    ck_assert(!tui_controller_needs_render(&controller));
    ck_assert_mem_eq(screen_buffer, painted, sizeof(painted));
    tui_controller_dispatch(&controller, tui_action_down);
    ck_assert(tui_controller_needs_render(&controller));
    tui_controller_render(&controller);
    ck_assert(!tui_controller_needs_render(&controller));
    tui_controller_pop(&controller);
    ck_assert(!tui_controller_needs_render(&controller));
}
END_TEST

// Returning from a child retains the originating component and list viewport.
START_TEST(test_tui_parent_focus_and_mixed_navigation) {
    static const char* const labels[] = {"ONE", "TWO", "THREE"};
    tui_component_t components[] = {
        {.kind = tui_component_button, .bounds = {1, 3, 38, 1}, .button = {.label = "MODEL"}},
        {.kind = tui_component_list, .bounds = {1, 5, 38, 2}, .list = {
            .labels = labels, .count = 3, .visible_rows = 2
        }},
        {.kind = tui_component_button, .bounds = {1, 8, 38, 1}, .button = {.label = "RESET"}},
    };
    tui_component_t child_component = {
        .kind = tui_component_button, .bounds = {1, 3, 38, 1}, .button = {.label = "BACK"}
    };
    tui_screen_t root = {.components = components, .component_count = count_of(components)};
    tui_screen_t child = {.components = &child_component, .component_count = 1};
    const window_t window = init_window();
    tui_controller_t controller;
    tui_controller_init(&controller, &window, &root);
    tui_controller_dispatch(&controller, tui_action_down);
    ck_assert_uint_eq(controller.focused_component, 1);
    tui_controller_dispatch(&controller, tui_action_down);
    tui_controller_dispatch(&controller, tui_action_down);
    ck_assert_uint_eq(components[1].list.selected, 2);
    ck_assert_uint_eq(components[1].list.first_visible, 1);
    tui_controller_render(&controller);
    ck_assert(!tui_controller_needs_render(&controller));
    tui_controller_push(&controller, &child);
    ck_assert(tui_controller_needs_render(&controller));
    tui_controller_render(&controller);
    tui_controller_pop(&controller);
    ck_assert(tui_controller_needs_render(&controller));
    ck_assert_uint_eq(controller.focused_component, 1);
    ck_assert_uint_eq(components[1].list.selected, 2);
    ck_assert_uint_eq(components[1].list.first_visible, 1);
    tui_controller_dispatch(&controller, tui_action_down);
    ck_assert_uint_eq(controller.focused_component, 2);
    tui_controller_dispatch(&controller, tui_action_up);
    ck_assert_uint_eq(controller.focused_component, 1);
    ck_assert_uint_eq(components[1].list.selected, 2);
}
END_TEST

// Explicit rectangles clip long labels and reverse only the focused row.
START_TEST(test_tui_positioned_render_is_bounded) {
    uint8_t guarded[32];
    memset(guarded, 0xa5, sizeof(guarded));
    const window_t window = window_create(guarded + 1, 6, 5);
    static const char* const labels[] = {"TOO LONG FOR ROW", "SECOND"};
    tui_component_t component = {
        .kind = tui_component_list, .bounds = {3, 3, 50, 50},
        .list = {.labels = labels, .count = 2, .visible_rows = 20}
    };
    tui_screen_t screen = {.components = &component, .component_count = 1};
    tui_controller_t controller;
    tui_controller_init(&controller, &window, &screen);
    tui_controller_render(&controller);
    ck_assert_uint_eq(guarded[0], 0xa5);
    ck_assert_uint_eq(guarded[31], 0xa5);
    for (size_t x = 3; x < 6; x++)
        ck_assert(guarded[1 + 3 * 6 + x] & 0x80);
    ck_assert_uint_eq(guarded[1 + 4 * 6 + 3], ascii_to_vrom('S'));
}
END_TEST

// Scrolling and page movement use the clipped viewport, not an oversized declaration.
START_TEST(test_tui_selection_remains_visible_when_clipped) {
    uint8_t guarded[32];
    memset(guarded, 0xa5, sizeof(guarded));
    const window_t window = window_create(guarded + 1, 6, 5);
    static const char* const labels[] = {"ONE", "TWO", "THREE", "FOUR", "FIVE"};
    tui_component_t component = {
        .kind = tui_component_list, .bounds = {0, 3, 6, _i == 0 ? 2 : 50},
        .list = {.labels = labels, .count = count_of(labels), .visible_rows = 20}
    };
    tui_screen_t screen = {.components = &component, .component_count = 1};
    tui_controller_t controller;
    tui_controller_init(&controller, &window, &screen);
    ck_assert_uint_eq(component.list.visible_rows, 2);
    for (size_t index = 1; index < count_of(labels); index++) {
        tui_controller_dispatch(&controller, tui_action_down);
        ck_assert_uint_lt(component.list.selected - component.list.first_visible, 2);
        tui_controller_render(&controller);
        const size_t row = 3 + component.list.selected - component.list.first_visible;
        ck_assert_uint_eq(guarded[1 + row * 6], ascii_to_vrom(labels[index][0]) ^ 0x80);
    }
    tui_controller_dispatch(&controller, tui_action_page_up);
    ck_assert_uint_eq(component.list.selected, 2);
    ck_assert_uint_eq(guarded[0], 0xa5);
    ck_assert_uint_eq(guarded[31], 0xa5);
}
END_TEST

static size_t loaded_first, page_loads;
static bool page_success;

// Models a failing or successful page source without rendering-time I/O.
static bool load_test_page(void* context, size_t first, size_t rows) {
    (void) context;
    ck_assert_uint_eq(rows, 2);
    loaded_first = first;
    page_loads++;
    return page_success;
}

// Borrows a label only when its page was requested first.
static const char* test_page_label(void* context, size_t index) {
    (void) context;
    ck_assert_uint_ge(index, loaded_first);
    ck_assert_uint_lt(index - loaded_first, 2);
    return "CACHED";
}

// Loads on scrolling, never while painting, and blocks activation after a failed page.
START_TEST(test_tui_page_provider) {
    tui_component_t component = {
        .kind = tui_component_list,
        .bounds = {1, 3, 38, 2},
        .list = {.count = 7, .visible_rows = 2, .load_page = load_test_page, .label_at = test_page_label}
    };
    tui_screen_t screen = {.components = &component, .component_count = 1};
    const window_t window = init_window();
    tui_controller_t controller;
    page_loads = 0;
    page_success = true;
    tui_controller_init(&controller, &window, &screen);
    ck_assert_uint_eq(page_loads, 1);
    tui_controller_render(&controller);
    tui_controller_render(&controller);
    ck_assert_uint_eq(page_loads, 1);
    tui_controller_dispatch(&controller, tui_action_down);
    ck_assert_uint_eq(page_loads, 1);
    tui_controller_dispatch(&controller, tui_action_down);
    ck_assert_uint_eq(loaded_first, 1);
    ck_assert_uint_eq(page_loads, 2);
    page_success = false;
    tui_controller_dispatch(&controller, tui_action_page_down);
    ck_assert(!component.list.page_valid);
    tui_controller_render(&controller);
}
END_TEST

static unsigned int button_activations;
static tui_screen_t second_screen;

static void push_second_screen(tui_controller_t* const controller, void* const context) {
    (void) context;
    button_activations++;
    tui_controller_push(controller, &second_screen);
}

static void count_button_activation(tui_controller_t* const controller, void* const context) {
    (void) controller;
    (void) context;
    button_activations++;
}

START_TEST(test_tui_buttons_and_screen_stack) {
    tui_component_t first_components[] = {
        {
            .kind = tui_component_button,
            .bounds = {1, 3, 38, 1},
            .button = {
                .label = "NEXT",
                .on_activate = push_second_screen,
            },
        },
    };
    tui_component_t second_components[] = {
        {
            .kind = tui_component_button,
            .bounds = {1, 3, 38, 1},
            .button = { .label = "BACK" },
        },
    };
    tui_screen_t first_screen = {
        .components = first_components,
        .component_count = count_of(first_components),
    };
    tui_controller_t controller;
    const window_t window = init_window();

    second_screen = (tui_screen_t) {
        .components = second_components,
        .component_count = count_of(second_components),
    };
    button_activations = 0;
    tui_controller_init(&controller, &window, &first_screen);

    tui_controller_dispatch(&controller, tui_action_activate);
    ck_assert_uint_eq(button_activations, 1);
    ck_assert_uint_eq(controller.screen_count, 2);

    tui_controller_dispatch(&controller, tui_action_back);
    ck_assert_uint_eq(controller.screen_count, 1);
    tui_controller_dispatch(&controller, tui_action_back);
    ck_assert(!tui_controller_is_active(&controller));
}
END_TEST

START_TEST(test_tui_empty_list_focuses_button) {
    tui_component_t components[] = {
        {
            .kind = tui_component_list,
            .bounds = {1, 3, 38, 2},
            .list = {
                .labels = NULL,
                .count = 0,
                .visible_rows = 2,
            },
        },
        {
            .kind = tui_component_button,
            .bounds = {1, 6, 38, 1},
            .button = {
                .label = "BACK",
                .on_activate = count_button_activation,
            },
        },
    };
    tui_screen_t screen = {
        .components = components,
        .component_count = count_of(components),
    };
    tui_controller_t controller;
    const window_t window = init_window();

    button_activations = 0;
    tui_controller_init(&controller, &window, &screen);
    ck_assert_uint_eq(controller.focused_component, 1);
    tui_controller_dispatch(&controller, tui_action_activate);
    ck_assert_uint_eq(button_activations, 1);
}
END_TEST

Suite* tui_suite(void) {
    Suite* const suite = suite_create("tui");
    TCase* const test_case = tcase_create("core");

    tcase_add_test(test_case, test_tui_list_navigation);
    tcase_add_test(test_case, test_tui_buttons_and_screen_stack);
    tcase_add_test(test_case, test_tui_empty_list_focuses_button);
    tcase_add_test(test_case, test_tui_parent_focus_and_mixed_navigation);
    tcase_add_test(test_case, test_tui_positioned_render_is_bounded);
    tcase_add_loop_test(test_case, test_tui_selection_remains_visible_when_clipped, 0, 2);
    tcase_add_test(test_case, test_tui_page_provider);
    tcase_add_test(test_case, test_tui_repaint_invalidation);
    suite_add_tcase(suite, test_case);
    return suite;
}
