#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <stdlib.h>
#include <time.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/util/log.h>

static void output_update_wallpaper(struct kono_output *output) {
	int width, height;
	wlr_output_effective_resolution(output->wlr_output, &width, &height);

	struct wlr_box layout_box;
	wlr_output_layout_get_box(output->server->output_layout, output->wlr_output, &layout_box);

	if (!output->wallpaper) {
		output->wallpaper = wlr_scene_rect_create(
				output->server->layers[KONO_LAYER_BACKGROUND],
				width, height, output->server->config.background);
	} else {
		wlr_scene_rect_set_size(output->wallpaper, width, height);
		wlr_scene_rect_set_color(output->wallpaper, output->server->config.background);
	}
	if (output->wallpaper) {
		wlr_scene_node_set_position(&output->wallpaper->node, layout_box.x, layout_box.y);
	}
	kono_output_apply_wallpaper(output);
}

static void output_build_bar(struct kono_output *output) {
	struct kono_server *server = output->server;
	if (!server->config.builtin_bar) {
		if (output->bar_tree) {
			wlr_scene_node_set_enabled(&output->bar_tree->node, false);
		}
		return;
	}

	if (!output->bar_tree) {
		output->bar_tree = wlr_scene_tree_create(server->layers[KONO_LAYER_TOP]);
		output->bar_bg = wlr_scene_rect_create(output->bar_tree, 1, KONO_BAR_HEIGHT, server->config.bar_bg);
		for (int i = 0; i < KONO_WORKSPACES; i++) {
			output->bar_ws[i] = wlr_scene_rect_create(output->bar_tree, 18, 18, server->config.bar_inactive);
		}
	}

	wlr_scene_node_set_enabled(&output->bar_tree->node, true);
	kono_output_update_bar(output);
}

void kono_output_update_bar(struct kono_output *output) {
	if (!output->bar_tree || !output->server->config.builtin_bar) {
		return;
	}

	int width, height;
	wlr_output_effective_resolution(output->wlr_output, &width, &height);
	struct wlr_box layout_box;
	wlr_output_layout_get_box(output->server->output_layout, output->wlr_output, &layout_box);

	wlr_scene_node_set_position(&output->bar_tree->node, layout_box.x, layout_box.y);
	if (output->bar_bg) {
		wlr_scene_rect_set_size(output->bar_bg, width, KONO_BAR_HEIGHT);
		wlr_scene_rect_set_color(output->bar_bg, output->server->config.bar_bg);
	}

	bool occupied[KONO_WORKSPACES] = {0};
	struct kono_view *view;
	wl_list_for_each(view, &output->server->views, link) {
		if (view->mapped && view->workspace >= 1 && view->workspace <= KONO_WORKSPACES) {
			occupied[view->workspace - 1] = true;
		}
	}

	int x = 14;
	int y = (KONO_BAR_HEIGHT - 14) / 2;
	for (int i = 0; i < KONO_WORKSPACES; i++) {
		if (!output->bar_ws[i]) {
			continue;
		}
		const float *color = output->server->config.bar_inactive;
		int size = 14;
		if (i + 1 == output->server->current_workspace) {
			color = output->server->config.bar_active;
			size = 16;
		} else if (occupied[i]) {
			color = output->server->config.bar_occupied;
		}
		wlr_scene_rect_set_color(output->bar_ws[i], color);
		wlr_scene_rect_set_size(output->bar_ws[i], size, size);
		wlr_scene_node_set_position(&output->bar_ws[i]->node, x, y - (size - 14) / 2);
		x += 22;
	}
}

void kono_output_usable_area(struct kono_output *output, struct wlr_box *box) {
	*box = output->usable;
}

struct kono_output *kono_output_at(struct kono_server *server, double lx, double ly) {
	struct wlr_output *wlr_output = wlr_output_layout_output_at(server->output_layout, lx, ly);
	if (!wlr_output) {
		return NULL;
	}
	struct kono_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output == wlr_output) {
			return output;
		}
	}
	return NULL;
}

struct kono_output *kono_output_preferred(struct kono_server *server) {
	if (wl_list_empty(&server->outputs)) {
		return NULL;
	}
	struct kono_output *at = kono_output_at(server, server->cursor->x, server->cursor->y);
	if (at) {
		return at;
	}
	return wl_container_of(server->outputs.next, at, link);
}

