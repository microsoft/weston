/*
 * Copyright © 2016 Benoit Gschwind
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef WESTON_COMPOSITOR_RDP_H
#define WESTON_COMPOSITOR_RDP_H

#ifdef  __cplusplus
extern "C" {
#endif

#include <libweston/libweston.h>
#include <libweston/plugin-registry.h>

#include <freerdp/freerdp.h>

#define WESTON_RDP_MODE_FREQ 60 // Hz

#define WESTON_RDP_OUTPUT_API_NAME "weston_rdp_output_api_v1"

struct weston_rdp_output_api {
	/** Get config from RDP client when connected
	 */
	rdpMonitor *(*head_get_rdpmonitor)(const struct weston_head *head);

	/** Set mode for an output */
	void (*output_set_mode)(struct weston_output *base, struct weston_mode *mode);

	/** Get primary output */
	struct weston_output *(*output_get_primary)(struct weston_compositor *compositor);
};

static inline const struct weston_rdp_output_api *
weston_rdp_output_get_api(struct weston_compositor *compositor)
{
	const void *api;
	api = weston_plugin_api_get(compositor, WESTON_RDP_OUTPUT_API_NAME,
				    sizeof(struct weston_rdp_output_api));

	return (const struct weston_rdp_output_api *)api;
}

/* RDPRAIL api extension */

struct weston_rdprail_shell_api {
	/** Restore a window to original position.
	 */
	void (*request_window_restore)(struct weston_surface *surface);

	/** Minimize a window.
	 */
	void (*request_window_minimize)(struct weston_surface *surface);

	/** Maximize a window.
	 */
	void (*request_window_maximize)(struct weston_surface *surface);

	/** Move a window.
	 */
	void (*request_window_move)(struct weston_surface *surface, int x, int y, int width, int height);

	/** Snap a window.
	 */
	void (*request_window_snap)(struct weston_surface *surface, int x, int y, int width, int height);

	/** Activate a window.
	 */
	void (*request_window_activate)(void *shell_context, struct weston_seat *seat, struct weston_surface *surface);

	/** Close a window.
	 */
	void (*request_window_close)(struct weston_surface *surface);

	/** Set desktop work area of specified output.
	 */
	void (*set_desktop_workarea)(struct weston_output *output, void *context, pixman_rectangle32_t *workarea);

	/** Get app_id and pid
	  */
	pid_t (*get_window_app_id)(void *shell_context, struct weston_surface *surface,
				char *app_id, size_t app_id_size, char *image_name, size_t image_name_size);

	/** Start/stop application list update
	  */
	bool (*start_app_list_update)(void *shell_context, char* clientLanguageId);
	void (*stop_app_list_update)(void *shell_context);

	/** Request shell to send window icon
	  */
	void (*request_window_icon)(struct weston_surface *surface);

	/** Request launch shell process
	  */
	struct wl_client* (*request_launch_shell_process)(void *shell_context, char *exec_name);

	/** Query window geometry
	  */
	void (*get_window_geometry)(struct weston_surface *surface, struct weston_geometry *geometry);

	/** Request to send window minmax info
	  */
	void (*request_window_minmax_info)(struct weston_surface *surface);
};

#define WESTON_RDPRAIL_API_NAME "weston_rdprail_api_v1"

struct weston_rdprail_app_list_data {
	bool inSync;
	bool syncStart;
	bool syncEnd;
	bool newAppId;
	bool deleteAppId;
	bool deleteAppProvider;
	bool associateWindowId;
	char *appId;
	char *appGroup;
	char *appExecPath;
	char *appWorkingDir;
	char *appDesc;
	char *appProvider;
	pixman_image_t *appIcon;
	uint32_t appWindowId;
};

struct weston_rdp_rail_window_pos {
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
};

struct weston_rdprail_api {
	/** Initialize
	 */
	void *(*shell_initialize_notify)(struct weston_compositor *compositor,
					const struct weston_rdprail_shell_api *rdprail_shell_api,
					void *context, char *name);

	/** Start a local window move operation
	 */
	void (*start_window_move)(struct weston_surface *surface, 
		int pointerGrabX, int pointerGrabY);

	/** End local window move operation
	 */
	void (*end_window_move)(struct weston_surface *surface);

	/** Send window min/max information.
	 */
	void (*send_window_minmax_info)(struct weston_surface* surface,
		struct weston_rdp_rail_window_pos* maxPosSize,
		struct weston_size* minTrackSize,
		struct weston_size* maxTrackSize);

