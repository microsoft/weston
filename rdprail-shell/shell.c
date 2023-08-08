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

#include "config.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/input.h>
#include <assert.h>
#include <signal.h>
#include <math.h>
#include <sys/types.h>
#include <sys/time.h>
#include <errno.h>

#include "shell.h"
#include "compositor/weston.h"
#include "weston-rdprail-shell-server-protocol.h"
#include <libweston/config-parser.h>
#include "shared/helpers.h"
#include "shared/timespec-util.h"
#include "shared/image-loader.h"
#include <libweston-desktop/libweston-desktop.h>
#include <libweston/libweston.h>
#include <libweston/backend.h>
#include <libweston/backend-rdp.h>

struct focus_state {
	struct desktop_shell *shell;
	struct weston_seat *seat;
	struct workspace *ws;
	struct weston_surface *keyboard_focus;
	struct wl_list link;
	struct wl_listener seat_destroy_listener;
	struct wl_listener surface_destroy_listener;
};

/*
 * Surface stacking and ordering.
 *
 * This is handled using several linked lists of surfaces, organised into
 * ‘layers’. The layers are ordered, and each of the surfaces in one layer are
 * above all of the surfaces in the layer below. The set of layers is static and
 * in the following order (top-most first):
 *  • Cursor layer
 *  • Fullscreen layer
 *  • Workspace layers
 *
 * The list of layers may be manipulated to remove whole layers of surfaces from
 * display. For example, when locking the screen, all layers except the lock
 * layer are removed.
 *
 * A surface’s layer is modified on configuring the surface, in
 * set_surface_type() (which is only called when the surface’s type change is
 * _committed_). If a surface’s type changes (e.g. when making a window
 * fullscreen) its layer changes too.
 *
 * In order to allow popup and transient surfaces to be correctly stacked above
 * their parent surfaces, each surface tracks both its parent surface, and a
 * linked list of its children. When a surface’s layer is updated, so are the
 * layers of its children. Note that child surfaces are *not* the same as
 * subsurfaces — child/parent surfaces are purely for maintaining stacking
 * order.
 *
 * The children_link list of siblings of a surface (i.e. those surfaces which
 * have the same parent) only contains weston_surfaces which have a
 * shell_surface. Stacking is not implemented for non-shell_surface
 * weston_surfaces. This means that the following implication does *not* hold:
 *     (shsurf->parent != NULL) ⇒ !wl_list_is_empty(shsurf->children_link)
 */

struct shell_surface {
	struct wl_signal destroy_signal;

	struct weston_desktop_surface *desktop_surface;
	struct weston_view *view;
	int32_t last_width, last_height;

	struct desktop_shell *shell;

	struct shell_surface *parent;
	struct wl_list children_list;
	struct wl_list children_link;

	int32_t saved_x, saved_y;
	bool saved_position_valid;
	uint32_t saved_showstate;
	bool saved_showstate_valid;
	bool saved_rotation_valid;
	int unresponsive, grabbed;
	uint32_t resize_edges;

	struct {
		struct weston_transform transform;
		struct weston_matrix rotation;
	} rotation;

	struct {
		struct weston_transform transform; /* matrix from x, y */
		struct weston_view *black_view;
	} fullscreen;

	struct {
		bool grab_unmaximized;
		int32_t saved_surface_width;
		int32_t saved_width;
		int32_t saved_height;
	} maximized;

	struct weston_output *fullscreen_output;
	struct weston_output *output;
	struct wl_listener output_destroy_listener;

	struct surface_state {
		bool fullscreen;
		bool maximized;
		bool lowered;
	} state;

	struct {
		bool is_set;
		int32_t x;
		int32_t y;
	} xwayland;

	int focus_count;

	bool destroying;

	struct {
		bool is_snapped;
		bool is_maximized_requested;
		int x;
		int y;
		int width;
		int height;
		int saved_x;
		int saved_y;
		int saved_surface_width;
		int saved_width; // based on window geometry
		int saved_height; // based on window geometry
		int last_grab_x;
		int last_grab_y;
	} snapped;

	struct {
		bool is_default_icon_used;
		bool is_icon_set;
	} icon;

	struct {
		bool is_window_app_id_associated;
	} app_id;

	struct wl_listener metadata_listener;
};

struct shell_grab {
	struct weston_pointer_grab grab;
	struct shell_surface *shsurf;
	struct wl_listener shsurf_destroy_listener;
};

struct shell_touch_grab {
	struct weston_touch_grab grab;
	struct shell_surface *shsurf;
	struct wl_listener shsurf_destroy_listener;
	struct weston_touch *touch;
};

struct weston_move_grab {
	struct shell_grab base;
	wl_fixed_t dx, dy;
	bool client_initiated;
};

struct weston_touch_move_grab {
	struct shell_touch_grab base;
	int active;
	wl_fixed_t dx, dy;
};

struct rotate_grab {
	struct shell_grab base;
	struct weston_matrix rotation;
	struct {
		float x;
		float y;
	} center;
};

struct shell_seat {
	struct weston_seat *seat;
	struct desktop_shell *shell;
	struct wl_listener seat_destroy_listener;
	struct weston_surface *focused_surface;

	struct wl_listener caps_changed_listener;
	struct wl_listener pointer_focus_listener;
	struct wl_listener keyboard_focus_listener;
};

static const struct weston_pointer_grab_interface move_grab_interface;

static void set_maximized(struct shell_surface *shsurf, bool maximized);

static void grab_unsnap_motion(struct weston_pointer_grab *grab);
static void grab_unmaximized_motion(struct weston_pointer_grab *grab);

static struct desktop_shell *
shell_surface_get_shell(struct shell_surface *shsurf);

static void
set_busy_cursor(struct shell_surface *shsurf, struct weston_pointer *pointer);

static void
surface_rotate(struct shell_surface *surface, struct weston_pointer *pointer);

static struct shell_seat *
get_shell_seat(struct weston_seat *seat);

static struct shell_output *
find_shell_output_from_weston_output(struct desktop_shell *shell,
				     struct weston_output *output);

static void
shell_surface_update_child_surface_layers(struct shell_surface *shsurf);

static void
shell_backend_request_window_activate(void *shell_context, struct weston_seat *seat, struct weston_surface *surface);

static void
shell_backend_request_window_close(struct weston_surface *surface);

static void launch_desktop_shell_process(void *data);

#define ICON_STRIDE( W, BPP ) ((((W) * (BPP) + 31) / 32) * 4)

#define TITLEBAR_GRAB_MARGIN_X (30)
#define TITLEBAR_GRAB_MARGIN_Y (10)

static int cached_tm_mday = -1;

static char *
shell_rdp_log_timestamp(char *buf, size_t len)
{
	struct timeval tv;
	struct tm *brokendown_time;
	char datestr[128];
	char timestr[128];

	gettimeofday(&tv, NULL);

	brokendown_time = localtime(&tv.tv_sec);
	if (brokendown_time == NULL) {
		snprintf(buf, len, "%s", "[(NULL)localtime] ");
		return buf;
	}

	memset(datestr, 0, sizeof(datestr));
	if (brokendown_time->tm_mday != cached_tm_mday) {
		strftime(datestr, sizeof(datestr), "Date: %Y-%m-%d %Z\n",
			 brokendown_time);
		cached_tm_mday = brokendown_time->tm_mday;
	}

	strftime(timestr, sizeof(timestr), "%H:%M:%S", brokendown_time);
	/* if datestr is empty it prints only timestr*/
	snprintf(buf, len, "%s[%s.%03li]", datestr,
		 timestr, (tv.tv_usec / 1000));

	return buf;
}

void
shell_rdp_debug_print(struct weston_log_scope *scope, bool cont, char *fmt, ...)
{
	if (scope && weston_log_scope_is_enabled(scope)) {
		va_list ap;
		va_start(ap, fmt);
		if (cont) {
			weston_log_scope_vprintf(scope, fmt, ap);
		} else {
			char timestr[128];
			int len_va;
			char *str;
			shell_rdp_log_timestamp(timestr, sizeof(timestr));
			len_va = vasprintf(&str, fmt, ap);
			if (len_va >= 0) {
				weston_log_scope_printf(scope, "%s %s",
							timestr, str);
				free(str);
			} else {
				const char *oom = "Out of memory";
				weston_log_scope_printf(scope, "%s %s",
							timestr, oom);
			}
		}
		va_end(ap);
	}
}

void
shell_blend_overlay_icon(struct desktop_shell *shell, pixman_image_t *app_image, pixman_image_t *overlay_image)
{
	int app_width, overlay_width;
	int app_height, overlay_height;
	double overlay_scale_width, overlay_scale_height;
	pixman_transform_t transform;

	/* can't overlay to itself */
	assert(app_image);
	assert(overlay_image);
	assert(app_image != overlay_image);

	app_width = pixman_image_get_width(app_image);
	app_height = pixman_image_get_height(app_image);
	if (!app_width || !app_height)
		return;

	overlay_width = pixman_image_get_width(overlay_image);
	overlay_height = pixman_image_get_height(overlay_image);
	if (!overlay_width || !overlay_height)
		return;

	overlay_scale_width = 1.0f / (((double)app_width / overlay_width) / 1.75f);
	overlay_scale_height = 1.0f / (((double)app_height / overlay_height) / 1.75f);

	shell_rdp_debug_verbose(shell, "%s: app %dx%d; overlay %dx%d; scale %4.2fx%4.2f\n",
		__func__, app_width, app_height, overlay_width, overlay_height,
		overlay_scale_width, overlay_scale_height);

	pixman_transform_init_scale(&transform,
				    pixman_double_to_fixed(overlay_scale_width),
				    pixman_double_to_fixed(overlay_scale_height));
	pixman_image_set_transform(overlay_image, &transform);
	pixman_image_set_filter(overlay_image, PIXMAN_FILTER_BILINEAR, NULL, 0);

	pixman_image_composite32(PIXMAN_OP_OVER,
			overlay_image, /* src */
			NULL, /* mask */
			app_image, /* dest */
			0, 0, /* src_x, src_y */
			0, 0, /* mask_x, mask_y */
			app_width/2 , app_height/2, /* dest_x, dest_y */
			app_width, /* width */
			app_height /* height */);

	pixman_image_set_filter(overlay_image, PIXMAN_FILTER_NEAREST, NULL, 0);
	pixman_image_set_transform(overlay_image, NULL);
}

static void
shell_surface_set_window_icon(struct weston_desktop_surface *desktop_surface,
				int32_t width, int32_t height, int32_t bpp,
				void *bits, void *user_data)
{
	struct shell_surface *shsurf;
	struct weston_surface *surface;
	const struct weston_xwayland_surface_api *api;
	pixman_image_t *image = NULL;
	pixman_format_code_t format;
	const char *id;
	char *class_name;

	shsurf = weston_desktop_surface_get_user_data(desktop_surface);
	if (!shsurf)
		return;

	surface = weston_desktop_surface_get_surface(desktop_surface);
	if (!surface)
		return;

	if (shsurf->shell->rdprail_api->set_window_icon) {
		if (width && height && bpp && bits) {
			/* When caller supplied custom image, it's always be used */
			switch (bpp) {
				case 32:
					format = PIXMAN_a8r8g8b8;
					break;
				default:
					shell_rdp_debug_error(shsurf->shell, "%s: unsupported bpp: %d\n", __func__, bpp);
					return;
			}
			image = pixman_image_create_bits_no_clear(format,
				width, height, bits, ICON_STRIDE(width, bpp));
			if (!image) {
				shell_rdp_debug_error(shsurf->shell, "%s: pixman_image_create_bits_no_clear failed\n", __func__);
				return;
			}
			shsurf->icon.is_default_icon_used = false;
		}
		if (!image) {
			/* If this is X app, query from X first */
			api = shsurf->shell->xwayland_surface_api;
			if (!api) {
				api = weston_xwayland_surface_get_api(shsurf->shell->compositor);
				shsurf->shell->xwayland_surface_api = api;
			}
			if (api && api->is_xwayland_surface(surface)) {
				/* trigger_set_window_icon calls back this function
				   with custom icon image obtained from X app. */
				if (api->trigger_set_window_icon(surface))
					return;
			}
		}
		if (!image) {
			/* Next, try icon from .desktop file */
			id = weston_desktop_surface_get_app_id(desktop_surface);
			if (id)
				image = app_list_load_icon_file(shsurf->shell, id);
			if (image)
				shsurf->icon.is_default_icon_used = false;
		}
		if (!image) {
			/* If this is X app, try window class name as id for icon */
			if (api && api->is_xwayland_surface(surface)) {
				class_name = api->get_class_name(surface);
				if (class_name) {
					image = app_list_load_icon_file(shsurf->shell, class_name);
					if (image)
						shsurf->icon.is_default_icon_used = false;
					free(class_name);
				}
			}
		}
		if (!image) {
			/* When caller doens't supply custom image, look for default images */
			image = shsurf->shell->image_default_app_icon;
			if (image) {
				pixman_image_ref(image);
				shsurf->icon.is_default_icon_used = true;
			}
		}
		if (!image)
			return;
		/* no need to blend default icon as it's already pre-blended if requested. */
		if (shsurf->shell->is_blend_overlay_icon_taskbar &&
		    shsurf->shell->image_default_app_icon != image &&
		    shsurf->shell->image_default_app_overlay_icon)
			shell_blend_overlay_icon(shsurf->shell,
						 image,
						 shsurf->shell->image_default_app_overlay_icon);
		shsurf->shell->rdprail_api->set_window_icon(surface, image);
		pixman_image_unref(image);
	}
}

static int
shell_surface_get_label(struct weston_surface *surface, char *buf, size_t len)
{
	const char *t, *c;
	struct weston_desktop_surface *desktop_surface =
		weston_surface_get_desktop_surface(surface);
	struct shell_surface *shsurf =
		shsurf = get_shell_surface(surface);

	t = weston_desktop_surface_get_title(desktop_surface);
	c = weston_desktop_surface_get_app_id(desktop_surface);

	return snprintf(buf, len, "%s window%s%s%s%s%s",
		shsurf && shsurf->parent ? "child" : "top-level",
		t ? " '" : "", t ?: "", t ? "'" : "",
		c ? " of " : "", c ?: "");
}

static void
destroy_shell_grab_shsurf(struct wl_listener *listener, void *data)
{
	struct shell_grab *grab;

	grab = container_of(listener, struct shell_grab,
			    shsurf_destroy_listener);

	grab->shsurf = NULL;
}

struct weston_view *
get_default_view(struct weston_surface *surface)
{
	struct shell_surface *shsurf;
	struct weston_view *view;

	if (!surface || wl_list_empty(&surface->views))
		return NULL;

	shsurf = get_shell_surface(surface);
	if (shsurf)
		return shsurf->view;

	wl_list_for_each(view, &surface->views, surface_link)
		if (weston_view_is_mapped(view))
			return view;

	return container_of(surface->views.next, struct weston_view, surface_link);
}

static void
shell_send_minmax_info(struct weston_surface *surface)
{
	struct shell_surface *shsurf = get_shell_surface(surface);
	struct desktop_shell *shell;
	struct weston_output *output;
	struct weston_rdp_rail_window_pos maxPosSize;
	struct weston_size min_size;
	struct weston_size max_size;

	if (!shsurf)
		return;

	shell = shsurf->shell;

	if (shell->rdprail_api->send_window_minmax_info) {
		/* minmax info is based on primary monitor */
		/* https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-minmaxinfo */
		output = get_default_output(shell->compositor);
		assert(output);

		maxPosSize.x = output->x;
		maxPosSize.y = output->y;
		maxPosSize.width = output->width;
		maxPosSize.height = output->height;

		min_size = weston_desktop_surface_get_min_size(shsurf->desktop_surface);
		max_size = weston_desktop_surface_get_max_size(shsurf->desktop_surface);
		if (max_size.width == 0)
			max_size.width = output->width;
		if (max_size.height == 0)
			max_size.height = output->height;

		shell->rdprail_api->send_window_minmax_info(
			weston_desktop_surface_get_surface(shsurf->desktop_surface),
			&maxPosSize, &min_size, &max_size);
	}
}

static void
shell_grab_start(struct shell_grab *grab,
		 const struct weston_pointer_grab_interface *interface,
		 struct shell_surface *shsurf,
		 struct weston_pointer *pointer,
		 enum weston_rdprail_shell_cursor cursor)
{
	struct desktop_shell *shell = shsurf->shell;

	weston_seat_break_desktop_grabs(pointer->seat);

	grab->grab.interface = interface;
	grab->shsurf = shsurf;
	grab->shsurf_destroy_listener.notify = destroy_shell_grab_shsurf;
	wl_signal_add(&shsurf->destroy_signal,
		      &grab->shsurf_destroy_listener);

	shsurf->grabbed = 1;
	weston_pointer_start_grab(pointer, &grab->grab);

	if (shell->is_localmove_supported &&
		(interface == &move_grab_interface) && 
		shell->rdprail_api->start_window_move) {

		shell->is_localmove_pending = true;

		shell_send_minmax_info(
			weston_desktop_surface_get_surface(shsurf->desktop_surface));

		shell->rdprail_api->start_window_move(
			weston_desktop_surface_get_surface(shsurf->desktop_surface),
			wl_fixed_to_int(pointer->grab_x),
			wl_fixed_to_int(pointer->grab_y));
	}
}

static void
shell_grab_end(struct shell_grab *grab)
{
	if (grab->shsurf) {
		struct desktop_shell* shell = grab->shsurf->shell;
		struct weston_surface* surface =
		weston_desktop_surface_get_surface(grab->shsurf->desktop_surface);		

		wl_list_remove(&grab->shsurf_destroy_listener.link);
		grab->shsurf->grabbed = 0;

		if (shell->is_localmove_supported &&
			(grab->grab.interface == &move_grab_interface) &&
			shell->rdprail_api->end_window_move) {
			
			grab->shsurf->snapped.last_grab_x = wl_fixed_to_int(grab->grab.pointer->x);
			grab->shsurf->snapped.last_grab_y = wl_fixed_to_int(grab->grab.pointer->y);
			
			shell->rdprail_api->end_window_move(surface);
		}

		if (grab->shsurf->resize_edges) {
			grab->shsurf->resize_edges = 0;
		} else {
			/* This is nessesary to make "double check" on title bar to max/restore
			   with X applications. When title bar is clicked first time, Xwayland 
			   enters "grab_move" (see where FRAME_STATUS_MOVE), and when left button
			   is released, grab_move ends. On desktop-shell, when entered grab_move,
			   the focus is moved to shell "grab_surface" (see desktop-shell/shell.c:
			   shell_grab_start() where setting focus to shell->grab_surface.
			   But RDP shell doesn't have expclicit grab surface, thus
			   focus remains at the window who owned the title bar clicked.
			   This itself is OK, but when X and Xwayland depends on seeing
			   focus change when mouse button is released when grab_move ends,
			   so that they can recognized that mouse button is released *without
			   receiving explicit mouse message* using weston_pointer_send_button.
			   thus, here patch pointer's sx/sy to (0,0), and this trigger refocus
			   at weston_pointer_set_focus even focus isn't changed, and sx/sy will
			   be updated at weston_pointer_set_focus. */
			grab->grab.pointer->sx = 0;
			grab->grab.pointer->sy = 0;
		}
	}

	weston_pointer_end_grab(grab->grab.pointer);
}

static void
shell_touch_grab_start(struct shell_touch_grab *grab,
		       const struct weston_touch_grab_interface *interface,
		       struct shell_surface *shsurf,
		       struct weston_touch *touch)
{
	weston_seat_break_desktop_grabs(touch->seat);

	grab->grab.interface = interface;
	grab->shsurf = shsurf;
	grab->shsurf_destroy_listener.notify = destroy_shell_grab_shsurf;
	wl_signal_add(&shsurf->destroy_signal,
		      &grab->shsurf_destroy_listener);

	grab->touch = touch;
	shsurf->grabbed = 1;

	weston_touch_start_grab(touch, &grab->grab);
}

static void
shell_touch_grab_end(struct shell_touch_grab *grab)
{
	if (grab->shsurf) {
		wl_list_remove(&grab->shsurf_destroy_listener.link);
		grab->shsurf->grabbed = 0;
	}

	weston_touch_end_grab(grab->touch);
}

