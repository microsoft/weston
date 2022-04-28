/*
 * Copyright © 2020 Microsoft
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

#ifndef RDP_H
#define RDP_H

#include <freerdp/version.h>

#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/update.h>
#include <freerdp/input.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/rfx.h>
#include <freerdp/codec/nsc.h>
#include <freerdp/locale/keyboard.h>
#include <freerdp/server/rail.h>
#include <freerdp/server/drdynvc.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/server/disp.h>
#include <freerdp/server/rdpsnd.h>
#include <freerdp/server/audin.h>
#include <freerdp/server/cliprdr.h>
#ifdef HAVE_FREERDP_GFXREDIR_H
#include <freerdp/server/gfxredir.h>
#endif // HAVE_FREERDP_GFXREDIR_H
#ifdef HAVE_FREERDP_RDPAPPLIST_H
#include <rdpapplist/rdpapplist_config.h>
#include <rdpapplist/rdpapplist_protocol.h>
#include <rdpapplist/rdpapplist_server.h>
#endif // HAVE_FREERDP_RDPAPPLIST_H

#include <libweston/libweston.h>
#include <libweston/backend-rdp.h>
#include <libweston/weston-log.h>

#include "hash.h"
#include "backend.h"

#include "shared/helpers.h"
#include "shared/string-helpers.h"
#include "shared/timespec-util.h"

#define MAX_FREERDP_FDS 32
#define RDP_MAX_MONITOR 16 // RDP max monitors.

#define DEFAULT_PIXEL_FORMAT PIXEL_FORMAT_BGRA32

struct rdp_output;
struct rdp_clipboard_data_source;
struct rdp_backend;

struct rdp_id_manager {
	struct rdp_backend *rdp_backend;
	UINT32 id;
	UINT32 id_low_limit;
	UINT32 id_high_limit;
	UINT32 id_total;
	UINT32 id_used;
	pthread_mutex_t mutex;
	pid_t mutex_tid;
	struct hash_table *hash_table;
};

struct rdp_backend {
	struct weston_backend base;
	struct weston_compositor *compositor;

	freerdp_listener *listener;
	struct wl_event_source *listener_events[MAX_FREERDP_FDS];
	struct rdp_output *output_default; // default output created at backend initialize
	struct rdp_head *head_default; // default head created at backend initialize
	struct wl_list output_list; // rdp_output::link
	struct wl_list head_list; // rdp_head::link
	struct wl_list head_pending_list; // used during monitor layout change.
	struct wl_list head_move_pending_list; // used during monitor layout change.
	uint32_t head_index;
	struct weston_log_scope *debug;
	uint32_t debugLevel;
	struct weston_log_scope *debugClipboard;
	uint32_t debugClipboardLevel;

	char *server_cert;
	char *server_key;
	char *server_cert_content;
	char *server_key_content;
	char *rdp_key;
	int no_clients_resize;
	int force_no_compression;
	bool redirect_clipboard;
	bool redirect_audio_playback;
	bool redirect_audio_capture;

	const struct weston_rdprail_shell_api *rdprail_shell_api;
	void *rdprail_shell_context;
	char *rdprail_shell_name;
	bool enable_copy_warning_title;
	bool enable_distro_name_title;

	freerdp_peer *rdp_peer; // this points a single instance of RAIL RDP peer.
	pid_t compositor_tid;

	struct weston_binding *debug_binding_M;
	struct weston_binding *debug_binding_W;

	struct wl_listener create_window_listener;

	bool enable_window_zorder_sync;
	bool enable_window_snap_arrange;

	bool enable_display_power_by_screenupdate;

	bool enable_hi_dpi_support;
	bool enable_fractional_hi_dpi_support;
	bool enable_fractional_hi_dpi_roundup;
	uint32_t debug_desktop_scaling_factor; /* must be between 100 to 500 */

	int rdp_monitor_refresh_rate;

#ifdef HAVE_FREERDP_RDPAPPLIST_H
	/* import from libfreerdp-server2.so */
	RdpAppListServerContext* (*rdpapplist_server_context_new)(HANDLE vcm);
	void (*rdpapplist_server_context_free)(RdpAppListServerContext* context);

	void *libRDPApplistServer;
	bool use_rdpapplist;
