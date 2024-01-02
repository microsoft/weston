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

#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <linux/input.h>
#include <sys/utsname.h>

#include <winpr/rpc.h>

#include <stdio.h>
#include <wchar.h>
#include <strings.h>

#include "rdp.h"

#include "libweston-internal.h"
#include "shared/xalloc.h"

#define RAIL_WINDOW_FULLSCREEN_STYLE (WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_GROUP | WS_TABSTOP)
#define RAIL_WINDOW_NORMAL_STYLE (RAIL_WINDOW_FULLSCREEN_STYLE | WS_THICKFRAME | WS_CAPTION)

#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) > (b)) ? (b) : (a))

extern PWtsApiFunctionTable FreeRDP_InitWtsApi(void);

static void rdp_rail_destroy_window(struct wl_listener *listener, void *data);
static void rdp_rail_schedule_update_window(struct wl_listener *listener, void *data);
static void rdp_rail_dump_window_label(struct weston_surface *surface, char *label, uint32_t label_size);

struct lang_GUID {
	uint32_t Data1;
	uint16_t Data2;
	uint16_t Data3;
	BYTE Data4_0;
	BYTE Data4_1;
	BYTE Data4_2;
	BYTE Data4_3;
	BYTE Data4_4;
	BYTE Data4_5;
	BYTE Data4_6;
	BYTE Data4_7;
};

struct rdp_rail_dispatch_data {
	struct rdp_loop_task task_base;
	freerdp_peer *client;
	union {
		RAIL_SYSPARAM_ORDER sys_param;
		RAIL_SYSCOMMAND_ORDER sys_command;
		RAIL_SYSMENU_ORDER sys_menu;
		RAIL_ACTIVATE_ORDER activate;
		RAIL_EXEC_ORDER exec;
		RAIL_WINDOW_MOVE_ORDER window_move;
		RAIL_SNAP_ARRANGE snap_arrange;
		RAIL_GET_APPID_REQ_ORDER get_appid_req;
		RAIL_LANGUAGEIME_INFO_ORDER language_ime_info;
#ifdef HAVE_FREERDP_RDPAPPLIST_H
		RDPAPPLIST_CLIENT_CAPS_PDU app_list_caps;
#endif /* HAVE_FREERDP_RDPAPPLIST_H */
	};
};

#define RDP_DISPATCH_TO_DISPLAY_LOOP(context, arg_type, arg, callback) \
	{ \
		freerdp_peer *client = (context)->custom; \
		RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context; \
		struct rdp_backend *b = peer_ctx->rdpBackend; \
		struct rdp_rail_dispatch_data *dispatch_data; \
		dispatch_data = xmalloc(sizeof(*dispatch_data)); \
		assert_not_compositor_thread(b); \
		dispatch_data->client = client; \
		dispatch_data->arg_type = *(arg); \
		rdp_dispatch_task_to_display_loop(peer_ctx, callback, &dispatch_data->task_base); \
	}

#ifdef HAVE_FREERDP_RDPAPPLIST_H
static void
applist_client_Caps_callback(bool freeOnly, void *arg)
{
	struct rdp_rail_dispatch_data *data = wl_container_of(arg, data, task_base);
	const RDPAPPLIST_CLIENT_CAPS_PDU *caps = &data->app_list_caps;
	freerdp_peer *client = data->client;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;
	const struct weston_rdprail_shell_api *api = b->rdprail_shell_api;
	char client_language_id[RDPAPPLIST_LANG_SIZE + 1] = {}; /* +1 to ensure null-terminate. */

	rdp_debug(b, "Client AppList caps version:%d\n", caps->version);

	assert_compositor_thread(b);

	if (freeOnly)
		goto free;

	if (!api || !api->start_app_list_update)
		goto free;

	strncpy(client_language_id, caps->clientLanguageId, RDPAPPLIST_LANG_SIZE);
	rdp_debug(b, "Client AppList client language id: %s\n", client_language_id);

	peer_ctx->isAppListEnabled = api->start_app_list_update(b->rdprail_shell_context,
								client_language_id);

free:
	free(data);
}

static UINT
applist_client_Caps(RdpAppListServerContext *context, const RDPAPPLIST_CLIENT_CAPS_PDU *arg)
{
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, app_list_caps, arg, applist_client_Caps_callback);
	return CHANNEL_RC_OK;
}
#endif /* HAVE_FREERDP_RDPAPPLIST_H */

static UINT
rail_client_Handshake(RailServerContext *context, const RAIL_HANDSHAKE_ORDER *handshake)
{
	freerdp_peer *client = context->custom;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;

	rdp_debug(b, "Client HandShake buildNumber:%d\n", handshake->buildNumber);

	peer_ctx->handshakeCompleted = TRUE;
	return CHANNEL_RC_OK;
}

static void
rail_ClientExec_destroy(struct wl_listener *listener, void *data)
{
	RdpPeerContext *peer_ctx = container_of(listener, RdpPeerContext,
						clientExec_destroy_listener);
	struct rdp_backend *b = peer_ctx->rdpBackend;

	rdp_debug(b, "Client ExecOrder program terminated\n");

	wl_list_remove(&peer_ctx->clientExec_destroy_listener.link);
	peer_ctx->clientExec_destroy_listener.notify = NULL;
	peer_ctx->clientExec = NULL;
}

static void
rail_client_Exec_callback(bool freeOnly, void *arg)
{
	struct rdp_rail_dispatch_data *data = wl_container_of(arg, data, task_base);
	const RAIL_EXEC_ORDER *exec = &data->exec;
	freerdp_peer *client = data->client;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;
	const struct weston_rdprail_shell_api *api = b->rdprail_shell_api;
	UINT result = RAIL_EXEC_E_FAIL;
	RAIL_EXEC_RESULT_ORDER orderResult = {};
	char *remoteProgramAndArgs = exec->RemoteApplicationProgram;

	rdp_debug(b, "Client ExecOrder:0x%08X, Program:%s, WorkingDir:%s, RemoteApplicationArguments:%s\n",
		  (UINT)exec->flags,
		  exec->RemoteApplicationProgram,
		  exec->RemoteApplicationWorkingDir,
		  exec->RemoteApplicationArguments);

	assert_compositor_thread(peer_ctx->rdpBackend);

	if (!freeOnly &&
	    exec->RemoteApplicationProgram) {
		if (!utf8_string_to_rail_string(exec->RemoteApplicationProgram, &orderResult.exeOrFile))
			goto send_result;

		if (exec->RemoteApplicationArguments) {
			/* construct remote program path and arguments */
			remoteProgramAndArgs = malloc(strlen(exec->RemoteApplicationProgram) +
						      strlen(exec->RemoteApplicationArguments) +
						      2); /* space between program and args + null terminate. */
			if (!remoteProgramAndArgs)
				goto send_result;
			sprintf(remoteProgramAndArgs, "%s %s", exec->RemoteApplicationProgram, exec->RemoteApplicationArguments);
		}

		/* TODO: server state machine, wait until activation complated */
		while (!peer_ctx->activationRailCompleted)
			usleep(10000);

		/* launch the process specified by RDP client. */
		rdp_debug(b, "Client ExecOrder launching %s\n", remoteProgramAndArgs);
		if (api && api->request_launch_shell_process) {
			peer_ctx->clientExec =
				api->request_launch_shell_process(b->rdprail_shell_context,
								  remoteProgramAndArgs);
		}
		if (peer_ctx->clientExec) {
			assert(!peer_ctx->clientExec_destroy_listener.notify);
			peer_ctx->clientExec_destroy_listener.notify = rail_ClientExec_destroy;
			wl_client_add_destroy_listener(peer_ctx->clientExec,
						       &peer_ctx->clientExec_destroy_listener);
			result = RAIL_EXEC_S_OK;
		} else {
			rdp_debug_error(b, "%s: fail to launch shell process %s\n",
					__func__, remoteProgramAndArgs);
		}
	}

send_result:

	if (!freeOnly) {
		orderResult.flags = exec->flags;
		orderResult.execResult = result;
		orderResult.rawResult = 0;
		peer_ctx->rail_server_context->ServerExecResult(peer_ctx->rail_server_context,
							       &orderResult);
	}

	free(orderResult.exeOrFile.string);
	if (remoteProgramAndArgs != exec->RemoteApplicationProgram)
		free(remoteProgramAndArgs);
	free(exec->RemoteApplicationProgram);
	free(exec->RemoteApplicationWorkingDir);
	free(exec->RemoteApplicationArguments);

	free(data);
}

static UINT
rail_client_Exec(RailServerContext *context, const RAIL_EXEC_ORDER *arg)
{
	RAIL_EXEC_ORDER exec_order = {};

	exec_order.flags = arg->flags;
	if (arg->RemoteApplicationProgram) {
		exec_order.RemoteApplicationProgram = xmalloc(strlen(arg->RemoteApplicationProgram) + 1);
		strcpy(exec_order.RemoteApplicationProgram,
		       arg->RemoteApplicationProgram);
	}
	if (arg->RemoteApplicationWorkingDir) {
		exec_order.RemoteApplicationWorkingDir = xmalloc(strlen(arg->RemoteApplicationWorkingDir) + 1);
		strcpy(exec_order.RemoteApplicationWorkingDir,
		       arg->RemoteApplicationWorkingDir);
	}
	if (arg->RemoteApplicationArguments) {
		exec_order.RemoteApplicationArguments = xmalloc(strlen(arg->RemoteApplicationArguments) + 1);
		strcpy(exec_order.RemoteApplicationArguments,
		       arg->RemoteApplicationArguments);
	}
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, exec, &exec_order,
				     rail_client_Exec_callback);
	return CHANNEL_RC_OK;
}

static void
rail_client_Activate_callback(bool freeOnly, void *arg)
{
	struct rdp_rail_dispatch_data *data = wl_container_of(arg, data, task_base);
	const RAIL_ACTIVATE_ORDER *activate = &data->activate;
	freerdp_peer *client = data->client;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;
	const struct weston_rdprail_shell_api *api = b->rdprail_shell_api;
	struct weston_surface *surface = NULL;

	rdp_debug_verbose(b, "Client: ClientActivate: WindowId:0x%x, enabled:%d\n",
			  activate->windowId, activate->enabled);

	assert_compositor_thread(b);

	if (!freeOnly &&
	    api && api->request_window_activate &&
	    b->rdprail_shell_context) {
		if (activate->windowId && activate->enabled) {
			surface = rdp_id_manager_lookup(&peer_ctx->windowId,
							activate->windowId);
			if (!surface)
				rdp_debug_error(b, "Client: ClientActivate: WindowId:0x%x is not found.\n",
						activate->windowId);
		}
		api->request_window_activate(b->rdprail_shell_context,
					     peer_ctx->item.seat,
					     surface);
	}

	free(data);
}

static UINT
rail_client_Activate(RailServerContext *context, const RAIL_ACTIVATE_ORDER *arg)
{
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, activate,
				     arg, rail_client_Activate_callback);
	return CHANNEL_RC_OK;
}

static void
rail_client_SnapArrange_callback(bool freeOnly, void *arg)
{
	struct rdp_rail_dispatch_data *data = wl_container_of(arg, data, task_base);
	const RAIL_SNAP_ARRANGE *snap = &data->snap_arrange;
	freerdp_peer *client = data->client;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;
	const struct weston_rdprail_shell_api *api = b->rdprail_shell_api;
	struct weston_surface *surface;
	struct weston_surface_rail_state *rail_state;
	pixman_rectangle32_t snap_rect;
	struct weston_geometry geometry;

	rdp_debug_verbose(b, "Client: SnapArrange: WindowId:0x%x at (%d, %d) %dx%d\n",
		  snap->windowId,
		  snap->left,
		  snap->top,
		  snap->right - snap->left,
		  snap->bottom - snap->top);

	assert_compositor_thread(b);

	surface = NULL;
	if (!freeOnly)
		surface = rdp_id_manager_lookup(&peer_ctx->windowId,
						snap->windowId);
	if (surface) {
		rail_state = surface->backend_state;
		if (api && api->request_window_snap) {
			snap_rect.x = snap->left;
			snap_rect.y = snap->top;
			snap_rect.width = snap->right - snap->left;
			snap_rect.height = snap->bottom - snap->top;
			to_weston_coordinate(peer_ctx,
					     &snap_rect.x,
					     &snap_rect.y,
					     &snap_rect.width,
					     &snap_rect.height);
			/* offset window shadow area as there is no shadow when snapped */
			/* window_geometry here is last commited geometry */
			api->get_window_geometry(surface,
						 &geometry);
			snap_rect.x -= geometry.x;
			snap_rect.y -= geometry.y;
			snap_rect.width += (surface->width - geometry.width);
			snap_rect.height += (surface->height - geometry.height);
			api->request_window_snap(surface,
						 snap_rect.x,
						 snap_rect.y,
						 snap_rect.width,
						 snap_rect.height);
			rail_state->forceUpdateWindowState = true;
			rdp_rail_schedule_update_window(NULL, surface);
		}
	}

	free(data);
}

static UINT
rail_client_SnapArrange(RailServerContext *context, const RAIL_SNAP_ARRANGE *arg)
{
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, snap_arrange, arg,
				     rail_client_SnapArrange_callback);
	return CHANNEL_RC_OK;
}

static void
rail_client_WindowMove_callback(bool freeOnly, void *arg)
{
	struct rdp_rail_dispatch_data *data = wl_container_of(arg, data, task_base);
	const RAIL_WINDOW_MOVE_ORDER *windowMove = &data->window_move;
	freerdp_peer *client = data->client;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;
	struct weston_surface *surface;
	struct weston_surface_rail_state *rail_state;
	pixman_rectangle32_t windowMoveRect;
	struct weston_geometry geometry;
	const struct weston_rdprail_shell_api *api = b->rdprail_shell_api;

	rdp_debug(b, "Client: WindowMove: WindowId:0x%x at (%d,%d) %dx%d\n",
		  windowMove->windowId,
		  windowMove->left,
		  windowMove->top,
		  windowMove->right - windowMove->left,
		  windowMove->bottom - windowMove->top);

	assert_compositor_thread(b);

	surface = NULL;
	if (!freeOnly)
		surface = rdp_id_manager_lookup(&peer_ctx->windowId,
						windowMove->windowId);
	if (surface) {
		rail_state = surface->backend_state;
		if (api && api->request_window_move) {
			windowMoveRect.x = windowMove->left;
			windowMoveRect.y = windowMove->top;
			windowMoveRect.width = windowMove->right - windowMove->left;
			windowMoveRect.height = windowMove->bottom - windowMove->top;
			if (!rail_state->isWindowSnapped) {
				/* WindowMove PDU include window resize margin */
				/* [MS-RDPERP] - v20200304 - 3.2.5.1.6 Processing Window Information Orders
				    However, the Client Window Move PDU (section 2.2.2.7.4) and Client Window Snap PDU
				    (section 2.2.2.7.5) do include resize margins in the window boundaries. */
				windowMoveRect.x += rail_state->window_margin_left;
				windowMoveRect.y += rail_state->window_margin_top;
				windowMoveRect.width -= rail_state->window_margin_left +
							rail_state->window_margin_right;
				windowMoveRect.height -= rail_state->window_margin_top +
							 rail_state->window_margin_bottom;
			}
			to_weston_coordinate(peer_ctx,
					     &windowMoveRect.x,
					     &windowMoveRect.y,
					     &windowMoveRect.width,
					     &windowMoveRect.height);
			if (is_window_shadow_remoting_disabled(peer_ctx) ||
				rail_state->isWindowSnapped) {
				/* offset window shadow area */
				/* window_geometry here is last commited geometry */
				api->get_window_geometry(surface,
							 &geometry);
				windowMoveRect.x -= geometry.x;
				windowMoveRect.y -= geometry.y;
				windowMoveRect.width += (surface->width - geometry.width);
				windowMoveRect.height += (surface->height - geometry.height);
			}
			api->request_window_move(surface,
						 windowMoveRect.x,
						 windowMoveRect.y,
						 windowMoveRect.width,
						 windowMoveRect.height);
			rail_state->forceUpdateWindowState = true;
			rdp_rail_schedule_update_window(NULL, surface);
		}
	}

	free(data);
}

static UINT
rail_client_WindowMove(RailServerContext *context, const RAIL_WINDOW_MOVE_ORDER *arg)
{
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, window_move, arg,
				     rail_client_WindowMove_callback);
	return CHANNEL_RC_OK;
}

static void
rail_client_Syscommand_callback(bool freeOnly, void *arg)
{
	struct rdp_rail_dispatch_data *data = wl_container_of(arg, data, task_base);
	const RAIL_SYSCOMMAND_ORDER *syscommand = &data->sys_command;
	freerdp_peer *client = data->client;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;
	struct weston_surface *surface;
	const struct weston_rdprail_shell_api *api = b->rdprail_shell_api;

	assert_compositor_thread(b);

	surface = NULL;
	if (!freeOnly)
		surface = rdp_id_manager_lookup(&peer_ctx->windowId,
						syscommand->windowId);
	if (!surface) {
		rdp_debug_error(b,
				"Client: ClientSyscommand: WindowId:0x%x is not found.\n",
				syscommand->windowId);
		goto Exit;
	}

	char *commandString = NULL;
	switch (syscommand->command) {
	case SC_SIZE:
		commandString = "SC_SIZE";
		break;
	case SC_MOVE:
		commandString = "SC_MOVE";
		break;
	case SC_MINIMIZE:
		commandString = "SC_MINIMIZE";
		if (api && api->request_window_minimize)
			api->request_window_minimize(surface);
		break;
	case SC_MAXIMIZE:
		commandString = "SC_MAXIMIZE";
		if (api && api->request_window_maximize)
			api->request_window_maximize(surface);
		break;
	case SC_CLOSE:
		commandString = "SC_CLOSE";
		if (api && api->request_window_close)
			api->request_window_close(surface);
		break;
	case SC_KEYMENU:
		commandString = "SC_KEYMENU";
		break;
	case SC_RESTORE:
		commandString = "SC_RESTORE";
		if (api && api->request_window_restore)
			api->request_window_restore(surface);
		break;
	case SC_DEFAULT:
		commandString = "SC_DEFAULT";
		break;
	default:
		commandString = "Unknown";
		break;
	}

	rdp_debug_verbose(b,
		  "Client: ClientSyscommand: WindowId:0x%x, surface:0x%p, command:%s (0x%x)\n",
		  syscommand->windowId, surface, commandString,
		  syscommand->command);

Exit:
	free(data);
}

static UINT
rail_client_Syscommand(RailServerContext *context, const RAIL_SYSCOMMAND_ORDER *arg)
{
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, sys_command, arg,
				     rail_client_Syscommand_callback);
	return CHANNEL_RC_OK;
}

static void
rail_client_Sysmenu_callback(bool freeOnly, void *arg)
{
	struct rdp_rail_dispatch_data *data = wl_container_of(arg, data, task_base);
	const RAIL_SYSMENU_ORDER *sysmenu = &data->sys_menu;
	freerdp_peer *client = data->client;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;

	rdp_debug_verbose(b,
		  "Client: ClientSyscommand: WindowId:0x%x, left:%d, top:%d\n",
		  sysmenu->windowId, sysmenu->left, sysmenu->top);

	free(data);
}

static UINT
rail_client_Sysmenu(RailServerContext* context, const RAIL_SYSMENU_ORDER* arg)
{
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, sys_menu, arg,
				     rail_client_Sysmenu_callback);
	return CHANNEL_RC_OK;
}

static void
rail_client_ClientSysparam_callback(bool freeOnly, void *arg)
{
	struct rdp_rail_dispatch_data *data = wl_container_of(arg, data,
							      task_base);
	const RAIL_SYSPARAM_ORDER *sysparam = &data->sys_param;
	freerdp_peer *client = data->client;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;
	pixman_rectangle32_t workareaRect;
	pixman_rectangle32_t workareaRectClient;
	struct weston_output *base_output;
	struct weston_head *base_head_iter;
	const struct weston_rdprail_shell_api *api = b->rdprail_shell_api;

	assert_compositor_thread(b);

	if (sysparam->params & SPI_MASK_SET_DRAG_FULL_WINDOWS) {
		rdp_debug(b, "Client: ClientSysparam: dragFullWindows:%d\n",
			  sysparam->dragFullWindows);
	}

	if (sysparam->params & SPI_MASK_SET_KEYBOARD_CUES) {
		rdp_debug(b, "Client: ClientSysparam: keyboardCues:%d\n",
			  sysparam->keyboardCues);
	}

	if (sysparam->params & SPI_MASK_SET_KEYBOARD_PREF) {
		rdp_debug(b, "Client: ClientSysparam: keyboardPref:%d\n",
			  sysparam->keyboardPref);
	}

	if (sysparam->params & SPI_MASK_SET_MOUSE_BUTTON_SWAP) {
		rdp_debug(b, "Client: ClientSysparam: mouseButtonSwap:%d\n", sysparam->mouseButtonSwap);
		peer_ctx->mouseButtonSwap = sysparam->mouseButtonSwap;
	}

	if (sysparam->params & SPI_MASK_SET_WORK_AREA) {
		rdp_debug(b,
			  "Client: ClientSysparam: workArea:(left:%u, top:%u, right:%u, bottom:%u)\n",
			  sysparam->workArea.left,
			  sysparam->workArea.top,
			  sysparam->workArea.right,
			  sysparam->workArea.bottom);
	}

	if (sysparam->params & SPI_MASK_DISPLAY_CHANGE) {
		rdp_debug(b, "Client: ClientSysparam: displayChange:(left:%u, top:%u, right:%u, bottom:%u)\n",
			  sysparam->displayChange.left,
			  sysparam->displayChange.top,
			  sysparam->displayChange.right,
			  sysparam->displayChange.bottom);
	}

	if (sysparam->params & SPI_MASK_TASKBAR_POS) {
		rdp_debug(b, "Client: ClientSysparam: taskbarPos:(left:%u, top:%u, right:%u, bottom:%u)\n",
			 sysparam->taskbarPos.left,
			 sysparam->taskbarPos.top,
			 sysparam->taskbarPos.right,
			 sysparam->taskbarPos.bottom);
	}

	if (sysparam->params & SPI_MASK_SET_HIGH_CONTRAST) {
		rdp_debug(b, "Client: ClientSysparam: highContrast\n");
	}

	if (sysparam->params & SPI_MASK_SET_CARET_WIDTH) {
		rdp_debug(b, "Client: ClientSysparam: caretWidth:%d\n",
			  sysparam->caretWidth);
	}

	if (sysparam->params & SPI_MASK_SET_STICKY_KEYS) {
		rdp_debug(b, "Client: ClientSysparam: stickyKeys:%d\n",
			  sysparam->stickyKeys);
	}

	if (sysparam->params & SPI_MASK_SET_TOGGLE_KEYS) {
		rdp_debug(b, "Client: ClientSysparam: toggleKeys:%d\n",
			  sysparam->toggleKeys);
	}

	if (sysparam->params & SPI_MASK_SET_FILTER_KEYS) {
		rdp_debug(b, "Client: ClientSysparam: filterKeys\n");
	}

	if (sysparam->params & SPI_MASK_SET_SCREEN_SAVE_ACTIVE) {
		rdp_debug(b, "Client: ClientSysparam: setScreenSaveActive:%d\n",
			  sysparam->setScreenSaveActive);
	}

	if (sysparam->params & SPI_MASK_SET_SET_SCREEN_SAVE_SECURE) {
		rdp_debug(b, "Client: ClientSysparam: setScreenSaveSecure:%d\n",
			  sysparam->setScreenSaveSecure);
	}

	if (!freeOnly) {
		if (sysparam->params & SPI_MASK_SET_WORK_AREA &&
		    api && api->set_desktop_workarea) {
			workareaRectClient.x = (int32_t)(int16_t)sysparam->workArea.left;
			workareaRectClient.y = (int32_t)(int16_t)sysparam->workArea.top;
			workareaRectClient.width = (int32_t)(int16_t)sysparam->workArea.right - workareaRectClient.x;
			workareaRectClient.height = (int32_t)(int16_t)sysparam->workArea.bottom - workareaRectClient.y;
			/* Workarea is reported in client coordinate where primary monitor' upper-left is (0,0). */
			/* traslate to weston coordinate where entire desktop's upper-left is (0,0). */
			workareaRect = workareaRectClient;
			base_output = to_weston_coordinate(peer_ctx,
							   &workareaRect.x,
							   &workareaRect.y,
							   &workareaRect.width,
							   &workareaRect.height);
			if (base_output) {
				rdp_debug(b,
					  "Translated workarea:(%d,%d)-(%d,%d) at %s:(%d,%d)-(%d,%d)\n",
					  workareaRect.x, workareaRect.y,
					  workareaRect.x + workareaRect.width,
					  workareaRect.y + workareaRect.height,
					  base_output->name,
					  base_output->x, base_output->y,
					  base_output->x + base_output->width,
					  base_output->y + base_output->height);
				api->set_desktop_workarea(base_output,
							  b->rdprail_shell_context,
							  &workareaRect);
				wl_list_for_each(base_head_iter, &base_output->head_list, output_link) {
					struct rdp_head *head = to_rdp_head(base_head_iter);

					head->workarea = workareaRect;
					head->workareaClient = workareaRectClient;
				}
			} else {
				rdp_debug_error(b, "Client: ClientSysparam: workArea isn't belonging to an output\n");
			}
		}
	}

	free(data);
}

static UINT
rail_client_ClientSysparam(RailServerContext *context, const RAIL_SYSPARAM_ORDER *arg)
{
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, sys_param, arg,
				     rail_client_ClientSysparam_callback);
	return CHANNEL_RC_OK;
}