static void
get_output_work_area(struct desktop_shell *shell,
		     struct weston_output *output,
		     pixman_rectangle32_t *area)
{
	if (!output) {
		area->x = 0;
		area->y = 0;
		area->width = 0;
		area->height = 0;

		return;
	}

	struct shell_output *shell_output =
		find_shell_output_from_weston_output(shell, output);

	if (shell_output) {
		*area = shell_output->desktop_workarea;
	} else {
		area->x = output->x;
		area->y = output->y;
		area->width = output->width;
		area->height = output->height;
	}

	return;
}

static void
center_on_output(struct weston_view *view,
		 struct weston_output *output);

static enum weston_keyboard_modifier
get_modifier(char *modifier)
{
	if (!modifier)
		return 0; // default to no binding-modifier.

	if (!strcmp("ctrl", modifier))
		return MODIFIER_CTRL;
	else if (!strcmp("alt", modifier))
		return MODIFIER_ALT;
	else if (!strcmp("super", modifier))
		return MODIFIER_SUPER;
	else if (!strcmp("none", modifier))
		return 0;
	else
		return 0; // default to no binding-modifier.
}

static bool
read_rdpshell_config_bool(char *config_name, bool default_value)
{
	char *s;

	s = getenv(config_name);
	if (s) {
		if (strcmp(s, "true") == 0)
			return true;
		else if (strcmp(s, "false") == 0)
			return false;
		else if (strcmp(s, "1") == 0)
			return true;
		else if (strcmp(s, "0") == 0)
			return false;
	}

	return default_value;
}

static void
shell_configuration(struct desktop_shell *shell)
{
	struct weston_config_section *section;
	char *s, *client;
	bool allow_zap;
	bool allow_alt_f4_to_close_app;
	bool is_localmove_supported;

	section = weston_config_get_section(wet_get_config(shell->compositor),
					    "shell", NULL, NULL);

	client = wet_get_libexec_path("weston-rdprail-shell");
	weston_config_section_get_string(section, "client", &s, client);
	free(client);
	shell->client = s;

	/* default to not allow zap */
	weston_config_section_get_bool(section,
				       "allow-zap", &allow_zap, false);
	allow_zap = read_rdpshell_config_bool("WESTON_RDPRAIL_SHELL_ALLOW_ZAP", allow_zap);
	shell->allow_zap = allow_zap;
	shell_rdp_debug(shell, "RDPRAIL-shell: allow-zap:%d\n", shell->allow_zap);

	/* default to allow alt+F4 to close app */
	weston_config_section_get_bool(section,
				       "alt-f4-to-close-app", &allow_alt_f4_to_close_app, true);
	allow_alt_f4_to_close_app = read_rdpshell_config_bool("WESTON_RDPRAIL_SHELL_ALLOW_ALT_F4_TO_CLOSE_APP", allow_alt_f4_to_close_app);
	shell->allow_alt_f4_to_close_app = allow_alt_f4_to_close_app;
	shell_rdp_debug(shell, "RDPRAIL-shell: allow-alt-f4-to-close-app:%d\n", shell->allow_alt_f4_to_close_app);

	/* set "none" to default to disable optional key-bindings */
	weston_config_section_get_string(section,
					 "binding-modifier", &s, "none");
	shell->binding_modifier = get_modifier(s);
	shell_rdp_debug(shell, "RDPRAIL-shell: binding-modifier:%s\n", s);
	free(s);

	/* default to disable local move (not fully supported yet */
	weston_config_section_get_bool(section,
				       "local-move", &is_localmove_supported, false);
	is_localmove_supported = read_rdpshell_config_bool(
		"WESTON_RDPRAIL_SHELL_LOCAL_MOVE", is_localmove_supported);
	shell->is_localmove_supported = is_localmove_supported;
	shell_rdp_debug(shell, "RDPRAIL-shell: local-move:%d\n", shell->is_localmove_supported);

	/* distro name is provided from WSL via enviromment variable */
	shell->distroNameLength = 0;
	shell->distroName = getenv("WSL2_DISTRO_NAME");
	if (!shell->distroName)
		shell->distroName = getenv("WSL_DISTRO_NAME");
	if (shell->distroName)
		shell->distroNameLength = strlen(shell->distroName);
	shell_rdp_debug(shell, "RDPRAIL-shell: distro name:%s (len:%ld)\n",
		shell->distroName, shell->distroNameLength);

	/* default icon path is provided from WSL via enviromment variable */
	s = getenv("WSL2_DEFAULT_APP_ICON");
	if (s && (strcmp(s, "disabled") != 0))
		shell->image_default_app_icon = load_image(s);
	shell_rdp_debug(shell, "RDPRAIL-shell: WSL2_DEFAULT_APP_ICON:%s (loaded:%s)\n",
		s, shell->image_default_app_icon ? "yes" : "no");

	/* default overlay icon path is provided from WSL via enviromment variable */
	s = getenv("WSL2_DEFAULT_APP_OVERLAY_ICON");
	if (s && (strcmp(s, "disabled") != 0))
		shell->image_default_app_overlay_icon = load_image(s);
	shell_rdp_debug(shell, "RDPRAIL-shell: WSL2_DEFAULT_APP_OVERLAY_ICON:%s (loaded:%s)\n",
		s, shell->image_default_app_overlay_icon ? "yes" : "no");

	shell->is_appid_with_distro_name = read_rdpshell_config_bool(
		"WESTON_RDPRAIL_SHELL_APPEND_DISTRONAME_STARTMENU", true);
	shell_rdp_debug(shell, "RDPRAIL-shell: WESTON_RDPRAIL_SHELL_APPEND_DISTRONAME_STARTMEN:%d\n",
		shell->is_appid_with_distro_name);

	shell->is_blend_overlay_icon_app_list = read_rdpshell_config_bool(
		"WESTON_RDPRAIL_SHELL_BLEND_OVERLAY_ICON_APPLIST", true);
	shell_rdp_debug(shell, "RDPRAIL-shell: WESTON_RDPRAIL_SHELL_BLEND_OVERLAY_ICON_APPLIST:%d\n",
		shell->is_blend_overlay_icon_app_list);

	shell->is_blend_overlay_icon_taskbar = read_rdpshell_config_bool(
		"WESTON_RDPRAIL_SHELL_BLEND_OVERLAY_ICON_TASKBAR", true);
	shell_rdp_debug(shell, "RDPRAIL-shell: WESTON_RDPRAIL_SHELL_BLEND_OVERLAY_ICON_TASKBAR:%d\n",
		shell->is_blend_overlay_icon_taskbar);

	/* preblend overlay icon over app icon */
	if (shell->is_blend_overlay_icon_taskbar &&
	    shell->image_default_app_icon &&
	    shell->image_default_app_overlay_icon)
		shell_blend_overlay_icon(shell,
					 shell->image_default_app_icon,
					 shell->image_default_app_overlay_icon);

	shell->use_wslpath = read_rdpshell_config_bool(
		"WESTON_RDPRAIL_SHELL_USE_WSLPATH", false);
	shell_rdp_debug(shell, "RDPRAIL-shell: WESTON_RDPRAIL_SHELL_USE_WSLPATH:%d\n",
		shell->use_wslpath);

	shell->workspaces.num = 1;
}

struct weston_output *
get_default_output(struct weston_compositor *compositor)
{
	const struct weston_rdp_output_api *api = weston_rdp_output_get_api(compositor);
	return api->output_get_primary(compositor);
}

static struct weston_output *
get_output_containing(struct desktop_shell *shell, int x, int y, bool use_default)
{
	struct weston_compositor *compositor = shell->compositor;

	if (wl_list_empty(&compositor->output_list))
		return NULL;

	struct weston_output *output;
	wl_list_for_each(output, &compositor->output_list, link) {
		if (x >= output->region.extents.x1 && x < output->region.extents.x2 &&
			y >= output->region.extents.y1 && y < output->region.extents.y2) {
			return output;
		}
	}

	if (use_default) {
		shell_rdp_debug_verbose(shell, "%s: Didn't find output containing (%d, %d), return default\n",
			__func__, x, y);
		return get_default_output(compositor);
	} else {
		return NULL;
	}
}


/* no-op func for checking focus surface */
static void
focus_surface_committed(struct weston_surface *es, int32_t sx, int32_t sy)
{
}

static bool
is_focus_surface (struct weston_surface *es)
{
	return (es->committed == focus_surface_committed);
}

static bool
is_focus_view (struct weston_view *view)
{
	return is_focus_surface (view->surface);
}

static void
focus_state_destroy(struct focus_state *state)
{
	wl_list_remove(&state->seat_destroy_listener.link);
	wl_list_remove(&state->surface_destroy_listener.link);
	free(state);
}

static void
focus_state_seat_destroy(struct wl_listener *listener, void *data)
{
	struct focus_state *state = container_of(listener,
						 struct focus_state,
						 seat_destroy_listener);

	wl_list_remove(&state->link);
	focus_state_destroy(state);
}

static void
focus_state_surface_destroy(struct wl_listener *listener, void *data)
{
	struct focus_state *state = container_of(listener,
						 struct focus_state,
						 surface_destroy_listener);
	struct weston_surface *main_surface;
	struct weston_view *next;
	struct weston_view *view;

	main_surface = weston_surface_get_main_surface(state->keyboard_focus);

	next = NULL;
	wl_list_for_each(view,
			 &state->ws->layer.view_list.link, layer_link.link) {
		if (view->surface == main_surface)
			continue;
		if (is_focus_view(view))
			continue;
		if (!get_shell_surface(view->surface))
			continue;

		next = view;
		break;
	}

	/* if the focus was a sub-surface, activate its main surface */
	if (main_surface != state->keyboard_focus)
		next = get_default_view(main_surface);

	if (next) {
		if (state->keyboard_focus) {
			wl_list_remove(&state->surface_destroy_listener.link);
			wl_list_init(&state->surface_destroy_listener.link);
		}
		state->keyboard_focus = NULL;
		activate(state->shell, next, state->seat,
			 WESTON_ACTIVATE_FLAG_CONFIGURE);
	} else {
		wl_list_remove(&state->link);
		focus_state_destroy(state);
	}
}

static struct focus_state *
focus_state_create(struct desktop_shell *shell, struct weston_seat *seat,
		   struct workspace *ws)
{
	struct focus_state *state;

	state = malloc(sizeof *state);
	if (state == NULL)
		return NULL;

	state->shell = shell;
	state->keyboard_focus = NULL;
	state->ws = ws;
	state->seat = seat;
	wl_list_insert(&ws->focus_list, &state->link);

	state->seat_destroy_listener.notify = focus_state_seat_destroy;
	state->surface_destroy_listener.notify = focus_state_surface_destroy;
	wl_signal_add(&seat->destroy_signal,
		      &state->seat_destroy_listener);
	wl_list_init(&state->surface_destroy_listener.link);

	return state;
}

static struct focus_state *
ensure_focus_state(struct desktop_shell *shell, struct weston_seat *seat)
{
	struct workspace *ws = get_current_workspace(shell);
	struct focus_state *state;

	wl_list_for_each(state, &ws->focus_list, link)
		if (state->seat == seat)
			break;

	if (&state->link == &ws->focus_list)
		state = focus_state_create(shell, seat, ws);

	return state;
}

static void
focus_state_set_focus(struct focus_state *state,
		      struct weston_surface *surface)
{
	if (state->keyboard_focus) {
		wl_list_remove(&state->surface_destroy_listener.link);
		wl_list_init(&state->surface_destroy_listener.link);
	}

	state->keyboard_focus = surface;
	if (surface)
		wl_signal_add(&surface->destroy_signal,
			      &state->surface_destroy_listener);
}

static void
drop_focus_state(struct desktop_shell *shell, struct workspace *ws,
		 struct weston_surface *surface)
{
	struct focus_state *state;

	wl_list_for_each(state, &ws->focus_list, link)
		if (state->keyboard_focus == surface)
			focus_state_set_focus(state, NULL);
}

static void
workspace_destroy(struct workspace *ws)
{
	struct focus_state *state, *next;

	wl_list_for_each_safe(state, next, &ws->focus_list, link)
		focus_state_destroy(state);

	free(ws);
}

static void
seat_destroyed(struct wl_listener *listener, void *data)
{
	struct weston_seat *seat = data;
	struct focus_state *state, *next;
	struct workspace *ws = container_of(listener,
					    struct workspace,
					    seat_destroyed_listener);

	wl_list_for_each_safe(state, next, &ws->focus_list, link)
		if (state->seat == seat)
			wl_list_remove(&state->link);
}

static struct workspace *
workspace_create(struct desktop_shell *shell)
{
	struct workspace *ws = malloc(sizeof *ws);
	if (ws == NULL)
		return NULL;

	weston_layer_init(&ws->layer, shell->compositor);

	wl_list_init(&ws->focus_list);
	wl_list_init(&ws->seat_destroyed_listener.link);
	ws->seat_destroyed_listener.notify = seat_destroyed;

	return ws;
}

static struct workspace *
get_workspace(struct desktop_shell *shell, unsigned int index)
{
	struct workspace **pws = shell->workspaces.array.data;
	assert(index < shell->workspaces.num);
	pws += index;
	return *pws;
}

struct workspace *
get_current_workspace(struct desktop_shell *shell)
{
	return get_workspace(shell, shell->workspaces.current);
}

static void
activate_workspace(struct desktop_shell *shell, unsigned int index)
{
	struct workspace *ws;

	ws = get_workspace(shell, index);
	weston_layer_set_position(&ws->layer, WESTON_LAYER_POSITION_NORMAL);

	shell->workspaces.current = index;
}

static void
surface_keyboard_focus_lost(struct weston_surface *surface)
{
	struct weston_compositor *compositor = surface->compositor;
	struct weston_seat *seat;
	struct weston_surface *focus;

	wl_list_for_each(seat, &compositor->seat_list, link) {
		struct weston_keyboard *keyboard =
			weston_seat_get_keyboard(seat);

		if (!keyboard)
			continue;

		focus = weston_surface_get_main_surface(keyboard->focus);
		if (focus == surface)
			weston_keyboard_set_focus(keyboard, NULL);
	}
}

static void
touch_move_grab_down(struct weston_touch_grab *grab,
		     const struct timespec *time,
		     int touch_id, wl_fixed_t x, wl_fixed_t y)
{
}

static void
touch_move_grab_up(struct weston_touch_grab *grab, const struct timespec *time,
		   int touch_id)
{
	struct weston_touch_move_grab *move =
		(struct weston_touch_move_grab *) container_of(
			grab, struct shell_touch_grab, grab);

	if (touch_id == 0)
		move->active = 0;

	if (grab->touch->num_tp == 0) {
		shell_touch_grab_end(&move->base);
		free(move);
	}
}

static void
touch_move_grab_motion(struct weston_touch_grab *grab,
		       const struct timespec *time, int touch_id,
		       wl_fixed_t x, wl_fixed_t y)
{
	struct weston_touch_move_grab *move = (struct weston_touch_move_grab *) grab;
	struct shell_surface *shsurf = move->base.shsurf;
	struct weston_surface *es;
	int dx = wl_fixed_to_int(grab->touch->grab_x + move->dx);
	int dy = wl_fixed_to_int(grab->touch->grab_y + move->dy);

	if (!shsurf || !move->active)
		return;

	es = weston_desktop_surface_get_surface(shsurf->desktop_surface);

	weston_view_set_position(shsurf->view, dx, dy);

	weston_compositor_schedule_repaint(es->compositor);
}

static void
touch_move_grab_frame(struct weston_touch_grab *grab)
{
}

static void
touch_move_grab_cancel(struct weston_touch_grab *grab)
{
	struct weston_touch_move_grab *move =
		(struct weston_touch_move_grab *) container_of(
			grab, struct shell_touch_grab, grab);

	shell_touch_grab_end(&move->base);
	free(move);
}

static const struct weston_touch_grab_interface touch_move_grab_interface = {
	touch_move_grab_down,
	touch_move_grab_up,
	touch_move_grab_motion,
	touch_move_grab_frame,
	touch_move_grab_cancel,
};

static int
surface_touch_move(struct shell_surface *shsurf, struct weston_touch *touch)
{
	struct weston_touch_move_grab *move;

	if (!shsurf)
		return -1;

	if (weston_desktop_surface_get_fullscreen(shsurf->desktop_surface) ||
	    weston_desktop_surface_get_maximized(shsurf->desktop_surface))
		return 0;

	move = malloc(sizeof *move);
	if (!move)
		return -1;

	move->active = 1;
	move->dx = wl_fixed_from_double(shsurf->view->geometry.x) -
		   touch->grab_x;
	move->dy = wl_fixed_from_double(shsurf->view->geometry.y) -
		   touch->grab_y;

	shell_touch_grab_start(&move->base, &touch_move_grab_interface, shsurf,
			       touch);

	return 0;
}

static void
noop_grab_focus(struct weston_pointer_grab *grab)
{
}

static void
noop_grab_axis(struct weston_pointer_grab *grab,
	       const struct timespec *time,
	       struct weston_pointer_axis_event *event)
{
}

static void
noop_grab_axis_source(struct weston_pointer_grab *grab,
		      uint32_t source)
{
}

static void
noop_grab_frame(struct weston_pointer_grab *grab)
{
}

static void
constrain_position(struct weston_move_grab *move, int *cx, int *cy)
{
	struct weston_pointer *pointer = move->base.grab.pointer;
	*cx = wl_fixed_to_int(pointer->x + move->dx);
	*cy = wl_fixed_to_int(pointer->y + move->dy);
}

static void
move_grab_motion(struct weston_pointer_grab *grab,
		 const struct timespec *time,
		 struct weston_pointer_motion_event *event)
{
	struct weston_move_grab *move = (struct weston_move_grab *) grab;
	struct weston_pointer *pointer = grab->pointer;
	struct shell_surface *shsurf = move->base.shsurf;
	struct weston_surface *surface;
	int cx, cy;

	weston_pointer_move(pointer, event);
	if (!shsurf)
		return;

	/* if local move is expected, but recieved the mouse move,
	   then cacenl local move. */
	if (shsurf->shell->is_localmove_pending &&
		(pointer->grab_x != pointer->x ||
		 pointer->grab_y != pointer->y)) {
		shell_rdp_debug_verbose(shsurf->shell, "%s: mouse move is detected while attempting local move\n", __func__);
		shsurf->shell->is_localmove_pending = false;
	}

	surface = weston_desktop_surface_get_surface(shsurf->desktop_surface);

	if (shsurf->snapped.is_snapped &&
		(pointer->grab_x != pointer->x ||
		 pointer->grab_y != pointer->y)) {
		grab_unsnap_motion(grab);
	} else if (shsurf->state.maximized &&
		(pointer->grab_x != pointer->x ||
		 pointer->grab_y != pointer->y)) {
		grab_unmaximized_motion(grab);
	} else {
		constrain_position(move, &cx, &cy);
		weston_view_set_position(shsurf->view, cx, cy);
	}

	weston_compositor_schedule_repaint(surface->compositor);
}

static void
move_grab_button(struct weston_pointer_grab *grab,
		 const struct timespec *time, uint32_t button, uint32_t state_w)
{
	struct shell_grab *shell_grab = container_of(grab, struct shell_grab,
						    grab);
	struct weston_pointer *pointer = grab->pointer;
	enum wl_pointer_button_state state = state_w;

	if (pointer->button_count == 0 &&
	    state == WL_POINTER_BUTTON_STATE_RELEASED) {
		shell_grab_end(shell_grab);
		free(grab);
	}
}

static void
move_grab_cancel(struct weston_pointer_grab *grab)
{
	struct shell_grab *shell_grab =
		container_of(grab, struct shell_grab, grab);

	shell_grab_end(shell_grab);
	free(grab);
}

static const struct weston_pointer_grab_interface move_grab_interface = {
	noop_grab_focus,
	move_grab_motion,
	move_grab_button,
	noop_grab_axis,
	noop_grab_axis_source,
	noop_grab_frame,
	move_grab_cancel,
};

