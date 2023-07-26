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

#include "weston.h"
#include "rdpdisp.h"
#include <libweston/backend-rdp.h>


static bool
is_line_intersected(int l1, int l2, int r1, int r2)
{
	int l = l1 > r1 ? l1 : r1;
	int r = l2 < r2 ? l2 : r2;
	return (l < r);
}

static int
compare_monitors_x(const void *p1, const void *p2)
{
	const struct weston_rdp_output_api *api;
	const struct weston_head *whl = *(const struct weston_head **)p1;
	const struct weston_head *whr = *(const struct weston_head **)p2;
	rdpMonitor *l, *r;

	api = weston_rdp_output_get_api(whr->compositor);
	l = api->head_get_rdpmonitor(whl);
	r = api->head_get_rdpmonitor(whr);

	return l->x > r->x;
}

static int
compare_monitors_y(const void *p1, const void *p2)
{
	const struct weston_rdp_output_api *api;
	const struct weston_head *whl = *(const struct weston_head **)p1;
	const struct weston_head *whr = *(const struct weston_head **)p2;
	rdpMonitor *l, *r;

	api = weston_rdp_output_get_api(whr->compositor);
	l = api->head_get_rdpmonitor(whl);
	r = api->head_get_rdpmonitor(whr);

	return l->y > r->y;
}

static float
disp_get_client_scale_from_monitor(struct weston_compositor *ec, const rdpMonitor *config)
{
	struct wet_rdp_params *rdp_params = wet_get_rdp_params(ec);

	if (config->attributes.desktopScaleFactor == 0.0)
		return 1.0f;

	if (rdp_params->enable_hi_dpi_support) {
		if (rdp_params->debug_desktop_scaling_factor)
			return (float)rdp_params->debug_desktop_scaling_factor / 100.f;
		else if (rdp_params->enable_fractional_hi_dpi_support)
			return (float)config->attributes.desktopScaleFactor / 100.0f;
		else if (rdp_params->enable_fractional_hi_dpi_roundup)
			return (float)(int)((config->attributes.desktopScaleFactor + 50) / 100);
		else
			return (float)(int)(config->attributes.desktopScaleFactor / 100);
	} else {
		return 1.0f;
	}
}

static int
disp_get_output_scale_from_monitor(struct weston_compositor *ec, const rdpMonitor *config)
{
	return (int) disp_get_client_scale_from_monitor(ec, config);
}

static rdpMonitor *
get_first_head_config(struct weston_compositor *ec)
{
	const struct weston_rdp_output_api *api = weston_rdp_output_get_api(ec);
	struct weston_head *head;

	wl_list_for_each(head, &ec->head_list, compositor_link)
		return api->head_get_rdpmonitor(head);

	return NULL;
}

static void
sort_head_list(struct weston_compositor *ec, int (*compar)(const void *, const void *))
{
	int count = wl_list_length(&ec->head_list);
	struct weston_head *head_array[count];
	struct weston_head *iter, *tmp;
	int i = 0;

	wl_list_for_each_safe(iter, tmp, &ec->head_list, compositor_link) {
		head_array[i++] = iter;
		wl_list_remove(&iter->compositor_link);
	}

	qsort(head_array, count, sizeof(struct weston_head *), compar);

	wl_list_init(&ec->head_list);
	for (i = 0; i < count; i++)
		wl_list_insert(ec->head_list.prev, &head_array[i]->compositor_link);
}