static void
rail_client_ClientGetAppidReq_callback(bool freeOnly, void *arg)
{
	struct rdp_rail_dispatch_data* data = wl_container_of(arg, data, task_base);
	const RAIL_GET_APPID_REQ_ORDER* getAppidReq = &data->get_appid_req;
	freerdp_peer *client = data->client;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;
	const struct weston_rdprail_shell_api *api = b->rdprail_shell_api;
	char appId[520] = {};
	char imageName[520] = {};
	pid_t pid;
	size_t i;
	unsigned short *p;
	struct weston_surface *surface;

	rdp_debug_verbose(b, "Client: ClientGetAppidReq: WindowId:0x%x\n",
			  getAppidReq->windowId);

	assert_compositor_thread(b);

	if (!freeOnly &&
	    api && api->get_window_app_id) {

		surface = rdp_id_manager_lookup(&peer_ctx->windowId, getAppidReq->windowId);
		if (!surface) {
			rdp_debug_error(b, "Client: ClientGetAppidReq: WindowId:0x%x is not found.\n",
					getAppidReq->windowId);
			goto Exit;
		}

		pid = api->get_window_app_id(b->rdprail_shell_context,
					     surface, &appId[0],
					     sizeof(appId), &imageName[0],
					     sizeof(imageName));
		if (appId[0] == '\0') {
			rdp_debug_error(b, "Client: ClientGetAppidReq: WindowId:0x%x does not have appId, or not top level window.\n",
					getAppidReq->windowId);
			goto Exit;
		}

		rdp_debug(b, "Client: ClientGetAppidReq: pid:%d appId:%s WindowId:0x%x\n",
			  (uint32_t)pid, appId, getAppidReq->windowId);
		rdp_debug_verbose(b,
				  "Client: ClientGetAppidReq: pid:%d imageName:%s\n",
				  (uint32_t)pid, imageName);

		/* Reply with RAIL_GET_APPID_RESP_EX when pid/imageName is valid and client supports it */
		if ((pid >= 0) && (imageName[0] != '\0') &&
		    peer_ctx->clientStatusFlags & TS_RAIL_CLIENTSTATUS_GET_APPID_RESPONSE_EX_SUPPORTED) {
			RAIL_GET_APPID_RESP_EX getAppIdRespEx = {};

			getAppIdRespEx.windowID = getAppidReq->windowId;
			for (i = 0, p = &getAppIdRespEx.applicationID[0]; i < strlen(appId); i++, p++)
				*p = (unsigned short)appId[i];
			getAppIdRespEx.processId = (uint32_t) pid;
			for (i = 0, p = &getAppIdRespEx.processImageName[0]; i < strlen(imageName); i++, p++)
				*p = (unsigned short)imageName[i];
			peer_ctx->rail_server_context->ServerGetAppidRespEx(peer_ctx->rail_server_context,
									   &getAppIdRespEx);
		} else {
			RAIL_GET_APPID_RESP_ORDER getAppIdResp = {};

			getAppIdResp.windowId = getAppidReq->windowId;
			for (i = 0, p = &getAppIdResp.applicationId[0]; i < strlen(appId); i++, p++)
				*p = (unsigned short)appId[i];
			peer_ctx->rail_server_context->ServerGetAppidResp(peer_ctx->rail_server_context,
									 &getAppIdResp);
		}
	}

Exit:
	free(data);
}

static UINT
rail_client_ClientGetAppidReq(RailServerContext *context,
			      const RAIL_GET_APPID_REQ_ORDER *arg)
{
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, get_appid_req, arg,
				     rail_client_ClientGetAppidReq_callback);
	return CHANNEL_RC_OK;
}

static UINT
rail_client_ClientStatus(RailServerContext *context,
			 const RAIL_CLIENT_STATUS_ORDER *clientStatus)
{
	freerdp_peer *client = context->custom;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;

	rdp_debug(b, "Client: ClientStatus:0x%x\n", clientStatus->flags);
	if (clientStatus->flags & TS_RAIL_CLIENTSTATUS_ALLOWLOCALMOVESIZE)
		rdp_debug(b, "     - TS_RAIL_CLIENTSTATUS_ALLOWLOCALMOVESIZE\n");
	if (clientStatus->flags & TS_RAIL_CLIENTSTATUS_AUTORECONNECT)
		rdp_debug(b, "     - TS_RAIL_CLIENTSTATUS_AUTORECONNECT\n");
	if (clientStatus->flags & TS_RAIL_CLIENTSTATUS_ZORDER_SYNC)
		rdp_debug(b, "     - TS_RAIL_CLIENTSTATUS_ZORDER_SYNC\n");
	if (clientStatus->flags & TS_RAIL_CLIENTSTATUS_WINDOW_RESIZE_MARGIN_SUPPORTED)
		rdp_debug(b, "     - TS_RAIL_CLIENTSTATUS_WINDOW_RESIZE_MARGIN_SUPPORTED\n");
	if (clientStatus->flags & TS_RAIL_CLIENTSTATUS_HIGH_DPI_ICONS_SUPPORTED)
		rdp_debug(b, "     - TS_RAIL_CLIENTSTATUS_HIGH_DPI_ICONS_SUPPORTED\n");
	if (clientStatus->flags & TS_RAIL_CLIENTSTATUS_APPBAR_REMOTING_SUPPORTED)
		rdp_debug(b, "     - TS_RAIL_CLIENTSTATUS_APPBAR_REMOTING_SUPPORTED\n");
	if (clientStatus->flags & TS_RAIL_CLIENTSTATUS_POWER_DISPLAY_REQUEST_SUPPORTED)
		rdp_debug(b, "     - TS_RAIL_CLIENTSTATUS_POWER_DISPLAY_REQUEST_SUPPORTED\n");
	if (clientStatus->flags & TS_RAIL_CLIENTSTATUS_GET_APPID_RESPONSE_EX_SUPPORTED)
		rdp_debug(b, "     - TS_RAIL_CLIENTSTATUS_GET_APPID_RESPONSE_EX_SUPPORTED\n");
	if (clientStatus->flags & TS_RAIL_CLIENTSTATUS_BIDIRECTIONAL_CLOAK_SUPPORTED)
		rdp_debug(b, "     - TS_RAIL_CLIENTSTATUS_BIDIRECTIONAL_CLOAK_SUPPORTED\n");

	peer_ctx->clientStatusFlags = clientStatus->flags;
	return CHANNEL_RC_OK;
}

static UINT
rail_client_LangbarInfo(RailServerContext *context,
			const RAIL_LANGBAR_INFO_ORDER *langbarInfo)
{
	freerdp_peer *client = context->custom;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;

	rdp_debug(b, "Client: LangbarInfo: LanguageBarStatus:%d\n",
		  langbarInfo->languageBarStatus);

	return CHANNEL_RC_OK;
}

/* GUID_CHTIME_BOPOMOFO is not defined in FreeRDP */
#ifndef GUID_CHTIME_BOPOMOFO
#define GUID_CHTIME_BOPOMOFO \
{ \
	0xB115690A, 0xEA02, 0x48D5, 0xA2, 0x31, 0xE3, 0x57, 0x8D, 0x2F, 0xDF, 0x80 \
}
#endif

/* Define GUID for Google IME */
#define GUID_GOOGLEIME_JPN \
{ \
	0xd5a86fd5, 0x5308, 0x47ea, 0xad, 0x16, 0x9c, 0x4e, 0xb1, 0x60, 0xec, 0x3c \
}

#define GUID_PROFILE_GOOGLEIME_JPN \
{ \
	0x773eb24e, 0xca1d, 0x4b1b, 0xb4, 0x20, 0xfa, 0x98, 0x5b, 0xb0, 0xb8, 0x0d \
}

static void
rdp_debug_raw_guid_string(struct rdp_backend *b, const GUID *guid)
{
	rdp_debug_continue(b, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		guid->Data1, guid->Data2, guid->Data3,
		guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
		guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
}

static char *
languageGuid_to_string(const GUID *guid)
{
	static_assert(sizeof(struct lang_GUID) == sizeof(GUID));
	static const struct lang_GUID c_GUID_NULL = GUID_NULL;
	static const struct lang_GUID c_GUID_MS_JPNIME = GUID_MSIME_JPN;
	static const struct lang_GUID c_GUID_GOOGLE_JPNIME = GUID_GOOGLEIME_JPN;
	static const struct lang_GUID c_GUID_KORIME = GUID_MSIME_KOR;
	static const struct lang_GUID c_GUID_CHSIME = GUID_CHSIME;
	static const struct lang_GUID c_GUID_CHTIME = GUID_CHTIME;
	static const struct lang_GUID c_GUID_CHTIME_BOPOMOFO = GUID_CHTIME_BOPOMOFO;
	static const struct lang_GUID c_GUID_PROFILE_NEWPHONETIC = GUID_PROFILE_NEWPHONETIC;
	static const struct lang_GUID c_GUID_PROFILE_CHANGJIE = GUID_PROFILE_CHANGJIE;
	static const struct lang_GUID c_GUID_PROFILE_QUICK = GUID_PROFILE_QUICK;
	static const struct lang_GUID c_GUID_PROFILE_CANTONESE = GUID_PROFILE_CANTONESE;
	static const struct lang_GUID c_GUID_PROFILE_PINYIN = GUID_PROFILE_PINYIN;
	static const struct lang_GUID c_GUID_PROFILE_SIMPLEFAST = GUID_PROFILE_SIMPLEFAST;
	static const struct lang_GUID c_GUID_PROFILE_MSIME_JPN = GUID_GUID_PROFILE_MSIME_JPN;
	static const struct lang_GUID c_GUID_PROFILE_GOOGLEIME_JPN = GUID_PROFILE_GOOGLEIME_JPN;
	static const struct lang_GUID c_GUID_PROFILE_MSIME_KOR = GUID_PROFILE_MSIME_KOR;

	RPC_STATUS rpc_status;
	if (UuidEqual(guid, (GUID *)&c_GUID_NULL, &rpc_status))
		return "GUID_NULL";
	else if (UuidEqual(guid, (GUID *)&c_GUID_MS_JPNIME, &rpc_status))
		return "GUID_MS_JPNIME";
	else if (UuidEqual(guid, (GUID *)&c_GUID_GOOGLE_JPNIME, &rpc_status))
		return "GUID_GOOGLE_JPNIME";
	else if (UuidEqual(guid, (GUID *)&c_GUID_KORIME, &rpc_status))
		return "GUID_KORIME";
	else if (UuidEqual(guid, (GUID *)&c_GUID_CHSIME, &rpc_status))
		return "GUID_CHSIME";
	else if (UuidEqual(guid, (GUID *)&c_GUID_CHTIME, &rpc_status))
		return "GUID_CHTIME";
	else if (UuidEqual(guid, (GUID *)&c_GUID_CHTIME_BOPOMOFO, &rpc_status))
		return "GUID_CHTIME_BOPOMOFO";
	else if (UuidEqual(guid, (GUID *)&c_GUID_PROFILE_NEWPHONETIC, &rpc_status))
		return "GUID_PROFILE_NEWPHONETIC";
	else if (UuidEqual(guid, (GUID *)&c_GUID_PROFILE_CHANGJIE, &rpc_status))
		return "GUID_PROFILE_CHANGJIE";
	else if (UuidEqual(guid, (GUID *)&c_GUID_PROFILE_QUICK, &rpc_status))
		return "GUID_PROFILE_QUICK";
	else if (UuidEqual(guid, (GUID *)&c_GUID_PROFILE_CANTONESE, &rpc_status))
		return "GUID_PROFILE_CANTONESE";
	else if (UuidEqual(guid, (GUID *)&c_GUID_PROFILE_PINYIN, &rpc_status))
		return "GUID_PROFILE_PINYIN";
	else if (UuidEqual(guid, (GUID *)&c_GUID_PROFILE_SIMPLEFAST, &rpc_status))
		return "GUID_PROFILE_SIMPLEFAST";
	else if (UuidEqual(guid, (GUID *)&c_GUID_PROFILE_MSIME_JPN, &rpc_status))
		return "GUID_PROFILE_MSIME_JPN";
	else if (UuidEqual(guid, (GUID *)&c_GUID_PROFILE_GOOGLEIME_JPN, &rpc_status))
		return "GUID_PROFILE_GOOGLEIME_JPN";
	else if (UuidEqual(guid, (GUID *)&c_GUID_PROFILE_MSIME_KOR, &rpc_status))
		return "GUID_PROFILE_MSIME_KOR";
	else
		return "Unknown GUID";
}

static void
rail_client_LanguageImeInfo_callback(bool freeOnly, void *arg)
{
	struct rdp_rail_dispatch_data *data = wl_container_of(arg, data,
							      task_base);
	const RAIL_LANGUAGEIME_INFO_ORDER *languageImeInfo = &data->language_ime_info;
	freerdp_peer *client = data->client;
	rdpSettings *settings = client->context->settings;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;
	uint32_t new_keyboard_layout = 0;
	struct xkb_keymap *keymap = NULL;
	struct xkb_rule_names xkbRuleNames = {};
	char *s;

	assert_compositor_thread(b);

	switch (languageImeInfo->ProfileType) {
	case TF_PROFILETYPE_INPUTPROCESSOR:
		s = "TF_PROFILETYPE_INPUTPROCESSOR";
		break;
	case TF_PROFILETYPE_KEYBOARDLAYOUT:
		s = "TF_PROFILETYPE_KEYBOARDLAYOUT";
		break;
	default:
		s = "Unknown profile type";
		break;
	}
	rdp_debug(b, "Client: LanguageImeInfo: ProfileType: %d (%s)\n",
		  languageImeInfo->ProfileType, s);
	rdp_debug(b, "Client: LanguageImeInfo: LanguageID: 0x%x\n",
		  languageImeInfo->LanguageID);

	rdp_debug(b, "Client: LanguageImeInfo: LanguageProfileCLSID: %s : ",
		  languageGuid_to_string(&languageImeInfo->LanguageProfileCLSID));
	rdp_debug_raw_guid_string(b, &languageImeInfo->LanguageProfileCLSID);
	rdp_debug_continue(b, "\n");

	rdp_debug(b, "Client: LanguageImeInfo: ProfileGUID: %s : ",
		  languageGuid_to_string(&languageImeInfo->ProfileGUID));
	rdp_debug_raw_guid_string(b, &languageImeInfo->ProfileGUID);
	rdp_debug_continue(b, "\n");

	rdp_debug(b, "Client: LanguageImeInfo: KeyboardLayout: 0x%x\n",
		  languageImeInfo->KeyboardLayout);

	if (!freeOnly) {
		if (languageImeInfo->ProfileType == TF_PROFILETYPE_KEYBOARDLAYOUT) {
			new_keyboard_layout = languageImeInfo->KeyboardLayout;
		} else if (languageImeInfo->ProfileType == TF_PROFILETYPE_INPUTPROCESSOR) {
			static_assert(sizeof(struct lang_GUID) == sizeof(GUID));

			static const struct lang_GUID c_GUID_MS_JPNIME = GUID_MSIME_JPN;
			static const struct lang_GUID c_GUID_GOOGLE_JPNIME = GUID_GOOGLEIME_JPN;
			static const struct lang_GUID c_GUID_KORIME = GUID_MSIME_KOR;
			static const struct lang_GUID c_GUID_CHSIME = GUID_CHSIME;
			static const struct lang_GUID c_GUID_CHTIME = GUID_CHTIME;
			static const struct lang_GUID c_GUID_CHTIME_BOPOMOFO = GUID_CHTIME_BOPOMOFO;

			RPC_STATUS rpc_status;
			if (UuidEqual(&languageImeInfo->LanguageProfileCLSID,
				      (GUID *)&c_GUID_MS_JPNIME, &rpc_status))
				new_keyboard_layout = KBD_JAPANESE;
			else if (UuidEqual(&languageImeInfo->LanguageProfileCLSID,
				      (GUID *)&c_GUID_GOOGLE_JPNIME, &rpc_status))
				new_keyboard_layout = KBD_JAPANESE;
			else if (UuidEqual(&languageImeInfo->LanguageProfileCLSID,
					   (GUID *)&c_GUID_KORIME, &rpc_status))
				new_keyboard_layout = KBD_KOREAN;
			else if (UuidEqual(&languageImeInfo->LanguageProfileCLSID,
					   (GUID *)&c_GUID_CHSIME, &rpc_status))
				new_keyboard_layout = KBD_CHINESE_SIMPLIFIED_US;
			else if (UuidEqual(&languageImeInfo->LanguageProfileCLSID,
					   (GUID *)&c_GUID_CHTIME, &rpc_status))
				new_keyboard_layout = KBD_CHINESE_TRADITIONAL_US;
			else if (UuidEqual(&languageImeInfo->LanguageProfileCLSID,
					   (GUID *)&c_GUID_CHTIME_BOPOMOFO, &rpc_status))
				new_keyboard_layout = KBD_CHINESE_TRADITIONAL_US;
			else
				new_keyboard_layout = KBD_US;
		}

		if (new_keyboard_layout &&
		    (new_keyboard_layout != settings->KeyboardLayout)) {
			convert_rdp_keyboard_to_xkb_rule_names(settings->KeyboardType,
							       settings->KeyboardSubType,
							       new_keyboard_layout,
							       &xkbRuleNames);
			if (xkbRuleNames.layout) {
				keymap = xkb_keymap_new_from_names(b->compositor->xkb_context,
								   &xkbRuleNames, 0);
				if (keymap) {
					weston_seat_update_keymap(peer_ctx->item.seat, keymap);
					xkb_keymap_unref(keymap);
					settings->KeyboardLayout = new_keyboard_layout;
					rdp_debug(b, "%s: new keyboard layout: 0x%x\n",
						__func__, new_keyboard_layout);
				}
			}
			if (!keymap) {
				rdp_debug_error(b, "%s: Failed to switch to kbd_layout:0x%x kbd_type:0x%x kbd_subType:0x%x\n",
						__func__, new_keyboard_layout,
						settings->KeyboardType,
						settings->KeyboardSubType);

                                rdp_debug_error(b, "%s: Resetting default keymap\n", __func__);
                                keymap = xkb_keymap_new_from_names(peer_ctx->item.seat->compositor->xkb_context,
                                                                   &peer_ctx->item.seat->compositor->xkb_names,
                                                                   0);
                                weston_seat_update_keymap(peer_ctx->item.seat, keymap);
                                xkb_keymap_unref(keymap);
                                settings->KeyboardLayout = new_keyboard_layout;
			}
		}
	}

	free(data);
}

static UINT
rail_client_LanguageImeInfo(RailServerContext *context,
				const RAIL_LANGUAGEIME_INFO_ORDER *arg)
{
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, language_ime_info, arg,
				     rail_client_LanguageImeInfo_callback);
	return CHANNEL_RC_OK;
}

static UINT
rail_client_CompartmentInfo(RailServerContext *context,
				const RAIL_COMPARTMENT_INFO_ORDER *compartmentInfo)
{
	freerdp_peer *client = (freerdp_peer *)context->custom;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;

	rdp_debug(b, "Client: CompartmentInfo: ImeStatus: %s\n",
		  compartmentInfo->ImeState ? "OPEN" : "CLOSED");
	rdp_debug(b, "Client: CompartmentInfo: ImeConvMode: 0x%x\n",
		  compartmentInfo->ImeConvMode);
	rdp_debug(b, "Client: CompartmentInfo: ImeSentenceMode: 0x%x\n",
		  compartmentInfo->ImeSentenceMode);
	rdp_debug(b, "Client: CompartmentInfo: KanaMode: %s\n",
		  compartmentInfo->KanaMode ? "ON" : "OFF");

	return CHANNEL_RC_OK;
}

static UINT
rail_grfx_client_caps_advertise(RdpgfxServerContext *context, const RDPGFX_CAPS_ADVERTISE_PDU *capsAdvertise)
{
	freerdp_peer *client = (freerdp_peer *)context->custom;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;
	RdpgfxServerContext *gfx_ctx = peer_ctx->rail_grfx_server_context;

	rdp_debug(b, "Client: GrfxCaps count:0x%x\n",
		  capsAdvertise->capsSetCount);
	for (int i = 0; i < capsAdvertise->capsSetCount; i++) {
		RDPGFX_CAPSET *capsSet = &(capsAdvertise->capsSets[i]);

		rdp_debug(b, "Client: GrfxCaps[%d] version:0x%x length:%d flags:0x%x\n",
			  i, capsSet->version, capsSet->length, capsSet->flags);
		switch(capsSet->version) {
 		case RDPGFX_CAPVERSION_8:
			rdp_debug(b, "	Version : RDPGFX_CAPVERSION_8\n");
			break;
 		case RDPGFX_CAPVERSION_81:
			rdp_debug(b, "	Version : RDPGFX_CAPVERSION_81\n");
			break;
 		case RDPGFX_CAPVERSION_10:
			rdp_debug(b, "	Version : RDPGFX_CAPVERSION_10\n");
			break;
		case RDPGFX_CAPVERSION_101:
			rdp_debug(b, "	Version : RDPGFX_CAPVERSION_101\n");
			break;
 		case RDPGFX_CAPVERSION_102:
			rdp_debug(b, "	Version : RDPGFX_CAPVERSION_102\n");
			break;
 		case RDPGFX_CAPVERSION_103:
			rdp_debug(b, "	Version : RDPGFX_CAPVERSION_103\n");
			break;
 		case RDPGFX_CAPVERSION_104:
			rdp_debug(b, "	Version : RDPGFX_CAPVERSION_104\n");
			break;
 		case RDPGFX_CAPVERSION_105:
			rdp_debug(b, "	Version : RDPGFX_CAPVERSION_105\n");
			break;
 		case RDPGFX_CAPVERSION_106:
			rdp_debug(b, "	Version : RDPGFX_CAPVERSION_106\n");
			break;
		}

		if (capsSet->flags & RDPGFX_CAPS_FLAG_THINCLIENT)
			rdp_debug(b, "     - RDPGFX_CAPS_FLAG_THINCLIENT\n");
		if (capsSet->flags & RDPGFX_CAPS_FLAG_SMALL_CACHE)
			rdp_debug(b, "     - RDPGFX_CAPS_FLAG_SMALL_CACHE\n");
		if (capsSet->flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED)
			rdp_debug(b, "     - RDPGFX_CAPS_FLAG_AVC420_ENABLED\n");
		if (capsSet->flags & RDPGFX_CAPS_FLAG_AVC_DISABLED)
			rdp_debug(b, "     - RDPGFX_CAPS_FLAG_AVC_DISABLED\n");
		if (capsSet->flags & RDPGFX_CAPS_FLAG_AVC_THINCLIENT)
			rdp_debug(b, "     - RDPGFX_CAPS_FLAG_AVC_THINCLIENT\n");

		switch(capsSet->version) {
 		case RDPGFX_CAPVERSION_8:
		{
			/*RDPGFX_CAPSET_VERSION8 *caps8 = (RDPGFX_CAPSET_VERSION8 *)capsSet;*/
			break;
		}
 		case RDPGFX_CAPVERSION_81:
		{
			/*RDPGFX_CAPSET_VERSION81 *caps81 = (RDPGFX_CAPSET_VERSION81 *)capsSet;*/
		 	break;
		}
 		case RDPGFX_CAPVERSION_10:
 		case RDPGFX_CAPVERSION_101:
 		case RDPGFX_CAPVERSION_102:
 		case RDPGFX_CAPVERSION_103:
 		case RDPGFX_CAPVERSION_104:
 		case RDPGFX_CAPVERSION_105:
 		case RDPGFX_CAPVERSION_106:
		{
			/*RDPGFX_CAPSET_VERSION10 *caps10 = (RDPGFX_CAPSET_VERSION10 *)capsSet;*/
			break;
		}
		default:
			rdp_debug_error(b, "	Version : UNKNOWN(%d)\n",
					capsSet->version);
		}
	}

	/* send caps confirm */
	RDPGFX_CAPS_CONFIRM_PDU capsConfirm = {};

	capsConfirm.capsSet = capsAdvertise->capsSets; /* TODO: choose right one.*/
	gfx_ctx->CapsConfirm(gfx_ctx, &capsConfirm);

	/* ready to use graphics channel */
	peer_ctx->activationGraphicsCompleted = TRUE;
	return CHANNEL_RC_OK;
}

static UINT
rail_grfx_client_cache_import_offer(RdpgfxServerContext *context,
				    const RDPGFX_CACHE_IMPORT_OFFER_PDU *cacheImportOffer)
{
	freerdp_peer *client = context->custom;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;

	rdp_debug_verbose(b, "Client: GrfxCacheImportOffer\n");
	return CHANNEL_RC_OK;
}

static UINT
rail_grfx_client_frame_acknowledge(RdpgfxServerContext *context,
				   const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge)
{
	freerdp_peer *client = context->custom;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;
	rdp_debug_verbose(b, "Client: GrfxFrameAcknowledge(queueDepth = 0x%x, frameId = 0x%x, decodedFrame = %d)\n",
			  frameAcknowledge->queueDepth, frameAcknowledge->frameId, frameAcknowledge->totalFramesDecoded);
	peer_ctx->acknowledgedFrameId = frameAcknowledge->frameId;
	peer_ctx->isAcknowledgedSuspended = (frameAcknowledge->queueDepth == 0xffffffff);
	return CHANNEL_RC_OK;
}

#ifdef HAVE_FREERDP_GFXREDIR_H
static UINT
gfxredir_client_graphics_redirection_legacy_caps(GfxRedirServerContext *context,
						 const GFXREDIR_LEGACY_CAPS_PDU *redirectionCaps)
{
	freerdp_peer *client = context->custom;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;

	rdp_debug(b, "Client: gfxredir_caps: version:%d\n",
		  redirectionCaps->version);
	/* This is legacy caps callback, version must be 1 */
	if (redirectionCaps->version != GFXREDIR_CHANNEL_VERSION_LEGACY) {
		rdp_debug_error(b, "Client: gfxredir_caps: invalid version:%d\n",
				redirectionCaps->version);
		return ERROR_INTERNAL_ERROR;
	}

	/* Legacy version 1 client is not supported, so don't set 'activationGraphicsRedirectionCompleted'. */
	rdp_debug_error(b, "Client: gfxredir_caps: version 1 is not supported.\n");

	return CHANNEL_RC_OK;
}