static int
surface_move(struct shell_surface *shsurf, struct weston_pointer *pointer,
	     bool client_initiated)
{
	struct weston_move_grab *move;

	if (!shsurf)
		return -1;

	if (shsurf->grabbed ||
	    weston_desktop_surface_get_fullscreen(shsurf->desktop_surface)/* ||
	    // RAIL shell to allow unmaximize by dragging title bar 
	    weston_desktop_surface_get_maximized(shsurf->desktop_surface) */)
		return 0;

	move = malloc(sizeof *move);
	if (!move)
		return -1;

	move->dx = wl_fixed_from_double(shsurf->view->geometry.x) -
		   pointer->grab_x;
	move->dy = wl_fixed_from_double(shsurf->view->geometry.y) -
		   pointer->grab_y;
	move->client_initiated = client_initiated;

	shell_grab_start(&move->base, &move_grab_interface, shsurf,
			 pointer, WESTON_RDPRAIL_SHELL_CURSOR_MOVE);

	return 0;
}

struct weston_resize_grab {
	struct shell_grab base;
	uint32_t edges;
	int32_t width, height;
};

static void
resize_grab_motion(struct weston_pointer_grab *grab,
		   const struct timespec *time,
		   struct weston_pointer_motion_event *event)
{
	struct weston_resize_grab *resize = (struct weston_resize_grab *) grab;
	struct weston_pointer *pointer = grab->pointer;
	struct shell_surface *shsurf = resize->base.shsurf;
	int32_t width, height;
	struct weston_size min_size, max_size;
	wl_fixed_t from_x, from_y;
	wl_fixed_t to_x, to_y;

	weston_pointer_move(pointer, event);

	if (!shsurf)
		return;

	weston_view_from_global_fixed(shsurf->view,
				      pointer->grab_x, pointer->grab_y,
				      &from_x, &from_y);
	weston_view_from_global_fixed(shsurf->view,
				      pointer->x, pointer->y, &to_x, &to_y);

	width = resize->width;
	if (resize->edges & WL_SHELL_SURFACE_RESIZE_LEFT) {
		width += wl_fixed_to_int(from_x - to_x);
	} else if (resize->edges & WL_SHELL_SURFACE_RESIZE_RIGHT) {
		width += wl_fixed_to_int(to_x - from_x);
	}

	height = resize->height;
	if (resize->edges & WL_SHELL_SURFACE_RESIZE_TOP) {
		height += wl_fixed_to_int(from_y - to_y);
	} else if (resize->edges & WL_SHELL_SURFACE_RESIZE_BOTTOM) {
		height += wl_fixed_to_int(to_y - from_y);
	}

	max_size = weston_desktop_surface_get_max_size(shsurf->desktop_surface);
	min_size = weston_desktop_surface_get_min_size(shsurf->desktop_surface);

	min_size.width = MAX(1, min_size.width);
	min_size.height = MAX(1, min_size.height);

	if (width < min_size.width)
		width = min_size.width;
	else if (max_size.width > 0 && width > max_size.width)
		width = max_size.width;
	if (height < min_size.height)
		height = min_size.height;
	else if (max_size.height > 0 && height > max_size.height)
		height = max_size.height;
	weston_desktop_surface_set_size(shsurf->desktop_surface, width, height);
}

static void
resize_grab_button(struct weston_pointer_grab *grab,
		   const struct timespec *time,
		   uint32_t button, uint32_t state_w)
{
	struct weston_resize_grab *resize = (struct weston_resize_grab *) grab;
	struct weston_pointer *pointer = grab->pointer;
	enum wl_pointer_button_state state = state_w;

	if (pointer->button_count == 0 &&
	    state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (resize->base.shsurf != NULL) {
			struct weston_desktop_surface *desktop_surface =
				resize->base.shsurf->desktop_surface;
			weston_desktop_surface_set_resizing(desktop_surface,
							    false);
		}

		shell_grab_end(&resize->base);
		free(grab);
	}
}

static void
resize_grab_cancel(struct weston_pointer_grab *grab)
{
	struct weston_resize_grab *resize = (struct weston_resize_grab *) grab;

	if (resize->base.shsurf != NULL) {
		struct weston_desktop_surface *desktop_surface =
			resize->base.shsurf->desktop_surface;
		weston_desktop_surface_set_resizing(desktop_surface, false);
	}

	shell_grab_end(&resize->base);
	free(grab);
}

static const struct weston_pointer_grab_interface resize_grab_interface = {
	noop_grab_focus,
	resize_grab_motion,
	resize_grab_button,
	noop_grab_axis,
	noop_grab_axis_source,
	noop_grab_frame,
	resize_grab_cancel,
};

/*
 * Returns the bounding box of a surface and all its sub-surfaces,
 * in surface-local coordinates. */
static void
surface_subsurfaces_boundingbox(struct weston_surface *surface, int32_t *x,
				int32_t *y, int32_t *w, int32_t *h) {
	pixman_region32_t region;
	pixman_box32_t *box;
	struct weston_subsurface *subsurface;

	pixman_region32_init_rect(&region, 0, 0,
	                          surface->width,
	                          surface->height);

	wl_list_for_each(subsurface, &surface->subsurface_list, parent_link) {
		pixman_region32_union_rect(&region, &region,
		                           subsurface->position.x,
		                           subsurface->position.y,
		                           subsurface->surface->width,
		                           subsurface->surface->height);
	}

	box = pixman_region32_extents(&region);
	if (x)
		*x = box->x1;
	if (y)
		*y = box->y1;
	if (w)
		*w = box->x2 - box->x1;
	if (h)
		*h = box->y2 - box->y1;

	pixman_region32_fini(&region);
}

static int
surface_resize(struct shell_surface *shsurf,
	       struct weston_pointer *pointer, uint32_t edges)
{
	struct weston_resize_grab *resize;
	const unsigned resize_topbottom =
		WL_SHELL_SURFACE_RESIZE_TOP | WL_SHELL_SURFACE_RESIZE_BOTTOM;
	const unsigned resize_leftright =
		WL_SHELL_SURFACE_RESIZE_LEFT | WL_SHELL_SURFACE_RESIZE_RIGHT;
	const unsigned resize_any = resize_topbottom | resize_leftright;
	struct weston_geometry geometry;

	if (shsurf->grabbed ||
	    weston_desktop_surface_get_fullscreen(shsurf->desktop_surface) ||
	    weston_desktop_surface_get_maximized(shsurf->desktop_surface))
		return 0;

	/* Check for invalid edge combinations. */
	if (edges == WL_SHELL_SURFACE_RESIZE_NONE || edges > resize_any ||
	    (edges & resize_topbottom) == resize_topbottom ||
	    (edges & resize_leftright) == resize_leftright)
		return 0;

	resize = malloc(sizeof *resize);
	if (!resize)
		return -1;

	resize->edges = edges;

	geometry = weston_desktop_surface_get_geometry(shsurf->desktop_surface);
	resize->width = geometry.width;
	resize->height = geometry.height;

	shsurf->resize_edges = edges;
	weston_desktop_surface_set_resizing(shsurf->desktop_surface, true);
	shell_grab_start(&resize->base, &resize_grab_interface, shsurf,
			 pointer, edges);

	return 0;
}

static void
busy_cursor_grab_focus(struct weston_pointer_grab *base)
{
	struct shell_grab *grab = (struct shell_grab *) base;
	struct weston_pointer *pointer = base->pointer;
	struct weston_desktop_surface *desktop_surface = NULL;
	struct weston_view *view;
	wl_fixed_t sx, sy;

	view = weston_compositor_pick_view(pointer->seat->compositor,
					   pointer->x, pointer->y,
					   &sx, &sy);
	/* With RAIL, it's possible that cursor can be at where has no view underneath. */
	if (view)
		desktop_surface = weston_surface_get_desktop_surface(view->surface);

	if (desktop_surface == NULL || !grab->shsurf || grab->shsurf->desktop_surface != desktop_surface) {
		shell_grab_end(grab);
		free(grab);
	}
}

static void
busy_cursor_grab_motion(struct weston_pointer_grab *grab,
			const struct timespec *time,
			struct weston_pointer_motion_event *event)
{
	weston_pointer_move(grab->pointer, event);
}

static void
busy_cursor_grab_button(struct weston_pointer_grab *base,
			const struct timespec *time,
			uint32_t button, uint32_t state)
{
	struct shell_grab *grab = (struct shell_grab *) base;
	struct shell_surface *shsurf = grab->shsurf;
	struct weston_pointer *pointer = grab->grab.pointer;
	struct weston_seat *seat = pointer->seat;

	if (shsurf && button == BTN_LEFT && state) {
		activate(shsurf->shell, shsurf->view, seat,
			 WESTON_ACTIVATE_FLAG_CONFIGURE);
		surface_move(shsurf, pointer, false);
	} else if (shsurf && button == BTN_RIGHT && state) {
		activate(shsurf->shell, shsurf->view, seat,
			 WESTON_ACTIVATE_FLAG_CONFIGURE);
		surface_rotate(shsurf, pointer);
	}
}

static void
busy_cursor_grab_cancel(struct weston_pointer_grab *base)
{
	struct shell_grab *grab = (struct shell_grab *) base;

	shell_grab_end(grab);
	free(grab);
}

static const struct weston_pointer_grab_interface busy_cursor_grab_interface = {
	busy_cursor_grab_focus,
	busy_cursor_grab_motion,
	busy_cursor_grab_button,
	noop_grab_axis,
	noop_grab_axis_source,
	noop_grab_frame,
	busy_cursor_grab_cancel,
};

static void
handle_pointer_focus(struct wl_listener *listener, void *data)
{
	struct weston_pointer *pointer = data;
	struct weston_view *view = pointer->focus;
	struct shell_surface *shsurf;
	struct weston_desktop_client *client;

	if (!view)
		return;

	shsurf = get_shell_surface(view->surface);
	if (!shsurf)
		return;

	client = weston_desktop_surface_get_client(shsurf->desktop_surface);

	if (shsurf->unresponsive)
		set_busy_cursor(shsurf, pointer);
	else
		weston_desktop_client_ping(client);
}

static void
shell_surface_lose_keyboard_focus(struct shell_surface *shsurf)
{
	if (--shsurf->focus_count == 0)
		weston_desktop_surface_set_activated(shsurf->desktop_surface, false);
}

static void
shell_surface_gain_keyboard_focus(struct shell_surface *shsurf)
{
	if (shsurf->focus_count++ == 0)
		weston_desktop_surface_set_activated(shsurf->desktop_surface, true);
}

static void
handle_keyboard_focus(struct wl_listener *listener, void *data)
{
	struct weston_keyboard *keyboard = data;
	struct shell_seat *seat = get_shell_seat(keyboard->seat);
	struct desktop_shell *shell = seat->shell;
	struct weston_surface* new_focused_surface = weston_surface_get_main_surface(keyboard->focus);
	struct weston_surface* old_focused_surface = seat->focused_surface;

	if (shell->debugLevel >= RDPRAIL_SHELL_DEBUG_LEVEL_VERBOSE) {
		struct weston_desktop_surface *old_desktop_surface = NULL;
		struct weston_desktop_surface *new_desktop_surface = NULL;
		const char *old_title = NULL;
		const char *new_title = NULL;

		if (old_focused_surface)
			old_desktop_surface = weston_surface_get_desktop_surface(old_focused_surface);
		if (new_focused_surface)
			new_desktop_surface = weston_surface_get_desktop_surface(new_focused_surface);
		if (old_desktop_surface)
			old_title = weston_desktop_surface_get_title(old_desktop_surface);
		if (new_desktop_surface)
			new_title = weston_desktop_surface_get_title(new_desktop_surface);

		shell_rdp_debug_verbose(shell, "%s: moving focus from %p:%s to %p:%s\n", __func__,
			old_focused_surface, old_title, new_focused_surface, new_title);
	}

	if (old_focused_surface) {
		struct shell_surface *shsurf = get_shell_surface(old_focused_surface);
		if (shsurf)
			shell_surface_lose_keyboard_focus(shsurf);
	}

	seat->focused_surface = new_focused_surface;

	if (new_focused_surface) {
		struct shell_surface *shsurf = get_shell_surface(new_focused_surface);
		if (shsurf)
			shell_surface_gain_keyboard_focus(shsurf);
	}

	if (new_focused_surface == shell->focus_proxy_surface) {
		/* When new focused window is focus proxy window, client side window is
		   taking focus and server side window is losing focus, thus let keyboard
		   to clear out currently pressed keys. This is because once server side
		   window is gone from client desktop, the client no longer sends keyboard
		   inputs including key release, thus if any keys are currently at pressed
		   state, it doesn't recieve release for those keys from RDP client. */
		while (keyboard->keys.size) {
			struct timespec time;
			uint32_t *k = keyboard->keys.data;
			weston_compositor_get_time(&time);
			notify_key(seat->seat, &time, *k,
				WL_KEYBOARD_KEY_STATE_RELEASED, STATE_UPDATE_AUTOMATIC);
			/* shell_rdp_debug_verbose(shell, "%s: released key:0x%x\n", __func__, *k); */
		}
	}
}

/* The surface will be inserted into the list immediately after the link
 * returned by this function (i.e. will be stacked immediately above the
 * returned link). */
static struct weston_layer_entry *
shell_surface_calculate_layer_link (struct shell_surface *shsurf)
{
	struct workspace *ws;

	if (weston_desktop_surface_get_fullscreen(shsurf->desktop_surface) &&
	    !shsurf->state.lowered) {
		return &shsurf->shell->fullscreen_layer.view_list;
	}

	/* Move the surface to a normal workspace layer so that surfaces
	 * which were previously fullscreen or transient are no longer
	 * rendered on top. */
	ws = get_current_workspace(shsurf->shell);
	return &ws->layer.view_list;
}

static void
shell_surface_update_child_surface_layers (struct shell_surface *shsurf)
{
	weston_desktop_surface_propagate_layer(shsurf->desktop_surface);
}

/* Update the surface’s layer. Mark both the old and new views as having dirty
 * geometry to ensure the changes are redrawn.
 *
 * If any child surfaces exist and are mapped, ensure they’re in the same layer
 * as this surface. */
static void
shell_surface_update_layer(struct shell_surface *shsurf)
{
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(shsurf->desktop_surface);
	struct weston_layer_entry *new_layer_link;

	new_layer_link = shell_surface_calculate_layer_link(shsurf);

	if (new_layer_link == NULL)
		return;
	if (new_layer_link == &shsurf->view->layer_link)
		return;

	weston_view_geometry_dirty(shsurf->view);
	weston_layer_entry_remove(&shsurf->view->layer_link);
	weston_layer_entry_insert(new_layer_link, &shsurf->view->layer_link);
	weston_view_geometry_dirty(shsurf->view);
	weston_surface_damage(surface);

	shell_surface_update_child_surface_layers(shsurf);
}

static void
notify_output_destroy(struct wl_listener *listener, void *data)
{
	struct shell_surface *shsurf =
		container_of(listener,
			     struct shell_surface, output_destroy_listener);

	shsurf->output = NULL;
	shsurf->output_destroy_listener.notify = NULL;
}

static void
shell_surface_set_output(struct shell_surface *shsurf,
                         struct weston_output *output)
{
	struct weston_surface *es =
		weston_desktop_surface_get_surface(shsurf->desktop_surface);

	/* get the default output, if the client set it as NULL
	   check whether the output is available */
	if (output)
		shsurf->output = output;
	else if (es->output)
		shsurf->output = es->output;
	else
		shsurf->output = get_default_output(es->compositor);

	if (shsurf->output_destroy_listener.notify) {
		wl_list_remove(&shsurf->output_destroy_listener.link);
		shsurf->output_destroy_listener.notify = NULL;
	}

	if (!shsurf->output)
		return;

	shsurf->output_destroy_listener.notify = notify_output_destroy;
	wl_signal_add(&shsurf->output->destroy_signal,
		      &shsurf->output_destroy_listener);
}

static void
weston_view_set_initial_position(struct shell_surface *shsurf);

static void
unset_fullscreen(struct shell_surface *shsurf)
{
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(shsurf->desktop_surface);
	struct weston_surface_rail_state *rail_state =
		(struct weston_surface_rail_state *)surface->backend_state;

	if (!rail_state)
		return;

	/* Unset the fullscreen output, driver configuration and transforms. */
	wl_list_remove(&shsurf->fullscreen.transform.link);
	wl_list_init(&shsurf->fullscreen.transform.link);

	if (shsurf->fullscreen.black_view)
		weston_surface_destroy(shsurf->fullscreen.black_view->surface);
	shsurf->fullscreen.black_view = NULL;

	if (shsurf->saved_showstate_valid)
		rail_state->showState_requested = shsurf->saved_showstate;
	else
		rail_state->showState_requested = RDP_WINDOW_SHOW;
	shsurf->saved_showstate_valid = false;

	if (shsurf->saved_position_valid)
		weston_view_set_position(shsurf->view,
					 shsurf->saved_x, shsurf->saved_y);
	else
		weston_view_set_initial_position(shsurf);
	shsurf->saved_position_valid = false;

	if (shsurf->saved_rotation_valid) {
		wl_list_insert(&shsurf->view->geometry.transformation_list,
		               &shsurf->rotation.transform.link);
		shsurf->saved_rotation_valid = false;
	}

}

static void
unset_maximized(struct shell_surface *shsurf)
{
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(shsurf->desktop_surface);
	struct weston_surface_rail_state *rail_state =
		(struct weston_surface_rail_state *)surface->backend_state;

	if (!rail_state)
		return;

	/* if shell surface has already output assigned, leave where it is. (don't move to primary). */
	if (!shsurf->output)
		shell_surface_set_output(shsurf, get_default_output(surface->compositor));

	if (shsurf->saved_showstate_valid)
		rail_state->showState_requested = shsurf->saved_showstate;
	else
		rail_state->showState_requested = RDP_WINDOW_SHOW;
	shsurf->saved_showstate_valid = false;

	if (shsurf->snapped.is_snapped) {
		/* If previously snapped state, don't go back to snap, but restore */
		/* weston_desktop_surface_set_size() expects the size in window geometry coordinates */
		/* saved_width and saved_height is already based on window geometry. */
		weston_desktop_surface_set_size(
			shsurf->desktop_surface,
			shsurf->snapped.saved_width, shsurf->snapped.saved_height);
		weston_view_set_position(shsurf->view, 
			shsurf->snapped.saved_x, shsurf->snapped.saved_y);
		shsurf->snapped.is_snapped = false;
		rail_state->isWindowSnapped = false;
	} else if (!shsurf->maximized.grab_unmaximized) {
		/* Restore to previous position or make up one if the window started maximized.
		   if window is not being grabbed */
		if (shsurf->saved_position_valid)
			weston_view_set_position(shsurf->view,
						shsurf->saved_x, shsurf->saved_y);
		else
			weston_view_set_initial_position(shsurf);
	}
	shsurf->saved_position_valid = false;
	shsurf->maximized.grab_unmaximized = false;

	if (shsurf->saved_rotation_valid) {
		wl_list_insert(&shsurf->view->geometry.transformation_list,
			       &shsurf->rotation.transform.link);
		shsurf->saved_rotation_valid = false;
	}
}

static void
set_minimized(struct weston_surface *surface)
{
	struct shell_surface *shsurf;
	struct workspace *current_ws;
	struct weston_view *view;
	struct weston_surface_rail_state *rail_state =
		(struct weston_surface_rail_state *)surface->backend_state;

	view = get_default_view(surface);
	if (!view)
		return;

	if (!rail_state)
		return;

	assert(weston_surface_get_main_surface(view->surface) == view->surface);

	shsurf = get_shell_surface(surface);

	shsurf->saved_showstate = rail_state->showState;
	shsurf->saved_showstate_valid = true;
	rail_state->showState_requested = RDP_WINDOW_SHOW_MINIMIZED;

	current_ws = get_current_workspace(shsurf->shell);

	weston_layer_entry_remove(&view->layer_link);
	weston_layer_entry_insert(&shsurf->shell->minimized_layer.view_list, &view->layer_link);

	drop_focus_state(shsurf->shell, current_ws, view->surface);
	surface_keyboard_focus_lost(surface);

	shell_surface_update_child_surface_layers(shsurf);
	weston_view_damage_below(view);
}

