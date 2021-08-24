/*
 * Copyright © 2013 Hardening <rdp.effort@gmail.com>
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
#include <linux/input.h>

#include <unistd.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <netdb.h>
#include <linux/vm_sockets.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include "rdp.h"

#include <winpr/input.h>

#if FREERDP_VERSION_MAJOR >= 2
#include <winpr/ssl.h>
#endif

#include "shared/timespec-util.h"
#include <libweston/libweston.h>
#include <libweston/backend-rdp.h>
#include "pixman-renderer.h"

#if HAVE_OPENSSL
/* for session certificate generation */
#include <openssl/crypto.h>
#include <openssl/conf.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>
#include <openssl/rand.h>
#endif

extern PWtsApiFunctionTable FreeRDP_InitWtsApi(void);

static void
rdp_peer_seat_led_update(struct weston_seat *seat_base, enum weston_led leds)
{
	/*TODO: if Caps/Num lock change is triggered by server side, here can forward to client */
}

static void
rdp_peer_refresh_rfx(pixman_region32_t *damage, pixman_image_t *image, freerdp_peer *peer)
{
	int width, height, nrects, i;
	pixman_box32_t *region, *rects;
	uint32_t *ptr;
	RFX_RECT *rfxRect;
	rdpUpdate *update = peer->update;
	SURFACE_BITS_COMMAND cmd;
	RdpPeerContext *context = (RdpPeerContext *)peer->context;

	Stream_Clear(context->encode_stream);
	Stream_SetPosition(context->encode_stream, 0);

	width = (damage->extents.x2 - damage->extents.x1);
	height = (damage->extents.y2 - damage->extents.y1);

#ifdef HAVE_SKIP_COMPRESSION
	cmd.skipCompression = TRUE;
#else
	memset(&cmd, 0, sizeof(*cmd));
#endif
#ifdef HAVE_SURFCMD_CMDTYPE
	cmd.cmdType = CMDTYPE_STREAM_SURFACE_BITS;
#endif
	cmd.destLeft = damage->extents.x1;
	cmd.destTop = damage->extents.y1;
	cmd.destRight = damage->extents.x2;
	cmd.destBottom = damage->extents.y2;
	SURFACE_BPP(cmd) = 32;
	SURFACE_CODECID(cmd) = peer->settings->RemoteFxCodecId;
	SURFACE_WIDTH(cmd) = width;
	SURFACE_HEIGHT(cmd) = height;

	ptr = pixman_image_get_data(image) + damage->extents.x1 +
				damage->extents.y1 * (pixman_image_get_stride(image) / sizeof(uint32_t));

	rects = pixman_region32_rectangles(damage, &nrects);
	context->rfx_rects = realloc(context->rfx_rects, nrects * sizeof *rfxRect);

	for (i = 0; i < nrects; i++) {
		region = &rects[i];
		rfxRect = &context->rfx_rects[i];

		rfxRect->x = (region->x1 - damage->extents.x1);
		rfxRect->y = (region->y1 - damage->extents.y1);
		rfxRect->width = (region->x2 - region->x1);
		rfxRect->height = (region->y2 - region->y1);
	}

	rfx_compose_message(context->rfx_context, context->encode_stream, context->rfx_rects, nrects,
			(BYTE *)ptr, width, height,
			pixman_image_get_stride(image)
	);

	SURFACE_BITMAP_DATA_LEN(cmd) = Stream_GetPosition(context->encode_stream);
	SURFACE_BITMAP_DATA(cmd) = Stream_Buffer(context->encode_stream);

	update->SurfaceBits(update->context, &cmd);
}


static void
rdp_peer_refresh_nsc(pixman_region32_t *damage, pixman_image_t *image, freerdp_peer *peer)
{
	int width, height;
	uint32_t *ptr;
	rdpUpdate *update = peer->update;
	SURFACE_BITS_COMMAND cmd;
	RdpPeerContext *context = (RdpPeerContext *)peer->context;

	Stream_Clear(context->encode_stream);
	Stream_SetPosition(context->encode_stream, 0);

	width = (damage->extents.x2 - damage->extents.x1);
	height = (damage->extents.y2 - damage->extents.y1);

#ifdef HAVE_SKIP_COMPRESSION
	cmd.skipCompression = TRUE;
#else
	memset(cmd, 0, sizeof(*cmd));
#endif
#ifdef HAVE_SURFCMD_CMDTYPE
	cmd.cmdType = CMDTYPE_SET_SURFACE_BITS;
#endif
	cmd.destLeft = damage->extents.x1;
	cmd.destTop = damage->extents.y1;
	cmd.destRight = damage->extents.x2;
	cmd.destBottom = damage->extents.y2;
	SURFACE_BPP(cmd) = 32;
	SURFACE_CODECID(cmd) = peer->settings->NSCodecId;
	SURFACE_WIDTH(cmd) = width;
	SURFACE_HEIGHT(cmd) = height;

	ptr = pixman_image_get_data(image) + damage->extents.x1 +
				damage->extents.y1 * (pixman_image_get_stride(image) / sizeof(uint32_t));

	nsc_compose_message(context->nsc_context, context->encode_stream, (BYTE *)ptr,
			width, height,
			pixman_image_get_stride(image));

	SURFACE_BITMAP_DATA_LEN(cmd) = Stream_GetPosition(context->encode_stream);
	SURFACE_BITMAP_DATA(cmd) = Stream_Buffer(context->encode_stream);

	update->SurfaceBits(update->context, &cmd);
}

static void
pixman_image_flipped_subrect(const pixman_box32_t *rect, pixman_image_t *img, BYTE *dest)
{
	int stride = pixman_image_get_stride(img);
	int h;
	int toCopy = (rect->x2 - rect->x1) * 4;
	int height = (rect->y2 - rect->y1);
	const BYTE *src = (const BYTE *)pixman_image_get_data(img);
	src += ((rect->y2-1) * stride) + (rect->x1 * 4);

	for (h = 0; h < height; h++, src -= stride, dest += toCopy)
		   memcpy(dest, src, toCopy);
}

static void
rdp_peer_refresh_raw(pixman_region32_t *region, pixman_image_t *image, freerdp_peer *peer)
{
	rdpUpdate *update = peer->update;
	SURFACE_BITS_COMMAND cmd;
	SURFACE_FRAME_MARKER marker;
	pixman_box32_t *rect, subrect;
	int nrects, i;
	int heightIncrement, remainingHeight, top;

	rect = pixman_region32_rectangles(region, &nrects);
	if (!nrects)
		return;

	marker.frameId++;
	marker.frameAction = SURFACECMD_FRAMEACTION_BEGIN;
	update->SurfaceFrameMarker(peer->context, &marker);

	memset(&cmd, 0, sizeof(cmd));
#ifdef HAVE_SURFCMD_CMDTYPE
	cmd.cmdType = CMDTYPE_SET_SURFACE_BITS;
#endif
	SURFACE_BPP(cmd) = 32;
	SURFACE_CODECID(cmd) = 0;

	for (i = 0; i < nrects; i++, rect++) {
		/*weston_log("rect(%d,%d, %d,%d)\n", rect->x1, rect->y1, rect->x2, rect->y2);*/
		cmd.destLeft = rect->x1;
		cmd.destRight = rect->x2;
		SURFACE_WIDTH(cmd) = rect->x2 - rect->x1;

		heightIncrement = peer->settings->MultifragMaxRequestSize / (16 + SURFACE_WIDTH(cmd) * 4);
		remainingHeight = rect->y2 - rect->y1;
		top = rect->y1;

		subrect.x1 = rect->x1;
		subrect.x2 = rect->x2;

		while (remainingHeight) {
			   SURFACE_HEIGHT(cmd) = (remainingHeight > heightIncrement) ? heightIncrement : remainingHeight;
			   cmd.destTop = top;
			   cmd.destBottom = top + SURFACE_HEIGHT(cmd);
			   SURFACE_BITMAP_DATA_LEN(cmd) = SURFACE_WIDTH(cmd) * SURFACE_HEIGHT(cmd) * 4;
			   SURFACE_BITMAP_DATA(cmd) = (BYTE *)realloc(SURFACE_BITMAP_DATA(cmd), SURFACE_BITMAP_DATA_LEN(cmd));

			   subrect.y1 = top;
			   subrect.y2 = top + SURFACE_HEIGHT(cmd);
			   pixman_image_flipped_subrect(&subrect, image, SURFACE_BITMAP_DATA(cmd));

			   /*weston_log("*  sending (%d,%d, %d,%d)\n", subrect.x1, subrect.y1, subrect.x2, subrect.y2); */
			   update->SurfaceBits(peer->context, &cmd);

			   remainingHeight -= SURFACE_HEIGHT(cmd);
			   top += SURFACE_HEIGHT(cmd);
		}
	}

	free(SURFACE_BITMAP_DATA(cmd));

	marker.frameAction = SURFACECMD_FRAMEACTION_END;
	update->SurfaceFrameMarker(peer->context, &marker);
}

static void
rdp_peer_refresh_region(pixman_region32_t *region, freerdp_peer *peer)
{
	RdpPeerContext *context = (RdpPeerContext *)peer->context;
	struct rdp_output *output = context->rdpBackend->output_default;
	rdpSettings *settings = peer->settings;

	if (settings->RemoteFxCodec)
		rdp_peer_refresh_rfx(region, output->shadow_surface, peer);
	else if (settings->NSCodec)
		rdp_peer_refresh_nsc(region, output->shadow_surface, peer);
	else
		rdp_peer_refresh_raw(region, output->shadow_surface, peer);
}

static int
rdp_output_start_repaint_loop(struct weston_output *output)
{
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->compositor, &ts);
	weston_output_finish_frame(output, &ts, WP_PRESENTATION_FEEDBACK_INVALID);

	return 0;
}

static int
rdp_output_repaint(struct weston_output *output_base, pixman_region32_t *damage,
		   void *repaint_data)
{
	struct rdp_output *output = container_of(output_base, struct rdp_output, base);
	struct weston_compositor *ec = output->base.compositor;
	struct rdp_peers_item *outputPeer;
	struct rdp_backend *b = to_rdp_backend(ec);

	/* Calculate the time we should complete this frame such that frames
	   are spaced out by the specified monitor refresh. */
	struct timespec now;
	weston_compositor_read_presentation_clock(ec, &now);

	struct timespec target;
	int refresh_nsec = millihz_to_nsec(output_base->current_mode->refresh);
	int refresh_msec = refresh_nsec / 1000000;
	timespec_add_nsec(&target, &output_base->frame_time, refresh_nsec);

	int next_frame_delta = (int)timespec_sub_to_msec(&target, &now);
	if ( next_frame_delta < 1 || next_frame_delta > refresh_msec) {
		next_frame_delta = refresh_msec;
	}

	if (b->rdp_peer &&
		b->rdp_peer->settings->HiDefRemoteApp) {
		/* RAIL mode, repaint RAIL window */
		rdp_rail_output_repaint(output_base, damage);
	} else if (output_base->renderer_state) {
		/* Add above 'output_base->renderer_state' check since this turns NULL when RDP
		   connection is disconnected and hit fault at pixman_renderer_output_set_buffer() */
		pixman_renderer_output_set_buffer(output_base, output->shadow_surface);
		ec->renderer->repaint_output(&output->base, damage);
		if (pixman_region32_not_empty(damage)) {
			pixman_region32_t transformed_damage;
			pixman_region32_init(&transformed_damage);
			weston_transformed_region(output_base->width,
						  output_base->height,
						  output_base->transform,
						  output_base->current_scale,
						  damage, &transformed_damage);
			/* note: if this code really need to walk peers in HiDef mode,     */
			/*       it must walk from output_default in backend, in non-HiDef */
			/*       there must be only one default output, so doesn't matter. */
			wl_list_for_each(outputPeer, &output->peers, link) {
				if ((outputPeer->flags & RDP_PEER_ACTIVATED) &&
					(outputPeer->flags & RDP_PEER_OUTPUT_ENABLED))
					rdp_peer_refresh_region(&transformed_damage, outputPeer->peer);
			}
			pixman_region32_fini(&transformed_damage);
		}

		pixman_region32_subtract(&ec->primary_plane.damage,
					&ec->primary_plane.damage, damage);
	}

	wl_event_source_timer_update(output->finish_frame_timer, next_frame_delta);
	return 0;
}

