// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "ui/tui.h"

#include "display/char_encoding.h"
#include "fatal.h"

// Returns the borrowed screen at the top of the hierarchy.
static tui_screen_t* active_screen(const tui_controller_t* const controller) {
    assert(controller->screen_count > 0);
    return controller->screens[controller->screen_count - 1];
}

// Returns the current navigation target.
static tui_component_t* focused_component(const tui_controller_t* const controller) {
    tui_screen_t* const screen = active_screen(controller);
    assert(controller->has_focus);
    assert(controller->focused_component < screen->component_count);
    return &screen->components[controller->focused_component];
}

// Keeps selection and viewport within the current item range.
static void list_clamp(tui_list_t* const list) {
    if (list->count == 0) {
        list->selected = 0;
        list->first_visible = 0;
        return;
    }

    if (list->selected >= list->count) {
        list->selected = list->count - 1;
    }
    if (list->overlapping_pages && list->visible_rows > 1) {
        const size_t stride = list->visible_rows - 1;
        if (list->first_visible > list->selected) {
            list->first_visible = MIN(list->selected,
                list->first_visible - MIN(list->first_visible, stride));
        }
        if (list->selected - list->first_visible >= list->visible_rows)
            list->first_visible += (list->selected - list->first_visible - 1) / stride * stride;
        return;
    }
    if (list->first_visible > list->selected) {
        list->first_visible = list->selected;
    }
    if (list->visible_rows > 0 && list->selected - list->first_visible >= list->visible_rows) {
        list->first_visible = list->selected - list->visible_rows + 1;
    }
}

// Controls without drawable bounds and lists without visible rows are skipped during navigation.
static bool component_is_focusable(const tui_component_t* const component, const window_t* const window) {
    if (component->bounds.width == 0 || component->bounds.x >= window->width || component->bounds.height == 0 ||
        component->bounds.y >= window->height)
        return false;
    return component->kind == tui_component_button || (component->list.count != 0 && component->list.visible_rows != 0);
}

// Loads a borrowed label page outside the rendering path.
static void prepare_list(tui_controller_t* const controller, tui_list_t* const list) {
    list_clamp(list);
    if (list->load_page == NULL) {
        list->page_valid = true;
        return;
    }

    controller->callback_depth++;
    list->page_valid = list->load_page(list->context, list->first_visible, list->visible_rows);
    controller->callback_depth--;
}

// Invokes a screen callback while tracking callback nesting.
static void call_screen_callback(tui_controller_t* const controller, tui_screen_fn callback, void* context) {
    controller->callback_depth++;
    callback(controller, context);
    controller->callback_depth--;
}

// Clamps list state and retains focus without loading pages.
static void normalize_screen_state(tui_controller_t* const controller) {
    tui_screen_t* const screen = active_screen(controller);
    for (size_t i = 0; i < screen->component_count; i++) {
        tui_component_t* const component = &screen->components[i];
        const unsigned int top = component->bounds.y;
        const unsigned int available = top < controller->window->height
            ? controller->window->height - top : 0;
        if (component->kind == tui_component_list) {
            const size_t old_first_visible = component->list.first_visible;
            const size_t old_visible_rows = component->list.visible_rows;
            const size_t rows = MIN(component->bounds.height, available);
            component->list.visible_rows = MIN(component->list.visible_rows, rows);
            list_clamp(&component->list);
            vet(component->list.count == 0 || (component->list.visible_rows != 0 && component->bounds.width != 0 &&
                                               component->bounds.x < controller->window->width),
                "TUI list with items is outside the visible window");
            if (component->list.load_page != NULL && (component->list.first_visible != old_first_visible ||
                                                      component->list.visible_rows != old_visible_rows)) {
                component->list.page_valid = false;
            }
        }
    }

    if (controller->focused_component < screen->component_count &&
        component_is_focusable(&screen->components[controller->focused_component], controller->window)) {
        controller->has_focus = true;
        return;
    }
    for (size_t i = 0; i < screen->component_count; i++) {
        if (component_is_focusable(&screen->components[i], controller->window)) {
            controller->focused_component = i;
            controller->has_focus = true;
            return;
        }
    }
    controller->focused_component = 0;
    controller->has_focus = false;
}

