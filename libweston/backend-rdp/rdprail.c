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

#define RAIL_WINDOW_FULLSCREEN_STYLE (WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_GROUP | WS_TABSTOP)
#define RAIL_WINDOW_NORMAL_STYLE (RAIL_WINDOW_FULLSCREEN_STYLE | WS_THICKFRAME | WS_CAPTION)

extern PWtsApiFunctionTable FreeRDP_InitWtsApi(void);

static void rdp_rail_destroy_window(struct wl_listener *listener, void *data);
static void rdp_rail_schedule_update_window(struct wl_listener *listener, void *data);
static void rdp_rail_dump_window_label(struct weston_surface *surface, char *label, uint32_t label_size);

struct rdp_dispatch_data {
	struct rdp_loop_event_source _base_event_source;
	freerdp_peer *client;
	union {
		RAIL_SYSPARAM_ORDER u_sysParam;
		RAIL_SYSCOMMAND_ORDER u_sysCommand;
		RAIL_ACTIVATE_ORDER u_activate;
		RAIL_EXEC_ORDER u_exec;
		RAIL_WINDOW_MOVE_ORDER u_windowMove;
		RAIL_SNAP_ARRANGE u_snapArrange;
		RAIL_GET_APPID_REQ_ORDER u_getAppidReq;
		RAIL_LANGUAGEIME_INFO_ORDER u_languageImeInfo;
#ifdef HAVE_FREERDP_RDPAPPLIST_H
		RDPAPPLIST_CLIENT_CAPS_PDU u_appListCaps;
#endif // HAVE_FREERDP_RDPAPPLIST_H
	};
};

#define RDP_DISPATCH_TO_DISPLAY_LOOP(context, arg_type, arg, callback) \
	{ \
		freerdp_peer *client = (freerdp_peer*)(context)->custom; \
		RdpPeerContext *peerCtx = (RdpPeerContext *)client->context; \
		struct rdp_backend *b = peerCtx->rdpBackend; \
		struct rdp_dispatch_data *dispatch_data; \
		dispatch_data = (struct rdp_dispatch_data *)malloc(sizeof(*dispatch_data)); \
		if (dispatch_data) { \
			ASSERT_NOT_COMPOSITOR_THREAD(b); \
			dispatch_data->client = client; \
			dispatch_data->u_##arg_type = *(arg); \
			pthread_mutex_lock(&peerCtx->loop_event_source_list_mutex); \
			wl_list_insert(&peerCtx->loop_event_source_list, &dispatch_data->_base_event_source.link); \
			pthread_mutex_unlock(&peerCtx->loop_event_source_list_mutex); \
			if (!rdp_defer_rdp_task_to_display_loop( \
				peerCtx, callback, \
				dispatch_data, &dispatch_data->_base_event_source.event_source)) { \
				rdp_debug_error(b, "%s: rdp_queue_deferred_task failed\n", __func__); \
				pthread_mutex_lock(&peerCtx->loop_event_source_list_mutex); \
				wl_list_remove(&dispatch_data->_base_event_source.link); \
				pthread_mutex_unlock(&peerCtx->loop_event_source_list_mutex); \
				free(dispatch_data); \
			} \
		} else { \
			rdp_debug_error(b, "%s: malloc failed\n", __func__); \
		} \
	}

#define RDP_DISPATCH_DISPLAY_LOOP_COMPLETED(peerCtx, dispatch_data) \
	{ \
		ASSERT_COMPOSITOR_THREAD(peerCtx->rdpBackend); \
		rdp_defer_rdp_task_done(peerCtx); \
		assert(dispatch_data->_base_event_source.event_source); \
		wl_event_source_remove(dispatch_data->_base_event_source.event_source); \
		pthread_mutex_lock(&peerCtx->loop_event_source_list_mutex); \
		wl_list_remove(&dispatch_data->_base_event_source.link); \
		pthread_mutex_unlock(&peerCtx->loop_event_source_list_mutex); \
		free(dispatch_data); \
		return 0; \
	}

#ifdef HAVE_FREERDP_RDPAPPLIST_H
static int
applist_client_Caps_callback(int fd, uint32_t mask, void *arg)
{
	struct rdp_dispatch_data* data = (struct rdp_dispatch_data*)arg; 
	const RDPAPPLIST_CLIENT_CAPS_PDU* caps = &data->u_appListCaps;
	freerdp_peer *client = data->client;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	char clientLanguageId[RDPAPPLIST_LANG_SIZE + 1] = {}; // +1 to ensure null-terminate.

	rdp_debug(b, "Client AppList caps version:%d\n", caps->version);

	ASSERT_COMPOSITOR_THREAD(b);

	if (b->rdprail_shell_api &&
		b->rdprail_shell_api->start_app_list_update) {

		strncpy(clientLanguageId, caps->clientLanguageId, RDPAPPLIST_LANG_SIZE);
		rdp_debug(b, "Client AppList client language id: %s\n", clientLanguageId);

		peerCtx->isAppListEnabled =
			b->rdprail_shell_api->start_app_list_update(
				b->rdprail_shell_context,
				clientLanguageId);
	}

	RDP_DISPATCH_DISPLAY_LOOP_COMPLETED(peerCtx, data);
}

static UINT
applist_client_Caps(RdpAppListServerContext *context, const RDPAPPLIST_CLIENT_CAPS_PDU* arg)
{
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, appListCaps, arg, applist_client_Caps_callback);
	return CHANNEL_RC_OK;
}
#endif // HAVE_FREERDP_RDPAPPLIST_H

static UINT
rail_client_Handshake(RailServerContext* context, const RAIL_HANDSHAKE_ORDER* handshake)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;

	rdp_debug(b, "Client HandShake buildNumber:%d\n", handshake->buildNumber);

	peerCtx->handshakeCompleted = TRUE;
	return CHANNEL_RC_OK;
}

static void
rail_ClientExec_destroy(struct wl_listener *listener, void *data)
{
	RdpPeerContext *peerCtx = container_of(listener, RdpPeerContext,
			clientExec_destroy_listener);
	struct rdp_backend *b = peerCtx->rdpBackend;

	rdp_debug(b, "Client ExecOrder program terminated\n");

	wl_list_remove(&peerCtx->clientExec_destroy_listener.link);
	peerCtx->clientExec_destroy_listener.notify = NULL;
	peerCtx->clientExec = NULL;
}

static int
rail_client_Exec_callback(int fd, uint32_t mask, void *arg)
{
	struct rdp_dispatch_data* data = (struct rdp_dispatch_data*)arg; 
	const RAIL_EXEC_ORDER* exec = &data->u_exec;
	freerdp_peer *client = data->client;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	UINT result = RAIL_EXEC_E_FAIL; 
	RAIL_EXEC_RESULT_ORDER orderResult = {};
	char *remoteProgramAndArgs = exec->RemoteApplicationProgram;

	rdp_debug(b, "Client ExecOrder:0x%08X, Program:%s, WorkingDir:%s, RemoteApplicationArguments:%s\n",
		 (UINT)exec->flags,
		 exec->RemoteApplicationProgram,
		 exec->RemoteApplicationWorkingDir,
		 exec->RemoteApplicationArguments);

	ASSERT_COMPOSITOR_THREAD(peerCtx->rdpBackend);

	if (exec->RemoteApplicationProgram) {
		if (!utf8_string_to_rail_string(exec->RemoteApplicationProgram, &orderResult.exeOrFile))
			goto send_result;

		if (exec->RemoteApplicationArguments) {
			/* construct remote program path and arguments */
			remoteProgramAndArgs = malloc(strlen(exec->RemoteApplicationProgram) +
				strlen(exec->RemoteApplicationArguments) +
				2); // space between program and args + null terminate.
			if (!remoteProgramAndArgs)
				goto send_result;
			sprintf(remoteProgramAndArgs, "%s %s", exec->RemoteApplicationProgram, exec->RemoteApplicationArguments);
		}

		/* TODO: server state machine, wait until activation complated */
		while (!peerCtx->activationRailCompleted)
			USleep(10000);

		/* launch the process specified by RDP client. */
		rdp_debug(b, "Client ExecOrder launching %s\n", remoteProgramAndArgs);
		if (b->rdprail_shell_api &&
			b->rdprail_shell_api->request_launch_shell_process) {
			peerCtx->clientExec =
				b->rdprail_shell_api->request_launch_shell_process(
					b->rdprail_shell_context, remoteProgramAndArgs);
		}
		if (peerCtx->clientExec) {
			assert(NULL == peerCtx->clientExec_destroy_listener.notify);
			peerCtx->clientExec_destroy_listener.notify = rail_ClientExec_destroy;
			wl_client_add_destroy_listener(peerCtx->clientExec,
				&peerCtx->clientExec_destroy_listener);
			result = RAIL_EXEC_S_OK;
		} else {
			rdp_debug_error(b, "%s: fail to launch shell process %s\n",
				__func__, remoteProgramAndArgs);
		}
	}

send_result:
	orderResult.flags = exec->flags;
	orderResult.execResult = result;
	orderResult.rawResult = 0;
	peerCtx->rail_server_context->ServerExecResult(peerCtx->rail_server_context, &orderResult);

	if (orderResult.exeOrFile.string)
		free(orderResult.exeOrFile.string);
	if (remoteProgramAndArgs && remoteProgramAndArgs != exec->RemoteApplicationProgram)
		free(remoteProgramAndArgs);
	if (exec->RemoteApplicationProgram)
		free(exec->RemoteApplicationProgram);
	if (exec->RemoteApplicationWorkingDir)
		free(exec->RemoteApplicationWorkingDir);
	if (exec->RemoteApplicationArguments)
		free(exec->RemoteApplicationArguments);

	RDP_DISPATCH_DISPLAY_LOOP_COMPLETED(peerCtx, data);
}

static UINT
rail_client_Exec(RailServerContext* context, const RAIL_EXEC_ORDER* arg) 
{
	RAIL_EXEC_ORDER execOrder = {};
	execOrder.flags = arg->flags;
	if (arg->RemoteApplicationProgram) {
		execOrder.RemoteApplicationProgram = malloc(strlen(arg->RemoteApplicationProgram)+1);
		if (!execOrder.RemoteApplicationProgram)
			goto Exit_Error;
		strcpy(execOrder.RemoteApplicationProgram, arg->RemoteApplicationProgram);
	}
	if (arg->RemoteApplicationWorkingDir) {
		execOrder.RemoteApplicationWorkingDir = malloc(strlen(arg->RemoteApplicationWorkingDir)+1);
		if (!execOrder.RemoteApplicationWorkingDir)
			goto Exit_Error;
		strcpy(execOrder.RemoteApplicationWorkingDir, arg->RemoteApplicationWorkingDir);
	}
	if (arg->RemoteApplicationArguments) {
		execOrder.RemoteApplicationArguments = malloc(strlen(arg->RemoteApplicationArguments)+1);
		if (!execOrder.RemoteApplicationArguments)
			goto Exit_Error;
		strcpy(execOrder.RemoteApplicationArguments, arg->RemoteApplicationArguments);
	}
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, exec, &execOrder, rail_client_Exec_callback);
	return CHANNEL_RC_OK;

Exit_Error:
	if (execOrder.RemoteApplicationProgram)
		free(execOrder.RemoteApplicationProgram);
	if (execOrder.RemoteApplicationWorkingDir)
		free(execOrder.RemoteApplicationWorkingDir);
	if (execOrder.RemoteApplicationArguments)
		free(execOrder.RemoteApplicationArguments);
	return CHANNEL_RC_NO_BUFFER;
}

static int
rail_client_Activate_callback(int fd, uint32_t mask, void *arg)
{
	struct rdp_dispatch_data* data = (struct rdp_dispatch_data*)arg; 
	const RAIL_ACTIVATE_ORDER* activate = &data->u_activate;
	freerdp_peer *client = data->client;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct weston_surface *surface = NULL;

	rdp_debug_verbose(b, "Client: ClientActivate: WindowId:0x%x, enabled:%d\n", activate->windowId, activate->enabled);

	ASSERT_COMPOSITOR_THREAD(b);

	if (b->rdprail_shell_api &&
		b->rdprail_shell_api->request_window_activate &&
		b->rdprail_shell_context) {
		if (activate->windowId && activate->enabled) {
			surface = (struct weston_surface *)hash_table_lookup(peerCtx->windowId.hash_table, activate->windowId);
			if (!surface)
				rdp_debug_error(b, "Client: ClientActivate: WindowId:0x%x is not found.\n", activate->windowId);
		}
		b->rdprail_shell_api->request_window_activate(b->rdprail_shell_context, peerCtx->item.seat, surface);
	}

	RDP_DISPATCH_DISPLAY_LOOP_COMPLETED(peerCtx, data);
}

static UINT
rail_client_Activate(RailServerContext* context, const RAIL_ACTIVATE_ORDER* arg)
{
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, activate, arg, rail_client_Activate_callback);
	return CHANNEL_RC_OK;
}

static int
rail_client_SnapArrange_callback(int fd, uint32_t mask, void *arg)
{
	struct rdp_dispatch_data* data = (struct rdp_dispatch_data*)arg; 
	const RAIL_SNAP_ARRANGE* snap = &data->u_snapArrange;
	freerdp_peer *client = data->client;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct weston_surface *surface;
	struct weston_surface_rail_state *rail_state;

	rdp_debug(b, "SnapArrange(%d) - (%d, %d, %d, %d)\n", 
		snap->windowId,
		snap->left,
		snap->top,
		snap->right - snap->left,
		snap->bottom - snap->top);

	ASSERT_COMPOSITOR_THREAD(b);

	surface = (struct weston_surface *)hash_table_lookup(peerCtx->windowId.hash_table, snap->windowId);
	if (surface) {
		rail_state = (struct weston_surface_rail_state *)surface->backend_state;
		if (b->rdprail_shell_api &&
			b->rdprail_shell_api->request_window_move) {
			/* TODO: HI-DPI MULTIMON */
			b->rdprail_shell_api->request_window_snap(surface, 
				to_weston_x(peerCtx, snap->left), 
				to_weston_y(peerCtx, snap->top),
				snap->right - snap->left,
				snap->bottom - snap->top);
		}

		rail_state->forceUpdateWindowState = true;
		rdp_rail_schedule_update_window(NULL, (void*)surface);
	}

	RDP_DISPATCH_DISPLAY_LOOP_COMPLETED(peerCtx, data);
}

static UINT
rail_client_SnapArrange(RailServerContext* context, const RAIL_SNAP_ARRANGE* arg)
{
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, snapArrange, arg, rail_client_SnapArrange_callback);
	return CHANNEL_RC_OK;
}

static int
rail_client_WindowMove_callback(int fd, uint32_t mask, void *arg)
{
	struct rdp_dispatch_data* data = (struct rdp_dispatch_data*)arg; 
	const RAIL_WINDOW_MOVE_ORDER* windowMove = &data->u_windowMove;
	freerdp_peer *client = data->client;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct weston_surface *surface;

	rdp_debug(b, "WindowMove(%d) - (%d, %d, %d, %d)\n", 
		windowMove->windowId,
		windowMove->left,
		windowMove->top,
		windowMove->right - windowMove->left,
		windowMove->bottom - windowMove->top);

	ASSERT_COMPOSITOR_THREAD(b);

	surface = (struct weston_surface *)hash_table_lookup(peerCtx->windowId.hash_table, windowMove->windowId);
	if (surface) {
		if (b->rdprail_shell_api &&
			b->rdprail_shell_api->request_window_move) {
			/* TODO: HI-DPI MULTIMON */
			b->rdprail_shell_api->request_window_move(surface, 
				to_weston_x(peerCtx, windowMove->left), 
				to_weston_y(peerCtx, windowMove->top));
		}
	}

	rdp_debug(b, "Surface Size (%d, %d)\n", surface->width, surface->height);

	RDP_DISPATCH_DISPLAY_LOOP_COMPLETED(peerCtx, data);
}

static UINT 
rail_client_WindowMove(RailServerContext* context, const RAIL_WINDOW_MOVE_ORDER* arg)
{
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, windowMove, arg, rail_client_WindowMove_callback);
	return CHANNEL_RC_OK;
}

static int
rail_client_Syscommand_callback(int fd, uint32_t mask, void *arg)
{
	struct rdp_dispatch_data* data = (struct rdp_dispatch_data*)arg; 
	const RAIL_SYSCOMMAND_ORDER* syscommand = &data->u_sysCommand;
	freerdp_peer *client = data->client;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct weston_surface* surface;

	ASSERT_COMPOSITOR_THREAD(b);

	surface = (struct weston_surface *)hash_table_lookup(peerCtx->windowId.hash_table, syscommand->windowId);
	if (!surface) {
		rdp_debug_error(b, "Client: ClientSyscommand: WindowId:0x%x is not found.\n", syscommand->windowId);
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
		if (b->rdprail_shell_api &&
			b->rdprail_shell_api->request_window_minimize)
			b->rdprail_shell_api->request_window_minimize(surface);
		break;
	case SC_MAXIMIZE:
		commandString = "SC_MAXIMIZE";
		if (b->rdprail_shell_api &&
			b->rdprail_shell_api->request_window_maximize)
			b->rdprail_shell_api->request_window_maximize(surface);
		break;
	case SC_CLOSE:
		commandString = "SC_CLOSE";
		if (b->rdprail_shell_api &&
			b->rdprail_shell_api->request_window_close)
			b->rdprail_shell_api->request_window_close(surface);
		break;
	case SC_KEYMENU:
		commandString = "SC_KEYMENU";
		break;
	case SC_RESTORE:
		commandString = "SC_RESTORE";
		if (b->rdprail_shell_api &&
			b->rdprail_shell_api->request_window_restore)
			b->rdprail_shell_api->request_window_restore(surface);
		break;
	case SC_DEFAULT:
		commandString = "SC_DEFAULT";
		break;
	default:
		commandString = "Unknown";
		break;
	}

	rdp_debug(b, "Client: ClientSyscommand: WindowId:0x%x, surface:0x%p, command:%s (0x%x)\n",
		 syscommand->windowId, surface, commandString, syscommand->command);

Exit:
	RDP_DISPATCH_DISPLAY_LOOP_COMPLETED(peerCtx, data);
}

static UINT
rail_client_Syscommand(RailServerContext* context, const RAIL_SYSCOMMAND_ORDER* arg)
{
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, sysCommand, arg, rail_client_Syscommand_callback);
	return CHANNEL_RC_OK;
}

