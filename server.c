#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

struct kono_server *g_server;

static void handle_sigint(int sig) {
	(void)sig;
	if (g_server && g_server->wl_display) {
		wl_display_terminate(g_server->wl_display);
	}
}

static int handle_sigusr1(int sig, void *data) {
	(void)sig;
	kono_reload_wallpaper(data);
	return 0;
}

void kono_spawn(const char *cmd) {
	if (!cmd || !cmd[0]) {
		return;
	}
	wlr_log(WLR_INFO, "exec: %s", cmd);

	char token_name[128] = {0};
	if (g_server && g_server->xdg_activation) {
		struct wlr_xdg_activation_token_v1 *token =
				wlr_xdg_activation_token_v1_create(g_server->xdg_activation);
		if (token) {
			token->seat = g_server->seat;
			if (g_server->exclusive_layer) {
				token->surface = g_server->exclusive_layer->layer_surface->surface;
			} else if (g_server->focused && g_server->focused->mapped) {
				token->surface = g_server->focused->xdg_toplevel->base->surface;
			}
			const char *name = wlr_xdg_activation_token_v1_get_name(token);
			if (name) {
				snprintf(token_name, sizeof(token_name), "%s", name);
			}
		}
	}

	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		if (token_name[0]) {
			setenv("XDG_ACTIVATION_TOKEN", token_name, 1);
			setenv("DESKTOP_STARTUP_ID", token_name, 1);
		}
		execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
		_exit(1);
	}
}

void kono_quit(void) {
	if (g_server && g_server->wl_display) {
		wl_display_terminate(g_server->wl_display);
	}
}

static struct wlr_surface *toplevel_surface(struct kono_view *view) {
	return view->xdg_toplevel->base->surface;
}

void kono_view_set_border_color(struct kono_view *view, const float color[4]) {
	for (int i = 0; i < 4; i++) {
		if (view->borders[i]) {
			wlr_scene_rect_set_color(view->borders[i], color);
		}
	}
}

static void view_update_borders(struct kono_view *view) {
	int bw = view->fullscreen ? 0 : view->server->config.border_size;
	int w = view->width;
	int h = view->height;
	if (w < 1) {
		w = 1;
	}
	if (h < 1) {
		h = 1;
	}

	if (view->borders[0]) {
		wlr_scene_rect_set_size(view->borders[0], bw, h);
		wlr_scene_node_set_position(&view->borders[0]->node, 0, 0);
		wlr_scene_node_set_enabled(&view->borders[0]->node, bw > 0);
	}
	if (view->borders[1]) {
		wlr_scene_rect_set_size(view->borders[1], w, bw);
		wlr_scene_node_set_position(&view->borders[1]->node, 0, 0);
		wlr_scene_node_set_enabled(&view->borders[1]->node, bw > 0);
	}
	if (view->borders[2]) {
		wlr_scene_rect_set_size(view->borders[2], bw, h);
		wlr_scene_node_set_position(&view->borders[2]->node, w - bw, 0);
		wlr_scene_node_set_enabled(&view->borders[2]->node, bw > 0);
	}
	if (view->borders[3]) {
		wlr_scene_rect_set_size(view->borders[3], w, bw);
		wlr_scene_node_set_position(&view->borders[3]->node, 0, h - bw);
		wlr_scene_node_set_enabled(&view->borders[3]->node, bw > 0);
	}
	wlr_scene_node_set_position(&view->content_tree->node, bw, bw);
}

static void view_place(struct kono_view *view, int x, int y, int width, int height) {
	view->x = x;
	view->y = y;
	view->width = width;
	view->height = height;
	wlr_scene_node_set_position(&view->scene_tree->node, x, y);
	view_update_borders(view);

	int bw = view->fullscreen ? 0 : view->server->config.border_size;
	int inner_w = width - 2 * bw;
	int inner_h = height - 2 * bw;
	if (inner_w < 1) {
		inner_w = 1;
	}
	if (inner_h < 1) {
		inner_h = 1;
	}
	if (view->xdg_toplevel->base->initialized) {
		wlr_xdg_toplevel_set_size(view->xdg_toplevel, inner_w, inner_h);
		uint32_t tiled = view->floating || view->fullscreen ? 0 :
				(WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
		wlr_xdg_toplevel_set_tiled(view->xdg_toplevel, tiled);
		wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, view->fullscreen);
	}
}