static int
finish_frame_handler(void *data)
{
	struct rdp_output *output = data;
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->base.compositor, &ts);
	weston_output_finish_frame(&output->base, &ts, 0);

	return 1;
}

static struct weston_mode *
rdp_insert_new_mode(struct weston_output *output, int width, int height, int rate)
{
	struct weston_mode *ret;
	ret = zalloc(sizeof *ret);
	if (!ret)
		return NULL;
	ret->width = width;
	ret->height = height;
	ret->refresh = rate;
	wl_list_insert(&output->mode_list, &ret->link);
	return ret;
}

static struct weston_mode *
ensure_matching_mode(struct weston_output *output, struct weston_mode *target)
{
	struct rdp_backend *b = to_rdp_backend(output->compositor);
	struct weston_mode *local;

	wl_list_for_each(local, &output->mode_list, link) {
		if ((local->width == target->width) && (local->height == target->height))
			return local;
	}

	return rdp_insert_new_mode(output, target->width, target->height, b->rdp_monitor_refresh_rate);
}

static int
rdp_switch_mode(struct weston_output *output, struct weston_mode *target_mode)
{
	struct rdp_output *rdpOutput = container_of(output, struct rdp_output, base);
	struct rdp_backend *rdpBackend = to_rdp_backend(output->compositor);
	struct rdp_peers_item *rdpPeer;
	rdpSettings *settings;
	pixman_image_t *new_shadow_buffer;
	struct weston_mode *local_mode, *previous_mode;
	const struct pixman_renderer_output_options options = { };
	bool HiDefRemoteApp = false;

	if (rdpBackend->rdp_peer && rdpBackend->rdp_peer->settings->HiDefRemoteApp)
		HiDefRemoteApp = true;

	local_mode = ensure_matching_mode(output, target_mode);
	if (!local_mode) {
		rdp_debug_error(rdpBackend, "mode %dx%d not available\n", target_mode->width, target_mode->height);
		return -ENOENT;
	}

	if (local_mode == output->current_mode)
		return 0;

	if (HiDefRemoteApp)
		previous_mode = output->current_mode;
	else
		output->current_mode->flags &= ~WL_OUTPUT_MODE_CURRENT;

	output->current_mode = local_mode;
	output->current_mode->flags |= WL_OUTPUT_MODE_CURRENT;

	if (HiDefRemoteApp) {
		/* Mark current mode as preferred mode */
		output->current_mode->flags |= WL_OUTPUT_MODE_PREFERRED;

		/* In HiDefRemoteApp mode, free previous current_mode,
		   since it only want to expose current mode to app */
		wl_list_remove(&previous_mode->link);
		free(previous_mode);
	}

	if (!HiDefRemoteApp) {
		pixman_renderer_output_destroy(output);
		pixman_renderer_output_create(output, &options);

		new_shadow_buffer = pixman_image_create_bits(PIXMAN_x8r8g8b8, target_mode->width,
				target_mode->height, 0, target_mode->width * 4);
		pixman_image_composite32(PIXMAN_OP_SRC, rdpOutput->shadow_surface, 0, new_shadow_buffer,
				0, 0, 0, 0, 0, 0, target_mode->width, target_mode->height);
		pixman_image_unref(rdpOutput->shadow_surface);
		rdpOutput->shadow_surface = new_shadow_buffer;

		wl_list_for_each(rdpPeer, &rdpBackend->output_default->peers, link) {
			settings = rdpPeer->peer->settings;
			if (settings->DesktopWidth == (UINT32)target_mode->width &&
					settings->DesktopHeight == (UINT32)target_mode->height)
				continue;

			if (!settings->DesktopResize) {
				/* too bad this peer does not support desktop resize */
				rdp_debug_error(rdpBackend, "%s: desktop resize is not allowed\n", __func__);
				rdpPeer->peer->Close(rdpPeer->peer);
			} else {
				settings->DesktopWidth = target_mode->width;
				settings->DesktopHeight = target_mode->height;
				rdpPeer->peer->update->DesktopResize(rdpPeer->peer->context);
			}
		}
	}
	return 0;
}

static int
rdp_output_get_config(struct weston_output *base,
			int *width, int *height, int *scale)
{
	struct rdp_output *output = to_rdp_output(base);
	struct rdp_backend *rdpBackend = to_rdp_backend(base->compositor);
	freerdp_peer *client = rdpBackend->rdp_peer;
	struct weston_head *head;

	wl_list_for_each(head, &output->base.head_list, output_link) {
		struct rdp_head *h = to_rdp_head(head);

		rdp_debug(rdpBackend, "get_config: attached head [%d]: make:%s, mode:%s, name:%s, (%p)\n",
			h->index, head->make, head->model, head->name, head);
		rdp_debug(rdpBackend, "get_config: attached head [%d]: x:%d, y:%d, width:%d, height:%d\n",
			h->index, h->monitorMode.monitorDef.x, h->monitorMode.monitorDef.y,
				  h->monitorMode.monitorDef.width, h->monitorMode.monitorDef.height);

		/* In HiDef RAIL mode, get monitor resolution from RDP client if provided. */
		if (client && client->settings->HiDefRemoteApp) {
			if (h->monitorMode.monitorDef.width && h->monitorMode.monitorDef.height) {
				/* Return true client resolution (not adjusted by DPI) */
				*width = h->monitorMode.monitorDef.width;
				*height = h->monitorMode.monitorDef.height;
				*scale = h->monitorMode.scale;
			}
			break; // only one head per output in HiDef.
		}
	}
	return 0;
}

static int
rdp_output_set_size(struct weston_output *base,
			int width, int height)
{
	struct rdp_output *output = to_rdp_output(base);
	struct rdp_backend *rdpBackend = to_rdp_backend(base->compositor);
	freerdp_peer *client = rdpBackend->rdp_peer;
	struct weston_head *head;
	struct weston_mode *currentMode;
	struct weston_mode initMode;
	BOOL is_preferred_mode = false;

	/* We can only be called once. */
	assert(!output->base.current_mode);

	wl_list_for_each(head, &output->base.head_list, output_link) {
		struct rdp_head *h = to_rdp_head(head);

		weston_head_set_monitor_strings(head, "weston", "rdp", NULL);

		rdp_debug(rdpBackend, "set_size: attached head [%d]: make:%s, mode:%s, name:%s, (%p)\n",
			h->index, head->make, head->model, head->name, head);
		rdp_debug(rdpBackend, "set_size: attached head [%d]: x:%d, y:%d, width:%d, height:%d\n",
			h->index, h->monitorMode.monitorDef.x, h->monitorMode.monitorDef.y,
				  h->monitorMode.monitorDef.width, h->monitorMode.monitorDef.height);

		/* This is a virtual output, so report a zero physical size.
		 * It's better to let frontends/clients use their defaults. */
		/* If MonitorDef has it, use it from MonitorDef */
		weston_head_set_physical_size(head,
			h->monitorMode.monitorDef.attributes.physicalWidth,
			h->monitorMode.monitorDef.attributes.physicalHeight);

		/* In HiDef RAIL mode, set this mode as preferred mode */
		if (client && client->settings->HiDefRemoteApp) {
			if (h->monitorMode.monitorDef.width && h->monitorMode.monitorDef.height) {
				/* given width/height must match with monitor's if provided */
				assert(width == h->monitorMode.monitorDef.width);
				assert(height == h->monitorMode.monitorDef.height);
				is_preferred_mode = true;
			}
			break; // only one head per output in HiDef.
		}
	}

	wl_list_init(&output->peers);

	initMode.flags = WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	initMode.width = width;
	initMode.height = height;
	initMode.refresh = rdpBackend->rdp_monitor_refresh_rate;
	currentMode = ensure_matching_mode(&output->base, &initMode);
	if (!currentMode)
		return -1;

	currentMode->flags |= WL_OUTPUT_MODE_CURRENT;
	if (is_preferred_mode)
		currentMode->flags |= WL_OUTPUT_MODE_PREFERRED;

	output->base.current_mode = currentMode;
	output->base.native_mode = currentMode;
	output->base.native_scale = base->scale;

	output->base.start_repaint_loop = rdp_output_start_repaint_loop;
	output->base.repaint = rdp_output_repaint;
	output->base.assign_planes = NULL;
	output->base.set_backlight = NULL;
	output->base.set_dpms = NULL;
	output->base.switch_mode = rdp_switch_mode;

	return 0;
}

static int
rdp_output_enable(struct weston_output *base)
{
	struct rdp_output *output = to_rdp_output(base);
	struct rdp_backend *b = to_rdp_backend(base->compositor);
	struct wl_event_loop *loop;
	const struct pixman_renderer_output_options options = {
		.use_shadow = true,
	};
	bool HiDefRemoteApp = false;

	if (b->rdp_peer && b->rdp_peer->settings->HiDefRemoteApp)
		HiDefRemoteApp = true;

	if (HiDefRemoteApp) {
		struct weston_head *eh;
		wl_list_for_each(eh, &output->base.head_list, output_link) {
			struct rdp_head *h = to_rdp_head(eh);
			rdp_debug(b, "move head/output %s (%d,%d) -> (%d,%d)\n",
				output->base.name, output->base.x, output->base.y,
				h->monitorMode.rectWeston.x,
				h->monitorMode.rectWeston.y);
			weston_output_move(&output->base,
				h->monitorMode.rectWeston.x,
				h->monitorMode.rectWeston.y);
			break; // must be only 1 head per output.
		}
	} else {
		output->shadow_surface = pixman_image_create_bits(PIXMAN_x8r8g8b8,
								  output->base.current_mode->width,
								  output->base.current_mode->height,
								  NULL,
								  output->base.current_mode->width * 4);
		if (output->shadow_surface == NULL) {
			rdp_debug_error(b, "Failed to create surface for frame buffer.\n");
			return -1;
		}

		if (pixman_renderer_output_create(&output->base, &options) < 0) {
			pixman_image_unref(output->shadow_surface);
			output->shadow_surface = NULL;
			return -1;
		}
	}

	loop = wl_display_get_event_loop(b->compositor->wl_display);
	output->finish_frame_timer = wl_event_loop_add_timer(loop, finish_frame_handler, output);

	return 0;
}