static void
set_unminimized(struct weston_surface *surface)
{
	struct shell_surface *shsurf;
	struct workspace *current_ws;
	struct weston_view *view;
	struct weston_surface_rail_state *rail_state =
		(struct weston_surface_rail_state *)surface->backend_state;

	view = get_default_view(surface);
	if (!view)
		return;

	if (!rail_state)
		return;

	assert(weston_surface_get_main_surface(view->surface) == view->surface);

	shsurf = get_shell_surface(surface);

	if (shsurf->saved_showstate_valid)
		rail_state->showState_requested = shsurf->saved_showstate;
	else
		rail_state->showState_requested = RDP_WINDOW_SHOW;
	shsurf->saved_showstate_valid = false;

	current_ws = get_current_workspace(shsurf->shell);

	weston_layer_entry_remove(&view->layer_link);
	weston_layer_entry_insert(&current_ws->layer.view_list, &view->layer_link);

	shell_surface_update_child_surface_layers(shsurf);
	weston_view_damage_below(view);
}

static void
grab_unsnap_motion(struct weston_pointer_grab *grab)
{
	struct weston_pointer *pointer = grab->pointer;
	struct weston_move_grab *move = (struct weston_move_grab *) grab;
	struct shell_surface *shsurf = move->base.shsurf;
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(shsurf->desktop_surface);
	struct weston_surface_rail_state *rail_state =
		(struct weston_surface_rail_state *)surface->backend_state;
	int cx, cy, geometry_offset;
	float dx, move_dx;

	if (!shsurf->snapped.is_snapped)
		return;

	if (!rail_state)
		return;

	/* window is no longer in snap state */
	shsurf->snapped.is_snapped = false;
	rail_state->isWindowSnapped = false;
	rail_state->showState_requested = RDP_WINDOW_SHOW;
	shsurf->saved_showstate_valid = false;

	geometry_offset = shsurf->snapped.saved_surface_width -
			  shsurf->snapped.saved_width;
	geometry_offset /= 2;

	/* Reposition the window such that the mouse remain within the 
	 * new bound of the window after resize. */
	/* calc based on pointer position based on current (snapped) window size */
	/* move->dx is offset of pointer position from window origin */
	dx = wl_fixed_to_double(move->dx) / surface->width;
	dx = fabsf(dx);
	/* calc the distance based on unsnapped window size */
	cx = shsurf->snapped.saved_width * dx;
	/* add window geometry offset */
	cx += geometry_offset;
	/* obtain new window offset from current pointer position */
	cx = wl_fixed_to_int(pointer->x) - cx;

	/* adjust move->dx based on new position */
	move_dx = wl_fixed_from_int(cx) - pointer->grab_x;

	shell_rdp_debug_verbose(shsurf->shell, "%s: restore surface:%p at (%d,%d) size:%dx%d from (%d,%d) size:%dx%d\n",
			__func__, surface,
			cx, cy,
			shsurf->snapped.saved_width, shsurf->snapped.saved_height,
			(int)shsurf->view->geometry.x, (int)shsurf->view->geometry.y,
			surface->width, surface->height);
	shell_rdp_debug_verbose(shsurf->shell, "%s: restore surface:%p, geometry_offset:%d, grab_ratio:%f, move_dx:%d->%d\n",
			__func__, surface, geometry_offset, dx,
			wl_fixed_to_int(move->dx), wl_fixed_to_int(move_dx));

	/* restore original size */
	weston_desktop_surface_set_size(shsurf->desktop_surface,
			shsurf->snapped.saved_width,
			shsurf->snapped.saved_height);

	/* update move_dx based on new position */
	move->dx = move_dx;

	/* and move to new position reletive to pointer */
	constrain_position(move, &cx, &cy);
	weston_view_set_position(shsurf->view, cx, cy);
}

static void
grab_unmaximized_motion(struct weston_pointer_grab *grab)
{
	struct weston_pointer *pointer = grab->pointer;
	struct weston_move_grab *move = (struct weston_move_grab *) grab;
	struct shell_surface *shsurf = move->base.shsurf;
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(shsurf->desktop_surface);
	const struct weston_xwayland_surface_api *api;
	int cx, cy, geometry_offset;
	float dx, move_dx;

	if (shsurf->maximized.grab_unmaximized)
		return;

	geometry_offset = shsurf->maximized.saved_surface_width -
			  shsurf->maximized.saved_width;
	geometry_offset /= 2;

	/* Reposition the window such that the mouse remain within the 
	 * new bound of the window after resize. */
	/* calc based on pointer position based on current (maximized) window size */
	/* move->dx is offset of pointer position from window origin */
	dx = wl_fixed_to_double(move->dx) / surface->width;
	dx = fabsf(dx);
	/* calc the distance based on restored window size */
	cx = shsurf->maximized.saved_width * dx;
	/* add window geometry offset */
	cx += geometry_offset;
	/* obtain new window offset from current pointer position */
	cx = wl_fixed_to_int(pointer->x) - cx;

	/* adjust move->dx based on new position */
	move_dx = wl_fixed_from_int(cx) - pointer->grab_x;

	shell_rdp_debug_verbose(shsurf->shell, "%s: restore surface:%p at (%d,%d) size:%dx%d from (%d,%d) size:%dx%d\n",
			__func__, surface,
			cx, cy,
			shsurf->maximized.saved_width, shsurf->maximized.saved_height,
			(int)shsurf->view->geometry.x, (int)shsurf->view->geometry.y,
			surface->width, surface->height);
	shell_rdp_debug_verbose(shsurf->shell, "%s: restore surface:%p, geometry_offset:%d, grab_ratio:%f, move_dx:%d->%d\n",
			__func__, surface, geometry_offset, dx,
			wl_fixed_to_int(move->dx), wl_fixed_to_int(move_dx));

	/* restore from maximized */
	api = shsurf->shell->xwayland_surface_api;
	if (!api) {
		api = weston_xwayland_surface_get_api(shsurf->shell->compositor);
		shsurf->shell->xwayland_surface_api = api;
	}
	if (api && api->is_xwayland_surface(surface)) {
		api->set_maximized(surface, false);
	} else {
		set_maximized(shsurf, false);
	}

	/* update move_dx based on new position */
	move->dx = move_dx;

	/* and move to new position reletive to pointer */
	constrain_position(move, &cx, &cy);
	weston_view_set_position(shsurf->view, cx, cy);

	/* don't reset position at next commit */
	shsurf->maximized.grab_unmaximized = true;
}

static struct desktop_shell *
shell_surface_get_shell(struct shell_surface *shsurf)
{
	return shsurf->shell;
}

static int
black_surface_get_label(struct weston_surface *surface, char *buf, size_t len)
{
	struct weston_view *fs_view = surface->committed_private;
	struct weston_surface *fs_surface = fs_view->surface;
	int n;
	int rem;
	int ret;

	n = snprintf(buf, len, "black background surface for ");
	if (n < 0)
		return n;

	rem = (int)len - n;
	if (rem < 0)
		rem = 0;

	if (fs_surface->get_label)
		ret = fs_surface->get_label(fs_surface, buf + n, rem);
	else
		ret = snprintf(buf + n, rem, "<unknown>");

	if (ret < 0)
		return n;

	return n + ret;
}

static void
black_surface_committed(struct weston_surface *es, int32_t sx, int32_t sy);

static struct weston_view *
create_black_surface(struct weston_compositor *ec,
		     struct weston_view *fs_view,
		     float x, float y, int w, int h)
{
	struct weston_surface *surface = NULL;
	struct weston_view *view;

	surface = weston_surface_create(ec);
	if (surface == NULL) {
		weston_log("%s: no memory\n", __func__);
		return NULL;
	}
	view = weston_view_create(surface);
	if (surface == NULL) {
		weston_log("%s: no memory\n", __func__);
		weston_surface_destroy(surface);
		return NULL;
	}

	surface->committed = black_surface_committed;
	surface->committed_private = fs_view;
	weston_surface_set_label_func(surface, black_surface_get_label);
	weston_surface_set_color(surface, 0.0, 0.0, 0.0, 1);
	pixman_region32_fini(&surface->opaque);
	pixman_region32_init_rect(&surface->opaque, 0, 0, w, h);
	pixman_region32_fini(&surface->input);
	pixman_region32_init_rect(&surface->input, 0, 0, w, h);

	weston_surface_set_size(surface, w, h);
	weston_view_set_position(view, x, y);

	return view;
}

static void
shell_ensure_fullscreen_black_view(struct shell_surface *shsurf)
{
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(shsurf->desktop_surface);
	struct weston_output *output = shsurf->fullscreen_output;

	assert(weston_desktop_surface_get_fullscreen(shsurf->desktop_surface));

	if (!shsurf->fullscreen.black_view)
		shsurf->fullscreen.black_view =
			create_black_surface(surface->compositor,
			                     shsurf->view,
			                     output->x, output->y,
			                     output->width,
			                     output->height);

	weston_view_geometry_dirty(shsurf->fullscreen.black_view);
	weston_layer_entry_remove(&shsurf->fullscreen.black_view->layer_link);
	weston_layer_entry_insert(&shsurf->view->layer_link,
				  &shsurf->fullscreen.black_view->layer_link);
	weston_view_geometry_dirty(shsurf->fullscreen.black_view);
	weston_surface_damage(surface);

	shsurf->fullscreen.black_view->is_mapped = true;
	shsurf->state.lowered = false;
}

/* Create black surface and append it to the associated fullscreen surface.
 * Handle size dismatch and positioning according to the method. */
static void
shell_configure_fullscreen(struct shell_surface *shsurf)
{
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(shsurf->desktop_surface);
	int32_t surf_x, surf_y, surf_width, surf_height;

	/* Reverse the effect of lower_fullscreen_layer() */
	weston_layer_entry_remove(&shsurf->view->layer_link);
	weston_layer_entry_insert(&shsurf->shell->fullscreen_layer.view_list,
				  &shsurf->view->layer_link);

	if (!shsurf->fullscreen_output) {
		/* If there is no output, there's not much we can do.
		 * Position the window somewhere, whatever. */
		weston_view_set_position(shsurf->view, 0, 0);
		return;
	}

	shell_ensure_fullscreen_black_view(shsurf);

	surface_subsurfaces_boundingbox(surface, &surf_x, &surf_y,
	                                &surf_width, &surf_height);

	if (surface->buffer_ref.buffer)
		center_on_output(shsurf->view, shsurf->fullscreen_output);
}

static void
shell_map_fullscreen(struct shell_surface *shsurf)
{
	shell_configure_fullscreen(shsurf);
}

static struct weston_output *
get_focused_output(struct weston_compositor *compositor)
{
	struct weston_seat *seat;
	struct weston_output *output = NULL;

	wl_list_for_each(seat, &compositor->seat_list, link) {
		struct weston_touch *touch = weston_seat_get_touch(seat);
		struct weston_pointer *pointer = weston_seat_get_pointer(seat);
		struct weston_keyboard *keyboard =
			weston_seat_get_keyboard(seat);

		/* Priority has touch focus, then pointer and
		 * then keyboard focus. We should probably have
		 * three for loops and check first for touch,
		 * then for pointer, etc. but unless somebody has some
		 * objections, I think this is sufficient. */
		if (touch && touch->focus)
			output = touch->focus->output;
		else if (pointer && pointer->focus)
			output = pointer->focus->output;
		else if (keyboard && keyboard->focus)
			output = keyboard->focus->output;

		if (output)
			break;
	}

	return output;
}

static void
destroy_shell_seat(struct wl_listener *listener, void *data)
{
	struct shell_seat *shseat =
		container_of(listener,
			     struct shell_seat, seat_destroy_listener);

	wl_list_remove(&shseat->seat_destroy_listener.link);
	free(shseat);
}

static void
shell_seat_caps_changed(struct wl_listener *listener, void *data)
{
	struct weston_keyboard *keyboard;
	struct weston_pointer *pointer;
	struct shell_seat *seat;

	seat = container_of(listener, struct shell_seat, caps_changed_listener);
	keyboard = weston_seat_get_keyboard(seat->seat);
	pointer = weston_seat_get_pointer(seat->seat);

	if (keyboard &&
	    wl_list_empty(&seat->keyboard_focus_listener.link)) {
		wl_signal_add(&keyboard->focus_signal,
			      &seat->keyboard_focus_listener);
	} else if (!keyboard) {
		wl_list_remove(&seat->keyboard_focus_listener.link);
		wl_list_init(&seat->keyboard_focus_listener.link);
	}

	if (pointer &&
	    wl_list_empty(&seat->pointer_focus_listener.link)) {
		wl_signal_add(&pointer->focus_signal,
			      &seat->pointer_focus_listener);
	} else if (!pointer) {
		wl_list_remove(&seat->pointer_focus_listener.link);
		wl_list_init(&seat->pointer_focus_listener.link);
	}
}

static struct shell_seat *
create_shell_seat(struct desktop_shell *shell, struct weston_seat *seat)
{
	struct shell_seat *shseat;

	shseat = calloc(1, sizeof *shseat);
	if (!shseat) {
		weston_log("%s: no memory to allocate shell seat\n", __func__);
		return NULL;
	}

	shseat->seat = seat;
	shseat->shell = shell;

	shseat->seat_destroy_listener.notify = destroy_shell_seat;
	wl_signal_add(&seat->destroy_signal,
	              &shseat->seat_destroy_listener);

	shseat->keyboard_focus_listener.notify = handle_keyboard_focus;
	wl_list_init(&shseat->keyboard_focus_listener.link);

	shseat->pointer_focus_listener.notify = handle_pointer_focus;
	wl_list_init(&shseat->pointer_focus_listener.link);

	shseat->caps_changed_listener.notify = shell_seat_caps_changed;
	wl_signal_add(&seat->updated_caps_signal,
		      &shseat->caps_changed_listener);
	shell_seat_caps_changed(&shseat->caps_changed_listener, NULL);

	return shseat;
}

static struct shell_seat *
get_shell_seat(struct weston_seat *seat)
{
	struct wl_listener *listener;

	listener = wl_signal_get(&seat->destroy_signal, destroy_shell_seat);
	assert(listener != NULL);

	return container_of(listener,
			    struct shell_seat, seat_destroy_listener);
}

struct shell_surface *
get_shell_surface(struct weston_surface *surface)
{
	if (weston_surface_is_desktop_surface(surface)) {
		struct weston_desktop_surface *desktop_surface =
			weston_surface_get_desktop_surface(surface);
		return weston_desktop_surface_get_user_data(desktop_surface);
	}
	return NULL;
}

/*
 * libweston-desktop
 */

static void
handle_metadata_change(struct wl_listener *listener, void *data)
{
	struct weston_desktop_surface *desktop_surface =
		(struct weston_desktop_surface *)data;
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(desktop_surface);
	struct weston_surface_rail_state *rail_state =
		(struct weston_surface_rail_state *)surface->backend_state;

	/* invalidate get_label, this force update title at next update */
	if (rail_state)
		rail_state->get_label = NULL;
}

static void
desktop_surface_added(struct weston_desktop_surface *desktop_surface,
		      void *data)
{
	struct weston_desktop_client *client =
		weston_desktop_surface_get_client(desktop_surface);
	struct wl_client *wl_client =
		weston_desktop_client_get_client(client);
	struct weston_view *view;
	struct shell_surface *shsurf;
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(desktop_surface);
	struct desktop_shell *shell =
		(struct desktop_shell *)data;
	struct weston_compositor *ec = shell->compositor;
	struct wl_event_loop *loop;

	view = weston_desktop_surface_create_view(desktop_surface);
	if (!view)
		return;

	shsurf = calloc(1, sizeof *shsurf);
	if (!shsurf) {
		if (wl_client)
			wl_client_post_no_memory(wl_client);
		else
			shell_rdp_debug(((struct desktop_shell *)shell),
				"%s: no memory to allocate shell surface\n", __func__);
		return;
	}

	weston_surface_set_label_func(surface, shell_surface_get_label);

	shsurf->shell = shell;
	shsurf->unresponsive = 0;
	shsurf->saved_showstate_valid = false;
	shsurf->saved_position_valid = false;
	shsurf->saved_rotation_valid = false;
	shsurf->desktop_surface = desktop_surface;
	shsurf->view = view;
	shsurf->fullscreen.black_view = NULL;
	wl_list_init(&shsurf->fullscreen.transform.link);

	shell_surface_set_output(shsurf, get_default_output(ec));

	wl_signal_init(&shsurf->destroy_signal);

	/* empty when not in use */
	wl_list_init(&shsurf->rotation.transform.link);
	weston_matrix_init(&shsurf->rotation.rotation);

	/*
	 * initialize list as well as link. The latter allows to use
	 * wl_list_remove() even when this surface is not in another list.
	 */
	wl_list_init(&shsurf->children_list);
	wl_list_init(&shsurf->children_link);

	weston_desktop_surface_set_user_data(desktop_surface, shsurf);
	weston_desktop_surface_set_activated(desktop_surface,
					     shsurf->focus_count > 0);

	shsurf->metadata_listener.notify = handle_metadata_change;
	weston_desktop_surface_add_metadata_listener(desktop_surface,
		&shsurf->metadata_listener);

	/* when surface is added, compositor is in wake state */
	weston_compositor_wake(ec);
	/* and, shell process (= focus_proxy) is running */
	if (!shell->child.client) {
		loop = wl_display_get_event_loop(ec->wl_display);
		wl_event_loop_add_idle(loop, launch_desktop_shell_process, shell);
	}
}

static void
desktop_surface_removed(struct weston_desktop_surface *desktop_surface,
			void *_shell)
{
	struct desktop_shell *shell =
		(struct desktop_shell *)_shell;
	struct shell_surface *shsurf =
		weston_desktop_surface_get_user_data(desktop_surface);
	struct shell_surface *shsurf_child, *tmp;
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(desktop_surface);

	if (!shsurf)
		return;

	/* if this is focus proxy, reset to NULL */
	if (shell->focus_proxy_surface == surface) {
		shell->focus_proxy_surface = NULL;
		if (shell->rdprail_api->notify_window_proxy_surface)
			shell->rdprail_api->notify_window_proxy_surface(NULL);
	}

	wl_list_for_each_safe(shsurf_child, tmp, &shsurf->children_list, children_link) {
		wl_list_remove(&shsurf_child->children_link);
		wl_list_init(&shsurf_child->children_link);
	}
	wl_list_remove(&shsurf->children_link);

	wl_signal_emit(&shsurf->destroy_signal, shsurf);

	if (shsurf->fullscreen.black_view)
		weston_surface_destroy(shsurf->fullscreen.black_view->surface);

	weston_surface_set_label_func(surface, NULL);
	weston_desktop_surface_set_user_data(shsurf->desktop_surface, NULL);
	shsurf->desktop_surface = NULL;

	weston_desktop_surface_unlink_view(shsurf->view);
	weston_view_destroy(shsurf->view);

	if (shsurf->output_destroy_listener.notify) {
		wl_list_remove(&shsurf->output_destroy_listener.link);
		shsurf->output_destroy_listener.notify = NULL;
	}

	if (shsurf->metadata_listener.notify) {
		wl_list_remove(&shsurf->metadata_listener.link);
		shsurf->metadata_listener.notify = NULL;
	}

	free(shsurf);
}

static void
set_maximized_position(struct desktop_shell *shell,
		       struct shell_surface *shsurf)
{
	pixman_rectangle32_t area;
	struct weston_geometry geometry;

	get_output_work_area(shell, shsurf->output, &area);
	geometry = weston_desktop_surface_get_geometry(shsurf->desktop_surface);

	weston_view_set_position(shsurf->view,
				 area.x - geometry.x,
				 area.y - geometry.y);
}