	/** Set window icon
	 */
	void (*set_window_icon)(struct weston_surface *surface,
		pixman_image_t *icon);

	/** Report application list
	 */
	bool (*notify_app_list)(void *rdp_backend,
		struct weston_rdprail_app_list_data *app_list_data);

	/** Get primary output
	 */
	struct weston_output *(*get_primary_output)(void *rdp_backend);

	/** Update window zorder
	 */
	void (*notify_window_zorder_change)(struct weston_compositor *compositor);

	/** Notify window proxy surface
	 */
	void (*notify_window_proxy_surface)(struct weston_surface *proxy_surface);
};

static inline const struct weston_rdprail_api *
weston_rdprail_get_api(struct weston_compositor *compositor)
{
	const void *api;
	api = weston_plugin_api_get(compositor, WESTON_RDPRAIL_API_NAME,
					sizeof(struct weston_rdprail_api));

	return (const struct weston_rdprail_api *)api;
}

#define RDP_SHARED_MEMORY_NAME_SIZE (32 + 4 + 2)

struct weston_rdp_shared_memory {
	int fd;
	void *addr;
	size_t size;
	char name[RDP_SHARED_MEMORY_NAME_SIZE + 1]; // +1 for NULL
};

/* weston_surface_rail_state.showState_requested */
#define RDP_WINDOW_HIDE 0x00
#define RDP_WINDOW_SHOW_MINIMIZED 0x02
#define RDP_WINDOW_SHOW_MAXIMIZED 0x03
#define RDP_WINDOW_SHOW_FULLSCREEN 0x04
#define RDP_WINDOW_SHOW 0x05

struct weston_surface_rail_state {
	struct wl_listener destroy_listener;
	struct wl_listener repaint_listener;
	uint32_t window_id;
	struct weston_rdp_rail_window_pos pos;
	struct weston_rdp_rail_window_pos clientPos;
	int bufferWidth;
	int bufferHeight;
	float bufferScaleFactorWidth;
	float bufferScaleFactorHeight;
	pixman_region32_t damage;
	struct weston_output *output;
	int32_t output_scale;
	struct weston_surface *parent_surface;
	uint32_t parent_window_id;
	bool isCursor;
	bool isWindowCreated;
	bool isWindowSnapped;
	uint32_t showState_requested;
	uint32_t showState;
	bool forceRecreateSurface;
	bool forceUpdateWindowState;
	bool error;
	bool isUpdatePending;
	bool isFirstUpdateDone;
	void *get_label;
	int taskbarButton;

	uint32_t window_margin_top;
	uint32_t window_margin_left;
	uint32_t window_margin_right;
	uint32_t window_margin_bottom;

	/* gfxredir shared memory */
	uint32_t pool_id;
	uint32_t buffer_id;
	void *surfaceBuffer;
	struct weston_rdp_shared_memory shared_memory;

	/* rdpgfx surface */
	uint32_t surface_id;
};

#define WESTON_RDP_BACKEND_CONFIG_VERSION 3

typedef void *(*rdp_audio_in_setup)(struct weston_compositor *c, void *vcm);
typedef void (*rdp_audio_in_teardown)(void *audio_private);
typedef void *(*rdp_audio_out_setup)(struct weston_compositor *c, void *vcm);
typedef void (*rdp_audio_out_teardown)(void *audio_private);

struct weston_rdp_backend_config {
	struct weston_backend_config base;
	char *bind_address;
	int port;
	char *rdp_key;
	char *server_cert;
	char *server_key;
	int env_socket;
	int no_clients_resize;
	int force_no_compression;
	bool redirect_clipboard;
	rdp_audio_in_setup audio_in_setup;
	rdp_audio_in_teardown audio_in_teardown;
	rdp_audio_out_setup audio_out_setup;
	rdp_audio_out_teardown audio_out_teardown;
	int rdp_monitor_refresh_rate;
	struct {
		bool use_rdpapplist;
		bool use_shared_memory;
		bool enable_hi_dpi_support;
		bool enable_fractional_hi_dpi_support;
		bool enable_fractional_hi_dpi_roundup;
		int debug_desktop_scaling_factor;
		bool enable_window_zorder_sync;
		bool enable_window_snap_arrange;
		bool enable_window_shadow_remoting;
		bool enable_distro_name_title;
		bool enable_copy_warning_title;
		bool enable_display_power_by_screenupdate;
	} rail_config;
};

#ifdef  __cplusplus
}
#endif

#endif /* WESTON_COMPOSITOR_RDP_H */