static int
rdp_output_disable(struct weston_output *base)
{
	struct rdp_output *output = to_rdp_output(base);

	if (!output->base.enabled)
		return 0;

	if (output->shadow_surface) {
		pixman_image_unref(output->shadow_surface);
		pixman_renderer_output_destroy(&output->base);
		output->shadow_surface = NULL;
	}

	wl_event_source_remove(output->finish_frame_timer);

	return 0;
}

static void
rdp_output_destroy(struct weston_output *base)
{
	struct rdp_output *output = to_rdp_output(base);

	rdp_output_disable(&output->base);
	weston_output_release(&output->base);
	wl_list_remove(&output->link);

	free(output);
}

static int
rdp_output_attach_head(struct weston_output *output_base,
		       struct weston_head *head_base)
{
	struct rdp_backend *b = to_rdp_backend(output_base->compositor);
	struct rdp_output *o = to_rdp_output(output_base);
	struct rdp_head *h = to_rdp_head(head_base);
	rdp_debug(b, "Head attaching: %s, index:%d, is_primary: %d\n",
		head_base->name, h->index, h->monitorMode.monitorDef.is_primary);
	if (!wl_list_empty(&output_base->head_list)) {
		rdp_debug_error(b, "attaching more than 1 head to single output (= clone) is not supported\n");
		return -1;
	}
	o->index = h->index;
	if (h->monitorMode.monitorDef.is_primary) {
		assert(b->output_default == NULL);
		b->output_default = o;
	}
	return 0;
}

static void
rdp_output_detach_head(struct weston_output *output_base,
		       struct weston_head *head_base)
{
	struct rdp_backend *b = to_rdp_backend(output_base->compositor);
	struct rdp_head *h = to_rdp_head(head_base);
	rdp_debug(b, "Head detaching: %s, index:%d, is_primary: %d\n",
		head_base->name, h->index, h->monitorMode.monitorDef.is_primary);
	if (h->monitorMode.monitorDef.is_primary) {
		assert(b->output_default == to_rdp_output(output_base));
		b->output_default = NULL;
	}
}

static struct weston_output *
rdp_output_create(struct weston_compositor *compositor, const char *name)
{
	struct rdp_backend *backend = to_rdp_backend(compositor);
	struct rdp_output *output;

	output = zalloc(sizeof *output);
	if (output == NULL)
		return NULL;

	wl_list_insert(&backend->output_list, &output->link);

	weston_output_init(&output->base, compositor, name);

	output->base.destroy = rdp_output_destroy;
	output->base.disable = rdp_output_disable;
	output->base.enable = rdp_output_enable;
	output->base.attach_head = rdp_output_attach_head;
	output->base.detach_head = rdp_output_detach_head;

	weston_compositor_add_pending_output(&output->base, compositor);

	return &output->base;
}

struct rdp_head *
rdp_head_create(struct weston_compositor *compositor, BOOL isPrimary, struct rdp_monitor_mode *monitorMode)
{
	struct rdp_backend *b = to_rdp_backend(compositor);
	struct rdp_head *head;
	char name[13] = {}; // 'rdp-' + 8 chars for hex uint32_t + NULL.

	head = zalloc(sizeof *head);
	if (!head)
		return NULL;

	head->index = b->head_index++;
	if (monitorMode) {
		head->monitorMode = *monitorMode;
		pixman_region32_init_rect(&head->regionClient,
			monitorMode->monitorDef.x, monitorMode->monitorDef.y,
			monitorMode->monitorDef.width, monitorMode->monitorDef.height);
		pixman_region32_init_rect(&head->regionWeston,
			monitorMode->rectWeston.x, monitorMode->rectWeston.y,
			monitorMode->rectWeston.width, monitorMode->rectWeston.height);
	} else {
		head->monitorMode.scale = 1.0f;
		head->monitorMode.clientScale = 1;
		pixman_region32_init(&head->regionClient);
		pixman_region32_init(&head->regionWeston);
	}
	if (isPrimary) {
		rdp_debug(b, "Default head is being added\n");
		b->head_default = head;
	}
	head->monitorMode.monitorDef.is_primary = isPrimary;
	wl_list_insert(&b->head_list, &head->link);
	sprintf(name, "rdp-%x", head->index);

	weston_head_init(&head->base, name);
	weston_head_set_connection_status(&head->base, true);
	weston_compositor_add_head(compositor, &head->base);

	return head;
}

void
rdp_head_destroy(struct weston_compositor *compositor, struct rdp_head *head)
{
	struct rdp_backend *b = to_rdp_backend(compositor);
	weston_head_release(&head->base);
	wl_list_remove(&head->link);
	pixman_region32_fini(&head->regionWeston);
	pixman_region32_fini(&head->regionClient);
	if (b->head_default == head) {
		rdp_debug(b, "Default head is being removed\n");
		b->head_default = NULL;
	}
	free(head);
}

static void
rdp_destroy(struct weston_compositor *ec)
{
	struct rdp_backend *b = to_rdp_backend(ec);
	struct weston_head *base, *next;
	struct rdp_peers_item *rdp_peer, *tmp;
	int i;

	if (b->output_default) {
		wl_list_for_each_safe(rdp_peer, tmp, &b->output_default->peers, link) {
			freerdp_peer* client = rdp_peer->peer;

			client->Disconnect(client);
			freerdp_peer_context_free(client);
			freerdp_peer_free(client);
		}
	} else if (b->rdp_peer) {
		freerdp_peer* client = b->rdp_peer;
		assert(client->settings->HiDefRemoteApp);

		client->Disconnect(client);
		freerdp_peer_context_free(client);
		freerdp_peer_free(client);
	}

	for (i = 0; i < MAX_FREERDP_FDS; i++)
		if (b->listener_events[i])
			wl_event_source_remove(b->listener_events[i]);

	rdp_rail_destroy(b);

	if (b->debug) {
		weston_log_scope_destroy(b->debug);
		b->debug = NULL;
	}

	weston_compositor_shutdown(ec);

	wl_list_for_each_safe(base, next, &ec->head_list, compositor_link)
		rdp_head_destroy(ec, to_rdp_head(base));

	assert(wl_list_empty(&b->head_list));

	freerdp_listener_free(b->listener);

	free(b->server_cert);
	free(b->server_key);
	free(b->rdp_key);
	free(b);
}

static
int rdp_listener_activity(int fd, uint32_t mask, void *data)
{
	freerdp_listener* instance = (freerdp_listener *)data;

	if (!(mask & WL_EVENT_READABLE))
		return 0;
	if (!instance->CheckFileDescriptor(instance)) {
		weston_log("failed to check FreeRDP file descriptor\n");
		return -1;
	}
	return 0;
}

static
int rdp_implant_listener(struct rdp_backend *b, freerdp_listener* instance)
{
	int i, fd;
	int rcount = 0;
	void* rfds[MAX_FREERDP_FDS];
	struct wl_event_loop *loop;

	if (!instance->GetFileDescriptor(instance, rfds, &rcount)) {
		weston_log("Failed to get FreeRDP file descriptor\n");
		return -1;
	}

	loop = wl_display_get_event_loop(b->compositor->wl_display);
	for (i = 0; i < rcount; i++) {
		fd = (int)(long)(rfds[i]);
		b->listener_events[i] = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
				rdp_listener_activity, instance);
	}

	for ( ; i < MAX_FREERDP_FDS; i++)
		b->listener_events[i] = 0;
	return 0;
}


static FREERDP_CB_RET_TYPE
rdp_peer_context_new(freerdp_peer* client, RdpPeerContext* context)
{
	context->item.peer = client;
	context->item.flags = RDP_PEER_OUTPUT_ENABLED;

	context->loop_event_source_fd = -1;

#if FREERDP_VERSION_MAJOR == 1 && FREERDP_VERSION_MINOR == 1
	context->rfx_context = rfx_context_new();
#else
	context->rfx_context = rfx_context_new(TRUE);
#endif
	if (!context->rfx_context) {
		FREERDP_CB_RETURN(FALSE);
	}

	context->rfx_context->mode = RLGR3;
	context->rfx_context->width = client->settings->DesktopWidth;
	context->rfx_context->height = client->settings->DesktopHeight;
	rfx_context_set_pixel_format(context->rfx_context, DEFAULT_PIXEL_FORMAT);

	context->nsc_context = nsc_context_new();
	if (!context->nsc_context)
		goto out_error_nsc;

#ifdef HAVE_NSC_CONTEXT_SET_PARAMETERS
	nsc_context_set_parameters(context->nsc_context, NSC_COLOR_FORMAT, DEFAULT_PIXEL_FORMAT);
#else
	nsc_context_set_pixel_format(context->nsc_context, DEFAULT_PIXEL_FORMAT);
#endif
	context->encode_stream = Stream_New(NULL, 65536);
	if (!context->encode_stream)
		goto out_error_stream;

	FREERDP_CB_RETURN(TRUE);

out_error_nsc:
	rfx_context_free(context->rfx_context);
out_error_stream:
	nsc_context_free(context->nsc_context);
	FREERDP_CB_RETURN(FALSE);
}

static void
rdp_peer_context_free(freerdp_peer* client, RdpPeerContext* context)
{
	unsigned i;
	if (!context)
		return;

	wl_list_remove(&context->item.link);

	if (context->loop_event_source_fd != -1)
		close(context->loop_event_source_fd);

	for (i = 0; i < ARRAY_LENGTH(context->events); i++) {
		if (context->events[i])
			wl_event_source_remove(context->events[i]);
	}

	rdp_audioin_destroy(context);

	rdp_audio_destroy(context);

	rdp_clipboard_destroy(context);

	rdp_rail_peer_context_free(client, context);

	rdp_drdynvc_destroy(context);

	if (context->vcm)
		WTSCloseServer(context->vcm);

	/* clear the peer, in RAIL mode, this allows new peer to connect */
	if (context->rdpBackend->rdp_peer == client)
		context->rdpBackend->rdp_peer = NULL;

	if (context->item.flags & RDP_PEER_ACTIVATED) {
		weston_seat_release_keyboard(context->item.seat);
		weston_seat_release_pointer(context->item.seat);
		if (!client->settings->RemoteApplicationMode) {
			/* XXX we should weston_seat_release(context->item.seat); here
			 * but it would crash on reconnect */
		} else {
			/* Without understanding full details of above comments, but
			   in RAIL mode, only one peer per backend can be activated,
			   and no "deactivate_all" PDU to be used (since no client
			   side desktop resize is allowed), so safe to free seat here
			   to prevent possible memory leak. */
			weston_seat_release(context->item.seat);
			context->item.seat = NULL;
			context->item.flags &= ~RDP_PEER_ACTIVATED;
		}
	}

	Stream_Free(context->encode_stream, TRUE);
	nsc_context_free(context->nsc_context);
	rfx_context_free(context->rfx_context);
	free(context->rfx_rects);
}

static int
rdp_client_activity(int fd, uint32_t mask, void *data)
{
	freerdp_peer* client = (freerdp_peer *)data;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *rdpBackend = peerCtx->rdpBackend;

	if (!client->CheckFileDescriptor(client)) {
		rdp_debug_error(rdpBackend, "unable to checkDescriptor for %p\n", client);
		goto out_clean;
	}

	if (peerCtx && peerCtx->vcm)
	{
		if (!WTSVirtualChannelManagerCheckFileDescriptor(peerCtx->vcm)) {
			rdp_debug_error(rdpBackend, "failed to check FreeRDP WTS VC file descriptor for %p\n", client);
			goto out_clean;
        	}
	}

	return 0;

out_clean:
	freerdp_peer_context_free(client);
	freerdp_peer_free(client);
	return 0;
}