void
disp_monitor_validate_and_compute_layout(struct weston_compositor *ec)
{
	const struct weston_rdp_output_api *api = weston_rdp_output_get_api(ec);
	struct weston_head *iter;
	bool isConnected_H = false;
	bool isConnected_V = false;
	bool isScalingUsed = false;
	bool isScalingSupported = true;
	int upperLeftX = 0;
	int upperLeftY = 0;
	int i;
	int count = wl_list_length(&ec->head_list);
	pixman_rectangle32_t rectWeston[count];

	wl_list_for_each(iter, &ec->head_list, compositor_link) {
		rdpMonitor *head = api->head_get_rdpmonitor(iter);
		float client_scale = disp_get_client_scale_from_monitor(ec, head);

		/* check if any monitor has scaling enabled */
		if (client_scale != 1.0f)
			isScalingUsed = true;

		/* find upper-left corner of combined monitors in client space */
		if (upperLeftX > head->x)
			upperLeftX = head->x;
		if (upperLeftY > head->y)
			upperLeftY = head->y;
	}
	assert(upperLeftX <= 0);
	assert(upperLeftY <= 0);
	weston_log("Client desktop upper left coordinate (%d,%d)\n", upperLeftX, upperLeftY);

	count = wl_list_length(&ec->head_list);

	if (count > 1) {
		rdpMonitor *head, *last;
		int32_t offsetFromOriginClient;

		/* first, sort monitors horizontally */
		sort_head_list(ec, compare_monitors_x);
		head = get_first_head_config(ec);
		last = head;
		assert(upperLeftX == head->x);

		/* check if monitors are horizontally connected each other */
		offsetFromOriginClient = head->x + head->width;
		i = 0;
		wl_list_for_each(iter, &ec->head_list, compositor_link) {
			rdpMonitor *cur = api->head_get_rdpmonitor(iter);

			i++;
			if (i == 1)
				continue;

			if (offsetFromOriginClient != cur->x) {
				weston_log("\tRDP client reported monitors not horizontally connected each other at %d (x check)\n", i);
				break;
			}
			offsetFromOriginClient += cur->width;

			if (!is_line_intersected(last->y,
						 last->y + last->height,
						 cur->y,
						 cur->y + cur->height)) {
				weston_log("\tRDP client reported monitors not horizontally connected each other at %d (y check)\n\n", i);
				break;
			}
			last = cur;
		}
		if (i == count) {
			weston_log("\tAll monitors are horizontally placed\n");
			isConnected_H = true;
		} else {
			rdpMonitor *head, *last;
			/* next, trying sort monitors vertically */
			sort_head_list(ec, compare_monitors_y);
			head = get_first_head_config(ec);
			last = head;
			assert(upperLeftY == head->y);

			/* make sure monitors are horizontally connected each other */
			offsetFromOriginClient = head->y + head->height;
			i = 0;
			wl_list_for_each(iter, &ec->head_list, compositor_link) {
				rdpMonitor *cur = api->head_get_rdpmonitor(iter);

				i++;
				if (i == 1)
					continue;

				if (offsetFromOriginClient != cur->y) {
					weston_log("\tRDP client reported monitors not vertically connected each other at %d (y check)\n", i);
					break;
				}
				offsetFromOriginClient += cur->height;

				if (!is_line_intersected(last->x,
							 last->x + last->width,
							 cur->x,
							 cur->x + cur->width)) {
					weston_log("\tRDP client reported monitors not horizontally connected each other at %d (x check)\n\n", i);
					break;
				}
				last = cur;
			}

			if (i == count) {
				weston_log("\tAll monitors are vertically placed\n");
				isConnected_V = true;
			}
		}
	} else {
		isConnected_H = true;
	}

	if (isScalingUsed && (!isConnected_H && !isConnected_V)) {
		/* scaling can't be supported in complex monitor placement */
		weston_log("\nWARNING\nWARNING\nWARNING: Scaling is used, but can't be supported in complex monitor placement\nWARNING\nWARNING\n");
		isScalingSupported = false;
	}

	if (isScalingUsed && isScalingSupported) {
		uint32_t offsetFromOriginWeston = 0;
		i = 0;

		wl_list_for_each(iter, &ec->head_list, compositor_link) {
			rdpMonitor *head = api->head_get_rdpmonitor(iter);
			int scale = disp_get_output_scale_from_monitor(ec, head);

			rectWeston[i].width = head->width / scale;
			rectWeston[i].height = head->height / scale;
			if (isConnected_H) {
				assert(isConnected_V == false);
				rectWeston[i].x = offsetFromOriginWeston;
				rectWeston[i].y = abs((upperLeftY - head->y) / scale);
				offsetFromOriginWeston += rectWeston[i].width;
			} else {
				assert(isConnected_V == true);
				rectWeston[i].x = abs((upperLeftX - head->x) / scale);
				rectWeston[i].y = offsetFromOriginWeston;
				offsetFromOriginWeston += rectWeston[i].height;
			}
			assert(rectWeston[i].x >= 0);
			assert(rectWeston[i].y >= 0);
			i++;
		}
	} else {
		i = 0;

		struct wet_rdp_params *rdp_params = wet_get_rdp_params(ec);
		if (rdp_params->debug_desktop_scaling_factor)
		{
			float scale = rdp_params->debug_desktop_scaling_factor / 100.0f;

			weston_log("Forcing debug desktop scaling factor [%2f] uniformly on all monitors\n", scale);

			wl_list_for_each(iter, &ec->head_list, compositor_link) {
				rdpMonitor *head = api->head_get_rdpmonitor(iter);

				rectWeston[i].width = head->width / scale;
				rectWeston[i].height = head->height / scale;
				rectWeston[i].x = (head->x + abs(upperLeftX)) / scale;
				rectWeston[i].y = (head->y + abs(upperLeftY)) / scale;
				head->attributes.desktopScaleFactor = rdp_params->debug_desktop_scaling_factor;
				assert(rectWeston[i].x >= 0);
				assert(rectWeston[i].y >= 0);
				i++;
			}
		}
		else
		{
			/* no scaling is used or monitor placement is too complex to scale in weston space, fallback to 1.0f */
			wl_list_for_each(iter, &ec->head_list, compositor_link) {
				rdpMonitor *head = api->head_get_rdpmonitor(iter);

				rectWeston[i].width = head->width;
				rectWeston[i].height = head->height;
				rectWeston[i].x = head->x + abs(upperLeftX);
				rectWeston[i].y = head->y + abs(upperLeftY);
				head->attributes.desktopScaleFactor = 0.0;
				assert(rectWeston[i].x >= 0);
				assert(rectWeston[i].y >= 0);
				i++;
			}
		}
	}

	weston_log("%s:---OUTPUT---\n", __func__);
	i = 0;
	wl_list_for_each(iter, &ec->head_list, compositor_link) {
		rdpMonitor *head = api->head_get_rdpmonitor(iter);
		float client_scale = disp_get_client_scale_from_monitor(ec, head);
		int scale = disp_get_output_scale_from_monitor(ec, head);

		weston_log("	rdpMonitor[%d]: x:%d, y:%d, width:%d, height:%d, is_primary:%d\n",
			i, head->x, head->y,
			   head->width, head->height,
			   head->is_primary);
		weston_log("	rdpMonitor[%d]: weston x:%d, y:%d, width:%d, height:%d\n",
			i, rectWeston[i].x, rectWeston[i].y,
			   rectWeston[i].width, rectWeston[i].height);
		weston_log("	rdpMonitor[%d]: physicalWidth:%d, physicalHeight:%d, orientation:%d\n",
			i, head->attributes.physicalWidth,
			   head->attributes.physicalHeight,
			   head->attributes.orientation);
		weston_log("	rdpMonitor[%d]: desktopScaleFactor:%d, deviceScaleFactor:%d\n",
			i, head->attributes.desktopScaleFactor,
			   head->attributes.deviceScaleFactor);
		weston_log("	rdpMonitor[%d]: scale:%d, clientScale:%3.2f\n",
			i, scale, client_scale);
		i++;
	}

	i = 0;
	wl_list_for_each(iter, &ec->head_list, compositor_link) {
		rdpMonitor *current = api->head_get_rdpmonitor(iter);
		struct weston_output *output = iter->output;
		float client_scale = disp_get_client_scale_from_monitor(ec, current);
		int scale = disp_get_output_scale_from_monitor(ec, current);
		struct wet_rdp_params *rdp_params = wet_get_rdp_params(ec);
		int force_width = rdp_params->default_width;
		int force_height = rdp_params->default_height;
		struct weston_mode new_mode = {};

		assert(output);

		if (!output->enabled) {
			int width = force_width;
			int height = force_height;

			width = width ? width : current->width;
			height = height ? height : current->height;

			/* At startup the backend creates a 0,0 request
			 * If this wasn't overridden by config, just
			 * set it to 640 x 480.
			 */
			width = width ? width : 640;
			height = height ? height : 480;

			new_mode.width = width;
			new_mode.height = height;
			api->output_set_mode(iter->output, &new_mode);
		} else if (force_width && force_height) {
			/* If we had command line parameters, we want to forcibly
			 * set any outputs changed by the backend matching code
			 * back to the forced settings.
			 */
			new_mode.width = force_width;
			new_mode.height = force_height;
			api->output_set_mode(iter->output, &new_mode);
		}
		weston_log("Head mode change:%s NEW width:%d, height:%d, scale:%d, clientScale:%f\n",
			  output->name, current->width,
			  current->height,
			  scale,
			  client_scale);
		if (output->scale != scale) {
			bool was_enabled = false;

			if (output->enabled) {
				weston_output_disable(output);
				was_enabled = true;
			}
			output->scale = 0; /* reset scale first, otherwise assert */
			weston_output_set_scale(output, scale);
			if (was_enabled)
				weston_output_enable(output);
		}

		/* Notify clients for updated resolution/scale. */
		weston_output_set_transform(output, WL_OUTPUT_TRANSFORM_NORMAL);

		/* move output to final location */
		weston_log("move head/output %s (%d,%d) -> (%d,%d)\n",
			iter->name,
			iter->output->x,
			iter->output->y,
			rectWeston[i].x,
			rectWeston[i].y);
		/* Notify clients for updated output position. */
		weston_output_move(iter->output,
			rectWeston[i].x,
			rectWeston[i].y);
		i++;
	}

	/* make sure head list is not empty */
	assert(!wl_list_empty(&ec->head_list));

	BOOL is_primary_found = FALSE;
	i = 0;
	wl_list_for_each(iter, &ec->head_list, compositor_link) {
		rdpMonitor *current = api->head_get_rdpmonitor(iter);

		if (current->is_primary) {
			weston_log("client origin (0,0) is (%d,%d) in Weston space\n",
				rectWeston[i].x,
				rectWeston[i].y);
			/* primary must be at (0,0) in client space */
			assert(current->x == 0);
			assert(current->y == 0);
			/* there must be only one primary */
			assert(is_primary_found == FALSE);
			is_primary_found = TRUE;
		}
		i++;
	}
}
