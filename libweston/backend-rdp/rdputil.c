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
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "rdp.h"

pid_t rdp_get_tid()
{
#ifdef SYS_gettid
	return syscall(SYS_gettid);
#else
	return gettid();
#endif
}

static int cached_tm_mday = -1;

static char *
rdp_log_timestamp(char *buf, size_t len)
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

void rdp_debug_print(struct weston_log_scope *log_scope, bool cont, char *fmt, ...)
{
	if (log_scope && weston_log_scope_is_enabled(log_scope)) {
		va_list ap;
		va_start(ap, fmt);
		if (cont) {
			weston_log_scope_vprintf(log_scope, fmt, ap);
		} else {
			char timestr[128];
			int len_va;
			char *str;
			rdp_log_timestamp(timestr, sizeof(timestr));
			len_va = vasprintf(&str, fmt, ap);
			if (len_va >= 0) {
				weston_log_scope_printf(log_scope, "%s %s",
							timestr, str);
				free(str);
			} else {
				const char *oom = "Out of memory";
				weston_log_scope_printf(log_scope, "%s %s",
							timestr, oom);
			}
		}
	}
}

#ifdef ENABLE_RDP_THREAD_CHECK
void assert_compositor_thread(struct rdp_backend *b)
{
	assert(b->compositor_tid == rdp_get_tid());
}
void assert_not_compositor_thread(struct rdp_backend *b)
{
	assert(b->compositor_tid != rdp_get_tid());
}
#endif // ENABLE_RDP_THREAD_CHECK

BOOL
rdp_allocate_shared_memory(struct rdp_backend *b, struct weston_rdp_shared_memory *shared_memory)
{
	int fd = -1;
	void *addr = NULL;
        // + 1 for '/' and + 1 for NULL.
	char path[b->shared_memory_mount_path_size + 1 + RDP_SHARED_MEMORY_NAME_SIZE + 1];

	if (shared_memory->size <= 0) {
		rdp_debug_error(b, "%s: invalid size %ld\n",
			__func__, shared_memory->size);
		goto error_exit;
	}
	// validate page size sysconf(_SC_PAGESIZE);

	// name must be guid format, 32 chars + 4 of '-' + '{' + '}'
	// if not provided, read from kernel.
	if (shared_memory->name[0] == '\0') {
		int fd_uuid = open("/proc/sys/kernel/random/uuid", O_RDONLY);
		if (fd_uuid < 0) {
			rdp_debug_error(b, "%s: open uuid failed with error %s\n",
				__func__, strerror(errno));
			goto error_exit;
		}
		if (read(fd_uuid, &shared_memory->name[1], 32 + 4) < 0) {
			rdp_debug_error(b, "%s: read uuid failed with error %s\n",
				__func__, strerror(errno));
			goto error_exit;
		}
		close(fd_uuid);
		shared_memory->name[0] = '{';
		shared_memory->name[RDP_SHARED_MEMORY_NAME_SIZE - 1] = '}';
		shared_memory->name[RDP_SHARED_MEMORY_NAME_SIZE] = '\0';
	} else if ((strlen(shared_memory->name) != RDP_SHARED_MEMORY_NAME_SIZE) ||
		(shared_memory->name[0] != '{') ||
		(shared_memory->name[RDP_SHARED_MEMORY_NAME_SIZE - 1] != '}') ||
		(shared_memory->name[RDP_SHARED_MEMORY_NAME_SIZE] != '\0')) {
		rdp_debug_error(b, "%s: name is not in GUID form \"%s\"\n",
			__func__, shared_memory->name);
		goto error_exit;
	}

	strcpy(path, b->shared_memory_mount_path);
	strcat(path, "/");
	strcat(path, shared_memory->name);

	fd = open(path, O_CREAT | O_RDWR | O_EXCL, S_IWUSR | S_IRUSR);
	if (fd < 0) {
		rdp_debug_error(b, "%s: Failed to open \"%s\" with error: %s\n",
			__func__, path, strerror(errno));
		goto error_exit;
	}

	if (fallocate(fd, 0, 0, shared_memory->size) < 0) {
		rdp_debug_error(b, "%s: Failed to allocate %d: \"%s\" %ld bytes with error: %s\n",
			__func__, fd, path, shared_memory->size, strerror(errno));
		goto error_exit;
	}

	addr = mmap(NULL, shared_memory->size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		rdp_debug_error(b, "%s: Failed to mmmap %d: \"%s\" %ld bytes with error: %s\n",
			__func__, fd, path, shared_memory->size, strerror(errno));
		goto error_exit;
	}

	rdp_debug_verbose(b, "%s: allocated %d: %s (%ld bytes) at %p\n",
		__func__, fd, shared_memory->name, shared_memory->size, shared_memory->addr);

	shared_memory->fd = fd;
	shared_memory->addr = addr;

	return TRUE;

error_exit:

	if (fd > 0)
		close(fd);

	if (addr)
		munmap(addr, shared_memory->size);

	shared_memory->fd = -1;
	shared_memory->addr = NULL;

	return FALSE;
}

