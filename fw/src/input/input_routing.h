// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/input_actions.h"

/** Reset routing and pending input at startup or in a host fixture. */
void input_routing_init(void);
/** Begin one acquisition cycle, allowing button priority only over its new requests. */
void input_routing_begin_cycle(void);
/** Seal emitted requests so later observations cannot replace pending intent. */
void input_routing_end_cycle(void);
/** Cancel routed input and held gestures without changing the active destination. */
void input_routing_reset(void);
/** Transfer input ownership and discard work from the previous context. */
void input_routing_set_menu_active(bool active);
/** Report whether decoded navigation currently belongs to the firmware menu. */
bool input_routing_menu_active(void);
/** Submit decoded navigation in observation order, logging rejected overflow. */
bool input_routing_submit(input_source_t source, tui_action_t action);
/** Cancel one source's navigation and runtime ownership, retaining emitted machine intent. */
void input_routing_cancel(input_source_t source);
/** Route a decoded terminal character using the authoritative terminal session. */
void input_routing_terminal(int key);
/** Route a source's decoded batch to navigation or the shared runtime core. */
void input_routing_keyboard(const keyboard_batch_t* batch);
/** Bind a recognized gesture to menu navigation or genuine machine intent. */
void input_routing_gesture(input_source_t source, input_gesture_t gesture);
/** Read navigation only, independently of pending machine requests. */
tui_action_t input_routing_navigation(void);
/** Latch machine intent, applying button priority only within the current acquisition cycle. */
void input_routing_request(input_system_request_t request);
/** Observe durable machine intent without consuming it or navigation. */
input_system_request_t input_routing_pending_request(void);
/** Take the pending machine request without polling or discarding navigation. */
input_system_request_t input_routing_take_request(void);