// Normalizes the active screen and prepares all visible list pages outside rendering.
static void normalize_screen(tui_controller_t* const controller) {
    normalize_screen_state(controller);
    tui_screen_t* const screen = active_screen(controller);
    for (size_t i = 0; i < screen->component_count; i++) {
        if (screen->components[i].kind == tui_component_list) {
            prepare_list(controller, &screen->components[i].list);
        }
    }
}

// Normalizes the resulting screen after a callback, unless the callback dismissed the controller.
static void refresh_active_screen(tui_controller_t* const controller) {
    if (controller->active) {
        normalize_screen(controller);
    }
}

// Rejects paint callbacks that leave list geometry or indices invalid.
static void vet_paint_state(const tui_screen_t* const screen, const window_t* const window) {
    for (size_t i = 0; i < screen->component_count; i++) {
        const tui_component_t* const component = &screen->components[i];
        if (component->kind != tui_component_list)
            continue;
        const tui_list_t* const list = &component->list;
        const unsigned int available = component->bounds.y < window->height ? window->height - component->bounds.y : 0;
        const size_t max_rows = MIN(component->bounds.height, available);
        vet(list->visible_rows <= max_rows, "TUI list geometry changed during rendering");
        vet(list->count == 0 ||
                (list->visible_rows != 0 && component->bounds.width != 0 && component->bounds.x < window->width),
            "TUI list with items is outside the visible window");
        vet(list->count == 0 ? list->selected == 0 && list->first_visible == 0
                             : list->selected < list->count && list->first_visible < list->count,
            "TUI list model changed during rendering");
    }
}

void tui_controller_init(tui_controller_t* const controller, const window_t* const window,
                         tui_screen_t* const initial_screen) {
    assert(window != NULL);
    assert(initial_screen != NULL);
    assert(initial_screen->component_count > 0);

    *controller = (tui_controller_t) {
        .window = window,
        .screens = { initial_screen },
        .screen_count = 1,
        .focused_component = 0,
        .active = true,
        .dirty = true,
    };
    if (initial_screen->on_show != NULL) {
        call_screen_callback(controller, initial_screen->on_show, initial_screen->context);
    }
    refresh_active_screen(controller);
}

void tui_controller_push(tui_controller_t* const controller, tui_screen_t* const screen) {
    vet(!controller->rendering, "cannot change TUI screens while rendering");
    vet(controller->screen_count < TUI_SCREEN_STACK_CAPACITY, "TUI screen stack overflow");
    vet(screen != NULL && screen->component_count > 0, "TUI screen has no components");

    controller->saved_focus[controller->screen_count - 1] = controller->focused_component;
    controller->screens[controller->screen_count++] = screen;
    controller->focused_component = 0;
    controller->dirty = true;
    if (screen->on_show != NULL) {
        call_screen_callback(controller, screen->on_show, screen->context);
    }
    refresh_active_screen(controller);
}

void tui_controller_pop(tui_controller_t* const controller) {
    vet(!controller->rendering, "cannot change TUI screens while rendering");
    assert(controller->screen_count > 0);
    controller->dirty = true;

    if (controller->screen_count == 1) {
        controller->active = false;
        return;
    }

    controller->screen_count--;
    controller->focused_component = controller->saved_focus[controller->screen_count - 1];
    tui_screen_t* const screen = active_screen(controller);
    if (screen->on_show != NULL) {
        call_screen_callback(controller, screen->on_show, screen->context);
    }
    refresh_active_screen(controller);
}

bool tui_controller_is_active(const tui_controller_t* const controller) {
    return controller->active;
}

bool tui_controller_needs_render(const tui_controller_t* const controller) {
    return controller->active && controller->dirty;
}

