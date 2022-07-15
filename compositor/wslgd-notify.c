/*
 * Copyright (c) 2022 Microsoft. All rights reservied.
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

#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <wayland-server.h>

#include "shared/helpers.h"
#include "shared/string-helpers.h"
#include <libweston/zalloc.h>
#include <libweston/libweston.h>
#include "weston.h"

WL_EXPORT int
wet_module_init(struct weston_compositor *compositor,
		int *argc, char *argv[])
{
	struct sockaddr_un addr = {};
	socklen_t size, name_size;
	char *socket_path = getenv("WSLGD_NOTIFY_SOCKET");
	if (!socket_path) {
		weston_log("%s: socket path is not specified\n", __func__);
		return 0;
	}

	int socket_fd = socket(PF_LOCAL, SOCK_SEQPACKET, 0);
	if (socket_fd < 0) {
		weston_log("%s: socket failed\n", __func__);
		return -1;
	}

	addr.sun_family = AF_LOCAL;
	name_size = snprintf(addr.sun_path, sizeof addr.sun_path,
		"%s", socket_path) + 1;
	size = offsetof(struct sockaddr_un, sun_path) + name_size;

	int fd = connect(socket_fd, &addr, size); 
	if (fd < 0) {
		weston_log("%s: connect(%s) failed %s\n", __func__, addr.sun_path, strerror(errno));
		goto close_socket_fd;
	}

	weston_log("%s: socket connected\n", __FILE__);

	close(fd);

close_socket_fd:
	close(socket_fd);

	return 0;
}