static void layer_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_layer *layer = wl_container_of(listener, layer, commit);
	if (!layer->layer_surface->initialized) {
		return;
	}
	kono_output_arrange_layers(layer->output);
	kono_arrange(layer->server);
}

static void layer_map(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_layer *layer = wl_container_of(listener, layer, map);
	wlr_scene_node_set_enabled(&layer->scene->tree->node, true);
	kono_output_arrange_layers(layer->output);
	if (layer->layer_surface->current.keyboard_interactive !=
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
		layer->server->exclusive_layer = layer;
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(layer->server->seat);
		if (kb) {
			wlr_seat_keyboard_notify_enter(layer->server->seat, layer->layer_surface->surface,
					kb->keycodes, kb->num_keycodes, &kb->modifiers);
		}
	}
	seat_process_cursor_motion(layer->server, 0);
	kono_arrange(layer->server);
}

static void layer_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_layer *layer = wl_container_of(listener, layer, unmap);
	wlr_scene_node_set_enabled(&layer->scene->tree->node, false);
	if (layer->server->exclusive_layer == layer) {
		kono_layer_unfocus(layer->server);
	}
	seat_process_cursor_motion(layer->server, 0);
	kono_output_arrange_layers(layer->output);
	kono_arrange(layer->server);
}

static void layer_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_layer *layer = wl_container_of(listener, layer, destroy);
	if (layer->server->exclusive_layer == layer) {
		kono_layer_unfocus(layer->server);
	}
	if (layer->popups) {
		wlr_scene_node_destroy(&layer->popups->node);
	}
	wl_list_remove(&layer->link);
	wl_list_remove(&layer->map.link);
	wl_list_remove(&layer->unmap.link);
	wl_list_remove(&layer->commit.link);
	wl_list_remove(&layer->destroy.link);
	wl_list_remove(&layer->new_popup.link);
	free(layer);
}

static void layer_new_popup(struct wl_listener *listener, void *data) {
	struct kono_layer *layer = wl_container_of(listener, layer, new_popup);
	struct wlr_xdg_popup *popup = data;
	struct wlr_scene_tree *parent = layer->popups ? layer->popups : layer->scene->tree;
	popup->base->data = wlr_scene_xdg_surface_create(parent, popup->base);
}

static enum kono_scene_layer shell_layer_to_kono(enum zwlr_layer_shell_v1_layer layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return KONO_LAYER_BACKGROUND;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return KONO_LAYER_BOTTOM;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return KONO_LAYER_TOP;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
	default:
		return KONO_LAYER_OVERLAY;
	}
}

static void arrange_layer_list(struct kono_output *output, enum zwlr_layer_shell_v1_layer shell_layer,
		const struct wlr_box *full, struct wlr_box *usable) {
	struct kono_layer *layer;
	wl_list_for_each(layer, &output->layers, link) {
		if (!layer->layer_surface->initialized) {
			continue;
		}
		if (layer->layer_surface->current.layer != shell_layer) {
			continue;
		}
		enum kono_scene_layer dest = shell_layer_to_kono(shell_layer);
		wlr_scene_node_reparent(&layer->scene->tree->node, output->server->layers[dest]);
		wlr_scene_layer_surface_v1_configure(layer->scene, full, usable);
	}
}

void kono_output_arrange_layers(struct kono_output *output) {
	if (!output) {
		return;
	}

	struct wlr_box full;
	wlr_output_layout_get_box(output->server->output_layout, output->wlr_output, &full);
	struct wlr_box usable = full;

	if (output->server->config.builtin_bar) {
		usable.y += KONO_BAR_HEIGHT;
		usable.height -= KONO_BAR_HEIGHT;
		if (usable.height < 1) {
			usable.height = 1;
		}
	}

	arrange_layer_list(output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, &full, &usable);
	arrange_layer_list(output, ZWLR_LAYER_SHELL_V1_LAYER_TOP, &full, &usable);
	arrange_layer_list(output, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, &full, &usable);
	arrange_layer_list(output, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, &full, &usable);

	output->usable = usable;
}