static UINT
gfxredir_client_graphics_redirection_caps_advertise(GfxRedirServerContext *context,
						    const GFXREDIR_CAPS_ADVERTISE_PDU *redirectionCaps)
{
	freerdp_peer *client = context->custom;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;
	const GFXREDIR_CAPS_HEADER *current = (const GFXREDIR_CAPS_HEADER *)redirectionCaps->caps;
	const GFXREDIR_CAPS_V2_0_PDU *capsV2 = NULL;

	/* dump client caps */
	uint32_t i = 0;
	uint32_t length = redirectionCaps->length;

	rdp_debug(b, "Client: gfxredir_caps: length:%d\n",
		  redirectionCaps->length);
	while (length <= redirectionCaps->length &&
	       length >= sizeof(GFXREDIR_CAPS_HEADER)) {
		rdp_debug(b, "Client: gfxredir_caps[%d]: signature:0x%x\n",
			  i, current->signature);
		rdp_debug(b, "Client: gfxredir_caps[%d]: version:0x%x\n",
			  i, current->version);
		rdp_debug(b, "Client: gfxredir_caps[%d]: length:%d\n",
			  i, current->length);
		if (current->version == GFXREDIR_CAPS_VERSION2_0) {
			capsV2 = (GFXREDIR_CAPS_V2_0_PDU *)current;
			rdp_debug(b, "Client: gfxredir_caps[%d]: supportedFeatures:0x%x\n",
				  i, capsV2->supportedFeatures);
		}
		i++;
		length -= current->length;
		current = (const GFXREDIR_CAPS_HEADER *)((BYTE*)current + current->length);
	}

	/* select client caps */
	const GFXREDIR_CAPS_HEADER *selected = NULL;
	uint32_t selectedVersion = 0;

	current = (const GFXREDIR_CAPS_HEADER *)redirectionCaps->caps;
	length = redirectionCaps->length;
	while (length <= redirectionCaps->length &&
	       length >= sizeof(GFXREDIR_CAPS_HEADER)) {
		if (current->signature != GFXREDIR_CAPS_SIGNATURE)
			return ERROR_INVALID_DATA;
		/* Choose >= ver. 2_0 */
		if (current->version >= selectedVersion) {
			selected = current;
			selectedVersion = current->version;
		}
		length -= current->length;
		current = (const GFXREDIR_CAPS_HEADER *)((BYTE*)current + current->length);
	}

	/* reply selected caps */
	if (selected) {
		GFXREDIR_CAPS_CONFIRM_PDU confirmPdu = {};

		rdp_debug(b, "Client: gfxredir selected caps: version:0x%x\n",
			  selected->version);

		confirmPdu.version = selected->version; /* return the version of selected caps */
		confirmPdu.length = selected->length; /* must return same length as selected caps from advertised */
		confirmPdu.capsData = (const BYTE *)(selected + 1); /* return caps data in selected caps */

		context->GraphicsRedirectionCapsConfirm(context,
							&confirmPdu);
	}

	/* ready to use graphics redirection channel */
	peer_ctx->activationGraphicsRedirectionCompleted = true;
	return CHANNEL_RC_OK;
}

static UINT
gfxredir_client_present_buffer_ack(GfxRedirServerContext *context,
				   const GFXREDIR_PRESENT_BUFFER_ACK_PDU *presentAck)
{
	freerdp_peer *client = context->custom;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;
	struct weston_surface *surface;
	struct weston_surface_rail_state *rail_state;

	rdp_debug_verbose(b,
			  "Client: gfxredir_present_buffer_ack: windowId:0x%lx\n",
			  presentAck->windowId);
	rdp_debug_verbose(b,
			  "Client: gfxredir_present_buffer_ack: presentId:0x%lx\n",
			  presentAck->presentId);

	peer_ctx->acknowledgedFrameId = (uint32_t)presentAck->presentId;

	/* when accessing ID outside of wayland display loop thread, aquire lock */
	rdp_id_manager_lock(&peer_ctx->windowId);
	surface = rdp_id_manager_lookup(&peer_ctx->windowId,
					presentAck->windowId);
	if (surface) {
		rail_state = surface->backend_state;
		rail_state->isUpdatePending = FALSE;
	} else {
		rdp_debug_error(b,
				"Client: PresentBufferAck: WindowId:0x%lx is not found.\n",
				presentAck->windowId);
	}
	rdp_id_manager_unlock(&peer_ctx->windowId);

	return CHANNEL_RC_OK;
}
#endif /* HAVE_FREERDP_GFXREDIR_H */

static int
rdp_rail_create_cursor(struct weston_surface *surface)
{
	struct weston_compositor *compositor = surface->compositor;
	struct rdp_backend *b = to_rdp_backend(compositor);
	RdpPeerContext *peer_ctx = (RdpPeerContext *)b->rdp_peer->context;
	struct weston_surface_rail_state *rail_state = surface->backend_state;

	assert_compositor_thread(b);

	rail_state->clientPos.width = 0; /* triggers force update on next update. */
	rail_state->clientPos.height = 0;
	pixman_region32_init_rect(&rail_state->damage, 0, 0,
				  surface->width_from_buffer,
				  surface->height_from_buffer);

	if (peer_ctx->cursorSurface)
		rdp_debug_error(b,
				"cursor surface already exists old %p vs new %p\n",
				peer_ctx->cursorSurface, surface);
	peer_ctx->cursorSurface = surface;

	return 0;
}

static int
rdp_rail_update_cursor(struct weston_surface *surface)
{
	struct weston_pointer *pointer = surface->committed_private;
	struct weston_compositor *compositor = surface->compositor;
	struct weston_surface_rail_state *rail_state = surface->backend_state;
	struct rdp_backend *b = to_rdp_backend(compositor);
	RdpPeerContext *peer_ctx = (RdpPeerContext *)b->rdp_peer->context;
	rdpUpdate *update = b->rdp_peer->context->update;
	BOOL isCursorResized = FALSE;
	BOOL isCursorHidden = FALSE;
	BOOL isCursorDamanged = FALSE;
	struct weston_rdp_rail_window_pos newPos = {.x = 0, .y = 0, .width = surface->width, .height = surface->height};
	struct weston_rdp_rail_window_pos newClientPos = {.x = 0, .y = 0, .width = surface->width, .height = surface->height};
	int content_buffer_width;
	int content_buffer_height;

	assert_compositor_thread(b);
	assert(rail_state);

	weston_surface_get_content_size(surface,
					&content_buffer_width,
					&content_buffer_height);
	newClientPos.width = content_buffer_width;
	newClientPos.height = content_buffer_height;
	if (surface->output)
		to_client_coordinate(peer_ctx, surface->output,
				     &newClientPos.x, &newClientPos.y, /* these are zero since position at client side doesn't matter */
				     &newClientPos.width,
				     &newClientPos.height);

	if (newPos.x < 0 || newPos.y < 0 || /* check if negative in weston space */
	    newClientPos.width <= 0 || newClientPos.height <= 0) {
		isCursorHidden = TRUE;
		rdp_debug_verbose(b, "CursorUpdate: hidden\n");
	} else if (rail_state->clientPos.width != newClientPos.width || /* check if size changed in client side */
		   rail_state->clientPos.height != newClientPos.height) {
		isCursorResized = TRUE;
		rdp_debug_verbose(b, "CursorUpdate: resized\n");
	} else if (pixman_region32_not_empty(&rail_state->damage)) {
		isCursorDamanged = TRUE;
		rdp_debug_verbose(b, "CursorUpdate: dirty\n");
	}

	rail_state->clientPos = newClientPos;
	pixman_region32_clear(&rail_state->damage);

	if (isCursorHidden) {
		/* hide pointer */
		POINTER_SYSTEM_UPDATE pointerSystem = {};

		pointerSystem.type = SYSPTR_NULL;
		update->BeginPaint(update->context);
		update->pointer->PointerSystem(update->context,
					       &pointerSystem);
		update->EndPaint(update->context);
	} else if (isCursorResized || isCursorDamanged) {
		POINTER_LARGE_UPDATE pointerUpdate = {};
		int cursorBpp = 4; /* Bytes Per Pixel. */
		int pointerBitsSize = newClientPos.width * cursorBpp*newClientPos.height;
		BYTE *pointerBits = xmalloc(pointerBitsSize);

		/* client expects y-flip image for cursor */
		if (weston_surface_copy_content(surface,
						pointerBits,
						pointerBitsSize, 0,
						newClientPos.width,
						newClientPos.height,
						0, 0,
						content_buffer_width,
						content_buffer_height,
						true /* y-flip */,
						true /* is_argb */) < 0) {
			rdp_debug_error(b, "weston_surface_copy_content failed for cursor shape\n");
			free(pointerBits);
			return -1;
		}

		pointerUpdate.xorBpp = cursorBpp * 8; /* Bits Per Pixel. */
		pointerUpdate.cacheIndex = 0;
		pointerUpdate.hotSpotX = pointer ? pointer->hotspot_x : 0;
		pointerUpdate.hotSpotY = pointer ? pointer->hotspot_y : 0;
		pointerUpdate.width = newClientPos.width;
		pointerUpdate.height = newClientPos.height;
		pointerUpdate.lengthAndMask = 0;
		pointerUpdate.lengthXorMask = pointerBitsSize;
		pointerUpdate.xorMaskData = pointerBits;
		pointerUpdate.andMaskData = NULL;

		rdp_debug_verbose(b, "CursorUpdate(width %d, height %d)\n", newPos.width, newPos.height);
		update->BeginPaint(update->context);
		update->pointer->PointerLarge(update->context, &pointerUpdate);
		update->EndPaint(update->context);

		free(pointerBits);
	}

	return 0;
}

static char *
rdp_showstate_to_string(uint32_t showstate)
{
	switch (showstate) {
	case RDP_WINDOW_HIDE:
		return "Hide";
	case RDP_WINDOW_SHOW_MINIMIZED:
		return "Minimized";
	case RDP_WINDOW_SHOW_MAXIMIZED:
		return "Maximized";
	case RDP_WINDOW_SHOW_FULLSCREEN:
		return "Fullscreen";
	case RDP_WINDOW_SHOW:
		return "Normal";
	default:
		return "Unknown";
	}
}

static void
rdp_rail_create_window(struct wl_listener *listener, void *data)
{
	struct weston_surface *surface = data;
	struct weston_compositor *compositor = surface->compositor;
	struct weston_surface_rail_state *rail_state = surface->backend_state;
	struct rdp_backend *b = to_rdp_backend(compositor);
	const struct weston_rdprail_shell_api *api = b->rdprail_shell_api;
	rdpUpdate *update;
	WINDOW_ORDER_INFO window_order_info = {};
	WINDOW_STATE_ORDER window_state_order = {};
	struct weston_rdp_rail_window_pos pos = {
		.x = 0, .y = 0,
		.width = surface->width, .height = surface->height,
	};
	struct weston_rdp_rail_window_pos clientPos = {
		.x = 0, .y = 0,
		.width = surface->width, .height = surface->height,
	};
	struct weston_geometry geometry = {
		.x = 0, .y = 0,
		.width = surface->width, .height = surface->height,
	};
	RECTANGLE_16 window_rect = { 0, 0, surface->width, surface->height };
	RECTANGLE_16 window_vis = { 0, 0, surface->width, surface->height };
	uint32_t window_margin_top = 0, window_margin_left = 0;
	uint32_t window_margin_right = 0, window_margin_bottom = 0;
	int numViews;
	struct weston_view *view;
	uint32_t window_id;
	RdpPeerContext *peer_ctx;

	/* negative width/height is not allowed, allow window to be created with zeros */
	if (surface->width < 0 || surface->height < 0) {
		rdp_debug_error(b, "surface width and height are negative\n");
		return;
	}

	if (!b || !b->rdp_peer) {
		rdp_debug_error(b, "CreateWndow(): rdp_peer is not initalized\n");
		return;
	}

	if (!b->rdp_peer->context->settings->HiDefRemoteApp)
		return;

	if (!b->rdp_peer->context) {
		rdp_debug_verbose(b, "CreateWndow(): rdp_peer->context is not initalized\n");
		return;
	}

	peer_ctx = (RdpPeerContext *)b->rdp_peer->context;

	assert_compositor_thread(b);

	if (!peer_ctx->activationRailCompleted) {
		rdp_debug_verbose(b, "CreateWindow(): rdp_peer rail is not activated.\n");
		return;
	}

	/* HiDef requires graphics channel to be ready */
	if (!peer_ctx->activationGraphicsCompleted) {
		rdp_debug_verbose(b, "CreateWindow(): graphics channel is not activated.\n");
		return;
	}

	if (!rail_state) {
		rail_state = xzalloc(sizeof *rail_state);
		surface->backend_state = rail_state;
	} else {
		/* If ever encouter error for this window, no more attempt to create window */
		if (rail_state->error)
			return;
	}

	/* windowId can be assigned only after activation completed */
	if (!rdp_id_manager_allocate_id(&peer_ctx->windowId, surface, &window_id)) {
		rail_state->error = true;
		rdp_debug_error(b, "CreateWindow(): fail to insert windowId (windowId:0x%x surface:%p).\n",
				window_id, surface);
		return;
	}
	rail_state->window_id = window_id;
	/* Once this surface is inserted to hash table, we want to be notified for destroy */
	assert(!rail_state->destroy_listener.notify);
	rail_state->destroy_listener.notify = rdp_rail_destroy_window;
	wl_signal_add(&surface->destroy_signal, &rail_state->destroy_listener);

	if (surface->role_name != NULL) {
		if (strcmp(surface->role_name, "wl_subsurface") == 0) {
			rail_state->parent_surface = weston_surface_get_main_surface(surface);
			assert(surface != rail_state->parent_surface);
		} else if (strcmp(surface->role_name, "wl_pointer-cursor") == 0) {
			rail_state->isCursor = true;
		}
	}
	if (rail_state->isCursor) {
		if (rdp_rail_create_cursor(surface) < 0)
			rail_state->error = true;
		goto Exit;
	}

	/* obtain view's global position */
	numViews = 0;
	wl_list_for_each(view, &surface->views, surface_link) {
		float sx, sy;

		weston_view_to_global_float(view, 0, 0, &sx, &sy);
		clientPos.x = pos.x = (int)sx;
		clientPos.y = pos.y = (int)sy;
		numViews++;
		break; /* just handle the first view for this hack */
	}
	if (numViews == 0) {
		view = NULL;
		rdp_debug_verbose(b, "%s: surface has no view (windowId:0x%x)\n",
				  __func__, rail_state->window_id);
	}

	if (is_window_shadow_remoting_disabled(peer_ctx)) {
		/* drop window shadow area */
		api->get_window_geometry(surface, &geometry);

		/* calculate window margin from input extents */
		if (geometry.x > max(0, surface->input.extents.x1))
			window_margin_left = geometry.x - max(0, surface->input.extents.x1);
		window_margin_left = max(window_margin_left, RDP_RAIL_WINDOW_RESIZE_MARGIN);

		if (geometry.y > max(0, surface->input.extents.y1))
			window_margin_top = geometry.y - max(0, surface->input.extents.y1);
		window_margin_top = max(window_margin_top, RDP_RAIL_WINDOW_RESIZE_MARGIN);

		if (min(surface->input.extents.x2, surface->width) > (geometry.x + geometry.width))
			window_margin_right = min(surface->input.extents.x2, surface->width) - (geometry.x + geometry.width);
		window_margin_right = max(window_margin_right, RDP_RAIL_WINDOW_RESIZE_MARGIN);

		if (min(surface->input.extents.y2, surface->height) > (geometry.y + geometry.height))
			window_margin_bottom = min(surface->input.extents.y2, surface->height) - (geometry.y + geometry.height);
		window_margin_bottom = max(window_margin_bottom, RDP_RAIL_WINDOW_RESIZE_MARGIN);

		/* offset window origin by window geometry */
		clientPos.x += geometry.x;
		clientPos.y += geometry.y;
		clientPos.width = geometry.width;
		clientPos.height = geometry.height;
	}

	/* apply global to output transform, and translate to client coordinate */
	if (surface->output) {
		to_client_coordinate(peer_ctx, surface->output,
				     &clientPos.x, &clientPos.y,
				     &clientPos.width, &clientPos.height);

		if (is_window_shadow_remoting_disabled(peer_ctx)) {
			to_client_coordinate(peer_ctx, surface->output,
					     &window_margin_left,
					     &window_margin_top,
					     NULL, NULL);
			to_client_coordinate(peer_ctx, surface->output,
					     &window_margin_right,
					     &window_margin_bottom,
					     NULL, NULL);
		}
	}

	window_rect.top = window_vis.top = clientPos.y;
	window_rect.left = window_vis.left = clientPos.x;
	window_rect.right = window_vis.right = clientPos.x + clientPos.width;
	window_rect.bottom = window_vis.bottom = clientPos.y + clientPos.height;

	window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW |
				       WINDOW_ORDER_STATE_NEW;
	window_order_info.windowId = window_id;

	window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_STYLE;
	window_state_order.style = RAIL_WINDOW_NORMAL_STYLE;
	window_state_order.extendedStyle = WS_EX_LAYERED;

	window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_OWNER;
	if (rail_state->parent_surface &&
	    rail_state->parent_surface->backend_state) {
		struct weston_surface_rail_state *parent_rail_state =
			rail_state->parent_surface->backend_state;

		window_state_order.ownerWindowId = parent_rail_state->window_id;
	} else {
		window_state_order.ownerWindowId = RDP_RAIL_DESKTOP_WINDOW_ID;
	}

	/* window is created with hidden and no taskbar icon always, and
	   it become visbile when window has some contents to show. */
	window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW |
					WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
	window_state_order.showState = WINDOW_HIDE;
	window_state_order.TaskbarButton = 1;

	window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_OFFSET;
	window_state_order.clientOffsetX = 0;
	window_state_order.clientOffsetY = 0;

	window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE;
	window_state_order.clientAreaWidth = clientPos.width;
	window_state_order.clientAreaHeight = clientPos.height;

	window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_OFFSET;
	window_state_order.windowOffsetX = clientPos.x;
	window_state_order.windowOffsetY = clientPos.y;

	window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_CLIENT_DELTA;
	window_state_order.windowClientDeltaX = 0;
	window_state_order.windowClientDeltaY = 0;

	window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_SIZE;
	window_state_order.windowWidth = clientPos.width;
	window_state_order.windowHeight = clientPos.height;

	window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_RECTS;
	window_state_order.numWindowRects = 1;
	window_state_order.windowRects = &window_rect;

	window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VIS_OFFSET;
	window_state_order.visibleOffsetX = 0;
	window_state_order.visibleOffsetY = 0;

	window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VISIBILITY;
	window_state_order.numVisibilityRects = 1;
	window_state_order.visibilityRects = &window_vis;

	if (is_window_shadow_remoting_disabled(peer_ctx)) {
		/* add resize margin area */
		window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_RESIZE_MARGIN_X |
						WINDOW_ORDER_FIELD_RESIZE_MARGIN_Y;
		window_state_order.resizeMarginLeft = window_margin_left;
		window_state_order.resizeMarginTop = window_margin_top;
		window_state_order.resizeMarginRight = window_margin_right;
		window_state_order.resizeMarginBottom = window_margin_bottom;
	}

	/*window_state_order.titleInfo = NULL; */
	/*window_state_order.OverlayDescription = 0;*/

	rdp_debug_verbose(b, "WindowCreate(0x%x - (%d, %d, %d, %d)\n",
			  window_id, clientPos.x, clientPos.y,
			  clientPos.width, clientPos.height);

	update = b->rdp_peer->context->update;
	update->BeginPaint(update->context);
	update->window->WindowCreate(update->context,
				     &window_order_info,
				     &window_state_order);
	update->EndPaint(update->context);

	rail_state->parent_window_id = window_state_order.ownerWindowId;
	rail_state->pos = pos;
	rail_state->clientPos = clientPos;
	rail_state->window_margin_left = window_margin_left;
	rail_state->window_margin_top = window_margin_top;
	rail_state->window_margin_right = window_margin_right;
	rail_state->window_margin_bottom = window_margin_bottom;
	rail_state->isWindowCreated = TRUE;
	rail_state->get_label = (void *)-1; /* label to be re-checked at update. */
	rail_state->taskbarButton = window_state_order.TaskbarButton;
	assert(window_state_order.showState == WINDOW_HIDE);
	rail_state->showState = RDP_WINDOW_HIDE;
	rail_state->showState_requested = RDP_WINDOW_SHOW; // show window at following update.
	pixman_region32_init_rect(&rail_state->damage, 0, 0,
				  surface->width_from_buffer,
				  surface->height_from_buffer);

	/* as new window created, mark z order dirty */
	/* TODO: ideally this better be triggered from shell, but shell isn't notified
		 creation/destruction of certain type of window, such as dropdown menu
		 (popup in Wayland, override_redirect in X), thus do it here. */
	peer_ctx->is_window_zorder_dirty = true;

Exit:
	/* once window is successfully created, start listening repaint update */
	if (!rail_state->error) {
		assert(!rail_state->repaint_listener.notify);
		rail_state->repaint_listener.notify = rdp_rail_schedule_update_window;
		wl_signal_add(&surface->repaint_signal,
			      &rail_state->repaint_listener);
	}

	return;
}

#ifdef HAVE_FREERDP_GFXREDIR_H
static void
rdp_destroy_shared_buffer(struct weston_surface *surface)
{
	struct weston_compositor *compositor = surface->compositor;
	struct weston_surface_rail_state *rail_state = surface->backend_state;
	struct rdp_backend *b = to_rdp_backend(compositor);
	RdpPeerContext *peer_ctx = (RdpPeerContext *)b->rdp_peer->context;
	GfxRedirServerContext *redir_ctx = peer_ctx->gfxredir_server_context;

	assert(b->use_gfxredir);

	if (rail_state->buffer_id) {
		GFXREDIR_DESTROY_BUFFER_PDU destroyBuffer = {};

		destroyBuffer.bufferId = rail_state->buffer_id;
		redir_ctx->DestroyBuffer(redir_ctx, &destroyBuffer);

		rdp_id_manager_free_id(&peer_ctx->bufferId,
				       rail_state->buffer_id);
		rail_state->buffer_id = 0;
	}

	if (rail_state->pool_id) {
		GFXREDIR_CLOSE_POOL_PDU closePool = {};

		closePool.poolId = rail_state->pool_id;
		redir_ctx->ClosePool(redir_ctx, &closePool);

		rdp_id_manager_free_id(&peer_ctx->poolId, rail_state->pool_id);
		rail_state->pool_id = 0;
	}

	rdp_free_shared_memory(b, &rail_state->shared_memory);

	rail_state->surfaceBuffer = NULL;
}
#endif /* HAVE_FREERDP_GFXREDIR_H */

static void
rdp_rail_destroy_window(struct wl_listener *listener, void *data)
{
	struct weston_surface *surface = data;
	struct weston_compositor *compositor = surface->compositor;
	struct weston_surface_rail_state *rail_state = surface->backend_state;
	struct rdp_backend *b = to_rdp_backend(compositor);
	RdpgfxServerContext *gfx_ctx;
	rdpUpdate *update;
	WINDOW_ORDER_INFO window_order_info = {};
	POINTER_SYSTEM_UPDATE pointerSystem = {};
	uint32_t window_id;
	RdpPeerContext *peer_ctx;

	if (!rail_state)
		return;

	window_id = rail_state->window_id;
	if (!window_id)
		goto Exit;

	assert(b && b->rdp_peer);

	assert_compositor_thread(b);

	peer_ctx = (RdpPeerContext *)b->rdp_peer->context;
	gfx_ctx = peer_ctx->rail_grfx_server_context;
	update = b->rdp_peer->context->update;
	if (rail_state->isCursor) {
		pointerSystem.type = SYSPTR_NULL;
		update->BeginPaint(update->context);
		update->pointer->PointerSystem(update->context,
					       &pointerSystem);
		update->EndPaint(update->context);
		if (peer_ctx->cursorSurface == surface)
			peer_ctx->cursorSurface = NULL;
		rail_state->isCursor = false;
	} else {
		if (rail_state->isWindowCreated) {
			if (rail_state->surface_id || rail_state->buffer_id) {
				/* When update is pending, need to wait reply from client */
				/* TODO: Defer destroy to FreeRDP callback ? */
				freerdp_peer *client = peer_ctx->rail_grfx_server_context->custom;
				int waitRetry = 0;
				while (rail_state->isUpdatePending ||
				       (peer_ctx->currentFrameId != peer_ctx->acknowledgedFrameId &&
				        !peer_ctx->isAcknowledgedSuspended)) {
					if (++waitRetry > 1000) { /* timeout after 10 sec. */
						rdp_debug_error(b, "%s: update is still pending in client side (windowId:0x%x)\n",
								__func__, window_id);
						break;
					}
					usleep(10000); /* wait 0.01 sec. */
					client->CheckFileDescriptor(client);
					WTSVirtualChannelManagerCheckFileDescriptor(peer_ctx->vcm);
				}
			}

#ifdef HAVE_FREERDP_GFXREDIR_H
			if (b->use_gfxredir)
				rdp_destroy_shared_buffer(surface);
#endif /* HAVE_FREERDP_GFXREDIR_H */

			window_order_info.windowId = window_id;
			window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW |
						       WINDOW_ORDER_STATE_DELETED;

			rdp_debug_verbose(b, "WindowDestroy(0x%x)\n",
					  window_id);
			update->BeginPaint(update->context);
			update->window->WindowDelete(update->context,
						     &window_order_info);
			update->EndPaint(update->context);

			if (rail_state->surface_id) {
				RDPGFX_DELETE_SURFACE_PDU deleteSurface = {};

				rdp_debug_verbose(b, "DeleteSurface(surfaceId:0x%x for windowsId:0x%x)\n",
						  rail_state->surface_id, window_id);

				deleteSurface.surfaceId = (uint16_t)rail_state->surface_id;
				gfx_ctx->DeleteSurface(gfx_ctx, &deleteSurface);

				rdp_id_manager_free_id(&peer_ctx->surfaceId,
						       rail_state->surface_id);
				rail_state->surface_id = 0;
			}
			rail_state->isWindowCreated = FALSE;
		}
	}
	pixman_region32_fini(&rail_state->damage);

	rdp_id_manager_free_id(&peer_ctx->windowId, window_id);
	rail_state->window_id = 0;

	/* as window destroyed, mark z order dirty and if this is active window, clear it */
	/* TODO: ideally this better be triggered from shell, but shell isn't notified
		 creation/destruction of certain type of window, such as dropdown menu
		 (popup in Wayland, override_redirect in X), thus do it here. */
	peer_ctx->is_window_zorder_dirty = true;

	if (rail_state->repaint_listener.notify) {
		wl_list_remove(&rail_state->repaint_listener.link);
		rail_state->repaint_listener.notify = NULL;
	}

	if (rail_state->destroy_listener.notify) {
		wl_list_remove(&rail_state->destroy_listener.link);
		rail_state->destroy_listener.notify = NULL;
	}

Exit:
	free(rail_state);
	surface->backend_state = NULL;

	return;
}