#endif // HAVE_FREERDP_RDPAPPLIST_H

#ifdef HAVE_FREERDP_GFXREDIR_H
	/* import from libfreerdp-server2.so */
	GfxRedirServerContext* (*gfxredir_server_context_new)(HANDLE vcm);
	void (*gfxredir_server_context_free)(GfxRedirServerContext* context);

	void *libFreeRDPServer;
	bool use_gfxredir;
	char *shared_memory_mount_path;
	size_t shared_memory_mount_path_size;
#endif // HAVE_FREERDP_GFXREDIR_H
};

enum peer_item_flags {
	RDP_PEER_ACTIVATED      = (1 << 0),
	RDP_PEER_OUTPUT_ENABLED = (1 << 1),
};

struct rdp_peers_item {
	int flags;
	freerdp_peer *peer;
	struct weston_seat *seat;

	struct wl_list link; // rdp_output::peers
};

struct rdp_monitor_mode {
	rdpMonitor monitorDef; // in client coordinate.
	int scale; // per monitor DPI scaling.
	float clientScale;
	pixman_rectangle32_t rectWeston; // in weston coordinate.
};

struct rdp_head {
	struct weston_head base;
	uint32_t index;
	struct rdp_monitor_mode monitorMode;
	/*TODO: these region/rectangles can be moved to rdp_output */
	pixman_region32_t regionClient; // in client coordnate.
	pixman_region32_t regionWeston; // in weston coordnate.
	pixman_rectangle32_t workareaClient; // in client coordinate.
	pixman_rectangle32_t workarea; // in weston coordinate.

	struct wl_list link; // rdp_backend::head_list
};

struct rdp_output {
	struct weston_output base;
	struct wl_event_source *finish_frame_timer;
	pixman_image_t *shadow_surface;
	uint32_t index;

	struct wl_list peers;
	struct wl_list link; // rdp_backend::output_list
};

typedef struct _rdp_audio_block_info {
	UINT64 submissionTime;
	UINT64 ackReceivedTime;
	UINT64 ackPlayedTime;
} rdp_audio_block_info;

struct rdp_peer_context {
	rdpContext _p;

	struct rdp_backend *rdpBackend;
	struct wl_event_source *events[MAX_FREERDP_FDS+1]; // +1 for WTSVirtualChannelManagerGetFileDescriptor
	RFX_CONTEXT *rfx_context;
	wStream *encode_stream;
	RFX_RECT *rfx_rects;
	NSC_CONTEXT *nsc_context;

	struct rdp_peers_item item;

	bool button_state[5];
	bool mouseButtonSwap;
	int verticalAccumWheelRotationPrecise;
	int verticalAccumWheelRotationDiscrete;
	int horizontalAccumWheelRotationPrecise;
	int horizontalAccumWheelRotationDiscrete;

	// RAIL support
	HANDLE vcm;
	RailServerContext* rail_server_context;
	DrdynvcServerContext* drdynvc_server_context;
	DispServerContext* disp_server_context;
	RdpgfxServerContext* rail_grfx_server_context;
#ifdef HAVE_FREERDP_GFXREDIR_H
	GfxRedirServerContext* gfxredir_server_context;
#endif // HAVE_FREERDP_GFXREDIR_H
#ifdef HAVE_FREERDP_RDPAPPLIST_H
	RdpAppListServerContext* applist_server_context;
#endif // HAVE_FREERDP_RDPAPPLIST_H
	BOOL handshakeCompleted;
	BOOL activationRailCompleted;
	BOOL activationGraphicsCompleted;
	BOOL activationGraphicsRedirectionCompleted;
	UINT32 clientStatusFlags;
	struct rdp_id_manager windowId;
	struct rdp_id_manager surfaceId;
#ifdef HAVE_FREERDP_GFXREDIR_H
	struct rdp_id_manager poolId;
	struct rdp_id_manager bufferId;
#endif // HAVE_FREERDP_GFXREDIR_H
	UINT32 currentFrameId;
	UINT32 acknowledgedFrameId;
	BOOL isAcknowledgedSuspended;
	struct wl_client *clientExec;
	struct wl_listener clientExec_destroy_listener;
	struct weston_surface *cursorSurface;