void kono_handle_new_layer_surface(struct wl_listener *listener, void *data) {
	struct kono_server *server = wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *layer_surface = data;

	struct kono_output *output = NULL;
	if (layer_surface->output) {
		struct kono_output *iter;
		wl_list_for_each(iter, &server->outputs, link) {
			if (iter->wlr_output == layer_surface->output) {
				output = iter;
				break;
			}
		}
	}
	if (!output) {
		output = kono_output_preferred(server);
		if (output) {
			layer_surface->output = output->wlr_output;
		}
	}
	if (!output) {
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	struct kono_layer *layer = calloc(1, sizeof(*layer));
	if (!layer) {
		return;
	}
	layer->server = server;
	layer->output = output;
	layer->layer_surface = layer_surface;

	enum kono_scene_layer dest = shell_layer_to_kono(layer_surface->pending.layer);
	layer->scene = wlr_scene_layer_surface_v1_create(server->layers[dest], layer_surface);
	layer->scene->tree->node.data = layer;
	layer_surface->data = layer;
	layer->popups = wlr_scene_tree_create(server->layers[KONO_LAYER_OVERLAY]);

	layer->map.notify = layer_map;
	wl_signal_add(&layer_surface->surface->events.map, &layer->map);
	layer->unmap.notify = layer_unmap;
	wl_signal_add(&layer_surface->surface->events.unmap, &layer->unmap);
	layer->commit.notify = layer_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &layer->commit);
	layer->destroy.notify = layer_destroy;
	wl_signal_add(&layer_surface->events.destroy, &layer->destroy);
	layer->new_popup.notify = layer_new_popup;
	wl_signal_add(&layer_surface->events.new_popup, &layer->new_popup);

	wl_list_insert(&output->layers, &layer->link);
}

static void output_frame(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *scene_output =
			wlr_scene_get_scene_output(output->server->scene, output->wlr_output);
	if (!scene_output) {
		return;
	}
	if (!wlr_scene_output_commit(scene_output, NULL)) {
		return;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct kono_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_commit(struct wl_listener *listener, void *data) {
	struct kono_output *output = wl_container_of(listener, output, commit);
	const struct wlr_output_event_commit *event = data;
	if (event->state->committed & (WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_SCALE |
			WLR_OUTPUT_STATE_TRANSFORM | WLR_OUTPUT_STATE_ENABLED)) {
		output_update_wallpaper(output);
		output_build_bar(output);
		kono_output_arrange_layers(output);
		kono_arrange(output->server);
	}
}

static void output_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_output *output = wl_container_of(listener, output, destroy);
	wl_list_remove(&output->link);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->commit.link);
	wl_list_remove(&output->destroy.link);
	free(output);
}

void kono_output_handle_new(struct wl_listener *listener, void *data) {
	struct kono_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_log(WLR_INFO, "New output: %s", wlr_output->name);
	if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
		wlr_log(WLR_ERROR, "Failed to init renderer on %s", wlr_output->name);
		return;
	}

	struct kono_output *output = calloc(1, sizeof(*output));
	if (!output) {
		return;
	}
	output->wlr_output = wlr_output;
	output->server = server;
	wl_list_init(&output->layers);
	wlr_output->data = output;

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->commit.notify = output_commit;
	wl_signal_add(&wlr_output->events.commit, &output->commit);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *l_output =
			wlr_output_layout_add_auto(server->output_layout, wlr_output);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);

	wlr_xcursor_manager_load(server->xcursor_manager, wlr_output->scale);
	seat_set_default_cursor(server);

	wlr_output_effective_resolution(wlr_output, &output->usable.width, &output->usable.height);
	output_update_wallpaper(output);
	output_build_bar(output);
	kono_output_arrange_layers(output);
	kono_arrange(server);

	struct wlr_box cursor_box;
	wlr_output_layout_get_box(server->output_layout, wlr_output, &cursor_box);
	if (cursor_box.width > 0 && cursor_box.height > 0) {
		wlr_cursor_warp_closest(server->cursor, NULL,
				cursor_box.x + cursor_box.width / 2.0,
				cursor_box.y + cursor_box.height / 2.0);
		seat_process_cursor_motion(server, 0);
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode) {
		wlr_output_state_set_mode(&state, mode);
	}
	if (!wlr_scene_output_build_state(scene_output, &state, NULL)) {
		wlr_log(WLR_ERROR, "Failed to build scene state for %s", wlr_output->name);
	}
	if (!wlr_output_commit_state(wlr_output, &state)) {
		wlr_log(WLR_ERROR, "Failed to enable output %s", wlr_output->name);
	}
	wlr_output_state_finish(&state);

	wlr_log(WLR_INFO, "Output %s ready (%dx%d)", wlr_output->name,
			wlr_output->width, wlr_output->height);
}