static void
rdp_rail_schedule_update_window(struct wl_listener *listener, void *data)
{
	struct weston_surface *surface = data;
	struct weston_compositor *compositor = surface->compositor;
	struct rdp_backend *b = to_rdp_backend(compositor);
	struct weston_surface_rail_state *rail_state = surface->backend_state;
	uint32_t window_id;

	if (!rail_state || rail_state->error)
		return;

	window_id = rail_state->window_id;
	if (!window_id)
		return;

	assert_compositor_thread(b);

	/* negative width/height is not allowed */
	if (surface->width < 0 || surface->height < 0) {
		rdp_debug_error(b, "surface width and height are negative\n");
		return;
	}

	/* TODO: what width or hight 0 means? should window be hidden? */
	if (surface->width == 0 || surface->height == 0) {
		rdp_debug_verbose(b, "surface width and height are zero WindowId:0x%x (%dx%d)\n",
				  rail_state->window_id, surface->width, surface->height);
		return;
	}

	if (!pixman_region32_union(&rail_state->damage,
				   &rail_state->damage,
				   &surface->damage)) {
		/* if union failed, make entire size of bufer based on current buffer */
		pixman_region32_clear(&rail_state->damage);
		pixman_region32_init_rect(&rail_state->damage, 0, 0,
					  surface->width_from_buffer,
					  surface->height_from_buffer);
	}

	return;
}

struct update_window_iter_data {
	uint32_t output_id;
	uint32_t startedFrameId;
	BOOL needEndFrame;
	BOOL isUpdatePending;
};

static int
rdp_rail_update_window(struct weston_surface *surface,
		       struct update_window_iter_data *iter_data)
{
	struct weston_compositor *compositor = surface->compositor;
	struct weston_surface_rail_state *rail_state = surface->backend_state;
	struct rdp_backend *b = to_rdp_backend(compositor);
	const struct weston_rdprail_shell_api *api = b->rdprail_shell_api;
	rdpUpdate *update;
	WINDOW_ORDER_INFO window_order_info = {};
	WINDOW_STATE_ORDER window_state_order = {};
	struct weston_rdp_rail_window_pos newPos = {
		.x = 0, .y = 0,
		.width = surface->width, .height = surface->height,
	};
	struct weston_rdp_rail_window_pos newClientPos = {
		.x = 0, .y = 0,
		.width = surface->width, .height = surface->height,
	};
	struct weston_geometry geometry = {
		.x = 0, .y = 0,
		.width = surface->width, .height = surface->height,
	};
	struct weston_geometry content_buffer_window_geometry;
	RECTANGLE_16 window_rect;
	RECTANGLE_16 window_vis;
	uint32_t window_margin_top = 0, window_margin_left = 0;
	uint32_t window_margin_right = 0, window_margin_bottom = 0;
	int numViews;
	struct weston_view *view;
	uint32_t window_id;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)b->rdp_peer->context;
	uint32_t new_surface_id = 0;
	uint32_t old_surface_id = 0;
	RAIL_UNICODE_STRING rail_window_title_string = { 0, NULL };
	char window_title[256];
	char window_title_mod[256];
	char *title = NULL;

	assert_compositor_thread(b);

	if (!rail_state || rail_state->error)
		return 0;

	window_id = rail_state->window_id;
	if (!window_id)
		return 0;

	if (surface->role_name != NULL) {
		if (!rail_state->parent_surface) {
			if (strcmp(surface->role_name, "wl_subsurface") == 0) {
				rail_state->parent_surface = weston_surface_get_main_surface(surface);
				assert(surface != rail_state->parent_surface);
			}
		}
		if (!rail_state->isCursor) {
			if (strcmp(surface->role_name, "wl_pointer-cursor") == 0) {
				rdp_debug_error(b, "!!!cursor role is added after creation - WindowId:0x%x\n", window_id);

				/* convert to RDP cursor */
				rdp_rail_destroy_window(NULL, surface);
				assert(!surface->backend_state);

				rdp_rail_create_window(NULL, surface);
				rail_state = surface->backend_state;
				if (!rail_state || rail_state->window_id == 0) {
					rdp_debug_error(b, "Fail to convert to RDP cursor - surface:0x%p\n", surface);
					return 0;
				}
				assert(rail_state->isCursor);
				return rdp_rail_update_cursor(surface);
			}
		}
	}

	/* some spews for future investigation */
	{
		if (surface->width == 0 || surface->height == 0)
			rdp_debug_verbose(b, "update_window: surface width and height is zero windowId:0x%x (%dx%d)\n", window_id, surface->width, surface->height);

		if ((surface->width_from_buffer != surface->width) ||
			(surface->height_from_buffer != surface->height)) {
				rdp_debug(b, "surface width/height doesn't match with buffer (windowId:0x%x)\n", window_id);
				rdp_debug(b, "	surface width %d, height %d\n", surface->width, surface->height);
				rdp_debug(b, "	buffer width %d, height %d\n", surface->width_from_buffer, surface->height_from_buffer);
		}

		static const struct weston_matrix identity = {
			.d = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 },
			.type = 0,
		};

#if 0
		if (memcmp(&surface->buffer_to_surface_matrix.d, &identity.d, sizeof(identity.d)) != 0)
			rdp_debug(b, "buffer to surface matrix is not identity matrix type:0x%x (windowId:0x%x)\n",
					surface->buffer_to_surface_matrix.type, window_id);