static void dwindle(struct kono_view **views, int n, struct wlr_box area, int gaps_in) {
	if (n <= 0) {
		return;
	}
	if (n == 1) {
		int g = gaps_in / 2;
		int x = area.x + g;
		int y = area.y + g;
		int w = area.width - gaps_in;
		int h = area.height - gaps_in;
		if (w < 50) {
			w = area.width > 1 ? area.width : 1;
			x = area.x;
		}
		if (h < 50) {
			h = area.height > 1 ? area.height : 1;
			y = area.y;
		}
		view_place(views[0], x, y, w, h);
		return;
	}

	struct wlr_box a = area, b = area;
	if (area.width >= area.height) {
		a.width = area.width / 2;
		b.x = area.x + a.width;
		b.width = area.width - a.width;
	} else {
		a.height = area.height / 2;
		b.y = area.y + a.height;
		b.height = area.height - a.height;
	}
	dwindle(views, 1, a, gaps_in);
	dwindle(views + 1, n - 1, b, gaps_in);
}

void kono_arrange(struct kono_server *server) {
	struct kono_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		kono_output_arrange_layers(output);
		kono_output_update_bar(output);

		struct wlr_box usable = output->usable;
		int go = server->config.gaps_out;
		usable.x += go;
		usable.y += go;
		usable.width -= 2 * go;
		usable.height -= 2 * go;
		if (usable.width < 1) {
			usable.width = 1;
		}
		if (usable.height < 1) {
			usable.height = 1;
		}

		struct kono_view *tiled[128];
		int n = 0;
		struct kono_view *view;
		wl_list_for_each(view, &server->views, link) {
			if (!view->mapped || view->workspace != server->current_workspace) {
				continue;
			}
			if (view->fullscreen) {
				struct wlr_box full;
				wlr_output_layout_get_box(server->output_layout, output->wlr_output, &full);
				view_place(view, full.x, full.y, full.width, full.height);
				wlr_scene_node_reparent(&view->scene_tree->node, server->layers[KONO_LAYER_OVERLAY]);
				wlr_scene_node_raise_to_top(&view->scene_tree->node);
				continue;
			}
			if (view->floating) {
				wlr_scene_node_reparent(&view->scene_tree->node, server->layers[KONO_LAYER_FLOAT]);
				view_place(view, view->x, view->y, view->width, view->height);
				continue;
			}
			if (n < 128) {
				tiled[n++] = view;
			}
			wlr_scene_node_reparent(&view->scene_tree->node, server->layers[KONO_LAYER_TILE]);
		}
		dwindle(tiled, n, usable, server->config.gaps_in);
	}
}

void kono_focus_view(struct kono_view *view, struct wlr_surface *surface) {
	if (!view) {
		return;
	}
	struct kono_server *server = view->server;
	struct wlr_surface *prev = server->seat->keyboard_state.focused_surface;

	if (!surface) {
		surface = toplevel_surface(view);
	}

	if (prev == surface) {
		return;
	}

	if (prev) {
		struct wlr_xdg_toplevel *prev_tl = wlr_xdg_toplevel_try_from_wlr_surface(prev);
		if (prev_tl && prev_tl->base->initialized) {
			wlr_xdg_toplevel_set_activated(prev_tl, false);
		}
		struct kono_view *iter;
		wl_list_for_each(iter, &server->views, link) {
			if (iter->mapped) {
				kono_view_set_border_color(iter, server->config.inactive_border);
			}
		}
	}

	if (view->floating || view->fullscreen) {
		wlr_scene_node_raise_to_top(&view->scene_tree->node);
	}
	wl_list_remove(&view->link);
	wl_list_insert(&server->views, &view->link);

	if (view->xdg_toplevel->base->initialized) {
		wlr_xdg_toplevel_set_activated(view->xdg_toplevel, true);
	}
	kono_view_set_border_color(view, server->config.active_border);
	server->focused = view;
	server->ws_focus[view->workspace] = view;

	if (server->exclusive_layer) {
		return;
	}

	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	if (kb) {
		wlr_seat_keyboard_notify_enter(server->seat, toplevel_surface(view),
				kb->keycodes, kb->num_keycodes, &kb->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(server->seat, toplevel_surface(view), NULL, 0, NULL);
	}
}

struct kono_view *kono_view_from_surface(struct kono_server *server, struct wlr_surface *surface) {
	struct wlr_xdg_toplevel *tl = wlr_xdg_toplevel_try_from_wlr_surface(surface);
	if (!tl) {
		return NULL;
	}
	struct kono_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->xdg_toplevel == tl) {
			return view;
		}
	}
	return NULL;
}