static BOOL
xf_peer_capabilities(freerdp_peer* client)
{
	return TRUE;
}

struct rdp_to_xkb_keyboard_layout {
	UINT32 rdpLayoutCode;
	const char *xkbLayout;
	const char *xkbVariant;
};

/* table reversed from
	https://github.com/awakecoding/FreeRDP/blob/master/libfreerdp/locale/xkb_layout_ids.c#L811 */
static const
struct rdp_to_xkb_keyboard_layout rdp_keyboards[] = {
	{KBD_ARABIC_101, "ara", 0},
	{KBD_BULGARIAN, 0, 0},
	{KBD_CHINESE_TRADITIONAL_US, 0},
	{KBD_CZECH, "cz", 0},
	{KBD_CZECH_PROGRAMMERS, "cz", "bksl"},
	{KBD_CZECH_QWERTY, "cz", "qwerty"},
	{KBD_DANISH, "dk", 0},
	{KBD_GERMAN, "de", 0},
	{KBD_GERMAN_NEO, "de", "neo"},
	{KBD_GERMAN_IBM, "de", "qwerty"},
	{KBD_GREEK, "gr", 0},
	{KBD_GREEK_220, "gr", "simple"},
	{KBD_GREEK_319, "gr", "extended"},
	{KBD_GREEK_POLYTONIC, "gr", "polytonic"},
	{KBD_US, "us", 0},
	{KBD_US_ENGLISH_TABLE_FOR_IBM_ARABIC_238_L, "ara", "buckwalter"},
	{KBD_SPANISH, "es", 0},
	{KBD_SPANISH_VARIATION, "es", "nodeadkeys"},
	{KBD_FINNISH, "fi", 0},
	{KBD_FRENCH, "fr", 0},
	{KBD_HEBREW, "il", 0},
	{KBD_HUNGARIAN, "hu", 0},
	{KBD_HUNGARIAN_101_KEY, "hu", "standard"},
	{KBD_ICELANDIC, "is", 0},
	{KBD_ITALIAN, "it", 0},
	{KBD_ITALIAN_142, "it", "nodeadkeys"},
	{KBD_JAPANESE, "jp", 0},
	{KBD_JAPANESE_INPUT_SYSTEM_MS_IME2002, "jp", 0}, // variant is changed to alphabetical input (0) from "kana".
	{KBD_KOREAN, "kr", 0},
	{KBD_KOREAN_INPUT_SYSTEM_IME_2000, "kr", "kr104"},
	{KBD_DUTCH, "nl", 0},
	{KBD_NORWEGIAN, "no", 0},
	{KBD_POLISH_PROGRAMMERS, "pl", 0},
	{KBD_POLISH_214, "pl", "qwertz"},
	{KBD_ROMANIAN, "ro", 0},
	{KBD_RUSSIAN, "ru", 0},
	{KBD_RUSSIAN_TYPEWRITER, "ru", "typewriter"},
	{KBD_CROATIAN, "hr", 0},
	{KBD_SLOVAK, "sk", 0},
	{KBD_SLOVAK_QWERTY, "sk", "qwerty"},
	{KBD_ALBANIAN, 0, 0},
	{KBD_SWEDISH, "se", 0},
	{KBD_THAI_KEDMANEE, "th", 0},
	{KBD_THAI_KEDMANEE_NON_SHIFTLOCK, "th", "tis"},
	{KBD_TURKISH_Q, "tr", 0},
	{KBD_TURKISH_F, "tr", "f"},
	{KBD_URDU, "in", "urd-phonetic3"},
	{KBD_UKRAINIAN, "ua", 0},
	{KBD_BELARUSIAN, "by", 0},
	{KBD_SLOVENIAN, "si", 0},
	{KBD_ESTONIAN, "ee", 0},
	{KBD_LATVIAN, "lv", 0},
	{KBD_LITHUANIAN_IBM, "lt", "ibm"},
	// 0x429 (KBD_FARSI) is for Persian(Iran)
	// TODO: define exact match with Windows layout in Xkb.
	//       Such as key <AE01>~<AE10> is 1,2,3...0 on Windows, not Persian numbers,
	//       but Xkb doesn't have that layout in "ir" group.
	{KBD_FARSI, "ir", "pes"},
	// 0x50429 is for Dari(Afghanistan)
	// TODO: define KBD_DARI in winpr's keyboard.h
	{0x50429, "af", "basic"},
	{KBD_VIETNAMESE, "vn", 0},
	{KBD_ARMENIAN_EASTERN, "am", 0},
	{KBD_AZERI_LATIN, 0, 0},
	{KBD_FYRO_MACEDONIAN, "mk", 0},
	{KBD_GEORGIAN, "ge", 0},
	{KBD_FAEROESE, 0, 0},
	{KBD_DEVANAGARI_INSCRIPT, 0, 0},
	{KBD_MALTESE_47_KEY, 0, 0},
	{KBD_NORWEGIAN_WITH_SAMI, "no", "smi"},
	{KBD_KAZAKH, "kz", 0},
	{KBD_KYRGYZ_CYRILLIC, "kg", "phonetic"},
	{KBD_TATAR, "ru", "tt"},
	{KBD_BENGALI, "bd", 0},
	{KBD_BENGALI_INSCRIPT, "bd", "probhat"},
	{KBD_PUNJABI, 0, 0},
	{KBD_GUJARATI, "in", "guj"},
	{KBD_TAMIL, "in", "tam"},
	{KBD_TELUGU, "in", "tel"},
	{KBD_KANNADA, "in", "kan"},
	{KBD_MALAYALAM, "in", "mal"},
	{KBD_HINDI_TRADITIONAL, "in", 0},
	{KBD_MARATHI, 0, 0},
	{KBD_MONGOLIAN_CYRILLIC, "mn", 0},
	{KBD_UNITED_KINGDOM_EXTENDED, "gb", "intl"},
	{KBD_SYRIAC, "syc", 0},
	{KBD_SYRIAC_PHONETIC, "syc", "syc_phonetic"},
	{KBD_NEPALI, "np", 0},
	{KBD_PASHTO, "af", "ps"},
	{KBD_DIVEHI_PHONETIC, 0, 0},
	{KBD_LUXEMBOURGISH, 0, 0},
	{KBD_MAORI, "mao", 0},
	{KBD_CHINESE_SIMPLIFIED_US, 0, 0},
	{KBD_SWISS_GERMAN, "ch", "de_nodeadkeys"},
	{KBD_UNITED_KINGDOM, "gb", 0},
	{KBD_LATIN_AMERICAN, "latam", 0},
	{KBD_BELGIAN_FRENCH, "be", 0},
	{KBD_BELGIAN_PERIOD, "be", "oss_sundeadkeys"},
	{KBD_PORTUGUESE, "pt", 0},
	{KBD_SERBIAN_LATIN, "rs", 0},
	{KBD_AZERI_CYRILLIC, "az", "cyrillic"},
	{KBD_SWEDISH_WITH_SAMI, "se", "smi"},
	{KBD_UZBEK_CYRILLIC, "af", "uz"},
	{KBD_INUKTITUT_LATIN, "ca", "ike"},
	{KBD_CANADIAN_FRENCH_LEGACY, "ca", "fr-legacy"},
	{KBD_SERBIAN_CYRILLIC, "rs", 0},
	{KBD_CANADIAN_FRENCH, "ca", "fr-legacy"},
	{KBD_SWISS_FRENCH, "ch", "fr"},
	{KBD_BOSNIAN, "ba", 0},
	{KBD_IRISH, 0, 0},
	{KBD_BOSNIAN_CYRILLIC, "ba", "us"},
	{KBD_UNITED_STATES_DVORAK, "us", "dvorak"},
	{KBD_PORTUGUESE_BRAZILIAN_ABNT2, "br", "abnt2"},
	{KBD_CANADIAN_MULTILINGUAL_STANDARD, "ca", "multix"},
	{KBD_GAELIC, "ie", "CloGaelach"},

	{0x00000000, 0, 0},
};

/* taken from 2.2.7.1.6 Input Capability Set (TS_INPUT_CAPABILITYSET) */
static const char *rdp_keyboard_types[] = {
	"",	/* 0: unused */
	"", /* 1: IBM PC/XT or compatible (83-key) keyboard */
	"", /* 2: Olivetti "ICO" (102-key) keyboard */
	"", /* 3: IBM PC/AT (84-key) or similar keyboard */
	"pc102",/* 4: IBM enhanced (101- or 102-key) keyboard */
	"", /* 5: Nokia 1050 and similar keyboards */
	"",	/* 6: Nokia 9140 and similar keyboards */
	"jp106",/* 7: Japanese keyboard */ /* alternative ja106 */
	"pc102" /* 8: Korean keyboard which is based on pc101, + 2 special Korean keys */
};

void
convert_rdp_keyboard_to_xkb_rule_names(
	UINT32 KeyboardType,
	UINT32 KeyboardSubType,
	UINT32 KeyboardLayout,
	struct xkb_rule_names *xkbRuleNames)
{
	int i;
	memset(xkbRuleNames, 0, sizeof(*xkbRuleNames));
	if (KeyboardType <= ARRAY_LENGTH(rdp_keyboard_types))
		xkbRuleNames->model = rdp_keyboard_types[KeyboardType];
	for (i = 0; rdp_keyboards[i].rdpLayoutCode; i++) {
		if (rdp_keyboards[i].rdpLayoutCode == KeyboardLayout) {
			xkbRuleNames->layout = rdp_keyboards[i].xkbLayout;
			xkbRuleNames->variant = rdp_keyboards[i].xkbVariant;
			break;
		}
	}

	/* Korean keyboard support (KeyboardType 8, LangID 0x412) */
	if (KeyboardType == 8 && ((KeyboardLayout & 0xFFFF) == 0x412)) {
		/* TODO: PC/AT 101 Enhanced Korean Keyboard (Type B) and (Type C) is not supported yet
			 because default Xkb settings for Korean layout doesn't have corresponding
			 configuration.
			 (Type B): KeyboardSubType:4: rctrl_hangul/ratl_hanja
			 (Type C): KeyboardSubType:5: shift_space_hangul/crtl_space_hanja */
		if (KeyboardSubType == 0 ||
		    KeyboardSubType == 3) // PC/AT 101 Enhanced Korean Keyboard (Type A)
			xkbRuleNames->variant = "kr104"; // kr(ralt_hangul)/kr(rctrl_hanja)
		else if (KeyboardSubType == 6) // PC/AT 103 Enhanced Korean Keyboard
			xkbRuleNames->variant = "kr106"; // kr(hw_keys)
	}
	/* Japanese keyboard layout is used with other than Japanese 106/109 keyboard */
	else if (KeyboardType != 7 && ((KeyboardLayout & 0xFFFF) == 0x411)) {
		/* when Japanese keyboard layout is used other than Japanese 106/109 keyboard (keyboard type 7),
		   use "us" layout, since the "jp" layout in xkb expects Japanese 106/109 keyboard layout. */
		xkbRuleNames->layout = "us";
		xkbRuleNames->variant = 0;
	}
	/* Brazilian ABNT2 keyboard */
	else if (KeyboardLayout == KBD_PORTUGUESE_BRAZILIAN_ABNT2) {
		xkbRuleNames->model = "pc105";
	}