#endif

		if (!surface->is_opaque && pixman_region32_not_empty(&surface->opaque)) {
			int numRects = 0;
			pixman_box32_t *rects = pixman_region32_rectangles(&surface->opaque, &numRects);
			rdp_debug_verbose(b, "Window has opaque region: numRects:%d (windowId:0x%x)\n", numRects, window_id);
			for (int n = 0; n < numRects; n++)
				rdp_debug_verbose(b, "  [%d]: (%d, %d) - (%d, %d)\n",
					n, rects[n].x1, rects[n].y1, rects[n].x2, rects[n].y2);
		}

		numViews = 0;
		wl_list_for_each(view, &surface->views, surface_link) {
			numViews++;
			if (view->transform.enabled)
				if (memcmp(&view->transform.matrix.d, &identity.d, sizeof(identity.d)) != 0) {
					if (view->transform.matrix.type != WESTON_MATRIX_TRANSFORM_TRANSLATE) {
						rdp_debug(b, "view[%d] matrix is not identity or translate (windowId:0x%x)\n", numViews, window_id);
						if (view->transform.dirty)
							rdp_debug(b, "view[%d] transform is dirty (windowId:0x%x)\n", numViews, window_id);
					}
				}

			if (view->alpha != 1.0)
				rdp_debug(b, "view[%d] alpha is not 1 (%f) (windowId:0x%x)\n", numViews, view->alpha, window_id);
		}
		if (numViews > 1) {
			view = NULL;
			rdp_debug(b, "suface has more than 1 views. numViews = %d (windowId:0x%x)\n", numViews, window_id);
		}

		/*TODO: when surface is not associated to any output, it looks must not be visible. Need to verify. */
		if (!surface->output)
			rdp_debug_verbose(b, "surface has no output assigned. (windowId:0x%x)\n", window_id);

		/* test with weston-subsurfaces */
		if (surface->subsurface_list.prev != surface->subsurface_list.next)
			rdp_debug_verbose(b, "suface has subsurface (windowId:0x%x)\n", window_id);
	}
	/* end of some spews for future investigation */

	/* obtain view's global position */
	numViews = 0;
	wl_list_for_each(view, &surface->views, surface_link) {
		float sx, sy;

		weston_view_to_global_float(view, 0, 0, &sx, &sy);
		newClientPos.x = newPos.x = (int)sx;
		newClientPos.y = newPos.y = (int)sy;
		numViews++;
		break; /* just handle the first view for this hack */
	}
	if (numViews == 0) {
		view = NULL;
		rdp_debug_verbose(b, "%s: surface has no view (windowId:0x%x)\n",
				  __func__, rail_state->window_id);
	}

	if (is_window_shadow_remoting_disabled(peer_ctx) ||
		rail_state->isWindowSnapped) {
		/* drop window shadow area */
		api->get_window_geometry(surface, &geometry);

		if (is_window_shadow_remoting_disabled(peer_ctx)) {
			/* calculate window margin from input extents */
			if (geometry.x > max(0, surface->input.extents.x1))
				window_margin_left = geometry.x -
					     max(0, surface->input.extents.x1);
			window_margin_left = max(window_margin_left,
						 RDP_RAIL_WINDOW_RESIZE_MARGIN);

			if (geometry.y > max(0, surface->input.extents.y1))
				window_margin_top = geometry.y -
						    max(0, surface->input.extents.y1);
			window_margin_top = max(window_margin_top,
						RDP_RAIL_WINDOW_RESIZE_MARGIN);

			if (min(surface->input.extents.x2, surface->width) > (geometry.x + geometry.width))
				window_margin_right = min(surface->input.extents.x2, surface->width) -
						      (geometry.x + geometry.width);
			window_margin_right = max(window_margin_right,
						  RDP_RAIL_WINDOW_RESIZE_MARGIN);

			if (min(surface->input.extents.y2, surface->height) > (geometry.y + geometry.height))
				window_margin_bottom = min(surface->input.extents.y2, surface->height) -
					       (geometry.y + geometry.height);
			window_margin_bottom = max(window_margin_bottom,
						   RDP_RAIL_WINDOW_RESIZE_MARGIN);
		}

		/* offset window origin by window geometry */
		newClientPos.x += geometry.x;
		newClientPos.y += geometry.y;
		newClientPos.width = geometry.width;
		newClientPos.height = geometry.height;
	}
	content_buffer_window_geometry = geometry;

	/* apply global to output transform, and translate to client coordinate */
	if (surface->output) {
		to_client_coordinate(peer_ctx, surface->output,
				     &newClientPos.x, &newClientPos.y,
				     &newClientPos.width,
				     &newClientPos.height);

		if (is_window_shadow_remoting_disabled(peer_ctx) ||
			rail_state->isWindowSnapped) {
			to_client_coordinate(peer_ctx, surface->output,
					     &window_margin_left,
					     &window_margin_top,
					     NULL, NULL);
			to_client_coordinate(peer_ctx, surface->output,
					     &window_margin_right,
					     &window_margin_bottom,
					     NULL, NULL);
		}
	}

	/* when window move to new output with different scale, refresh all state to client. */
	if (rail_state->output_scale != surface->output->current_scale) {
		rail_state->forceUpdateWindowState = true;
		rail_state->forceRecreateSurface = true;
		rail_state->output_scale = surface->output->current_scale;
	}

	/* Adjust the Windows size and position on the screen */
	if (rail_state->clientPos.x != newClientPos.x ||
	    rail_state->clientPos.y != newClientPos.y ||
	    rail_state->clientPos.width != newClientPos.width ||
	    rail_state->clientPos.height != newClientPos.height ||
	    rail_state->showState != rail_state->showState_requested ||
	    rail_state->get_label != surface->get_label ||
	    rail_state->forceUpdateWindowState) {
  		window_order_info.windowId = window_id;
		window_order_info.fieldFlags =
			WINDOW_ORDER_TYPE_WINDOW;

		if (rail_state->parent_surface &&
		    rail_state->parent_surface->backend_state) {
			struct weston_surface_rail_state *parent_rail_state =
				rail_state->parent_surface->backend_state;
			if (rail_state->parent_window_id != parent_rail_state->window_id) {
				window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_OWNER;

				window_state_order.ownerWindowId = parent_rail_state->window_id;

				rail_state->parent_window_id = parent_rail_state->window_id;

				rdp_debug_verbose(b, "WindowUpdate(0x%x - parent window id:%x)\n",
						  window_id,
						  rail_state->parent_window_id);
			}
		}

		if (rail_state->showState != rail_state->showState_requested) {
			rdp_debug_verbose(b, "WindowUpdate(0x%x - showState:%s -> %s)\n",
					  window_id,
					  rdp_showstate_to_string(rail_state->showState),
					  rdp_showstate_to_string(rail_state->showState_requested));
			/* if exiting fullscreen, restore window style to normal style */
			if (rail_state->showState == RDP_WINDOW_SHOW_FULLSCREEN) {
				window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_STYLE;
				window_state_order.style = RAIL_WINDOW_NORMAL_STYLE;
				window_state_order.extendedStyle = WS_EX_LAYERED;
				/* force update window geometry */
				rail_state->forceUpdateWindowState = true;
			}
			window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW;
			switch (rail_state->showState_requested) {
			case RDP_WINDOW_HIDE:
				window_state_order.showState = WINDOW_HIDE;
				break;
			case RDP_WINDOW_SHOW:
				/* if previoulsy hidden, send minmax info */
				if (rail_state->showState == WINDOW_HIDE &&
				    api && api->request_window_minmax_info)
					api->request_window_minmax_info(surface);
				window_state_order.showState = WINDOW_SHOW;
				break;
			case RDP_WINDOW_SHOW_MINIMIZED:
				window_state_order.showState = WINDOW_SHOW_MINIMIZED;
				break;
			case RDP_WINDOW_SHOW_MAXIMIZED:
				window_state_order.showState = WINDOW_SHOW_MAXIMIZED;
				break;
			case RDP_WINDOW_SHOW_FULLSCREEN:
				/* fullscreen is treat as normal window at Window's client */
				window_state_order.showState = WINDOW_SHOW;
				/* entering fullscreen mode, change window style */
				window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_STYLE;
				window_state_order.style = RAIL_WINDOW_FULLSCREEN_STYLE;
				/* force update window geometry */
				rail_state->forceUpdateWindowState = true;
				break;
			default:
				assert(false);
			}
			rail_state->showState = rail_state->showState_requested;
		}

		if (rail_state->forceUpdateWindowState ||
		    rail_state->get_label != surface->get_label) {
			window_order_info.fieldFlags |=
				WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
			if (rail_state->parent_surface ||
			    (surface->get_label == NULL))
				window_state_order.TaskbarButton = 1;
			else
				window_state_order.TaskbarButton = 0;

			if (surface->get_label &&
			    surface->get_label(surface, window_title, sizeof(window_title))) {
				/* see rdprail-shell for naming convension for label */
				/* TODO: For X11 app, ideally it should check "override" property, but somehow
				         Andriod Studio's (at least 4.1.1) dropdown menu is not "override" window,
				         thus here checks child window, but this causes the other issue that
				         the pop up window, such as "Confirm Exit" (in Andriod Studio) is not shown
				         in taskbar. */
				if (strncmp(window_title, "child window", sizeof("child window") - 1) == 0)
					window_state_order.TaskbarButton = 1;
				title = strchr(window_title, '\'');
				if (title) {
					char *end = strrchr(window_title, '\'');
					if (end != title) {
						*title++ = '\0';
						*end = '\0';
					}
				} else {
					title = window_title;
				}
#ifdef HAVE_FREERDP_GFXREDIR_H
				/* this is for debugging only */
				if (!b->use_gfxredir && b->enable_copy_warning_title) {
					if (snprintf(window_title_mod,
					    sizeof window_title_mod,
					    "[WARN:COPY MODE] %s (%s)",
					    title, b->rdprail_shell_name ? b->rdprail_shell_name : "Linux") > 0)
						title = window_title_mod;
				} else
#endif /* HAVE_FREERDP_GFXREDIR_H */
				if (b->enable_distro_name_title) {
					if (snprintf(window_title_mod,
					    sizeof window_title_mod,
					    "%s (%s)",
					    title, b->rdprail_shell_name ? b->rdprail_shell_name : "Linux") > 0)
						title = window_title_mod;
				} else {
					if (snprintf(window_title_mod,
					    sizeof window_title_mod,
					    "%s",
					    title) > 0)
						title = window_title_mod;
				}
				if (utf8_string_to_rail_string(title, &rail_window_title_string)) {
					window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_TITLE;
					window_state_order.titleInfo = rail_window_title_string;
				}
			}

			rail_state->get_label = surface->get_label;
			rail_state->taskbarButton = window_state_order.TaskbarButton;

			rdp_debug_verbose(b,
					  "WindowUpdate(0x%x - title \"%s\") TaskbarButton:%d\n",
					  window_id, title,
					  window_state_order.TaskbarButton);
		} else {
			/* There seems a bug in mstsc client that previous taskbar button state is
			   not preserved, thus sending taskbar field always. */
			window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
			window_state_order.TaskbarButton = (BYTE)rail_state->taskbarButton;
		}

		if (is_window_shadow_remoting_disabled(peer_ctx)) {
			/* Due to how mstsc/msrdc works, window margin must not be set
			   while window is snapped unless they are changed. */
			if ((rail_state->forceUpdateWindowState &&
				!rail_state->isWindowSnapped) ||
				rail_state->window_margin_left != window_margin_left ||
				rail_state->window_margin_top != window_margin_top ||
				rail_state->window_margin_right != window_margin_right ||
				rail_state->window_margin_bottom != window_margin_bottom) {
				/* add resize margin area */
				window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_RESIZE_MARGIN_X |
								WINDOW_ORDER_FIELD_RESIZE_MARGIN_Y;
				window_state_order.resizeMarginLeft = window_margin_left;
				window_state_order.resizeMarginTop = window_margin_top;
				window_state_order.resizeMarginRight = window_margin_right;
				window_state_order.resizeMarginBottom = window_margin_bottom;

				rail_state->window_margin_left = window_margin_left;
				rail_state->window_margin_top = window_margin_top;
				rail_state->window_margin_right = window_margin_right;
				rail_state->window_margin_bottom = window_margin_bottom;

				rdp_debug_verbose(b,
						  "WindowUpdate(0x%x - window margin left:%d, top:%d, right:%d, bottom:%d\n",
						  window_id,
						  window_margin_left,
						  window_margin_top,
						  window_margin_right,
						  window_margin_bottom);
			}
		}

		if (rail_state->forceUpdateWindowState ||
		    rail_state->clientPos.width != newClientPos.width ||
		    rail_state->clientPos.height != newClientPos.height ||
		    rail_state->output != surface->output) {
			window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_SIZE |
							WINDOW_ORDER_FIELD_WND_RECTS |
							WINDOW_ORDER_FIELD_VISIBILITY |
							WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE;

			window_rect.top = window_vis.top = newClientPos.y;
			window_rect.left = window_vis.left = newClientPos.x;
			window_rect.right = window_vis.right = newClientPos.x +
							       newClientPos.width;
			window_rect.bottom = window_vis.bottom = newClientPos.y +
								 newClientPos.height;

			window_state_order.windowWidth = newClientPos.width;
			window_state_order.windowHeight = newClientPos.height;
			window_state_order.numWindowRects = 1;
			window_state_order.windowRects = &window_rect;
			window_state_order.numVisibilityRects = 1;
			window_state_order.visibilityRects = &window_vis;
			window_state_order.clientAreaWidth = newClientPos.width;
			window_state_order.clientAreaHeight = newClientPos.height;
			if (rail_state->showState != RDP_WINDOW_SHOW_FULLSCREEN) {
				/* when window is not in fullscreen, there should be 'some' area for title bar,
				   thus substracting 32 pixels out from window size for client area, this value
				   does not need to be accurate at all, all here need to tell RDP client is that
				   'real' application client area size is different from window size.
				   To pursue accuracy if desired, this value can be pulled from X for X app,
				   but this seems not possible for Wayland native application. */
				if (window_state_order.clientAreaHeight > 8)
					window_state_order.clientAreaHeight -= 8;
			}

			/* if previous window size is 0 and new window is not,
			   show and place in taskbar (if not set yet) */
			if (rail_state->output == NULL ||
			    ((rail_state->clientPos.width == 0 || rail_state->clientPos.height == 0) &&
			     newClientPos.width && newClientPos.height)) {
				if ((window_order_info.fieldFlags & WINDOW_ORDER_FIELD_TASKBAR_BUTTON) == 0) {
					window_order_info.fieldFlags |=
						WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
					window_state_order.TaskbarButton = 0;
				}
				if ((window_order_info.fieldFlags & WINDOW_ORDER_FIELD_SHOW) == 0) {
					window_order_info.fieldFlags |=
						WINDOW_ORDER_FIELD_SHOW;
					window_state_order.showState = WINDOW_SHOW;
				}

				rdp_debug_verbose(b,
						  "WindowUpdate(0x%x - taskbar:%d showState:%d))\n",
						  window_id,
						  window_state_order.TaskbarButton,
						  window_state_order.showState);
			}
			/* if new window size is 0, and previous is not, or
			   no output assigned, do not show window and
			   do not place in taskbar */
			if (surface->output == NULL ||
			    ((newClientPos.width == 0 || newClientPos.height == 0) &&
			     rail_state->clientPos.width && rail_state->clientPos.height)) {
				window_order_info.fieldFlags |=
					WINDOW_ORDER_FIELD_SHOW |
					WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
				window_state_order.TaskbarButton = 1;
				window_state_order.showState = WINDOW_HIDE;

				rdp_debug_verbose(b,
						  "WindowUpdate(0x%x - taskbar:%d showState:%d))\n",
						  window_id,
						  window_state_order.TaskbarButton,
						  window_state_order.showState);
			}

			rail_state->pos.width = newPos.width;
			rail_state->pos.height = newPos.height;
			rail_state->clientPos.width = newClientPos.width;
			rail_state->clientPos.height = newClientPos.height;
			rail_state->output = surface->output;

			rdp_debug_verbose(b,
					  "WindowUpdate(0x%x - size (%d, %d) in RDP client size (%d, %d)\n",
					  window_id, newPos.width,
					  newPos.height, newClientPos.width,
					  newClientPos.height);
		}

		if (rail_state->forceUpdateWindowState ||
		    rail_state->clientPos.x != newClientPos.x ||
		    rail_state->clientPos.y != newClientPos.y) {
			window_order_info.fieldFlags |=
				WINDOW_ORDER_FIELD_WND_OFFSET;

			window_state_order.windowOffsetX = newClientPos.x;
			window_state_order.windowOffsetY = newClientPos.y;

			rail_state->pos.x = newPos.x;
			rail_state->pos.y = newPos.y;
			rail_state->clientPos.x = newClientPos.x;
			rail_state->clientPos.y = newClientPos.y;

			rdp_debug_verbose(b,
					  "WindowUpdate(0x%x - pos (%d, %d) - RDP client pos (%d, %d)\n",
					  window_id, newPos.x, newPos.y,
					  newClientPos.x, newClientPos.y);
		}

		update = b->rdp_peer->context->update;
		update->BeginPaint(update->context);
		update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
		update->EndPaint(update->context);

		free(rail_window_title_string.string);

		rail_state->forceUpdateWindowState = false;
	}

	/* update window buffer contents */
	{
#ifdef HAVE_FREERDP_GFXREDIR_H
		GfxRedirServerContext *redir_ctx = peer_ctx->gfxredir_server_context;
#endif /* HAVE_FREERDP_GFXREDIR_H */
		BOOL isBufferSizeChanged = FALSE;
		float scaleFactorWidth = 1.0f, scaleFactorHeight = 1.0f;
		int damage_width, damage_height;
		int copy_buffer_stride, copy_buffer_size;
		int copy_buffer_width, copy_buffer_height;
		int client_buffer_width, client_buffer_height;
		int content_buffer_width, content_buffer_height;
		int bufferBpp = 4; /* Bytes Per Pixel. */
		bool hasAlpha = view ? !weston_view_is_opaque(view, &view->transform.boundingbox) : false;
		pixman_box32_t damage_box = *pixman_region32_extents(&rail_state->damage);
		long page_size = sysconf(_SC_PAGESIZE);

		/* clientBuffer represents Windows size on client desktop */
		/* this size is adjusted on whether including or excluding window shadow */
		client_buffer_width = newClientPos.width;
		client_buffer_height = newClientPos.height;

		/* contentBuffer represents buffer from Linux application. */
		/* this size could be larger than it's window size for native HI-DPI rendering */
		weston_surface_get_content_size(surface,
						&content_buffer_width,
						&content_buffer_height);

		/* scale window geometry to content buffer base */
		rdp_matrix_transform_position(&surface->surface_to_buffer_matrix,
					      &content_buffer_window_geometry.x,
					      &content_buffer_window_geometry.y);
		rdp_matrix_transform_position(&surface->surface_to_buffer_matrix,
					      &content_buffer_window_geometry.width,
					      &content_buffer_window_geometry.height);

		/* copy buffer represents the buffer allocated to share with RDP client. */
		/* this can be shared memory buffer or grfx channel surface */
		copy_buffer_width = content_buffer_window_geometry.width;
		copy_buffer_height = content_buffer_window_geometry.height;
		copy_buffer_stride = copy_buffer_width * bufferBpp;
		copy_buffer_size = ((copy_buffer_stride * copy_buffer_height) + page_size - 1) & ~(page_size - 1);

		if (content_buffer_width && content_buffer_height &&
		    copy_buffer_width && copy_buffer_height) {
#ifdef HAVE_FREERDP_GFXREDIR_H
			if (b->use_gfxredir) {
				/* scaling is done by client. */
				scaleFactorWidth = 1.0f;
				scaleFactorHeight = 1.0f;
			} else {
#else
			{
#endif /* HAVE_FREERDP_GFXREDIR_H */
				scaleFactorWidth = (float)surface->width /
						   content_buffer_width;
				scaleFactorHeight = (float)surface->height /
						    content_buffer_height;
			}

			if (rail_state->bufferWidth != copy_buffer_width ||
			    rail_state->bufferHeight != copy_buffer_height)
				isBufferSizeChanged = TRUE;

			if (isBufferSizeChanged || rail_state->forceRecreateSurface ||
			    (rail_state->surfaceBuffer == NULL && rail_state->surface_id == 0)) {

#ifdef HAVE_FREERDP_GFXREDIR_H
				if (b->use_gfxredir) {
					assert(rail_state->isUpdatePending == FALSE);

					if (rail_state->surfaceBuffer) {
						rdp_destroy_shared_buffer(surface);
						/* at window resize, reset name as old name might still be referenced by client */
						rail_state->shared_memory.name[0] = '\0';
					}
					assert(rail_state->surfaceBuffer == NULL);
					assert(rail_state->shared_memory.addr == NULL);
					rail_state->shared_memory.size = copy_buffer_size;
					if (rdp_allocate_shared_memory(b, &rail_state->shared_memory)) {
						uint32_t new_pool_id = 0;

						if (rdp_id_manager_allocate_id(&peer_ctx->poolId, surface, &new_pool_id)) {
							/* +1 for NULL terminate. */
							unsigned short section_name[RDP_SHARED_MEMORY_NAME_SIZE + 1];
							/* In Linux wchar_t is 4 types, but Windows wants 2 bytes wchar...
							 * convert to 2 bytes wchar_t.
							 */
							for (uint32_t i = 0; i < RDP_SHARED_MEMORY_NAME_SIZE; i++)
								section_name[i] = rail_state->shared_memory.name[i];
							section_name[RDP_SHARED_MEMORY_NAME_SIZE] = 0;

							GFXREDIR_OPEN_POOL_PDU open_pool = {};
							open_pool.poolId = new_pool_id;
							open_pool.poolSize = copy_buffer_size;
							open_pool.sectionNameLength = RDP_SHARED_MEMORY_NAME_SIZE + 1;
							open_pool.sectionName = section_name;

							if (redir_ctx->OpenPool(redir_ctx, &open_pool) == 0) {
								uint32_t new_buffer_id = 0;

								if (rdp_id_manager_allocate_id(&peer_ctx->bufferId, (void *)surface, &new_buffer_id)) {
									GFXREDIR_CREATE_BUFFER_PDU create_buffer = {};

									create_buffer.poolId = open_pool.poolId;
									create_buffer.bufferId = new_buffer_id;
									create_buffer.offset = 0;
									create_buffer.stride = copy_buffer_stride;
									create_buffer.width = copy_buffer_width;
									create_buffer.height = copy_buffer_height;
									create_buffer.format = GFXREDIR_BUFFER_PIXEL_FORMAT_ARGB_8888;
									if (redir_ctx->CreateBuffer(redir_ctx, &create_buffer) == 0) {
										rail_state->surfaceBuffer = rail_state->shared_memory.addr;
										rail_state->buffer_id = create_buffer.bufferId;
										rail_state->pool_id = open_pool.poolId;
										rail_state->bufferWidth = copy_buffer_width;
										rail_state->bufferHeight = copy_buffer_height;
									}
								}
							}
						}
						/* if failed, clean up */
						if (!rail_state->surfaceBuffer)
							rdp_destroy_shared_buffer(surface);
					}
				} else {
#else
				{
#endif /* HAVE_FREERDP_GFXREDIR_H */
					if (rdp_id_manager_allocate_id(&peer_ctx->surfaceId, surface, &new_surface_id)) {
						RdpgfxServerContext *gfx_ctx;

						gfx_ctx = peer_ctx->rail_grfx_server_context;
						RDPGFX_CREATE_SURFACE_PDU createSurface = {};
						/* create surface */
						rdp_debug_verbose(b,
								  "CreateSurface(surfaceId:0x%x - (%d, %d) size:%d for windowsId:0x%x)\n",
								  new_surface_id,
								  copy_buffer_width,
								  copy_buffer_height,
								  copy_buffer_size,
								  window_id);
						createSurface.surfaceId = (uint16_t)new_surface_id;
						createSurface.width = copy_buffer_width;
						createSurface.height = copy_buffer_height;
						/* regardless buffer as alpha or not, always use alpha to avoid mstsc bug */
						createSurface.pixelFormat = GFX_PIXEL_FORMAT_ARGB_8888;
						if (gfx_ctx->CreateSurface(gfx_ctx, &createSurface) == 0) {
							/* store new surface id */
							old_surface_id = rail_state->surface_id;
							rail_state->surface_id = new_surface_id;
							rail_state->bufferWidth = copy_buffer_width;
							rail_state->bufferHeight = copy_buffer_height;
						}
					}
				}
				rail_state->forceRecreateSurface = false;

				/* make entire content buffer damaged */
				damage_box.x1 = 0;
				damage_box.y1 = 0;
				damage_box.x2 = content_buffer_width;
				damage_box.y2 = content_buffer_height;
			} else if (damage_box.x2 > 0 && damage_box.y2 > 0) {
				/* scale damage using surface to buffer matrix */
				rdp_matrix_transform_position(&surface->surface_to_buffer_matrix,
							      &damage_box.x1,
							      &damage_box.y1);
				rdp_matrix_transform_position(&surface->surface_to_buffer_matrix,
							      &damage_box.x2, &damage_box.y2);
			}
			/* damage_box represents damaged area in contentBuffer */
			/* if it's not remoting window shadow, exclude the area from damage_box */
			if (is_window_shadow_remoting_disabled(peer_ctx) ||
				rail_state->isWindowSnapped) {
				if (damage_box.x1 < content_buffer_window_geometry.x)
					damage_box.x1 = content_buffer_window_geometry.x;
				if (damage_box.x2 > content_buffer_window_geometry.x + content_buffer_window_geometry.width)
					damage_box.x2 = content_buffer_window_geometry.x + content_buffer_window_geometry.width;
				if (damage_box.y1 < content_buffer_window_geometry.y)
					damage_box.y1 = content_buffer_window_geometry.y;
				if (damage_box.y2 > content_buffer_window_geometry.y + content_buffer_window_geometry.height)
					damage_box.y2 = content_buffer_window_geometry.y + content_buffer_window_geometry.height;
				damage_width = damage_box.x2 - damage_box.x1;
				damage_height = damage_box.y2 - damage_box.y1;
			} else {
				damage_width = damage_box.x2 - damage_box.x1;
				if (damage_width > content_buffer_width) {
					rdp_debug(b,
						  "damage_width (%d) is larger than content width(%d), clamp to avoid protocol error.\n",
						  damage_width,
						  content_buffer_width);
					damage_box.x1 = 0;
					damage_box.x2 = content_buffer_width;
					damage_width = content_buffer_width;
				}
				damage_height = damage_box.y2 - damage_box.y1;
				if (damage_height > content_buffer_height) {
					rdp_debug(b,
						  "damage_height (%d) is larger than content height(%d), clamp to avoid protocol error.\n",
						  damage_height,
						  content_buffer_height);
					damage_box.y1 = 0;
					damage_box.y2 = content_buffer_height;
					damage_height = content_buffer_height;
				}
			}
		} else {
			/* no content buffer bound, thus no damage */
			damage_width = 0;
			damage_height = 0;
		}
		/* Check to see if we have any content update to send to the new surface */
		if (damage_width > 0 && damage_height > 0) {
#ifdef HAVE_FREERDP_GFXREDIR_H
			if (b->use_gfxredir && rail_state->surfaceBuffer) {
				int copy_damage_x1 = (float)(damage_box.x1 - content_buffer_window_geometry.x) * scaleFactorWidth;
				int copy_damage_y1 = (float)(damage_box.y1 - content_buffer_window_geometry.y) * scaleFactorHeight;
				int copy_damage_width = (float)damage_width * scaleFactorWidth;
				int copy_damage_height = (float)damage_height * scaleFactorHeight;
				int copyStartOffset = copy_damage_x1 * bufferBpp + copy_damage_y1 * copy_buffer_stride;
				BYTE *copy_buffer_bits = (BYTE *)(rail_state->surfaceBuffer) + copyStartOffset;
				GfxRedirServerContext *redir_ctx;
				redir_ctx = peer_ctx->gfxredir_server_context;

				rdp_debug_verbose(b,
						  "copy source: x:%d, y:%d, width:%d, height:%d\n",
						  damage_box.x1, damage_box.y1,
						  damage_width, damage_height);
				rdp_debug_verbose(b,
						  "copy target: x:%d, y:%d, width:%d, height:%d, stride:%d\n",
						  copy_damage_x1, copy_damage_y1,
						  copy_damage_width,
						  copy_damage_height,
						  copy_buffer_stride);
				rdp_debug_verbose(b,
						  "copy scale: scaleFactorWidth:%5.3f, scaleFactorHeight:%5.3f\n",
						  scaleFactorWidth,
						  scaleFactorHeight);

				if (weston_surface_copy_content(surface,
								copy_buffer_bits,
								copy_buffer_size,
								copy_buffer_stride,
								copy_damage_width,
								copy_damage_height,
								damage_box.x1,
								damage_box.y1,
								damage_width,
								damage_height,
								false /* y-flip */,
								true /* is_argb */) < 0) {
					rdp_debug_error(b,
							"weston_surface_copy_content failed for windowId:0x%x, copyBuffer:%dx%d %d, damage:(%d,%d) %dx%d, content:%dx%d\n",
							window_id,
							copy_damage_width,
							copy_damage_height,
							copy_buffer_size,damage_box.x1,
							damage_box.y1,
							damage_width,
							damage_height,
							content_buffer_width,
							content_buffer_height);
					return -1;
				}

				GFXREDIR_PRESENT_BUFFER_PDU present_buffer = {};
				RECTANGLE_32 opaque_rect;

				/* specify opaque area */
				if (!hasAlpha) {
					opaque_rect.left = copy_damage_x1;
					opaque_rect.top = copy_damage_y1;
					opaque_rect.width = copy_damage_width;
					opaque_rect.height = copy_damage_height;
				}

				present_buffer.timestamp = 0; /* set 0 to disable A/V sync at client side */
				present_buffer.presentId = ++peer_ctx->currentFrameId;
				present_buffer.windowId = window_id;
				present_buffer.bufferId = rail_state->buffer_id;
				present_buffer.orientation = 0; /* 0, 90, 180 or 270 */
				present_buffer.targetWidth = newClientPos.width;
				present_buffer.targetHeight = newClientPos.height;
				present_buffer.dirtyRect.left = copy_damage_x1;
				present_buffer.dirtyRect.top = copy_damage_y1;
				present_buffer.dirtyRect.width = copy_damage_width;
				present_buffer.dirtyRect.height = copy_damage_height;
				if (!hasAlpha) {
					present_buffer.numOpaqueRects = 1;
					present_buffer.opaqueRects = &opaque_rect;
				} else {
					present_buffer.numOpaqueRects = 0;
					present_buffer.opaqueRects = NULL;
				}

				if (redir_ctx->PresentBuffer(redir_ctx, &present_buffer) == 0) {
					rail_state->isUpdatePending = TRUE;
					iter_data->isUpdatePending = TRUE;
				} else {
					rdp_debug_error(b,
							"PresentBuffer failed for windowId:0x%x\n",
							window_id);
				}
			} else
#endif /* HAVE_FREERDP_GFXREDIR_H */
			if (rail_state->surface_id) {
				RDPGFX_SURFACE_COMMAND surfaceCommand = {};
				int damageStride = damage_width * bufferBpp;
				int damageSize = damageStride * damage_height;
				BYTE *data = NULL;
				int alphaCodecHeaderSize = 4;
				BYTE *alpha = NULL;
				int alphaSize;
				RdpgfxServerContext *gfx_ctx = peer_ctx->rail_grfx_server_context;
				data = xmalloc(damageSize);

				if (hasAlpha)
					alphaSize = alphaCodecHeaderSize +
						    damage_width *
						    damage_height;
				else {
					/* 8 = max of ALPHA_RLE_SEGMENT for single alpha value. */
					alphaSize = alphaCodecHeaderSize + 8;
				}
				alpha = xmalloc(alphaSize);

				if (weston_surface_copy_content(surface,
								data, damageSize, 0, 0, 0,
								damage_box.x1, damage_box.y1, damage_width, damage_height,
								false /* y-flip */, true /* is_argb */) < 0) {
					rdp_debug_error(b,
							"weston_surface_copy_content failed for windowId:0x%x, damageSize:%d, damage:(%d,%d) %dx%d, content:%dx%d\n",
							window_id, damageSize,
							damage_box.x1,
							damage_box.y1,
							damage_width,
							damage_height,
							content_buffer_width,
							content_buffer_height);
					free(data);
					free(alpha);
					return -1;
				}

				/* generate alpha only bitmap */
				/* set up alpha codec header */
				alpha[0] = 'L';	/* signature */
				alpha[1] = 'A';	/* signature */
				alpha[2] = hasAlpha ? 0 : 1; /* compression: RDP spec indicate this is non-zero value for compressed, but it must be 1.*/
				alpha[3] = 0; /* compression */

				if (hasAlpha) {
					BYTE *alphaBits = &data[0];

					for (int i = 0; i < damage_height; i++, alphaBits+=damageStride) {
						BYTE *srcAlphaPixel = alphaBits + 3; /* 3 = xxxA. */
						BYTE *dstAlphaPixel = &alpha[alphaCodecHeaderSize + (i * damage_width)];

						for (int j = 0; j < damage_width; j++, srcAlphaPixel += bufferBpp, dstAlphaPixel++) {
							*dstAlphaPixel = *srcAlphaPixel;
						}
					}
				} else {
					/* whether buffer has alpha or not, always use alpha to avoid mstsc bug */
					/* CLEARCODEC_ALPHA_RLE_SEGMENT */
					int bitmapSize = damage_width * damage_height;

					alpha[alphaCodecHeaderSize] = 0xFF; /* alpha value (opaque) */
					if (bitmapSize < 0xFF) {
						alpha[alphaCodecHeaderSize + 1] = (BYTE)bitmapSize;
						alphaSize = alphaCodecHeaderSize + 2; /* alpha value + size in byte. */
					} else if (bitmapSize < 0xFFFF) {
						alpha[alphaCodecHeaderSize+1] = 0xFF;
						*(short*)&(alpha[alphaCodecHeaderSize+2]) = (short)bitmapSize;
						alphaSize = alphaCodecHeaderSize+4; /* alpha value + 1 + size in short. */
					} else {
						alpha[alphaCodecHeaderSize+1] = 0xFF;
						*(short*)&(alpha[alphaCodecHeaderSize+2]) = 0xFFFF;
						*(int*)&(alpha[alphaCodecHeaderSize+4]) = bitmapSize;
						alphaSize = alphaCodecHeaderSize+8; /* alpha value + 1 + 2 + size in int. */
					}
				}

				if (iter_data->needEndFrame == FALSE) {
					/* if frame is not started yet, send StartFrame first before sendng surface command. */
					RDPGFX_START_FRAME_PDU startFrame = {};
					startFrame.frameId = ++peer_ctx->currentFrameId;
					rdp_debug_verbose(b, "StartFrame(frameId:0x%x, windowId:0x%x)\n",
							  startFrame.frameId,
							  window_id);
					gfx_ctx->StartFrame(gfx_ctx,
							    &startFrame);
					iter_data->startedFrameId = startFrame.frameId;
					iter_data->needEndFrame = TRUE;
					iter_data->isUpdatePending = TRUE;
				}

				surfaceCommand.surfaceId = rail_state->surface_id;
				surfaceCommand.contextId = 0;
				surfaceCommand.format = PIXEL_FORMAT_BGRA32;
				surfaceCommand.left = damage_box.x1 - content_buffer_window_geometry.x;
				surfaceCommand.top = damage_box.y1 - content_buffer_window_geometry.y;
				surfaceCommand.right = damage_box.x2 - content_buffer_window_geometry.x;
				surfaceCommand.bottom = damage_box.y2 - content_buffer_window_geometry.y;
				surfaceCommand.width = damage_width;
				surfaceCommand.height = damage_height;
				surfaceCommand.extra = NULL;

				/* send alpha channel */
				surfaceCommand.codecId = RDPGFX_CODECID_ALPHA;
				surfaceCommand.length = alphaSize;
				surfaceCommand.data = &alpha[0];
				rdp_debug_verbose(b, "SurfaceCommand(frameId:0x%x, windowId:0x%x) for alpha\n",
						  iter_data->startedFrameId,
						  window_id);
				gfx_ctx->SurfaceCommand(gfx_ctx,
							&surfaceCommand);

				/* send bitmap data */
				surfaceCommand.codecId = RDPGFX_CODECID_UNCOMPRESSED;
				surfaceCommand.length = damageSize;
				surfaceCommand.data = &data[0];
				rdp_debug_verbose(b, "SurfaceCommand(frameId:0x%x, windowId:0x%x) for bitmap\n",
						  iter_data->startedFrameId,
						  window_id);
				gfx_ctx->SurfaceCommand(gfx_ctx, &surfaceCommand);

				free(data);
				free(alpha);
			}

			pixman_region32_clear(&rail_state->damage);

			/* TODO: this is a temporary workaround, some windows are not visible to shell
			   (such as subsurfaces, override_redirect), so z order update is 
			   not done by activate callback, thus trigger it at first update.
			   solution would make those surface visible to shell or hook signal on
			   when view_list is changed on libweston/compositor.c */
			if (!rail_state->isFirstUpdateDone) {
				peer_ctx->is_window_zorder_dirty = true;
				rail_state->isFirstUpdateDone = true;
			}
		}

#ifdef HAVE_FREERDP_GFXREDIR_H
		if (!b->use_gfxredir) {
#else
		{
#endif /* HAVE_FREERDP_GFXREDIR_H */
			RdpgfxServerContext *gfx_ctx = peer_ctx->rail_grfx_server_context;

			if (new_surface_id ||
			    rail_state->bufferScaleFactorWidth != scaleFactorWidth ||
			    rail_state->bufferScaleFactorHeight != scaleFactorHeight) {
				/* map surface to window */
				assert(new_surface_id == 0 || (new_surface_id == rail_state->surface_id));
				rdp_debug_verbose(b, "MapSurfaceToWindow(surfaceId:0x%x - windowsId:%x)\n",
						  rail_state->surface_id,
						  window_id);
				rdp_debug_verbose(b, "	targetWidth:0x%d - targetWidth:%d)\n",
						  newClientPos.width,
						  newClientPos.height);
				rdp_debug_verbose(b, "	mappedWidth:0x%d - mappedHeight:%d)\n",
						  content_buffer_width,
						  content_buffer_height);
				/* Always use scaled version to avoid bug in mstsc.exe, mstsc.exe
				 * seems can't handle mixed of scale and non-scaled version of procotols.
				 */
				RDPGFX_MAP_SURFACE_TO_SCALED_WINDOW_PDU mapSurfaceToScaledWindow = {
					.surfaceId = (uint16_t)rail_state->surface_id,
					.windowId = window_id,
					.mappedWidth = copy_buffer_width,
					.mappedHeight = copy_buffer_height,
					.targetWidth = client_buffer_width,
					.targetHeight = client_buffer_height,
				};

				gfx_ctx->MapSurfaceToScaledWindow(gfx_ctx,
								  &mapSurfaceToScaledWindow);
				rail_state->bufferScaleFactorWidth = scaleFactorWidth;
				rail_state->bufferScaleFactorHeight = scaleFactorHeight;
			}

			/* destroy old surface */
			if (old_surface_id) {
				RDPGFX_DELETE_SURFACE_PDU deleteSurface = {};

				rdp_debug_verbose(b, "DeleteSurface(surfaceId:0x%x for windowId:0x%x)\n",
						  old_surface_id, window_id);
				deleteSurface.surfaceId = (uint16_t)old_surface_id;
				gfx_ctx->DeleteSurface(gfx_ctx, &deleteSurface);
			}
		}
	}

	return 0;
}

static void
rdp_rail_update_window_iter(void *element, void *data)
{
	struct weston_surface *surface = element;
	struct weston_compositor *compositor = surface->compositor;
	struct rdp_backend *b = to_rdp_backend(compositor);
	struct update_window_iter_data *iter_data = data;
	struct weston_surface_rail_state *rail_state = surface->backend_state;

	/* this iter is looping from window hash table, thus it must have
	 * rail_state initialized.
	 **/
	assert(rail_state);

	if (!(surface->output_mask & (1u << iter_data->output_id)))
		return;

	if (rail_state->isCursor)
		rdp_rail_update_cursor(surface);
	else if (rail_state->isUpdatePending == FALSE)
		rdp_rail_update_window(surface, iter_data);
	else
		rdp_debug_verbose(b, "window update is skipped for windowId:0x%x, isUpdatePending = %d\n",
				  rail_state->window_id,
				  rail_state->isUpdatePending);
}

static uint32_t
rdp_insert_window_zorder_array(struct weston_view *view,
			       uint32_t *windowIdArray,
			       uint32_t WindowIdArraySize,
			       uint32_t iCurrent)
{
	struct weston_surface *surface = view->surface;
	struct weston_compositor *compositor = surface->compositor;
	struct rdp_backend *b = to_rdp_backend(compositor);
	struct weston_surface_rail_state *rail_state = surface->backend_state;
	struct weston_subsurface *sub;

	/* insert subsurface first to zorder list */
	wl_list_for_each(sub, &surface->subsurface_list, parent_link) {
		struct weston_view *sub_view;

		wl_list_for_each(sub_view, &sub->surface->views, surface_link) {
			if (sub_view->parent_view != view)
				continue;

			iCurrent = rdp_insert_window_zorder_array(sub_view,
								  windowIdArray,
								  WindowIdArraySize,
								  iCurrent);
			if (iCurrent == UINT_MAX)
				return iCurrent;
		}
	}

	/* insert itself as parent (which is below sub-surfaces in z order) */
	/* because z order is taken from compositor's scene-graph, it's possible
	   there is surface hasn't been associated with rail_state, so check it.
	   and if window is not remoted to client side, or minimized (or going to be
	   minimized), those won't included in z order list. */
	if (rail_state &&
	    rail_state->isWindowCreated &&
	    rail_state->showState != RDP_WINDOW_SHOW_MINIMIZED &&
	    rail_state->showState_requested != RDP_WINDOW_SHOW_MINIMIZED) {
		if (iCurrent >= WindowIdArraySize) {
			rdp_debug_error(b, "%s: more windows in tree than ID manager tracking (%d vs %d)\n",
					__func__, iCurrent, WindowIdArraySize);
			return UINT_MAX;
		}
		if (b->debugLevel >= RDP_DEBUG_LEVEL_VERBOSE) {
			char label[256];

			rdp_rail_dump_window_label(surface,
						   label,
						   sizeof(label));
			rdp_debug_verbose(b, "    window[%d]: %x: %s\n",
					  iCurrent,
					  rail_state->window_id, label);
		}
		windowIdArray[iCurrent++] = rail_state->window_id;
	}

	return iCurrent;
}

static void
rdp_rail_sync_window_zorder(struct weston_compositor *compositor)
{
	struct rdp_backend *b = to_rdp_backend(compositor);
	freerdp_peer* client = b->rdp_peer;
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	uint32_t numWindowId = 0;
	uint32_t *windowIdArray = NULL;
	WINDOW_ORDER_INFO window_order_info = {};
	MONITORED_DESKTOP_ORDER monitored_desktop_order = {};
	uint32_t iCurrent = 0;

	assert_compositor_thread(b);

	if (!b->enable_window_zorder_sync)
		return;

	numWindowId = peer_ctx->windowId.id_used;
	if (numWindowId == 0)
		return;
	/* +1 for marker window (aka proxy_surface) */
	numWindowId++;
	windowIdArray = xzalloc(numWindowId * sizeof(uint32_t));

	rdp_debug_verbose(b, "Dump Window Z order\n");
	/* walk windows in z-order */
	struct weston_layer *layer;

	wl_list_for_each(layer, &compositor->layer_list, link) {
		struct weston_view *view;

		wl_list_for_each(view, &layer->view_list.link, layer_link.link) {
			if (view->surface == b->proxy_surface) {
				rdp_debug_verbose(b, "    window[%d]: %x: %s\n",
						  iCurrent,
						  RDP_RAIL_MARKER_WINDOW_ID,
						  "marker window");
				windowIdArray[iCurrent++] = RDP_RAIL_MARKER_WINDOW_ID;
			} else {
				iCurrent = rdp_insert_window_zorder_array(view,
									  windowIdArray,
									  numWindowId,
									  iCurrent);
				if (iCurrent == UINT_MAX)
					goto Exit;
			}
		}
	}
	assert(iCurrent <= numWindowId);
	if (iCurrent > 0) {
		rdp_debug_verbose(b, "    send Window Z order: numWindowIds:%d\n",
				  iCurrent);

		window_order_info.fieldFlags = WINDOW_ORDER_TYPE_DESKTOP |
					       WINDOW_ORDER_FIELD_DESKTOP_ZORDER |
					       WINDOW_ORDER_FIELD_DESKTOP_ACTIVE_WND;
		monitored_desktop_order.activeWindowId = windowIdArray[0];
		monitored_desktop_order.numWindowIds = iCurrent;
		monitored_desktop_order.windowIds = windowIdArray;

		client->context->update->window->MonitoredDesktop(client->context,
								  &window_order_info,
								  &monitored_desktop_order);
		client->DrainOutputBuffer(client);
	}

Exit:
	free(windowIdArray);

	return;
}

void
rdp_rail_output_repaint(struct weston_output *output,
			pixman_region32_t *damage)
{
	struct weston_compositor *ec = output->compositor;
	struct rdp_backend *b = to_rdp_backend(ec);
	RdpPeerContext *peer_ctx = (RdpPeerContext *)b->rdp_peer->context;

	if (peer_ctx->isAcknowledgedSuspended ||
	    ((peer_ctx->currentFrameId - peer_ctx->acknowledgedFrameId) < 2)) {
		struct update_window_iter_data iter_data = {};

		/* notify window z order to client first,
		   mstsc/msrdc needs this to be sent before window update. */
		if (peer_ctx->is_window_zorder_dirty) {
			rdp_rail_sync_window_zorder(b->compositor);
			peer_ctx->is_window_zorder_dirty = false;
		}
		rdp_debug_verbose(b, "currentFrameId:0x%x, acknowledgedFrameId:0x%x, isAcknowledgedSuspended:%d\n",
				   peer_ctx->currentFrameId,
				   peer_ctx->acknowledgedFrameId,
				   peer_ctx->isAcknowledgedSuspended);

		iter_data.output_id = output->id;
		rdp_id_manager_for_each(&peer_ctx->windowId,
					rdp_rail_update_window_iter,
					&iter_data);
		if (iter_data.needEndFrame) {
			/* if frame is started at above iteration, send EndFrame here. */
			RDPGFX_END_FRAME_PDU endFrame = {};
			RdpgfxServerContext *gfx_ctx;
			gfx_ctx = peer_ctx->rail_grfx_server_context;

			endFrame.frameId = iter_data.startedFrameId;
			rdp_debug_verbose(b, "EndFrame(frameId:0x%x)\n", endFrame.frameId);
			gfx_ctx->EndFrame(gfx_ctx, &endFrame);
		}
		if (iter_data.isUpdatePending &&
		    b->enable_display_power_by_screenupdate) {
			/* By default, compositor won't update idle timer by screen activity,
			   thus, here manually call wake function to postpone idle timer when
			   RDP backend sends frame to client. */
			weston_compositor_wake(b->compositor);
		}
	} else {
		rdp_debug_verbose(b, "frame update is skipped. currentFrameId:%d, acknowledgedFrameId:%d, isAcknowledgedSuspended:%d\n",
				  peer_ctx->currentFrameId,
				  peer_ctx->acknowledgedFrameId,
				  peer_ctx->isAcknowledgedSuspended);
	}
	return;
}

static void
disp_force_recreate_iter(void *element, void *data)
{
	struct weston_surface *surface = element;
	struct weston_surface_rail_state *rail_state = surface->backend_state;

	rail_state->forceRecreateSurface = TRUE;
	rail_state->forceUpdateWindowState = TRUE;
}

struct disp_schedule_monitor_layout_change_data {
	struct rdp_loop_task _base;
	DispServerContext *context;
	int count;
	rdpMonitor *monitors;
};

static void
disp_monitor_layout_change_callback(bool freeOnly, void *dataIn)
{
	struct disp_schedule_monitor_layout_change_data *data = wl_container_of(dataIn, data, _base);
	DispServerContext *context = data->context;
	freerdp_peer *client = (freerdp_peer *)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	RDPGFX_RESET_GRAPHICS_PDU reset_graphics = {};
	MONITOR_DEF *reset_monitor_def = NULL;

	assert_compositor_thread(b);

	if (freeOnly)
		goto out;

	/* Skip reset graphics on failure */
	if (!handle_adjust_monitor_layout(client, data->count, data->monitors))
		goto out;

	reset_monitor_def = xmalloc(sizeof(MONITOR_DEF) * data->count);

	for (int i = 0; i < data->count; i++) {
		reset_monitor_def[i].left = data->monitors[i].x;
		reset_monitor_def[i].top = data->monitors[i].y;
		reset_monitor_def[i].right = data->monitors[i].width;
		reset_monitor_def[i].bottom = data->monitors[i].height;
		reset_monitor_def[i].flags = data->monitors[i].is_primary;
        }

	/* tell client the server updated the monitor layout */
	reset_graphics.width = peerCtx->desktop_width;
	reset_graphics.height = peerCtx->desktop_height;
	reset_graphics.monitorCount = data->count;
	reset_graphics.monitorDefArray = reset_monitor_def;
	peerCtx->rail_grfx_server_context->ResetGraphics(peerCtx->rail_grfx_server_context, &reset_graphics);

	/* force recreate all surface and redraw. */
	rdp_id_manager_for_each(&peerCtx->windowId, disp_force_recreate_iter, NULL);
	weston_compositor_damage_all(b->compositor);
out:
	free(reset_monitor_def);
	free(data);
	return;
}

static unsigned int
disp_client_monitor_layout_change(DispServerContext *context, const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU *display_control)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	rdpSettings *settings = client->context->settings;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct disp_schedule_monitor_layout_change_data *data;
	unsigned int i;

	assert_not_compositor_thread(b);

	rdp_debug(b, "Client: DisplayLayoutChange: monitor count:0x%x\n", display_control->NumMonitors);

	assert(settings->HiDefRemoteApp);

	data = xmalloc(sizeof(*data) + (sizeof(rdpMonitor) * display_control->NumMonitors));

	data->context = context;
	data->monitors = (rdpMonitor *)(data + 1);
	data->count = display_control->NumMonitors;
	for (i = 0; i < display_control->NumMonitors; i++) {
		DISPLAY_CONTROL_MONITOR_LAYOUT *ml = &display_control->Monitors[i];

		data->monitors[i].x = ml->Left;
		data->monitors[i].y = ml->Top;
		data->monitors[i].width = ml->Width;
		data->monitors[i].height = ml->Height;
		data->monitors[i].is_primary = !!(ml->Flags & DISPLAY_CONTROL_MONITOR_PRIMARY);
		data->monitors[i].attributes.physicalWidth = ml->PhysicalWidth;
		data->monitors[i].attributes.physicalHeight = ml->PhysicalHeight;
		data->monitors[i].attributes.orientation = ml->Orientation;
		data->monitors[i].attributes.desktopScaleFactor = ml->DesktopScaleFactor;
		data->monitors[i].attributes.deviceScaleFactor = ml->DeviceScaleFactor;
		data->monitors[i].orig_screen = 0;
	}

	rdp_dispatch_task_to_display_loop(peerCtx, disp_monitor_layout_change_callback, &data->_base);

	return CHANNEL_RC_OK;
}

static void
rdp_rail_idle_handler(struct wl_listener *listener, void *data)
{
	RAIL_POWER_DISPLAY_REQUEST displayRequest;
	RdpPeerContext *peer_ctx = container_of(listener, RdpPeerContext,
					       idle_listener);
	struct rdp_backend *b = peer_ctx->rdpBackend;
	RailServerContext *rail_ctx = peer_ctx->rail_server_context;

	assert_compositor_thread(b);

	rdp_debug(b, "%s is called on peer_ctx:%p\n", __func__, peer_ctx);

	if (peer_ctx->clientStatusFlags & TS_RAIL_CLIENTSTATUS_POWER_DISPLAY_REQUEST_SUPPORTED) {
		displayRequest.active = FALSE;
		rail_ctx->ServerPowerDisplayRequest(rail_ctx, &displayRequest);
	}
}

static void
rdp_rail_wake_handler(struct wl_listener *listener, void *data)
{
	RAIL_POWER_DISPLAY_REQUEST displayRequest;
	RdpPeerContext *peer_ctx = container_of(listener, RdpPeerContext,
					       wake_listener);
	struct rdp_backend *b = peer_ctx->rdpBackend;
	RailServerContext *rail_ctx = peer_ctx->rail_server_context;

	assert_compositor_thread(b);

	rdp_debug(b, "%s is called on peer_ctx:%p\n", __func__, peer_ctx);

	if (peer_ctx->clientStatusFlags & TS_RAIL_CLIENTSTATUS_POWER_DISPLAY_REQUEST_SUPPORTED) {
		displayRequest.active = TRUE;
		rail_ctx->ServerPowerDisplayRequest(rail_ctx, &displayRequest);
	}
}

bool
rdp_rail_peer_activate(freerdp_peer* client)
{
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;
	rdpSettings *settings = client->context->settings;
	RailServerContext *rail_ctx = NULL;
	RdpgfxServerContext *gfx_ctx = NULL;
	DispServerContext *disp_ctx = NULL;
	bool rail_server_started = false;
	bool disp_server_opened = false;
	bool rail_grfx_server_opened = false;
#ifdef HAVE_FREERDP_GFXREDIR_H
	GfxRedirServerContext *redir_ctx = NULL;
	bool gfxredir_server_opened = false;
#endif /* HAVE_FREERDP_GFXREDIR_H */
#ifdef HAVE_FREERDP_RDPAPPLIST_H
	RdpAppListServerContext *applist_ctx = NULL;
	bool applist_server_opened = false;
	RDPAPPLIST_SERVER_CAPS_PDU app_list_caps = {};
#endif /* HAVE_FREERDP_RDPAPPLIST_H */
	uint waitRetry;

	assert_compositor_thread(b);

	/* In RAIL mode, client must not be resized */
	assert(b->no_clients_resize == 0);
	/* Server must not ask client to resize */
	settings->DesktopResize = FALSE;

	/* HiDef requires graphics pipeline to be supported */
	if (settings->SupportGraphicsPipeline == FALSE) {
		if (settings->HiDefRemoteApp) {
			rdp_debug_error(b, "HiDef remoting is going to be disabled because client doesn't support graphics pipeline\n");
			settings->HiDefRemoteApp = FALSE;
		}
	}

	/* Start RAIL server */
	rail_ctx = rail_server_context_new(peer_ctx->vcm);
	if (!rail_ctx)
		goto error_exit;
	peer_ctx->rail_server_context = rail_ctx;
	rail_ctx->custom = client;
	rail_ctx->ClientHandshake = rail_client_Handshake;
	rail_ctx->ClientClientStatus = rail_client_ClientStatus;
	rail_ctx->ClientExec = rail_client_Exec;
	rail_ctx->ClientActivate = rail_client_Activate;
	rail_ctx->ClientSyscommand = rail_client_Syscommand;
	rail_ctx->ClientSysmenu = rail_client_Sysmenu;
	rail_ctx->ClientSysparam = rail_client_ClientSysparam;
	rail_ctx->ClientGetAppidReq = rail_client_ClientGetAppidReq;
	rail_ctx->ClientWindowMove = rail_client_WindowMove;
	rail_ctx->ClientSnapArrange = rail_client_SnapArrange;
	rail_ctx->ClientLangbarInfo = rail_client_LangbarInfo;
	rail_ctx->ClientLanguageImeInfo = rail_client_LanguageImeInfo;
	rail_ctx->ClientCompartmentInfo = rail_client_CompartmentInfo;
	if (rail_ctx->Start(rail_ctx) != CHANNEL_RC_OK)
		goto error_exit;
	rail_server_started = true;

	/* send handshake to client */
	if (settings->RemoteApplicationSupportLevel & RAIL_LEVEL_HANDSHAKE_EX_SUPPORTED) {
		RAIL_HANDSHAKE_EX_ORDER handshakeEx = {};
		uint32_t railHandshakeFlags = TS_RAIL_ORDER_HANDSHAKEEX_FLAGS_HIDEF |
					      TS_RAIL_ORDER_HANDSHAKE_EX_FLAGS_EXTENDED_SPI_SUPPORTED;

		if (b->enable_window_snap_arrange)
			railHandshakeFlags |= TS_RAIL_ORDER_HANDSHAKE_EX_FLAGS_SNAP_ARRANGE_SUPPORTED;
		handshakeEx.buildNumber = 0;
		handshakeEx.railHandshakeFlags = railHandshakeFlags;
		if (rail_ctx->ServerHandshakeEx(rail_ctx, &handshakeEx) != CHANNEL_RC_OK)
			goto error_exit;
		client->DrainOutputBuffer(client);
	} else {
		RAIL_HANDSHAKE_ORDER handshake = {};

		handshake.buildNumber = 0;
		if (rail_ctx->ServerHandshake(rail_ctx, &handshake) != CHANNEL_RC_OK)
			goto error_exit;
		client->DrainOutputBuffer(client);
	}

	/* wait handshake reponse from client */
	waitRetry = 0;
	while (!peer_ctx->handshakeCompleted) {
		if (++waitRetry > 10000) /* timeout after 100 sec. */
			goto error_exit;
		usleep(10000); /* wait 0.01 sec. */
		client->CheckFileDescriptor(client);
		WTSVirtualChannelManagerCheckFileDescriptor(peer_ctx->vcm);
	}

	/* open Disp channel */
	disp_ctx = disp_server_context_new(peer_ctx->vcm);
	if (!disp_ctx)
		goto error_exit;
	peer_ctx->disp_server_context = disp_ctx;
	disp_ctx->custom = client;
	disp_ctx->MaxNumMonitors = RDP_MAX_MONITOR;
	disp_ctx->MaxMonitorAreaFactorA = DISPLAY_CONTROL_MAX_MONITOR_WIDTH;
	disp_ctx->MaxMonitorAreaFactorB = DISPLAY_CONTROL_MAX_MONITOR_HEIGHT;
	disp_ctx->DispMonitorLayout = disp_client_monitor_layout_change;
	if (disp_ctx->Open(disp_ctx) != CHANNEL_RC_OK)
		goto error_exit;
	disp_server_opened = TRUE;
	if (disp_ctx->DisplayControlCaps(disp_ctx) != CHANNEL_RC_OK)
		goto error_exit;

	/* open HiDef (aka rdpgfx) channel. */
	gfx_ctx = rdpgfx_server_context_new(peer_ctx->vcm);
	if (!gfx_ctx)
		goto error_exit;
	peer_ctx->rail_grfx_server_context = gfx_ctx;
	gfx_ctx->custom = client;
	gfx_ctx->CapsAdvertise = rail_grfx_client_caps_advertise;
	gfx_ctx->CacheImportOffer = rail_grfx_client_cache_import_offer;
	gfx_ctx->FrameAcknowledge = rail_grfx_client_frame_acknowledge;
	if (!gfx_ctx->Open(gfx_ctx))
		goto error_exit;
	rail_grfx_server_opened = TRUE;

#ifdef HAVE_FREERDP_GFXREDIR_H
	/* open Graphics Redirection channel. */
	if (b->use_gfxredir) {

		redir_ctx = b->gfxredir_server_context_new(peer_ctx->vcm);
		if (!redir_ctx)
			goto error_exit;
		peer_ctx->gfxredir_server_context = redir_ctx;
		redir_ctx->custom = client;
		redir_ctx->GraphicsRedirectionLegacyCaps = gfxredir_client_graphics_redirection_legacy_caps;
		redir_ctx->GraphicsRedirectionCapsAdvertise = gfxredir_client_graphics_redirection_caps_advertise;
		redir_ctx->PresentBufferAck = gfxredir_client_present_buffer_ack;
		if (redir_ctx->Open(redir_ctx) != CHANNEL_RC_OK)
			goto error_exit;
		gfxredir_server_opened = TRUE;
	}
#endif /* HAVE_FREERDP_GFXREDIR_H */

#ifdef HAVE_FREERDP_RDPAPPLIST_H
	/* open Application List channel. */
	if (b->rdprail_shell_name && b->use_rdpapplist) {
		applist_ctx = b->rdpapplist_server_context_new(peer_ctx->vcm);
		if (!applist_ctx)
			goto error_exit;
		peer_ctx->applist_server_context = applist_ctx;
		applist_ctx->custom = client;
		applist_ctx->ApplicationListClientCaps = applist_client_Caps;
		if (applist_ctx->Open(applist_ctx) != CHANNEL_RC_OK)
			goto error_exit;
		applist_server_opened = TRUE;

		rdp_debug(b, "Server AppList caps version:%d\n", RDPAPPLIST_CHANNEL_VERSION);
		app_list_caps.version = RDPAPPLIST_CHANNEL_VERSION;
		rdp_debug(b, "    appListProviderName:%s\n", b->rdprail_shell_name);
		if (!utf8_string_to_rail_string(b->rdprail_shell_name,
						&app_list_caps.appListProviderName))
			goto error_exit;
#if RDPAPPLIST_CHANNEL_VERSION >= 4
		/* assign unique id */
		char *s = getenv("WSLG_SERVICE_ID");
		if (!s)
			s = b->rdprail_shell_name;
		rdp_debug(b, "    appListProviderUniqueId:%s\n", s);
		if (!utf8_string_to_rail_string(s,
						&app_list_caps.appListProviderUniqueId))
			goto error_exit;
#endif /* RDPAPPLIST_CHANNEL_VERSION >= 4 */
		if (applist_ctx->ApplicationListCaps(applist_ctx, &app_list_caps) != CHANNEL_RC_OK)
			goto error_exit;
		free(app_list_caps.appListProviderName.string);
	}
#endif /* HAVE_FREERDP_RDPAPPLIST_H */

	/* wait graphics channel (and optionally graphics redir channel) reponse from client */
	waitRetry = 0;
	while (!peer_ctx->activationGraphicsCompleted
#ifdef HAVE_FREERDP_GFXREDIR_H
		|| (gfxredir_server_opened && !peer_ctx->activationGraphicsRedirectionCompleted)
#endif /* HAVE_FREERDP_GFXREDIR_H */
		) {
		if (++waitRetry > 10000) /* timeout after 100 sec. */
			goto error_exit;
		usleep(10000); /* wait 0.01 sec. */
		client->CheckFileDescriptor(client);
		WTSVirtualChannelManagerCheckFileDescriptor(peer_ctx->vcm);
	}

	/* subscribe idle/wake signal from compositor */
	peer_ctx->idle_listener.notify = rdp_rail_idle_handler;
	wl_signal_add(&b->compositor->idle_signal, &peer_ctx->idle_listener);
	peer_ctx->wake_listener.notify = rdp_rail_wake_handler;
	wl_signal_add(&b->compositor->wake_signal, &peer_ctx->wake_listener);

	return TRUE;

error_exit:

#ifdef HAVE_FREERDP_RDPAPPLIST_H
	if (applist_server_opened) {
		applist_ctx->Close(applist_ctx);
		free(app_list_caps.appListProviderName.string);
	}
	if (applist_ctx) {
		assert(b->rdpapplist_server_context_free);
		b->rdpapplist_server_context_free(applist_ctx);
		peer_ctx->applist_server_context = NULL;
	}
#endif /* HAVE_FREERDP_RDPAPPLIST_H */

#ifdef HAVE_FREERDP_GFXREDIR_H
	if (gfxredir_server_opened)
		redir_ctx->Close(redir_ctx);
	if (redir_ctx) {
		assert(b->gfxredir_server_context_free);
		b->gfxredir_server_context_free(redir_ctx);
		peer_ctx->gfxredir_server_context = NULL;
		peer_ctx->activationGraphicsRedirectionCompleted = FALSE;
	}
#endif /* HAVE_FREERDP_GFXREDIR_H */

	if (rail_grfx_server_opened)
		gfx_ctx->Close(gfx_ctx);
	if (gfx_ctx) {
		rdpgfx_server_context_free(gfx_ctx);
		peer_ctx->rail_grfx_server_context = NULL;
		peer_ctx->activationGraphicsCompleted = FALSE;
	}

	if (disp_server_opened)
		disp_ctx->Close(disp_ctx);
	if (disp_ctx) {
		disp_server_context_free(disp_ctx);
		peer_ctx->disp_server_context = NULL;
	}

	if (rail_server_started)
		rail_ctx->Stop(rail_ctx);
	if (rail_ctx) {
		rail_server_context_free(rail_ctx);
		peer_ctx->rail_server_context = NULL;
	}

	return FALSE;
}

static void
rdp_rail_notify_window_proxy_surface(struct weston_surface *proxy_surface)
{
	struct rdp_backend *b = to_rdp_backend(proxy_surface->compositor);

	assert_compositor_thread(b);

	b->proxy_surface = proxy_surface;
}

static void
rdp_rail_notify_window_zorder_change(struct weston_compositor *compositor)
{
	struct rdp_backend *b = to_rdp_backend(compositor);
	RdpPeerContext *peer_ctx = (RdpPeerContext *)b->rdp_peer->context;

	assert_compositor_thread(b);

	/* z order will be sent to client at next repaint */
	peer_ctx->is_window_zorder_dirty = true;
}

void
rdp_rail_sync_window_status(freerdp_peer *client)
{
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;
	const struct weston_rdprail_shell_api *api = b->rdprail_shell_api;
	RailServerContext *rail_ctx = peer_ctx->rail_server_context;
	rdpUpdate *update = b->rdp_peer->context->update;
	struct weston_view *view;
	bool anyWindowCreated = false;

	assert_compositor_thread(b);

	{
		RAIL_SYSPARAM_ORDER sysParamOrder = {
			.param = SPI_SETSCREENSAVESECURE,
			.setScreenSaveSecure = 0,
		};
		rail_ctx->ServerSysparam(rail_ctx, &sysParamOrder);
		client->DrainOutputBuffer(client);
	}

	{
		RAIL_SYSPARAM_ORDER sysParamOrder = {
			.param = SPI_SETSCREENSAVEACTIVE,
			.setScreenSaveActive = 0,
		};
		rail_ctx->ServerSysparam(rail_ctx, &sysParamOrder);
		client->DrainOutputBuffer(client);
	}

	{
		RAIL_ZORDER_SYNC zOrderSync = {
			.windowIdMarker = RDP_RAIL_MARKER_WINDOW_ID,
		};
		rail_ctx->ServerZOrderSync(rail_ctx, &zOrderSync);
		client->DrainOutputBuffer(client);
	}

	{
		WINDOW_ORDER_INFO window_order_info = {
			.windowId = RDP_RAIL_MARKER_WINDOW_ID,
			.fieldFlags = WINDOW_ORDER_TYPE_DESKTOP |
				      WINDOW_ORDER_FIELD_DESKTOP_HOOKED |
				      WINDOW_ORDER_FIELD_DESKTOP_ARC_BEGAN,
		};
		MONITORED_DESKTOP_ORDER monitored_desktop_order = {};

		update->window->MonitoredDesktop(update->context,
						 &window_order_info,
						 &monitored_desktop_order);
		client->DrainOutputBuffer(client);
	}

	{
		uint32_t windowsIdArray[1] = {};
		WINDOW_ORDER_INFO window_order_info = {
			.windowId = RDP_RAIL_MARKER_WINDOW_ID,
			.fieldFlags = WINDOW_ORDER_TYPE_DESKTOP |
				      WINDOW_ORDER_FIELD_DESKTOP_ZORDER |
				      WINDOW_ORDER_FIELD_DESKTOP_ACTIVE_WND,
		};
		windowsIdArray[0] = RDP_RAIL_MARKER_WINDOW_ID;
		MONITORED_DESKTOP_ORDER monitored_desktop_order = {
			.activeWindowId = RDP_RAIL_DESKTOP_WINDOW_ID,
			.numWindowIds = 1,
			.windowIds = (UINT *)&windowsIdArray,
		};

		update->window->MonitoredDesktop(update->context,
						 &window_order_info,
						 &monitored_desktop_order);
		client->DrainOutputBuffer(client);
	}

	{
		WINDOW_ORDER_INFO window_order_info = {
			.windowId = RDP_RAIL_MARKER_WINDOW_ID,
			.fieldFlags = WINDOW_ORDER_TYPE_DESKTOP |
				      WINDOW_ORDER_FIELD_DESKTOP_ARC_COMPLETED,
		};
		MONITORED_DESKTOP_ORDER monitored_desktop_order = {};

		update->window->MonitoredDesktop(update->context,
						 &window_order_info,
						 &monitored_desktop_order);
		client->DrainOutputBuffer(client);
	}

	peer_ctx->activationRailCompleted = true;

	wl_list_for_each_reverse(view, &b->compositor->view_list, link) {
		struct weston_surface *surface = view->surface;
		struct weston_subsurface *sub;
		struct weston_surface_rail_state *rail_state = surface->backend_state;

		if (!rail_state || rail_state->window_id == 0) {
			rdp_rail_create_window(NULL, surface);
			rail_state = surface->backend_state;
			if (rail_state && rail_state->window_id) {
				if (api && api->request_window_icon)
					api->request_window_icon(surface);
				wl_list_for_each_reverse(sub, &surface->subsurface_list, parent_link) {
					struct weston_surface_rail_state *sub_rail_state = sub->surface->backend_state;
					if (sub->surface == surface)
						continue;
					if (!sub_rail_state || sub_rail_state->window_id == 0)
						rdp_rail_create_window(NULL, sub->surface);
				}
			}
			anyWindowCreated = true;
		}
	}

	if (anyWindowCreated) {
		/* resync window zorder with RDP client */
		peer_ctx->is_window_zorder_dirty = true;
		/* this assume repaint to be scheduled on idle loop, not directly from here */
		weston_compositor_wake(b->compositor);
		weston_compositor_damage_all(b->compositor);
	}
}

static void
rdp_rail_send_window_minmax_info(
	struct weston_surface* surface,
	struct weston_rdp_rail_window_pos* maxPosSize,
	struct weston_size* minTrackSize,
	struct weston_size* maxTrackSize)
{
	struct weston_compositor *compositor = surface->compositor;
	struct weston_surface_rail_state *rail_state = surface->backend_state;
	struct rdp_backend *b = to_rdp_backend(compositor);
	struct weston_output *output = rdp_output_get_primary(compositor);
	RdpPeerContext *peer_ctx;
	RailServerContext *rail_ctx;
	RAIL_MINMAXINFO_ORDER minmax_order;
	int dummyX = 0, dummyY = 0;

	if (!b->rdp_peer || !b->rdp_peer->context->settings->HiDefRemoteApp) {
		return;
	}

	peer_ctx = (RdpPeerContext *)b->rdp_peer->context;

	/* apply global to output transform, and translate to client coordinate */
	/* minmax info is based on primary monitor */
	/* https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-minmaxinfo */
	if (output) {
		to_client_coordinate(peer_ctx, output,
				     &maxPosSize->x, &maxPosSize->y,
				     &maxPosSize->width, &maxPosSize->height);
		to_client_coordinate(peer_ctx, output,
				     &dummyX, &dummyY,
				     &minTrackSize->width, &minTrackSize->height);
		to_client_coordinate(peer_ctx, output,
				     &dummyX, &dummyY,
				     &maxTrackSize->width, &maxTrackSize->height);
	}

	/* Inform the RDP client about the minimum/maximum width and height allowed
	 * on this window.
	 */
	minmax_order.windowId = rail_state->window_id;
	minmax_order.maxPosX = maxPosSize->x;
	minmax_order.maxPosY = maxPosSize->y;
	minmax_order.maxWidth = maxPosSize->width;
	minmax_order.maxHeight = maxPosSize->height;
	minmax_order.minTrackWidth = minTrackSize->width;
	minmax_order.minTrackHeight = minTrackSize->height;
	minmax_order.maxTrackWidth = maxTrackSize->width;
	minmax_order.maxTrackHeight = maxTrackSize->height;

	rdp_debug_verbose(b,
		  "Minmax order: maxPosX:%d, maxPosY:%d, maxWidth:%d, maxHeight:%d\n",
		  minmax_order.maxPosX,
		  minmax_order.maxPosY,
		  minmax_order.maxWidth,
		  minmax_order.maxHeight);
	rdp_debug_verbose(b,
		  "Minmax order: minTrackWidth:%d, minTrackHeight:%d, maxTrackWidth:%d, maxTrackHeight:%d\n",
		  minmax_order.minTrackWidth,
		  minmax_order.minTrackHeight,
		  minmax_order.maxTrackWidth,
		  minmax_order.maxTrackHeight);

	rail_ctx = peer_ctx->rail_server_context;
	rail_ctx->ServerMinMaxInfo(rail_ctx, &minmax_order);
}

static void
rdp_rail_start_window_move(
	struct weston_surface* surface,
	int pointerGrabX,
	int pointerGrabY)
{
	struct weston_compositor *compositor = surface->compositor;
	struct weston_surface_rail_state *rail_state = surface->backend_state;
	struct weston_geometry geometry = {
		.x = 0,
		.y = 0,
		.width = surface->width,
		.height = surface->height,
	};
	struct rdp_backend *b = to_rdp_backend(compositor);
	const struct weston_rdprail_shell_api *api = b->rdprail_shell_api;
	RdpPeerContext *peer_ctx;
	RAIL_LOCALMOVESIZE_ORDER move_order;
	int posX = 0, posY = 0;
	int numViews = 0;
	struct weston_view* view;
	RailServerContext *rail_ctx;

	if (!b->rdp_peer || !b->rdp_peer->context->settings->HiDefRemoteApp)
		return;

	peer_ctx = (RdpPeerContext *)b->rdp_peer->context;

	assert_compositor_thread(b);
	assert(rail_state);

	wl_list_for_each(view, &surface->views, surface_link) {
		numViews++;
		posX = view->geometry.x;
		posY = view->geometry.y;
		break;
	}
	if (numViews == 0) {
		view = NULL;
		rdp_debug_verbose(b,
				  "%s: surface has no view (windowId:0x%x)\n",
				  __func__, rail_state->window_id);
	}

	if (is_window_shadow_remoting_disabled(peer_ctx)) {
		/* offset window shadow area */
		api->get_window_geometry(surface, &geometry);
		posX += geometry.x;
		posY += geometry.y;
	}

	/* apply global to output transform, and translate to client coordinate */
	if (surface->output) {
		to_client_coordinate(peer_ctx, surface->output,
				     &posX, &posY, NULL, NULL);
		to_client_coordinate(peer_ctx, surface->output,
				     &pointerGrabX, &pointerGrabY, NULL, NULL);
	}

	rdp_debug(b, "============== StartWindowMove ==============\n");
	rdp_debug(b, "WindowsPosition: Pre-move (%d,%d) at client.\n", posX, posY);
	rdp_debug(b, "pointerGrab: (%d,%d)\n", pointerGrabX, pointerGrabY);

	/* Start the local Window move.
	 */
	move_order.windowId = rail_state->window_id;
	move_order.isMoveSizeStart = true;
	move_order.moveSizeType = RAIL_WMSZ_MOVE;
	move_order.posX = pointerGrabX - posX;
	move_order.posY = pointerGrabY - posY;

	rdp_debug(b,
		  "Move order: windowId:0x%x, isMoveSizeStart:%d, moveType:%d, pos:(%d,%d)\n",
		  move_order.windowId,
		  move_order.isMoveSizeStart,
		  move_order.moveSizeType,
		  move_order.posX,
		  move_order.posY);

	rail_ctx = peer_ctx->rail_server_context;
	rail_ctx->ServerLocalMoveSize(rail_ctx, &move_order);

	rdp_debug(b, "============== StartWindowMove ==============\n");
}

static void
rdp_rail_end_window_move(struct weston_surface *surface)
{
	struct weston_compositor *compositor = surface->compositor;
	struct weston_surface_rail_state *rail_state = surface->backend_state;
	struct weston_geometry geometry = {
		.x = 0,
		.y = 0,
		.width = surface->width,
		.height = surface->height,
	};
	struct rdp_backend *b = to_rdp_backend(compositor);
	const struct weston_rdprail_shell_api *api = b->rdprail_shell_api;
	RdpPeerContext *peer_ctx = NULL;
	RailServerContext *rail_ctx;
	RAIL_LOCALMOVESIZE_ORDER move_order;
	int posX = 0, posY = 0;
	int numViews = 0;
	struct weston_view *view;

	if (!b->rdp_peer || !b->rdp_peer->context->settings->HiDefRemoteApp) {
		return;
	}

	assert_compositor_thread(b);
	assert(rail_state);

	peer_ctx = (RdpPeerContext *)b->rdp_peer->context;

	wl_list_for_each(view, &surface->views, surface_link) {
		numViews++;
		posX = view->geometry.x;
		posY = view->geometry.y;
		break;
	}
	if (numViews == 0) {
		view = NULL;
		rdp_debug_verbose(b,
				  "%s: surface has no view (windowId:0x%x)\n",
				  __func__, rail_state->window_id);
	}

	if (is_window_shadow_remoting_disabled(peer_ctx)) {
		/* offset window shadow area */
		api->get_window_geometry(surface, &geometry);
		posX += geometry.x;
		posY += geometry.y;
	}

	/* apply global to output transform, and translate to client coordinate */
	if (surface->output)
		to_client_coordinate(peer_ctx, surface->output,
				     &posX, &posY, NULL, NULL);

	rdp_debug(b, "=============== EndWindowMove ===============\n");
	rdp_debug(b, "WindowsPosition: Post-move (%d,%d) at client.\n", posX, posY);

	move_order.windowId = rail_state->window_id;
	move_order.isMoveSizeStart = false;
	move_order.moveSizeType = RAIL_WMSZ_MOVE;
	move_order.posX = posX;
	move_order.posY = posY;

	rdp_debug(b, "Move order: windowId:0x%x, isMoveSizeStart:%d, moveType:%d, pos:(%d,%d)\n",
		  move_order.windowId,
		  move_order.isMoveSizeStart,
		  move_order.moveSizeType,
		  move_order.posX,
		  move_order.posY);

	rail_ctx = peer_ctx->rail_server_context;
	rail_ctx->ServerLocalMoveSize(rail_ctx, &move_order);

	rdp_debug(b, "=============== EndWindowMove ===============\n");
}

static void
rdp_rail_destroy_window_iter(void *element, void *data)
{
	struct weston_surface *surface = element;

	rdp_rail_destroy_window(NULL, surface);
}

void
rdp_rail_peer_context_free(freerdp_peer *client, RdpPeerContext *context)
{
	RailServerContext *rail_ctx;
	RdpgfxServerContext *gfx_ctx;
	DispServerContext *disp_ctx;

	rail_ctx = context->rail_server_context;
	gfx_ctx = context->rail_grfx_server_context;
	disp_ctx = context->disp_server_context;

	rdp_id_manager_for_each(&context->windowId,
				rdp_rail_destroy_window_iter,
				NULL);

#ifdef HAVE_FREERDP_RDPAPPLIST_H
	if (context->applist_server_context) {
		struct rdp_backend *b = context->rdpBackend;
		if (context->isAppListEnabled)
			context->rdpBackend->rdprail_shell_api->stop_app_list_update(context->rdpBackend->rdprail_shell_context);
		context->applist_server_context->Close(context->applist_server_context);
		assert(b->rdpapplist_server_context_free);
		b->rdpapplist_server_context_free(context->applist_server_context);
	}
#endif /* HAVE_FREERDP_RDPAPPLIST_H */

#ifdef HAVE_FREERDP_GFXREDIR_H
	if (context->gfxredir_server_context) {
		struct rdp_backend *b = context->rdpBackend;
		GfxRedirServerContext *redir_ctx;
		redir_ctx = context->gfxredir_server_context;

		redir_ctx->Close(redir_ctx);
		assert(b->gfxredir_server_context_free);
		b->gfxredir_server_context_free(redir_ctx);
	}
#endif /* HAVE_FREERDP_GFXREDIR_H */

	if (gfx_ctx) {
		gfx_ctx->Close(gfx_ctx);
		rdpgfx_server_context_free(gfx_ctx);
	}

	if (disp_ctx) {
		disp_ctx->Close(disp_ctx);
		disp_server_context_free(disp_ctx);
	}

	if (rail_ctx) {
		rail_ctx->Stop(rail_ctx);
		rail_server_context_free(rail_ctx);
	}

	if (context->clientExec_destroy_listener.notify) {
		wl_list_remove(&context->clientExec_destroy_listener.link);
		context->clientExec_destroy_listener.notify = NULL;
	}

	if (context->idle_listener.notify) {
		wl_list_remove(&context->idle_listener.link);
		context->idle_listener.notify = NULL;
	}

	if (context->wake_listener.notify) {
		wl_list_remove(&context->wake_listener.link);
		context->wake_listener.notify = NULL;
	}

#ifdef HAVE_FREERDP_GFXREDIR_H
	rdp_id_manager_free(&context->bufferId);
	rdp_id_manager_free(&context->poolId);
#endif /* HAVE_FREERDP_GFXREDIR_H */
	rdp_id_manager_free(&context->surfaceId);
	rdp_id_manager_free(&context->windowId);
}

bool
rdp_drdynvc_init(freerdp_peer *client)
{
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	DrdynvcServerContext *vc_ctx;

	assert_compositor_thread(peer_ctx->rdpBackend);

	/* Open Dynamic virtual channel */
	vc_ctx = drdynvc_server_context_new(peer_ctx->vcm);

	peer_ctx->drdynvc_server_context = drdynvc_server_context_new(peer_ctx->vcm);
	if (!vc_ctx)
		return false;
	if (vc_ctx->Start(vc_ctx) != CHANNEL_RC_OK) {
		drdynvc_server_context_free(vc_ctx);
		return false;
	}
	peer_ctx->drdynvc_server_context = vc_ctx;

	/* Force Dynamic virtual channel to exchange caps */
	if (WTSVirtualChannelManagerGetDrdynvcState(peer_ctx->vcm) == DRDYNVC_STATE_NONE) {
		int waitRetry = 0;

		client->activated = TRUE;
		/* Wait reply to arrive from client */
		while (WTSVirtualChannelManagerGetDrdynvcState(peer_ctx->vcm) != DRDYNVC_STATE_READY) {
			if (++waitRetry > 10000) { /* timeout after 100 sec. */
				rdp_drdynvc_destroy(peer_ctx);
				return FALSE;
			}
			usleep(10000); /* wait 0.01 sec. */
			client->CheckFileDescriptor(client);
			WTSVirtualChannelManagerCheckFileDescriptor(peer_ctx->vcm);
		}
	}

	return true;
}

void
rdp_drdynvc_destroy(RdpPeerContext *context)
{
	DrdynvcServerContext *vc_ctx = context->drdynvc_server_context;

	if (vc_ctx) {
		vc_ctx->Stop(vc_ctx);
		drdynvc_server_context_free(vc_ctx);
	}
}

bool
rdp_rail_peer_init(freerdp_peer *client, RdpPeerContext *peer_ctx)
{
	struct rdp_backend *b = peer_ctx->rdpBackend;

	/* RDP window ID must be within 31 bits range. MSB is reserved and exclude 0. */
	if (!rdp_id_manager_init(b, &peer_ctx->windowId, 0x1, 0x7FFFFFFF)) {
		rdp_debug_error(b, "unable to create windowId.\n");
		goto error_return;
	}
	/* RDP surface ID must be within 16 bits range, exclude 0. */
	if (!rdp_id_manager_init(b, &peer_ctx->surfaceId, 0x1, 0xFFFF)) {
		rdp_debug_error(b, "unable to create windowId.\n");
		goto error_return;
	}
#ifdef HAVE_FREERDP_GFXREDIR_H
	/* RDP pool ID must be within 32 bits range, exclude 0. */
	if (!rdp_id_manager_init(b, &peer_ctx->poolId, 0x1, 0xFFFFFFFF)) {
		rdp_debug_error(b, "unable to create windowId.\n");
		goto error_return;
	}
	/* RDP buffer ID must be within 32 bits range, exclude 0. */
	if (!rdp_id_manager_init(b, &peer_ctx->bufferId, 0x1, 0xFFFFFFFF)) {
		rdp_debug_error(b, "unable to create windowId.\n");
		goto error_return;
	}
#endif /* HAVE_FREERDP_GFXREDIR_H */

	peer_ctx->currentFrameId = 0;
	peer_ctx->acknowledgedFrameId = 0;

	return TRUE;

error_return:

#ifdef HAVE_FREERDP_GFXREDIR_H
	rdp_id_manager_free(&peer_ctx->bufferId);
	rdp_id_manager_free(&peer_ctx->poolId);
#endif /* HAVE_FREERDP_GFXREDIR_H */
	rdp_id_manager_free(&peer_ctx->surfaceId);
	rdp_id_manager_free(&peer_ctx->windowId);

	return FALSE;
}

static void
print_matrix_type(FILE *fp, unsigned int type)
{
	fprintf(fp, "        matrix type: %x: ", type);
	if (type == 0) {
		fprintf(fp, "identify ");
	} else {
		if (type & WESTON_MATRIX_TRANSFORM_TRANSLATE)
			fprintf(fp, "translate ");
		if (type & WESTON_MATRIX_TRANSFORM_SCALE)
			fprintf(fp, "scale ");
		if (type & WESTON_MATRIX_TRANSFORM_ROTATE)
			fprintf(fp, "rotate ");
		if (type & WESTON_MATRIX_TRANSFORM_OTHER)
			fprintf(fp, "other ");
	}
	fprintf(fp, "\n");
}

static void
print_matrix(FILE *fp, const char *name, const struct weston_matrix *matrix)
{
	int i;

	if (name)
		fprintf(fp, "    %s\n", name);
	print_matrix_type(fp, matrix->type);
	for (i = 0; i < 4; i++)
		fprintf(fp,
			"        %8.2f, %8.2f, %8.2f, %8.2f\n",
			matrix->d[4*i+0], matrix->d[4*i+1],
			matrix->d[4*1+2], matrix->d[4*i+3]);
}

static void
print_rdp_head(FILE *fp, const struct rdp_head *current)
{
	const struct weston_head *wh = &current->base;
	struct weston_compositor *ec = wh->compositor;
	struct rdp_backend *b = to_rdp_backend(ec);
	float client_scale = disp_get_client_scale_from_monitor(b, &current->config);
	int scale = disp_get_output_scale_from_monitor(b, &current->config);

	fprintf(fp,"    rdp_head: %s: index:%d: is_primary:%d\n",
		current->base.name, current->index,
		current->config.is_primary);
	fprintf(fp,"    x:%d, y:%d, RDP client x:%d, y:%d\n",
		current->base.output->x, current->base.output->y,
		current->config.x, current->config.y);
	fprintf(fp,"    width:%d, height:%d, RDP client width:%d, height: %d\n",
		current->base.output->width, current->base.output->height,
		current->config.width, current->config.height);
	fprintf(fp,"    physicalWidth:%dmm, physicalHeight:%dmm, orientation:%d\n",
		current->config.attributes.physicalWidth,
		current->config.attributes.physicalHeight,
		current->config.attributes.orientation);
	fprintf(fp,"    desktopScaleFactor:%d, deviceScaleFactor:%d\n",
		current->config.attributes.desktopScaleFactor,
		current->config.attributes.deviceScaleFactor);
	fprintf(fp,"    scale:%d, client scale :%3.2f\n",
		scale, client_scale);
	fprintf(fp,"    workarea: x:%d, y:%d, width:%d, height:%d\n",
		current->workarea.x, current->workarea.y,
		current->workarea.width, current->workarea.height);
	fprintf(fp, "    RDP client workarea: x:%d, y:%d, width:%d, height%d\n",
		current->workareaClient.x, current->workareaClient.y,
		current->workareaClient.width, current->workareaClient.height);
	fprintf(fp, "    connected:%d, non_desktop:%d\n",
		current->base.connected, current->base.non_desktop);
	fprintf(fp, "    assigned output: %s\n",
		current->base.output ? current->base.output->name : "(no output)");
	if (current->base.output) {
		fprintf(fp, "    output extents box: x1:%d, y1:%d, x2:%d, y2:%d\n",
			current->base.output->region.extents.x1,
			current->base.output->region.extents.y1,
			current->base.output->region.extents.x2,
			current->base.output->region.extents.y2);
		fprintf(fp, "    output scale:%d, output native_scale:%d\n",
			current->base.output->scale,
			current->base.output->native_scale);
		print_matrix(fp, "global to output matrix:",
			     &current->base.output->matrix);
		print_matrix(fp, "output to global matrix:",
			     &current->base.output->inverse_matrix);
	}
}

static void
rdp_rail_dump_monitor_binding(struct weston_keyboard *keyboard,
			      const struct timespec *time,
			      uint32_t key, void *data)
{
	struct rdp_backend *b = data;

	if (b) {
		struct weston_head *current;
		int err;
		char *str;
		size_t len;
		FILE *fp = open_memstream(&str, &len);

		assert(fp);
		fprintf(fp,"\nrdp debug binding 'M' - dump all monitor.\n");
		wl_list_for_each(current, &b->compositor->head_list, compositor_link) {
			struct rdp_head *head = to_rdp_head(current);

			print_rdp_head(fp, head);
			fprintf(fp,"\n");
		}
		err = fclose(fp);
		assert(err == 0);
		rdp_debug_error(b, "%s", str);
		free(str);
	}
}

struct rdp_rail_dump_window_context {
	FILE *fp;
	RdpPeerContext *peer_ctx;
};

static void
rdp_rail_dump_window_label(struct weston_surface *surface, char *label, uint32_t label_size)
{
	if (surface->get_label) {
		strcpy(label, "Label: "); /* 7 chars */
		surface->get_label(surface, label + 7, label_size - 7);
	} else if (surface->role_name) {
		snprintf(label, label_size,
			 "RoleName: %s", surface->role_name);
	} else {
		strcpy(label, "(No Label, No Role name)");
	}
}

static void
rdp_rail_dump_window_iter(void *element, void *data)
{
	struct weston_surface *surface = element;
	struct weston_surface_rail_state *rail_state = surface->backend_state;
	struct rdp_rail_dump_window_context *context = data;
	struct rdp_backend *b = context->peer_ctx->rdpBackend;
	const struct weston_rdprail_shell_api *api = b->rdprail_shell_api;
	FILE *fp = context->fp;
	char label[256] = {};
	struct weston_geometry geometry = {};
	struct weston_view *view;
	int content_buffer_width, content_buffer_height;

	/* this iter is looping from window hash table,
	 * thus it must have rail_state initialized.
	 */
	assert(rail_state);

	weston_surface_get_content_size(surface,
					&content_buffer_width,
					&content_buffer_height);

	if (api && api->get_window_geometry)
		api->get_window_geometry(surface, &geometry);

	rdp_rail_dump_window_label(surface, label, sizeof(label));
	fprintf(fp, "    %s\n", label);
	fprintf(fp, "    WindowId:0x%x, SurfaceId:0x%x\n",
		rail_state->window_id, rail_state->surface_id);
	fprintf(fp, "    PoolId:0x%x, BufferId:0x%x\n",
		rail_state->pool_id, rail_state->buffer_id);
	fprintf(fp, "    Position x:%d, y:%d width:%d height:%d\n",
		rail_state->pos.x, rail_state->pos.y,
		rail_state->pos.width, rail_state->pos.height);
	fprintf(fp, "    RDP client position x:%d, y:%d width:%d height:%d\n",
		rail_state->clientPos.x, rail_state->clientPos.y,
		rail_state->clientPos.width, rail_state->clientPos.height);
	fprintf(fp, "    Window margin left:%d, top:%d, right:%d bottom:%d\n",
		rail_state->window_margin_left, rail_state->window_margin_top,
		rail_state->window_margin_right, rail_state->window_margin_bottom);
	fprintf(fp, "    Window geometry x:%d, y:%d, width:%d height:%d\n",
		geometry.x, geometry.y,
		geometry.width, geometry.height);
	fprintf(fp, "    input extents: x1:%d, y1:%d, x2:%d, y2:%d\n",
		surface->input.extents.x1, surface->input.extents.y1,
		surface->input.extents.x2, surface->input.extents.y2);
	fprintf(fp, "    bufferWidth:%d, bufferHeight:%d\n",
		rail_state->bufferWidth, rail_state->bufferHeight);
	fprintf(fp, "    bufferScaleFactorWidth:%.2f, bufferScaleFactorHeight:%.2f\n",
		rail_state->bufferScaleFactorWidth, rail_state->bufferScaleFactorHeight);
	fprintf(fp, "    content_buffer_width:%d, content_buffer_height:%d\n",
		content_buffer_width, content_buffer_height);
	fprintf(fp, "    is_opaque:%d\n", surface->is_opaque);
	if (!surface->is_opaque && pixman_region32_not_empty(&surface->opaque)) {
		int numRects = 0;
		pixman_box32_t *rects = pixman_region32_rectangles(&surface->opaque, &numRects);

		fprintf(fp, "    opaque region: numRects:%d\n", numRects);
		for (int n = 0; n < numRects; n++)
			fprintf(fp, "        [%d]: (%d, %d) - (%d, %d)\n",
				n, rects[n].x1, rects[n].y1, rects[n].x2, rects[n].y2);
	}
	fprintf(fp, "    parent_surface:%p, isCursor:%d, isWindowCreated:%d\n",
		rail_state->parent_surface, rail_state->isCursor, rail_state->isWindowCreated);
	fprintf(fp, "    showState:%s, showState_requested:%s\n",
		rdp_showstate_to_string(rail_state->showState),
		rdp_showstate_to_string(rail_state->showState_requested));
	fprintf(fp, "    forceRecreateSurface:%d, error:%d\n",
		rail_state->forceRecreateSurface, rail_state->error);
	fprintf(fp, "    isUdatePending:%d, isFirstUpdateDone:%d\n",
		rail_state->isUpdatePending, rail_state->isFirstUpdateDone);
	fprintf(fp, "    surface:0x%p\n", surface);
	wl_list_for_each(view, &surface->views, surface_link) {
		fprintf(fp, "    view: %p\n", view);
		fprintf(fp, "    view's alpha: %3.2f\n", view->alpha);
		fprintf(fp, "    view's opaque region: x1:%d, y1:%d, x2:%d, y2:%d\n",
			view->transform.opaque.extents.x1,
			view->transform.opaque.extents.y1,
			view->transform.opaque.extents.x2,
			view->transform.opaque.extents.y2);
		if (pixman_region32_not_empty(&view->transform.opaque)) {
			int numRects = 0;
			pixman_box32_t *rects = pixman_region32_rectangles(&view->transform.opaque, &numRects);

			fprintf(fp, "    view's opaque region: numRects:%d\n", numRects);
			for (int n = 0; n < numRects; n++)
				fprintf(fp, "        [%d]: (%d, %d) - (%d, %d)\n",
					n, rects[n].x1, rects[n].y1, rects[n].x2, rects[n].y2);
		}
		fprintf(fp, "    view's boundingbox: x1:%d, y1:%d, x2:%d, y2:%d\n",
			view->transform.boundingbox.extents.x1,
			view->transform.boundingbox.extents.y1,
			view->transform.boundingbox.extents.x2,
			view->transform.boundingbox.extents.y2);
		fprintf(fp, "    view's scissor: x1:%d, y1:%d, x2:%d, y2:%d\n",
			view->geometry.scissor.extents.x1,
			view->geometry.scissor.extents.y1,
			view->geometry.scissor.extents.x2,
			view->geometry.scissor.extents.y2);
		fprintf(fp, "    view's transform: enabled:%d\n",
			view->transform.enabled);
		if (view->transform.enabled)
			print_matrix(fp, NULL, &view->transform.matrix);
	}
	print_matrix(fp, "buffer to surface matrix:",
		     &surface->buffer_to_surface_matrix);
	print_matrix(fp, "surface to buffer matrix:",
		     &surface->surface_to_buffer_matrix);
	fprintf(fp, "    output:0x%p (%s)\n", surface->output,
		surface->output ? surface->output->name : "(no output assigned)");
	if (surface->output) {
		struct weston_head *base_head;

		wl_list_for_each(base_head, &surface->output->head_list, output_link)
			print_rdp_head(fp, to_rdp_head(base_head));
	}
	fprintf(fp,"\n");
}

static void
rdp_rail_dump_window_binding(struct weston_keyboard *keyboard,
			     const struct timespec *time,
			     uint32_t key, void *data)
{
	struct rdp_backend *b = data;
	RdpPeerContext *peer_ctx;

	if (b && b->rdp_peer && b->rdp_peer->context) {
		/* print window from window hash table */
		struct rdp_rail_dump_window_context context;
		int err;
		char *str;
		size_t len;
		FILE *fp = open_memstream(&str, &len);

		assert(fp);
		fprintf(fp, "\nrdp debug binding 'W' - dump all window.\n");
		peer_ctx = (RdpPeerContext *)b->rdp_peer->context;
		dump_id_manager_state(fp, &peer_ctx->windowId, "windowId");
		dump_id_manager_state(fp, &peer_ctx->surfaceId, "surfaceId");
#ifdef HAVE_FREERDP_GFXREDIR_H
		dump_id_manager_state(fp, &peer_ctx->poolId, "poolId");
		dump_id_manager_state(fp, &peer_ctx->bufferId, "bufferId");
#endif /* HAVE_FREERDP_GFXREDIR_H */
		context.peer_ctx = peer_ctx;
		context.fp = fp;
		rdp_id_manager_for_each(&peer_ctx->windowId, rdp_rail_dump_window_iter, (void*)&context);
		err = fclose(fp);
		assert(err == 0);
		rdp_debug_error(b, "%s", str);
		free(str);

		/* print out compositor's scene graph */
		str = weston_compositor_print_scene_graph(b->compositor);
		rdp_debug_error(b, "%s", str);
		free(str);
	}
}

static void *
rdp_rail_shell_initialize_notify(struct weston_compositor *compositor,
				 const struct weston_rdprail_shell_api *rdprail_shell_api,
				 void *context, char *name)
{
	struct rdp_backend *b = to_rdp_backend(compositor);

	b->rdprail_shell_api = rdprail_shell_api;
	b->rdprail_shell_context = context;
	free(b->rdprail_shell_name);
	b->rdprail_shell_name = name ? strdup(name) : NULL;
	rdp_debug(b, "%s: shell: distro name: %s\n",
		  __func__, b->rdprail_shell_name);
	return (void *)b;
}

#define WINDOW_ORDER_ICON_ROWLENGTH( W, BPP ) ((((W) * (BPP) + 31) / 32) * 4)

static void
rdp_rail_set_window_icon(struct weston_surface *surface, pixman_image_t *icon)
{
	struct weston_surface_rail_state *rail_state = surface->backend_state;
	struct weston_compositor *compositor = surface->compositor;
	struct rdp_backend *b = to_rdp_backend(compositor);
	rdpUpdate *update;
	RdpPeerContext *peer_ctx;
	WINDOW_ORDER_INFO order_info = {};
	WINDOW_ICON_ORDER icon_order = {};
	ICON_INFO icon_info = {};
	pixman_image_t *scaled_icon = NULL;
	bool bits_color_allocated = false;
	void *bits_color = NULL;
	void *bits_mask = NULL;
	int width;
	int height;
	int stride;
	double x_scale;
	double y_scale;
	struct pixman_transform transform;
	pixman_format_code_t format;
	int max_icon_width;
	int max_icon_height;
	int target_icon_width;
	int target_icon_height;

	if (!b || !b->rdp_peer) {
		rdp_debug_error(b, "set_window_icon(): rdp_peer is not initalized\n");
		return;
	}

	peer_ctx = (RdpPeerContext *)b->rdp_peer->context;
	update = b->rdp_peer->context->update;

	if (!b->rdp_peer->context->settings->HiDefRemoteApp)
		return;

	assert_compositor_thread(b);

	if (!rail_state || rail_state->window_id == 0) {
		rdp_rail_create_window(NULL, (void *)surface);
		rail_state = surface->backend_state;
		if (!rail_state || rail_state->window_id == 0)
			return;
	}

	width = pixman_image_get_width(icon);
	height = pixman_image_get_height(icon);
	format = pixman_image_get_format(icon);
	stride = pixman_image_get_stride(icon);

	if (width == 0 || height == 0)
		return;

	rdp_debug_verbose(b, "rdp_rail_set_window_icon: original icon width:%d height:%d format:%d\n",
			  width, height, format);

	/* TS_RAIL_CLIENTSTATUS_HIGH_DPI_ICONS_SUPPORTED
	   Indicates that the client supports icons up to 96 pixels in size in the
	   Window Icon PDU. If this flag is not present, icon dimensions are limited
	   to 32 pixels. */
	if (peer_ctx->clientStatusFlags & TS_RAIL_CLIENTSTATUS_HIGH_DPI_ICONS_SUPPORTED) {
		max_icon_width = 96;
		max_icon_height = 96;
	} else {
		max_icon_width = 32;
		max_icon_height = 32;
	}

	if (width > max_icon_width)
		target_icon_width = max_icon_width;
	else
		target_icon_width = width;

	if (height > max_icon_height)
		target_icon_height = max_icon_height;
	else
		target_icon_height = height;

	/* create icon bitmap with flip in Y-axis, and client always expects a8r8g8b8 format. */
	scaled_icon = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8,
							target_icon_width,
							target_icon_height,
							NULL, 0);
	if (!scaled_icon)
		return;

	x_scale = (double)width / target_icon_width;
	y_scale = (double)height / target_icon_height;
	pixman_transform_init_scale(&transform,
				    pixman_double_to_fixed(x_scale),
				    pixman_double_to_fixed(y_scale * -1)); /* flip Y. */
	pixman_transform_translate(&transform, NULL,
				   0, pixman_int_to_fixed(height));
	pixman_image_set_transform(icon, &transform);
	pixman_image_set_filter(icon, PIXMAN_FILTER_BILINEAR, NULL, 0);

	pixman_image_composite32(PIXMAN_OP_SRC,
				 icon, /* src */
				 NULL, /* mask */
				 scaled_icon, /* dest */
				 0, 0, /* src_x, src_y */
				 0, 0, /* mask_x, mask_y */
				 0, 0, /* dest_x, dest_y */
				 target_icon_width, /* width */
				 target_icon_height /* height */);

	pixman_image_set_filter(icon, PIXMAN_FILTER_NEAREST, NULL, 0);
	pixman_image_set_transform(icon, NULL);

	icon = scaled_icon;
	width = pixman_image_get_width(icon);
	height = pixman_image_get_height(icon);
	format = pixman_image_get_format(icon);
	stride = pixman_image_get_stride(icon);

	assert(width == target_icon_width);
	assert(height == target_icon_height);
	assert(format == PIXMAN_a8r8g8b8);

	rdp_debug_verbose(b, "rdp_rail_set_window_icon: converted icon width:%d height:%d format:%d\n",
			  width, height, format);

	/* color bitmap is 32 bits */
	int stride_color = WINDOW_ORDER_ICON_ROWLENGTH(width, 32);
	int size_color = stride_color * height;

	if (stride_color != stride) {
		/* when pixman's stride is differnt from client's expetation, need to adjust. */
		size_color = stride_color * height;
		bits_color = xmalloc(size_color);
		bits_color_allocated = true;
	} else {
		bits_color = pixman_image_get_data(icon);
	}

	/* Mask is 1 bit */
	int stride_mask = WINDOW_ORDER_ICON_ROWLENGTH(width, 1);
	int size_mask = stride_mask * height;

	bits_mask = xzalloc(size_mask);

	/* generate mask and copy color bits, match to the stride RDP wants when different. */
	char *src_color = (char *)pixman_image_get_data(icon);
	char *dst_color = bits_color;
	char *dst_mask = bits_mask;

	for (int i = 0; i < height; i++) {
		uint32_t *src = (uint32_t *)src_color;
		uint32_t *dst = (uint32_t *)dst_color;
		char *mask = dst_mask;

		for (int j = 0; j < width; j++) {
			if (dst != src)
				*dst = *src;
			if (*dst & 0xFF000000)
				mask[j / 8] |= 0x80 >> (j % 8);
			dst++;
			src++;
		}
		src_color += stride;
		dst_color += stride_color;
		dst_mask += stride_mask;
	}

	order_info.windowId = rail_state->window_id;
	order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_ICON;
	icon_info.cacheEntry = 0xFFFF; /* no cache */
	icon_info.cacheId = 0xFF; /* no cache */
	icon_info.bpp = 32;
	icon_info.width = (uint32_t)width;
	icon_info.height = (uint32_t)height;
	icon_info.cbColorTable = 0;
	icon_info.cbBitsMask = size_mask;
	icon_info.cbBitsColor = size_color;
	icon_info.bitsMask = bits_mask;
	icon_info.colorTable = NULL;
	icon_info.bitsColor = bits_color;
	icon_order.iconInfo = &icon_info;

	update->BeginPaint(update->context);
	update->window->WindowIcon(update->context, &order_info, &icon_order);
	update->EndPaint(update->context);

	free(bits_mask);
	if (bits_color_allocated)
		free(bits_color);
	if (scaled_icon)
		pixman_image_unref(scaled_icon);

	return;
}