	// list of outstanding event_source sent from FreeRDP thread to display loop.
	int loop_task_event_source_fd;
	struct wl_event_source *loop_task_event_source;
	pthread_mutex_t loop_task_list_mutex;
	struct wl_list loop_task_list; // struct rdp_loop_task::link

	// RAIL power management.
	struct wl_listener idle_listener;
	struct wl_listener wake_listener;

	bool is_window_zorder_dirty;
	struct weston_surface *active_surface;

	// Multiple monitor support (monitor topology)
	pixman_region32_t regionClientHeads;
	pixman_region32_t regionWestonHeads;

	// Audio support
	RdpsndServerContext* rdpsnd_server_context;
	BOOL audioExitSignal;
	int pulseAudioSinkListenerFd;
	int pulseAudioSinkFd;
	pthread_t pulseAudioSinkThread;
	int bytesPerFrame;
	UINT audioBufferSize;
	BYTE* audioBuffer;
	BYTE lastBlockSent;
	UINT64 lastNetworkLatency;
	UINT64 accumulatedNetworkLatency;
	UINT accumulatedNetworkLatencyCount;
	UINT64 lastRenderedLatency;
	UINT64 accumulatedRenderedLatency;
	UINT accumulatedRenderedLatencyCount;
	rdp_audio_block_info blockInfo[256];
	int nextValidBlock;
	UINT PAVersion;

	// AudioIn support
	audin_server_context* audin_server_context;
	BOOL audioInExitSignal;
	int pulseAudioSourceListenerFd;
	int pulseAudioSourceFd;
	int closeAudioSourceFd;
	int audioInSem;
	pthread_t pulseAudioSourceThread;
	BOOL isAudioInStreamOpened;

	// Clipboard support
	CliprdrServerContext* clipboard_server_context;

	struct rdp_clipboard_data_source* clipboard_client_data_source;
	struct rdp_clipboard_data_source* clipboard_inflight_client_data_source;

	struct wl_listener clipboard_selection_listener;

	// Application List support
	BOOL isAppListEnabled;
};

typedef struct rdp_peer_context RdpPeerContext;

typedef void (*rdp_loop_task_func_t)(bool freeOnly, void *data);

struct rdp_loop_task {
	struct wl_list link;
	RdpPeerContext *peerCtx;
	rdp_loop_task_func_t func;
};

#define RDP_RAIL_MARKER_WINDOW_ID  0xFFFFFFFE
#define RDP_RAIL_DESKTOP_WINDOW_ID 0xFFFFFFFF

#define ENABLE_RDP_THREAD_CHECK

#ifdef ENABLE_RDP_THREAD_CHECK
#define ASSERT_COMPOSITOR_THREAD(b)     assert_compositor_thread(b)
#define ASSERT_NOT_COMPOSITOR_THREAD(b) assert_not_compositor_thread(b)
#else
#define ASSERT_COMPOSITOR_THREAD(b)
#define ASSERT_NOT_COMPOSITOR_THREAD(b)
#endif // ENABLE_RDP_THREAD_CHECK

#define RDP_DEBUG_LEVEL_NONE    0
#define RDP_DEBUG_LEVEL_ERR     1
#define RDP_DEBUG_LEVEL_WARN    2
#define RDP_DEBUG_LEVEL_INFO    3
#define RDP_DEBUG_LEVEL_DEBUG   4
#define RDP_DEBUG_LEVEL_VERBOSE 5

/* To enable rdp_debug message, add "--logger-scopes=rdp-backend". */
#define RDP_DEBUG_LEVEL_DEFAULT RDP_DEBUG_LEVEL_INFO

#define rdp_debug_verbose(b, ...) \
	if (b->debugLevel >= RDP_DEBUG_LEVEL_VERBOSE) \
		rdp_debug_print(b->debug, false, __VA_ARGS__)