// Draws a clipped row of text and optionally reverses its entire selection range.
static void paint_text(const window_t* const window, tui_rect_t bounds,
                       unsigned int row, const char* text, bool reversed) {
    if (bounds.x >= window->width || row >= window->height) return;
    const unsigned int width = MIN(bounds.width, window->width - bounds.x);
    uint8_t* const start = window_xy(window, bounds.x, row);
    for (unsigned int x = 0; x < width && text[x] != '\0'; x++) {
        start[x] = ascii_to_vrom((uint8_t) text[x]);
    }
    if (reversed) window_reverse(window, start, width);
}

// Paints cached labels within the list rectangle.
static void render_list(const tui_controller_t* const controller, const tui_list_t* const list,
                        bool focused, tui_rect_t bounds) {
    const window_t* const window = controller->window;
    if (list->first_visible >= list->count)
        return;
    const size_t remaining = list->count - list->first_visible;
    const size_t visible = MIN(MIN(list->visible_rows, remaining), bounds.height);

    for (size_t i = 0; list->page_valid && i < visible; i++) {
        const size_t index = list->first_visible + i;
        const char* const label = list->label_at != NULL
            ? list->label_at(list->context, index) : list->labels[index];
        vet(label != NULL, "TUI page is missing item %zu", index);
        paint_text(window, bounds, bounds.y + i, label, focused && index == list->selected);
    }
}

// Paints a compact button centered in its rectangle.
static void render_button(const tui_controller_t* const controller, const tui_button_t* const button,
                          bool focused, tui_rect_t bounds) {
    const window_t* const window = controller->window;
    if (bounds.width == 0 || bounds.height == 0 || bounds.x >= window->width || bounds.y >= window->height)
        return;
    char text[80];
    snprintf(text, sizeof(text), "[ %.*s ]", (int) sizeof(text) - 5, button->label);
    const unsigned int visible_width = MIN(bounds.width, window->width - bounds.x);
    const size_t length = MIN(strlen(text), visible_width);
    bounds.x += (visible_width - length) / 2;
    bounds.width = length;
    paint_text(window, bounds, bounds.y, text, focused);
}

void tui_controller_render(tui_controller_t* const controller) {
    vet(!controller->rendering, "cannot render TUI recursively");
    vet(controller->callback_depth == 0, "cannot render TUI from a callback");
    const window_t* const window = controller->window;
    tui_screen_t* const screen = active_screen(controller);

    controller->dirty = false;
    controller->rendering = true;
    window_fill(window, CH_SPACE);
    if (screen->on_paint != NULL) {
        screen->on_paint(window, screen->context);
    }
    vet_paint_state(screen, window);

    for (size_t i = 0; i < screen->component_count; i++) {
        const tui_component_t* const component = &screen->components[i];
        const bool focused = i == controller->focused_component;
        if (component->kind == tui_component_list) {
            render_list(controller, &component->list, focused, component->bounds);
        } else {
            render_button(controller, &component->button, focused, component->bounds);
        }
    }
    controller->rendering = false;
}

// Moves between visible lists and buttons with wraparound.
static void move_focus(tui_controller_t* const controller, int delta) {
    tui_screen_t* const screen = active_screen(controller);
    const size_t count = screen->component_count;

    for (size_t i = 0; i < count; i++) {
        if (delta < 0) {
            controller->focused_component = controller->focused_component == 0
                ? count - 1 : controller->focused_component - 1;
        } else {
            controller->focused_component = (controller->focused_component + 1) % count;
        }
        if (component_is_focusable(&screen->components[controller->focused_component], controller->window)) {
            return;
        }
    }
}

// Moves one item within a standalone list.
static void list_move(tui_list_t* const list, int delta) {
    if (list->count == 0) {
        return;
    }

    if (delta < 0) {
        list->selected = list->selected == 0 ? list->count - 1 : list->selected - 1;
    } else {
        list->selected = (list->selected + 1) % list->count;
    }
    list_clamp(list);
}