#ifdef HAVE_FREERDP_RDPAPPLIST_H
static bool
rdp_rail_notify_app_list(void *rdp_backend,
			 struct weston_rdprail_app_list_data *app_list_data)
{
	struct rdp_backend *b = rdp_backend;
	RdpAppListServerContext *applist_ctx;
	RdpPeerContext *peer_ctx;

	if (!b || !b->rdp_peer) {
		rdp_debug_error(b, "rdp_rail_notify_app_list(): rdp_peer is not initalized\n");
		/* return false only when peer is not ready for
		 * possible re-send.
		 */
		return false;
	}

	if (!b->rdp_peer->context->settings->HiDefRemoteApp)
		return true;

	peer_ctx = (RdpPeerContext *)b->rdp_peer->context;

	applist_ctx = peer_ctx->applist_server_context;
	if (!applist_ctx)
		return false;

	rdp_debug(b, "rdp_rail_notify_app_list(): rdp_peer %p\n", peer_ctx);
	rdp_debug(b, "    inSync: %d\n", app_list_data->inSync);
	rdp_debug(b, "    syncStart: %d\n", app_list_data->syncStart);
	rdp_debug(b, "    syncEnd: %d\n", app_list_data->syncEnd);
	rdp_debug(b, "    newAppId: %d\n", app_list_data->newAppId);
	rdp_debug(b, "    deleteAppId: %d\n", app_list_data->deleteAppId);
	rdp_debug(b, "    deleteAppProvider: %d\n", app_list_data->deleteAppProvider);
	rdp_debug(b, "    associateWindowId: %d\n", app_list_data->associateWindowId);
	rdp_debug(b, "    appId: %s\n", app_list_data->appId);
	rdp_debug(b, "    appGroup: %s\n", app_list_data->appGroup);
	rdp_debug(b, "    appExecPath: %s\n", app_list_data->appExecPath);
	rdp_debug(b, "    appWorkingDir: %s\n", app_list_data->appWorkingDir);
	rdp_debug(b, "    appDesc: %s\n", app_list_data->appDesc);
	rdp_debug(b, "    appIcon: %p\n", app_list_data->appIcon);
	rdp_debug(b, "    appProvider: %s\n", app_list_data->appProvider);
	rdp_debug(b, "    appWindowId: 0x%x\n", app_list_data->appWindowId);