void
rdp_free_shared_memory(struct rdp_backend *b, struct weston_rdp_shared_memory *shared_memory)
{
	if (shared_memory->addr) {
		munmap(shared_memory->addr, shared_memory->size);
		shared_memory->addr = NULL;
	}

	if (shared_memory->fd > 0) {
		close(shared_memory->fd);
		shared_memory->fd = -1;
	}
}

BOOL
rdp_id_manager_init(struct rdp_backend *rdp_backend, struct rdp_id_manager *id_manager, UINT32 low_limit, UINT32 high_limit)
{
	assert(low_limit < high_limit);
	id_manager->rdp_backend = rdp_backend;
	id_manager->id_total = high_limit - low_limit;
	id_manager->id_used = 0;
	id_manager->id_low_limit = low_limit;
	id_manager->id_high_limit = high_limit;
	id_manager->id = low_limit;
	id_manager->hash_table = hash_table_create();
	if (!id_manager->hash_table)
		rdp_debug_error(rdp_backend, "%s: unable to create hash_table.\n", __func__);
	return id_manager->hash_table != NULL;
}

void
rdp_id_manager_free(struct rdp_id_manager *id_manager)
{
	if (id_manager->id_used != 0)
		rdp_debug_error(id_manager->rdp_backend, "%s: possible id leak: %d\n", __func__, id_manager->id_used);
	if (id_manager->hash_table) {
		hash_table_destroy(id_manager->hash_table);
		id_manager->hash_table = NULL;
	}
	id_manager->id = 0;
	id_manager->id_low_limit = 0;
	id_manager->id_high_limit = 0;
	id_manager->id_total = 0;
	id_manager->id_used = 0;
	id_manager->rdp_backend = NULL;
}

BOOL
rdp_id_manager_allocate_id(struct rdp_id_manager *id_manager, void *object, UINT32 *new_id)
{
	UINT32 id = 0;
	for(;id_manager->id_used < id_manager->id_total;) {
		id = id_manager->id++;
		if (id_manager->id == id_manager->id_high_limit)
			id_manager->id = id_manager->id_low_limit;
		/* Make sure this id is not currently used */
		if (hash_table_lookup(id_manager->hash_table, id) == NULL) {
			if (hash_table_insert(id_manager->hash_table, id, object) < 0)
				break;
			/* successfully to reserve new id for given object */
			id_manager->id_used++;
			*new_id = id;
			return TRUE;
		}
	}
	return FALSE;
}

void
rdp_id_manager_free_id(struct rdp_id_manager *id_manager, UINT32 id)
{
	hash_table_remove(id_manager->hash_table, id);
	id_manager->id_used--;
}

void
dump_id_manager_state(FILE *fp, struct rdp_id_manager *id_manager, char* title)
{
	fprintf(fp,"ID Manager status: %s\n", title);
	fprintf(fp,"    current ID: %u\n", id_manager->id);
	fprintf(fp,"    lowest ID: %u\n", id_manager->id_low_limit);
	fprintf(fp,"    hightest ID: %u\n", id_manager->id_high_limit);
	fprintf(fp,"    total IDs: %u\n", id_manager->id_total);
	fprintf(fp,"    used IDs: %u\n", id_manager->id_used);
	fprintf(fp,"\n");
}

/* this function is ONLY used to defer the task from RDP thread,
   to be performed at wayland display loop thread */
struct wl_event_source *
rdp_defer_rdp_task_to_display_loop(RdpPeerContext *peerCtx, wl_event_loop_fd_func_t func, void *data)
{
	if (peerCtx->vcm) {
		struct rdp_backend *b = peerCtx->rdpBackend;
		ASSERT_NOT_COMPOSITOR_THREAD(b);
		struct wl_event_loop *loop = wl_display_get_event_loop(b->compositor->wl_display);
		struct wl_event_source *event_source =
			wl_event_loop_add_fd(loop,
				peerCtx->loop_event_source_fd,
				WL_EVENT_READABLE,
				func, data);
		if (event_source) {
			eventfd_write(peerCtx->loop_event_source_fd, 1);
		} else {
			rdp_debug_error(b, "%s: wl_event_loop_add_idle failed\n", __func__);
		}
		return event_source;
	} else {
		/* RDP server is not opened, this must not be used */
		assert(false);
		return NULL;
	}
}

void
rdp_defer_rdp_task_done(RdpPeerContext *peerCtx)
{
	eventfd_t dummy;
	eventfd_read(peerCtx->loop_event_source_fd, &dummy);
}

