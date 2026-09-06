#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <wlr/util/edges.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

void seat_reset_cursor_mode(struct kono_server *server) {
	server->cursor_mode = KONO_CURSOR_PASSTHROUGH;
	server->grabbed_view = NULL;
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->wlr_keyboard->modifiers);
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct kono_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct kono_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms = NULL;
	int nsyms = 0;
	xkb_layout_index_t layout = xkb_state_key_get_layout(keyboard->wlr_keyboard->xkb_state, keycode);
	if (layout != XKB_LAYOUT_INVALID) {
		nsyms = xkb_keymap_key_get_syms_by_level(keyboard->wlr_keyboard->keymap,
				keycode, layout, 0, &syms);
	}
	if (nsyms <= 0) {
		nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);
	}

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);

	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			if (handle_keybinding(server, syms[i], modifiers)) {
				handled = true;
				break;
			}
		}

		if (!handled && server->session && (modifiers & WLR_MODIFIER_CTRL) &&
				(modifiers & WLR_MODIFIER_ALT)) {
			for (int i = 0; i < nsyms; i++) {
				unsigned vt = 0;
				if (syms[i] >= XKB_KEY_XF86Switch_VT_1 && syms[i] <= XKB_KEY_XF86Switch_VT_12) {
					vt = 1 + (syms[i] - XKB_KEY_XF86Switch_VT_1);
				} else if (syms[i] >= XKB_KEY_F1 && syms[i] <= XKB_KEY_F12) {
					vt = 1 + (syms[i] - XKB_KEY_F1);
				}
				if (vt) {
					wlr_session_change_vt(server->session, vt);
					handled = true;
					break;
				}
			}
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->destroy.link);
	free(keyboard);
}

static void server_new_keyboard(struct kono_server *server, struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
	struct kono_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	if (!keyboard) {
		return;
	}
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names rules = {0};
	if (server->config.kb_layout[0]) {
		rules.layout = server->config.kb_layout;
	}
	if (server->config.kb_variant[0]) {
		rules.variant = server->config.kb_variant;
	}
	if (server->config.kb_options[0]) {
		rules.options = server->config.kb_options;
	}
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, wlr_keyboard);
	wl_list_insert(&server->keyboards, &keyboard->link);
	wlr_log(WLR_INFO, "Keyboard connected");
}

static void server_new_pointer(struct kono_server *server, struct wlr_input_device *device) {
	wlr_cursor_attach_input_device(server->cursor, device);
	wlr_log(WLR_INFO, "Pointer connected");
}