static int
rail_client_ClientSysparam_callback(int fd, uint32_t mask, void *arg)
{
	struct rdp_dispatch_data* data = (struct rdp_dispatch_data*)arg; 
	const RAIL_SYSPARAM_ORDER* sysparam = &data->u_sysParam;
	freerdp_peer *client = data->client;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	pixman_rectangle32_t workareaRect;
	pixman_rectangle32_t workareaRectClient;
	struct weston_output *base_output;
	struct weston_head *base_head_iter;

	ASSERT_COMPOSITOR_THREAD(b);

	if (sysparam->params & SPI_MASK_SET_DRAG_FULL_WINDOWS) {
		rdp_debug(b, "Client: ClientSysparam: dragFullWindows:%d\n", sysparam->dragFullWindows);
	}

	if (sysparam->params & SPI_MASK_SET_KEYBOARD_CUES) {
		rdp_debug(b, "Client: ClientSysparam: keyboardCues:%d\n", sysparam->keyboardCues);
	}

	if (sysparam->params & SPI_MASK_SET_KEYBOARD_PREF) {
		rdp_debug(b, "Client: ClientSysparam: keyboardPref:%d\n", sysparam->keyboardPref);
	}

	if (sysparam->params & SPI_MASK_SET_MOUSE_BUTTON_SWAP) {
		rdp_debug(b, "Client: ClientSysparam: mouseButtonSwap:%d\n", sysparam->mouseButtonSwap);
		peerCtx->mouseButtonSwap = sysparam->mouseButtonSwap;
	}

	if (sysparam->params & SPI_MASK_SET_WORK_AREA) {
		rdp_debug(b, "Client: ClientSysparam: workArea:(left:%d, top:%d, right:%d, bottom:%d)\n", 
			(INT32)(INT16)sysparam->workArea.left,
			(INT32)(INT16)sysparam->workArea.top,
			(INT32)(INT16)sysparam->workArea.right,
			(INT32)(INT16)sysparam->workArea.bottom);
	}

	if (sysparam->params & SPI_MASK_DISPLAY_CHANGE) {
		rdp_debug(b, "Client: ClientSysparam: displayChange:(left:%d, top:%d, right:%d, bottom:%d)\n", 
			(INT32)(INT16)sysparam->displayChange.left,
			(INT32)(INT16)sysparam->displayChange.top,
			(INT32)(INT16)sysparam->displayChange.right,
			(INT32)(INT16)sysparam->displayChange.bottom);
	}

	if (sysparam->params & SPI_MASK_TASKBAR_POS) {
		rdp_debug(b, "Client: ClientSysparam: taskbarPos:(left:%d, top:%d, right:%d, bottom:%d)\n", 
			(INT32)(INT16)sysparam->taskbarPos.left,
			(INT32)(INT16)sysparam->taskbarPos.top,
			(INT32)(INT16)sysparam->taskbarPos.right,
			(INT32)(INT16)sysparam->taskbarPos.bottom);
	}

	if (sysparam->params & SPI_MASK_SET_HIGH_CONTRAST) {
		rdp_debug(b, "Client: ClientSysparam: highContrast\n");
	}

	if (sysparam->params & SPI_MASK_SET_CARET_WIDTH) {
		rdp_debug(b, "Client: ClientSysparam: caretWidth:%d\n", sysparam->caretWidth);
	}

	if (sysparam->params & SPI_MASK_SET_STICKY_KEYS) {
		rdp_debug(b, "Client: ClientSysparam: stickyKeys:%d\n", sysparam->stickyKeys);
	}

	if (sysparam->params & SPI_MASK_SET_TOGGLE_KEYS) {
		rdp_debug(b, "Client: ClientSysparam: toggleKeys:%d\n", sysparam->toggleKeys);
	}

	if (sysparam->params & SPI_MASK_SET_FILTER_KEYS) {
		rdp_debug(b, "Client: ClientSysparam: filterKeys\n");
	}

	if (sysparam->params & SPI_MASK_SET_SCREEN_SAVE_ACTIVE) {
		rdp_debug(b, "Client: ClientSysparam: setScreenSaveActive:%d\n", sysparam->setScreenSaveActive);
	}

	if (sysparam->params & SPI_MASK_SET_SET_SCREEN_SAVE_SECURE) {
		rdp_debug(b, "Client: ClientSysparam: setScreenSaveSecure:%d\n", sysparam->setScreenSaveSecure);
	}

	if (sysparam->params & SPI_MASK_SET_WORK_AREA) {
		if (b->rdprail_shell_api &&
			b->rdprail_shell_api->set_desktop_workarea) {
			workareaRectClient.x = (INT32)(INT16)sysparam->workArea.left;
			workareaRectClient.y = (INT32)(INT16)sysparam->workArea.top;
			workareaRectClient.width = (INT32)(INT16)sysparam->workArea.right - workareaRectClient.x;
			workareaRectClient.height = (INT32)(INT16)sysparam->workArea.bottom - workareaRectClient.y;
			/* Workarea is reported in client coordinate where primary monitor' upper-left is (0,0). */
			/* traslate to weston coordinate where entire desktop's upper-left is (0,0). */
			workareaRect = workareaRectClient;
			base_output = to_weston_coordinate(peerCtx,
				&workareaRect.x, &workareaRect.y,
				&workareaRect.width, &workareaRect.height);
			if (base_output) {
				rdp_debug(b, "Translated workarea:(%d,%d)-(%d,%d) at %s:(%d,%d)-(%d,%d)\n",
					workareaRect.x, workareaRect.y,
					workareaRect.x + workareaRect.width, workareaRect.y + workareaRect.height,
					base_output->name,
					base_output->x, base_output->y,
					base_output->x + base_output->width, base_output->y + base_output->height);
				b->rdprail_shell_api->set_desktop_workarea(base_output, b->rdprail_shell_context, &workareaRect);
				wl_list_for_each(base_head_iter, &base_output->head_list, output_link) {
					to_rdp_head(base_head_iter)->workarea = workareaRect;
					to_rdp_head(base_head_iter)->workareaClient = workareaRectClient;
				}
			} else {
				rdp_debug_error(b, "Client: ClientSysparam: workArea isn't belonging to an output\n");
			}
		}
	}

	RDP_DISPATCH_DISPLAY_LOOP_COMPLETED(peerCtx, data);
}

static UINT
rail_client_ClientSysparam(RailServerContext* context, const RAIL_SYSPARAM_ORDER* arg)
{
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, sysParam, arg, rail_client_ClientSysparam_callback);
	return CHANNEL_RC_OK;
}

static int
rail_client_ClientGetAppidReq_callback(int fd, uint32_t mask, void *arg)
{
	struct rdp_dispatch_data* data = (struct rdp_dispatch_data*)arg; 
	const RAIL_GET_APPID_REQ_ORDER* getAppidReq = &data->u_getAppidReq;
	freerdp_peer *client = data->client;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	char appId[520] = {};
	char imageName[520] = {};
	pid_t pid;
	size_t i;
	unsigned short *p;
	struct weston_surface *surface;

	rdp_debug_verbose(b, "Client: ClientGetAppidReq: WindowId:0x%x\n", getAppidReq->windowId);

	ASSERT_COMPOSITOR_THREAD(b);

	if (b->rdprail_shell_api &&
		b->rdprail_shell_api->get_window_app_id) {

		surface = (struct weston_surface *)hash_table_lookup(peerCtx->windowId.hash_table, getAppidReq->windowId);
		if (!surface) {
			rdp_debug_error(b, "Client: ClientGetAppidReq: WindowId:0x%x is not found.\n", getAppidReq->windowId);
			goto Exit;
		}

		pid = b->rdprail_shell_api->get_window_app_id(b->rdprail_shell_context,
					surface, &appId[0], sizeof(appId), &imageName[0], sizeof(imageName));
		if (appId[0] == '\0') {
			rdp_debug_error(b, "Client: ClientGetAppidReq: WindowId:0x%x does not have appId, or not top level window.\n", getAppidReq->windowId);
			goto Exit;
		}

		rdp_debug(b, "Client: ClientGetAppidReq: pid:%d appId:%s\n", (UINT32)pid, appId);
		rdp_debug_verbose(b, "Client: ClientGetAppidReq: pid:%d imageName:%s\n", (UINT32)pid, imageName);

		/* Reply with RAIL_GET_APPID_RESP_EX when pid/imageName is valid and client supports it */
		if ((pid >= 0) && (imageName[0] != '\0') &&
			peerCtx->clientStatusFlags & TS_RAIL_CLIENTSTATUS_GET_APPID_RESPONSE_EX_SUPPORTED) {
			RAIL_GET_APPID_RESP_EX getAppIdRespEx = {};
			getAppIdRespEx.windowID = getAppidReq->windowId;
			for (i = 0, p = &getAppIdRespEx.applicationID[0]; i < strlen(appId); i++, p++)
				*p = (unsigned short)appId[i];
			getAppIdRespEx.processId = (UINT32) pid;
			for (i = 0, p = &getAppIdRespEx.processImageName[0]; i < strlen(imageName); i++, p++)
				*p = (unsigned short)imageName[i];
			peerCtx->rail_server_context->ServerGetAppidRespEx(peerCtx->rail_server_context, &getAppIdRespEx);
		} else {
			RAIL_GET_APPID_RESP_ORDER getAppIdResp = {};
			getAppIdResp.windowId = getAppidReq->windowId;
			for (i = 0, p = &getAppIdResp.applicationId[0]; i < strlen(appId); i++, p++)
				*p = (unsigned short)appId[i];
			peerCtx->rail_server_context->ServerGetAppidResp(peerCtx->rail_server_context, &getAppIdResp);
		}
	}

Exit:
	RDP_DISPATCH_DISPLAY_LOOP_COMPLETED(peerCtx, data);
}

static UINT
rail_client_ClientGetAppidReq(RailServerContext* context,
                              const RAIL_GET_APPID_REQ_ORDER* arg)
{
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, getAppidReq, arg, rail_client_ClientGetAppidReq_callback);
	return CHANNEL_RC_OK;
}

static UINT
rail_client_ClientStatus(RailServerContext* context,
                              const RAIL_CLIENT_STATUS_ORDER* clientStatus)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;

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

	peerCtx->clientStatusFlags = clientStatus->flags;
	return CHANNEL_RC_OK;
}

static UINT
rail_client_LangbarInfo(RailServerContext* context,
				const RAIL_LANGBAR_INFO_ORDER* langbarInfo)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;

	rdp_debug(b, "Client: LangbarInfo: LanguageBarStatus:%d\n", langbarInfo->languageBarStatus);

	return CHANNEL_RC_OK;
}

static char *
languageGuid_to_string(const GUID *guid)
{
	typedef struct _lang_GUID
	{
		UINT32 Data1;
		UINT16 Data2;
		UINT16 Data3;
		BYTE Data4_0;
		BYTE Data4_1;
		BYTE Data4_2;
		BYTE Data4_3;
		BYTE Data4_4;
		BYTE Data4_5;
		BYTE Data4_6;
		BYTE Data4_7;
	} lang_GUID;

	static const lang_GUID c_GUID_NULL = GUID_NULL;
	static const lang_GUID c_GUID_JPNIME = GUID_MSIME_JPN;
	static const lang_GUID c_GUID_KORIME = GUID_MSIME_KOR;
	static const lang_GUID c_GUID_CHSIME = GUID_CHSIME;
	static const lang_GUID c_GUID_CHTIME = GUID_CHTIME;
	static const lang_GUID c_GUID_PROFILE_NEWPHONETIC = GUID_PROFILE_NEWPHONETIC;
	static const lang_GUID c_GUID_PROFILE_CHANGJIE = GUID_PROFILE_CHANGJIE;
	static const lang_GUID c_GUID_PROFILE_QUICK = GUID_PROFILE_QUICK;
	static const lang_GUID c_GUID_PROFILE_CANTONESE = GUID_PROFILE_CANTONESE;
	static const lang_GUID c_GUID_PROFILE_PINYIN = GUID_PROFILE_PINYIN;
	static const lang_GUID c_GUID_PROFILE_SIMPLEFAST = GUID_PROFILE_SIMPLEFAST;
	static const lang_GUID c_GUID_PROFILE_MSIME_JPN = GUID_GUID_PROFILE_MSIME_JPN;
	static const lang_GUID c_GUID_PROFILE_MSIME_KOR = GUID_PROFILE_MSIME_KOR;

	RPC_STATUS rpc_status;
	if (UuidEqual(guid, (GUID *)&c_GUID_NULL, &rpc_status))
		return "GUID_NULL";
	else if (UuidEqual(guid, (GUID *)&c_GUID_JPNIME, &rpc_status))
		return "GUID_JPNIME";
	else if (UuidEqual(guid, (GUID *)&c_GUID_KORIME, &rpc_status))
		return "GUID_KORIME";
	else if (UuidEqual(guid, (GUID *)&c_GUID_CHSIME, &rpc_status))
		return "GUID_CHSIME";
	else if (UuidEqual(guid, (GUID *)&c_GUID_CHTIME, &rpc_status))
		return "GUID_CHTIME";
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
	else if (UuidEqual(guid, (GUID *)&c_GUID_PROFILE_MSIME_KOR, &rpc_status))
		return "GUID_PROFILE_MSIME_KOR";
	else
		return "Unknown GUID";
}

static int
rail_client_LanguageImeInfo_callback(int fd, uint32_t mask, void *arg)
{
	struct rdp_dispatch_data* data = (struct rdp_dispatch_data*)arg; 
	const RAIL_LANGUAGEIME_INFO_ORDER* languageImeInfo = &data->u_languageImeInfo;
	freerdp_peer *client = data->client;
	rdpSettings *settings = client->settings;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	UINT32 new_keyboard_layout = 0;
	struct xkb_keymap *keymap = NULL;
	struct xkb_rule_names xkbRuleNames;
	char *s;

	ASSERT_COMPOSITOR_THREAD(b);

	switch (languageImeInfo->ProfileType)
	{
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
	rdp_debug(b, "Client: LanguageImeInfo: ProfileType: %d (%s)\n", languageImeInfo->ProfileType, s);
	rdp_debug(b, "Client: LanguageImeInfo: LanguageID: 0x%x\n", languageImeInfo->LanguageID);
	rdp_debug(b, "Client: LanguageImeInfo: LanguageProfileCLSID: %s\n",
			languageGuid_to_string(&languageImeInfo->LanguageProfileCLSID));
	rdp_debug(b, "Client: LanguageImeInfo: ProfileGUID: %s\n",
			languageGuid_to_string(&languageImeInfo->ProfileGUID));
	rdp_debug(b, "Client: LanguageImeInfo: KeyboardLayout: 0x%x\n", languageImeInfo->KeyboardLayout);

	if (languageImeInfo->ProfileType == TF_PROFILETYPE_KEYBOARDLAYOUT) {
		new_keyboard_layout = languageImeInfo->KeyboardLayout;
	} else if (languageImeInfo->ProfileType == TF_PROFILETYPE_INPUTPROCESSOR) {
		typedef struct _lang_GUID
		{
			UINT32 Data1;
			UINT16 Data2;
			UINT16 Data3;
			BYTE Data4_0;
			BYTE Data4_1;
			BYTE Data4_2;
			BYTE Data4_3;
			BYTE Data4_4;
			BYTE Data4_5;
			BYTE Data4_6;
			BYTE Data4_7;
		} lang_GUID;

		static const lang_GUID c_GUID_JPNIME = GUID_MSIME_JPN;
		static const lang_GUID c_GUID_KORIME = GUID_MSIME_KOR;
		static const lang_GUID c_GUID_CHSIME = GUID_CHSIME;
		static const lang_GUID c_GUID_CHTIME = GUID_CHTIME;

		RPC_STATUS rpc_status;
		if (UuidEqual(&languageImeInfo->LanguageProfileCLSID,
			(GUID *)&c_GUID_JPNIME, &rpc_status))
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
		else 
			new_keyboard_layout = KBD_US;
	}

	if (new_keyboard_layout && (new_keyboard_layout != settings->KeyboardLayout)) {
		convert_rdp_keyboard_to_xkb_rule_names(settings->KeyboardType,
						       settings->KeyboardSubType,
						       new_keyboard_layout,
						       &xkbRuleNames);
		if (xkbRuleNames.layout) {
			keymap = xkb_keymap_new_from_names(b->compositor->xkb_context,
							   &xkbRuleNames, 0);
			if (keymap) {
				weston_seat_update_keymap(peerCtx->item.seat, keymap);
				xkb_keymap_unref(keymap);
				settings->KeyboardLayout = new_keyboard_layout;
			}
		}
		if (!keymap) {
			rdp_debug_error(b, "%s: Failed to switch to kbd_layout:0x%x kbd_type:0x%x kbd_subType:0x%x\n",
				__func__, new_keyboard_layout, settings->KeyboardType, settings->KeyboardSubType);
		}
	}

	RDP_DISPATCH_DISPLAY_LOOP_COMPLETED(peerCtx, data);
}

static UINT
rail_client_LanguageImeInfo(RailServerContext* context,
				const RAIL_LANGUAGEIME_INFO_ORDER* arg)
{
	RDP_DISPATCH_TO_DISPLAY_LOOP(context, languageImeInfo, arg, rail_client_LanguageImeInfo_callback);
	return CHANNEL_RC_OK;
}

static UINT
rail_client_CompartmentInfo(RailServerContext* context,
				const RAIL_COMPARTMENT_INFO_ORDER* compartmentInfo)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;

	rdp_debug(b, "Client: CompartmentInfo: ImeStatus: %s\n", compartmentInfo->ImeState ? "OPEN" : "CLOSED");
	rdp_debug(b, "Client: CompartmentInfo: ImeConvMode: 0x%x\n", compartmentInfo->ImeConvMode);
	rdp_debug(b, "Client: CompartmentInfo: ImeSentenceMode: 0x%x\n", compartmentInfo->ImeSentenceMode);
	rdp_debug(b, "Client: CompartmentInfo: KanaMode: %s\n", compartmentInfo->KanaMode ? "ON" : "OFF");

	return CHANNEL_RC_OK;
}

static UINT
rail_grfx_client_caps_advertise(RdpgfxServerContext* context, const RDPGFX_CAPS_ADVERTISE_PDU* capsAdvertise)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	rdp_debug(b, "Client: GrfxCaps count:0x%x\n", capsAdvertise->capsSetCount);
	for (int i = 0; i < capsAdvertise->capsSetCount; i++) {
		RDPGFX_CAPSET* capsSet = &(capsAdvertise->capsSets[i]);
		rdp_debug(b, "Client: GrfxCaps[%d] version:0x%x length:%d flags:0x%x\n",
					 i, capsSet->version, capsSet->length, capsSet->flags);
		switch(capsSet->version)
		{
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
		 		
		switch(capsSet->version)
		{
 		case RDPGFX_CAPVERSION_8:
		{
		 	//RDPGFX_CAPSET_VERSION8 *caps8 = (RDPGFX_CAPSET_VERSION8 *)capsSet;
			break;
		}
 		case RDPGFX_CAPVERSION_81:
		{ 
		 	//RDPGFX_CAPSET_VERSION81 *caps81 = (RDPGFX_CAPSET_VERSION81 *)capsSet;
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
			//RDPGFX_CAPSET_VERSION10 *caps10 = (RDPGFX_CAPSET_VERSION10 *)capsSet;
			break;
		}
		default:
			rdp_debug_error(b, "	Version : UNKNOWN(%d)\n", capsSet->version);
		}
	}

	/* send caps confirm */
	RDPGFX_CAPS_CONFIRM_PDU capsConfirm = {};
	capsConfirm.capsSet = capsAdvertise->capsSets; // TODO: choose right one.
	peerCtx->rail_grfx_server_context->CapsConfirm(peerCtx->rail_grfx_server_context, &capsConfirm);

	/* ready to use graphics channel */
	peerCtx->activationGraphicsCompleted = TRUE;
	return CHANNEL_RC_OK;
}

static UINT
rail_grfx_client_cache_import_offer(RdpgfxServerContext* context, const RDPGFX_CACHE_IMPORT_OFFER_PDU* cacheImportOffer)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	rdp_debug_verbose(b, "Client: GrfxCacheImportOffer\n");
	return CHANNEL_RC_OK;
}

static UINT
rail_grfx_client_frame_acknowledge(RdpgfxServerContext* context, const RDPGFX_FRAME_ACKNOWLEDGE_PDU* frameAcknowledge)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	rdp_debug_verbose(b, "Client: GrfxFrameAcknowledge(queueDepth = 0x%x, frameId = 0x%x, decodedFrame = %d)\n",
			  frameAcknowledge->queueDepth, frameAcknowledge->frameId, frameAcknowledge->totalFramesDecoded);
	peerCtx->acknowledgedFrameId = frameAcknowledge->frameId;
	peerCtx->isAcknowledgedSuspended = (frameAcknowledge->queueDepth == 0xffffffff);
	return CHANNEL_RC_OK;
}

#ifdef HAVE_FREERDP_GFXREDIR_H
static UINT
gfxredir_client_graphics_redirection_legacy_caps(GfxRedirServerContext* context, const GFXREDIR_LEGACY_CAPS_PDU* redirectionCaps)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;

	rdp_debug(b, "Client: gfxredir_caps: version:%d\n", redirectionCaps->version);
	/* This is legacy caps callback, version must be 1 */
	if (redirectionCaps->version != GFXREDIR_CHANNEL_VERSION_LEGACY) {
		rdp_debug_error(b, "Client: gfxredir_caps: invalid version:%d\n", redirectionCaps->version);
		return ERROR_INTERNAL_ERROR;  
	}

	/* Legacy version 1 client is not supported, so don't set 'activationGraphicsRedirectionCompleted'. */
	rdp_debug_error(b, "Client: gfxredir_caps: version 1 is not supported.\n");

	return CHANNEL_RC_OK;
}

