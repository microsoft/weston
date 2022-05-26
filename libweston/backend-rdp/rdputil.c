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

void assert_compositor_thread(struct rdp_backend *b)
{
	assert(b->compositor_tid == rdp_get_tid());
}

void assert_not_compositor_thread(struct rdp_backend *b)
{
	assert(b->compositor_tid != rdp_get_tid());
}

#ifdef HAVE_FREERDP_GFXREDIR_H
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
		__func__, fd, shared_memory->name, shared_memory->size, addr);

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
	rdp_debug_verbose(b, "%s: freed %d: %s (%ld bytes) at %p\n", __func__,
		shared_memory->fd, shared_memory->name,
		shared_memory->size, shared_memory->addr);

	if (shared_memory->addr) {
		munmap(shared_memory->addr, shared_memory->size);
		shared_memory->addr = NULL;
	}

	if (shared_memory->fd > 0) {
		close(shared_memory->fd);
		shared_memory->fd = -1;
	}
}
#endif // HAVE_FREERDP_GFXREDIR_H

BOOL
rdp_id_manager_init(struct rdp_backend *rdp_backend, struct rdp_id_manager *id_manager, UINT32 low_limit, UINT32 high_limit)
{
	assert_compositor_thread(rdp_backend);

	assert(id_manager->hash_table == NULL);
	assert(low_limit > 0);
	assert(low_limit < high_limit);
	id_manager->rdp_backend = rdp_backend;
	id_manager->id_total = high_limit - low_limit;
	id_manager->id_used = 0;
	id_manager->id_low_limit = low_limit;
	id_manager->id_high_limit = high_limit;
	id_manager->id = low_limit;
	id_manager->hash_table = hash_table_create();
	if (id_manager->hash_table) {
		pthread_mutex_init(&id_manager->mutex, NULL);
		/* by default, pretend mutex is held by compositor thread,
		   so it can be accessed without trigerring assert */
		id_manager->mutex_tid = rdp_backend->compositor_tid;
	} else {
		rdp_debug_error(rdp_backend, "%s: unable to create hash_table.\n", __func__);
	}
	return id_manager->hash_table != NULL;
}

void
rdp_id_manager_free(struct rdp_id_manager *id_manager)
{
	assert_compositor_thread(id_manager->rdp_backend);

	if (id_manager->id_used != 0)
		rdp_debug_error(id_manager->rdp_backend, "%s: possible id leak: %d\n", __func__, id_manager->id_used);
	if (id_manager->hash_table) {
		hash_table_destroy(id_manager->hash_table);
		pthread_mutex_destroy(&id_manager->mutex);
	}
	id_manager->mutex_tid = 0;
	id_manager->hash_table = NULL;
	id_manager->id = 0;
	id_manager->id_low_limit = 0;
	id_manager->id_high_limit = 0;
	id_manager->id_total = 0;
	id_manager->id_used = 0;
	id_manager->rdp_backend = NULL;
}

void
rdp_id_manager_lock(struct rdp_id_manager *id_manager)
{
	assert_not_compositor_thread(id_manager->rdp_backend);

	pthread_mutex_lock(&id_manager->mutex);
	id_manager->mutex_tid = rdp_get_tid();
}

void
rdp_id_manager_unlock(struct rdp_id_manager *id_manager)
{
	assert_not_compositor_thread(id_manager->rdp_backend);

	/* At unlock, restore compositor thread as owner */
	id_manager->mutex_tid = id_manager->rdp_backend->compositor_tid;
	pthread_mutex_unlock(&id_manager->mutex);
}

void *
rdp_id_manager_lookup(struct rdp_id_manager *id_manager, UINT32 id)
{
	/* lookup can be done under compositor thread or after mutex held by rdp_id_manager_lock */
	assert(id_manager->mutex_tid == rdp_get_tid());

	assert(id_manager->hash_table);
	return hash_table_lookup(id_manager->hash_table, id);
}

void
rdp_id_manager_for_each(struct rdp_id_manager *id_manager, hash_table_iterator_func_t func, void *data)
{
	assert_compositor_thread(id_manager->rdp_backend);

	if (!id_manager->hash_table)
		return;

	hash_table_for_each(id_manager->hash_table, func, data);
}

BOOL
rdp_id_manager_allocate_id(struct rdp_id_manager *id_manager, void *object, UINT32 *new_id)
{
	UINT32 id = 0;

	assert_compositor_thread(id_manager->rdp_backend);
	assert(id_manager->hash_table);

	for(;id_manager->id_used < id_manager->id_total;) {
		id = id_manager->id++;
		if (id_manager->id == id_manager->id_high_limit)
			id_manager->id = id_manager->id_low_limit;
		/* Make sure this id is not currently used */
		if (rdp_id_manager_lookup(id_manager, id) == NULL) {
			if (hash_table_insert(id_manager->hash_table, id, object) < 0)
				break;
			/* successfully to reserve new id for given object */
			id_manager->id_used++;
			*new_id = id;
			break;
		}
	}
	return id != 0;
}