static void
set_position_from_xwayland(struct shell_surface *shsurf)
{
	struct weston_geometry geometry;
	int x;
	int y;
	struct weston_output *output;
	pixman_rectangle32_t area;

	assert(shsurf->xwayland.is_set);

	geometry = weston_desktop_surface_get_geometry(shsurf->desktop_surface);
	x = shsurf->xwayland.x - geometry.x;
	y = shsurf->xwayland.y - geometry.y;

	/* Make sure the position given from xwayland is a part of workarea */
	output = get_output_containing(shsurf->shell, shsurf->xwayland.x, shsurf->xwayland.y, false);
	if (output) {
		get_output_work_area(shsurf->shell, output, &area);
		/* Use xwayland position as this is the X app's origin of client area */
		if (shsurf->xwayland.x >= area.x &&
		    shsurf->xwayland.y >= area.y &&
		    shsurf->xwayland.x <= (int32_t)(area.x + area.width - TITLEBAR_GRAB_MARGIN_X) &&
		    shsurf->xwayland.y <= (int32_t)(area.y + area.height - TITLEBAR_GRAB_MARGIN_Y)) {

			weston_view_set_position(shsurf->view, x, y);

			shell_rdp_debug(shsurf->shell, "%s: XWM %d, %d; geometry %d, %d; view %d, %d\n",
				   __func__, shsurf->xwayland.x, shsurf->xwayland.y,
				   geometry.x, geometry.y, x, y);

			return;
		}
	}

	/* Otherwise, move to default initial position */
	weston_view_set_initial_position(shsurf);
}

static void
set_default_position_from_parent(struct shell_surface *shsurf)
{
	struct weston_geometry parent_geometry, geometry;
	int32_t x;
	int32_t y;

	parent_geometry = weston_desktop_surface_get_geometry(shsurf->parent->desktop_surface);
	geometry = weston_desktop_surface_get_geometry(shsurf->desktop_surface);

	x = parent_geometry.x + (parent_geometry.width - geometry.width) / 2;
	y = parent_geometry.y + (parent_geometry.height - geometry.height) / 2;

	x += shsurf->parent->view->geometry.x;
	y += shsurf->parent->view->geometry.y;

	shell_rdp_debug_verbose(shsurf->shell, "%s: view:%p, (%d, %d)\n",
				__func__, shsurf->view, x, y); 

	weston_view_set_position(shsurf->view, x, y);
}

static void
map(struct desktop_shell *shell, struct shell_surface *shsurf,
    int32_t sx, int32_t sy)
{
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(shsurf->desktop_surface);
	struct weston_compositor *compositor = shell->compositor;
	struct weston_seat *seat;

	/* initial positioning, see also configure() */
	if (shsurf->state.fullscreen) {
		center_on_output(shsurf->view, shsurf->fullscreen_output);
		shell_map_fullscreen(shsurf);
	} else if (shsurf->state.maximized) {
		set_maximized_position(shell, shsurf);
	} else if (shsurf->xwayland.is_set) {
		set_position_from_xwayland(shsurf);
	} else if (shsurf->parent) {
		set_default_position_from_parent(shsurf);
	} else {
		weston_view_set_initial_position(shsurf);
	}

	/* Surface stacking order, see also activate(). */
	shell_surface_update_layer(shsurf);

	weston_view_update_transform(shsurf->view);
	shsurf->view->is_mapped = true;
	if (shsurf->state.maximized) {
		surface->output = shsurf->output;
		weston_view_set_output(shsurf->view, shsurf->output);
	}

	wl_list_for_each(seat, &compositor->seat_list, link)
		activate(shell, shsurf->view, seat,
			 WESTON_ACTIVATE_FLAG_CONFIGURE);
}

static void
desktop_surface_committed(struct weston_desktop_surface *desktop_surface,
			  int32_t sx, int32_t sy, void *data)
{
	struct shell_surface *shsurf =
		weston_desktop_surface_get_user_data(desktop_surface);
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(desktop_surface);
	struct weston_surface_rail_state *rail_state =
		(struct weston_surface_rail_state *)surface->backend_state;
	struct weston_view *view = shsurf->view;
	struct desktop_shell *shell = data;
	bool was_fullscreen;
	bool was_maximized;

	if (surface->width == 0)
		return;

	was_fullscreen = shsurf->state.fullscreen;
	was_maximized = shsurf->state.maximized;

	shsurf->state.fullscreen =
		weston_desktop_surface_get_fullscreen(desktop_surface);
	shsurf->state.maximized =
		weston_desktop_surface_get_maximized(desktop_surface);

	if (!weston_surface_is_mapped(surface)) {
		map(shell, shsurf, sx, sy);
		surface->is_mapped = true;
		return;
	}

	if (sx == 0 && sy == 0 &&
	    shsurf->last_width == surface->width &&
	    shsurf->last_height == surface->height &&
	    was_fullscreen == shsurf->state.fullscreen &&
	    was_maximized == shsurf->state.maximized)
	    return;

	if (was_fullscreen && !shsurf->state.fullscreen)
		unset_fullscreen(shsurf);
	if (was_maximized && !shsurf->state.maximized)
		unset_maximized(shsurf);

	if ((shsurf->state.fullscreen || shsurf->state.maximized)) {
		if (!shsurf->saved_position_valid) {
			shsurf->saved_x = shsurf->view->geometry.x;
			shsurf->saved_y = shsurf->view->geometry.y;
			shsurf->saved_position_valid = true;
		}

		if (!shsurf->saved_showstate_valid) {
			if (shsurf->state.fullscreen)
				rail_state->showState_requested = RDP_WINDOW_SHOW_FULLSCREEN;
			else
				rail_state->showState_requested = RDP_WINDOW_SHOW_MAXIMIZED;
			shsurf->saved_showstate = rail_state ? rail_state->showState : RDP_WINDOW_SHOW;
			shsurf->saved_showstate_valid = true;
		}

		if (!wl_list_empty(&shsurf->rotation.transform.link)) {
			wl_list_remove(&shsurf->rotation.transform.link);
			wl_list_init(&shsurf->rotation.transform.link);
			weston_view_geometry_dirty(shsurf->view);
			shsurf->saved_rotation_valid = true;
		}
	}

	if (shsurf->state.fullscreen) {
		shell_configure_fullscreen(shsurf);
	} else if (shsurf->state.maximized) {
		set_maximized_position(shell, shsurf);
		surface->output = shsurf->output;
	} else if (shsurf->snapped.is_snapped) {
		weston_view_set_position(shsurf->view, shsurf->snapped.x, shsurf->snapped.y);
	} else {
		float from_x, from_y;
		float to_x, to_y;
		float x, y;

		if (shsurf->resize_edges) {
			sx = 0;
			sy = 0;

			if (shsurf->resize_edges & WL_SHELL_SURFACE_RESIZE_LEFT)
				sx = shsurf->last_width - surface->width;
			if (shsurf->resize_edges & WL_SHELL_SURFACE_RESIZE_TOP)
				sy = shsurf->last_height - surface->height;

			weston_view_to_global_float(shsurf->view, 0, 0, &from_x, &from_y);
			weston_view_to_global_float(shsurf->view, sx, sy, &to_x, &to_y);
			x = shsurf->view->geometry.x + to_x - from_x;
			y = shsurf->view->geometry.y + to_y - from_y;

			weston_view_set_position(shsurf->view, x, y);
		}
	}

	shsurf->last_width = surface->width;
	shsurf->last_height = surface->height;

	/* XXX: would a fullscreen surface need the same handling? */
	if (surface->output) {
		wl_list_for_each(view, &surface->views, surface_link)
			weston_view_update_transform(view);
	}
}

static void
get_maximized_size(struct shell_surface *shsurf, int32_t *width, int32_t *height)
{
	struct desktop_shell *shell;
	pixman_rectangle32_t area;

	shell = shell_surface_get_shell(shsurf);
	get_output_work_area(shell, shsurf->output, &area);

	*width = area.width;
	*height = area.height;
}

static void
set_fullscreen(struct shell_surface *shsurf, bool fullscreen,
	       struct weston_output *output)
{
	struct weston_desktop_surface *desktop_surface = shsurf->desktop_surface;
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(shsurf->desktop_surface);
	struct weston_surface_rail_state *rail_state =
		(struct weston_surface_rail_state *)surface->backend_state;
	int32_t width = 0, height = 0;

	if (!rail_state)
		return;

	if (fullscreen) {
		/* if window is created as fullscreen, always set previous state as normal */
		shsurf->saved_showstate = weston_surface_is_mapped(surface) ? \
			rail_state->showState : RDP_WINDOW_SHOW;
		shsurf->saved_showstate_valid = true;
		rail_state->showState_requested = RDP_WINDOW_SHOW_FULLSCREEN;

		/* handle clients launching in fullscreen */
		if (output == NULL && !weston_surface_is_mapped(surface)) {
			/* Set the output to the one that has focus currently. */
			output = get_focused_output(surface->compositor);
		}

		shell_surface_set_output(shsurf, output);
		shsurf->fullscreen_output = shsurf->output;

		width = shsurf->output->width;
		height = shsurf->output->height;
	} else if (weston_desktop_surface_get_maximized(desktop_surface)) {
		shsurf->saved_showstate = rail_state->showState;
		shsurf->saved_showstate_valid = true;
		rail_state->showState_requested = RDP_WINDOW_SHOW_MAXIMIZED;
		get_maximized_size(shsurf, &width, &height);
	} else {
		if (shsurf->saved_showstate_valid)
			rail_state->showState_requested = shsurf->saved_showstate;
		else
			rail_state->showState_requested = RDP_WINDOW_SHOW;
		shsurf->saved_showstate_valid = false;
	}

	weston_desktop_surface_set_fullscreen(desktop_surface, fullscreen);
	weston_desktop_surface_set_size(desktop_surface, width, height);
}

static void
desktop_surface_move(struct weston_desktop_surface *desktop_surface,
		     struct weston_seat *seat, uint32_t serial, void *shell)
{
	struct weston_pointer *pointer = weston_seat_get_pointer(seat);
	struct weston_touch *touch = weston_seat_get_touch(seat);
	struct shell_surface *shsurf =
		weston_desktop_surface_get_user_data(desktop_surface);
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(shsurf->desktop_surface);
	struct wl_resource *resource = surface->resource;
	struct weston_surface *focus;

	if (pointer &&
	    pointer->focus &&
	    pointer->button_count > 0 &&
	    pointer->grab_serial == serial) {
		focus = weston_surface_get_main_surface(pointer->focus->surface);
		if ((focus == surface) &&
		    (surface_move(shsurf, pointer, true) < 0))
			wl_resource_post_no_memory(resource);
	} else if (touch &&
		   touch->focus &&
		   touch->grab_serial == serial) {
		focus = weston_surface_get_main_surface(touch->focus->surface);
		if ((focus == surface) &&
		    (surface_touch_move(shsurf, touch) < 0))
			wl_resource_post_no_memory(resource);
	}
}

static void
desktop_surface_resize(struct weston_desktop_surface *desktop_surface,
		       struct weston_seat *seat, uint32_t serial,
		       enum weston_desktop_surface_edge edges, void *shell)
{
	struct weston_pointer *pointer = weston_seat_get_pointer(seat);
	struct shell_surface *shsurf =
		weston_desktop_surface_get_user_data(desktop_surface);
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(shsurf->desktop_surface);
	struct wl_resource *resource = surface->resource;
	struct weston_surface *focus;

	if (!pointer ||
	    pointer->button_count == 0 ||
	    pointer->grab_serial != serial ||
	    pointer->focus == NULL)
		return;

	focus = weston_surface_get_main_surface(pointer->focus->surface);
	if (focus != surface)
		return;

	if (surface_resize(shsurf, pointer, edges) < 0)
		wl_resource_post_no_memory(resource);
}

static void
desktop_surface_set_parent(struct weston_desktop_surface *desktop_surface,
			   struct weston_desktop_surface *parent,
			   void *shell)
{
	struct shell_surface *shsurf_parent;
	struct shell_surface *shsurf =
		weston_desktop_surface_get_user_data(desktop_surface);

	/* unlink any potential child */
	wl_list_remove(&shsurf->children_link);

	if (parent) {
		shsurf_parent = weston_desktop_surface_get_user_data(parent);
		if (shsurf_parent) {
			wl_list_insert(shsurf_parent->children_list.prev,
				       &shsurf->children_link);
			/* libweston-desktop doesn't establish parent/child relationship
			   with weston_desktop_api shell_desktop_api.set_parent call,
			   thus calling weston_desktop_surface_get_parent won't work,
			   so shell need to track by itself. This also means child's
			   geometry won't be adjusted to relative to parent. */
			shsurf->parent = shsurf_parent;
		} else {
			shell_rdp_debug_error(shsurf->shell, "RDP shell: parent is not toplevel surface\n");
			wl_list_init(&shsurf->children_link);
			shsurf->parent = NULL;
		}
	} else {
		wl_list_init(&shsurf->children_link);
		shsurf->parent = NULL;
	}
}

static void
desktop_surface_fullscreen_requested(struct weston_desktop_surface *desktop_surface,
				     bool fullscreen,
				     struct weston_output *output, void *shell)
{
	struct shell_surface *shsurf =
		weston_desktop_surface_get_user_data(desktop_surface);

	set_fullscreen(shsurf, fullscreen, output);
}

static void
set_maximized(struct shell_surface *shsurf, bool maximized)
{
	struct weston_desktop_surface *desktop_surface = shsurf->desktop_surface;
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(shsurf->desktop_surface);
	struct weston_surface_rail_state *rail_state =
		(struct weston_surface_rail_state *)surface->backend_state;
	struct weston_geometry geometry;
	int32_t width = 0, height = 0;

	if (!rail_state)
		return;

	if (maximized) {
		struct weston_output *output;

		/* if window is created as maximized, always set previous state as normal */
		shsurf->saved_showstate = weston_surface_is_mapped(surface) ? \
			rail_state->showState : RDP_WINDOW_SHOW;
		shsurf->saved_showstate_valid = true;
		rail_state->showState_requested = RDP_WINDOW_SHOW_MAXIMIZED;

		if (!weston_surface_is_mapped(surface))
			output = get_focused_output(surface->compositor);
		else
			/* TODO: Need to revisit here for local move. */
			output = surface->output;

		shell_surface_set_output(shsurf, output);

		get_maximized_size(shsurf, &width, &height);

		/* save current window size */
		geometry = weston_desktop_surface_get_geometry(shsurf->desktop_surface);
		shsurf->maximized.saved_surface_width = surface->width;
		shsurf->maximized.saved_width = geometry.width;
		shsurf->maximized.saved_height = geometry.height;

		/* send current minmax info to client */
		shell_send_minmax_info
			(weston_desktop_surface_get_surface(shsurf->desktop_surface));
	} else {
		if (shsurf->saved_showstate_valid)
			rail_state->showState_requested = shsurf->saved_showstate;
		else
			rail_state->showState_requested = RDP_WINDOW_SHOW;
		shsurf->saved_showstate_valid = false;
	}
	weston_desktop_surface_set_maximized(desktop_surface, maximized);
	weston_desktop_surface_set_size(desktop_surface, width, height);
}

static void
desktop_surface_maximized_requested(struct weston_desktop_surface *desktop_surface,
				    bool maximized, void *shell)
{
	struct shell_surface *shsurf =
		weston_desktop_surface_get_user_data(desktop_surface);

	set_maximized(shsurf, maximized);
}

static void
desktop_surface_minimized_requested(struct weston_desktop_surface *desktop_surface,
				    void *shell)
{
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(desktop_surface);

	 /* apply compositor's own minimization logic (hide) */
	set_minimized(surface);
}

static void
desktop_surface_set_window_icon(struct weston_desktop_surface *desktop_surface,
				int32_t width, int32_t height, int32_t bpp,
				void *bits, void *user_data)
{
	shell_surface_set_window_icon(desktop_surface, width, height, bpp, bits, user_data);
}

static void
shell_backend_request_window_minimize(struct weston_surface *surface)
{
	struct shell_surface *shsurf = get_shell_surface(surface);

	if (!shsurf)
		return;

	set_minimized(surface);
}

static void
shell_backend_request_window_maximize(struct weston_surface *surface)
{
	struct shell_surface *shsurf = get_shell_surface(surface);
	const struct weston_xwayland_surface_api *api;

	if (!shsurf)
		return;

	if (shsurf->shell->is_localmove_pending) {
		/* Delay maximizing the surface until the move ends. The client
		 * will send up a snap request once the move ends, we'll 
		 * maximize the window at that time once we know which monitor
		 * to maximize on.
		 */
		shsurf->snapped.is_maximized_requested = true;
		return;
	}

	api = shsurf->shell->xwayland_surface_api;
	if (!api) {
		api = weston_xwayland_surface_get_api(shsurf->shell->compositor);
		shsurf->shell->xwayland_surface_api = api;
	}
	if (api && api->is_xwayland_surface(surface)) {
		api->set_maximized(surface, true);
	} else {
		set_maximized(shsurf, true);
	}
}

static void
shell_backend_request_window_restore(struct weston_surface *surface)
{
	struct shell_surface *shsurf = get_shell_surface(surface);
	struct weston_surface_rail_state *rail_state =
		(struct weston_surface_rail_state *)surface->backend_state;
	const struct weston_xwayland_surface_api *api;

	if (!shsurf)
		return;

	if (!rail_state)
		return;

	if (rail_state->showState == RDP_WINDOW_SHOW_MINIMIZED) {
		set_unminimized(surface);
	} else if (shsurf->snapped.is_snapped) {
		/* weston_desktop_surface_set_size() expects the size in window geometry coordinates */
		/* saved_width and saved_height is already based on window geometry. */
		weston_desktop_surface_set_size(
			shsurf->desktop_surface,
			shsurf->snapped.saved_width, shsurf->snapped.saved_height);
		weston_view_set_position(shsurf->view, 
			shsurf->snapped.saved_x, shsurf->snapped.saved_y);
		shsurf->snapped.is_snapped = false;
		rail_state->isWindowSnapped = false;
	} else if (shsurf->state.fullscreen) {
		/* fullscreen is treated as normal (aka restored) state in
		   Windows client, thus there should be not be 'restore'
		   request to be made while in fullscreen state. */
		shell_rdp_debug(shsurf->shell,
			 "%s: surface:%p is requested to be restored while in fullscreen\n",
			__func__, surface);
	} else if (shsurf->state.maximized) {
		api = shsurf->shell->xwayland_surface_api;
		if (!api) {
			api = weston_xwayland_surface_get_api(shsurf->shell->compositor);
			shsurf->shell->xwayland_surface_api = api;
		}
		if (api && api->is_xwayland_surface(surface)) {
			api->set_maximized(surface, false);
		} else {
			set_maximized(shsurf, false);
		}
	}
}

static void
shell_backend_request_window_move(struct weston_surface *surface, int x, int y, int width, int height)
{
	struct weston_view *view;
	struct shell_surface *shsurf = get_shell_surface(surface);

	view = get_default_view(surface);
	if (!view)
		return;

	if (shsurf && shsurf->shell->is_localmove_pending) {
		shsurf->shell->is_localmove_pending = false;
	}

	assert(!shsurf->snapped.is_maximized_requested);

	if (surface->width != width || surface->height != height) {
		//TODO: support window resize (width x height)
		shell_rdp_debug(shsurf->shell, "%s: surface:%p is resized (%dx%d) -> (%d,%d)\n",
			__func__, surface, surface->width, surface->height, width, height);
	}

	weston_view_set_position(view, x, y);

	shell_rdp_debug(shsurf->shell, "%s: surface:%p is moved to (%d,%d) %dx%d\n",
		__func__, surface, x, y, width, height);
}