struct kono_view *kono_view_at(struct kono_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
	if (!node) {
		if (surface) {
			*surface = NULL;
		}
		return NULL;
	}

	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *buf = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buf);
		if (scene_surface && surface) {
			*surface = scene_surface->surface;
		}
	} else if (surface) {
		*surface = NULL;
	}

	struct wlr_scene_tree *tree = node->parent;
	while (tree && !tree->node.data) {
		tree = tree->node.parent;
	}
	if (!tree || !tree->node.data) {
		return NULL;
	}

	struct kono_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->scene_tree == tree || view->content_tree == tree) {
			return view;
		}
	}
	return NULL;
}

void kono_pointer_at(struct kono_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy, struct kono_view **view) {
	if (surface) {
		*surface = NULL;
	}
	if (view) {
		*view = NULL;
	}

	struct kono_layer *layer = server->exclusive_layer;
	if (layer && layer->layer_surface && layer->layer_surface->surface->mapped) {
		int nx, ny;
		wlr_scene_node_coords(&layer->scene->tree->node, &nx, &ny);
		double lsx = lx - nx;
		double lsy = ly - ny;
		struct wlr_surface *surf = wlr_layer_surface_v1_surface_at(
				layer->layer_surface, lsx, lsy, sx, sy);
		if (!surf) {
			surf = wlr_layer_surface_v1_popup_surface_at(
					layer->layer_surface, lsx, lsy, sx, sy);
		}
		if (!surf) {
			surf = layer->layer_surface->surface;
			if (sx) {
				*sx = lsx;
			}
			if (sy) {
				*sy = lsy;
			}
		}
		if (surface) {
			*surface = surf;
		}
		return;
	}

	struct kono_view *found = kono_view_at(server, lx, ly, surface, sx, sy);
	if (view) {
		*view = found;
	}
}

void kono_view_begin_interactive(struct kono_view *view, enum kono_cursor_mode mode, uint32_t edges) {
	struct kono_server *server = view->server;
	if (!view->floating && !view->fullscreen) {
		view->floating = true;
		kono_arrange(server);
	}
	server->grabbed_view = view;
	server->cursor_mode = mode;
	if (mode == KONO_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - view->x;
		server->grab_y = server->cursor->y - view->y;
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, "grabbing");
	} else {
		server->grab_x = server->cursor->x;
		server->grab_y = server->cursor->y;
		server->grab_geobox = (struct wlr_box){view->x, view->y, view->width, view->height};
		server->resize_edges = edges;
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, "grabbing");
	}
	kono_focus_view(view, NULL);
}

void kono_kill_active(void) {
	if (!g_server || !g_server->focused || !g_server->focused->mapped) {
		return;
	}
	wlr_xdg_toplevel_send_close(g_server->focused->xdg_toplevel);
}