#define rdp_debug_verbose_continue(b, ...) \
	if (b->debugLevel >= RDP_DEBUG_LEVEL_VERBOSE) \
		rdp_debug_print(b->debug, true,  __VA_ARGS__)
#define rdp_debug(b, ...) \
	if (b->debugLevel >= RDP_DEBUG_LEVEL_INFO) \
		rdp_debug_print(b->debug, false, __VA_ARGS__)
#define rdp_debug_continue(b, ...) \
	if (b->debugLevel >= RDP_DEBUG_LEVEL_INFO) \
		rdp_debug_print(b->debug, true,  __VA_ARGS__)
#define rdp_debug_error(b, ...) \
	if (b->debugLevel >= RDP_DEBUG_LEVEL_ERR) \
		rdp_debug_print(b->debug, false, __VA_ARGS__)

/* To enable rdp_debug_clipboard message, add "--logger-scopes=rdp-backend-clipboard". */
#define RDP_DEBUG_CLIPBOARD_LEVEL_DEFAULT RDP_DEBUG_LEVEL_ERR

#define rdp_debug_clipboard_verbose(b, ...) \
	if (b->debugClipboardLevel >= RDP_DEBUG_LEVEL_VERBOSE) \
		rdp_debug_print(b->debugClipboard, false, __VA_ARGS__)
#define rdp_debug_clipboard_verbose_continue(b, ...) \
	if (b->debugClipboardLevel >= RDP_DEBUG_LEVEL_VERBOSE) \
		rdp_debug_print(b->debugClipboard, true,  __VA_ARGS__)
#define rdp_debug_clipboard(b, ...) \
	if (b->debugClipboardLevel >= RDP_DEBUG_LEVEL_INFO) \
		rdp_debug_print(b->debugClipboard, false, __VA_ARGS__)
#define rdp_debug_clipboard_continue(b, ...) \
	if (b->debugClipboardLevel >= RDP_DEBUG_LEVEL_INFO) \
		rdp_debug_print(b->debugClipboard, true,  __VA_ARGS__)
#define rdp_debug_clipboard_error(b, ...) \
	if (b->debugClipboardLevel >= RDP_DEBUG_LEVEL_ERR) \
		rdp_debug_print(b->debugClipboard, false, __VA_ARGS__)

/* To enable rdp_debug message, add "--logger-scopes=rdp-backend". */

// rdp.c
void convert_rdp_keyboard_to_xkb_rule_names(UINT32 KeyboardType, UINT32 KeyboardSubType, UINT32 KeyboardLayout, struct xkb_rule_names *xkbRuleNames);
struct rdp_head * rdp_head_create(struct weston_compositor *compositor, BOOL isPrimary, struct rdp_monitor_mode *monitorMode);
void rdp_head_destroy(struct weston_compositor *compositor, struct rdp_head *head);

// rdputil.c
pid_t rdp_get_tid(void);
void rdp_debug_print(struct weston_log_scope *log_scope, bool cont, char *fmt, ...);
#ifdef ENABLE_RDP_THREAD_CHECK
void assert_compositor_thread(struct rdp_backend *b);
void assert_not_compositor_thread(struct rdp_backend *b);
#endif // ENABLE_RDP_THREAD_CHECK
#ifdef HAVE_FREERDP_GFXREDIR_H
BOOL rdp_allocate_shared_memory(struct rdp_backend *b, struct weston_rdp_shared_memory *shared_memory);
void rdp_free_shared_memory(struct rdp_backend *b, struct weston_rdp_shared_memory *shared_memory);
#endif // HAVE_FREERDP_GFXREDIR_H
BOOL rdp_id_manager_init(struct rdp_backend *rdp_backend, struct rdp_id_manager *id_manager, UINT32 low_limit, UINT32 high_limit);
void rdp_id_manager_free(struct rdp_id_manager *id_manager);
void rdp_id_manager_lock(struct rdp_id_manager *id_manager);
void rdp_id_manager_unlock(struct rdp_id_manager *id_manager);
void *rdp_id_manager_lookup(struct rdp_id_manager *id_manager, UINT32 id);
void rdp_id_manager_for_each(struct rdp_id_manager *id_manager, hash_table_iterator_func_t func, void *data);
BOOL rdp_id_manager_allocate_id(struct rdp_id_manager *id_manager, void *object, UINT32 *new_id);
void rdp_id_manager_free_id(struct rdp_id_manager *id_manager, UINT32 id);
void dump_id_manager_state(FILE *fp, struct rdp_id_manager *id_manager, char* title);
bool rdp_defer_rdp_task_to_display_loop(RdpPeerContext *peerCtx, wl_event_loop_fd_func_t func, void *data, struct wl_event_source **event_source);
void rdp_defer_rdp_task_done(RdpPeerContext *peerCtx);
bool rdp_event_loop_add_fd(struct wl_event_loop *loop, int fd, uint32_t mask, wl_event_loop_fd_func_t func, void *data, struct wl_event_source **event_source);
void rdp_dispatch_task_to_display_loop(RdpPeerContext *peerCtx, rdp_loop_task_func_t func, struct rdp_loop_task *task);
bool rdp_initialize_dispatch_task_event_source(RdpPeerContext *peerCtx);
void rdp_destroy_dispatch_task_event_source(RdpPeerContext *peerCtx);