static void
shell_backend_request_window_snap(struct weston_surface *surface, int x, int y, int width, int height)
{
	struct weston_view *view;
	struct shell_surface *shsurf = get_shell_surface(surface);
	struct weston_geometry geometry;
	struct weston_surface_rail_state *rail_state =
		(struct weston_surface_rail_state *)surface->backend_state;

	view = get_default_view(surface);
	if (!view || !shsurf || !rail_state)
		return;

	if (shsurf->shell->is_localmove_pending) {
		shsurf->shell->is_localmove_pending = false;
	}

	if (shsurf->state.maximized) {
		return;
	}

	if (shsurf->snapped.is_maximized_requested) {
		assert(!shsurf->shell->is_localmove_pending);
		
		shsurf->snapped.is_maximized_requested = false;

		/* We may need to pick a new output for the window
		 * based on the last position of the mouse when the
		 * grab event finished.
		 */
		struct weston_output *output = get_output_containing(shsurf->shell, 
				shsurf->snapped.last_grab_x, 
				shsurf->snapped.last_grab_y,
				true);

		weston_view_set_output(shsurf->view, output);
		shell_surface_set_output(shsurf, output);

		shell_backend_request_window_maximize(surface);
		return;
	}

	geometry = weston_desktop_surface_get_geometry(shsurf->desktop_surface);

	if (!shsurf->snapped.is_snapped) {
		shsurf->snapped.saved_x = shsurf->view->geometry.x;
		shsurf->snapped.saved_y = shsurf->view->geometry.y;
		/* saved_width and height is based on window geometry */
		shsurf->snapped.saved_surface_width = surface->width;
		shsurf->snapped.saved_width = geometry.width;
		shsurf->snapped.saved_height = geometry.height;
	}
	shsurf->snapped.is_snapped = true;
	rail_state->isWindowSnapped = true;

	if (surface->width != width || surface->height != height) {
		struct weston_desktop_surface *desktop_surface =
			weston_surface_get_desktop_surface(surface);

		struct weston_size max_size = weston_desktop_surface_get_max_size(desktop_surface);
		struct weston_size min_size = weston_desktop_surface_get_min_size(desktop_surface);
		/* weston_desktop_surface_set_size() expects the size in window geometry coordinates */
		width -= (surface->width - geometry.width);
		height -= (surface->height - geometry.height);

		min_size.width = MAX(1, min_size.width);
		min_size.height = MAX(1, min_size.height);

		if (width < min_size.width)
			width = min_size.width;
		else if (max_size.width > 0 && width > max_size.width)
			width = max_size.width;
		if (height < min_size.height)
			height = min_size.height;
		else if (max_size.height > 0 && height > max_size.height)
			height = max_size.height;

		shell_rdp_debug_verbose(shsurf->shell, "%s: surface:%p is resized (%dx%d) -> (%d,%d)\n",
			__func__, surface, surface->width, surface->height, width, height);
		weston_desktop_surface_set_size(desktop_surface, width, height);
	}

	weston_view_set_position(view, x, y);

	shsurf->snapped.x = x;
	shsurf->snapped.y = y;
	shsurf->snapped.width = width; // save width in window geometry coordinates.
	shsurf->snapped.height = height; // save height in window geometry coordinates.

	shell_rdp_debug_verbose(shsurf->shell, "%s: surface:%p is snapped at (%d,%d) %dx%d\n",
		__func__, surface, x, y, width, height);
}

static void
set_busy_cursor(struct shell_surface *shsurf, struct weston_pointer *pointer)
{
	struct shell_grab *grab;

	if (pointer->grab->interface == &busy_cursor_grab_interface)
		return;

	grab = malloc(sizeof *grab);
	if (!grab)
		return;

	shell_grab_start(grab, &busy_cursor_grab_interface, shsurf, pointer,
			 WESTON_RDPRAIL_SHELL_CURSOR_BUSY);
	/* Mark the shsurf as ungrabbed so that button binding is able
	 * to move it. */
	shsurf->grabbed = 0;
}

static void
end_busy_cursor(struct weston_compositor *compositor,
		struct weston_desktop_client *desktop_client)
{
	struct shell_surface *shsurf;
	struct shell_grab *grab;
	struct weston_seat *seat;

	wl_list_for_each(seat, &compositor->seat_list, link) {
		struct weston_pointer *pointer = weston_seat_get_pointer(seat);
		struct weston_desktop_client *grab_client;

		if (!pointer)
			continue;

		if (pointer->grab->interface != &busy_cursor_grab_interface)
			continue;

		grab = (struct shell_grab *) pointer->grab;
		shsurf = grab->shsurf;
		if (!shsurf)
			continue;

		grab_client =
			weston_desktop_surface_get_client(shsurf->desktop_surface);
		if (grab_client  == desktop_client) {
			shell_grab_end(grab);
			free(grab);
		}
	}
}

static void
desktop_surface_set_unresponsive(struct weston_desktop_surface *desktop_surface,
				 void *user_data)
{
	struct shell_surface *shsurf =
		weston_desktop_surface_get_user_data(desktop_surface);
	bool *unresponsive = user_data;

	shsurf->unresponsive = *unresponsive;
}

static void
desktop_surface_ping_timeout(struct weston_desktop_client *desktop_client,
			     void *shell_)
{
	struct desktop_shell *shell = shell_;
	struct shell_surface *shsurf;
	struct weston_seat *seat;
	bool unresponsive = true;

	weston_desktop_client_for_each_surface(desktop_client,
					       desktop_surface_set_unresponsive,
					       &unresponsive);


	wl_list_for_each(seat, &shell->compositor->seat_list, link) {
		struct weston_pointer *pointer = weston_seat_get_pointer(seat);
		struct weston_desktop_client *grab_client;

		if (!pointer || !pointer->focus)
			continue;

		shsurf = get_shell_surface(pointer->focus->surface);
		if (!shsurf)
			continue;

		grab_client =
			weston_desktop_surface_get_client(shsurf->desktop_surface);
		if (grab_client == desktop_client)
			set_busy_cursor(shsurf, pointer);
	}
}

static void
desktop_surface_pong(struct weston_desktop_client *desktop_client,
		     void *shell_)
{
	struct desktop_shell *shell = shell_;
	bool unresponsive = false;

	weston_desktop_client_for_each_surface(desktop_client,
					       desktop_surface_set_unresponsive,
					       &unresponsive);
	end_busy_cursor(shell->compositor, desktop_client);
}

static void
desktop_surface_set_xwayland_position(struct weston_desktop_surface *surface,
				      int32_t x, int32_t y, void *shell_)
{
	struct shell_surface *shsurf =
		weston_desktop_surface_get_user_data(surface);

	shsurf->xwayland.x = x;
	shsurf->xwayland.y = y;
	shsurf->xwayland.is_set = true;
}

static void
desktop_surface_get_position(struct weston_desktop_surface *surface,
			     int32_t *x, int32_t *y,
			     void *shell_)
{
	struct shell_surface *shsurf = weston_desktop_surface_get_user_data(surface);
	if (shsurf) {
		*x = shsurf->view->geometry.x;
		*y = shsurf->view->geometry.y;
	} else {
		/* Ideally libweston-desktop/xwayland.c must not call shell if
		   the surface is not reported to shell (surface.state == XWAYLAND),
		   but unfortunately this does happen, thus here workaround the crash
		   by returning (0,0) in such case. */
		*x = 0;
		*y = 0;
	}
}

static bool
area_contain_point(pixman_rectangle32_t *area, int x, int y) 
{
	return x >= area->x &&
	       y >= area->y &&
	       x < area->x + (int)area->width &&
	       y < area->y + (int)area->height;
}

static void
desktop_surface_move_xwayland_position(struct weston_desktop_surface *desktop_surface,
				       int32_t x, int32_t y, void *shell_)
{
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(desktop_surface);
	struct shell_surface *shsurf =
		weston_desktop_surface_get_user_data(desktop_surface);
	struct desktop_shell *shell = shsurf->shell;
	const struct weston_xwayland_surface_api *api;
	struct weston_geometry geometry;

	assert(shell == shell_);

	geometry = weston_desktop_surface_get_geometry(desktop_surface);
	if (shsurf->view->geometry.x == x - geometry.x &&
	    shsurf->view->geometry.y == y - geometry.y)
		return;

	api = shell->xwayland_surface_api;
	if (!api) {
		api = weston_xwayland_surface_get_api(shell->compositor);
		shell->xwayland_surface_api = api;
	}
	if (api && api->is_xwayland_surface(surface)) {
		/* TODO: Make sure the position given from xwayland is a part of workarea,
		         But this is not simple, for example, app can have accompanying
		         window which move along with other main window, in such case,
		         often, it's totally fine the accompanying goes out of workarea. */
		/* Below code to make sure window title bar is grab-able */
		struct weston_output *output;
		int left, right, top;
		pixman_rectangle32_t area;
		bool visible = false;

		left = x + TITLEBAR_GRAB_MARGIN_X;
		right = x + (surface->width - TITLEBAR_GRAB_MARGIN_X);
		top = y + TITLEBAR_GRAB_MARGIN_Y;

		/* check uppper left */
		output = get_output_containing(shell, left, top, false);
		if (output) {
			get_output_work_area(shell, output, &area);
			visible = area_contain_point(&area, left, top);
		}
		if (!visible) {
			/* check upper right */
			output = get_output_containing(shell, right, top, false);
			if (output) {
				get_output_work_area(shell, output, &area);
				visible = area_contain_point(&area, right, top);
			}
		}
		if (visible) {
			x -= geometry.x;
			y -= geometry.y;
			weston_view_set_position(shsurf->view, x, y);
			weston_compositor_schedule_repaint(shell->compositor);
			shell_rdp_debug_verbose(shell, "%s: surface:%p, position (%d,%d)\n",
				__func__, surface, x, y);
		}
	} else {
		shell_rdp_debug_error(shell, "%s: surface:%p is not from xwayland\n",
			__func__, surface);
	}
}

static const struct weston_desktop_api shell_desktop_api = {
	.struct_size = sizeof(struct weston_desktop_api),
	.surface_added = desktop_surface_added,
	.surface_removed = desktop_surface_removed,
	.committed = desktop_surface_committed,
	.move = desktop_surface_move,
	.resize = desktop_surface_resize,
	.set_parent = desktop_surface_set_parent,
	.fullscreen_requested = desktop_surface_fullscreen_requested,
	.maximized_requested = desktop_surface_maximized_requested,
	.minimized_requested = desktop_surface_minimized_requested,
	.ping_timeout = desktop_surface_ping_timeout,
	.pong = desktop_surface_pong,
	.set_xwayland_position = desktop_surface_set_xwayland_position,
	.get_position = desktop_surface_get_position,
	.move_xwayland_position = desktop_surface_move_xwayland_position,
	.set_window_icon = desktop_surface_set_window_icon,
};

/* ************************ *
 * end of libweston-desktop *
 * ************************ */

static struct shell_output *
find_shell_output_from_weston_output(struct desktop_shell *shell,
				     struct weston_output *output)
{
	struct shell_output *shell_output;

	wl_list_for_each(shell_output, &shell->output_list, link) {
		if (shell_output->output == output)
			return shell_output;
	}

	return NULL;
}

static void
move_binding(struct weston_pointer *pointer, const struct timespec *time,
	     uint32_t button, void *data)
{
	struct weston_surface *focus;
	struct weston_surface *surface;
	struct shell_surface *shsurf;

	if (pointer->focus == NULL)
		return;

	focus = pointer->focus->surface;

	surface = weston_surface_get_main_surface(focus);
	if (surface == NULL)
		return;

	shsurf = get_shell_surface(surface);
	if (shsurf == NULL ||
	    weston_desktop_surface_get_fullscreen(shsurf->desktop_surface) ||
	    weston_desktop_surface_get_maximized(shsurf->desktop_surface))
		return;

	shell_rdp_debug_verbose(shsurf->shell, "%s\n", __func__);
	surface_move(shsurf, pointer, false);
}

static void
maximize_binding(struct weston_keyboard *keyboard, const struct timespec *time,
		 uint32_t button, void *data)
{
	struct weston_surface *focus = keyboard->focus;
	struct weston_surface *surface;
	struct shell_surface *shsurf;

	surface = weston_surface_get_main_surface(focus);
	if (surface == NULL)
		return;

	shsurf = get_shell_surface(surface);
	if (shsurf == NULL)
		return;

	shell_rdp_debug_verbose(shsurf->shell, "%s\n", __func__);
	set_maximized(shsurf, !weston_desktop_surface_get_maximized(shsurf->desktop_surface));
}

static void
fullscreen_binding(struct weston_keyboard *keyboard,
		   const struct timespec *time, uint32_t button, void *data)
{
	struct weston_surface *focus = keyboard->focus;
	struct weston_surface *surface;
	struct shell_surface *shsurf;
	bool fullscreen;

	surface = weston_surface_get_main_surface(focus);
	if (surface == NULL)
		return;

	shsurf = get_shell_surface(surface);
	if (shsurf == NULL)
		return;

	fullscreen =
		weston_desktop_surface_get_fullscreen(shsurf->desktop_surface);

	shell_rdp_debug_verbose(shsurf->shell, "%s: fullscreen:%d\n", __func__, !fullscreen);
	set_fullscreen(shsurf, !fullscreen, NULL);
}

static void
touch_move_binding(struct weston_touch *touch, const struct timespec *time, void *data)
{
	struct weston_surface *focus;
	struct weston_surface *surface;
	struct shell_surface *shsurf;

	if (touch->focus == NULL)
		return;

	focus = touch->focus->surface;
	surface = weston_surface_get_main_surface(focus);
	if (surface == NULL)
		return;

	shsurf = get_shell_surface(surface);
	if (shsurf == NULL ||
	    weston_desktop_surface_get_fullscreen(shsurf->desktop_surface) ||
	    weston_desktop_surface_get_maximized(shsurf->desktop_surface))
		return;

	surface_touch_move(shsurf, touch);
}

static void
resize_binding(struct weston_pointer *pointer, const struct timespec *time,
	       uint32_t button, void *data)
{
	struct weston_surface *focus;
	struct weston_surface *surface;
	uint32_t edges = 0;
	int32_t x, y;
	struct shell_surface *shsurf;

	if (pointer->focus == NULL)
		return;

	focus = pointer->focus->surface;

	surface = weston_surface_get_main_surface(focus);
	if (surface == NULL)
		return;

	shsurf = get_shell_surface(surface);
	if (shsurf == NULL ||
	    weston_desktop_surface_get_fullscreen(shsurf->desktop_surface) ||
	    weston_desktop_surface_get_maximized(shsurf->desktop_surface))
		return;

	weston_view_from_global(shsurf->view,
				wl_fixed_to_int(pointer->grab_x),
				wl_fixed_to_int(pointer->grab_y),
				&x, &y);

	if (x < surface->width / 3)
		edges |= WL_SHELL_SURFACE_RESIZE_LEFT;
	else if (x < 2 * surface->width / 3)
		edges |= 0;
	else
		edges |= WL_SHELL_SURFACE_RESIZE_RIGHT;

	if (y < surface->height / 3)
		edges |= WL_SHELL_SURFACE_RESIZE_TOP;
	else if (y < 2 * surface->height / 3)
		edges |= 0;
	else
		edges |= WL_SHELL_SURFACE_RESIZE_BOTTOM;

	shell_rdp_debug_verbose(shsurf->shell, "%s edges:%x\n", __func__, edges);
	surface_resize(shsurf, pointer, edges);
}

static void
surface_opacity_binding(struct weston_pointer *pointer,
			const struct timespec *time,
			struct weston_pointer_axis_event *event,
			void *data)
{
	float step = 0.005;
	struct shell_surface *shsurf;
	struct weston_surface *focus = pointer->focus->surface;
	struct weston_surface *surface;

	/* XXX: broken for windows containing sub-surfaces */
	surface = weston_surface_get_main_surface(focus);
	if (surface == NULL)
		return;

	shsurf = get_shell_surface(surface);
	if (!shsurf)
		return;

	shsurf->view->alpha -= event->value * step;

	if (shsurf->view->alpha > 1.0)
		shsurf->view->alpha = 1.0;
	if (shsurf->view->alpha < step)
		shsurf->view->alpha = step;
	shell_rdp_debug_verbose(shsurf->shell, "%s alpha:%f\n", __func__, shsurf->view->alpha);

	weston_view_geometry_dirty(shsurf->view);
	weston_surface_damage(surface);
}

static void
terminate_binding(struct weston_keyboard *keyboard, const struct timespec *time,
		  uint32_t key, void *data)
{
	struct weston_compositor *compositor = data;

	weston_compositor_exit(compositor);
}

static void
close_focused_app_binding(struct weston_keyboard *keyboard, const struct timespec *time,
		  uint32_t key, void *data)
{
	struct weston_surface *focus = keyboard->focus;
	struct weston_surface *surface;

	surface = weston_surface_get_main_surface(focus);
	if (surface == NULL)
		return;

	shell_backend_request_window_close(surface);
}

static void
rotate_grab_motion(struct weston_pointer_grab *grab,
		   const struct timespec *time,
		   struct weston_pointer_motion_event *event)
{
	struct rotate_grab *rotate =
		container_of(grab, struct rotate_grab, base.grab);
	struct weston_pointer *pointer = grab->pointer;
	struct shell_surface *shsurf = rotate->base.shsurf;
	struct weston_surface *surface;
	float cx, cy, dx, dy, cposx, cposy, dposx, dposy, r;

	weston_pointer_move(pointer, event);

	if (!shsurf)
		return;

	surface = weston_desktop_surface_get_surface(shsurf->desktop_surface);

	cx = 0.5f * surface->width;
	cy = 0.5f * surface->height;

	dx = wl_fixed_to_double(pointer->x) - rotate->center.x;
	dy = wl_fixed_to_double(pointer->y) - rotate->center.y;
	r = sqrtf(dx * dx + dy * dy);

	wl_list_remove(&shsurf->rotation.transform.link);
	weston_view_geometry_dirty(shsurf->view);

	if (r > 20.0f) {
		struct weston_matrix *matrix =
			&shsurf->rotation.transform.matrix;

		weston_matrix_init(&rotate->rotation);
		weston_matrix_rotate_xy(&rotate->rotation, dx / r, dy / r);

		weston_matrix_init(matrix);
		weston_matrix_translate(matrix, -cx, -cy, 0.0f);
		weston_matrix_multiply(matrix, &shsurf->rotation.rotation);
		weston_matrix_multiply(matrix, &rotate->rotation);
		weston_matrix_translate(matrix, cx, cy, 0.0f);

		wl_list_insert(
			&shsurf->view->geometry.transformation_list,
			&shsurf->rotation.transform.link);
	} else {
		wl_list_init(&shsurf->rotation.transform.link);
		weston_matrix_init(&shsurf->rotation.rotation);
		weston_matrix_init(&rotate->rotation);
	}

	/* We need to adjust the position of the surface
	 * in case it was resized in a rotated state before */
	cposx = shsurf->view->geometry.x + cx;
	cposy = shsurf->view->geometry.y + cy;
	dposx = rotate->center.x - cposx;
	dposy = rotate->center.y - cposy;
	if (dposx != 0.0f || dposy != 0.0f) {
		weston_view_set_position(shsurf->view,
					 shsurf->view->geometry.x + dposx,
					 shsurf->view->geometry.y + dposy);
	}

	/* Repaint implies weston_view_update_transform(), which
	 * lazily applies the damage due to rotation update.
	 */
	weston_compositor_schedule_repaint(surface->compositor);
}

static void
rotate_grab_button(struct weston_pointer_grab *grab,
		   const struct timespec *time,
		   uint32_t button, uint32_t state_w)
{
	struct rotate_grab *rotate =
		container_of(grab, struct rotate_grab, base.grab);
	struct weston_pointer *pointer = grab->pointer;
	struct shell_surface *shsurf = rotate->base.shsurf;
	enum wl_pointer_button_state state = state_w;

	if (pointer->button_count == 0 &&
	    state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (shsurf)
			weston_matrix_multiply(&shsurf->rotation.rotation,
					       &rotate->rotation);
		shell_grab_end(&rotate->base);
		free(rotate);
	}
}

static void
rotate_grab_cancel(struct weston_pointer_grab *grab)
{
	struct rotate_grab *rotate =
		container_of(grab, struct rotate_grab, base.grab);

	shell_grab_end(&rotate->base);
	free(rotate);
}

static const struct weston_pointer_grab_interface rotate_grab_interface = {
	noop_grab_focus,
	rotate_grab_motion,
	rotate_grab_button,
	noop_grab_axis,
	noop_grab_axis_source,
	noop_grab_frame,
	rotate_grab_cancel,
};