void kono_toggle_floating(void) {
	if (!g_server || !g_server->focused || !g_server->focused->mapped) {
		return;
	}
	struct kono_view *view = g_server->focused;
	view->floating = !view->floating;
	view->fullscreen = false;
	if (view->floating) {
		if (view->width < 400) {
			view->width = 800;
		}
		if (view->height < 300) {
			view->height = 600;
		}
		struct kono_output *out = kono_output_preferred(g_server);
		if (out) {
			struct wlr_box box;
			wlr_output_layout_get_box(g_server->output_layout, out->wlr_output, &box);
			view->x = box.x + (box.width - view->width) / 2;
			view->y = box.y + (box.height - view->height) / 2;
		}
	}
	kono_arrange(g_server);
	kono_focus_view(view, NULL);
}

void kono_toggle_fullscreen(void) {
	if (!g_server || !g_server->focused || !g_server->focused->mapped) {
		return;
	}
	g_server->focused->fullscreen = !g_server->focused->fullscreen;
	kono_arrange(g_server);
	kono_focus_view(g_server->focused, NULL);
}

static void focus_workspace_top(struct kono_server *server) {
	struct kono_view *candidate = server->ws_focus[server->current_workspace];
	if (candidate && candidate->mapped && candidate->workspace == server->current_workspace) {
		kono_focus_view(candidate, NULL);
		return;
	}
	struct kono_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->mapped && view->workspace == server->current_workspace) {
			kono_focus_view(view, NULL);
			return;
		}
	}
	server->focused = NULL;
	wlr_seat_keyboard_notify_clear_focus(server->seat);
}

void kono_layer_unfocus(struct kono_server *server) {
	if (!server) {
		return;
	}
	server->exclusive_layer = NULL;
	if (server->focused && server->focused->mapped) {
		kono_focus_view(server->focused, NULL);
	}
}

void kono_switch_workspace(int ws) {
	if (!g_server || ws < 1 || ws > KONO_WORKSPACES) {
		return;
	}
	g_server->current_workspace = ws;
	struct kono_view *view;
	wl_list_for_each(view, &g_server->views, link) {
		wlr_scene_node_set_enabled(&view->scene_tree->node,
				view->mapped && view->workspace == ws);
	}
	kono_arrange(g_server);
	focus_workspace_top(g_server);
}

void kono_switch_workspace_arg(const char *arg) {
	if (!g_server || !arg || !arg[0]) {
		return;
	}
	int ws = g_server->current_workspace;
	if (arg[0] == 'e' && (arg[1] == '+' || arg[1] == '-')) {
		ws += atoi(arg + 1);
	} else if (arg[0] == '+' || arg[0] == '-') {
		ws += atoi(arg);
	} else {
		ws = atoi(arg);
	}
	while (ws > KONO_WORKSPACES) {
		ws -= KONO_WORKSPACES;
	}
	while (ws < 1) {
		ws += KONO_WORKSPACES;
	}
	kono_switch_workspace(ws);
}

void kono_move_to_workspace(int ws) {
	if (!g_server || !g_server->focused || ws < 1 || ws > KONO_WORKSPACES) {
		return;
	}
	g_server->focused->workspace = ws;
	wlr_scene_node_set_enabled(&g_server->focused->scene_tree->node, ws == g_server->current_workspace);
	kono_arrange(g_server);
	if (ws != g_server->current_workspace) {
		focus_workspace_top(g_server);
	}
}

void kono_move_focus(const char *dir) {
	if (!g_server) {
		return;
	}
	struct kono_view *cur = g_server->focused;
	if (!cur || !cur->mapped) {
		focus_workspace_top(g_server);
		return;
	}

	int cx = cur->x + cur->width / 2;
	int cy = cur->y + cur->height / 2;
	struct kono_view *best = NULL;
	int best_dist = INT_MAX;
	struct kono_view *view;
	wl_list_for_each(view, &g_server->views, link) {
		if (view == cur || !view->mapped || view->workspace != g_server->current_workspace) {
			continue;
		}
		int vx = view->x + view->width / 2;
		int vy = view->y + view->height / 2;
		int dx = vx - cx;
		int dy = vy - cy;
		bool ok = false;
		if ((dir[0] == 'l' || dir[0] == 'L') && dx < 0) {
			ok = true;
		} else if ((dir[0] == 'r' || dir[0] == 'R') && dx > 0) {
			ok = true;
		} else if ((dir[0] == 'u' || dir[0] == 'U') && dy < 0) {
			ok = true;
		} else if ((dir[0] == 'd' || dir[0] == 'D') && dy > 0) {
			ok = true;
		}
		if (!ok) {
			continue;
		}
		int dist = dx * dx + dy * dy;
		if (dist < best_dist) {
			best_dist = dist;
			best = view;
		}
	}
	if (best) {
		kono_focus_view(best, NULL);
	}
}

