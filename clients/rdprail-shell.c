/*
 * Copyright © 2011 Kristian Høgsberg
 * Copyright © 2011 Collabora, Ltd.
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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <cairo.h>
#include <sys/wait.h>
#include <linux/input.h>
#include <libgen.h>
#include <ctype.h>
#include <time.h>
#include <assert.h>

#include <wayland-client.h>
#include "window.h"
#include "shared/cairo-util.h"
#include <libweston/config-parser.h>
#include "shared/helpers.h"
#include "shared/xalloc.h"
#include <libweston/zalloc.h>
#include "shared/file-util.h"

#include "weston-rdprail-shell-client-protocol.h"

struct focus_proxy_window {
	struct window *window;
	struct widget *widget;
};

struct desktop {
	struct display *display;
	struct weston_rdprail_shell *shell;

	struct focus_proxy_window *focus_proxy_window;
};

static void
focus_proxy_window_redraw_handler(struct widget *widget, void *data)
{
}

static void
focus_proxy_window_resize_handler(struct widget *widget,
				  int32_t width, int32_t height, void *data)
{
}

static struct focus_proxy_window *
focus_proxy_create(struct desktop *desktop)
{
	struct focus_proxy_window *focus_proxy_window = NULL;

	focus_proxy_window = xzalloc(sizeof *focus_proxy_window);
	if (!focus_proxy_window)
		goto error_exit;

	focus_proxy_window->window = window_create(desktop->display);
	if (!focus_proxy_window->window)
		goto error_exit;

	focus_proxy_window->widget = window_add_widget(focus_proxy_window->window, focus_proxy_window);
	if (!focus_proxy_window->widget)
		goto error_exit;

	widget_set_allocation(focus_proxy_window->widget, 0, 0, 0, 0);

	window_set_title(focus_proxy_window->window, "rdprail-shell focus proxy window");
	window_set_user_data(focus_proxy_window->window, focus_proxy_window);

	widget_set_redraw_handler(focus_proxy_window->widget, focus_proxy_window_redraw_handler);
	widget_set_resize_handler(focus_proxy_window->widget, focus_proxy_window_resize_handler);

	struct wl_surface *s = window_get_wl_surface(focus_proxy_window->window);
	weston_rdprail_shell_set_focus_proxy(desktop->shell, s);

	return focus_proxy_window;

error_exit:
	if (focus_proxy_window && focus_proxy_window->widget)
		widget_destroy(focus_proxy_window->widget);

	if (focus_proxy_window && focus_proxy_window->window)
		window_destroy(focus_proxy_window->window);

	if (focus_proxy_window)
		free(focus_proxy_window);

	return NULL;
}

static void
focus_proxy_destroy(struct focus_proxy_window* focus_proxy_window)
{
	widget_destroy(focus_proxy_window->widget);
	window_destroy(focus_proxy_window->window);

	free(focus_proxy_window);
}

static void
global_handler(struct display *display, uint32_t id,
	       const char *interface, uint32_t version, void *data)
{
	struct desktop *desktop = data;

	if (!strcmp(interface, "weston_rdprail_shell")) {
		desktop->shell = display_bind(desktop->display,
					      id,
					      &weston_rdprail_shell_interface,
					      1);
	}
}

static void
sigchild_handler(int s)
{
	int status;
	pid_t pid;

	while (pid = waitpid(-1, &status, WNOHANG), pid > 0)
		fprintf(stderr, "child %d exited\n", pid);
}

int main(int argc, char *argv[])
{
	struct desktop desktop = { 0 };

	desktop.display = display_create(&argc, argv);
	if (desktop.display == NULL) {
		fprintf(stderr, "failed to create display: %s\n",
			strerror(errno));
		return -1;
	}

	display_set_user_data(desktop.display, &desktop);
	display_set_global_handler(desktop.display, global_handler);

	desktop.focus_proxy_window = focus_proxy_create(&desktop);
	if (desktop.focus_proxy_window == NULL) {
		fprintf(stderr, "failed to create focus proxy window.\n");
		return -1;
	}

	signal(SIGCHLD, sigchild_handler);

	display_run(desktop.display);

	focus_proxy_destroy(desktop.focus_proxy_window);
	weston_rdprail_shell_destroy(desktop.shell);
	display_destroy(desktop.display);

	return 0;
}