static void
surface_rotate(struct shell_surface *shsurf, struct weston_pointer *pointer)
{
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(shsurf->desktop_surface);
	struct rotate_grab *rotate;
	float dx, dy;
	float r;

	rotate = malloc(sizeof *rotate);
	if (!rotate)
		return;

	weston_view_to_global_float(shsurf->view,
				    surface->width * 0.5f,
				    surface->height * 0.5f,
				    &rotate->center.x, &rotate->center.y);

	dx = wl_fixed_to_double(pointer->x) - rotate->center.x;
	dy = wl_fixed_to_double(pointer->y) - rotate->center.y;
	r = sqrtf(dx * dx + dy * dy);
	if (r > 20.0f) {
		struct weston_matrix inverse;

		weston_matrix_init(&inverse);
		weston_matrix_rotate_xy(&inverse, dx / r, -dy / r);
		weston_matrix_multiply(&shsurf->rotation.rotation, &inverse);

		weston_matrix_init(&rotate->rotation);
		weston_matrix_rotate_xy(&rotate->rotation, dx / r, dy / r);
	} else {
		weston_matrix_init(&shsurf->rotation.rotation);
		weston_matrix_init(&rotate->rotation);
	}

	shell_grab_start(&rotate->base, &rotate_grab_interface, shsurf,
			 pointer, WESTON_RDPRAIL_SHELL_CURSOR_ARROW);
}

/*
//TODO: while RAIL can't do arbirary rotation, but can do 0,90,180,270 degree rotation
//      maybe it can have a new cap for that ?
static void
rotate_binding(struct weston_pointer *pointer, const struct timespec *time,
	       uint32_t button, void *data)
{
	struct weston_surface *focus;
	struct weston_surface *base_surface;
	struct shell_surface *surface;

	if (pointer->focus == NULL)
		return;

	focus = pointer->focus->surface;

	base_surface = weston_surface_get_main_surface(focus);
	if (base_surface == NULL)
		return;

	surface = get_shell_surface(base_surface);
	if (surface == NULL ||
	    weston_desktop_surface_get_fullscreen(surface->desktop_surface) ||
	    weston_desktop_surface_get_maximized(surface->desktop_surface))
		return;

	shell_rdp_debug(surface->shell, "%s\n", __func__);
	surface_rotate(surface, pointer);
}
*/

/* Move all fullscreen layers down to the current workspace and hide their
 * black views. The surfaces' state is set to both fullscreen and lowered,
 * and this is reversed when such a surface is re-configured, see
 * shell_configure_fullscreen() and shell_ensure_fullscreen_black_view().
 *
 * lowering_output = NULL - Lower on all outputs, else only lower on the
 *                   specified output.
 *
 * This should be used when implementing shell-wide overlays, such as
 * the alt-tab switcher, which need to de-promote fullscreen layers. */
void
lower_fullscreen_layer(struct desktop_shell *shell,
		       struct weston_output *lowering_output)
{
	struct workspace *ws;
	struct weston_view *view, *prev;

	ws = get_current_workspace(shell);
	wl_list_for_each_reverse_safe(view, prev,
				      &shell->fullscreen_layer.view_list.link,
				      layer_link.link) {
		struct shell_surface *shsurf = get_shell_surface(view->surface);

		if (!shsurf)
			continue;

		/* Only lower surfaces which have lowering_output as their fullscreen
		 * output, unless a NULL output asks for lowering on all outputs.
		 */
		if (lowering_output && (shsurf->fullscreen_output != lowering_output))
			continue;

		/* We can have a non-fullscreen popup for a fullscreen surface
		 * in the fullscreen layer. */
		if (weston_desktop_surface_get_fullscreen(shsurf->desktop_surface)) {
			/* Hide the black view */
			weston_layer_entry_remove(&shsurf->fullscreen.black_view->layer_link);
			wl_list_init(&shsurf->fullscreen.black_view->layer_link.link);
			weston_view_damage_below(shsurf->fullscreen.black_view);

		}

		/* Lower the view to the workspace layer */
		weston_layer_entry_remove(&view->layer_link);
		weston_layer_entry_insert(&ws->layer.view_list, &view->layer_link);
		weston_view_damage_below(view);
		weston_surface_damage(view->surface);

		shsurf->state.lowered = true;
	}
}

static struct shell_surface *get_last_child(struct shell_surface *shsurf)
{
	struct shell_surface *shsurf_child;

	wl_list_for_each_reverse(shsurf_child, &shsurf->children_list, children_link) {
		if (weston_view_is_mapped(shsurf_child->view))
			return shsurf_child;
	}

	return NULL;
}

void
activate(struct desktop_shell *shell, struct weston_view *view,
	 struct weston_seat *seat, uint32_t flags)
{
	struct weston_surface *es = view->surface;
	struct weston_surface *main_surface;
	struct focus_state *state;
	struct shell_surface *shsurf, *shsurf_child;

	main_surface = weston_surface_get_main_surface(es);
	shsurf = get_shell_surface(main_surface);
	assert(shsurf);

	shsurf_child = get_last_child(shsurf);
	if (shsurf_child) {
		/* Activate last xdg child instead of parent. */
		activate(shell, shsurf_child->view, seat, flags);
		return;
	}

	/* Only demote fullscreen surfaces on the output of activated shsurf.
	 * Leave fullscreen surfaces on unrelated outputs alone. */
	if (shsurf->output)
		lower_fullscreen_layer(shell, shsurf->output);

	weston_view_activate(view, seat, flags);

	state = ensure_focus_state(shell, seat);
	if (state == NULL)
		return;

	focus_state_set_focus(state, es);

	if (weston_desktop_surface_get_fullscreen(shsurf->desktop_surface) &&
	    flags & WESTON_ACTIVATE_FLAG_CONFIGURE)
		shell_configure_fullscreen(shsurf);

	/* Update the surface’s layer. This brings it to the top of the stacking
	 * order as appropriate. */
	shell_surface_update_layer(shsurf);

	if (shell->rdprail_api->notify_window_zorder_change)
		shell->rdprail_api->notify_window_zorder_change(shell->compositor);
}

/* no-op func for checking black surface */
static void
black_surface_committed(struct weston_surface *es, int32_t sx, int32_t sy)
{
}

static bool
is_black_surface_view(struct weston_view *view, struct weston_view **fs_view)
{
	struct weston_surface *surface = view->surface;

	if (surface->committed == black_surface_committed) {
		if (fs_view)
			*fs_view = surface->committed_private;
		return true;
	}
	return false;
}

static void
activate_binding(struct weston_seat *seat,
		 struct desktop_shell *shell,
		 struct weston_view *focus_view,
		 uint32_t flags)
{
	struct weston_view *main_view;
	struct weston_surface *main_surface;

	if (!focus_view)
		return;

	if (is_black_surface_view(focus_view, &main_view))
		focus_view = main_view;

	main_surface = weston_surface_get_main_surface(focus_view->surface);
	if (!get_shell_surface(main_surface))
		return;

	activate(shell, focus_view, seat, flags);
}

static void
click_to_activate_binding(struct weston_pointer *pointer,
		          const struct timespec *time,
			  uint32_t button, void *data)
{
	if (pointer->grab != &pointer->default_grab)
		return;
	if (pointer->focus == NULL)
		return;

	activate_binding(pointer->seat, data, pointer->focus,
			 WESTON_ACTIVATE_FLAG_CLICKED |
			 WESTON_ACTIVATE_FLAG_CONFIGURE);
}

static void
touch_to_activate_binding(struct weston_touch *touch,
			  const struct timespec *time,
			  void *data)
{
	if (touch->grab != &touch->default_grab)
		return;
	if (touch->focus == NULL)
		return;

	activate_binding(touch->seat, data, touch->focus,
			 WESTON_ACTIVATE_FLAG_CONFIGURE);
}

static void
shell_backend_request_window_activate(void *shell_context, struct weston_seat *seat, struct weston_surface *surface)
{
	struct desktop_shell *shell = (struct desktop_shell *)shell_context;
	struct weston_view *view;
	struct shell_surface *shsurf;

	if (!surface) {
		/* Here, focus is moving to a window in client side, thus none of Linux app has focus,
		   so move the focus to dummy marker window (focus_proxy), thus the rest of Linux app
		   window can correctly show as 'not focused' state (such as title bar) while client
		   (Windows) application has focus. */
		surface = shell->focus_proxy_surface;
	}
	if (!surface) {
		/* if no proxy window provided, nothing here can do */
		return;
	}

	view = NULL;
	wl_list_for_each(view, &surface->views, surface_link)
		break;
	if (!view)
		return;

	shsurf = get_shell_surface(surface);
	if (!shsurf)
		return;

	activate_binding(seat, shell, view, 
			 WESTON_ACTIVATE_FLAG_CLICKED |
			 WESTON_ACTIVATE_FLAG_CONFIGURE);
}

static void
shell_backend_request_window_close(struct weston_surface *surface)
{
	struct shell_surface *shsurf;
	const struct weston_xwayland_surface_api *api;

	shsurf = get_shell_surface(surface);
	if (!shsurf)
		return;

	api = shsurf->shell->xwayland_surface_api;
	if (!api) {
		api = weston_xwayland_surface_get_api(shsurf->shell->compositor);
		shsurf->shell->xwayland_surface_api = api;
	}
	if (api && api->is_xwayland_surface(surface)) {
		api->close_window(surface);
	} else {
		weston_desktop_surface_close(shsurf->desktop_surface);
	}
}

static void
transform_handler(struct wl_listener *listener, void *data)
{
	struct weston_surface *surface = data;
	struct shell_surface *shsurf = get_shell_surface(surface);
	const struct weston_xwayland_surface_api *api;
	int x, y;

	if (!shsurf)
		return;

	api = shsurf->shell->xwayland_surface_api;
	if (!api) {
		api = weston_xwayland_surface_get_api(shsurf->shell->compositor);
		shsurf->shell->xwayland_surface_api = api;
	}

	if (!api || !api->is_xwayland_surface(surface))
		return;

	if (!weston_view_is_mapped(shsurf->view))
		return;

	x = shsurf->view->geometry.x;
	y = shsurf->view->geometry.y;

	api->send_position(surface, x, y);
}

static void
center_on_output(struct weston_view *view, struct weston_output *output)
{
	int32_t surf_x, surf_y, width, height;
	float x, y;

	if (!output) {
		weston_view_set_position(view, 0, 0);
		return;
	}

	surface_subsurfaces_boundingbox(view->surface, &surf_x, &surf_y, &width, &height);

	x = output->x + (output->width - width) / 2 - surf_x / 2;
	y = output->y + (output->height - height) / 2 - surf_y / 2;

	weston_view_set_position(view, x, y);
}

static void
weston_view_set_initial_position(struct shell_surface *shsurf)
{
	struct weston_view *view = shsurf->view;
	struct desktop_shell *shell = shsurf->shell;
	struct weston_compositor *compositor = shell->compositor;
	int32_t range_x, range_y;
	int32_t x, y;
	struct weston_output *target_output = NULL;
	pixman_rectangle32_t area;
	struct weston_geometry geometry;

	/* As a heuristic place the new window on the same output as the
	 * pointer. Falling back to the output containing 0, 0.
	 *
	 * TODO: Do something clever for touch too?
	 */
	/*
	 * This code does not work well in RDP RAIL mode. Since
	 * pointer position outside of RAIL window in client is
	 * not known to RDP server side.
	 *
	int ix = 0, iy = 0;
	struct weston_seat *seat;
	wl_list_for_each(seat, &compositor->seat_list, link) {
		struct weston_pointer *pointer = weston_seat_get_pointer(seat);

		if (pointer) {
			ix = wl_fixed_to_int(pointer->x);
			iy = wl_fixed_to_int(pointer->y);
			break;
		}
	}

	struct weston_output *output;
	wl_list_for_each(output, &compositor->output_list, link) {
		if (pixman_region32_contains_point(&output->region, ix, iy, NULL)) {
			target_output = output;
			break;
		}
	}
	 */
	/* Because pointer position is not known in RAIL mode, it can end up
	   not finding the output where pointer is, thus use default monitor
	   in that case rather than randomly placed (which can end up outside
	   of work area. And only if no default output found, place randomly */
	if (!target_output && shell->rdprail_api->get_primary_output) {
		target_output = shell->rdprail_api->get_primary_output(shell->rdp_backend);
	}
	if (!target_output) {
		target_output = get_default_output(compositor);
	}

	if (!target_output) {
		weston_view_set_position(view, 10 + random() % 400,
					 10 + random() % 400);
		return;
	}

	/* Valid range within output where the surface will still be onscreen.
	 * If this is negative it means that the surface is bigger than
	 * output.
	 */
	get_output_work_area(shell, target_output, &area);
	geometry = weston_desktop_surface_get_geometry(shsurf->desktop_surface);
	x = area.x - geometry.x;
	y = area.y - geometry.y;
	range_x = area.width - view->surface->width;
	range_y = area.height - view->surface->height;

	if (range_x > 0)
		x += random() % range_x;

	if (range_y > 0)
		y += random() % range_y;

	shell_rdp_debug_verbose(shell, "%s: view:%p, (%d, %d)\n",
				__func__, view, x, y); 

	weston_view_set_position(view, x, y);
}

static bool
check_desktop_shell_crash_too_early(struct desktop_shell *shell)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return false;

	/*
	 * If the shell helper client dies before the session has been
	 * up for roughly 30 seconds, better just make Weston shut down,
	 * because the user likely has no way to interact with the desktop
	 * anyway.
	 */
	if (now.tv_sec - shell->startup_time.tv_sec < 30) {
		shell_rdp_debug(shell, "Error: %s apparently cannot run at all.\n",
			   shell->client);
		shell_rdp_debug(shell, STAMP_SPACE "Quitting...");
		weston_compositor_exit_with_code(shell->compositor,
						 EXIT_FAILURE);

		return true;
	}

	return false;
}

static void launch_desktop_shell_process(void *data);

static void
respawn_desktop_shell_process(struct desktop_shell *shell)
{
	struct timespec time;

	/* if desktop-shell dies more than 5 times in 30 seconds, give up */
	weston_compositor_get_time(&time);
	if (timespec_sub_to_msec(&time, &shell->child.deathstamp) > 30000) {
		shell->child.deathstamp = time;
		shell->child.deathcount = 0;
	}

	shell->child.deathcount++;
	if (shell->child.deathcount > 5) {
		shell_rdp_debug(shell, "%s disconnected, giving up.\n", shell->client);
		return;
	}

	shell_rdp_debug(shell, "%s disconnected, respawning...\n", shell->client);
	launch_desktop_shell_process(shell);
}

static void
desktop_shell_client_destroy(struct wl_listener *listener, void *data)
{
	struct desktop_shell *shell;

	shell = container_of(listener, struct desktop_shell,
			     child.client_destroy_listener);

	wl_list_remove(&shell->child.client_destroy_listener.link);
	shell->child.client = NULL;

	/* client is terminated, so focus_proxy is destroyed too. */
	shell->focus_proxy_surface = NULL;

	/*
	 * unbind_desktop_shell() will reset shell->child.desktop_shell
	 * before the respawned process has a chance to create a new
	 * desktop_shell object, because we are being called from the
	 * wl_client destructor which destroys all wl_resources before
	 * returning.
	 */

	if (!check_desktop_shell_crash_too_early(shell))
		respawn_desktop_shell_process(shell);
}

static void
launch_desktop_shell_process(void *data)
{
	struct desktop_shell *shell = data;

	/* check if it's already running */
	if (shell->child.client)
		return;

	shell->child.client = weston_client_start(shell->compositor,
						  shell->client);

	if (!shell->child.client) {
		shell_rdp_debug(shell, "not able to start %s\n", shell->client);
		return;
	}

	shell->child.client_destroy_listener.notify =
		desktop_shell_client_destroy;
	wl_client_add_destroy_listener(shell->child.client,
				       &shell->child.client_destroy_listener);
}

static void
desktop_shell_set_focus_proxy(struct wl_client *client,
			      struct wl_resource *resource,
			      struct wl_resource *surface_resource)
{
	struct desktop_shell *shell = wl_resource_get_user_data(resource);
	struct weston_surface *surface;
	struct shell_surface *shsurf;

	surface = wl_resource_get_user_data(surface_resource);
	if (!surface) {
		shell_rdp_debug(shell, "%s: surface is NULL\n", __func__);
		return;
	}

	shsurf = get_shell_surface(surface);
	if (!shsurf) {
		shell_rdp_debug(shell, "%s: surface:%p is not shell surface\n", __func__, surface);
		return;
	}

	if (shell->rdprail_api->notify_window_proxy_surface)
		shell->rdprail_api->notify_window_proxy_surface(surface);
	shell->focus_proxy_surface = surface;

	/* Update the surface’s layer. This brings it to the top of the stacking
	 * order as appropriate. */
	shell_surface_update_layer(shsurf);
}

static const struct weston_rdprail_shell_interface rdprail_shell_implementation = {
	desktop_shell_set_focus_proxy,
};

static void
unbind_desktop_shell(struct wl_resource *resource)
{
	struct desktop_shell *shell = wl_resource_get_user_data(resource);

	shell->child.desktop_shell = NULL;
}

static void
bind_desktop_shell(struct wl_client *client,
		   void *data, uint32_t version, uint32_t id)
{
	struct desktop_shell *shell = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client, &weston_rdprail_shell_interface,
				      1, id);

	if (client == shell->child.client) {
		wl_resource_set_implementation(resource,
					       &rdprail_shell_implementation,
					       shell, unbind_desktop_shell);
		shell->child.desktop_shell = resource;
		return;
	}

	wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
			       "permission to bind rdprail_shell denied");
}

static void
force_kill_binding(struct weston_keyboard *keyboard,
		   const struct timespec *time, uint32_t key, void *data)
{
	struct weston_surface *focus_surface;
	struct wl_client *client;
	struct desktop_shell *shell = data;
	struct weston_compositor *compositor = shell->compositor;
	pid_t pid;

	focus_surface = keyboard->focus;
	if (!focus_surface)
		return;

	wl_signal_emit(&compositor->kill_signal, focus_surface);

	client = wl_resource_get_client(focus_surface->resource);
	wl_client_get_credentials(client, &pid, NULL, NULL);

	/* Skip clients that we launched ourselves (the credentials of
	 * the socketpair is ours) */
	if (pid == getpid())
		return;

	kill(pid, SIGKILL);
}

static void
shell_reposition_view_on_output_change(struct weston_view *view)
{
	struct weston_output *output, *first_output;
	struct weston_compositor *ec = view->surface->compositor;
	struct shell_surface *shsurf;
	float x, y;
	int visible;

	if (wl_list_empty(&ec->output_list))
		return;

	x = view->geometry.x;
	y = view->geometry.y;

	/* At this point the destroyed output is not in the list anymore.
	 * If the view is still visible somewhere, we leave where it is,
	 * otherwise, move it to the first output. */
	visible = 0;
	wl_list_for_each(output, &ec->output_list, link) {
		if (pixman_region32_contains_point(&output->region,
						   x, y, NULL)) {
			visible = 1;
			break;
		}
	}

	if (!visible) {
		first_output = container_of(ec->output_list.next,
					    struct weston_output, link);

		x = first_output->x + first_output->width / 4;
		y = first_output->y + first_output->height / 4;

		weston_view_set_position(view, x, y);
	} else {
		weston_view_geometry_dirty(view);
	}


	shsurf = get_shell_surface(view->surface);
	if (!shsurf)
		return;

	shsurf->saved_position_valid = false;
	//this sets window size to 0x0 when output is removed. 
	//set_maximized(shsurf, false);
	//set_fullscreen(shsurf, false, NULL);
}

void
shell_for_each_layer(struct desktop_shell *shell,
		     shell_for_each_layer_func_t func, void *data)
{
	struct workspace **ws;

	func(shell, &shell->fullscreen_layer, data);

	wl_array_for_each(ws, &shell->workspaces.array)
		func(shell, &(*ws)->layer, data);
}

static void
shell_output_changed_move_layer(struct desktop_shell *shell,
				struct weston_layer *layer,
				void *data)
{
	struct weston_view *view;

	wl_list_for_each(view, &layer->view_list.link, layer_link.link)
		shell_reposition_view_on_output_change(view);

}