	if (app_list_data->associateWindowId) {
		RDPAPPLIST_ASSOCIATE_WINDOW_ID_PDU associate_window_id = {};

		assert(app_list_data->appProvider == NULL);
		associate_window_id.flags = RDPAPPLIST_FIELD_ID | RDPAPPLIST_FIELD_WINDOW_ID;
		associate_window_id.appWindowId = app_list_data->appWindowId;
		if (app_list_data->appId == NULL ||
		    !utf8_string_to_rail_string(app_list_data->appId, &associate_window_id.appId))
			goto Exit_associateWindowId;

		if (app_list_data->appGroup &&
		    utf8_string_to_rail_string(app_list_data->appGroup, &associate_window_id.appGroup)) {
			associate_window_id.flags |= RDPAPPLIST_FIELD_GROUP;
		}
		if (app_list_data->appExecPath &&
		    utf8_string_to_rail_string(app_list_data->appExecPath, &associate_window_id.appExecPath)) {
			associate_window_id.flags |= RDPAPPLIST_FIELD_EXECPATH;
		}
		if (app_list_data->appDesc &&
		    utf8_string_to_rail_string(app_list_data->appDesc, &associate_window_id.appDesc)) {
			associate_window_id.flags |= RDPAPPLIST_FIELD_DESC;
		}
		applist_ctx->AssociateWindowId(applist_ctx, &associate_window_id);
	Exit_associateWindowId:
		free(associate_window_id.appId.string);
		free(associate_window_id.appGroup.string);
	} else if (app_list_data->deleteAppId) {
		RDPAPPLIST_DELETE_APPLIST_PDU delete_app_list = {};

		assert(app_list_data->appProvider == NULL);
		delete_app_list.flags = RDPAPPLIST_FIELD_ID;
		if (app_list_data->appId == NULL ||
		    !utf8_string_to_rail_string(app_list_data->appId, &delete_app_list.appId))
			goto Exit_deletePath;

		if (app_list_data->appGroup &&
		    utf8_string_to_rail_string(app_list_data->appGroup, &delete_app_list.appGroup)) {
			delete_app_list.flags |= RDPAPPLIST_FIELD_GROUP;
		}
		applist_ctx->DeleteApplicationList(applist_ctx, &delete_app_list);
	Exit_deletePath:
		free(delete_app_list.appId.string);
		free(delete_app_list.appGroup.string);
	} else if (app_list_data->deleteAppProvider) {
		RDPAPPLIST_DELETE_APPLIST_PROVIDER_PDU del_provider = {};

		del_provider.flags = RDPAPPLIST_FIELD_PROVIDER;
		if (app_list_data->appProvider &&
		    utf8_string_to_rail_string(app_list_data->appProvider, &del_provider.appListProviderName))
			applist_ctx->DeleteApplicationListProvider(applist_ctx, &del_provider);
		free(del_provider.appListProviderName.string);
	} else {
		RDPAPPLIST_UPDATE_APPLIST_PDU update_app_list = {};
		RDPAPPLIST_ICON_DATA iconData = {};

		assert(app_list_data->appProvider == NULL);
		update_app_list.flags = app_list_data->newAppId ? RDPAPPLIST_HINT_NEWID : 0;
		if (app_list_data->inSync)
			update_app_list.flags |= RDPAPPLIST_HINT_SYNC;
		if (app_list_data->syncStart) {
			assert(app_list_data->inSync);
			update_app_list.flags |= RDPAPPLIST_HINT_SYNC_START;
		}
		if (app_list_data->syncEnd) {
			assert(app_list_data->inSync);
			update_app_list.flags |= RDPAPPLIST_HINT_SYNC_END;
		}
		update_app_list.flags |= RDPAPPLIST_FIELD_ID |
					 RDPAPPLIST_FIELD_EXECPATH |
					 RDPAPPLIST_FIELD_DESC;
		if (app_list_data->appId == NULL || /* id is required. */
		    !utf8_string_to_rail_string(app_list_data->appId, &update_app_list.appId))
			goto Exit_updatePath;
		if (app_list_data->appExecPath == NULL || /* exePath is required. */
		    !utf8_string_to_rail_string(app_list_data->appExecPath, &update_app_list.appExecPath))
			goto Exit_updatePath;
		if (app_list_data->appDesc == NULL || /* desc is required. */
		    !utf8_string_to_rail_string(app_list_data->appDesc, &update_app_list.appDesc))
			goto Exit_updatePath;

		if (app_list_data->appGroup && /* group is optional. */
		    utf8_string_to_rail_string(app_list_data->appGroup, &update_app_list.appGroup)) {
			update_app_list.flags |= RDPAPPLIST_FIELD_GROUP;
		}
		if (app_list_data->appWorkingDir && /* workingDir is optional. */
		    utf8_string_to_rail_string(app_list_data->appWorkingDir, &update_app_list.appWorkingDir)) {
			update_app_list.flags |= RDPAPPLIST_FIELD_WORKINGDIR;
		}
		if (app_list_data->appIcon) { /* icon is optional. */
			iconData.flags = 0;
			iconData.iconWidth = pixman_image_get_width(app_list_data->appIcon);
			iconData.iconHeight = pixman_image_get_height(app_list_data->appIcon);
			iconData.iconStride = pixman_image_get_stride(app_list_data->appIcon);
			iconData.iconBpp = 32;
			if (pixman_image_get_format(app_list_data->appIcon) != PIXMAN_a8r8g8b8)
				goto Exit_updatePath;
			iconData.iconFormat = RDPAPPLIST_ICON_FORMAT_BMP;
			iconData.iconBitsLength = iconData.iconHeight * iconData.iconStride;
			iconData.iconBits = xmalloc(iconData.iconBitsLength);
			char *src = (char *)pixman_image_get_data(app_list_data->appIcon);
			char *dst = (char *)iconData.iconBits + (iconData.iconHeight-1) * iconData.iconStride;

			for (uint32_t i = 0; i < iconData.iconHeight; i++) {
				memcpy(dst, src, iconData.iconStride);
				src += iconData.iconStride;
				dst -= iconData.iconStride;
			}
			update_app_list.appIcon = &iconData;
			update_app_list.flags |= RDPAPPLIST_FIELD_ICON;
		}
		applist_ctx->UpdateApplicationList(applist_ctx, &update_app_list);
	Exit_updatePath:
		free(iconData.iconBits);
		free(update_app_list.appId.string);
		free(update_app_list.appGroup.string);
		free(update_app_list.appExecPath.string);
		free(update_app_list.appWorkingDir.string);
		free(update_app_list.appDesc.string);
	}
	return true;
}
#endif /* HAVE_FREERDP_RDPAPPLIST_H */