void
rdp_id_manager_free_id(struct rdp_id_manager *id_manager, UINT32 id)
{
	assert_compositor_thread(id_manager->rdp_backend);
	assert(id_manager->hash_table);

	pthread_mutex_lock(&id_manager->mutex);
	hash_table_remove(id_manager->hash_table, id);
	pthread_mutex_unlock(&id_manager->mutex);
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

bool
rdp_event_loop_add_fd(struct wl_event_loop *loop, int fd, uint32_t mask, wl_event_loop_fd_func_t func, void *data, struct wl_event_source **event_source)
{
	*event_source = wl_event_loop_add_fd(loop, fd, 0, func, data);
	if (!*event_source) {
		weston_log("%s: wl_event_loop_add_fd failed.\n", __func__);
		return false;
	}

	wl_event_source_fd_update(*event_source, mask);
	return true;
}

void
rdp_dispatch_task_to_display_loop(RdpPeerContext *peerCtx, rdp_loop_task_func_t func, struct rdp_loop_task *task)
{
	/* this function is ONLY used to queue the task from FreeRDP thread,
	   and the task to be processed at wayland display loop thread. */
	assert_not_compositor_thread(peerCtx->rdpBackend);

	task->peerCtx = peerCtx;
	task->func = func;

	pthread_mutex_lock(&peerCtx->loop_task_list_mutex);
	/* this inserts at head */
	wl_list_insert(&peerCtx->loop_task_list, &task->link);
	pthread_mutex_unlock(&peerCtx->loop_task_list_mutex);

	eventfd_write(peerCtx->loop_task_event_source_fd, 1);
}

static int
rdp_dispatch_task(int fd, uint32_t mask, void *arg)
{
	RdpPeerContext *peerCtx = (RdpPeerContext *)arg;
	struct rdp_loop_task *task, *tmp;
	eventfd_t dummy;

	/* this must be called back at wayland display loop thread */
	assert_compositor_thread(peerCtx->rdpBackend);

	eventfd_read(peerCtx->loop_task_event_source_fd, &dummy);

	pthread_mutex_lock(&peerCtx->loop_task_list_mutex);
	/* dequeue the first task which is at last, so use reverse. */
	assert(!wl_list_empty(&peerCtx->loop_task_list));
	wl_list_for_each_reverse_safe(task, tmp, &peerCtx->loop_task_list, link) {
		wl_list_remove(&task->link);
		break;
	}
	pthread_mutex_unlock(&peerCtx->loop_task_list_mutex);

	/* Dispatch and task will be freed by caller. */
	task->func(false, task);

	return 0;
}

bool
rdp_initialize_dispatch_task_event_source(RdpPeerContext *peerCtx)
{
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct wl_event_loop *loop;

	if (pthread_mutex_init(&peerCtx->loop_task_list_mutex, NULL) == -1) {
		rdp_debug_error(b, "%s: pthread_mutex_init failed. %s\n", __func__, strerror(errno));
		goto error_mutex;
	}

	assert(peerCtx->loop_task_event_source_fd == -1);
	peerCtx->loop_task_event_source_fd = eventfd(0, EFD_SEMAPHORE | EFD_CLOEXEC);
	if (peerCtx->loop_task_event_source_fd == -1) {
		rdp_debug_error(b, "%s: eventfd(EFD_SEMAPHORE) failed. %s\n", __func__, strerror(errno));
		goto error_event_source_fd;
	}

	assert(wl_list_empty(&peerCtx->loop_task_list));

	loop = wl_display_get_event_loop(b->compositor->wl_display);
	assert(peerCtx->loop_task_event_source == NULL);
	if (!rdp_event_loop_add_fd(
		loop, peerCtx->loop_task_event_source_fd, WL_EVENT_READABLE,
		rdp_dispatch_task, peerCtx, &peerCtx->loop_task_event_source)) {
		goto error_event_loop_add_fd;
	}

	return true;

error_event_loop_add_fd:
	close(peerCtx->loop_task_event_source_fd);
	peerCtx->loop_task_event_source_fd = -1;

error_event_source_fd:
	pthread_mutex_destroy(&peerCtx->loop_task_list_mutex);

error_mutex:
	return false;
}

void
rdp_destroy_dispatch_task_event_source(RdpPeerContext *peerCtx)
{
	struct rdp_loop_task *task, *tmp;

	/* This function must be called all virtual channel thread at FreeRDP is terminated,
	   that ensures no more incoming tasks. */

	if (peerCtx->loop_task_event_source) {
		wl_event_source_remove(peerCtx->loop_task_event_source);
		peerCtx->loop_task_event_source = NULL;
	}

	wl_list_for_each_reverse_safe(task, tmp, &peerCtx->loop_task_list, link) {
		wl_list_remove(&task->link);
		/* inform caller task is not really scheduled prior to context destruction,
		   inform them to clean them up. */
		task->func(true /* freeOnly */, task);
	}
	assert(wl_list_empty(&peerCtx->loop_task_list));

	if (peerCtx->loop_task_event_source_fd != -1) {
		close(peerCtx->loop_task_event_source_fd);
		peerCtx->loop_task_event_source_fd = -1;
	}

	pthread_mutex_destroy(&peerCtx->loop_task_list_mutex);
}

/* This is a little tricky - it makes sure there's always at least
 * one spare byte in the array in case the caller needs to add a
 * null terminator to it. We can't just null terminate the array
 * here, because some callers won't want that - and some won't
 * like having an odd number of bytes.
 */
int
rdp_wl_array_read_fd(struct wl_array *array, int fd)
{
	int len, size;
	char *data;

	/* Make sure we have at least 1024 bytes of space left */
	if (array->alloc - array->size < 1024) {
		if (!wl_array_add(array, 1024)) {
			errno = ENOMEM;
			return -1;
		}
		array->size -= 1024;
	}
	data = (char *)array->data + array->size;
	/* Leave one char in case the caller needs space for a
	 * null terminator */
	size = array->alloc - array->size - 1;
	do {
		len = read(fd, data, size);
	} while (len == -1 && errno == EINTR);

	if (len == -1)
		return -1;

	array->size += len;

	return len;
}