static void
handle_output_destroy(struct wl_listener *listener, void *data)
{
	struct shell_output *output_listener =
		container_of(listener, struct shell_output, destroy_listener);
	struct desktop_shell *shell = output_listener->shell;

	shell_for_each_layer(shell, shell_output_changed_move_layer, NULL);

	wl_list_remove(&output_listener->destroy_listener.link);
	wl_list_remove(&output_listener->link);
	free(output_listener);
}

static void
create_shell_output(struct desktop_shell *shell,
					struct weston_output *output)
{
	struct shell_output *shell_output;

	shell_output = zalloc(sizeof *shell_output);
	if (shell_output == NULL)
		return;

	shell_output->output = output;
	shell_output->shell = shell;
	shell_output->destroy_listener.notify = handle_output_destroy;
	wl_signal_add(&output->destroy_signal,
		      &shell_output->destroy_listener);
	wl_list_insert(shell->output_list.prev, &shell_output->link);

	if (wl_list_length(&shell->output_list) == 1)
		shell_for_each_layer(shell,
				     shell_output_changed_move_layer, NULL);

	shell_output->desktop_workarea.x = output->x;
	shell_output->desktop_workarea.y = output->y;
	shell_output->desktop_workarea.width = output->width;
	shell_output->desktop_workarea.height = output->height;
}

static void
handle_output_create(struct wl_listener *listener, void *data)
{
	struct desktop_shell *shell =
		container_of(listener, struct desktop_shell, output_create_listener);
	struct weston_output *output = (struct weston_output *)data;

	create_shell_output(shell, output);
}

static void
handle_output_move_layer(struct desktop_shell *shell,
			 struct weston_layer *layer, void *data)
{
	struct weston_output *output = data;
	struct weston_view *view;
	float x, y;

	wl_list_for_each(view, &layer->view_list.link, layer_link.link) {
		if (view->output != output)
			continue;

		x = view->geometry.x + output->move_x;
		y = view->geometry.y + output->move_y;
		weston_view_set_position(view, x, y);
	}
}

static void
handle_output_move(struct wl_listener *listener, void *data)
{
	struct desktop_shell *shell;

	shell = container_of(listener, struct desktop_shell,
			     output_move_listener);

	shell_for_each_layer(shell, handle_output_move_layer, data);
}

static void
setup_output_destroy_handler(struct weston_compositor *ec,
							struct desktop_shell *shell)
{
	struct weston_output *output;

	wl_list_init(&shell->output_list);
	wl_list_for_each(output, &ec->output_list, link)
		create_shell_output(shell, output);

	shell->output_create_listener.notify = handle_output_create;
	wl_signal_add(&ec->output_created_signal,
				&shell->output_create_listener);

	shell->output_move_listener.notify = handle_output_move;
	wl_signal_add(&ec->output_moved_signal, &shell->output_move_listener);
}

static void
shell_destroy(struct wl_listener *listener, void *data)
{
	struct desktop_shell *shell =
		container_of(listener, struct desktop_shell, destroy_listener);
	struct workspace **ws;
	struct shell_output *shell_output, *tmp;

	wl_list_remove(&shell->destroy_listener.link);
	wl_list_remove(&shell->transform_listener.link);

	app_list_destroy(shell);
	text_backend_destroy(shell->text_backend);
	input_panel_destroy(shell);

	wl_list_for_each_safe(shell_output, tmp, &shell->output_list, link) {
		wl_list_remove(&shell_output->destroy_listener.link);
		wl_list_remove(&shell_output->link);
		free(shell_output);
	}

	wl_list_remove(&shell->output_create_listener.link);
	wl_list_remove(&shell->output_move_listener.link);

	wl_array_for_each(ws, &shell->workspaces.array)
		workspace_destroy(*ws);
	wl_array_release(&shell->workspaces.array);

	if (shell->image_default_app_icon)
		pixman_image_unref(shell->image_default_app_icon);

	if (shell->image_default_app_overlay_icon)
		pixman_image_unref(shell->image_default_app_overlay_icon);

	if (shell->debug)
		weston_log_scope_destroy(shell->debug);

	free(shell->client);
	free(shell);
}

static void
shell_add_bindings(struct weston_compositor *ec, struct desktop_shell *shell)
{
	uint32_t mod;

	if (shell->allow_zap)
		weston_compositor_add_key_binding(ec, KEY_BACKSPACE,
					          MODIFIER_CTRL | MODIFIER_ALT,
					          terminate_binding, ec);

	if (shell->allow_alt_f4_to_close_app)
		weston_compositor_add_key_binding(ec, KEY_F4,
					          MODIFIER_ALT,
					          close_focused_app_binding, ec);

	/* fixed bindings */
	weston_compositor_add_button_binding(ec, BTN_LEFT, 0,
					     click_to_activate_binding,
					     shell);
	weston_compositor_add_button_binding(ec, BTN_RIGHT, 0,
					     click_to_activate_binding,
					     shell);
	weston_compositor_add_touch_binding(ec, 0,
					    touch_to_activate_binding,
					    shell);

	mod = shell->binding_modifier;
	if (!mod)
		return;

	weston_compositor_add_axis_binding(ec, WL_POINTER_AXIS_VERTICAL_SCROLL,
				           mod | MODIFIER_ALT,
				           surface_opacity_binding, NULL);

	weston_compositor_add_key_binding(ec, KEY_M, mod | MODIFIER_SHIFT,
					  maximize_binding, NULL);
	weston_compositor_add_key_binding(ec, KEY_F, mod | MODIFIER_SHIFT,
					  fullscreen_binding, NULL);
	weston_compositor_add_button_binding(ec, BTN_LEFT, mod, move_binding,
					     shell);
	weston_compositor_add_touch_binding(ec, mod, touch_move_binding, shell);
	weston_compositor_add_button_binding(ec, BTN_RIGHT, mod,
					     resize_binding, shell);
	weston_compositor_add_button_binding(ec, BTN_LEFT,
					     mod | MODIFIER_SHIFT,
					     resize_binding, shell);

	//TODO: while RAIL can't do arbirary rotation, but can do 0,90,180,270 degree rotation
	//      maybe it can have a new cap for that ?
	//if (ec->capabilities & WESTON_CAP_ROTATION_ANY)
	//	weston_compositor_add_button_binding(ec, BTN_MIDDLE, mod,
	//					     rotate_binding, NULL);

	weston_compositor_add_key_binding(ec, KEY_K, mod,
					  force_kill_binding, shell);

	weston_install_debug_key_binding(ec, mod);
}

static void
handle_seat_created(struct wl_listener *listener, void *data)
{
	struct weston_seat *seat = data;
	struct desktop_shell *shell;

	shell = container_of(listener, struct desktop_shell,
			     seat_create_listener);

	create_shell_seat(shell, seat);
}

struct shell_workarea_change {
	struct weston_output *output;
	pixman_rectangle32_t old_workarea;
	pixman_rectangle32_t new_workarea;
};

static void
shell_reposition_view_on_workarea_change(struct weston_view *view, void *data)
{
	struct shell_surface *shsurf;
	struct shell_workarea_change *workarea_change = (struct shell_workarea_change *)data;
	struct weston_surface_rail_state *rail_state =
		(struct weston_surface_rail_state *)view->surface->backend_state;

	weston_view_geometry_dirty(view);

	shsurf = get_shell_surface(view->surface);
	if (!shsurf)
		return;

	if (view->output != workarea_change->output)
		return;

	shsurf->saved_position_valid = false;
	if (shsurf->state.maximized) {
		set_maximized(shsurf, true);
	} else if (shsurf->state.fullscreen) {
		set_fullscreen(shsurf, true, NULL);
	} else {
		bool posDirty = false;

		/* Force update window state at next window update.
		   When workarea changed, there is the case Windows client moves the RAIL window
		   but if it does, the server has no way to tell where it goes, thus here forcing
		   backend to resend window state to client, this force window state, especially
		   window position keeps in sync between server and client. */
		if (rail_state)
			rail_state->forceUpdateWindowState = true;

		/* If view's upper-left is within 10% of bottom-right of workarea boundary, adjust the position. */
		int new_workarea_width = (int)workarea_change->new_workarea.width;
		int x = (int)view->geometry.x - view->output->x;
		if (x + (new_workarea_width / 10) > new_workarea_width) {
			x += (new_workarea_width - workarea_change->old_workarea.width); 
			if (x < 0)
				x = 0;
			else if (x > workarea_change->new_workarea.x + new_workarea_width)
				x = workarea_change->new_workarea.x + new_workarea_width / 2;
			posDirty = true;
		}

		int new_workarea_height = (int)workarea_change->new_workarea.height;
		int y = (int)view->geometry.y - view->output->y;
		if (y + (new_workarea_height / 10) > new_workarea_height) {
			y += (new_workarea_height - workarea_change->old_workarea.height); 
			if (y < 0)
				y = 0;
			else if (y > workarea_change->new_workarea.y + new_workarea_height)
				y = workarea_change->new_workarea.y + new_workarea_height / 2;
			posDirty = true;
		}

		if (posDirty) {
			shell_rdp_debug(shsurf->shell, "shell_reposition_view_on_workarea_change(): view %p, (%d,%d) -> (%d,%d)\n",
				view, (int)view->geometry.x, (int)view->geometry.y, view->output->x + x, view->output->y + y);

			weston_view_set_position(view, view->output->x + x, view->output->y + y);
		}
	}
}

static void
shell_workarea_changed_layer(struct desktop_shell *shell,
				struct weston_layer *layer,
				void *data)
{
	struct weston_view *view;

	wl_list_for_each(view, &layer->view_list.link, layer_link.link)
		shell_reposition_view_on_workarea_change(view, data);
}

static void
shell_backend_set_desktop_workarea(struct weston_output *output, void *context, pixman_rectangle32_t *workarea)
{
	struct desktop_shell *shell = (struct desktop_shell *)context;
	struct shell_output *shell_output =
		find_shell_output_from_weston_output(shell, output);
	if (shell_output) {
		struct shell_workarea_change workarea_change;
		workarea_change.output = output;
		workarea_change.old_workarea = shell_output->desktop_workarea; 
		workarea_change.new_workarea = *workarea;

		shell_output->desktop_workarea = *workarea;
		shell_for_each_layer(shell, shell_workarea_changed_layer, (void*)&workarea_change);
	}
}

static pid_t
shell_backend_get_app_id(void *shell_context, struct weston_surface *surface, char *app_id, size_t app_id_size, char *image_name, size_t image_name_size)
{
	struct desktop_shell *shell = (struct desktop_shell *)shell_context;
	struct weston_desktop_surface *desktop_surface;
	struct weston_surface_rail_state *rail_state; 
	struct shell_surface *shsurf;
	const struct weston_xwayland_surface_api *api; 
	pid_t pid;
	const char *id;
	char *class_name;
	bool is_wayland = true;

	assert(shell);
	assert(app_id);
	assert(app_id_size);
	assert(image_name);
	assert(image_name_size);

	app_id[0] = '\0';
	image_name[0] = '\0';

	desktop_surface = weston_surface_get_desktop_surface(surface);
	if (!desktop_surface)
		return -1;

	shsurf = weston_desktop_surface_get_user_data(desktop_surface);
	if (!shsurf) 
		return -1;

	rail_state = (struct weston_surface_rail_state *)surface->backend_state;
	if (!rail_state)
		return -1;

	/* first obtain class name from X server for X app, and use as app_id */
	api = shsurf->shell->xwayland_surface_api;
	if (!api) {
		api = weston_xwayland_surface_get_api(shsurf->shell->compositor);
		shsurf->shell->xwayland_surface_api = api;
	}
	if (api && api->is_xwayland_surface(surface)) {
		class_name = api->get_class_name(surface);
		if (class_name) {
			strncpy(app_id, class_name, app_id_size);
			free(class_name);
			/* app_id is from Xwayland */
			is_wayland = false;
		}
	}
	/* if not, obtain application id specified via wayland interface */
	if (app_id[0] == '\0') {
		id = weston_desktop_surface_get_app_id(desktop_surface);
		if (id) 
			strncpy(app_id, id, app_id_size);
	}

	/* obtain pid for execuable path */
	pid = weston_desktop_surface_get_pid(desktop_surface);
	/* find image name via user-distro for Xwayland */
	if (pid > 0)
		app_list_find_image_name(shell, pid, image_name, image_name_size, is_wayland);

	/* if app_id is not obtained but image name, use image name (only name) as app_id. */
	/* NOTE: image name is Windows's style path, so separator is '\\', not '/'. */
	if (app_id[0] == '\0' && image_name[0] != '\0') {
		char *p = strrchr(image_name, '\\');
		if (p && p[1] != '\0')
			p++;
		else
			p = image_name;
		strncpy(app_id, p, app_id_size);
	} else if (app_id[0] != '\0' && image_name[0] == '\0') {
		strncpy(image_name, app_id, image_name_size);
	}

	shell_rdp_debug_verbose(shell, "shell_backend_get_app_id: 0x%p: pid:%d, app_id:%s, windowId:0x%x, image_name:%s\n",
		surface, pid, app_id, rail_state->window_id, image_name);

	/* obtain window icon for app */
	if (!shsurf->icon.is_icon_set) {
		/* TODO hook to meta data change notification */
		shell_surface_set_window_icon(desktop_surface, 0, 0, 0, NULL, NULL);
		shsurf->icon.is_icon_set = true;
	}

	/* associate window and app_id at client side */
	if (!shsurf->app_id.is_window_app_id_associated && app_id[0] != '\0') {
		app_list_associate_window_app_id(shsurf->shell, pid, app_id, rail_state->window_id);
		shsurf->app_id.is_window_app_id_associated = true;
	}

	return pid;
}

static bool
shell_backend_start_app_list_update(void *shell_context, char *clientLanguageId)
{
	struct desktop_shell *shell = (struct desktop_shell *)shell_context;
	return app_list_start_backend_update(shell, clientLanguageId);
}

static void
shell_backend_stop_app_list_update(void *shell_context)
{
	struct desktop_shell *shell = (struct desktop_shell *)shell_context;
	app_list_stop_backend_update(shell);
}

static void
shell_backend_request_window_icon(struct weston_surface *surface)
{
	struct shell_surface *shsurf = get_shell_surface(surface);

	if (!shsurf)
		return;

	/* reset icon state and send to client at next surface commit */
	shsurf->icon.is_icon_set = false;
	shsurf->icon.is_default_icon_used = false;
}

static struct wl_client *
shell_backend_launch_shell_process(void *shell_context, char *exec_name)
{
	struct desktop_shell *shell = (struct desktop_shell *)shell_context;
	return weston_client_start(shell->compositor, exec_name);
}

static void
shell_backend_get_window_geometry(struct weston_surface *surface, struct weston_geometry *geometry)
{
	struct weston_desktop_surface *desktop_surface =
		weston_surface_get_desktop_surface(surface);
	if (desktop_surface) {
		*geometry = weston_desktop_surface_get_geometry(desktop_surface);
		/* clamp geometry to surface size */
		if (geometry->x < 0)
			geometry->x = 0;
		if (geometry->y < 0)
			geometry->y = 0;
		if (geometry->width == 0)
			geometry->width = surface->width;
		else if (geometry->width > (geometry->x + surface->width))
			geometry->width = (geometry->x + surface->width);
		if (geometry->height == 0)
			geometry->height = surface->height;
		else if (geometry->height > (geometry->y + surface->height))
			geometry->height = (geometry->y + surface->height);
	} else {
		geometry->x = 0;
		geometry->y = 0;
		geometry->width = surface->width;
		geometry->height = surface->height;
	}
}

static const struct weston_rdprail_shell_api rdprail_shell_api = {
	.request_window_restore = shell_backend_request_window_restore,
	.request_window_minimize = shell_backend_request_window_minimize,
	.request_window_maximize = shell_backend_request_window_maximize,
	.request_window_move = shell_backend_request_window_move,
	.request_window_snap = shell_backend_request_window_snap,
	.request_window_activate = shell_backend_request_window_activate,
	.request_window_close = shell_backend_request_window_close,
	.set_desktop_workarea = shell_backend_set_desktop_workarea,
	.get_window_app_id = shell_backend_get_app_id,
	.start_app_list_update = shell_backend_start_app_list_update,
	.stop_app_list_update = shell_backend_stop_app_list_update,
	.request_window_icon = shell_backend_request_window_icon,
	.request_launch_shell_process = shell_backend_launch_shell_process,
	.get_window_geometry = shell_backend_get_window_geometry,
	.request_window_minmax_info = shell_send_minmax_info,
};

WL_EXPORT int
wet_shell_init(struct weston_compositor *ec,
	       int *argc, char *argv[])
{
	struct weston_seat *seat;
	struct desktop_shell *shell;
	struct workspace **pws;
	unsigned int i;
	char *debug_level;

	shell = zalloc(sizeof *shell);
	if (shell == NULL)
		return -1;

	shell->compositor = ec;

	if (!weston_compositor_add_destroy_listener_once(ec,
							 &shell->destroy_listener,
							 shell_destroy)) {
		free(shell);
		return 0;
	}

	shell->debug = weston_log_ctx_add_log_scope(ec->weston_log_ctx,
							"rdprail-shell",
							"Debug messages from RDP-RAIL shell\n",
							 NULL, NULL, NULL);
	if (shell->debug) {
		debug_level = getenv("WESTON_RDPRAIL_SHELL_DEBUG_LEVEL");
		if (debug_level) {
			shell->debugLevel = atoi(debug_level);
			if (shell->debugLevel > RDPRAIL_SHELL_DEBUG_LEVEL_VERBOSE)
				shell->debugLevel = RDPRAIL_SHELL_DEBUG_LEVEL_VERBOSE;
		} else {
			shell->debugLevel = RDPRAIL_SHELL_DEBUG_LEVEL_DEFAULT;
		}
	}
	weston_log("RDPRAIL-shell: WESTON_RDPRAIL_SHELL_DEBUG_LEVEL: %d.\n", shell->debugLevel);

	/* this make sure rdprail-shell to be used with only backend-rdp */
	shell->rdprail_api = weston_rdprail_get_api(ec);
	if (!shell->rdprail_api) {
		shell_rdp_debug_error(shell, "Failed to obrain rdprail API.\n");
		return -1;
	}

	shell_configuration(shell);

	shell->transform_listener.notify = transform_handler;
	wl_signal_add(&ec->transform_signal, &shell->transform_listener);

	weston_layer_init(&shell->fullscreen_layer, ec);

	weston_layer_set_position(&shell->fullscreen_layer,
				  WESTON_LAYER_POSITION_FULLSCREEN);

	wl_array_init(&shell->workspaces.array);
	wl_list_init(&shell->workspaces.client_list);

	if (input_panel_setup(shell) < 0)
		return -1;

	shell->text_backend = text_backend_init(ec);
	if (!shell->text_backend)
		return -1;

	for (i = 0; i < shell->workspaces.num; i++) {
		pws = wl_array_add(&shell->workspaces.array, sizeof *pws);
		if (pws == NULL)
			return -1;

		*pws = workspace_create(shell);
		if (*pws == NULL)
			return -1;
	}
	activate_workspace(shell, 0);

	weston_layer_init(&shell->minimized_layer, ec);

	shell->desktop = weston_desktop_create(ec, &shell_desktop_api, shell);
	if (!shell->desktop)
		return -1;

	if (wl_global_create(ec->wl_display,
			     &weston_rdprail_shell_interface, 1,
			     shell, bind_desktop_shell) == NULL)
		return -1;

	setup_output_destroy_handler(ec, shell);

	shell->child.client = NULL;

	wl_list_for_each(seat, &ec->seat_list, link)
		handle_seat_created(NULL, seat);
	shell->seat_create_listener.notify = handle_seat_created;
	wl_signal_add(&ec->seat_created_signal, &shell->seat_create_listener);

	screenshooter_create(ec);

	shell_add_bindings(ec, shell);

	clock_gettime(CLOCK_MONOTONIC, &shell->startup_time);

	if (shell->rdprail_api->shell_initialize_notify)
		shell->rdp_backend = shell->rdprail_api->shell_initialize_notify(ec, &rdprail_shell_api, (void*)shell, shell->distroName);

	app_list_init(shell);

	return 0;
}