static void server_new_input(struct wl_listener *listener, void *data) {
	struct kono_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}

	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct kono_server *server = wl_container_of(listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused = server->seat->pointer_state.focused_client;
	if (focused == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
	}
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct kono_server *server = wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void seat_request_set_primary_selection(struct wl_listener *listener, void *data) {
	struct kono_server *server = wl_container_of(listener, server, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

void seat_process_cursor_motion(struct kono_server *server, uint32_t time) {
	if (server->cursor_mode == KONO_CURSOR_MOVE) {
		struct kono_view *view = server->grabbed_view;
		if (!view) {
			return;
		}
		view->x = (int)(server->cursor->x - server->grab_x);
		view->y = (int)(server->cursor->y - server->grab_y);
		wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
		return;
	}

	if (server->cursor_mode == KONO_CURSOR_RESIZE) {
		struct kono_view *view = server->grabbed_view;
		if (!view) {
			return;
		}
		double border_x = server->cursor->x - server->grab_x;
		double border_y = server->cursor->y - server->grab_y;
		int new_left = server->grab_geobox.x;
		int new_right = server->grab_geobox.x + server->grab_geobox.width;
		int new_top = server->grab_geobox.y;
		int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

		if (server->resize_edges & WLR_EDGE_TOP) {
			new_top = (int)border_y;
			if (new_top >= new_bottom - 32) {
				new_top = new_bottom - 32;
			}
		} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
			new_bottom = (int)border_y;
			if (new_bottom <= new_top + 32) {
				new_bottom = new_top + 32;
			}
		}
		if (server->resize_edges & WLR_EDGE_LEFT) {
			new_left = (int)border_x;
			if (new_left >= new_right - 32) {
				new_left = new_right - 32;
			}
		} else if (server->resize_edges & WLR_EDGE_RIGHT) {
			new_right = (int)border_x;
			if (new_right <= new_left + 32) {
				new_right = new_left + 32;
			}
		}

		view->x = new_left;
		view->y = new_top;
		view->width = new_right - new_left;
		view->height = new_bottom - new_top;
		wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
		int bw = server->config.border_size;
		int inner_w = view->width - 2 * bw;
		int inner_h = view->height - 2 * bw;
		if (inner_w < 1) {
			inner_w = 1;
		}
		if (inner_h < 1) {
			inner_h = 1;
		}
		wlr_xdg_toplevel_set_size(view->xdg_toplevel, inner_w, inner_h);
		for (int i = 0; i < 4; i++) {
			if (!view->borders[i]) {
				continue;
			}
			if (i == 0) {
				wlr_scene_rect_set_size(view->borders[0], bw, view->height);
			} else if (i == 1) {
				wlr_scene_rect_set_size(view->borders[1], view->width, bw);
			} else if (i == 2) {
				wlr_scene_rect_set_size(view->borders[2], bw, view->height);
				wlr_scene_node_set_position(&view->borders[2]->node, view->width - bw, 0);
			} else {
				wlr_scene_rect_set_size(view->borders[3], view->width, bw);
				wlr_scene_node_set_position(&view->borders[3]->node, 0, view->height - bw);
			}
		}
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct kono_view *view = NULL;
	kono_pointer_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy, &view);

	if (!view && !surface) {
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, "left_ptr");
		wlr_seat_pointer_clear_focus(server->seat);
		return;
	}

	if (surface) {
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(server->seat);
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct kono_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
	seat_process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
	struct kono_server *server = wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
	seat_process_cursor_motion(server, event->time_msec);
}

static uint32_t edges_from_cursor(struct kono_view *view, double lx, double ly) {
	int border = 32;
	uint32_t edges = 0;
	if (lx < view->x + border) {
		edges |= WLR_EDGE_LEFT;
	} else if (lx >= view->x + view->width - border) {
		edges |= WLR_EDGE_RIGHT;
	}
	if (ly < view->y + border) {
		edges |= WLR_EDGE_TOP;
	} else if (ly >= view->y + view->height - border) {
		edges |= WLR_EDGE_BOTTOM;
	}
	if (!edges) {
		edges = WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM;
	}
	return edges;
}

static bool handle_bar_click(struct kono_server *server, double lx, double ly) {
	if (!server->config.builtin_bar) {
		return false;
	}
	struct kono_output *output = kono_output_at(server, lx, ly);
	if (!output) {
		return false;
	}
	struct wlr_box layout_box;
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &layout_box);
	if (ly < layout_box.y || ly >= layout_box.y + KONO_BAR_HEIGHT) {
		return false;
	}
	int local_x = (int)(lx - layout_box.x) - 14;
	if (local_x < 0) {
		return false;
	}
	int ws = local_x / 22 + 1;
	if (ws >= 1 && ws <= KONO_WORKSPACES) {
		kono_switch_workspace(ws);
		return true;
	}
	return false;
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	struct kono_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (server->cursor_mode != KONO_CURSOR_PASSTHROUGH) {
			seat_reset_cursor_mode(server);
			wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, "left_ptr");
		}
		wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
		return;
	}

	seat_process_cursor_motion(server, event->time_msec);

	if (!server->exclusive_layer && !server->seat->pointer_state.focused_surface &&
			handle_bar_click(server, server->cursor->x, server->cursor->y)) {
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct kono_view *view = NULL;
	kono_pointer_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy, &view);
	if (view && !server->exclusive_layer) {
		kono_focus_view(view, surface);
	}

	uint32_t mods = 0;
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	if (kb) {
		mods = wlr_keyboard_get_modifiers(kb);
	}

	if (view && !server->exclusive_layer && (mods & WLR_MODIFIER_LOGO)) {
		if (event->button == BTN_LEFT) {
			kono_view_begin_interactive(view, KONO_CURSOR_MOVE, 0);
			return;
		}
		if (event->button == BTN_RIGHT) {
			uint32_t edges = edges_from_cursor(view, server->cursor->x, server->cursor->y);
			kono_view_begin_interactive(view, KONO_CURSOR_RESIZE, edges);
			return;
		}
	}

	wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct kono_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation,
			event->delta, event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

void seat_set_default_cursor(struct kono_server *server) {
	if (!server->cursor || !server->xcursor_manager) {
		return;
	}
	wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, "left_ptr");
}

void seat_init(struct kono_server *server) {
	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
	server->xcursor_manager = wlr_xcursor_manager_create(NULL, 24);
	wlr_xcursor_manager_load(server->xcursor_manager, 1.0f);
	seat_set_default_cursor(server);

	server->cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
	server->cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server->cursor->events.motion_absolute, &server->cursor_motion_absolute);
	server->cursor_button.notify = server_cursor_button;
	wl_signal_add(&server->cursor->events.button, &server->cursor_button);
	server->cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
	server->cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

	server->seat = wlr_seat_create(server->wl_display, "seat0");
	server->request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor, &server->request_cursor);
	server->request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection, &server->request_set_selection);
	server->request_set_primary_selection.notify = seat_request_set_primary_selection;
	wl_signal_add(&server->seat->events.request_set_primary_selection,
			&server->request_set_primary_selection);

	server->new_input.notify = server_new_input;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);
}

void seat_fini(struct kono_server *server) {
	struct kono_keyboard *kb, *tmp;
	wl_list_for_each_safe(kb, tmp, &server->keyboards, link) {
		wl_list_remove(&kb->link);
		wl_list_remove(&kb->key.link);
		wl_list_remove(&kb->modifiers.link);
		wl_list_remove(&kb->destroy.link);
		free(kb);
	}
}
