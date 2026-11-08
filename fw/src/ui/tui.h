// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "display/window.h"
#include "input/input_actions.h"

#define TUI_SCREEN_STACK_CAPACITY 4

typedef struct tui_controller_s tui_controller_t;

typedef void (*tui_activate_fn)(tui_controller_t* controller, void* context, size_t index);
typedef void (*tui_screen_fn)(tui_controller_t* controller, void* context);
typedef void (*tui_paint_fn)(const window_t* window, const void* context);
typedef const char* (*tui_label_fn)(void* context, size_t index);
typedef bool (*tui_page_fn)(void* context, size_t first, size_t rows);

typedef struct {
    unsigned int x, y, width, height;
} tui_rect_t;

typedef struct {
    // Fallback labels indexed by item, used when label_at is NULL.
    const char* const* labels;
    // Total number of items.
    size_t count;
    // Index of the selected item.
    size_t selected;
    // Index of the first item in the visible viewport.
    size_t first_visible;
    // Maximum visible rows, clamped to drawable geometry when the screen opens.
    size_t visible_rows;
    // Page by visible_rows - 1 so adjacent pages share one item.
    bool overlapping_pages;
    // Called with the selected item index when the list is activated.
    tui_activate_fn on_activate;
    // User data passed to the list callbacks.
    void* context;
    // Optional callback that returns an item's label while painting.
    tui_label_fn label_at;
    // Optional callback that prepares the visible page on open or navigation, not while painting.
    tui_page_fn load_page;
    // Set by the controller to indicate whether the current page loaded successfully.
    bool page_valid;
} tui_list_t;

typedef struct {
    // Text displayed inside the button.
    const char* label;
    // Called when the button is activated.
    tui_screen_fn on_activate;
    // User data passed to the activation callback.
    void* context;
} tui_button_t;

typedef enum {
    tui_component_list,
    tui_component_button,
} tui_component_kind_t;

typedef struct {
    // Determines which payload in data is active.
    tui_component_kind_t kind;
    // Position and size of the component within the character window.
    tui_rect_t bounds;
    // Data for the selected component kind.
    union {
        tui_list_t list;
        tui_button_t button;
    };
} tui_component_t;

typedef struct {
    // Borrowed component array displayed by this screen.
    tui_component_t* components;
    // Number of entries in components.
    size_t component_count;
    // Called when this screen becomes active.
    tui_screen_fn on_show;
    // User data passed to the screen callbacks.
    void* context;
    // Called while rendering to paint screen-specific content; must not mutate screen models or navigate.
    tui_paint_fn on_paint;
    // Called for a back action; when NULL, the controller pops this screen.
    tui_screen_fn on_back;
} tui_screen_t;

struct tui_controller_s {
    // Borrowed character window used to paint screens.
    const window_t* window;
    // Borrowed screen stack, with the active screen at screen_count - 1.
    tui_screen_t* screens[TUI_SCREEN_STACK_CAPACITY];
    // Number of screens currently on the stack.
    size_t screen_count;
    // Focused component index in the active screen.
    size_t focused_component;
    // Whether the active screen has a visible, focusable component.
    bool has_focus;
    // Focus indices saved for parent screens while child screens are open.
    size_t saved_focus[TUI_SCREEN_STACK_CAPACITY];
    // False after the root screen is dismissed.
    bool active;
    // True when the current screen needs repainting.
    bool dirty;
    // True while painting, when controller operations that mutate navigation are forbidden.
    bool rendering;
    // Nesting depth of user callbacks, during which direct rendering is forbidden.
    size_t callback_depth;
};

// Starts a borrowed window/screen hierarchy. Its storage must outlive the controller.
void tui_controller_init(tui_controller_t* controller, const window_t* window, tui_screen_t* initial_screen);
// Opens a borrowed child screen and runs its refresh callback.
void tui_controller_push(tui_controller_t* controller, tui_screen_t* screen);
// Returns to the parent screen, or dismisses the root screen.
void tui_controller_pop(tui_controller_t* controller);
// Reports whether the root screen has been dismissed.
bool tui_controller_is_active(const tui_controller_t* controller);
// Reports whether navigation or invalidation requires repainting the screen.
bool tui_controller_needs_render(const tui_controller_t* controller);
// Paints the current screen into its character window; call only after callbacks return.
void tui_controller_render(tui_controller_t* controller);
// Applies navigation or invokes the focused component's activation callback.
void tui_controller_dispatch(tui_controller_t* controller, tui_action_t action);
