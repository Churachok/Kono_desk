#ifndef KONO_SERVER_H
#define KONO_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>

#define KONO_WORKSPACES 9
#define KONO_BAR_HEIGHT 32

enum kono_cursor_mode {
	KONO_CURSOR_PASSTHROUGH,
	KONO_CURSOR_MOVE,
	KONO_CURSOR_RESIZE,
};

enum kono_scene_layer {
	KONO_LAYER_BACKGROUND = 0,
	KONO_LAYER_BOTTOM,
	KONO_LAYER_TILE,
	KONO_LAYER_FLOAT,
	KONO_LAYER_TOP,
	KONO_LAYER_OVERLAY,
	KONO_LAYER_COUNT,
};

struct kono_config {
	int gaps_in;
	int gaps_out;
	int border_size;
	float active_border[4];
	float inactive_border[4];
	float background[4];
	float bar_bg[4];
	float bar_active[4];
	float bar_inactive[4];
	float bar_occupied[4];
	bool builtin_bar;
	char kb_layout[64];
	char kb_variant[64];
	char kb_options[128];
};

struct kono_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_xdg_shell *xdg_shell;
	struct wlr_layer_shell_v1 *layer_shell;
	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;

	struct wlr_output_layout *output_layout;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_scene_tree *layers[KONO_LAYER_COUNT];

	struct wl_list outputs;
	struct wl_list views;
	struct wl_list keyboards;
	struct wl_list decorations;

	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor_manager;

	enum kono_cursor_mode cursor_mode;
	struct kono_view *grabbed_view;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

	int current_workspace;
	struct kono_view *focused;
	struct kono_view *ws_focus[KONO_WORKSPACES + 1];

	char wallpaper_path[512];
	struct wlr_buffer *wallpaper_buffer;
	struct kono_layer *exclusive_layer;
	struct wlr_xdg_activation_v1 *xdg_activation;

	struct kono_config config;

	struct wl_listener new_output;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_listener new_layer_surface;
	struct wl_listener new_decoration;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	struct wl_listener request_activate;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	const char *socket;
};

struct kono_output {
	struct wl_list link;
	struct kono_server *server;
	struct wlr_output *wlr_output;
	struct wlr_scene_rect *wallpaper;
	struct wlr_scene_buffer *wallpaper_image;
	struct wlr_scene_tree *bar_tree;
	struct wlr_scene_rect *bar_bg;
	struct wlr_scene_rect *bar_ws[KONO_WORKSPACES];
	struct wlr_box usable;
	struct wl_list layers;

	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct kono_view {
	struct wl_list link;
	struct kono_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	struct wlr_scene_tree *content_tree;
	struct wlr_scene_rect *borders[4];

	int workspace;
	bool mapped;
	bool floating;
	bool fullscreen;
	int x, y, width, height;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_minimize;
};

struct kono_layer {
	struct wl_list link;
	struct kono_server *server;
	struct kono_output *output;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene;
	struct wlr_scene_tree *popups;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener new_popup;
};

struct kono_keyboard {
	struct wl_list link;
	struct kono_server *server;
	struct wlr_keyboard *wlr_keyboard;

	struct wl_listener key;
	struct wl_listener modifiers;
	struct wl_listener destroy;
};

struct kono_popup {
	struct kono_server *server;
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct kono_decoration {
	struct wl_list link;
	struct wlr_xdg_toplevel_decoration_v1 *wlr_deco;
	struct wl_listener request_mode;
	struct wl_listener destroy;
};

extern struct kono_server *g_server;

bool server_init(struct kono_server *server);
void server_run(struct kono_server *server);
void server_fini(struct kono_server *server);

void kono_output_handle_new(struct wl_listener *listener, void *data);
void kono_handle_new_layer_surface(struct wl_listener *listener, void *data);
void kono_output_update_bar(struct kono_output *output);
void kono_output_arrange_layers(struct kono_output *output);
struct kono_output *kono_output_at(struct kono_server *server, double lx, double ly);
struct kono_output *kono_output_preferred(struct kono_server *server);
void kono_output_usable_area(struct kono_output *output, struct wlr_box *box);

void seat_init(struct kono_server *server);
void seat_fini(struct kono_server *server);
void seat_process_cursor_motion(struct kono_server *server, uint32_t time);
void seat_reset_cursor_mode(struct kono_server *server);
void seat_set_default_cursor(struct kono_server *server);

void kono_focus_view(struct kono_view *view, struct wlr_surface *surface);
void kono_arrange(struct kono_server *server);
void kono_view_set_border_color(struct kono_view *view, const float color[4]);
void kono_view_begin_interactive(struct kono_view *view, enum kono_cursor_mode mode, uint32_t edges);
struct kono_view *kono_view_at(struct kono_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy);
void kono_pointer_at(struct kono_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy, struct kono_view **view);
struct kono_view *kono_view_from_surface(struct kono_server *server, struct wlr_surface *surface);

void kono_spawn(const char *cmd);
void kono_kill_active(void);
void kono_quit(void);
void kono_toggle_floating(void);
void kono_toggle_fullscreen(void);
void kono_switch_workspace(int ws);
void kono_switch_workspace_arg(const char *arg);
void kono_move_to_workspace(int ws);
void kono_move_focus(const char *dir);
void kono_cycle_view(int delta);
void kono_layer_unfocus(struct kono_server *server);

bool load_kono_config(struct kono_server *server, const char *path);
void kono_config_defaults(struct kono_config *cfg);
void kono_config_dir(char *out, size_t n);
void kono_ensure_dirs(void);
bool handle_keybinding(struct kono_server *server, xkb_keysym_t sym, uint32_t mods);
void kono_run_exec_once(void);

bool kono_set_wallpaper(struct kono_server *server, const char *path);
void kono_reload_wallpaper(struct kono_server *server);
void kono_output_apply_wallpaper(struct kono_output *output);

#endif