static struct weston_output *
rdp_rail_get_primary_output(void *rdp_backend)
{
	struct rdp_backend *b = rdp_backend;
	struct weston_head *current;

	wl_list_for_each(current, &b->compositor->head_list, compositor_link) {
		struct rdp_head *head = to_rdp_head(current);

		if (head->config.is_primary)
			return current->output;
	}
	return NULL;
}

struct weston_rdprail_api rdprail_api = {
	.shell_initialize_notify = rdp_rail_shell_initialize_notify,
	.start_window_move = rdp_rail_start_window_move,
	.end_window_move = rdp_rail_end_window_move,
	.send_window_minmax_info = rdp_rail_send_window_minmax_info,
	.set_window_icon = rdp_rail_set_window_icon,
#ifdef HAVE_FREERDP_RDPAPPLIST_H
	.notify_app_list = rdp_rail_notify_app_list,
#else
	.notify_app_list = NULL,
#endif /* HAVE_FREERDP_RDPAPPLIST_H */
	.get_primary_output = rdp_rail_get_primary_output,
	.notify_window_zorder_change = rdp_rail_notify_window_zorder_change,
	.notify_window_proxy_surface = rdp_rail_notify_window_proxy_surface,
};

int
rdp_rail_backend_create(struct rdp_backend *b, struct weston_rdp_backend_config *config)
{
	int ret = weston_plugin_api_register(b->compositor, WESTON_RDPRAIL_API_NAME,
					     &rdprail_api, sizeof(rdprail_api));
	if (ret < 0) {
		rdp_debug_error(b, "Failed to register rdprail API.\n");
		return -1;
	}

#ifdef HAVE_FREERDP_RDPAPPLIST_H
	bool use_rdpapplist = config->rail_config.use_rdpapplist;

	if (use_rdpapplist) {
		use_rdpapplist = false;

		rdp_debug(b, "RDPAPPLIST_MODULEDIR is set to %s\n", RDPAPPLIST_MODULEDIR);

		dlerror(); /* clear error */
		b->libRDPApplistServer = dlopen(RDPAPPLIST_MODULEDIR "/" "librdpapplist-server.so", RTLD_NOW);
		if (!b->libRDPApplistServer) {
			rdp_debug_error(b,
					"dlopen(%s/librdpapplist-server.so) failed with %s\n",
					RDPAPPLIST_MODULEDIR, dlerror());
			b->libRDPApplistServer = dlopen("librdpapplist-server.so", RTLD_NOW);
			if (!b->libRDPApplistServer) {
				rdp_debug_error(b,
						"dlopen(librdpapplist-server.so) failed with %s\n",
						dlerror());
			}
		}

		if (b->libRDPApplistServer) {
			b->rdpapplist_server_context_new = dlsym(b->libRDPApplistServer, "rdpapplist_server_context_new");
			b->rdpapplist_server_context_free = dlsym(b->libRDPApplistServer, "rdpapplist_server_context_free");
			if (b->rdpapplist_server_context_new && b->rdpapplist_server_context_free) {
				use_rdpapplist = true;
			} else {
				rdp_debug(b, "librdpapplist-server.so doesn't have required applist entry.\n");
				dlclose(b->libRDPApplistServer);
				b->libRDPApplistServer = NULL;
			}
		}
	}

	b->use_rdpapplist = use_rdpapplist;
	rdp_debug(b, "RDP backend: use_rdpapplist = %d\n", b->use_rdpapplist);
#endif /* HAVE_FREERDP_RDPAPPLIST_H */

#ifdef HAVE_FREERDP_GFXREDIR_H
	bool use_gfxredir = config->rail_config.use_shared_memory;
	/* check if shared memory mount path is set */
	if (use_gfxredir) {
		use_gfxredir = false;
		/* shared memory mount point path is always given as environment variable from WSL */
		char *s = getenv("WSL2_SHARED_MEMORY_MOUNT_POINT");

		if (s) {
			b->shared_memory_mount_path = s;
			b->shared_memory_mount_path_size = strlen(b->shared_memory_mount_path);
			use_gfxredir = true;
		} else {
			rdp_debug(b, "WSL2_SHARED_MEMORY_MOUNT_POINT is not set.\n");
		}
	}

	/* check if FreeRDP server lib supports graphics redirection channel API */
	if (use_gfxredir) {
		use_gfxredir = false;

		dlerror(); /* clear error */
#if FREERDP_VERSION_MAJOR >= 3
		b->libFreeRDPServer = dlopen("libfreerdp-server3.so", RTLD_NOW);
#else
		b->libFreeRDPServer = dlopen("libfreerdp-server2.so", RTLD_NOW);
#endif
		if (!b->libFreeRDPServer) {
			rdp_debug_error(b, "dlopen(libfreerdp-server%d.so) failed with %s\n",
					FREERDP_VERSION_MAJOR, dlerror());
		} else {
			b->gfxredir_server_context_new = dlsym(b->libFreeRDPServer, "gfxredir_server_context_new");
			b->gfxredir_server_context_free = dlsym(b->libFreeRDPServer, "gfxredir_server_context_free");
			if (b->gfxredir_server_context_new && b->gfxredir_server_context_new) {
				use_gfxredir = true;
			} else {
				rdp_debug(b, "libfreerdp-server%d.so doesn't support graphics redirection API.\n",
					FREERDP_VERSION_MAJOR);
				dlclose(b->libFreeRDPServer);
				b->libFreeRDPServer = NULL;
			}
		}
	}

	/* Test virtfsio actually works */
	if (use_gfxredir) {
		struct weston_rdp_shared_memory shmem = {};

		use_gfxredir = false;
		shmem.size = sysconf(_SC_PAGESIZE);
		if (rdp_allocate_shared_memory(b, &shmem)) {
			*(uint32_t *)shmem.addr = 0x12344321;
			assert(*(uint32_t *)shmem.addr == 0x12344321);
			rdp_free_shared_memory(b, &shmem);
			use_gfxredir = true;
		}
	}

	b->use_gfxredir = use_gfxredir;
	rdp_debug(b, "RDP backend: use_gfxredir = %d\n", b->use_gfxredir);
#endif /* HAVE_FREERDP_GFXREDIR_H */

	b->enable_hi_dpi_support = config->rail_config.enable_hi_dpi_support;
	rdp_debug(b, "RDP backend: enable_hi_dpi_support = %d\n",
		  b->enable_hi_dpi_support);

	b->enable_fractional_hi_dpi_support = config->rail_config.enable_fractional_hi_dpi_support;
	rdp_debug(b, "RDP backend: enable_fractional_hi_dpi_support = %d\n",
		  b->enable_fractional_hi_dpi_support);

	b->enable_fractional_hi_dpi_roundup = config->rail_config.enable_fractional_hi_dpi_roundup;
	rdp_debug(b, "RDP backend: enable_fractional_hi_dpi_roundup = %d\n",
		  b->enable_fractional_hi_dpi_roundup);

	b->debug_desktop_scaling_factor = config->rail_config.debug_desktop_scaling_factor;
	rdp_debug(b, "RDP backend: debug_desktop_scaling_factor = %d\n",
		  b->debug_desktop_scaling_factor);

	b->enable_window_zorder_sync = config->rail_config.enable_window_zorder_sync;
	rdp_debug(b, "RDP backend: enable_window_zorder_sync = %d\n",
		  b->enable_window_zorder_sync);

	b->enable_window_snap_arrange = config->rail_config.enable_window_snap_arrange;
	rdp_debug(b, "RDP backend: enable_window_snap_arrange = %d\n",
		  b->enable_window_snap_arrange);

	b->enable_window_shadow_remoting = config->rail_config.enable_window_shadow_remoting;
	rdp_debug(b, "RDP backend: enable_window_shadow_remoting = %d\n",
		  b->enable_window_shadow_remoting);

	b->enable_display_power_by_screenupdate = config->rail_config.enable_display_power_by_screenupdate;
	rdp_debug(b, "RDP backend: enable_display_power_by_screenupdate = %d\n",
		  b->enable_display_power_by_screenupdate);

	b->enable_distro_name_title = config->rail_config.enable_distro_name_title;
	rdp_debug(b, "RDP backend: enable_distro_name_title = %d\n",
		  b->enable_distro_name_title);

	b->enable_copy_warning_title = config->rail_config.enable_copy_warning_title;
	rdp_debug(b, "RDP backend: enable_copy_warning_title = %d\n",
		  b->enable_copy_warning_title);

	b->rdprail_shell_name = NULL;

	/* M to dump all outstanding monitor info */
	b->debug_binding_M = weston_compositor_add_debug_binding(b->compositor,
								 KEY_M,
								 rdp_rail_dump_monitor_binding,
								 b);
	/* W to dump all outstanding window info */
	b->debug_binding_W = weston_compositor_add_debug_binding(b->compositor,
								 KEY_W,
								 rdp_rail_dump_window_binding,
								 b);
	/* Trigger to enter debug key : CTRL+SHIFT+SPACE */
	weston_install_debug_key_binding(b->compositor, MODIFIER_CTRL);

	/* start listening surface creation */
	b->create_window_listener.notify = rdp_rail_create_window;
	wl_signal_add(&b->compositor->create_surface_signal,
		      &b->create_window_listener);

	return 0;
}

void
rdp_rail_destroy(struct rdp_backend *b)
{
	if (b->create_window_listener.notify) {
		wl_list_remove(&b->create_window_listener.link);
		b->create_window_listener.notify = NULL;
	}

	free(b->rdprail_shell_name);

	if (b->debug_binding_M)
		weston_binding_destroy(b->debug_binding_M);

	if (b->debug_binding_W)
		weston_binding_destroy(b->debug_binding_W);

#if defined(HAVE_FREERDP_RDPAPPLIST_H)
	if (b->libRDPApplistServer)
		dlclose(b->libRDPApplistServer);
#endif /* defined(HAVE_FREERDP_RDPAPPLIST_H) */

#if defined(HAVE_FREERDP_GFXREDIR_H)
	if (b->libFreeRDPServer)
		dlclose(b->libFreeRDPServer);
#endif /* defined(HAVE_FREERDP_GFXREDIR_H) */
}