	weston_log("%s: matching model=%s layout=%s variant=%s options=%s\n", __FUNCTION__,
		xkbRuleNames->model, xkbRuleNames->layout, xkbRuleNames->variant, xkbRuleNames->options);
}

static BOOL
xf_peer_activate(freerdp_peer* client)
{
	RdpPeerContext *peerCtx;
	struct rdp_backend *b;
	struct rdp_output *output;
	rdpSettings *settings;
	rdpPointerUpdate *pointer;
	struct rdp_peers_item *peersItem;
	struct xkb_rule_names xkbRuleNames;
	struct xkb_keymap *keymap;
	struct weston_output *weston_output;
	pixman_box32_t box;
	pixman_region32_t damage;
	char seat_name[50];
	POINTER_SYSTEM_UPDATE pointer_system;
	char *s;

	peerCtx = (RdpPeerContext *)client->context;
	b = peerCtx->rdpBackend;
	peersItem = &peerCtx->item;
	settings = client->settings;

	if (!settings->SurfaceCommandsEnabled) {
		rdp_debug_error(b, "client doesn't support required SurfaceCommands\n");
		return FALSE;
	}

	if (b->force_no_compression && settings->CompressionEnabled) {
		rdp_debug_error(b, "Forcing compression off\n");
		settings->CompressionEnabled = FALSE;
	}

	/* in RAIL mode, only one peer per backend can be activated */
	if (settings->RemoteApplicationMode) {
		if (b->rdp_peer != client) {
			rdp_debug_error(b, "Another RAIL connection active, only one connection is allowed.\n");
			return FALSE;
		}

		if (!settings->HiDefRemoteApp) {
			/* HiDef is required for RAIL mode. Cookie-cutter window remoting is not supported. */
			rdp_debug_error(b, "HiDef-RAIL is required for RAIL.\n");
			return FALSE;
		}

		/* in HiDef RAIL mode, RAIL-shell must be used */
		if (b->rdprail_shell_api == NULL) {
			rdp_debug_error(b, "HiDef-RAIL is requested from client, but RAIL-shell is not used\n");
			return FALSE;
		}
	}

	/* override settings by env variables */
	s = getenv("WESTON_RDP_DISABLE_CLIPBOARD");
	if (s) {
		if (strcmp(s, "true") == 0)
			settings->RedirectClipboard = FALSE;
	}

	s = getenv("WESTON_RDP_DISABLE_AUDIO_PLAYBACK");
	if (s) {
		if (strcmp(s, "true") == 0)
			settings->AudioPlayback = FALSE;
	}

	s = getenv("WESTON_RDP_DISABLE_AUDIO_CAPTURE");
	if (s) {
		if (strcmp(s, "true") == 0)
			settings->AudioCapture = FALSE;
	}

	if (settings->RemoteApplicationMode ||
		settings->RedirectClipboard ||
		settings->AudioPlayback ||
		settings->AudioCapture) {

		if (!peerCtx->vcm) {
			rdp_debug_error(b, "Virtual channel is required for RAIL, clipboard, audio playback/capture\n");
			goto error_exit;
		}

		/* RAIL, clipboard, Audio playback/capture requires dynamic virtual channel */
		if (!rdp_drdynvc_init(client))
			goto error_exit;

		if (settings->RemoteApplicationMode)
			if (!rdp_rail_peer_activate(client))
				goto error_exit;

		if (settings->AudioPlayback)
			if (rdp_audio_init(peerCtx) != 0)
				goto error_exit;

		if (settings->AudioCapture)
			if (rdp_audioin_init(peerCtx) != 0)
				goto error_exit;
	}

	if (settings->HiDefRemoteApp) {
		/* single monitor case, FreeRDP doesn't call AdjustMonitorsLayout callback, so call now */
		xf_peer_adjust_monitor_layout(client);
		output = NULL;
		weston_output = NULL;
	} else {
		/* multiple monitor is not supported in non-HiDef */
		assert(b->output_default);
		output = b->output_default;
		rdp_debug_error(b, "%s: DesktopWidth:%d, DesktopHeigh:%d, DesktopScaleFactor:%d\n", __FUNCTION__,
			settings->DesktopWidth, settings->DesktopHeight, settings->DesktopScaleFactor);
		if (output->base.width != (int)settings->DesktopWidth ||
			output->base.height != (int)settings->DesktopHeight) {
			if (b->no_clients_resize) {
				/* RDP peers don't dictate their resolution to weston */
				if (!settings->DesktopResize) {
					/* peer does not support desktop resize */
					rdp_debug_error(b, "%s: client doesn't support resizing, closing connection\n", __FUNCTION__);
					goto error_exit;
				} else {
					settings->DesktopWidth = output->base.width;
					settings->DesktopHeight = output->base.height;
					client->update->DesktopResize(client->context);
				}
			} else {
				/* ask weston to adjust size */
				struct weston_mode new_mode;
				struct weston_mode *target_mode;
				new_mode.width = (int)settings->DesktopWidth;
				new_mode.height = (int)settings->DesktopHeight;
				target_mode = ensure_matching_mode(&output->base, &new_mode);
				if (!target_mode) {
					rdp_debug_error(b, "client mode not found\n");
					goto error_exit;
				}
				weston_output_mode_set_native(&output->base, target_mode,
					output->base.scale ? output->base.scale : 1);
				weston_head_set_physical_size(&b->head_default->base,
					settings->DesktopPhysicalWidth,
					settings->DesktopPhysicalHeight);
			}
		}
		pixman_region32_clear(&peerCtx->regionClientHeads);
		pixman_region32_init_rect(&peerCtx->regionClientHeads,
			0, 0, settings->DesktopWidth, settings->DesktopHeight);

		pixman_region32_clear(&b->head_default->regionClient);
		pixman_region32_init_rect(&b->head_default->regionClient,
			0, 0, settings->DesktopWidth, settings->DesktopHeight);

		weston_output = &output->base;

		rdp_debug(b, "%s: OutputWidth:%d, OutputHeight:%d, OutputScaleFactor:%d\n", __FUNCTION__,
			weston_output->width, weston_output->height, weston_output->scale);

		pixman_region32_clear(&peerCtx->regionWestonHeads);
		pixman_region32_init_rect(&peerCtx->regionWestonHeads,
			0, 0, weston_output->width, weston_output->height);

		pixman_region32_clear(&b->head_default->regionWeston);
		pixman_region32_init_rect(&b->head_default->regionWeston,
			0, 0, weston_output->width, weston_output->height);

		RFX_RESET(peerCtx->rfx_context, weston_output->width, weston_output->height);
		NSC_RESET(peerCtx->nsc_context, weston_output->width, weston_output->height);
	}

	if (settings->RemoteApplicationMode)
		rdp_rail_sync_window_status(client);

	if (peersItem->flags & RDP_PEER_ACTIVATED)
		return TRUE;

	/* when here it's the first reactivation, we need to setup a little more */
	rdp_debug(b, "kbd_layout:0x%x kbd_type:0x%x kbd_subType:0x%x kbd_functionKeys:0x%x\n",
			settings->KeyboardLayout, settings->KeyboardType, settings->KeyboardSubType,
			settings->KeyboardFunctionKey);

	convert_rdp_keyboard_to_xkb_rule_names(settings->KeyboardType,
					       settings->KeyboardSubType,
					       settings->KeyboardLayout,
					       &xkbRuleNames);

	keymap = NULL;
	if (xkbRuleNames.layout) {
		keymap = xkb_keymap_new_from_names(b->compositor->xkb_context,
						   &xkbRuleNames, 0);
	}

	if (settings->ClientHostname)
		snprintf(seat_name, sizeof(seat_name), "RDP %s", settings->ClientHostname);
	else
		snprintf(seat_name, sizeof(seat_name), "RDP peer @%s", settings->ClientAddress);

	peersItem->seat = zalloc(sizeof(*peersItem->seat));
	if (!peersItem->seat) {
		xkb_keymap_unref(keymap);
		rdp_debug_error(b, "unable to create a weston_seat\n");
		goto error_exit;
	}

	weston_seat_init(peersItem->seat, b->compositor, seat_name);
	weston_seat_init_keyboard(peersItem->seat, keymap);
	xkb_keymap_unref(keymap);
	weston_seat_init_pointer(peersItem->seat);
	peersItem->seat->led_update = rdp_peer_seat_led_update;

	/* Initialize RDP clipboard after seat is initialized */
	if (settings->RedirectClipboard)
		if (rdp_clipboard_init(client) != 0)
			goto error_exit;

	peersItem->flags |= RDP_PEER_ACTIVATED;

	if (!settings->HiDefRemoteApp && output) {
		/* disable pointer on the client side */
		pointer = client->update->pointer;
		pointer_system.type = SYSPTR_NULL;
		pointer->PointerSystem(client->context, &pointer_system);

		/* sends a full refresh */
		box.x1 = 0;
		box.y1 = 0;
		box.x2 = output->base.width;
		box.y2 = output->base.height;
		pixman_region32_init_with_extents(&damage, &box);

		rdp_peer_refresh_region(&damage, client);

		pixman_region32_fini(&damage);
	}

	return TRUE;

error_exit:

	rdp_clipboard_destroy(peerCtx);
	rdp_audioin_destroy(peerCtx);
	rdp_audio_destroy(peerCtx);
	rdp_rail_peer_context_free(client, peerCtx);
	rdp_drdynvc_destroy(peerCtx);

	return FALSE;
}

static BOOL
xf_peer_post_connect(freerdp_peer *client)
{
	return TRUE;
}

static BOOL
rdp_translate_and_notify_mouse_position(RdpPeerContext *peerContext, UINT16 x, UINT16 y)
{
	struct timespec time;
	int sx, sy;

	if (!peerContext->item.seat)
		return FALSE;

	/* (TS_POINTERX_EVENT):The xy-coordinate of the pointer relative to the top-left
	                       corner of the server's desktop combined all monitors */
	/* first, convert to the coordinate based on primary monitor's upper-left as (0,0) */
	sx = x + peerContext->regionClientHeads.extents.x1;
	sy = y + peerContext->regionClientHeads.extents.y1;

	/* translate client's x/y to the coordinate in weston space. */
	/* TODO: to_weston_coordinate() is translate based on where pointer is,
	         not based-on where/which window underneath. Thus, this doesn't
	         work when window lays across more than 2 monitors and each monitor has
	         different scaling. In such case, hit test to that window area on
	         non primary-resident monitor (surface->output) dosn't work. */
	if (to_weston_coordinate(peerContext, &sx, &sy, NULL, NULL)) {
		weston_compositor_get_time(&time);
		notify_motion_absolute(peerContext->item.seat, &time, sx, sy);
		return TRUE;
	}
	return FALSE;
}