// Moves a page, optionally retaining an edge item and selecting its newly exposed neighbor.
static void list_page(tui_list_t* const list, int delta) {
    if (list->count == 0 || list->visible_rows == 0) {
        return;
    }

    const size_t page = list->visible_rows;
    if (list->overlapping_pages && page > 1) {
        const size_t last = list->count - 1;
        if (delta < 0) {
            list->selected = list->first_visible > 0 ? list->first_visible - 1 : 0;
            list->first_visible -= MIN(list->first_visible, page - 1);
        } else if (last - list->first_visible >= page) {
            list->first_visible += page - 1;
            list->selected = list->first_visible + 1;
        } else {
            list->selected = last;
        }
        list_clamp(list);
        return;
    }
    if (delta < 0) {
        list->selected = list->selected > page ? list->selected - page : 0;
    } else {
        const size_t last = list->count - 1;
        list->selected = page < last - list->selected ? list->selected + page : last;
    }
    list_clamp(list);
}

void tui_controller_dispatch(tui_controller_t* const controller, tui_action_t action) {
    vet(!controller->rendering, "cannot dispatch TUI actions while rendering");
    if (!controller->active) {
        return;
    }

    if (action == tui_action_none) {
        return;
    }
    if (action == tui_action_back) {
        controller->dirty = true;
        if (active_screen(controller)->on_back != NULL) {
            tui_screen_t* const screen = active_screen(controller);
            call_screen_callback(controller, screen->on_back, screen->context);
            refresh_active_screen(controller);
        } else {
            tui_controller_pop(controller);
        }
        return;
    }
    if (!controller->has_focus) {
        return;
    }

    tui_component_t* const component = focused_component(controller);
    const size_t old_focus = controller->focused_component;
    const size_t old_selected = component->kind == tui_component_list ? component->list.selected : 0;
    const size_t old_first = component->kind == tui_component_list ? component->list.first_visible : 0;
    const bool old_page_valid = component->kind == tui_component_list && component->list.page_valid;
    switch (action) {
        case tui_action_up:
            if (component->kind == tui_component_list && component->list.count > 0
                && (component->list.selected != 0 || active_screen(controller)->component_count == 1)) {
                list_move(&component->list, -1);
            } else {
                move_focus(controller, -1);
                tui_component_t* const target = focused_component(controller);
                if (target->kind == tui_component_list && target->list.count > 0)
                    target->list.selected = target->list.count - 1;
            }
            break;
        case tui_action_down:
            if (component->kind == tui_component_list && component->list.count > 0
                && (component->list.selected + 1 < component->list.count
                    || active_screen(controller)->component_count == 1)) {
                list_move(&component->list, 1);
            } else {
                move_focus(controller, 1);
                tui_component_t* const target = focused_component(controller);
                if (target->kind == tui_component_list) target->list.selected = 0;
            }
            break;
        case tui_action_page_up:
            if (component->kind == tui_component_list) {
                list_page(&component->list, -1);
            }
            break;
        case tui_action_page_down:
            if (component->kind == tui_component_list) {
                list_page(&component->list, 1);
            }
            break;
        case tui_action_activate:
            controller->dirty = true;
            if (component->kind == tui_component_list) {
                if (component->list.count != 0 && component->list.page_valid && component->list.on_activate != NULL) {
                    controller->callback_depth++;
                    component->list.on_activate(controller, component->list.context, component->list.selected);
                    controller->callback_depth--;
                    refresh_active_screen(controller);
                }
            } else if (component->button.on_activate != NULL) {
                call_screen_callback(controller, component->button.on_activate, component->button.context);
                refresh_active_screen(controller);
            }
            break;
        case tui_action_back:
            break;
        case tui_action_none:
            break;
    }
    if (action != tui_action_activate && action != tui_action_back) {
        tui_component_t* const target = focused_component(controller);
        if (target->kind == tui_component_list) {
            list_clamp(&target->list);
            if (target != component || target->list.first_visible != old_first || !target->list.page_valid)
                prepare_list(controller, &target->list);
        }
        if (controller->focused_component != old_focus ||
            (target->kind == tui_component_list &&
             (target->list.selected != old_selected || target->list.first_visible != old_first ||
              target->list.page_valid != old_page_valid))) {
            controller->dirty = true;
        }
    }
}