void kono_cycle_view(int delta) {
	if (!g_server || wl_list_empty(&g_server->views)) {
		return;
	}
	struct kono_view *start = g_server->focused;
	if (!start) {
		focus_workspace_top(g_server);
		return;
	}
	struct wl_list *link = &start->link;
	int guard = 0;
	do {
		link = (delta > 0) ? link->prev : link->next;
		if (link == &g_server->views) {
			link = (delta > 0) ? link->prev : link->next;
		}
		if (link == &g_server->views) {
			break;
		}
		struct kono_view *view = wl_container_of(link, view, link);
		if (view->mapped && view->workspace == g_server->current_workspace) {
			kono_focus_view(view, NULL);
			return;
		}
	} while (link != &start->link && ++guard < 256);
}

static void view_map(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_view *view = wl_container_of(listener, view, map);
	view->mapped = true;
	view->workspace = view->server->current_workspace;
	wlr_scene_node_set_enabled(&view->scene_tree->node, true);

	if (view->floating) {
		struct kono_output *out = kono_output_preferred(view->server);
		if (out) {
			struct wlr_box box;
			wlr_output_layout_get_box(view->server->output_layout, out->wlr_output, &box);
			if (view->width <= 0) {
				view->width = 800;
			}
			if (view->height <= 0) {
				view->height = 600;
			}
			view->x = box.x + (box.width - view->width) / 2;
			view->y = box.y + (box.height - view->height) / 2;
		}
	}

	kono_arrange(view->server);
	kono_focus_view(view, NULL);
	wlr_log(WLR_INFO, "Window mapped: %s", view->xdg_toplevel->app_id ? view->xdg_toplevel->app_id : "?");
}

static void view_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_view *view = wl_container_of(listener, view, unmap);
	view->mapped = false;
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	seat_reset_cursor_mode(view->server);
	if (view->server->focused == view) {
		view->server->focused = NULL;
	}
	if (view->server->ws_focus[view->workspace] == view) {
		view->server->ws_focus[view->workspace] = NULL;
	}
	kono_arrange(view->server);
	focus_workspace_top(view->server);
}

static void view_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_view *view = wl_container_of(listener, view, commit);
	if (view->xdg_toplevel->base->initial_commit) {
		wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
		struct kono_decoration *deco;
		wl_list_for_each(deco, &view->server->decorations, link) {
			if (deco->wlr_deco->toplevel == view->xdg_toplevel) {
				wlr_xdg_toplevel_decoration_v1_set_mode(deco->wlr_deco,
						WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
			}
		}
	}
}

static void view_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_view *view = wl_container_of(listener, view, destroy);
	wl_list_remove(&view->link);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->request_minimize.link);
	if (view->server->focused == view) {
		view->server->focused = NULL;
	}
	if (view->server->grabbed_view == view) {
		seat_reset_cursor_mode(view->server);
	}
	free(view);
}

static void view_request_move(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_view *view = wl_container_of(listener, view, request_move);
	kono_view_begin_interactive(view, KONO_CURSOR_MOVE, 0);
}

static void view_request_resize(struct wl_listener *listener, void *data) {
	struct kono_view *view = wl_container_of(listener, view, request_resize);
	struct wlr_xdg_toplevel_resize_event *event = data;
	kono_view_begin_interactive(view, KONO_CURSOR_RESIZE, event->edges);
}