static UINT
gfxredir_client_graphics_redirection_caps_advertise(GfxRedirServerContext* context, const GFXREDIR_CAPS_ADVERTISE_PDU* redirectionCaps)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	const GFXREDIR_CAPS_HEADER *current = (const GFXREDIR_CAPS_HEADER *)redirectionCaps->caps;
	const GFXREDIR_CAPS_V2_0_PDU *capsV2 = NULL;

	/* dump client caps */
	uint32_t i = 0;
	uint32_t length = redirectionCaps->length;
	rdp_debug(b, "Client: gfxredir_caps: length:%d\n", redirectionCaps->length);
	while (length <= redirectionCaps->length && 
	       length >= sizeof(GFXREDIR_CAPS_HEADER)) {
		rdp_debug(b, "Client: gfxredir_caps[%d]: signature:0x%x\n", i, current->signature);
		rdp_debug(b, "Client: gfxredir_caps[%d]: version:0x%x\n", i, current->version);
		rdp_debug(b, "Client: gfxredir_caps[%d]: length:%d\n", i, current->length);
		if (current->version == GFXREDIR_CAPS_VERSION2_0) {
			capsV2 = (GFXREDIR_CAPS_V2_0_PDU *)current;
			rdp_debug(b, "Client: gfxredir_caps[%d]: supportedFeatures:0x%x\n", i, capsV2->supportedFeatures);
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

		rdp_debug(b, "Client: gfxredir selected caps: version:0x%x\n", selected->version);

		confirmPdu.version = selected->version; /* return the version of selected caps */
		confirmPdu.length = selected->length; /* must return same length as selected caps from advertised */
		confirmPdu.capsData = (const BYTE*)(selected+1); /* return caps data in selected caps */

		peerCtx->gfxredir_server_context->GraphicsRedirectionCapsConfirm(context, &confirmPdu);
	}

	/* ready to use graphics redirection channel */
	peerCtx->activationGraphicsRedirectionCompleted = TRUE;
	return CHANNEL_RC_OK;
}

static UINT
gfxredir_client_present_buffer_ack(GfxRedirServerContext* context, const GFXREDIR_PRESENT_BUFFER_ACK_PDU* presentAck)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct weston_surface *surface;
	struct weston_surface_rail_state *rail_state;

	rdp_debug_verbose(b, "Client: gfxredir_present_buffer_ack: windowId:0x%lx\n", presentAck->windowId);
	rdp_debug_verbose(b, "Client: gfxredir_present_buffer_ack: presentId:0x%lx\n", presentAck->presentId);

	peerCtx->acknowledgedFrameId = (UINT32)presentAck->presentId;

	surface = (struct weston_surface *)hash_table_lookup(peerCtx->windowId.hash_table, presentAck->windowId);
	if (surface) {
		rail_state = (struct weston_surface_rail_state *)surface->backend_state;
		rail_state->isUpdatePending = FALSE;
	} else {
		rdp_debug_error(b, "Client: PresentBufferAck: WindowId:0x%lx is not found.\n", presentAck->windowId);
	}

	return CHANNEL_RC_OK;
}
#endif // HAVE_FREERDP_GFXREDIR_H

static int
rdp_rail_create_cursor(struct weston_surface *surface)
{
	struct weston_compositor *compositor = surface->compositor;
	struct rdp_backend *b = (struct rdp_backend*)compositor->backend;
	RdpPeerContext *peerCtx = (RdpPeerContext *)b->rdp_peer->context;

	ASSERT_COMPOSITOR_THREAD(b);

	if (peerCtx->cursorSurface)
		rdp_debug_error(b, "cursor surface already exists old %p vs new %p\n", peerCtx->cursorSurface, surface);
	peerCtx->cursorSurface = surface;
	return 0;
}

static int
rdp_rail_update_cursor(struct weston_surface *surface)
{
	struct weston_pointer *pointer = surface->committed_private;
	struct weston_compositor *compositor = surface->compositor;
	struct weston_surface_rail_state *rail_state = (struct weston_surface_rail_state *)surface->backend_state;
	struct rdp_backend *b = (struct rdp_backend*)compositor->backend;
	RdpPeerContext *peerCtx = (RdpPeerContext *)b->rdp_peer->context;
	BOOL isCursorResized = FALSE;
	BOOL isCursorHidden = FALSE;
	BOOL isCursorDamanged = FALSE;
	int numViews;
	struct weston_view *view;
	struct weston_rdp_rail_window_pos newPos = {.x = 0, .y = 0, .width = surface->width, .height = surface->height};
	struct weston_rdp_rail_window_pos newClientPos = {.x = 0, .y = 0, .width = surface->width, .height = surface->height};
	int contentBufferWidth;
	int contentBufferHeight;

	ASSERT_COMPOSITOR_THREAD(b);
	assert(rail_state);

	/* obtain view's global position */
	numViews = 0;
	wl_list_for_each(view, &surface->views, surface_link) {
		float sx, sy;
		weston_view_to_global_float(view, 0, 0, &sx, &sy);
		newPos.x = (int)sx;
		newPos.y = (int)sy;
		numViews++;
		break; // just handle the first view for this hack
	}
	if (numViews == 0) {
		view = NULL;
		rdp_debug_verbose(b, "%s: surface has no view (windowId:0x%x)\n", __func__, rail_state->window_id);
	}

	if (newPos.x < 0 || newPos.y < 0)
		isCursorHidden = TRUE;

	weston_surface_get_content_size(surface, &contentBufferWidth, &contentBufferHeight);
	newClientPos.width = contentBufferWidth;
	newClientPos.height = contentBufferHeight;
	if (surface->output)
		to_client_coordinate(peerCtx, surface->output,
			&newClientPos.x, &newClientPos.y, &newClientPos.width, &newClientPos.height);

	if (newClientPos.width > 0 && newClientPos.height > 0)
		isCursorResized = TRUE;
	else
		isCursorHidden = TRUE;

	rail_state->pos = newPos;
	rail_state->clientPos = newClientPos;

	if (!isCursorHidden && !isCursorResized) {
		if ((surface->damage.extents.x2 - surface->damage.extents.x1) > 0  ||
			(surface->damage.extents.y2 - surface->damage.extents.y1) > 0)
			isCursorDamanged = TRUE;
	}

	if (isCursorHidden) {
		/* hide pointer */
		POINTER_SYSTEM_UPDATE pointerSystem = {};
		pointerSystem.type = SYSPTR_NULL;
		b->rdp_peer->update->BeginPaint(b->rdp_peer->update->context);
		b->rdp_peer->update->pointer->PointerSystem(b->rdp_peer->update->context, &pointerSystem);
		b->rdp_peer->update->EndPaint(b->rdp_peer->update->context);
	} else if (isCursorResized || isCursorDamanged) {
		POINTER_LARGE_UPDATE pointerUpdate = {};
		int cursorBpp = 4; // Bytes Per Pixel.
		int pointerBitsSize = newClientPos.width*cursorBpp*newClientPos.height;
		BYTE *pointerBits = malloc(pointerBitsSize);
		if (!pointerBits) {
			rdp_debug_error(b, "malloc failed for cursor shape\n");
			return -1;
		}

		/* client expects y-flip image for cursor */
		if (weston_surface_copy_content(surface,
			pointerBits, pointerBitsSize, 0,
			newClientPos.width, newClientPos.height,
			0, 0, contentBufferWidth, contentBufferHeight,
			true /* y-flip */, true /* is_argb */) < 0) {
			rdp_debug_error(b, "weston_surface_copy_content failed for cursor shape\n");
			free(pointerBits);
			return -1;
		}

		pointerUpdate.xorBpp = cursorBpp*8; // Bits Per Pixel.
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
		b->rdp_peer->update->BeginPaint(b->rdp_peer->update->context);
		b->rdp_peer->update->pointer->PointerLarge(b->rdp_peer->update->context, &pointerUpdate);
		b->rdp_peer->update->EndPaint(b->rdp_peer->update->context);

		free(pointerBits);
	}

	return 0;
}

