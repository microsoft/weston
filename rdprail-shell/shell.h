/*
 * Copyright © 2010-2012 Intel Corporation
 * Copyright © 2011-2012 Collabora, Ltd.
 * Copyright © 2013 Raspberry Pi Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <libweston/libweston.h>
#include <libweston/xwayland-api.h>
#include <libweston/weston-log.h>

#include "weston-rdprail-shell-server-protocol.h"

#define RDPRAIL_SHELL_DEBUG_LEVEL_NONE    0
#define RDPRAIL_SHELL_DEBUG_LEVEL_ERR     1
#define RDPRAIL_SHELL_DEBUG_LEVEL_WARN    2
#define RDPRAIL_SHELL_DEBUG_LEVEL_INFO    3
#define RDPRAIL_SHELL_DEBUG_LEVEL_DEBUG   4
#define RDPRAIL_SHELL_DEBUG_LEVEL_VERBOSE 5

#define RDPRAIL_SHELL_DEBUG_LEVEL_DEFAULT RDPRAIL_SHELL_DEBUG_LEVEL_INFO

/* To enable shell_rdp_debug message, add "--logger-scopes=rdprail-shell" */
#define shell_rdp_debug_verbose(b, ...) \
	if (b->debugLevel >= RDPRAIL_SHELL_DEBUG_LEVEL_VERBOSE) \
		shell_rdp_debug_print(b->debug, false, __VA_ARGS__)
#define shell_rdp_debug(b, ...) \
	if (b->debugLevel >= RDPRAIL_SHELL_DEBUG_LEVEL_INFO) \
		shell_rdp_debug_print(b->debug, false, __VA_ARGS__)
#define shell_rdp_debug_error(b, ...) \
	if (b->debugLevel >= RDPRAIL_SHELL_DEBUG_LEVEL_ERR) \
		shell_rdp_debug_print(b->debug, false, __VA_ARGS__)

#define is_system_distro() (getenv("WSL2_VM_ID") != NULL)

struct workspace {
	struct weston_layer layer;

	struct wl_list focus_list;
	struct wl_listener seat_destroyed_listener;
};

struct shell_output {
	struct desktop_shell  *shell;
	struct weston_output  *output;
	struct wl_listener    destroy_listener;
	struct wl_list        link;

	pixman_rectangle32_t  desktop_workarea;
};

struct weston_desktop;
struct desktop_shell {
	struct weston_compositor *compositor;
	struct weston_desktop *desktop;
	const struct weston_xwayland_surface_api *xwayland_surface_api;

	struct wl_listener transform_listener;
	struct wl_listener destroy_listener;
	struct wl_listener show_input_panel_listener;

	struct weston_layer fullscreen_layer;

	struct wl_listener pointer_focus_listener;
	struct weston_surface *grab_surface;

	struct {
		struct wl_client *client;
		struct wl_resource *desktop_shell;
		struct wl_listener client_destroy_listener;

		unsigned deathcount;
		struct timespec deathstamp;
	} child;

	bool prepare_event_sent;

	struct text_backend *text_backend;

	struct {
		struct weston_surface *surface;
		pixman_box32_t cursor_rectangle;
	} text_input;

	struct {
		struct wl_array array;
		unsigned int current;
		unsigned int num;

		struct wl_list client_list;
	} workspaces;

	struct {
		struct wl_resource *binding;
	} input_panel;

	bool allow_zap;
	bool allow_alt_f4_to_close_app;
	uint32_t binding_modifier;

	struct weston_layer minimized_layer;

	struct wl_listener seat_create_listener;
	struct wl_listener output_create_listener;
	struct wl_listener output_move_listener;
	struct wl_list output_list;

	char *client;

	struct timespec startup_time;

	bool is_localmove_supported;
	bool is_localmove_pending;

	void *app_list_context;
	char *distroName;
	size_t distroNameLength;
	bool is_appid_with_distro_name;

	pixman_image_t *image_default_app_icon;
	pixman_image_t *image_default_app_overlay_icon;

	bool is_blend_overlay_icon_taskbar;
	bool is_blend_overlay_icon_app_list;

	struct weston_surface *focus_proxy_surface;

	const struct weston_rdprail_api *rdprail_api;
	void *rdp_backend;

	bool use_wslpath;

	struct weston_log_scope *debug;
	uint32_t debugLevel;
};

struct weston_output *
get_default_output(struct weston_compositor *compositor);

struct weston_view *
get_default_view(struct weston_surface *surface);

struct shell_surface *
get_shell_surface(struct weston_surface *surface);

struct workspace *
get_current_workspace(struct desktop_shell *shell);

void
lower_fullscreen_layer(struct desktop_shell *shell,
		       struct weston_output *lowering_output);

void
activate(struct desktop_shell *shell, struct weston_view *view,
	 struct weston_seat *seat, uint32_t flags);

int
input_panel_setup(struct desktop_shell *shell);
void
input_panel_destroy(struct desktop_shell *shell);

typedef void (*shell_for_each_layer_func_t)(struct desktop_shell *,
					    struct weston_layer *, void *);

void
shell_for_each_layer(struct desktop_shell *shell,
		     shell_for_each_layer_func_t func,
		     void *data);

void
shell_blend_overlay_icon(struct desktop_shell *shell,
			 pixman_image_t *app_image,
			 pixman_image_t *overlay_image);

void
shell_rdp_debug_print(struct weston_log_scope *scope, bool cont, char *fmt, ...);

// app-list.c
void app_list_init(struct desktop_shell *shell);
void app_list_destroy(struct desktop_shell *shell);
pixman_image_t *app_list_load_icon_file(struct desktop_shell *shell, const char *key);
bool app_list_start_backend_update(struct desktop_shell *shell, char *clientLanguageId);
void app_list_stop_backend_update(struct desktop_shell *shell);
void app_list_find_image_name(struct desktop_shell *shell, pid_t pid, char *image_name, size_t image_name_size, bool is_wayland);
void app_list_associate_window_app_id(struct desktop_shell *shell, pid_t pid, char *app_id, uint32_t window_id);
// img-load.c
pixman_image_t *load_image_svg(struct desktop_shell *shell, const void *data, uint32_t data_len, const char *filename);
void *load_file_svg(struct desktop_shell *shell, const char *filename, uint32_t *data_len);