static void view_request_maximize(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_view *view = wl_container_of(listener, view, request_maximize);
	if (view->xdg_toplevel->base->initialized) {
		wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, view->xdg_toplevel->requested.maximized);
	}
}

static void view_request_fullscreen(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_view *view = wl_container_of(listener, view, request_fullscreen);
	view->fullscreen = view->xdg_toplevel->requested.fullscreen;
	if (view->mapped) {
		kono_arrange(view->server);
	} else if (view->xdg_toplevel->base->initialized) {
		wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, view->fullscreen);
	}
}

static void view_request_minimize(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_view *view = wl_container_of(listener, view, request_minimize);
	if (view->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
	}
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct kono_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct kono_view *view = calloc(1, sizeof(*view));
	if (!view) {
		return;
	}
	view->server = server;
	view->xdg_toplevel = xdg_toplevel;
	view->workspace = server->current_workspace;
	view->width = 800;
	view->height = 600;

	view->scene_tree = wlr_scene_tree_create(server->layers[KONO_LAYER_TILE]);
	view->scene_tree->node.data = view;
	const float *col = server->config.inactive_border;
	int bw = server->config.border_size > 0 ? server->config.border_size : 1;
	view->borders[0] = wlr_scene_rect_create(view->scene_tree, bw, 1, col);
	view->borders[1] = wlr_scene_rect_create(view->scene_tree, 1, bw, col);
	view->borders[2] = wlr_scene_rect_create(view->scene_tree, bw, 1, col);
	view->borders[3] = wlr_scene_rect_create(view->scene_tree, 1, bw, col);
	view->content_tree = wlr_scene_xdg_surface_create(view->scene_tree, xdg_toplevel->base);
	view->content_tree->node.data = view;
	xdg_toplevel->base->data = view->content_tree;
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);

	view->map.notify = view_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &view->map);
	view->unmap.notify = view_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &view->unmap);
	view->commit.notify = view_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &view->commit);
	view->destroy.notify = view_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &view->destroy);
	view->request_move.notify = view_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &view->request_move);
	view->request_resize.notify = view_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &view->request_resize);
	view->request_maximize.notify = view_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &view->request_maximize);
	view->request_fullscreen.notify = view_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &view->request_fullscreen);
	view->request_minimize.notify = view_request_minimize;
	wl_signal_add(&xdg_toplevel->events.request_minimize, &view->request_minimize);

	wl_list_insert(&server->views, &view->link);
}

static void popup_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_popup *popup = wl_container_of(listener, popup, commit);
	if (!popup->xdg_popup->base->initial_commit) {
		return;
	}

	struct wlr_box box;
	wlr_output_layout_get_box(popup->server->output_layout, NULL, &box);
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(popup->xdg_popup->parent);
	if (parent && parent->data) {
		struct wlr_scene_tree *tree = parent->data;
		int lx, ly;
		wlr_scene_node_coords(&tree->node, &lx, &ly);
		box.x -= lx;
		box.y -= ly;
	}
	wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup, &box);
}

static void popup_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct kono_server *server = wl_container_of(listener, server, new_xdg_popup);
	struct wlr_xdg_popup *xdg_popup = data;

	struct wlr_scene_tree *parent_tree = NULL;
	if (xdg_popup->parent) {
		struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
		if (parent) {
			parent_tree = parent->data;
		} else {
			struct wlr_layer_surface_v1 *layer =
					wlr_layer_surface_v1_try_from_wlr_surface(xdg_popup->parent);
			if (layer && layer->data) {
				struct kono_layer *klayer = layer->data;
				parent_tree = klayer->popups ? klayer->popups : klayer->scene->tree;
			}
		}
	}
	if (!parent_tree) {
		parent_tree = server->layers[KONO_LAYER_OVERLAY];
	}
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	struct kono_popup *popup = calloc(1, sizeof(*popup));
	if (!popup) {
		return;
	}
	popup->server = server;
	popup->xdg_popup = xdg_popup;
	popup->commit.notify = popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void decoration_request_mode(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_decoration *deco = wl_container_of(listener, deco, request_mode);
	if (!deco->wlr_deco->toplevel->base->initialized) {
		return;
	}
	enum wlr_xdg_toplevel_decoration_v1_mode mode = deco->wlr_deco->requested_mode;
	if (mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE) {
		mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
	}
	wlr_xdg_toplevel_decoration_v1_set_mode(deco->wlr_deco, mode);
}