static void
dump_mouseinput(RdpPeerContext *peerContext, UINT16 flags, UINT16 x, UINT16 y, bool is_ex)
{
	struct rdp_backend *b = peerContext->rdpBackend;

	rdp_debug_verbose(b, "RDP mouse input%s: (%d, %d): flags:%x: ", is_ex ? "_ex" : "", x, y, flags);
	if (is_ex) {
		if (flags & PTR_XFLAGS_DOWN)
			rdp_debug_verbose_continue(b, "DOWN ");
		if (flags & PTR_XFLAGS_BUTTON1)
			rdp_debug_verbose_continue(b, "XBUTTON1 ");
		if (flags & PTR_XFLAGS_BUTTON2)
			rdp_debug_verbose_continue(b, "XBUTTON2 ");
	} else {
		if (flags & PTR_FLAGS_WHEEL)
			rdp_debug_verbose_continue(b, "WHEEL ");
		if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
			rdp_debug_verbose_continue(b, "WHEEL_NEGATIVE ");
		if (flags & PTR_FLAGS_HWHEEL)
			rdp_debug_verbose_continue(b, "HWHEEL ");
		if (flags & PTR_FLAGS_MOVE)
			rdp_debug_verbose_continue(b, "MOVE ");
		if (flags & PTR_FLAGS_DOWN)
			rdp_debug_verbose_continue(b, "DOWN ");
		if (flags & PTR_FLAGS_BUTTON1)
			rdp_debug_verbose_continue(b, "BUTTON1 ");
		if (flags & PTR_FLAGS_BUTTON2)
			rdp_debug_verbose_continue(b, "BUTTON2 ");
		if (flags & PTR_FLAGS_BUTTON3)
			rdp_debug_verbose_continue(b, "BUTTON3 ");
	}
	rdp_debug_verbose_continue(b, "\n");
}

static void
rdp_validate_button_state(RdpPeerContext *peerContext, bool pressed, uint32_t* button)
{
	struct rdp_backend *b = peerContext->rdpBackend;
	assert(*button >= BTN_LEFT && *button <= BTN_EXTRA);
	uint32_t index = *button - BTN_LEFT;
	assert(index < ARRAY_LENGTH(peerContext->button_state));
	if (pressed == peerContext->button_state[index]) {
		rdp_debug_verbose(b, "%s: inconsistent button state button:%d (index:%d) pressed:%d\n",
			__func__, *button, index, pressed);
		/* ignore button input */
		*button = 0;
	} else {
		peerContext->button_state[index] = pressed;
	}
	return;
}

static bool
rdp_notify_wheel_scroll(RdpPeerContext *peerContext, UINT16 flags, uint32_t axis)
{
	struct weston_pointer_axis_event weston_event;
	struct rdp_backend *b = peerContext->rdpBackend;
	int ivalue;
	double value;
	struct timespec time;
	int *accumWheelRotationPrecise;
	int *accumWheelRotationDiscrete;

	/*
	* The RDP specs says the lower bits of flags contains the "the number of rotation
	* units the mouse wheel was rotated".
	*
	* https://blogs.msdn.microsoft.com/oldnewthing/20130123-00/?p=5473 explains the 120 value
	*/
	if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
		ivalue = (int)((char)(flags & 0xff));
	else
		ivalue = (flags & 0xff);

	/*
	* Flip the scroll direction as the RDP direction is inverse of X/Wayland 
	* for vertical scroll 
	*/
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
		ivalue *= -1;

		accumWheelRotationPrecise = &peerContext->verticalAccumWheelRotationPrecise;
		accumWheelRotationDiscrete = &peerContext->verticalAccumWheelRotationDiscrete;
	}
	else {
		accumWheelRotationPrecise = &peerContext->horizontalAccumWheelRotationPrecise;
		accumWheelRotationDiscrete = &peerContext->horizontalAccumWheelRotationDiscrete;
	}

	/*
	* Accumulate the wheel increments.
	*
	* Every 12 wheel increments, we will send an update to our Wayland
	* clients with an updated value for the wheel for smooth scrolling.
	*
	* Every 120 wheel increments, we tick one discrete wheel click.
	*/
	*accumWheelRotationPrecise += ivalue;
	*accumWheelRotationDiscrete += ivalue;
	if (abs(*accumWheelRotationPrecise) >= 12) {
		value = (double)(*accumWheelRotationPrecise / 12);

		weston_event.axis = axis;
		weston_event.value = value;
		weston_event.discrete = *accumWheelRotationDiscrete / 120;
		weston_event.has_discrete = true;

		rdp_debug_verbose(b, "wheel: value:%f discrete:%d\n", 
			weston_event.value, weston_event.discrete);

		weston_compositor_get_time(&time);

		notify_axis(peerContext->item.seat, &time, &weston_event);

		*accumWheelRotationPrecise %= 12;
		*accumWheelRotationDiscrete %= 120;
		
		return true;
	}

	return false;
}

static FREERDP_CB_RET_TYPE
xf_mouseEvent(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y)
{
	RdpPeerContext *peerContext = (RdpPeerContext *)input->context;
	uint32_t button = 0;
	bool need_frame = false;
	struct timespec time;

	dump_mouseinput(peerContext, flags, x, y, false);

	/* Per RDP spec, the x,y position is valid on all input mouse messages,
	 * except for PTR_FLAGS_WHEEL and PTR_FLAGS_HWHEEL event. Take the opportunity
	 * to resample our x,y position even when PTR_FLAGS_MOVE isn't explicitly set,
	 * for example a button down/up only notification, to ensure proper sync with
	 * the RDP client.
	 */
	if (!(flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL))) {
		if (rdp_translate_and_notify_mouse_position(peerContext, x, y))
			need_frame = true;
	}

	if (flags & PTR_FLAGS_BUTTON1) {
		if (peerContext->mouseButtonSwap)
			button = BTN_RIGHT;
		else
			button = BTN_LEFT;
	}
	else if (flags & PTR_FLAGS_BUTTON2) {
		if (peerContext->mouseButtonSwap)
			button = BTN_LEFT;
		else
			button = BTN_RIGHT;
	}
	else if (flags & PTR_FLAGS_BUTTON3)
		button = BTN_MIDDLE;

	if (button)
		rdp_validate_button_state(
			peerContext,
			flags & PTR_FLAGS_DOWN ? true : false,
			&button);

	if (button) {
		weston_compositor_get_time(&time);
		notify_button(peerContext->item.seat, &time, button,
			(flags & PTR_FLAGS_DOWN) ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED
		);
		need_frame = true;
	}

	/* Per RDP spec, if both PTRFLAGS_WHEEL and PTRFLAGS_HWHEEL are specified
	 * then PTRFLAGS_WHEEL takes precedent
	 */
	if (flags & PTR_FLAGS_WHEEL) {
		if (rdp_notify_wheel_scroll(peerContext, flags, WL_POINTER_AXIS_VERTICAL_SCROLL))
			need_frame = true;
	} else if (flags & PTR_FLAGS_HWHEEL) {
		if (rdp_notify_wheel_scroll(peerContext, flags, WL_POINTER_AXIS_HORIZONTAL_SCROLL))
			need_frame = true;
	}

	if (need_frame)
		notify_pointer_frame(peerContext->item.seat);

	FREERDP_CB_RETURN(TRUE);
}

static FREERDP_CB_RET_TYPE
xf_extendedMouseEvent(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y)
{
	RdpPeerContext *peerContext = (RdpPeerContext *)input->context;
	uint32_t button = 0;
	bool need_frame = false;
	struct timespec time;

	dump_mouseinput(peerContext, flags, x, y, true);

	if (rdp_translate_and_notify_mouse_position(peerContext, x, y))
		need_frame = true;

	if (flags & PTR_XFLAGS_BUTTON1)
		button = BTN_SIDE;
	else if (flags & PTR_XFLAGS_BUTTON2)
		button = BTN_EXTRA;

	if (button)
		rdp_validate_button_state(
			peerContext,
			flags & PTR_XFLAGS_DOWN ? true : false,
			&button);

	if (button) {
		weston_compositor_get_time(&time);
		notify_button(peerContext->item.seat, &time, button,
			(flags & PTR_XFLAGS_DOWN) ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED
		);
		need_frame = true;
	}

	if (need_frame)
		notify_pointer_frame(peerContext->item.seat);

	FREERDP_CB_RETURN(TRUE);
}

static FREERDP_CB_RET_TYPE
xf_input_synchronize_event(rdpInput *input, UINT32 flags)
{
	freerdp_peer *client = input->context->peer;
	RdpPeerContext *peerCtx = (RdpPeerContext *)input->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct rdp_output *output = b->output_default;
	pixman_box32_t box;
	pixman_region32_t damage;

	rdp_debug_verbose(b, "RDP backend: %s ScrLk:%d, NumLk:%d, CapsLk:%d, KanaLk:%d\n",
		__func__,
		flags & KBD_SYNC_SCROLL_LOCK ? 1 : 0,
		flags & KBD_SYNC_NUM_LOCK ? 1 : 0,
		flags & KBD_SYNC_CAPS_LOCK ? 1 : 0,
		flags & KBD_SYNC_KANA_LOCK ? 1 : 0);
	struct weston_keyboard *keyboard =
		weston_seat_get_keyboard(peerCtx->item.seat);
	if (keyboard) {
		uint32_t value = 0;
		if (flags & KBD_SYNC_NUM_LOCK)
			value |= WESTON_NUM_LOCK;
		if (flags & KBD_SYNC_CAPS_LOCK)
			value |= WESTON_CAPS_LOCK;
		weston_keyboard_set_locks(keyboard,
			WESTON_NUM_LOCK|WESTON_CAPS_LOCK,
			value);
	}

	if (!client->settings->HiDefRemoteApp && output) {
		/* sends a full refresh */
		box.x1 = 0;
		box.y1 = 0;
		box.x2 = output->base.width;
		box.y2 = output->base.height;
		pixman_region32_init_with_extents(&damage, &box);

		rdp_peer_refresh_region(&damage, client);

		pixman_region32_fini(&damage);
	}

	FREERDP_CB_RETURN(TRUE);
}