// rdprail.c
int rdp_rail_backend_create(struct rdp_backend *b, struct weston_rdp_backend_config *config);
void rdp_rail_destroy(struct rdp_backend *b);
BOOL rdp_rail_peer_activate(freerdp_peer* client);
void rdp_rail_sync_window_status(freerdp_peer* client);
BOOL rdp_rail_peer_init(freerdp_peer *client, RdpPeerContext *peerCtx);
void rdp_rail_peer_context_free(freerdp_peer* client, RdpPeerContext* context);
void rdp_rail_output_repaint(struct weston_output *output, pixman_region32_t *damage);
BOOL rdp_drdynvc_init(freerdp_peer *client);
void rdp_drdynvc_destroy(RdpPeerContext* context);
void rdp_rail_start_window_move(struct weston_surface* surface, int pointerGrabX, int pointerGrabY, struct weston_size minSize, struct weston_size maxSize);
void rdp_rail_end_window_move(struct weston_surface* surface);

// rdpdisp.c
UINT disp_client_monitor_layout_change(DispServerContext* context, const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU* displayControl);
BOOL xf_peer_adjust_monitor_layout(freerdp_peer* client);

// rdpaudio.c
int rdp_audio_init(RdpPeerContext *peerCtx);
void rdp_audio_destroy(RdpPeerContext *peerCtx);

// rdpaudioin.c
int rdp_audioin_init(RdpPeerContext *peerCtx);
void rdp_audioin_destroy(RdpPeerContext *peerCtx);

// rdpclip.c
int rdp_clipboard_init(freerdp_peer* client);
void rdp_clipboard_destroy(RdpPeerContext *peerCtx);

static inline struct rdp_head *
to_rdp_head(struct weston_head *base)
{
	return container_of(base, struct rdp_head, base);
}

static inline struct rdp_output *
to_rdp_output(struct weston_output *base)
{
	return container_of(base, struct rdp_output, base);
}

static inline struct rdp_backend *
to_rdp_backend(struct weston_compositor *base)
{
	return container_of(base->backend, struct rdp_backend, base);
}

static inline void
rdp_matrix_transform_position(struct weston_matrix *matrix, int *x, int *y)
{
	struct weston_vector v;
	if (matrix->type != 0) {
		v.f[0] = *x;
		v.f[1] = *y;
		v.f[2] = 0.0f;
		v.f[3] = 1.0f;
		weston_matrix_transform(matrix, &v);
		*x = v.f[0] / v.f[3];
		*y = v.f[1] / v.f[3];
	}
}

static inline void
rdp_matrix_transform_scale(struct weston_matrix *matrix, int *sx, int *sy)
{
	struct weston_vector v;
	if (matrix->type != 0) {
		v.f[0] = *sx;
		v.f[1] = *sy;
		v.f[2] = 0.0f;
		v.f[3] = 0.0f;
		weston_matrix_transform(matrix, &v);
		*sx = v.f[0]; // / v.f[3];
		*sy = v.f[1]; // / v.f[3];
	}
}