static void decoration_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct kono_decoration *deco = wl_container_of(listener, deco, destroy);
	wl_list_remove(&deco->link);
	wl_list_remove(&deco->request_mode.link);
	wl_list_remove(&deco->destroy.link);
	free(deco);
}

static void server_new_decoration(struct wl_listener *listener, void *data) {
	struct kono_server *server = wl_container_of(listener, server, new_decoration);
	struct wlr_xdg_toplevel_decoration_v1 *wlr_deco = data;
	struct kono_decoration *deco = calloc(1, sizeof(*deco));
	if (!deco) {
		return;
	}
	deco->wlr_deco = wlr_deco;
	deco->request_mode.notify = decoration_request_mode;
	wl_signal_add(&wlr_deco->events.request_mode, &deco->request_mode);
	deco->destroy.notify = decoration_destroy;
	wl_signal_add(&wlr_deco->events.destroy, &deco->destroy);
	wl_list_insert(&server->decorations, &deco->link);
	if (wlr_deco->toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(wlr_deco,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
	}
}

static void handle_request_activate(struct wl_listener *listener, void *data) {
	struct kono_server *server = wl_container_of(listener, server, request_activate);
	const struct wlr_xdg_activation_v1_request_activate_event *event = data;
	struct wlr_surface *surface = event->surface;
	if (!surface) {
		return;
	}
	surface = wlr_surface_get_root_surface(surface);
	struct kono_view *view = kono_view_from_surface(server, surface);
	if (view && view->mapped) {
		kono_focus_view(view, NULL);
	}
}

static void idle_reload_wallpaper(void *data) {
	wlr_log(WLR_INFO, "Applying wallpaper");
	kono_reload_wallpaper(data);
}

bool server_init(struct kono_server *server) {
	memset(server, 0, sizeof(*server));
	g_server = server;
	server->current_workspace = 1;

	signal(SIGINT, handle_sigint);
	signal(SIGTERM, handle_sigint);
	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	kono_ensure_dirs();
	load_kono_config(server, NULL);

	server->wl_display = wl_display_create();
	if (!server->wl_display) {
		wlr_log(WLR_ERROR, "Failed to create Wayland display");
		return false;
	}

	struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
	wl_event_loop_add_signal(loop, SIGUSR1, handle_sigusr1, server);
	server->backend = wlr_backend_autocreate(loop, &server->session);
	if (!server->backend) {
		wlr_log(WLR_ERROR, "Failed to create backend");
		return false;
	}

	server->renderer = wlr_renderer_autocreate(server->backend);
	if (!server->renderer) {
		wlr_log(WLR_ERROR, "Failed to create renderer");
		return false;
	}
	wlr_renderer_init_wl_display(server->renderer, server->wl_display);

	server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
	if (!server->allocator) {
		wlr_log(WLR_ERROR, "Failed to create allocator");
		return false;
	}

	wlr_compositor_create(server->wl_display, 6, server->renderer);
	wlr_subcompositor_create(server->wl_display);
	wlr_data_device_manager_create(server->wl_display);
	wlr_primary_selection_v1_device_manager_create(server->wl_display);
	wlr_viewporter_create(server->wl_display);
	wlr_presentation_create(server->wl_display, server->backend, 2);
	wlr_fractional_scale_manager_v1_create(server->wl_display, 1);

	server->output_layout = wlr_output_layout_create(server->wl_display);
	wlr_xdg_output_manager_v1_create(server->wl_display, server->output_layout);

	server->scene = wlr_scene_create();
	server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);
	for (int i = 0; i < KONO_LAYER_COUNT; i++) {
		server->layers[i] = wlr_scene_tree_create(&server->scene->tree);
	}
	struct wlr_linux_dmabuf_v1 *linux_dmabuf =
			wlr_linux_dmabuf_v1_create_with_renderer(server->wl_display, 4, server->renderer);
	if (linux_dmabuf) {
		wlr_scene_set_linux_dmabuf_v1(server->scene, linux_dmabuf);
	}

	wl_list_init(&server->outputs);
	wl_list_init(&server->views);
	wl_list_init(&server->keyboards);
	wl_list_init(&server->decorations);

	server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 5);
	server->new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
	server->new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);

	server->layer_shell = wlr_layer_shell_v1_create(server->wl_display, 4);
	server->new_layer_surface.notify = kono_handle_new_layer_surface;
	wl_signal_add(&server->layer_shell->events.new_surface, &server->new_layer_surface);

	server->xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server->wl_display);
	server->new_decoration.notify = server_new_decoration;
	wl_signal_add(&server->xdg_decoration_manager->events.new_toplevel_decoration,
			&server->new_decoration);

	server->xdg_activation = wlr_xdg_activation_v1_create(server->wl_display);
	server->request_activate.notify = handle_request_activate;
	wl_signal_add(&server->xdg_activation->events.request_activate, &server->request_activate);

	server->new_output.notify = kono_output_handle_new;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);

	seat_init(server);

	const char *socket = wl_display_add_socket_auto(server->wl_display);
	if (!socket) {
		wlr_log(WLR_ERROR, "Failed to add Wayland socket");
		return false;
	}
	server->socket = socket;
	setenv("WAYLAND_DISPLAY", socket, true);
	setenv("XDG_CURRENT_DESKTOP", "Kono", true);
	setenv("XDG_SESSION_DESKTOP", "Kono", true);
	setenv("DESKTOP_SESSION", "Kono", true);
	setenv("XDG_SESSION_TYPE", "wayland", true);

	if (!wlr_backend_start(server->backend)) {
		wlr_log(WLR_ERROR, "Failed to start backend");
		return false;
	}

	setenv("GDK_BACKEND", "wayland", true);
	setenv("QT_QPA_PLATFORM", "wayland", true);
	setenv("ELECTRON_OZONE_PLATFORM_HINT", "wayland", true);
	wl_event_loop_add_idle(loop, idle_reload_wallpaper, server);
	kono_run_exec_once();

	wlr_log(WLR_INFO, "Kono started on WAYLAND_DISPLAY=%s", socket);
	wlr_log(WLR_INFO, "Super+Return terminal, Super+E dolphin, Super+Q close, Super+M quit");
	return true;
}