static FREERDP_CB_RET_TYPE
xf_input_keyboard_event(rdpInput *input, UINT16 flags, UINT16 code)
{
	uint32_t scan_code, vk_code, full_code;
	enum wl_keyboard_key_state keyState;
	freerdp_peer *client = input->context->peer;
	RdpPeerContext *peerContext = (RdpPeerContext *)input->context;
	struct weston_keyboard *keyboard = weston_seat_get_keyboard(peerContext->item.seat);
	/*struct rdp_backend *b = peerContext->rdpBackend;*/
	bool send_key = false;
	bool send_release_key = false;

	int notify = 0;
	struct timespec time;

	if (!(peerContext->item.flags & RDP_PEER_ACTIVATED))
		FREERDP_CB_RETURN(TRUE);

	if (flags & KBD_FLAGS_DOWN) {
		keyState = WL_KEYBOARD_KEY_STATE_PRESSED;
		notify = 1;
	} else if (flags & KBD_FLAGS_RELEASE) {
		keyState = WL_KEYBOARD_KEY_STATE_RELEASED;
		notify = 1;
	}

	if (keyboard && notify) {
		full_code = code;
		if (flags & KBD_FLAGS_EXTENDED)
			full_code |= KBD_FLAGS_EXTENDED;

		/* Korean keyboard support */
		/* WinPR's GetVirtualKeyCodeFromVirtualScanCode() can't handle hangul/hanja keys */
		/* 0x1f1 and 0x1f2 keys are only exists on Korean 103 keyboard (Type 8:SubType 6) */
		if (client->settings->KeyboardType == 8 &&
			client->settings->KeyboardSubType == 6 &&
			((full_code == 0x1f1) || (full_code == 0x1f2))) {
			if (full_code == 0x1f1)
				vk_code = VK_HANGUL;
			else if (full_code == 0x1f2)
				vk_code = VK_HANJA;
			/* From Linux's keyboard driver at drivers/input/keyboard/atkbd.c */
			/*
			 * HANGEUL and HANJA keys do not send release events so we need to
			 * generate such events ourselves
			 */
			/* RDP works same, there is no release for those 2 Korean keys,
			 * thus generate release right after press. */
			assert(keyState == WL_KEYBOARD_KEY_STATE_PRESSED);
			send_release_key = true;
		} else {
			vk_code = GetVirtualKeyCodeFromVirtualScanCode(full_code, client->settings->KeyboardType);
		}
		/* Korean keyboard support */
		/* WinPR's GetKeycodeFromVirtualKeyCode() expects no extended bit for VK_HANGUL and VK_HANJA */
		if (vk_code != VK_HANGUL && vk_code != VK_HANJA)
			if (flags & KBD_FLAGS_EXTENDED)
				vk_code |= KBDEXT;

		scan_code = GetKeycodeFromVirtualKeyCode(vk_code, KEYCODE_TYPE_EVDEV);
		/*weston_log("code=%x ext=%d vk_code=%x scan_code=%x\n", code, (flags & KBD_FLAGS_EXTENDED) ? 1 : 0,
				vk_code, scan_code);*/

		/* Ignore release if key is not previously pressed. */
		if (keyState == WL_KEYBOARD_KEY_STATE_RELEASED) {
			uint32_t *k, *end;
			end = keyboard->keys.data + keyboard->keys.size;
			for (k = keyboard->keys.data; k < end; k++) {
				if (*k == (scan_code - 8)) {
					send_key = true;
					break;
				}
			}
		} else {
			send_key = true;
		}

		if (send_key) {
send_release_key:
			weston_compositor_get_time(&time);
			notify_key(peerContext->item.seat, &time,
				scan_code - 8, keyState, STATE_UPDATE_AUTOMATIC);

			/*rdp_debug(b, "RDP backend: %s code=%x ext=%d vk_code=%x scan_code=%x pressed=%d, idle_inhibit=%d\n",
				__func__, code, (flags & KBD_FLAGS_EXTENDED) ? 1 : 0,
				vk_code, scan_code, keyState, b->compositor->idle_inhibit);*/

			if (send_release_key) {
				send_release_key = false;
				assert(keyState == WL_KEYBOARD_KEY_STATE_PRESSED); 
				keyState = WL_KEYBOARD_KEY_STATE_RELEASED;
				goto send_release_key;
			}
		}
	}

	FREERDP_CB_RETURN(TRUE);
}

static FREERDP_CB_RET_TYPE
xf_input_unicode_keyboard_event(rdpInput *input, UINT16 flags, UINT16 code)
{
	RdpPeerContext *peerContext = (RdpPeerContext *)input->context;
	struct rdp_backend *b = peerContext->rdpBackend;

	rdp_debug_error(b, "Client sent a unicode keyboard event (flags:0x%X code:0x%X)\n", flags, code);

	FREERDP_CB_RETURN(TRUE);
}


static FREERDP_CB_RET_TYPE
xf_suppress_output(rdpContext *context, BYTE allow, const RECTANGLE_16 *area)
{
	RdpPeerContext *peerContext = (RdpPeerContext *)context;

	if (allow)
		peerContext->item.flags |= RDP_PEER_OUTPUT_ENABLED;
	else
		peerContext->item.flags &= (~RDP_PEER_OUTPUT_ENABLED);

	FREERDP_CB_RETURN(TRUE);
}

static BOOL
using_session_tls(struct rdp_backend *b)
{
	return b->server_cert_content && b->server_key_content;
}

static BOOL
is_tls_enabled(struct rdp_backend *b)
{
	return (b->server_cert && b->server_key) || using_session_tls(b);
}

static int
rdp_peer_init(freerdp_peer *client, struct rdp_backend *b)
{
	unsigned i, rcount = 0;
	void *rfds[MAX_FREERDP_FDS+1]; // +1 for WTSVirtualChannelManagerGetFileDescriptor.
	int fd;
	struct wl_event_loop *loop;
	rdpSettings	*settings;
	rdpInput *input;
	RdpPeerContext *peerCtx;

	client->ContextSize = sizeof(RdpPeerContext);
	client->ContextNew = (psPeerContextNew)rdp_peer_context_new;
	client->ContextFree = (psPeerContextFree)rdp_peer_context_free;
	freerdp_peer_context_new(client);

	peerCtx = (RdpPeerContext *) client->context;
	peerCtx->rdpBackend = b;

	settings = client->settings;
	/* configure security settings */
	if (b->rdp_key)
		settings->RdpKeyFile = strdup(b->rdp_key);
	if (is_tls_enabled(b)) {
		if (using_session_tls(b)) {
			settings->CertificateContent = strdup(b->server_cert_content);
			settings->PrivateKeyContent = strdup(b->server_key_content);
		} else {
			settings->CertificateFile = strdup(b->server_cert);
			settings->PrivateKeyFile = strdup(b->server_key);
		}
	} else {
		settings->TlsSecurity = FALSE;
	}
	settings->NlaSecurity = FALSE;

	if (!client->Initialize(client)) {
		rdp_debug_error(b, "peer initialization failed\n");
		goto error_initialize;
	}

	settings->OsMajorType = OSMAJORTYPE_UNIX;
	settings->OsMinorType = OSMINORTYPE_PSEUDO_XSERVER;
	settings->ColorDepth = 32;
	settings->RefreshRect = TRUE;
	settings->RemoteFxCodec = FALSE; // TODO:
	settings->NSCodec = TRUE;
	settings->FrameMarkerCommandEnabled = TRUE;
	settings->SurfaceFrameMarkerEnabled = TRUE;
	settings->RemoteApplicationMode = TRUE;
	settings->RemoteApplicationSupportLevel =
		RAIL_LEVEL_SUPPORTED |
		RAIL_LEVEL_SHELL_INTEGRATION_SUPPORTED |
		RAIL_LEVEL_LANGUAGE_IME_SYNC_SUPPORTED |
		RAIL_LEVEL_SERVER_TO_CLIENT_IME_SYNC_SUPPORTED |
		RAIL_LEVEL_HANDSHAKE_EX_SUPPORTED;
	settings->SupportGraphicsPipeline = TRUE;
	settings->SupportMonitorLayoutPdu = TRUE;
	settings->RedirectClipboard = TRUE;
	settings->HasExtendedMouseEvent = TRUE;
	settings->HasHorizontalWheel = TRUE;

	client->Capabilities = xf_peer_capabilities;
	client->PostConnect = xf_peer_post_connect;
	client->Activate = xf_peer_activate;
	client->AdjustMonitorsLayout = xf_peer_adjust_monitor_layout;

	client->update->SuppressOutput = (pSuppressOutput)xf_suppress_output;

	input = client->input;
	input->SynchronizeEvent = xf_input_synchronize_event;
	input->MouseEvent = xf_mouseEvent;
	input->ExtendedMouseEvent = xf_extendedMouseEvent;
	input->KeyboardEvent = xf_input_keyboard_event;
	input->UnicodeKeyboardEvent = xf_input_unicode_keyboard_event;

	if (!client->GetFileDescriptor(client, rfds, &rcount)) {
		rdp_debug_error(b, "unable to retrieve client fds\n");
		goto error_initialize;
	}

	PWtsApiFunctionTable fn = FreeRDP_InitWtsApi();
	WTSRegisterWtsApiFunctionTable(fn);
	peerCtx->vcm = WTSOpenServerA((LPSTR)peerCtx);
	if (peerCtx->vcm) {
		WTSVirtualChannelManagerGetFileDescriptor(peerCtx->vcm, rfds, &rcount);
	} else {
		rdp_debug_error(b, "WTSOpenServer is failed! continue without virtual channel.\n");
	}

	loop = wl_display_get_event_loop(b->compositor->wl_display);
	for (i = 0; i < rcount; i++) {
		fd = (int)(long)(rfds[i]);

		peerCtx->events[i] = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
				rdp_client_activity, client);
	}
	for ( ; i < ARRAY_LENGTH(peerCtx->events); i++)
		peerCtx->events[i] = 0;

	peerCtx->loop_event_source_fd = eventfd(0, EFD_SEMAPHORE | EFD_CLOEXEC);
	if (peerCtx->loop_event_source_fd == -1)
		goto error_peer_initialize;

	if (!rdp_rail_peer_init(client, peerCtx))
		goto error_peer_initialize;

	/* This tracks the single peer connected. This field only used for RAIL mode
	   and, with RAIL mode, there can be only one peer per backend, and that
	   will be validated at xf_peer_activate once connection mode is reflected
	   in settings, and this will be reset to NULL when the peer disconnects */
	if (!b->rdp_peer)
		b->rdp_peer = client;

	/* chain peers at default_output */
	if (b->output_default)
		wl_list_insert(&b->output_default->peers, &peerCtx->item.link);
	return 0;

error_peer_initialize:
	if (peerCtx->loop_event_source_fd != -1) {
		close(peerCtx->loop_event_source_fd);
		peerCtx->loop_event_source_fd = -1;
	}
	for (i = 0; i < ARRAY_LENGTH(peerCtx->events); i++) {
		if (peerCtx->events[i]) {
			wl_event_source_remove(peerCtx->events[i]);
			peerCtx->events[i] = NULL;
		}
	}
	if (peerCtx->vcm) {
		WTSCloseServer(peerCtx->vcm);
		peerCtx->vcm = NULL;
	}

error_initialize:
	client->Close(client);
	return -1;
}


static FREERDP_CB_RET_TYPE
rdp_incoming_peer(freerdp_listener *instance, freerdp_peer *client)
{
	struct rdp_backend *b = (struct rdp_backend *)instance->param4;
	if (rdp_peer_init(client, b) < 0) {
		rdp_debug_error(b, "error when treating incoming peer\n");
		FREERDP_CB_RETURN(FALSE);
	}

	FREERDP_CB_RETURN(TRUE);
}