static inline void
to_weston_scale_only(RdpPeerContext *peer, struct weston_output *output, float scale, int *x, int *y)
{
	//rdp_matrix_transform_scale(&output->inverse_matrix, x, y);
	/* TODO: built-in to matrix */
	*x = (float)(*x) * scale;
	*y = (float)(*y) * scale;
}

/* Input x/y in client space, output x/y in weston space */
static inline struct weston_output *
to_weston_coordinate(RdpPeerContext *peerContext, int32_t *x, int32_t *y, uint32_t *width, uint32_t *height)
{
	struct rdp_backend *b = peerContext->rdpBackend;
	int sx = *x, sy = *y;
	/* First, find which monitor contains this x/y. */
	struct rdp_head *head_iter;
	wl_list_for_each(head_iter, &b->head_list, link) {
		if (pixman_region32_contains_point(&head_iter->regionClient, sx, sy, NULL)) {
			struct weston_output *output = head_iter->base.output;
			float scale = 1.0f / head_iter->monitorMode.clientScale;
			/* translate x/y to offset from this output on client space. */
			sx -= head_iter->monitorMode.monitorDef.x;
			sy -= head_iter->monitorMode.monitorDef.y;
			/* scale x/y to client output space. */
			to_weston_scale_only(peerContext, output, scale, &sx, &sy);
			if (width && height)
				to_weston_scale_only(peerContext, output, scale, width, height);
			/* translate x/y to offset from this output on weston space. */
			sx += head_iter->monitorMode.rectWeston.x;
			sy += head_iter->monitorMode.rectWeston.y;
			rdp_debug_verbose(b, "%s: (x:%d, y:%d) -> (sx:%d, sy:%d) at head:%s\n",
				__func__, *x, *y, sx, sy, head_iter->base.name);
			*x = sx;
			*y = sy;
			return output; // must be only 1 head per output.
		}
	}
	/* x/y is outside of any monitors. */
	return NULL;
}

/* TO BE REMOVED */
static inline int32_t
to_client_x(RdpPeerContext *peer, int32_t x)
{
	return x + peer->regionClientHeads.extents.x1;
}

/* TO BE REMOVED */
static inline int32_t
to_client_y(RdpPeerContext *peer, int32_t y)
{
	return y + peer->regionClientHeads.extents.y1;
}

static inline void
to_client_scale_only(RdpPeerContext *peer, struct weston_output *output, float scale, int *x, int *y)
{
	//rdp_matrix_transform_scale(&output->matrix, x, y);
	/* TODO: built-in to matrix */
	*x = (float)(*x) * scale;
	*y = (float)(*y) * scale;
}

/* Input x/y in weston space, output x/y in client space */
static inline void
to_client_coordinate(RdpPeerContext *peerContext, struct weston_output *output, int32_t *x, int32_t *y, uint32_t *width, uint32_t *height)
{
	struct rdp_backend *b = peerContext->rdpBackend;
	int sx = *x, sy = *y;
	/* Pick first head from output. */
	struct weston_head *head_iter;
	wl_list_for_each(head_iter, &output->head_list, output_link) {
		struct rdp_head *head = to_rdp_head(head_iter);
		float scale = head->monitorMode.clientScale;
		/* translate x/y to offset from this output on weston space. */
		sx -= head->monitorMode.rectWeston.x;
		sy -= head->monitorMode.rectWeston.y;
		/* scale x/y to client output space. */
		to_client_scale_only(peerContext, output, scale, &sx, &sy);
		if (width && height)
			to_client_scale_only(peerContext, output, scale, width, height);
		/* translate x/y to offset from this output on client space. */
		sx += head->monitorMode.monitorDef.x;
		sy += head->monitorMode.monitorDef.y;
		rdp_debug_verbose(b, "%s: (x:%d, y:%d) -> (sx:%d, sy:%d) at head:%s\n",
			__func__, *x, *y, sx, sy, head_iter->name);
		*x = sx;
		*y = sy;
		return; // must be only 1 head per output.
	}
}

#endif