void server_run(struct kono_server *server) {
	wl_display_run(server->wl_display);
}

void server_fini(struct kono_server *server) {
	wl_list_remove(&server->new_output.link);
	wl_list_remove(&server->new_xdg_toplevel.link);
	wl_list_remove(&server->new_xdg_popup.link);
	wl_list_remove(&server->new_layer_surface.link);
	wl_list_remove(&server->new_decoration.link);
	wl_list_remove(&server->new_input.link);
	wl_list_remove(&server->request_cursor.link);
	wl_list_remove(&server->request_set_selection.link);
	wl_list_remove(&server->request_set_primary_selection.link);
	wl_list_remove(&server->request_activate.link);
	wl_list_remove(&server->cursor_motion.link);
	wl_list_remove(&server->cursor_motion_absolute.link);
	wl_list_remove(&server->cursor_button.link);
	wl_list_remove(&server->cursor_axis.link);
	wl_list_remove(&server->cursor_frame.link);

	seat_fini(server);
	if (server->wallpaper_buffer) {
		wlr_buffer_drop(server->wallpaper_buffer);
		server->wallpaper_buffer = NULL;
	}
	wl_display_destroy_clients(server->wl_display);
	wlr_scene_node_destroy(&server->scene->tree.node);
	wlr_allocator_destroy(server->allocator);
	wlr_renderer_destroy(server->renderer);
	wlr_backend_destroy(server->backend);
	wl_display_destroy(server->wl_display);
}