#if HAVE_OPENSSL
static void
rdp_generate_session_tls(struct rdp_backend *b)
{
	EVP_PKEY *pkey;
	BIGNUM *rsa_bn;
	RSA *rsa;
	BIO *bio, *bio_x509;
	BUF_MEM *mem, *mem_x509;
	X509 *x509;
	long serial = 0;
	ASN1_TIME *before, *after;
	X509_NAME *name;
	X509V3_CTX ctx;
	X509_EXTENSION *ext;
	const EVP_MD *md;
	const char session_name[] = "weston";

	pkey = EVP_PKEY_new();
	assert(pkey != NULL);
	rsa_bn = BN_new();
	assert(rsa_bn != NULL);
	rsa = RSA_new();
	assert(rsa != NULL);
	BN_set_word(rsa_bn, RSA_F4);
	assert(RSA_generate_key_ex(rsa, 2048, rsa_bn, NULL) == 1);
	BN_clear_free(rsa_bn);
	EVP_PKEY_assign_RSA(pkey, rsa);

	bio = BIO_new(BIO_s_mem());
	assert(bio != NULL);
	assert(PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, NULL, NULL) == 1);
	BIO_get_mem_ptr(bio, &mem);
	b->server_key_content = (char *)calloc(mem->length+1, 1);
	memcpy(b->server_key_content, mem->data, mem->length);
	BIO_free_all(bio);

	x509 = X509_new();
	X509_set_version(x509, 2);
	RAND_bytes((unsigned char *)&serial, sizeof(serial));
	ASN1_INTEGER_set(X509_get_serialNumber(x509), serial);
	before = X509_getm_notBefore(x509);
	X509_gmtime_adj(before, 0);
	after = X509_getm_notAfter(x509);
	X509_gmtime_adj(after, 60);    /* good for a minute */
	X509_set_pubkey(x509, pkey);
	name = X509_get_subject_name(x509);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, session_name, sizeof(session_name)-1, -1, 0);
	X509_set_issuer_name(x509, name);
	X509V3_set_ctx_nodb(&ctx);
	X509V3_set_ctx(&ctx, x509, x509, NULL, NULL, 0);
	ext = X509V3_EXT_conf_nid(NULL, &ctx, NID_ext_key_usage, "serverAuth");
	assert(ext);
	X509_add_ext(x509, ext, -1);
	X509_EXTENSION_free(ext);
	md = EVP_sha256();
	assert(X509_sign(x509, pkey, md) != 0);

	bio_x509 = BIO_new(BIO_s_mem());
	assert(bio_x509 != NULL);
	PEM_write_bio_X509(bio, x509);
	BIO_get_mem_ptr(bio_x509, &mem_x509);
	b->server_cert_content = (char *)calloc(mem_x509->length+1, 1);
	memcpy(b->server_cert_content, mem_x509->data, mem_x509->length);
	//weston_log("%s", b->server_cert_content);
	BIO_free_all(bio_x509);

	X509_free(x509);
	EVP_PKEY_free(pkey);
}
#endif

static const struct weston_rdp_output_api api = {
	rdp_output_set_size,
	rdp_output_get_config,
};

static int create_vsock_fd(int port)
{
	struct sockaddr_vm socket_address;

	int socket_fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (socket_fd < 0) {
		weston_log("Fail to create vsocket");
		return -1;
	}

	const int bufferSize = 65536;

	if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize)) < 0) {
		weston_log("Fail to setsockopt SO_SNDBUF");
		return -1;
	}

	if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize)) < 0) {
		weston_log("Fail to setsockopt SO_RCVBUF");
		return -1;
	}

	memset(&socket_address, 0, sizeof(socket_address));

	socket_address.svm_family = AF_VSOCK;
	socket_address.svm_cid    = VMADDR_CID_ANY;
	socket_address.svm_port   = port;

	socklen_t socket_addr_size = sizeof(socket_address);

	if (bind(socket_fd, (const struct sockaddr *)&socket_address, socket_addr_size) < 0) {
		weston_log("Fail to bind socket to address socket");
		close(socket_fd);
		return -2;
	}

	int status = listen(socket_fd, 1);

	if (status != 0) {
		weston_log("Fail to listen on socket");
		close(socket_fd);
		return -4;
	}
	return socket_fd;
}

static int use_vsock_fd(int port)
{
	char *fd_str = getenv("USE_VSOCK");
	if (!fd_str) {
		return -1;
	}

	int fd;
	if (strlen(fd_str) != 0) {
		fd = atoi(fd_str);
		weston_log("Using external fd for incoming connections: %d\n", fd);
		if (fd == 0) {
			fd = -1;
		}
	} else {
		fd = create_vsock_fd(port);
		weston_log("Created vsock for external connections: %d\n", fd);
	}

	return fd;
}

extern char **environ; /* defined by libc */

static struct rdp_backend *
rdp_backend_create(struct weston_compositor *compositor,
		   struct weston_rdp_backend_config *config)
{
	struct rdp_backend *b;
	char *fd_str;
	char *fd_tail;
	int fd, ret;
	struct rdp_output *output;
	char *s;
	int i;
	struct timespec ts;

	b = zalloc(sizeof *b);
	if (b == NULL)
		return NULL;

	b->compositor_tid = rdp_get_tid();
	b->compositor = compositor;
	b->base.destroy = rdp_destroy;
	b->base.create_output = rdp_output_create;
	b->rdp_key = config->rdp_key ? strdup(config->rdp_key) : NULL;
	b->server_cert = config->server_cert ? strdup(config->server_cert) : NULL;
	b->server_key = config->server_key ? strdup(config->server_key) : NULL;
	b->no_clients_resize = config->no_clients_resize;
	b->force_no_compression = config->force_no_compression;

	wl_list_init(&b->output_list);
	wl_list_init(&b->head_list);
	b->head_index = 0;

	b->debug = weston_log_ctx_add_log_scope(compositor->weston_log_ctx,
						   "rdp-backend",
						   "Debug messages from RDP backend\n",
						    NULL, NULL, NULL);
	if (b->debug) {
		s = getenv("WESTON_RDP_DEBUG_LEVEL");
		if (s) {
			if (!safe_strtoint(s, &b->debugLevel))
				b->debugLevel = RDP_DEBUG_LEVEL_DEFAULT;
			else if (b->debugLevel > RDP_DEBUG_LEVEL_VERBOSE)
				b->debugLevel = RDP_DEBUG_LEVEL_VERBOSE;
		} else {
			b->debugLevel = RDP_DEBUG_LEVEL_DEFAULT;
		}
	}
	rdp_debug(b, "RDP backend: WESTON_RDP_DEBUG_LEVEL: %d\n", b->debugLevel);
	/* After here, rdp_debug() is ready to be used */

	s = getenv("WESTON_RDP_MONITOR_REFRESH_RATE");
	if (s) {
		if (!safe_strtoint(s, &b->rdp_monitor_refresh_rate) ||
			b->rdp_monitor_refresh_rate == 0) {
			b->rdp_monitor_refresh_rate = RDP_MODE_FREQ;
		} else {
			b->rdp_monitor_refresh_rate *= 1000;
		}
	} else {
		b->rdp_monitor_refresh_rate = RDP_MODE_FREQ;
	}
	rdp_debug(b, "RDP backend: WESTON_RDP_MONITOR_REFRESH_RATE: %d\n", b->rdp_monitor_refresh_rate);

	clock_getres(CLOCK_MONOTONIC, &ts);
	rdp_debug(b, "RDP backend: timer resolution tv_sec:%ld tv_nsec:%ld\n", (intmax_t)ts.tv_sec, ts.tv_nsec);

	/* For diagnostics purpose, dump all enviroment to log file */
	/* TODO: privacy review */
	rdp_debug(b, "RDP backend: Environment dump - start\n");
	for (i = 0; environ[i]; i++) {
		rdp_debug(b, "  %s\n", environ[i]);
	}
	rdp_debug(b, "RDP backend: Environment dump - end\n");

	compositor->backend = &b->base;

	fd = use_vsock_fd(config->port);
	/* if we are using VSOCK to connect to the rdp backend, we don't need to enforce the TLS
	   encryption, since FreeRDP will consider AF_UNIX and AF_VSOCK as a local connection */
	if (fd <= 0 || config->env_socket)
	{
		if (!b->rdp_key && (!b->server_cert || !b->server_key)) {
	#if HAVE_OPENSSL
			rdp_generate_session_tls(b);
	#else
			rdp_debug_error(b, "the RDP compositor requires keys and an optional certificate for RDP or TLS security ("
					"--rdp4-key or --rdp-tls-cert/--rdp-tls-key)\n");
			goto err_free_strings;
	#endif
		}

		/* activate TLS only if certificate/key are available */
		if (is_tls_enabled(b)) {
			rdp_debug_error(b, "TLS support activated\n");
		} else if (!b->rdp_key) {
			goto err_free_strings;
		}
	}

	if (weston_compositor_set_presentation_clock_software(compositor) < 0)
		goto err_compositor;

	if (pixman_renderer_init(compositor) < 0)
		goto err_compositor;

	if (rdp_head_create(compositor, TRUE, NULL) == NULL)
		goto err_compositor;

	if (rdp_rail_backend_create(b) < 0)
		goto err_output;

	compositor->capabilities |= WESTON_CAP_ARBITRARY_MODES;

	if (!config->env_socket) {
		b->listener = freerdp_listener_new();
		b->listener->PeerAccepted = rdp_incoming_peer;
		b->listener->param4 = b;
		if (fd > 0) {
			rdp_debug_error(b, "Using VSOCK for incoming connections: %d\n", fd);

			if (!b->listener->OpenFromSocket(b->listener, fd)) {
				rdp_debug_error(b, "unable opem from socket fd: %d\n", fd);
				goto err_listener;
			}
		} else {
			if (!b->listener->Open(b->listener, config->bind_address, config->port)) {
				rdp_debug_error(b, "unable to bind rdp socket\n");
				goto err_listener;
			}
		}

		if (rdp_implant_listener(b, b->listener) < 0)
			goto err_compositor;
	} else {
		/* get the socket from RDP_FD var */
		fd_str = getenv("RDP_FD");
		if (!fd_str) {
			rdp_debug_error(b, "RDP_FD env variable not set\n");
			goto err_output;
		}

		fd = strtoul(fd_str, &fd_tail, 10);
		if (errno != 0 || fd_tail == fd_str || *fd_tail != '\0'
			|| rdp_peer_init(freerdp_peer_new(fd), b))
			goto err_output;
	}

	ret = weston_plugin_api_register(compositor, WESTON_RDP_OUTPUT_API_NAME,
					 &api, sizeof(api));

	if (ret < 0) {
		rdp_debug_error(b, "Failed to register output API.\n");
		goto err_output;
	}

	return b;

err_listener:
	freerdp_listener_free(b->listener);
err_output:
	wl_list_for_each(output, &b->output_list, link) {
		weston_output_release(&output->base);
	}
	if (b->head_default) {
		rdp_head_destroy(compositor, b->head_default);
		assert(b->head_default == NULL);
	}
err_compositor:
	weston_compositor_shutdown(compositor);
err_free_strings:
	if (b->debug)
		weston_log_scope_destroy(b->debug);
	free(b->rdp_key);
	free(b->server_cert);
	free(b->server_key);
	free(b->server_cert_content);
	free(b->server_key_content);
	free(b);
	return NULL;
}

static void
config_init_to_defaults(struct weston_rdp_backend_config *config)
{
	config->bind_address = NULL;
	config->port = 3389;
	config->rdp_key = NULL;
	config->server_cert = NULL;
	config->server_key = NULL;
	config->env_socket = 0;
	config->no_clients_resize = 0;
	config->force_no_compression = 0;
}

WL_EXPORT int
weston_backend_init(struct weston_compositor *compositor,
			struct weston_backend_config *config_base)
{
	struct rdp_backend *b;
	struct weston_rdp_backend_config config = {{ 0, }};
	int major, minor, revision;

#if FREERDP_VERSION_MAJOR >= 2
	winpr_InitializeSSL(0);
#endif
	freerdp_get_version(&major, &minor, &revision);
	weston_log("using FreeRDP version %d.%d.%d\n", major, minor, revision);

	if (config_base == NULL ||
		config_base->struct_version != WESTON_RDP_BACKEND_CONFIG_VERSION ||
		config_base->struct_size > sizeof(struct weston_rdp_backend_config)) {
		weston_log("RDP backend config structure is invalid\n");
		return -1;
	}

	config_init_to_defaults(&config);
	memcpy(&config, config_base, config_base->struct_size);

	b = rdp_backend_create(compositor, &config);
	if (b == NULL)
		return -1;
	return 0;
}