static void
rdp_rail_create_window(struct wl_listener *listener, void *data)
{
	struct weston_surface *surface = (struct weston_surface *)data;
	struct weston_compositor *compositor = surface->compositor;
	struct weston_surface_rail_state *rail_state = (struct weston_surface_rail_state *)surface->backend_state;
	struct rdp_backend *b = (struct rdp_backend*)compositor->backend;
	WINDOW_ORDER_INFO window_order_info = {};
	WINDOW_STATE_ORDER window_state_order = {};
	struct weston_rdp_rail_window_pos pos = {.x = 0, .y = 0, .width = surface->width, .height = surface->height};
	struct weston_rdp_rail_window_pos clientPos = {.x = 0, .y = 0, .width = surface->width, .height = surface->height};
	RECTANGLE_16 window_rect = { 0, 0, surface->width, surface->height };
	RECTANGLE_16 window_vis = { 0, 0, surface->width, surface->height };
	int numViews;
	struct weston_view *view;
	UINT32 window_id;
	RdpPeerContext *peerCtx;

	/* negative width/height is not allowed, allow window to be created with zeros */
	if (surface->width < 0 || surface->height < 0) {
		rdp_debug_error(b, "surface width and height are negative\n");
		return;
	}

	if (!b || !b->rdp_peer) {
		rdp_debug_error(b, "CreateWndow(): rdp_peer is not initalized\n");
		return;
	}

	if (!b->rdp_peer->settings->HiDefRemoteApp)
		return;

	if (!b->rdp_peer->context) {
		rdp_debug_verbose(b, "CreateWndow(): rdp_peer->context is not initalized\n");
		return;
	}

	peerCtx = (RdpPeerContext *)b->rdp_peer->context;

	ASSERT_COMPOSITOR_THREAD(b);

	if (!peerCtx->activationRailCompleted) {
		rdp_debug_verbose(b, "CreateWindow(): rdp_peer rail is not activated.\n");
		return;
	}

	/* HiDef requires graphics channel to be ready */
	if (!peerCtx->activationGraphicsCompleted) {
		rdp_debug_verbose(b, "CreateWindow(): graphics channel is not activated.\n");
		return;
	}

	if (!rail_state) {
		rail_state = zalloc(sizeof *rail_state);
		if (!rail_state)
			return;
		surface->backend_state = (void *)rail_state;
	} else {
		/* If ever encouter error for this window, no more attempt to create window */
		if (rail_state->error)
			return;
	}

	/* windowId can be assigned only after activation completed */
	if (!rdp_id_manager_allocate_id(&peerCtx->windowId, (void*)surface, &window_id)) {
		rail_state->error = true;
		rdp_debug_error(b, "CreateWindow(): fail to insert windowId.hash_table (windowId:%d surface:%p.\n",
				 window_id, surface);
		return;
	}
	rail_state->window_id = window_id;
	/* Once this surface is inserted to hash table, we want to be notified for destroy */
	assert(NULL == rail_state->destroy_listener.notify);
	rail_state->destroy_listener.notify = rdp_rail_destroy_window;
	wl_signal_add(&surface->destroy_signal, &rail_state->destroy_listener);

	if (surface->role_name != NULL) {
		if (strncmp(surface->role_name, "wl_subsurface", sizeof("wl_subsurface")) == 0) {
			rail_state->parent_surface = weston_surface_get_main_surface(surface);
			assert(surface != rail_state->parent_surface);
		} else if (strncmp(surface->role_name, "wl_pointer-cursor", sizeof("wl_pointer-cursor")) == 0) {
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
		break; // just handle the first view for this hack
	}
	if (numViews == 0) {
		view = NULL;
		rdp_debug_verbose(b, "%s: surface has no view (windowId:0x%x)\n", __func__, rail_state->window_id);
	}

	/* apply global to output transform, and translate to client coordinate */
	if (surface->output)
		to_client_coordinate(peerCtx, surface->output,
			&clientPos.x, &clientPos.y, &clientPos.width, &clientPos.height);

	window_rect.top = window_vis.top = clientPos.y;
	window_rect.left = window_vis.left = clientPos.x;
	window_rect.right = window_vis.right = clientPos.x + clientPos.width;
	window_rect.bottom = window_vis.bottom = clientPos.y + clientPos.height;

	window_order_info.fieldFlags = 
		(WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_STATE_NEW);
	window_order_info.windowId = window_id;

	window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_STYLE;
	window_state_order.style = RAIL_WINDOW_NORMAL_STYLE;
	window_state_order.extendedStyle = WS_EX_LAYERED;

	window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_OWNER;
	if (rail_state->parent_surface &&
	    rail_state->parent_surface->backend_state) {
		struct weston_surface_rail_state *parent_rail_state =
			(struct weston_surface_rail_state *)rail_state->parent_surface->backend_state;
		window_state_order.ownerWindowId = parent_rail_state->window_id;
	} else {
		window_state_order.ownerWindowId = RDP_RAIL_DESKTOP_WINDOW_ID;
	}

	/* window is created with hidden and no taskbar icon always, and
	   it become visbile when window has some contents to show. */
	window_order_info.fieldFlags |= 
		(WINDOW_ORDER_FIELD_SHOW | WINDOW_ORDER_FIELD_TASKBAR_BUTTON);
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

	/*window_state_order.titleInfo = NULL; */
	/*window_state_order.OverlayDescription = 0;*/

	rdp_debug_verbose(b, "WindowCreate(0x%x - (%d, %d, %d, %d)\n",
		window_id, clientPos.x, clientPos.y, clientPos.width, clientPos.height);
	b->rdp_peer->update->BeginPaint(b->rdp_peer->update->context);
	b->rdp_peer->update->window->WindowCreate(b->rdp_peer->update->context, &window_order_info, &window_state_order);
	b->rdp_peer->update->EndPaint(b->rdp_peer->update->context);

	rail_state->parent_window_id = window_state_order.ownerWindowId;
	rail_state->pos = pos;
	rail_state->clientPos = clientPos;
	rail_state->isWindowCreated = TRUE;
	rail_state->get_label = (void *)-1; // label to be re-checked at update.
	rail_state->taskbarButton = window_state_order.TaskbarButton;
	pixman_region32_init_rect(&rail_state->damage,
		 0, 0, surface->width_from_buffer, surface->height_from_buffer);

	/* as new window created, mark z order dirty */
	/* TODO: ideally this better be triggered from shell, but shell isn't notified
		 creation/destruction of certain type of window, such as dropdown menu
		 (popup in Wayland, override_redirect in X), thus do it here. */
	peerCtx->is_window_zorder_dirty = true;

Exit:
	/* once window is successfully created, start listening repaint update */
	if (!rail_state->error) {
		assert(NULL == rail_state->repaint_listener.notify);
		rail_state->repaint_listener.notify = rdp_rail_schedule_update_window;
		wl_signal_add(&surface->repaint_signal, &rail_state->repaint_listener);
	}

	return;
}

static void
rdp_destroy_shared_buffer(struct weston_surface *surface)
{
	struct weston_compositor *compositor = surface->compositor;
	struct weston_surface_rail_state *rail_state = (struct weston_surface_rail_state *)surface->backend_state;
	struct rdp_backend *b = (struct rdp_backend*)compositor->backend;
	RdpPeerContext *peerCtx = (RdpPeerContext *)b->rdp_peer->context;

	assert(b->use_gfxredir);

	if (rail_state->buffer_id) {
		GFXREDIR_DESTROY_BUFFER_PDU destroyBuffer = {};

		destroyBuffer.bufferId = rail_state->buffer_id;
		peerCtx->gfxredir_server_context->DestroyBuffer(peerCtx->gfxredir_server_context, &destroyBuffer);

		rdp_id_manager_free_id(&peerCtx->bufferId, rail_state->buffer_id);
		rail_state->buffer_id = 0;
	}

	if (rail_state->pool_id) {
		GFXREDIR_CLOSE_POOL_PDU closePool = {};

		closePool.poolId = rail_state->pool_id;
		peerCtx->gfxredir_server_context->ClosePool(peerCtx->gfxredir_server_context, &closePool);

		rdp_id_manager_free_id(&peerCtx->poolId, rail_state->pool_id);
		rail_state->pool_id = 0;
	}

	rdp_free_shared_memory(b, &rail_state->shared_memory);

	rail_state->surfaceBuffer = NULL;
}

static void
rdp_rail_destroy_window(struct wl_listener *listener, void *data)
{
	struct weston_surface *surface = (struct weston_surface *)data;
	struct weston_compositor *compositor = surface->compositor;
	struct weston_surface_rail_state *rail_state = (struct weston_surface_rail_state *)surface->backend_state;
	struct rdp_backend *b = (struct rdp_backend*)compositor->backend;
	WINDOW_ORDER_INFO window_order_info = {};
	POINTER_SYSTEM_UPDATE pointerSystem = {};
	UINT32 window_id;
	RdpPeerContext *peerCtx;

	if (!rail_state)
		return;

	window_id = rail_state->window_id;
	if (!window_id)
		goto Exit;

	assert(b && b->rdp_peer);

	ASSERT_COMPOSITOR_THREAD(b);

	peerCtx = (RdpPeerContext *)b->rdp_peer->context;
	if (rail_state->isCursor) {
		pointerSystem.type = SYSPTR_NULL;
		b->rdp_peer->update->BeginPaint(b->rdp_peer->update->context);
		b->rdp_peer->update->pointer->PointerSystem(b->rdp_peer->update->context, &pointerSystem);
		b->rdp_peer->update->EndPaint(b->rdp_peer->update->context);
		if (peerCtx->cursorSurface == surface) 
			peerCtx->cursorSurface = NULL;
		rail_state->isCursor = false;
	} else {
		if (rail_state->isWindowCreated) {

			if (rail_state->surface_id || rail_state->buffer_id) {
				/* When update is pending, need to wait reply from client */
				/* TODO: Defer destroy to FreeRDP callback ? */
				freerdp_peer *client = (freerdp_peer*)peerCtx->rail_grfx_server_context->custom;
				int waitRetry = 0;
				while (rail_state->isUpdatePending ||
						(peerCtx->currentFrameId != peerCtx->acknowledgedFrameId &&
						 !peerCtx->isAcknowledgedSuspended)) {
					if (++waitRetry > 1000) { // timeout after 10 sec.
						rdp_debug_error(b, "%s: update is still pending in client side (windowId:0x%x)\n",
								__func__, window_id);
						break;
					}
					USleep(10000); // wait 0.01 sec.
					client->CheckFileDescriptor(client);
					WTSVirtualChannelManagerCheckFileDescriptor(peerCtx->vcm);
				}
			}

#ifdef HAVE_FREERDP_GFXREDIR_H
			if (b->use_gfxredir)
				rdp_destroy_shared_buffer(surface);
#endif // HAVE_FREERDP_GFXREDIR_H

			window_order_info.windowId = window_id;
			window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_STATE_DELETED;

			rdp_debug_verbose(b, "WindowDestroy(0x%x)\n", window_id);
			b->rdp_peer->update->BeginPaint(b->rdp_peer->update->context);
			b->rdp_peer->update->window->WindowDelete(b->rdp_peer->update->context, &window_order_info);
			b->rdp_peer->update->EndPaint(b->rdp_peer->update->context);

			if (rail_state->surface_id) {

				RDPGFX_DELETE_SURFACE_PDU deleteSurface = {};

				rdp_debug_verbose(b, "DeleteSurface(surfaceId:0x%x for windowsId:0x%x)\n", rail_state->surface_id, window_id);
				deleteSurface.surfaceId = (UINT16)rail_state->surface_id;
				peerCtx->rail_grfx_server_context->DeleteSurface(peerCtx->rail_grfx_server_context, &deleteSurface);

				rdp_id_manager_free_id(&peerCtx->surfaceId, rail_state->surface_id);
				rail_state->surface_id = 0;
			}
			rail_state->isWindowCreated = FALSE;
		}
		pixman_region32_fini(&rail_state->damage);
	}

	rdp_id_manager_free_id(&peerCtx->windowId, window_id);
	rail_state->window_id = 0;

	/* as window destroyed, mark z order dirty and if this is active window, clear it */
	/* TODO: ideally this better be triggered from shell, but shell isn't notified
		 creation/destruction of certain type of window, such as dropdown menu
		 (popup in Wayland, override_redirect in X), thus do it here. */
	peerCtx->is_window_zorder_dirty = true;
	if (peerCtx->active_surface == surface) {
		peerCtx->active_surface = NULL;
	}

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
	struct weston_surface *surface = (struct weston_surface *)data;
	struct weston_compositor *compositor = surface->compositor;
	struct rdp_backend *b = (struct rdp_backend*)compositor->backend;
	struct weston_surface_rail_state *rail_state = (struct weston_surface_rail_state *)surface->backend_state;
	UINT32 window_id;

	if (!rail_state || rail_state->error)
		return;

	window_id = rail_state->window_id;
	if (!window_id)
		return;

	ASSERT_COMPOSITOR_THREAD(b);

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

	if (!pixman_region32_union(&rail_state->damage, &rail_state->damage, &surface->damage)) {
		/* if union failed, make entire size of bufer based on current buffer */
		pixman_region32_clear(&rail_state->damage);
		pixman_region32_init_rect(&rail_state->damage,
			0, 0, surface->width_from_buffer, surface->height_from_buffer);
	}

	return;
}

struct update_window_iter_data {
	uint32_t output_id;
	UINT32 startedFrameId;
	BOOL needEndFrame;
	BOOL isUpdatePending;
};

static int
rdp_rail_update_window(struct weston_surface *surface, struct update_window_iter_data *iter_data)
{
	struct weston_compositor *compositor = surface->compositor;
	struct weston_surface_rail_state *rail_state = (struct weston_surface_rail_state *)surface->backend_state;
	struct rdp_backend *b = (struct rdp_backend*)compositor->backend;
	WINDOW_ORDER_INFO window_order_info = {};
	WINDOW_STATE_ORDER window_state_order = {};
	struct weston_rdp_rail_window_pos newPos = {.x = 0, .y = 0, .width = surface->width, .height = surface->height};
	struct weston_rdp_rail_window_pos newClientPos = {.x = 0, .y = 0, .width = surface->width, .height = surface->height};
	RECTANGLE_16 window_rect;
	RECTANGLE_16 window_vis;
	int numViews;
	struct weston_view *view;
	UINT32 window_id;
	RdpPeerContext *peerCtx = (RdpPeerContext *)b->rdp_peer->context;
	UINT32 new_surface_id = 0;
	UINT32 old_surface_id = 0;
	RAIL_UNICODE_STRING rail_window_title_string = { 0, NULL };
	char window_title[256];
	char window_title_mod[256];
	char *title = NULL;

	ASSERT_COMPOSITOR_THREAD(b);

	if (!rail_state || rail_state->error)
		return 0;

	window_id = rail_state->window_id;
	if (!window_id)
		return 0;

	if (surface->role_name != NULL) {
		if (!rail_state->parent_surface) {
			if (strncmp(surface->role_name, "wl_subsurface", sizeof("wl_subsurface")) == 0) {
				rail_state->parent_surface = weston_surface_get_main_surface(surface);
				assert(surface != rail_state->parent_surface);
			}
		}
		if (!rail_state->isCursor) {
			if (strncmp(surface->role_name, "wl_pointer-cursor", sizeof("wl_pointer-cursor")) == 0) {
				rdp_debug_error(b, "!!!cursor role is added after creation - WindowId:0x%x\n", window_id);

				/* convert to RDP cursor */
				rdp_rail_destroy_window(NULL, (void *)surface);
				assert(!surface->backend_state);

				rdp_rail_create_window(NULL, (void *)surface);
				rail_state = (struct weston_surface_rail_state *)surface->backend_state;
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

//		if (memcmp(&surface->buffer_to_surface_matrix.d, &identity.d, sizeof(identity.d)) != 0)
//			rdp_debug(b, "buffer to surface matrix is not identity matrix type:0x%x (windowId:0x%x)\n",
//					surface->buffer_to_surface_matrix.type, window_id);

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

		//TODO: when surface is not associated to any output, it looks must not be visible. Need to verify. 
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
		break; // just handle the first view for this hack
	}
	if (numViews == 0) {
		view = NULL;
		rdp_debug_verbose(b, "%s: surface has no view (windowId:0x%x)\n", __func__, rail_state->window_id);
	}

	/* apply global to output transform, and translate to client coordinate */
	if (surface->output)
		to_client_coordinate(peerCtx, surface->output,
			&newClientPos.x, &newClientPos.y, &newClientPos.width, &newClientPos.height);

	/* Adjust the Windows size and position on the screen */
	if (rail_state->clientPos.x != newClientPos.x ||
		rail_state->clientPos.y != newClientPos.y ||
		rail_state->clientPos.width != newClientPos.width ||
		rail_state->clientPos.height != newClientPos.height ||
		rail_state->is_minimized != rail_state->is_minimized_requested ||
		rail_state->get_label != surface->get_label ||
		rail_state->forceUpdateWindowState) {

  		window_order_info.windowId = window_id;
		window_order_info.fieldFlags = 
			WINDOW_ORDER_TYPE_WINDOW;

		if (rail_state->parent_surface &&
		    rail_state->parent_surface->backend_state) {
			struct weston_surface_rail_state *parent_rail_state =
				(struct weston_surface_rail_state *)rail_state->parent_surface->backend_state;
			if (rail_state->parent_window_id != parent_rail_state->window_id) {
				window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_OWNER;

				window_state_order.ownerWindowId = parent_rail_state->window_id;

				rail_state->parent_window_id = parent_rail_state->window_id;

				rdp_debug_verbose(b, "WindowUpdate(0x%x - parent window id:%x)\n",
						window_id, rail_state->parent_window_id);
			}
		}

		if (rail_state->forceUpdateWindowState ||
			rail_state->is_minimized != rail_state->is_minimized_requested) {
			window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW;

			window_state_order.showState = rail_state->is_minimized_requested ? WINDOW_SHOW_MINIMIZED : WINDOW_SHOW;

			rail_state->is_minimized = rail_state->is_minimized_requested;

			rdp_debug_verbose(b, "WindowUpdate(0x%x - is_minimized:%d)\n",
					window_id, rail_state->is_minimized_requested);
		}

		if (rail_state->is_maximized != rail_state->is_maximized_requested) {
			rdp_debug_verbose(b, "WindowUpdate(0x%x - is_maximized:%d)\n",
					window_id, rail_state->is_maximized_requested);
			rail_state->is_maximized = rail_state->is_maximized_requested;
		}

		if (rail_state->is_fullscreen != rail_state->is_fullscreen_requested) {
			rdp_debug_verbose(b, "WindowUpdate(0x%x - is_fullscreen:%d)\n",
					window_id, rail_state->is_fullscreen_requested);
			rail_state->is_fullscreen = rail_state->is_fullscreen_requested;

			window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_STYLE;
			if (rail_state->is_fullscreen)
				window_state_order.style = RAIL_WINDOW_FULLSCREEN_STYLE;
			else
				window_state_order.style = RAIL_WINDOW_NORMAL_STYLE;
			window_state_order.extendedStyle = WS_EX_LAYERED;
			/* force update window geometry */
			rail_state->forceUpdateWindowState = true;
		}

		if (rail_state->forceUpdateWindowState ||
			rail_state->get_label != surface->get_label) {
			window_order_info.fieldFlags |=
				 WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
			if (rail_state->parent_surface || (surface->get_label == NULL))
				window_state_order.TaskbarButton = 1;
			else
				window_state_order.TaskbarButton = 0;

			if (surface->get_label && surface->get_label(surface, window_title, sizeof(window_title))) {
				/* see rdprail-shell for naming convension for label */
				/* TODO: For X11 app, ideally it should check "override" property, but somehow
				         Andriod Studio's (at least 4.1.1) dropdown menu is not "override" window,
				         thus here checks child window, but this causes the other issue that
				         the pop up window, such as "Confirm Exit" (in Andriod Studio) is not shown
				         in taskbar. */
				if (strncmp(window_title, "child window", sizeof("child window") - 1) == 0)
					window_state_order.TaskbarButton = 1;
				title = strchr(window_title, 39); 
				if (title) {
					char *end = strrchr(window_title, 39);
					if (end != title) {
						*title++ = '\0';
						*end = '\0';
					}
				} else {
					title = window_title;
				}
#ifdef HAVE_FREERDP_GFXREDIR_H
				/* this is for debugging only */
				if (b->enable_copy_warning_title) {
					if (snprintf(window_title_mod,
						sizeof window_title_mod,
						"[WARN:COPY MODE] %s (%s)",
						title, b->rdprail_shell_name ? b->rdprail_shell_name : "Linux") > 0)
						title = window_title_mod;
				} else
#endif // HAVE_FREERDP_GFXREDIR_H
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
					window_order_info.fieldFlags |=
						WINDOW_ORDER_FIELD_TITLE;
					window_state_order.titleInfo = rail_window_title_string;
				}
			}

			rail_state->get_label = surface->get_label;
			rail_state->taskbarButton = window_state_order.TaskbarButton;

			rdp_debug_verbose(b, "WindowUpdate(0x%x - title \"%s\") TaskbarButton:%d\n",
					window_id, title, window_state_order.TaskbarButton);
		} else {
			/* There seems a bug in mstsc client that previous taskbar button state is
			   not preserved, thus sending taskbar field always. */
			window_order_info.fieldFlags |=
				 WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
			window_state_order.TaskbarButton = (BYTE) rail_state->taskbarButton;
		}

		if (rail_state->forceUpdateWindowState ||
			rail_state->clientPos.width != newClientPos.width ||
			rail_state->clientPos.height != newClientPos.height ||
			rail_state->output != surface->output) {
			window_order_info.fieldFlags |=
				(WINDOW_ORDER_FIELD_WND_SIZE |
				 WINDOW_ORDER_FIELD_WND_RECTS | 
				 WINDOW_ORDER_FIELD_VISIBILITY |
				 WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE);

			window_rect.top = window_vis.top = newClientPos.y;
			window_rect.left = window_vis.left = newClientPos.x;
			window_rect.right = window_vis.right = newClientPos.x + newClientPos.width;
			window_rect.bottom = window_vis.bottom = newClientPos.y + newClientPos.height;

			window_state_order.windowWidth = newClientPos.width;
			window_state_order.windowHeight = newClientPos.height;
			window_state_order.numWindowRects = 1;
			window_state_order.windowRects = &window_rect;
			window_state_order.numVisibilityRects = 1;
			window_state_order.visibilityRects = &window_vis;
			window_state_order.clientAreaWidth = newClientPos.width;
			window_state_order.clientAreaHeight = newClientPos.height;
			if (!rail_state->is_fullscreen) {
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

				rdp_debug_verbose(b, "WindowUpdate(0x%x - taskbar:%d showState:%d))\n",
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
					(WINDOW_ORDER_FIELD_SHOW |
				 	 WINDOW_ORDER_FIELD_TASKBAR_BUTTON);
				window_state_order.TaskbarButton = 1;
				window_state_order.showState = WINDOW_HIDE;

				rdp_debug_verbose(b, "WindowUpdate(0x%x - taskbar:%d showState:%d))\n",
						window_id,
						window_state_order.TaskbarButton,
						window_state_order.showState);
			}

			rail_state->pos.width = newPos.width;
			rail_state->pos.height = newPos.height;
			rail_state->clientPos.width = newClientPos.width;
			rail_state->clientPos.height = newClientPos.height;
			rail_state->output = surface->output;

			rdp_debug_verbose(b, "WindowUpdate(0x%x - size (%d, %d) in RDP client size (%d, %d)\n",
					window_id, newPos.width, newPos.height, newClientPos.width, newClientPos.height);
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

			rdp_debug_verbose(b, "WindowUpdate(0x%x - pos (%d, %d) - RDP client pos (%d, %d)\n",
					window_id, newPos.x, newPos.y, newClientPos.x, newClientPos.y);
		}

		b->rdp_peer->update->BeginPaint(b->rdp_peer->update->context);
		b->rdp_peer->update->window->WindowUpdate(b->rdp_peer->update->context, &window_order_info, &window_state_order);
		b->rdp_peer->update->EndPaint(b->rdp_peer->update->context);

		if (rail_window_title_string.string)
			free(rail_window_title_string.string);

		rail_state->forceUpdateWindowState = false;
	}

	/* update window buffer contents */
	{
		BOOL isBufferSizeChanged = FALSE;
		float scaleWidth = 1.0f, scaleHeight = 1.0f;
		int damageWidth, damageHeight;
		int copyBufferStride, copyBufferSize;
		int clientBufferWidth, clientBufferHeight;
		int contentBufferStride, contentBufferSize;
		int contentBufferWidth, contentBufferHeight;
		int bufferBpp = 4; // Bytes Per Pixel.
		bool hasAlpha = view ? !weston_view_is_opaque(view, &view->transform.boundingbox) : false;
		pixman_box32_t damageBox = *pixman_region32_extents(&rail_state->damage);
		long page_size = sysconf(_SC_PAGESIZE);

		clientBufferWidth = newClientPos.width;
		clientBufferHeight = newClientPos.height;

		weston_surface_get_content_size(surface, &contentBufferWidth, &contentBufferHeight);
		contentBufferStride = contentBufferWidth * bufferBpp;
		contentBufferSize = contentBufferStride * contentBufferHeight;

		copyBufferSize = (contentBufferSize + page_size - 1) & ~(page_size - 1);
		copyBufferStride = contentBufferStride;

		if (contentBufferWidth && contentBufferHeight) {

#ifdef HAVE_FREERDP_GFXREDIR_H
			if (b->use_gfxredir) {
				scaleWidth = 1.0f; // scaling is done by client.
				scaleHeight = 1.0f; // scaling is done by client.

				if (rail_state->bufferWidth != contentBufferWidth ||
					rail_state->bufferHeight != contentBufferHeight)
					isBufferSizeChanged = TRUE;
			} else {
#else
			{
#endif // HAVE_FREERDP_GFXREDIR_H
				scaleWidth = (float)clientBufferWidth / contentBufferWidth;
				scaleHeight = (float)clientBufferHeight / contentBufferHeight;

				if (rail_state->bufferWidth != contentBufferWidth ||
					rail_state->bufferHeight != contentBufferHeight)
					isBufferSizeChanged = TRUE;
			}

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
					rail_state->shared_memory.size = copyBufferSize;
					if (rdp_allocate_shared_memory(b, &rail_state->shared_memory)) {
						UINT32 new_pool_id = 0;
						if (rdp_id_manager_allocate_id(&peerCtx->poolId, (void*)surface, &new_pool_id)) {
							// +1 for NULL terminate.
							unsigned short sectionName[RDP_SHARED_MEMORY_NAME_SIZE + 1];
							// In Linux wchar_t is 4 types, but Windows wants 2 bytes wchar...
							// convert to 2 bytes wchar_t.
							for (uint32_t i = 0; i < RDP_SHARED_MEMORY_NAME_SIZE; i++)
								sectionName[i] = rail_state->shared_memory.name[i];
							sectionName[RDP_SHARED_MEMORY_NAME_SIZE] = 0;

							GFXREDIR_OPEN_POOL_PDU openPool = {};
							openPool.poolId = new_pool_id;
							openPool.poolSize = copyBufferSize;
							openPool.sectionNameLength = RDP_SHARED_MEMORY_NAME_SIZE + 1;
							openPool.sectionName = sectionName;
							if (peerCtx->gfxredir_server_context->OpenPool(
								peerCtx->gfxredir_server_context, &openPool) == 0) {
								UINT32 new_buffer_id = 0;
								if (rdp_id_manager_allocate_id(&peerCtx->bufferId, (void*)surface, &new_buffer_id)) {
									GFXREDIR_CREATE_BUFFER_PDU createBuffer = {};
									createBuffer.poolId = openPool.poolId;
									createBuffer.bufferId = new_buffer_id;
									createBuffer.offset = 0;
									createBuffer.stride = contentBufferStride;
									createBuffer.width = contentBufferWidth;
									createBuffer.height = contentBufferHeight;
									createBuffer.format = GFXREDIR_BUFFER_PIXEL_FORMAT_ARGB_8888;
									if (peerCtx->gfxredir_server_context->CreateBuffer(
										peerCtx->gfxredir_server_context, &createBuffer) == 0) {
										rail_state->surfaceBuffer = rail_state->shared_memory.addr;
										rail_state->buffer_id = createBuffer.bufferId;
										rail_state->pool_id = openPool.poolId;
										rail_state->bufferWidth = contentBufferWidth;
										rail_state->bufferHeight = contentBufferHeight;
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
#endif // HAVE_FREERDP_GFXREDIR_H
					if (rdp_id_manager_allocate_id(&peerCtx->surfaceId, (void*)surface, &new_surface_id)) {
						RDPGFX_CREATE_SURFACE_PDU createSurface = {};
						/* create surface */
						rdp_debug_verbose(b, "CreateSurface(surfaceId:0x%x - (%d, %d) size:%d for windowsId:0x%x)\n",
							new_surface_id, contentBufferWidth, contentBufferHeight, contentBufferSize, window_id);
						createSurface.surfaceId = (UINT16)new_surface_id;
						createSurface.width = contentBufferWidth;
						createSurface.height = contentBufferHeight;
						/* regardless buffer as alpha or not, always use alpha to avoid mstsc bug */
						createSurface.pixelFormat = GFX_PIXEL_FORMAT_ARGB_8888;
						if (peerCtx->rail_grfx_server_context->CreateSurface(peerCtx->rail_grfx_server_context, &createSurface) == 0) {
							/* store new surface id */
							old_surface_id = rail_state->surface_id;
							rail_state->surface_id = new_surface_id;
							rail_state->bufferWidth = contentBufferWidth;
							rail_state->bufferHeight = contentBufferHeight;
						}
					}
				}
				rail_state->forceRecreateSurface = false;

				/* When creating a new surface we need to upload it's entire content, expand damage */
				damageBox.x1 = 0;
				damageBox.y1 = 0;
				damageBox.x2 = contentBufferWidth;
				damageBox.y2 = contentBufferHeight;
			} else if (damageBox.x2 > 0 && damageBox.y2 > 0) {
				/* scale damage using surface to buffer matrix */
				rdp_matrix_transform_position(&surface->surface_to_buffer_matrix, &damageBox.x1, &damageBox.y1);
				rdp_matrix_transform_position(&surface->surface_to_buffer_matrix, &damageBox.x2, &damageBox.y2);
			}
		} else {
			/* no content buffer bound, thus no damage */
			damageBox.x1 = 0;
			damageBox.y1 = 0;
			damageBox.x2 = 0;
			damageBox.y2 = 0;
		}

		damageWidth = damageBox.x2 - damageBox.x1;
		if (damageWidth > contentBufferWidth) {
			rdp_debug(b, "damageWidth (%d) is larger than content width(%d), clamp to avoid protocol error.\n",
				damageWidth, contentBufferWidth);
			damageBox.x1 = 0;
			damageBox.x2 = contentBufferWidth;
			damageWidth = contentBufferWidth;
		}
		damageHeight = damageBox.y2 - damageBox.y1;
		if (damageHeight > contentBufferHeight) {
			rdp_debug(b, "damageHeight (%d) is larger than content height(%d), clamp to avoid protocol error.\n",
				damageHeight, contentBufferHeight);
			damageBox.y1 = 0;
			damageBox.y2 = contentBufferHeight;
			damageHeight = contentBufferHeight;
		}

		/* Check to see if we have any content update to send to the new surface */
		if (damageWidth > 0 && damageHeight > 0) {
#ifdef HAVE_FREERDP_GFXREDIR_H
			if (b->use_gfxredir &&
				rail_state->surfaceBuffer) {

				int copyDamageX1 = (float)damageBox.x1 * scaleWidth;
				int copyDamageY1 = (float)damageBox.y1 * scaleHeight;
				int copyDamageWidth = (float)damageWidth * scaleWidth;
				int copyDamageHeight = (float)damageHeight * scaleHeight;
				int copyStartOffset = copyDamageX1*bufferBpp + copyDamageY1*copyBufferStride;
				BYTE *copyBufferBits = (BYTE*)(rail_state->surfaceBuffer) + copyStartOffset;

				rdp_debug_verbose(b, "copy source: x:%d, y:%d, width:%d, height:%d\n",
					damageBox.x1, damageBox.y1, damageWidth, damageHeight);
				rdp_debug_verbose(b, "copy target: x:%d, y:%d, width:%d, height:%d, stride:%d\n",
					copyDamageX1, copyDamageY1, copyDamageWidth, copyDamageHeight, copyBufferStride);
				rdp_debug_verbose(b, "copy scale: scaleWidth:%5.3f, scaleHeight:%5.3f\n",
					scaleWidth, scaleHeight);

				if (weston_surface_copy_content(surface,
					copyBufferBits, copyBufferSize, copyBufferStride,
					copyDamageWidth, copyDamageHeight,
					damageBox.x1, damageBox.y1, damageWidth, damageHeight,
					false /* y-flip */, true /* is_argb */) < 0) {
					rdp_debug_error(b, "weston_surface_copy_content failed for windowId:0x%x\n",window_id);
					return -1;
				}

				GFXREDIR_PRESENT_BUFFER_PDU presentBuffer = {};
				RECTANGLE_32 opaqueRect;

				/* specify opaque area */
				if (!hasAlpha) {
					opaqueRect.left = copyDamageX1;
					opaqueRect.top = copyDamageY1;
					opaqueRect.width = copyDamageWidth;
					opaqueRect.height = copyDamageHeight;
				}

				presentBuffer.timestamp = 0; /* set 0 to disable A/V sync at client side */
				presentBuffer.presentId = ++peerCtx->currentFrameId;
				presentBuffer.windowId = window_id;
				presentBuffer.bufferId = rail_state->buffer_id;
				presentBuffer.orientation = 0; // 0, 90, 180 or 270.
				presentBuffer.targetWidth = newClientPos.width;
				presentBuffer.targetHeight = newClientPos.height;
				presentBuffer.dirtyRect.left = copyDamageX1;
				presentBuffer.dirtyRect.top = copyDamageY1;
				presentBuffer.dirtyRect.width = copyDamageWidth;
				presentBuffer.dirtyRect.height = copyDamageHeight;
				if (!hasAlpha) {
					presentBuffer.numOpaqueRects = 1;
					presentBuffer.opaqueRects = &opaqueRect;
				} else {
					presentBuffer.numOpaqueRects = 0;
					presentBuffer.opaqueRects = NULL;
				}

				if (peerCtx->gfxredir_server_context->PresentBuffer(peerCtx->gfxredir_server_context, &presentBuffer) == 0) {
					rail_state->isUpdatePending = TRUE;
					iter_data->isUpdatePending = TRUE;
				} else {
					rdp_debug_error(b, "PresentBuffer failed for windowId:0x%x\n",window_id);
				}
			} else
#endif // HAVE_FREERDP_GFXREDIR_H
			if (rail_state->surface_id) {

				RDPGFX_SURFACE_COMMAND surfaceCommand = {};
				int damageStride = damageWidth*bufferBpp;
				int damageSize = damageStride*damageHeight;
				BYTE *data = NULL;
				int alphaCodecHeaderSize = 4;
				BYTE *alpha = NULL;
				int alphaSize;

				data = malloc(damageSize);
				if (!data) {
					// need better handling to avoid leaking surface on host.
					rdp_debug_error(b, "Couldn't allocate memory for bitmap update.\n");
					return -1;
				}

				if (hasAlpha)
					alphaSize = alphaCodecHeaderSize+damageWidth*damageHeight;
				else {
					alphaSize = alphaCodecHeaderSize+8; // 8 = max of ALPHA_RLE_SEGMENT for single alpha value.
				}
				alpha = malloc(alphaSize);
				if (!alpha) {
					free(data);
					// need better handling to avoid leaking surface on host.
					rdp_debug_error(b, "Couldn't allocate memory for alpha update.\n");
					return -1;
				}

				if (weston_surface_copy_content(surface,
					data, damageSize, 0, 0, 0,
					damageBox.x1, damageBox.y1, damageWidth, damageHeight,
					false /* y-flip */, true /* is_argb */) < 0) {
					rdp_debug_error(b, "weston_surface_copy_content failed for cursor shape\n");
					free(data);
					free(alpha);
					return -1;
				}

				/* generate alpha only bitmap */
				/* set up alpha codec header */
				alpha[0] = 'L';	// signature
				alpha[1] = 'A';	// signature
				alpha[2] = hasAlpha ? 0 : 1; // compression: RDP spec inticate this is non-zero value for compressed, but it must be 1.
				alpha[3] = 0; // compression

				if (hasAlpha) {
					BYTE *alphaBits = &data[0];
					for (int i = 0; i < damageHeight; i++, alphaBits+=damageStride) {
						BYTE *srcAlphaPixel = alphaBits + 3; // 3 = xxxA.
						BYTE *dstAlphaPixel = &alpha[alphaCodecHeaderSize+(i*damageWidth)];
						for (int j = 0; j < damageWidth; j++, srcAlphaPixel+=bufferBpp, dstAlphaPixel++) {
							*dstAlphaPixel = *srcAlphaPixel;
						}
					}
				} else {
					/* regardless buffer as alpha or not, always use alpha to avoid mstsc bug */
					/* CLEARCODEC_ALPHA_RLE_SEGMENT */
					int bitmapSize = damageWidth*damageHeight;
					alpha[alphaCodecHeaderSize] = 0xFF; // alpha value (opaque)
					if (bitmapSize < 0xFF) {
						alpha[alphaCodecHeaderSize+1] = (BYTE)bitmapSize;
						alphaSize = alphaCodecHeaderSize+2; // alpha value + size in byte.
					} else if (bitmapSize < 0xFFFF) { 	
						alpha[alphaCodecHeaderSize+1] = 0xFF;
						*(short*)&(alpha[alphaCodecHeaderSize+2]) = (short)bitmapSize;
						alphaSize = alphaCodecHeaderSize+4; // alpha value + 1 + size in short.
					} else {
						alpha[alphaCodecHeaderSize+1] = 0xFF;
						*(short*)&(alpha[alphaCodecHeaderSize+2]) = 0xFFFF;
						*(int*)&(alpha[alphaCodecHeaderSize+4]) = bitmapSize;
						alphaSize = alphaCodecHeaderSize+8; // alpha value + 1 + 2 + size in int.
					}
				}

				if (iter_data->needEndFrame == FALSE) {
					/* if frame is not started yet, send StartFrame first before sendng surface command. */
					RDPGFX_START_FRAME_PDU startFrame = {};
					startFrame.frameId = ++peerCtx->currentFrameId;
					rdp_debug_verbose(b, "StartFrame(frameId:0x%x, windowId:0x%x)\n", startFrame.frameId, window_id);
					peerCtx->rail_grfx_server_context->StartFrame(peerCtx->rail_grfx_server_context, &startFrame);
					iter_data->startedFrameId = startFrame.frameId;
					iter_data->needEndFrame = TRUE;
					iter_data->isUpdatePending = TRUE;
				}

				surfaceCommand.surfaceId = rail_state->surface_id;
				surfaceCommand.contextId = 0;
				surfaceCommand.format = PIXEL_FORMAT_BGRA32;
				surfaceCommand.left = damageBox.x1;
				surfaceCommand.top = damageBox.y1;
				surfaceCommand.right = damageBox.x2;
				surfaceCommand.bottom = damageBox.y2;
				surfaceCommand.width = damageWidth;
				surfaceCommand.height = damageHeight;
				surfaceCommand.extra = NULL;

				/* send alpha channel */
				surfaceCommand.codecId = RDPGFX_CODECID_ALPHA;
				surfaceCommand.length = alphaSize;
				surfaceCommand.data = &alpha[0];
				rdp_debug_verbose(b, "SurfaceCommand(frameId:0x%x, windowId:0x%x) for alpha\n",
					iter_data->startedFrameId, window_id);
				peerCtx->rail_grfx_server_context->SurfaceCommand(peerCtx->rail_grfx_server_context, &surfaceCommand);

				/* send bitmap data */
				surfaceCommand.codecId = RDPGFX_CODECID_UNCOMPRESSED;
				surfaceCommand.length = damageSize;
				surfaceCommand.data = &data[0];
				rdp_debug_verbose(b, "SurfaceCommand(frameId:0x%x, windowId:0x%x) for bitmap\n",
					iter_data->startedFrameId, window_id);
				peerCtx->rail_grfx_server_context->SurfaceCommand(peerCtx->rail_grfx_server_context, &surfaceCommand);

				free(data);
				free(alpha);
			}

			pixman_region32_clear(&rail_state->damage);

			/* TODO: this is temporary workaround, some window is not visible to shell
			   (such as subsurfaces, override_redirect), so z order update is 
			   not done by activate callback, thus trigger it at first update.
			   solution would make those surface visible to shell or hook signal on
			   when view_list is changed on libweston/compositor.c */
			if (!rail_state->isFirstUpdateDone) {
				peerCtx->is_window_zorder_dirty = true;
				rail_state->isFirstUpdateDone = true;
			}
		}

#ifdef HAVE_FREERDP_GFXREDIR_H
		if (!b->use_gfxredir) {
#else
		{
#endif // HAVE_FREERDP_GFXREDIR_H
			if (new_surface_id || rail_state->bufferScaleWidth != scaleWidth || rail_state->bufferScaleHeight != scaleHeight) {
				/* map surface to window */
				assert(new_surface_id == 0 || (new_surface_id == rail_state->surface_id));
				rdp_debug_verbose(b, "MapSurfaceToWindow(surfaceId:0x%x - windowsId:%x)\n",
					rail_state->surface_id, window_id);
				rdp_debug_verbose(b, "	targetWidth:0x%d - targetWidth:%d)\n",
					newClientPos.width, newClientPos.height);
				rdp_debug_verbose(b, "	mappedWidth:0x%d - mappedHeight:%d)\n",
					contentBufferWidth, contentBufferHeight);
				// Always use scaled version to avoid bug in mstsc.exe, mstsc.exe
				// seems can't handle mixed of scale and non-scaled version of procotols.
				RDPGFX_MAP_SURFACE_TO_SCALED_WINDOW_PDU mapSurfaceToScaledWindow = {};
				mapSurfaceToScaledWindow.surfaceId = (UINT16)rail_state->surface_id;
				mapSurfaceToScaledWindow.windowId = window_id;
				mapSurfaceToScaledWindow.mappedWidth = contentBufferWidth;
				mapSurfaceToScaledWindow.mappedHeight = contentBufferHeight;
				mapSurfaceToScaledWindow.targetWidth = newClientPos.width;
				mapSurfaceToScaledWindow.targetHeight = newClientPos.height;
				peerCtx->rail_grfx_server_context->MapSurfaceToScaledWindow(peerCtx->rail_grfx_server_context, &mapSurfaceToScaledWindow);
				rail_state->bufferScaleWidth = scaleWidth;
				rail_state->bufferScaleHeight = scaleHeight;
			}

			/* destroy old surface */
			if (old_surface_id) {
				RDPGFX_DELETE_SURFACE_PDU deleteSurface = {};
				rdp_debug_verbose(b, "DeleteSurface(surfaceId:0x%x for windowId:0x%x)\n", old_surface_id, window_id);
				deleteSurface.surfaceId = (UINT16)old_surface_id;
				peerCtx->rail_grfx_server_context->DeleteSurface(peerCtx->rail_grfx_server_context, &deleteSurface);
			}
		}
	}

	return 0;
}

static void
rdp_rail_update_window_iter(void *element, void *data)
{
	struct weston_surface *surface = (struct weston_surface *)element;
	struct weston_compositor *compositor = surface->compositor;
	struct rdp_backend *b = (struct rdp_backend*)compositor->backend;
	struct update_window_iter_data *iter_data = (struct update_window_iter_data *)data;
	struct weston_surface_rail_state *rail_state = (struct weston_surface_rail_state *)surface->backend_state;
	assert(rail_state); // this iter is looping from window hash table, thus it must have rail_state initialized.
	if (surface->output_mask & (1u << iter_data->output_id)) {
		if (rail_state->isCursor)
			rdp_rail_update_cursor(surface);
		else if (rail_state->isUpdatePending == FALSE)
			rdp_rail_update_window(surface, iter_data);
		else
			rdp_debug_verbose(b, "window update is skipped for windowId:0x%x, isUpdatePending = %d\n",
				rail_state->window_id, rail_state->isUpdatePending);
	}
}

static UINT32
rdp_insert_window_zorder_array(struct weston_view *view, UINT32 *windowIdArray, UINT32 WindowIdArraySize, UINT32 iCurrent)
{
	struct weston_surface *surface = view->surface;
	struct weston_compositor *compositor = surface->compositor;
	struct rdp_backend *b = (struct rdp_backend*)compositor->backend;
	struct weston_surface_rail_state *rail_state =
		(struct weston_surface_rail_state *)surface->backend_state;

	/* insert subsurface first to zorder list */
	struct weston_subsurface *sub;
	wl_list_for_each(sub, &surface->subsurface_list, parent_link) {
		struct weston_view *sub_view;
		wl_list_for_each(sub_view, &sub->surface->views, surface_link) {
			if (sub_view->parent_view != view)
				continue;

			iCurrent = rdp_insert_window_zorder_array(sub_view, windowIdArray, WindowIdArraySize, iCurrent);
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
	    !rail_state->is_minimized &&
	    !rail_state->is_minimized_requested) {
		if (iCurrent >= WindowIdArraySize) {
			rdp_debug_error(b, "%s: more windows in tree than ID manager tracking (%d vs %d)\n",
					__func__, iCurrent, WindowIdArraySize);
			return UINT_MAX;
		}
		if (b->debugLevel >= RDP_DEBUG_LEVEL_VERBOSE) {
			char label[256];
			rdp_rail_dump_window_label(surface, label, sizeof(label));
			rdp_debug_verbose(b, "    window[%d]: %x: %s\n", iCurrent, rail_state->window_id, label);
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
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	UINT32 numWindowId = 0;
	UINT32 *windowIdArray = NULL;
	WINDOW_ORDER_INFO window_order_info = {};
	MONITORED_DESKTOP_ORDER monitored_desktop_order = {};
	UINT32 iCurrent = 0;

	ASSERT_COMPOSITOR_THREAD(b);

	if (!b->enable_window_zorder_sync)
		return;

	numWindowId = peerCtx->windowId.id_used + 1; // +1 for marker window.
	windowIdArray = zalloc(numWindowId * sizeof(UINT32));
	if (!windowIdArray) {
		rdp_debug_error(b, "%s: zalloc(%ld bytes) failed\n", __func__, numWindowId * sizeof(UINT32)); 
		return;
	}

	rdp_debug_verbose(b, "Dump Window Z order\n");
	if (!peerCtx->active_surface) {
		/* if no active window, put marker window top as client window has focus. */
		rdp_debug_verbose(b, "    window[%d]: %x: %s\n", iCurrent, RDP_RAIL_MARKER_WINDOW_ID, "marker window");
		windowIdArray[iCurrent++] = RDP_RAIL_MARKER_WINDOW_ID;
	}
	/* walk windows in z-order */
	struct weston_layer *layer;
	wl_list_for_each(layer, &compositor->layer_list, link) {
		struct weston_view *view;
		wl_list_for_each(view, &layer->view_list.link, layer_link.link) {
			iCurrent = rdp_insert_window_zorder_array(view, windowIdArray, numWindowId, iCurrent);
			if (iCurrent == UINT_MAX)
				goto Exit;
		}
	}
	if (peerCtx->active_surface) {
		/* TODO: marker window better be placed correct place relative to client window, not always bottom */
		/*       In order to do that, dummpy window to be created to track where is the highest client window. */
		rdp_debug_verbose(b, "    window[%d]: %x: %s\n", iCurrent, RDP_RAIL_MARKER_WINDOW_ID, "marker window");
		windowIdArray[iCurrent++] = RDP_RAIL_MARKER_WINDOW_ID;
	}
	assert(iCurrent <= numWindowId);
	assert(iCurrent > 0);
	rdp_debug_verbose(b, "    send Window Z order: numWindowIds:%d\n", iCurrent);

	window_order_info.fieldFlags = WINDOW_ORDER_TYPE_DESKTOP |
					WINDOW_ORDER_FIELD_DESKTOP_ZORDER |
					WINDOW_ORDER_FIELD_DESKTOP_ACTIVE_WND; 
	monitored_desktop_order.activeWindowId = windowIdArray[0];
	monitored_desktop_order.numWindowIds = iCurrent;
	monitored_desktop_order.windowIds = windowIdArray;

	client->update->window->MonitoredDesktop(client->context, &window_order_info, &monitored_desktop_order);
	client->DrainOutputBuffer(client);

Exit:
	if (windowIdArray)
		free(windowIdArray);

	return;
}

void 
rdp_rail_output_repaint(struct weston_output *output, pixman_region32_t *damage)
{
	struct weston_compositor *ec = output->compositor;
	struct rdp_backend *b = to_rdp_backend(ec);
	RdpPeerContext *peerCtx = (RdpPeerContext *)b->rdp_peer->context;

	if (peerCtx->isAcknowledgedSuspended || ((peerCtx->currentFrameId - peerCtx->acknowledgedFrameId) < 2)) {
		rdp_debug_verbose(b, "currentFrameId:0x%x, acknowledgedFrameId:0x%x, isAcknowledgedSuspended:%d\n",
			 peerCtx->currentFrameId, peerCtx->acknowledgedFrameId, peerCtx->isAcknowledgedSuspended);
		struct update_window_iter_data iter_data = {};
		iter_data.output_id = output->id;
		hash_table_for_each(peerCtx->windowId.hash_table, rdp_rail_update_window_iter, (void*) &iter_data);
		if (iter_data.needEndFrame) {
			/* if frame is started at above iteration, send EndFrame here. */
			RDPGFX_END_FRAME_PDU endFrame = {};
			endFrame.frameId = iter_data.startedFrameId;
			rdp_debug_verbose(b, "EndFrame(frameId:0x%x)\n", endFrame.frameId);
			peerCtx->rail_grfx_server_context->EndFrame(peerCtx->rail_grfx_server_context, &endFrame);
		}
		if (peerCtx->is_window_zorder_dirty) {
			/* notify window z order to client */
			rdp_rail_sync_window_zorder(b->compositor);
			peerCtx->is_window_zorder_dirty = false;
		}
		if (iter_data.isUpdatePending) {
			/* By default, compositor won't update idle timer by screen activity,
			   thus, here manually call wake function to postpone idle timer when
			   RDP backend sends frame to client. */
			weston_compositor_wake(b->compositor);
		}
	} else {
		rdp_debug_verbose(b, "frame update is skipped. currentFrameId:%d, acknowledgedFrameId:%d, isAcknowledgedSuspended:%d\n",
			peerCtx->currentFrameId, peerCtx->acknowledgedFrameId, peerCtx->isAcknowledgedSuspended);
	}
	return;
}

BOOL
rdp_rail_peer_activate(freerdp_peer* client)
{
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	rdpSettings *settings = client->settings;
	BOOL rail_server_started = FALSE;
	BOOL disp_server_opened = FALSE;
	BOOL rail_grfx_server_opened = FALSE;
#ifdef HAVE_FREERDP_GFXREDIR_H
	BOOL gfxredir_server_opened = FALSE;
#endif // HAVE_FREERDP_GFXREDIR_H
#ifdef HAVE_FREERDP_RDPAPPLIST_H
	BOOL applist_server_opened = FALSE;
	RDPAPPLIST_SERVER_CAPS_PDU appListCaps = {};
#endif // HAVE_FREERDP_RDPAPPLIST_H
	UINT waitRetry;

	ASSERT_COMPOSITOR_THREAD(b);

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
	peerCtx->rail_server_context = rail_server_context_new(peerCtx->vcm);
	if (!peerCtx->rail_server_context)
		goto error_exit;
	peerCtx->rail_server_context->custom = (void*)client;
	peerCtx->rail_server_context->ClientHandshake = rail_client_Handshake;
	peerCtx->rail_server_context->ClientClientStatus = rail_client_ClientStatus;
	peerCtx->rail_server_context->ClientExec = rail_client_Exec;
	peerCtx->rail_server_context->ClientActivate = rail_client_Activate;
	peerCtx->rail_server_context->ClientSyscommand = rail_client_Syscommand;
	peerCtx->rail_server_context->ClientSysparam = rail_client_ClientSysparam;
	peerCtx->rail_server_context->ClientGetAppidReq = rail_client_ClientGetAppidReq;
	peerCtx->rail_server_context->ClientWindowMove = rail_client_WindowMove;
	peerCtx->rail_server_context->ClientSnapArrange = rail_client_SnapArrange;
	peerCtx->rail_server_context->ClientLangbarInfo = rail_client_LangbarInfo;
	peerCtx->rail_server_context->ClientLanguageImeInfo = rail_client_LanguageImeInfo;
	peerCtx->rail_server_context->ClientCompartmentInfo = rail_client_CompartmentInfo;
	if (peerCtx->rail_server_context->Start(peerCtx->rail_server_context) != CHANNEL_RC_OK)
		goto error_exit;
	rail_server_started = TRUE;

	/* send handshake to client */
	if (settings->RemoteApplicationSupportLevel & RAIL_LEVEL_HANDSHAKE_EX_SUPPORTED) {
		RAIL_HANDSHAKE_EX_ORDER handshakeEx = {};
		UINT32 railHandshakeFlags =
			(TS_RAIL_ORDER_HANDSHAKEEX_FLAGS_HIDEF
			 | TS_RAIL_ORDER_HANDSHAKE_EX_FLAGS_EXTENDED_SPI_SUPPORTED
			 /*| TS_RAIL_ORDER_HANDSHAKE_EX_FLAGS_SNAP_ARRANGE_SUPPORTED*/);
		handshakeEx.buildNumber = 0;
		handshakeEx.railHandshakeFlags = railHandshakeFlags;
		if (peerCtx->rail_server_context->ServerHandshakeEx(peerCtx->rail_server_context, &handshakeEx) != CHANNEL_RC_OK)
			goto error_exit;
		client->DrainOutputBuffer(client); 
	} else {
		RAIL_HANDSHAKE_ORDER handshake = {};
		handshake.buildNumber = 0;
		if (peerCtx->rail_server_context->ServerHandshake(peerCtx->rail_server_context, &handshake) != CHANNEL_RC_OK)
			goto error_exit;
		client->DrainOutputBuffer(client); 
	}

	/* wait handshake reponse from client */
	waitRetry = 0;
	while (!peerCtx->handshakeCompleted) {
		if (++waitRetry > 10000) // timeout after 100 sec.
			goto error_exit;
		USleep(10000); // wait 0.01 sec.
		client->CheckFileDescriptor(client);
		WTSVirtualChannelManagerCheckFileDescriptor(peerCtx->vcm);
	}

	/* open Disp channel */
	peerCtx->disp_server_context = disp_server_context_new(peerCtx->vcm);
	if (!peerCtx->disp_server_context)
		goto error_exit;
	peerCtx->disp_server_context->custom = (void*)client;
	peerCtx->disp_server_context->MaxNumMonitors = RDP_MAX_MONITOR;
	peerCtx->disp_server_context->MaxMonitorAreaFactorA = DISPLAY_CONTROL_MAX_MONITOR_WIDTH;
	peerCtx->disp_server_context->MaxMonitorAreaFactorB = DISPLAY_CONTROL_MAX_MONITOR_HEIGHT;
	peerCtx->disp_server_context->DispMonitorLayout = disp_client_monitor_layout_change;
	if (peerCtx->disp_server_context->Open(peerCtx->disp_server_context) != CHANNEL_RC_OK)
		goto error_exit;
	disp_server_opened = TRUE;
	if (peerCtx->disp_server_context->DisplayControlCaps(peerCtx->disp_server_context) != CHANNEL_RC_OK)
		goto error_exit;

	/* open HiDef (aka rdpgfx) channel. */
	peerCtx->rail_grfx_server_context = rdpgfx_server_context_new(peerCtx->vcm);
	if (!peerCtx->rail_grfx_server_context)
		goto error_exit;
	peerCtx->rail_grfx_server_context->custom = (void*)client;
	peerCtx->rail_grfx_server_context->CapsAdvertise = rail_grfx_client_caps_advertise;
	peerCtx->rail_grfx_server_context->CacheImportOffer = rail_grfx_client_cache_import_offer;
	peerCtx->rail_grfx_server_context->FrameAcknowledge = rail_grfx_client_frame_acknowledge;
	if (!peerCtx->rail_grfx_server_context->Open(peerCtx->rail_grfx_server_context))
		goto error_exit;
	rail_grfx_server_opened = TRUE;

#ifdef HAVE_FREERDP_GFXREDIR_H
	/* open Graphics Redirection channel. */
	if (b->use_gfxredir) {
		peerCtx->gfxredir_server_context = b->gfxredir_server_context_new(peerCtx->vcm);
		if (!peerCtx->gfxredir_server_context)
			goto error_exit;
		peerCtx->gfxredir_server_context->custom = (void*)client;
		peerCtx->gfxredir_server_context->GraphicsRedirectionLegacyCaps = gfxredir_client_graphics_redirection_legacy_caps;
		peerCtx->gfxredir_server_context->GraphicsRedirectionCapsAdvertise = gfxredir_client_graphics_redirection_caps_advertise;
		peerCtx->gfxredir_server_context->PresentBufferAck = gfxredir_client_present_buffer_ack;
		if (peerCtx->gfxredir_server_context->Open(peerCtx->gfxredir_server_context) != CHANNEL_RC_OK)
			goto error_exit;
		gfxredir_server_opened = TRUE;
	}
#endif // HAVE_FREERDP_GFXREDIR_H

#ifdef HAVE_FREERDP_RDPAPPLIST_H
	/* open Application List channel. */
	if (b->rdprail_shell_api &&
		b->rdprail_shell_name &&
		b->use_rdpapplist) {
		peerCtx->applist_server_context = b->rdpapplist_server_context_new(peerCtx->vcm);
		if (!peerCtx->applist_server_context)
			goto error_exit;
		peerCtx->applist_server_context->custom = (void *)client;
		peerCtx->applist_server_context->ApplicationListClientCaps = applist_client_Caps;
		if (peerCtx->applist_server_context->Open(peerCtx->applist_server_context) != CHANNEL_RC_OK)
			goto error_exit;
		applist_server_opened = TRUE;

		rdp_debug(b, "Server AppList caps version:%d\n", RDPAPPLIST_CHANNEL_VERSION);
		appListCaps.version = RDPAPPLIST_CHANNEL_VERSION;
		if (!utf8_string_to_rail_string(b->rdprail_shell_name, &appListCaps.appListProviderName))
			goto error_exit;
		if (peerCtx->applist_server_context->ApplicationListCaps(peerCtx->applist_server_context, &appListCaps) != CHANNEL_RC_OK)
			goto error_exit;
		free(appListCaps.appListProviderName.string);
	}
#endif // HAVE_FREERDP_RDPAPPLIST_H

	/* wait graphics channel (and optionally graphics redir channel) reponse from client */
	waitRetry = 0;
	while (!peerCtx->activationGraphicsCompleted ||
		(gfxredir_server_opened && !peerCtx->activationGraphicsRedirectionCompleted)) {
		if (++waitRetry > 10000) // timeout after 100 sec.
			goto error_exit;
		USleep(10000); // wait 0.01 sec.
		client->CheckFileDescriptor(client);
		WTSVirtualChannelManagerCheckFileDescriptor(peerCtx->vcm);
	}

	return TRUE;

error_exit:

#ifdef HAVE_FREERDP_RDPAPPLIST_H
	if (applist_server_opened) {
		peerCtx->applist_server_context->Close(peerCtx->applist_server_context);
		if (appListCaps.appListProviderName.string)
			free(appListCaps.appListProviderName.string);
	}
	if (peerCtx->applist_server_context) {
		assert(b->rdpapplist_server_context_free);
		b->rdpapplist_server_context_free(peerCtx->applist_server_context);
		peerCtx->applist_server_context = NULL;
	}
#endif // HAVE_FREERDP_RDPAPPLIST_H

#ifdef HAVE_FREERDP_GFXREDIR_H
	if (gfxredir_server_opened)
		peerCtx->gfxredir_server_context->Close(peerCtx->gfxredir_server_context);
	if (peerCtx->gfxredir_server_context) {
		assert(b->gfxredir_server_context_free);
		b->gfxredir_server_context_free(peerCtx->gfxredir_server_context);
		peerCtx->gfxredir_server_context = NULL;
		peerCtx->activationGraphicsRedirectionCompleted = FALSE;
	}
#endif // HAVE_FREERDP_GFXREDIR_H

	if (rail_grfx_server_opened)
		peerCtx->rail_grfx_server_context->Close(peerCtx->rail_grfx_server_context);
	if (peerCtx->rail_grfx_server_context) {
		rdpgfx_server_context_free(peerCtx->rail_grfx_server_context);
		peerCtx->rail_grfx_server_context = NULL;
		peerCtx->activationGraphicsCompleted = FALSE;
	}

	if (disp_server_opened)
		peerCtx->disp_server_context->Close(peerCtx->disp_server_context);
	if (peerCtx->disp_server_context) {
		disp_server_context_free(peerCtx->disp_server_context);
		peerCtx->disp_server_context = NULL;
	}

	if (rail_server_started)
		peerCtx->rail_server_context->Stop(peerCtx->rail_server_context);
	if (peerCtx->rail_server_context) {
		rail_server_context_free(peerCtx->rail_server_context);
		peerCtx->rail_server_context = NULL;
	}

	return FALSE;
}

static void
rdp_rail_idle_handler(struct wl_listener *listener, void *data)
{
	RAIL_POWER_DISPLAY_REQUEST displayRequest;
	RdpPeerContext *peerCtx =
		container_of(listener, RdpPeerContext, idle_listener);
	struct rdp_backend *b = peerCtx->rdpBackend;

	ASSERT_COMPOSITOR_THREAD(b);

	rdp_debug(b, "%s is called on peerCtx:%p\n", __func__, peerCtx);

	displayRequest.active = FALSE;
	peerCtx->rail_server_context->ServerPowerDisplayRequest(
		peerCtx->rail_server_context, &displayRequest);
}

static void
rdp_rail_wake_handler(struct wl_listener *listener, void *data)
{
	RAIL_POWER_DISPLAY_REQUEST displayRequest;
	RdpPeerContext *peerCtx =
		container_of(listener, RdpPeerContext, wake_listener);
	struct rdp_backend *b = peerCtx->rdpBackend;

	ASSERT_COMPOSITOR_THREAD(b);

	rdp_debug(b, "%s is called on peerCtx:%p\n", __func__, peerCtx);

	displayRequest.active = TRUE;
	peerCtx->rail_server_context->ServerPowerDisplayRequest(
		peerCtx->rail_server_context, &displayRequest);
}

static void
rdp_rail_notify_window_zorder_change(struct weston_compositor *compositor, struct weston_surface *active_surface)
{
	struct rdp_backend *b = to_rdp_backend(compositor);
	freerdp_peer* client = b->rdp_peer;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;

	ASSERT_COMPOSITOR_THREAD(b);

	/* active_surface is NULL while client window has focus */
	peerCtx->active_surface = active_surface;
	/* z order will be sent to client at next repaint */
	peerCtx->is_window_zorder_dirty = true;
}

void
rdp_rail_sync_window_status(freerdp_peer* client)
{
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct weston_view *view;

	ASSERT_COMPOSITOR_THREAD(b);

	{
		RAIL_SYSPARAM_ORDER sysParamOrder = {};
		sysParamOrder.param = SPI_SETSCREENSAVESECURE;
		sysParamOrder.setScreenSaveSecure = 0;
		peerCtx->rail_server_context->ServerSysparam(peerCtx->rail_server_context, &sysParamOrder);
		client->DrainOutputBuffer(client); 
	}

	{
		RAIL_SYSPARAM_ORDER sysParamOrder = {};
		sysParamOrder.param = SPI_SETSCREENSAVEACTIVE;
		sysParamOrder.setScreenSaveActive = 0;
		peerCtx->rail_server_context->ServerSysparam(peerCtx->rail_server_context, &sysParamOrder);
		client->DrainOutputBuffer(client); 
	}

	{
		RAIL_ZORDER_SYNC zOrderSync = {};
		zOrderSync.windowIdMarker = RDP_RAIL_MARKER_WINDOW_ID;
		peerCtx->rail_server_context->ServerZOrderSync(peerCtx->rail_server_context, &zOrderSync);
		client->DrainOutputBuffer(client); 
	}

	{
		WINDOW_ORDER_INFO window_order_info = {};
		MONITORED_DESKTOP_ORDER monitored_desktop_order = {};

		window_order_info.windowId = RDP_RAIL_MARKER_WINDOW_ID;
		window_order_info.fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_HOOKED | WINDOW_ORDER_FIELD_DESKTOP_ARC_BEGAN;

		client->update->window->MonitoredDesktop(client->update->context, &window_order_info, &monitored_desktop_order);
		client->DrainOutputBuffer(client);
	}

	{
		UINT32 windowsIdArray[1] = {};
		WINDOW_ORDER_INFO window_order_info = {};
		MONITORED_DESKTOP_ORDER monitored_desktop_order = {};

		window_order_info.windowId = RDP_RAIL_MARKER_WINDOW_ID;
		window_order_info.fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_ZORDER | WINDOW_ORDER_FIELD_DESKTOP_ACTIVE_WND;

		monitored_desktop_order.activeWindowId = RDP_RAIL_DESKTOP_WINDOW_ID;
		monitored_desktop_order.numWindowIds = 1;
		windowsIdArray[0] = RDP_RAIL_MARKER_WINDOW_ID;
		monitored_desktop_order.windowIds = (UINT*)&windowsIdArray;

		client->update->window->MonitoredDesktop(client->update->context, &window_order_info, &monitored_desktop_order);
		client->DrainOutputBuffer(client);
	}

	{
		WINDOW_ORDER_INFO window_order_info = {};
		MONITORED_DESKTOP_ORDER monitored_desktop_order = {};

		window_order_info.windowId = RDP_RAIL_MARKER_WINDOW_ID;
		window_order_info.fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_ARC_COMPLETED;

		client->update->window->MonitoredDesktop(client->update->context, &window_order_info, &monitored_desktop_order);
		client->DrainOutputBuffer(client);
	}

	peerCtx->activationRailCompleted = true;

	{
		wl_list_for_each(view, &b->compositor->view_list, link) {
			struct weston_surface *surface = view->surface;
			struct weston_subsurface *sub;
			struct weston_surface_rail_state *rail_state = (struct weston_surface_rail_state *)surface->backend_state;
			if (!rail_state || rail_state->window_id == 0) {
				rdp_rail_create_window(NULL, (void *)surface);
				rail_state = (struct weston_surface_rail_state *)surface->backend_state;
				if (rail_state && rail_state->window_id) {
					if (b->rdprail_shell_api &&
						b->rdprail_shell_api->request_window_icon)
						b->rdprail_shell_api->request_window_icon(surface);
				}
				wl_list_for_each(sub, &surface->subsurface_list, parent_link) {
					struct weston_surface_rail_state *sub_rail_state = (struct weston_surface_rail_state *)sub->surface->backend_state;
					if (sub->surface == surface)
						continue;
					if (!sub_rail_state || sub_rail_state->window_id == 0)
						rdp_rail_create_window(NULL, (void *)sub->surface);
				}
			}
		}

		/* this assume repaint to be scheduled on idle loop, not directly from here */
		weston_compositor_damage_all(b->compositor);
	}

	if (peerCtx->clientStatusFlags & TS_RAIL_CLIENTSTATUS_POWER_DISPLAY_REQUEST_SUPPORTED) {
		RAIL_POWER_DISPLAY_REQUEST displayRequest;

		/* subscribe idle/wake signal from compositor */
		peerCtx->idle_listener.notify = rdp_rail_idle_handler;
		wl_signal_add(&b->compositor->idle_signal, &peerCtx->idle_listener);
		peerCtx->wake_listener.notify = rdp_rail_wake_handler;
		wl_signal_add(&b->compositor->wake_signal, &peerCtx->wake_listener);

		displayRequest.active = TRUE;
		peerCtx->rail_server_context->ServerPowerDisplayRequest(
			peerCtx->rail_server_context, &displayRequest);

		/* Upon client connection, make sure compositor is in wake state */
		weston_compositor_wake(b->compositor);
	}
}

void
rdp_rail_start_window_move(
	struct weston_surface* surface, 
	int pointerGrabX, 
	int pointerGrabY,
	struct weston_size minSize,
	struct weston_size maxSize)
{
	struct weston_compositor *compositor = surface->compositor;
	struct weston_surface_rail_state *rail_state = (struct weston_surface_rail_state *)surface->backend_state;
	struct rdp_backend *b = (struct rdp_backend*)compositor->backend;
	RdpPeerContext *peerCtx = (RdpPeerContext *)b->rdp_peer->context;;
	RAIL_MINMAXINFO_ORDER minmax_order;
	RAIL_LOCALMOVESIZE_ORDER move_order;

	if (!b->rdp_peer || !b->rdp_peer->settings->HiDefRemoteApp) {
			return;
	}

	ASSERT_COMPOSITOR_THREAD(b);
	assert(rail_state);

	int posX=0, posY=0;
	int numViews = 0;
	struct weston_view* view;
	wl_list_for_each(view, &surface->views, surface_link) {
		numViews++;
		posX = view->geometry.x;
		posY = view->geometry.y;
		break;
	}
	if (numViews == 0) {
		view = NULL;
		rdp_debug_verbose(b, "%s: surface has no view (windowId:0x%x)\n", __func__, rail_state->window_id);
	}

	/* TODO: HI-DPI MULTIMON */

	rdp_debug(b, "====================== StartWindowMove =============================\n");	
	rdp_debug(b, "WindowsPosition - Pre-move (%d, %d, %d, %d).\n",
		to_client_x(peerCtx, posX), to_client_y(peerCtx, posY), surface->width, surface->height);

	/* Inform the RDP client about the minimum/maximum width and height allowed
	 * on this window.
	 */ 
	minmax_order.windowId = rail_state->window_id;
	minmax_order.maxPosX = 0; 
	minmax_order.maxPosY = 0;
	minmax_order.maxWidth = 0;
	minmax_order.maxHeight = 0;
	minmax_order.minTrackWidth = minSize.width;
	minmax_order.minTrackHeight = minSize.height;
	minmax_order.maxTrackWidth = maxSize.width;
	minmax_order.maxTrackHeight = maxSize.height;

	rdp_debug(b, "maxPosX: %d, maxPosY: %d, maxWidth: %d, maxHeight: %d, minTrackWidth: %d, minTrackHeight: %d, maxTrackWidth: %d, maxTrackHeight: %d\n", 
		minmax_order.maxPosX, 
		minmax_order.maxPosY,
		minmax_order.maxWidth,
		minmax_order.maxHeight,
		minmax_order.minTrackWidth,
		minmax_order.minTrackHeight,
		minmax_order.maxTrackWidth,
		minmax_order.maxTrackHeight);

	peerCtx->rail_server_context->ServerMinMaxInfo(
		peerCtx->rail_server_context, &minmax_order);

	/* Start the local Window move.
	 */ 
	move_order.windowId = rail_state->window_id;
	move_order.isMoveSizeStart = true;
	move_order.moveSizeType = RAIL_WMSZ_MOVE;
	move_order.posX = pointerGrabX - posX;
	move_order.posY = pointerGrabY - posY;

	rdp_debug(b, "posX: %d, posY: %d \n", move_order.posX, move_order.posY);

	peerCtx->rail_server_context->ServerLocalMoveSize(
					peerCtx->rail_server_context, &move_order);
}

void
rdp_rail_end_window_move(struct weston_surface* surface)
{
	struct weston_compositor *compositor = surface->compositor;
	struct weston_surface_rail_state *rail_state = (struct weston_surface_rail_state *)surface->backend_state;
	struct rdp_backend *b = (struct rdp_backend*)compositor->backend;
	RdpPeerContext *peerCtx = NULL;
	RAIL_LOCALMOVESIZE_ORDER move_order;

	if (!b->rdp_peer || !b->rdp_peer->settings->HiDefRemoteApp) {
		return;
	}

	ASSERT_COMPOSITOR_THREAD(b);
	assert(rail_state);

	peerCtx = (RdpPeerContext *)b->rdp_peer->context;

	int posX=0, posY=0;
	int numViews = 0;
	struct weston_view *view;
	wl_list_for_each(view, &surface->views, surface_link) {
		numViews++;
		posX = to_client_x(peerCtx, view->geometry.x);
		posY = to_client_y(peerCtx, view->geometry.y);
		break;
	}
	if (numViews == 0) {
		view = NULL;
		rdp_debug_verbose(b, "%s: surface has no view (windowId:0x%x)\n", __func__, rail_state->window_id);
	}

	/* TODO: HI-DPI MULTIMON */

	move_order.windowId = rail_state->window_id;
	move_order.isMoveSizeStart = false;
	move_order.moveSizeType = RAIL_WMSZ_MOVE;
	move_order.posX = posX;
	move_order.posY = posY;	

	peerCtx->rail_server_context->ServerLocalMoveSize(
		peerCtx->rail_server_context, &move_order);

	rdp_debug(b, "WindowsPosition - Post-move (%d, %d, %d, %d).\n", posX, posY, surface->width, surface->height);
	rdp_debug(b, "====================== EndWindowMove =============================\n");
}

static void
rdp_rail_destroy_window_iter(void *element, void *data)
{
	struct weston_surface *surface = (struct weston_surface *)element;
	rdp_rail_destroy_window(NULL, (void *)surface);
}

void
rdp_rail_peer_context_free(freerdp_peer* client, RdpPeerContext* context)
{
	struct rdp_loop_event_source *current, *next;

	if (context->windowId.hash_table)
		hash_table_for_each(context->windowId.hash_table, rdp_rail_destroy_window_iter, NULL);

#ifdef HAVE_FREERDP_RDPAPPLIST_H
	if (context->applist_server_context) {
		struct rdp_backend *b = context->rdpBackend;
		if (context->isAppListEnabled)
			context->rdpBackend->rdprail_shell_api->stop_app_list_update(context->rdpBackend->rdprail_shell_context);
		context->applist_server_context->Close(context->applist_server_context);
		assert(b->rdpapplist_server_context_free);
		b->rdpapplist_server_context_free(context->applist_server_context);
	}
#endif // HAVE_FREERDP_RDPAPPLIST_H

#ifdef HAVE_FREERDP_GFXREDIR_H
	if (context->gfxredir_server_context) {
		struct rdp_backend *b = context->rdpBackend;
		context->gfxredir_server_context->Close(context->gfxredir_server_context);
		assert(b->gfxredir_server_context_free);
		b->gfxredir_server_context_free(context->gfxredir_server_context);
	}
#endif // HAVE_FREERDP_GFXREDIR_H

	if (context->rail_grfx_server_context) {
		context->rail_grfx_server_context->Close(context->rail_grfx_server_context);
		rdpgfx_server_context_free(context->rail_grfx_server_context);
	}

	if (context->disp_server_context) {
		context->disp_server_context->Close(context->disp_server_context);
		disp_server_context_free(context->disp_server_context);
	}

	if (context->rail_server_context) {
		context->rail_server_context->Stop(context->rail_server_context);
		rail_server_context_free(context->rail_server_context);
	}

	/* after stopping all FreeRDP server context, no more work to be queued, free anything remained */ 
	wl_list_for_each_safe(current, next, &context->loop_event_source_list, link) {
		wl_event_source_remove(current->event_source);
		wl_list_remove(&current->link);
		free(current);
	}
	pthread_mutex_destroy(&context->loop_event_source_list_mutex);

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
#endif // HAVE_FREERDP_GFXREDIR_H
	rdp_id_manager_free(&context->surfaceId);
	rdp_id_manager_free(&context->windowId);

	pixman_region32_fini(&context->regionClientHeads);
	pixman_region32_fini(&context->regionWestonHeads);
}

BOOL
rdp_drdynvc_init(freerdp_peer *client)
{
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;

	ASSERT_COMPOSITOR_THREAD(peerCtx->rdpBackend);

	/* Open Dynamic virtual channel */
	peerCtx->drdynvc_server_context = drdynvc_server_context_new(peerCtx->vcm);
	if (!peerCtx->drdynvc_server_context)
		return FALSE;
	if (peerCtx->drdynvc_server_context->Start(peerCtx->drdynvc_server_context) != CHANNEL_RC_OK) {
		drdynvc_server_context_free(peerCtx->drdynvc_server_context);
		peerCtx->drdynvc_server_context = NULL;
		return FALSE;
	}

	/* Force Dynamic virtual channel to exchange caps */
	if (WTSVirtualChannelManagerGetDrdynvcState(peerCtx->vcm) == DRDYNVC_STATE_NONE) {
		client->activated = TRUE;
		/* Wait reply to arrive from client */
		UINT waitRetry = 0;
		while (WTSVirtualChannelManagerGetDrdynvcState(peerCtx->vcm) != DRDYNVC_STATE_READY) {
			if (++waitRetry > 10000) { // timeout after 100 sec.
				rdp_drdynvc_destroy(peerCtx);
				return FALSE;
			}
			USleep(10000); // wait 0.01 sec.
			client->CheckFileDescriptor(client);
			WTSVirtualChannelManagerCheckFileDescriptor(peerCtx->vcm);
		}
	}

	return TRUE;
}

void
rdp_drdynvc_destroy(RdpPeerContext* context)
{
	if (context->drdynvc_server_context) {
		context->drdynvc_server_context->Stop(context->drdynvc_server_context);
		drdynvc_server_context_free(context->drdynvc_server_context);
	}
}

BOOL
rdp_rail_peer_init(freerdp_peer *client, RdpPeerContext *peerCtx)
{
	struct rdp_backend *b = peerCtx->rdpBackend;

	/* RDP window ID must be within 31 bits range. MSB is reserved and exclude 0. */
	if (!rdp_id_manager_init(b, &peerCtx->windowId, 0x1, 0x7FFFFFFF)) {
		rdp_debug_error(b, "unable to create windowId.\n");
		goto error_return;
	}
	/* RDP surface ID must be within 16 bits range, exclude 0. */
	if (!rdp_id_manager_init(b, &peerCtx->surfaceId, 0x1, 0xFFFF)) {
		rdp_debug_error(b, "unable to create windowId.\n");
		goto error_return;
	}
#ifdef HAVE_FREERDP_GFXREDIR_H
	/* RDP pool ID must be within 32 bits range, exclude 0. */
	if (!rdp_id_manager_init(b, &peerCtx->poolId, 0x1, 0xFFFFFFFF)) {
		rdp_debug_error(b, "unable to create windowId.\n");
		goto error_return;
	}
	/* RDP buffer ID must be within 32 bits range, exclude 0. */
	if (!rdp_id_manager_init(b, &peerCtx->bufferId, 0x1, 0xFFFFFFFF)) {
		rdp_debug_error(b, "unable to create windowId.\n");
		goto error_return;
	}
#endif // HAVE_FREERDP_GFXREDIR_H

	pthread_mutex_init(&peerCtx->loop_event_source_list_mutex, NULL);
	wl_list_init(&peerCtx->loop_event_source_list);

	peerCtx->currentFrameId = 0;
	peerCtx->acknowledgedFrameId = 0;

	pixman_region32_init(&peerCtx->regionClientHeads);
	pixman_region32_init(&peerCtx->regionWestonHeads);

	return TRUE;

error_return:

#ifdef HAVE_FREERDP_GFXREDIR_H
	rdp_id_manager_free(&peerCtx->bufferId);
	rdp_id_manager_free(&peerCtx->poolId);
#endif // HAVE_FREERDP_GFXREDIR_H
	rdp_id_manager_free(&peerCtx->surfaceId);
	rdp_id_manager_free(&peerCtx->windowId);

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
		fprintf(fp,"    %s\n", name);
	print_matrix_type(fp, matrix->type);
	for (i = 0; i < 4; i++)
		fprintf(fp,"        %8.2f, %8.2f, %8.2f, %8.2f\n",
			matrix->d[4*i+0], matrix->d[4*i+1], matrix->d[4*1+2], matrix->d[4*i+3]);
}

static void
print_rdp_head(FILE *fp, const struct rdp_head *current)
{
	fprintf(fp,"    rdp_head: %s: index:%d: is_primary:%d\n",
		current->base.name, current->index,
		current->monitorMode.monitorDef.is_primary);
	fprintf(fp,"    x:%d, y:%d, RDP client x:%d, y:%d\n",
		current->base.output->x, current->base.output->y,
		current->monitorMode.monitorDef.x, current->monitorMode.monitorDef.y);
	fprintf(fp,"    width:%d, height:%d, RDP client width:%d, height: %d\n",
		current->base.output->width, current->base.output->height,
		current->monitorMode.monitorDef.width, current->monitorMode.monitorDef.height);
	fprintf(fp,"    physicalWidth:%dmm, physicalHeight:%dmm, orientation:%d\n",
		current->monitorMode.monitorDef.attributes.physicalWidth,
		current->monitorMode.monitorDef.attributes.physicalHeight,
		current->monitorMode.monitorDef.attributes.orientation);
	fprintf(fp,"    desktopScaleFactor:%d, deviceScaleFactor:%d\n",
		current->monitorMode.monitorDef.attributes.desktopScaleFactor,
		current->monitorMode.monitorDef.attributes.deviceScaleFactor);
	fprintf(fp,"    scale:%d, client scale :%3.2f\n",
		current->monitorMode.scale, current->monitorMode.clientScale);
	fprintf(fp,"    regionClient: x1:%d, y1:%d, x2:%d, y2:%d\n",
		current->regionClient.extents.x1, current->regionClient.extents.y1,
		current->regionClient.extents.x2, current->regionClient.extents.y2);
	fprintf(fp,"    regionWeston: x1:%d, y1:%d, x2:%d, y2:%d\n",
		current->regionWeston.extents.x1, current->regionWeston.extents.y1,
		current->regionWeston.extents.x2, current->regionWeston.extents.y2);
	fprintf(fp,"    workarea: x:%d, y:%d, width:%d, height:%d\n",
		current->workarea.x, current->workarea.y,
		current->workarea.width, current->workarea.height);
	fprintf(fp,"    RDP client workarea: x:%d, y:%d, width:%d, height%d\n",
		current->workareaClient.x, current->workareaClient.y,
		current->workareaClient.width, current->workareaClient.height);
	fprintf(fp,"    connected:%d, non_desktop:%d\n",
		current->base.connected, current->base.non_desktop);
	fprintf(fp,"    assigned output: %s\n",
		current->base.output ? current->base.output->name : "(no output)");
	if (current->base.output) {
		fprintf(fp,"    output extents box: x1:%d, y1:%d, x2:%d, y2:%d\n",
			current->base.output->region.extents.x1, current->base.output->region.extents.y1,
			current->base.output->region.extents.x2, current->base.output->region.extents.y2);
		fprintf(fp,"    output scale:%d, output native_scale:%d\n",
			current->base.output->scale, current->base.output->native_scale);
		print_matrix(fp, "global to output matrix:", &current->base.output->matrix);
		print_matrix(fp, "output to global matrix:", &current->base.output->inverse_matrix);
	}
}

static void
rdp_rail_dump_monitor_binding(struct weston_keyboard *keyboard,
			const struct timespec *time, uint32_t key, void *data)
{
	struct rdp_backend *b = (struct rdp_backend *)data;
	if (b) {
		struct rdp_head *current;
		int err;
		char *str;
		size_t len;
		FILE *fp = open_memstream(&str, &len);
		assert(fp);
		fprintf(fp,"\nrdp debug binding 'M' - dump all monitor.\n");
		wl_list_for_each(current, &b->head_list, link) {
			print_rdp_head(fp, current);
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
	RdpPeerContext *peerCtx;
};

static void
rdp_rail_dump_window_label(struct weston_surface *surface, char *label, uint32_t label_size)
{
	if (surface->get_label) {
		strcpy(label, "Label: "); // 7 chars
		surface->get_label(surface, label + 7, label_size - 7);
	} else if (surface->role_name) {
		snprintf(label, label_size, "RoleName: %s", surface->role_name);
	} else {
		strcpy(label, "(No Label, No Role name)");
	}
}

static void
rdp_rail_dump_window_iter(void *element, void *data)
{
	struct weston_surface *surface = (struct weston_surface *)element;
	struct weston_surface_rail_state *rail_state = (struct weston_surface_rail_state *)surface->backend_state;
	struct rdp_rail_dump_window_context *context = (struct rdp_rail_dump_window_context *)data;
	assert(rail_state); // this iter is looping from window hash table, thus it must have rail_state initialized.
	FILE *fp = context->fp;
	char label[256] = {};
	struct weston_view *view;
	int contentBufferWidth, contentBufferHeight;
	weston_surface_get_content_size(surface, &contentBufferWidth, &contentBufferHeight);

	rdp_rail_dump_window_label(surface, label, sizeof(label));
	fprintf(fp,"    %s\n", label);
	fprintf(fp,"    WindowId:0x%x, SurfaceId:0x%x\n",
		rail_state->window_id, rail_state->surface_id);
	fprintf(fp,"    PoolId:0x%x, BufferId:0x%x\n",
		rail_state->pool_id, rail_state->buffer_id);
	fprintf(fp,"    Position x:%d, y:%d\n",
		rail_state->pos.x, rail_state->pos.y);
	fprintf(fp,"    width:%d, height:%d\n",
		rail_state->pos.width, rail_state->pos.height);
	fprintf(fp,"    RDP client position x:%d, y:%d\n",
		rail_state->clientPos.x, rail_state->clientPos.y);
	fprintf(fp,"    RDP client width:%d, height:%d\n",
		rail_state->clientPos.width, rail_state->clientPos.height);
	fprintf(fp,"    bufferWidth:%d, bufferHeight:%d\n",
		rail_state->bufferWidth, rail_state->bufferHeight);
	fprintf(fp,"    bufferScaleWidth:%.2f, bufferScaleHeight:%.2f\n",
		rail_state->bufferScaleWidth, rail_state->bufferScaleHeight);
	fprintf(fp,"    contentBufferWidth:%d, contentBufferHeight:%d\n",
		contentBufferWidth, contentBufferHeight);
	fprintf(fp,"    input extents: x1:%d, y1:%d, x2:%d, y2:%d\n",
		surface->input.extents.x1, surface->input.extents.y1,
		surface->input.extents.x2, surface->input.extents.y2);
	fprintf(fp,"    is_opaque:%d\n", surface->is_opaque);
	if (!surface->is_opaque && pixman_region32_not_empty(&surface->opaque)) {
		int numRects = 0;
		pixman_box32_t *rects = pixman_region32_rectangles(&surface->opaque, &numRects);
		fprintf(fp, "    opaque region: numRects:%d\n", numRects);
		for (int n = 0; n < numRects; n++)
			fprintf(fp, "        [%d]: (%d, %d) - (%d, %d)\n",
				n, rects[n].x1, rects[n].y1, rects[n].x2, rects[n].y2);
	}
	fprintf(fp,"    parent_surface:%p, isCursor:%d, isWindowCreated:%d\n",
		rail_state->parent_surface, rail_state->isCursor, rail_state->isWindowCreated);
	fprintf(fp,"    isWindowMinimized:%d, isWindowMinimizedRequested:%d\n",
		rail_state->is_minimized, rail_state->is_minimized_requested);
	fprintf(fp,"    isWindowMaximized:%d, isWindowMaximizedRequested:%d\n",
		rail_state->is_maximized, rail_state->is_maximized_requested);
	fprintf(fp,"    isWindowFullscreen:%d, isWindowFullscreenRequested:%d\n",
		rail_state->is_fullscreen, rail_state->is_fullscreen_requested);
	fprintf(fp,"    forceRecreateSurface:%d, error:%d\n",
		rail_state->forceRecreateSurface, rail_state->error);
	fprintf(fp,"    isUdatePending:%d, isFirstUpdateDone:%d\n",
		rail_state->isUpdatePending, rail_state->isFirstUpdateDone);
	fprintf(fp,"    surface:0x%p\n", surface);
	wl_list_for_each(view, &surface->views, surface_link) {
		fprintf(fp,"    view: %p\n", view);
		fprintf(fp,"    view's alpha: %3.2f\n", view->alpha);
		fprintf(fp,"    view's opaque region: x1:%d, y1:%d, x2:%d, y2:%d\n",
			view->transform.opaque.extents.x1,
			view->transform.opaque.extents.y1,
			view->transform.opaque.extents.x2,
			view->transform.opaque.extents.y2);
		if (pixman_region32_not_empty(&view->transform.opaque)) {
			int numRects = 0;
			pixman_box32_t *rects = pixman_region32_rectangles(&view->transform.opaque, &numRects);
			fprintf(fp,"    view's opaque region: numRects:%d\n", numRects);
			for (int n = 0; n < numRects; n++)
				fprintf(fp, "        [%d]: (%d, %d) - (%d, %d)\n",
					n, rects[n].x1, rects[n].y1, rects[n].x2, rects[n].y2);
		}
		fprintf(fp,"    view's boundingbox: x1:%d, y1:%d, x2:%d, y2:%d\n",
			view->transform.boundingbox.extents.x1,
			view->transform.boundingbox.extents.y1,
			view->transform.boundingbox.extents.x2,
			view->transform.boundingbox.extents.y2);
		fprintf(fp,"    view's scissor: x1:%d, y1:%d, x2:%d, y2:%d\n",
			view->geometry.scissor.extents.x1,
			view->geometry.scissor.extents.y1,
			view->geometry.scissor.extents.x2,
			view->geometry.scissor.extents.y2);
		fprintf(fp,"    view's transform: enabled:%d\n",
			view->transform.enabled);
		if (view->transform.enabled)
			print_matrix(fp, NULL, &view->transform.matrix);
	}
	print_matrix(fp, "buffer to surface matrix:", &surface->buffer_to_surface_matrix);
	print_matrix(fp, "surface to buffer matrix:", &surface->surface_to_buffer_matrix);
	fprintf(fp,"    output:0x%p (%s)\n", surface->output,
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
			const struct timespec *time, uint32_t key, void *data)
{
	struct rdp_backend *b = (struct rdp_backend *)data;
	RdpPeerContext *peerCtx;
	if (b && b->rdp_peer && b->rdp_peer->context) {
		/* print window from window hash table */
		struct rdp_rail_dump_window_context context;
		int err;
		char *str;
		size_t len;
		FILE *fp = open_memstream(&str, &len);
		assert(fp);
		fprintf(fp,"\nrdp debug binding 'W' - dump all window from window hash_table.\n");
		peerCtx = (RdpPeerContext *)b->rdp_peer->context;
		dump_id_manager_state(fp, &peerCtx->windowId, "windowId");
		dump_id_manager_state(fp, &peerCtx->surfaceId, "surfaceId");
#ifdef HAVE_FREERDP_GFXREDIR_H
		dump_id_manager_state(fp, &peerCtx->poolId, "poolId");
		dump_id_manager_state(fp, &peerCtx->bufferId, "bufferId");
#endif // HAVE_FREERDP_GFXREDIR_H
		context.peerCtx = peerCtx;
		context.fp = fp;
		hash_table_for_each(peerCtx->windowId.hash_table, rdp_rail_dump_window_iter, (void*)&context);
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
rdp_rail_shell_initialize_notify(struct weston_compositor *compositor, const struct weston_rdprail_shell_api *rdprail_shell_api, void *context, char *name)
{
	struct rdp_backend *b = to_rdp_backend(compositor);
	b->rdprail_shell_api = rdprail_shell_api;
	b->rdprail_shell_context = context;
	if (b->rdprail_shell_name)
		free(b->rdprail_shell_name);
	b->rdprail_shell_name = name ? strdup(name) : NULL;
	rdp_debug(b, "%s: shell: distro name: %s\n",__func__, b->rdprail_shell_name);
	return (void *) b;
}

#define WINDOW_ORDER_ICON_ROWLENGTH( W, BPP ) ((((W) * (BPP) + 31) / 32) * 4)

static void
rdp_rail_set_window_icon(struct weston_surface *surface, pixman_image_t *icon)
{
	struct weston_surface_rail_state *rail_state = (struct weston_surface_rail_state *)surface->backend_state;
	struct weston_compositor *compositor = surface->compositor;
	struct rdp_backend *b = (struct rdp_backend*)compositor->backend;
	RdpPeerContext *peerCtx;
	WINDOW_ORDER_INFO orderInfo = {};
	WINDOW_ICON_ORDER iconOrder = {};
	ICON_INFO iconInfo = {};
	pixman_image_t *scaledIcon = NULL;
	bool bitsColorAllocated = false;
	void *bitsColor = NULL;
	void *bitsMask = NULL;
	int width;
	int height;
	int stride;
	double xScale;
	double yScale;
	struct pixman_transform transform;
	pixman_format_code_t format;
	int maxIconWidth;
	int maxIconHeight;
	int targetIconWidth;
	int targetIconHeight;

	if (!b || !b->rdp_peer) {
		rdp_debug_error(b, "set_window_icon(): rdp_peer is not initalized\n");
		return;
	}

	peerCtx = (RdpPeerContext *)b->rdp_peer->context;

	if (!b->rdp_peer->settings->HiDefRemoteApp)
		return;

	ASSERT_COMPOSITOR_THREAD(b);

	if (!rail_state || rail_state->window_id == 0) {
		rdp_rail_create_window(NULL, (void *)surface);
		rail_state = (struct weston_surface_rail_state *)surface->backend_state;
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
	if (peerCtx->clientStatusFlags & TS_RAIL_CLIENTSTATUS_HIGH_DPI_ICONS_SUPPORTED) {
		maxIconWidth = 96;
		maxIconHeight = 96;
	} else {
		maxIconWidth = 32;
		maxIconHeight = 32;
	}

	if (width > maxIconWidth)
		targetIconWidth = maxIconWidth;
	else
		targetIconWidth = width;

	if (height > maxIconHeight)
		targetIconHeight = maxIconHeight;
	else
		targetIconHeight = height;

	/* create icon bitmap with flip in Y-axis, and client always expects a8r8g8b8 format. */
	scaledIcon = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8,
			targetIconWidth, targetIconHeight, NULL, 0);
	if (!scaledIcon)
		return;

	xScale = (double)width / targetIconWidth;
	yScale = (double)height / targetIconHeight;
	pixman_transform_init_scale(&transform,
			pixman_double_to_fixed(xScale),
			pixman_double_to_fixed(yScale * -1)); // flip Y.
	pixman_transform_translate(&transform, NULL,
			0, pixman_int_to_fixed(height));
	pixman_image_set_transform(icon, &transform);
	pixman_image_set_filter(icon, PIXMAN_FILTER_BILINEAR, NULL, 0);

	pixman_image_composite32(PIXMAN_OP_SRC,
			icon, /* src */
			NULL, /* mask */
			scaledIcon, /* dest */
			0, 0, /* src_x, src_y */
			0, 0, /* mask_x, mask_y */
			0, 0, /* dest_x, dest_y */
			targetIconWidth, /* width */
			targetIconHeight /* height */);

	pixman_image_set_filter(icon, PIXMAN_FILTER_NEAREST, NULL, 0);
	pixman_image_set_transform(icon, NULL);

	icon = scaledIcon;
	width = pixman_image_get_width(icon);
	height = pixman_image_get_height(icon);
	format = pixman_image_get_format(icon);
	stride = pixman_image_get_stride(icon);

	assert(width == targetIconWidth);
	assert(height == targetIconHeight);
	assert(format == PIXMAN_a8r8g8b8);

	rdp_debug_verbose(b, "rdp_rail_set_window_icon: converted icon width:%d height:%d format:%d\n",
			width, height, format);

	/* color bitmap is 32 bits */
	int strideColor = WINDOW_ORDER_ICON_ROWLENGTH(width, 32);
	int sizeColor = strideColor * height;
	if (strideColor != stride) {
		/* when pixman's stride is differnt from client's expetation, need to adjust. */
		sizeColor = strideColor * height;
		bitsColor = malloc(sizeColor);
		if (!bitsColor)
			goto exit;
		bitsColorAllocated = true;
	} else {
		bitsColor = (char *)pixman_image_get_data(icon);
	}

	/* Mask is 1 bit */
	int strideMask = WINDOW_ORDER_ICON_ROWLENGTH(width, 1);
	int sizeMask = strideMask * height;
	bitsMask = zalloc(sizeMask);
	if (!bitsMask)
		goto exit;

	/* generate mask and copy color bits, match to the stride RDP wants when different. */
	char *srcColor = (char *)pixman_image_get_data(icon);
	char *dstColor = (char *)bitsColor;
	char *dstMask = (char *)bitsMask;
	for (int i = 0; i < height; i++) {
		uint32_t *src = (uint32_t *)srcColor;
		uint32_t *dst = (uint32_t *)dstColor;
		char *mask = dstMask;
		for (int j = 0; j < width; j++) {
			if (dst != src)
				*dst = *src;
			if (*dst & 0xFF000000)
				mask[j / 8] |= (0x80 >> (j % 8));
			dst++; src++;
		}
		srcColor += stride;
		dstColor += strideColor;
		dstMask += strideMask;
	}

	orderInfo.windowId = rail_state->window_id;
	orderInfo.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_ICON;
	iconInfo.cacheEntry = 0xFFFF; // no cache
	iconInfo.cacheId = 0xFF; // no cache
	iconInfo.bpp = 32;
	iconInfo.width = (UINT32)width;
	iconInfo.height = (UINT32)height;
	iconInfo.cbColorTable = 0;
	iconInfo.cbBitsMask = sizeMask;
	iconInfo.cbBitsColor = sizeColor;
	iconInfo.bitsMask = bitsMask;
	iconInfo.colorTable = NULL;
	iconInfo.bitsColor = bitsColor;
	iconOrder.iconInfo = &iconInfo;

	b->rdp_peer->update->BeginPaint(b->rdp_peer->update->context);
	b->rdp_peer->update->window->WindowIcon(b->rdp_peer->update->context, &orderInfo, &iconOrder);
	b->rdp_peer->update->EndPaint(b->rdp_peer->update->context);

exit:
	if (bitsMask)
		free(bitsMask);
	if (bitsColorAllocated)
		free(bitsColor);
	if (scaledIcon)
		pixman_image_unref(scaledIcon);

	return;
}

#ifdef HAVE_FREERDP_RDPAPPLIST_H
static bool
rdp_rail_notify_app_list(void *rdp_backend, struct weston_rdprail_app_list_data *app_list_data)
{
	struct rdp_backend *b = (struct rdp_backend*)rdp_backend;
	RdpPeerContext *peerCtx;

	if (!b || !b->rdp_peer) {
		rdp_debug_error(b, "rdp_rail_notify_app_list(): rdp_peer is not initalized\n");
		return false; // return false only when peer is not ready for possible re-send.
	}

	if (!b->rdp_peer->settings->HiDefRemoteApp)
		return true;

	peerCtx = (RdpPeerContext *)b->rdp_peer->context;

	if (!peerCtx->applist_server_context)
		return false;

	rdp_debug(b, "rdp_rail_notify_app_list(): rdp_peer %p\n", peerCtx);
	rdp_debug(b, "    inSync: %d\n", app_list_data->inSync);
	rdp_debug(b, "    syncStart: %d\n", app_list_data->syncStart);
	rdp_debug(b, "    syncEnd: %d\n", app_list_data->syncEnd);
	rdp_debug(b, "    newAppId: %d\n", app_list_data->newAppId);
	rdp_debug(b, "    deleteAppId: %d\n", app_list_data->deleteAppId);
	rdp_debug(b, "    deleteAppProvider: %d\n", app_list_data->deleteAppProvider);
	rdp_debug(b, "    appId: %s\n", app_list_data->appId);
	rdp_debug(b, "    appGroup: %s\n", app_list_data->appGroup);
	rdp_debug(b, "    appExecPath: %s\n", app_list_data->appExecPath);
	rdp_debug(b, "    appWorkingDir: %s\n", app_list_data->appWorkingDir);
	rdp_debug(b, "    appDesc: %s\n", app_list_data->appDesc);
	rdp_debug(b, "    appIcon: %p\n", app_list_data->appIcon);
	rdp_debug(b, "    appProvider: %s\n", app_list_data->appProvider);

	if (app_list_data->deleteAppId) {
		RDPAPPLIST_DELETE_APPLIST_PDU deleteAppList = {};
		assert(app_list_data->appProvider == NULL); // provider must be NULL.
		deleteAppList.flags = RDPAPPLIST_FIELD_ID;
		if (app_list_data->appId == NULL || // appId is required.
			!utf8_string_to_rail_string(app_list_data->appId, &deleteAppList.appId))
			goto Exit_deletePath;

		if (app_list_data->appGroup && // appGroup is optional.
			utf8_string_to_rail_string(app_list_data->appGroup, &deleteAppList.appGroup)) {
			deleteAppList.flags |= RDPAPPLIST_FIELD_GROUP;
		}
		peerCtx->applist_server_context->DeleteApplicationList(peerCtx->applist_server_context, &deleteAppList);
	Exit_deletePath:
		if (deleteAppList.appId.string)
			free(deleteAppList.appId.string);
		if (deleteAppList.appGroup.string)
			free(deleteAppList.appGroup.string);
	} else if (app_list_data->deleteAppProvider) {
		RDPAPPLIST_DELETE_APPLIST_PROVIDER_PDU deleteAppListProvider = {};
		deleteAppListProvider.flags = RDPAPPLIST_FIELD_PROVIDER;
		if (app_list_data->appProvider && // appProvider is required.
			utf8_string_to_rail_string(app_list_data->appProvider, &deleteAppListProvider.appListProviderName))
			peerCtx->applist_server_context->DeleteApplicationListProvider(peerCtx->applist_server_context, &deleteAppListProvider);
		if (deleteAppListProvider.appListProviderName.string)
			free(deleteAppListProvider.appListProviderName.string);
	} else {
		RDPAPPLIST_UPDATE_APPLIST_PDU updateAppList = {};
		RDPAPPLIST_ICON_DATA iconData = {};
		assert(app_list_data->appProvider == NULL); // group must be NULL.
		updateAppList.flags = app_list_data->newAppId ? RDPAPPLIST_HINT_NEWID : 0;
		if (app_list_data->inSync)
			updateAppList.flags |= RDPAPPLIST_HINT_SYNC;
		if (app_list_data->syncStart) {
			assert(app_list_data->inSync);
			updateAppList.flags |= RDPAPPLIST_HINT_SYNC_START;
		}
		if (app_list_data->syncEnd) {
			assert(app_list_data->inSync);
			updateAppList.flags |= RDPAPPLIST_HINT_SYNC_END;
		}
		updateAppList.flags |= (RDPAPPLIST_FIELD_ID |
					RDPAPPLIST_FIELD_EXECPATH |
					RDPAPPLIST_FIELD_DESC);
		if (app_list_data->appId == NULL || // id is required.
			!utf8_string_to_rail_string(app_list_data->appId, &updateAppList.appId))
			goto Exit_updatePath;
		if (app_list_data->appExecPath == NULL || // exePath is required.
			!utf8_string_to_rail_string(app_list_data->appExecPath, &updateAppList.appExecPath))
			goto Exit_updatePath;
		if (app_list_data->appDesc == NULL || // desc is required.
			!utf8_string_to_rail_string(app_list_data->appDesc, &updateAppList.appDesc))
			goto Exit_updatePath;

		if (app_list_data->appGroup && // group is optional.
			utf8_string_to_rail_string(app_list_data->appGroup, &updateAppList.appGroup)) {
			updateAppList.flags |= RDPAPPLIST_FIELD_GROUP;
		}
		if (app_list_data->appWorkingDir && // workingDir is optional.
			utf8_string_to_rail_string(app_list_data->appWorkingDir, &updateAppList.appWorkingDir)) {
			updateAppList.flags |= RDPAPPLIST_FIELD_WORKINGDIR;
		}
		if (app_list_data->appIcon) { // icon is optional.
			iconData.flags = 0;
			iconData.iconWidth = pixman_image_get_width(app_list_data->appIcon);
			iconData.iconHeight = pixman_image_get_height(app_list_data->appIcon);
			iconData.iconStride = pixman_image_get_stride(app_list_data->appIcon);
			iconData.iconBpp = 32;
			if (pixman_image_get_format(app_list_data->appIcon) != PIXMAN_a8r8g8b8)
				goto Exit_updatePath;
			iconData.iconFormat = RDPAPPLIST_ICON_FORMAT_BMP;
			iconData.iconBitsLength = iconData.iconHeight * iconData.iconStride;
			iconData.iconBits = malloc(iconData.iconBitsLength);
			if (!iconData.iconBits)
				goto Exit_updatePath;
			char *src = (char *)pixman_image_get_data(app_list_data->appIcon);
			char *dst = (char *)iconData.iconBits + (iconData.iconHeight-1) * iconData.iconStride;
			for (UINT32 i = 0; i < iconData.iconHeight; i++) {
				memcpy(dst, src, iconData.iconStride);
				src += iconData.iconStride;
				dst -= iconData.iconStride;
			}
			updateAppList.appIcon = &iconData;
			updateAppList.flags |= RDPAPPLIST_FIELD_ICON;
		}
		peerCtx->applist_server_context->UpdateApplicationList(peerCtx->applist_server_context, &updateAppList);
	Exit_updatePath:
		if (iconData.iconBits)
			free(iconData.iconBits);
		if (updateAppList.appId.string)
			free(updateAppList.appId.string);
		if (updateAppList.appGroup.string)
			free(updateAppList.appGroup.string);
		if (updateAppList.appExecPath.string)
			free(updateAppList.appExecPath.string);
		if (updateAppList.appWorkingDir.string)
			free(updateAppList.appWorkingDir.string);
		if (updateAppList.appDesc.string)
			free(updateAppList.appDesc.string);
	}
	return true;
}
#endif // HAVE_FREERDP_RDPAPPLIST_H

static struct weston_output *
rdp_rail_get_primary_output(void *rdp_backend)
{
	struct rdp_backend *b = (struct rdp_backend*)rdp_backend;
	struct rdp_head *current;
	wl_list_for_each(current, &b->head_list, link) {
		if (current->monitorMode.monitorDef.is_primary)
			return current->base.output;
	}
	return NULL;
}

struct weston_rdprail_api rdprail_api = {
	.shell_initialize_notify = rdp_rail_shell_initialize_notify,
	.start_window_move = rdp_rail_start_window_move,
	.end_window_move = rdp_rail_end_window_move,
	.set_window_icon = rdp_rail_set_window_icon,
#ifdef HAVE_FREERDP_RDPAPPLIST_H
	.notify_app_list = rdp_rail_notify_app_list,
#else
	.notify_app_list = NULL,
#endif // HAVE_FREERDP_RDPAPPLIST_H
	.get_primary_output = rdp_rail_get_primary_output,
	.notify_window_zorder_change = rdp_rail_notify_window_zorder_change,
};

int
rdp_rail_backend_create(struct rdp_backend *b)
{
	char *s;
	int ret = weston_plugin_api_register(b->compositor, WESTON_RDPRAIL_API_NAME,
					&rdprail_api, sizeof(rdprail_api));
	if (ret < 0) {
		rdp_debug_error(b, "Failed to register rdprail API.\n");
		return -1;
	}

#ifdef HAVE_FREERDP_RDPAPPLIST_H
	bool use_rdpapplist = true;

	s = getenv("WESTON_RDP_DISABLE_APPLIST");
	if (s) {
		rdp_debug(b, "WESTON_RDP_DISABLE_APPLIST is set to %s.\n", s);
		if (strcmp(s, "true") == 0)
			use_rdpapplist = false;
	}

	if (use_rdpapplist) {
		use_rdpapplist = false;

		rdp_debug(b, "RDPAPPLIST_MODULEDIR is set to %s\n", RDPAPPLIST_MODULEDIR);

		dlerror(); /* clear error */
		b->libRDPApplistServer = dlopen(RDPAPPLIST_MODULEDIR "/" "librdpapplist-server.so", RTLD_NOW);
		if (!b->libRDPApplistServer) {
			rdp_debug_error(b, "dlopen(%s/librdpapplist-server.so) failed with %s\n", RDPAPPLIST_MODULEDIR, dlerror());
			b->libRDPApplistServer = dlopen("librdpapplist-server.so", RTLD_NOW);
			if (!b->libRDPApplistServer) {
				rdp_debug_error(b, "dlopen(librdpapplist-server.so) failed with %s\n", dlerror());
			}
		}

		if (b->libRDPApplistServer) {
			*(void **)(&b->rdpapplist_server_context_new) = dlsym(b->libRDPApplistServer, "rdpapplist_server_context_new");
			*(void **)(&b->rdpapplist_server_context_free) = dlsym(b->libRDPApplistServer, "rdpapplist_server_context_free");
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
#endif // HAVE_FREERDP_RDPAPPLIST_H

#ifdef HAVE_FREERDP_GFXREDIR_H
	bool use_gfxredir = true;

	s = getenv("WESTON_RDP_DISABLE_SHARED_MEMORY");
	if (s) {
		rdp_debug(b, "WESTON_RDP_DISABLE_SHARED_MEMORY is set to %s.\n", s);
		if (strcmp(s, "true") == 0)
			use_gfxredir = false;
	}

	/* check if shared memory mount path is set */
	if (use_gfxredir) {
		use_gfxredir = false;
		s = getenv("WSL2_SHARED_MEMORY_MOUNT_POINT");
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
			*(void **)(&b->gfxredir_server_context_new) = dlsym(b->libFreeRDPServer, "gfxredir_server_context_new");
			*(void **)(&b->gfxredir_server_context_free) = dlsym(b->libFreeRDPServer, "gfxredir_server_context_free");
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
		use_gfxredir = false;
		struct weston_rdp_shared_memory shmem = {};
		shmem.size = sysconf(_SC_PAGESIZE);
		if (rdp_allocate_shared_memory(b, &shmem)) {
			*(UINT32*)shmem.addr = 0x12344321;
			assert(*(UINT32*)shmem.addr == 0x12344321);
			rdp_free_shared_memory(b, &shmem);
			use_gfxredir = true;
		}
	}

	b->use_gfxredir = use_gfxredir;
	rdp_debug(b, "RDP backend: use_gfxredir = %d\n", b->use_gfxredir);
#endif // HAVE_FREERDP_GFXREDIR_H

	/*
	 * Configure HI-DPI scaling.
	 */
	b->enable_hi_dpi_support = true;
	s = getenv("WESTON_RDP_DISABLE_HI_DPI_SCALING");
	if (s) {
		if (strcmp(s, "true") == 0)
			b->enable_hi_dpi_support = false;
		else if (strcmp(s, "false") == 0)
			b->enable_hi_dpi_support = true;
	}
	rdp_debug(b, "RDP backend: enable_hi_dpi_support = %d\n", b->enable_hi_dpi_support);

	b->enable_fractional_hi_dpi_support = false;
	if (b->enable_hi_dpi_support) {
		/* Disable by default for now. b->enable_fractional_hi_dpi_support = true; */
		s = getenv("WESTON_RDP_DISABLE_FRACTIONAL_HI_DPI_SCALING");
		if (s) {
			if (strcmp(s, "true") == 0)
				b->enable_fractional_hi_dpi_support = false;
			else if (strcmp(s, "false") == 0)
				b->enable_fractional_hi_dpi_support = true;
		}
	}
	rdp_debug(b, "RDP backend: enable_fractional_hi_dpi_support = %d\n", b->enable_fractional_hi_dpi_support);

	b->debug_desktop_scaling_factor = 0;
	if (b->enable_hi_dpi_support) {
		char *debug_desktop_scaling_factor = getenv("WESTON_RDP_DEBUG_DESKTOP_SCALING_FACTOR");
		if (debug_desktop_scaling_factor) {
			if (!safe_strtoint(debug_desktop_scaling_factor, &b->debug_desktop_scaling_factor) ||
			    (b->debug_desktop_scaling_factor < 100 || b->debug_desktop_scaling_factor > 500)) {
				b->debug_desktop_scaling_factor = 0;
				rdp_debug(b, "WESTON_RDP_DEBUG_DESKTOP_SCALING_FACTOR = %s is invalid and ignored.\n",
					  debug_desktop_scaling_factor);
			} else {
				rdp_debug(b, "WESTON_RDP_DEBUG_DESKTOP_SCALING_FACTOR = %d is set.\n",
					  b->debug_desktop_scaling_factor);
			}
		}
	}
	rdp_debug(b, "RDP backend: debug_desktop_scaling_factor = %d\n", b->debug_desktop_scaling_factor);

	b->enable_window_zorder_sync = true;
	s = getenv("WESTON_RDP_DISABLE_WINDOW_ZORDER_SYNC");
	if (s) {
		if (strcmp(s, "true") == 0)
			b->enable_window_zorder_sync = false;
	}
	rdp_debug(b, "RDP backend: enable_window_zorder_sync = %d\n", b->enable_window_zorder_sync);

	b->rdprail_shell_name = NULL;

	b->enable_distro_name_title = true;
	s = getenv("WESTON_RDP_DISABLE_APPEND_DISTRONAME_TITLE");
	if (s) {
		if (strcmp(s, "true") == 0)
			b->enable_distro_name_title = false;
	}
	rdp_debug(b, "RDP backend: enable_distro_name_title = %d\n", b->enable_distro_name_title);

	b->enable_copy_warning_title = false;
	if (b->debugLevel >= RDP_DEBUG_LEVEL_WARN &&
            !b->use_gfxredir) {
		b->enable_copy_warning_title = true;
		s = getenv("WESTON_RDP_DISABLE_COPY_WARNING_TITLE");
		if (s) {
			if (strcmp(s, "true") == 0)
				b->enable_copy_warning_title = false;
		}
	}
	rdp_debug(b, "RDP backend: enable_copy_warning_title = %d\n", b->enable_copy_warning_title);

	/* M to dump all outstanding monitor info */
	b->debug_binding_M = weston_compositor_add_debug_binding(b->compositor, KEY_M,
						rdp_rail_dump_monitor_binding, b);
	/* W to dump all outstanding window info */
	b->debug_binding_W = weston_compositor_add_debug_binding(b->compositor, KEY_W,
						rdp_rail_dump_window_binding, b);
	/* Trigger to enter debug key : CTRL+SHIFT+SPACE */
	weston_install_debug_key_binding(b->compositor, MODIFIER_CTRL);

	/* start listening surface creation */
	b->create_window_listener.notify = rdp_rail_create_window;
	wl_signal_add(&b->compositor->create_surface_signal, &b->create_window_listener);

	return 0;
}

void
rdp_rail_destroy(struct rdp_backend *b)
{
	if (b->create_window_listener.notify) {
		wl_list_remove(&b->create_window_listener.link);
		b->create_window_listener.notify = NULL;
	}

	if (b->rdprail_shell_name)
		free(b->rdprail_shell_name);

	if (b->debug_binding_M)
		weston_binding_destroy(b->debug_binding_M);

	if (b->debug_binding_W)
		weston_binding_destroy(b->debug_binding_W);

#if defined(HAVE_FREERDP_RDPAPPLIST_H)
	if (b->libRDPApplistServer)
		dlclose(b->libRDPApplistServer);
#endif // defined(HAVE_FREERDP_RDPAPPLIST_H)

#if defined(HAVE_FREERDP_GFXREDIR_H)
	if (b->libFreeRDPServer)
		dlclose(b->libFreeRDPServer);
#endif // defined(HAVE_FREERDP_GFXREDIR_H)
}
