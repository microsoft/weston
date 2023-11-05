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

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#include <dirent.h>
#include <assert.h>

#include <libweston/libweston.h>
#include <libweston/config-parser.h>
#include <libweston/backend.h>
#include <libweston/backend-rdp.h>

#include "shell.h"
#include "shared/helpers.h"
#include "shared/image-loader.h"

#if HAVE_GLIB
#include <glib.h>
#endif

#if HAVE_WINPR
#include <winpr/version.h>
#include <winpr/crt.h>
#include <winpr/tchar.h>
#include <winpr/collections.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/file.h>
#endif

#if HAVE_GLIB && HAVE_WINPR

#define NUM_CONTROL_EVENT 6

#define EVENT_TIMEOUT_MS 2000 // 2 seconds
#define MAX_ICON_RETRY_COUNT 5

struct app_list_context {
	wHashTable* table;
	HANDLE thread;
	HANDLE stopEvent;           // control event: wait index 0
	HANDLE startRdpNotifyEvent; // control event: wait index 1
	HANDLE stopRdpNotifyEvent;  // control event: wait index 2
	HANDLE loadIconEvent;       // control event: wait index 3
	HANDLE findImageNameEvent;  // control event: wait index 4
	HANDLE associateWindowAppIdEvent;// control event: wait index 5
	HANDLE replyEvent;
	bool isRdpNotifyStarted;
	bool isAppListNamespaceAttached;
	int app_list_pidfd;
	int weston_pidfd;
	uint32_t icon_retry_count;
	pixman_image_t* default_icon;
	pixman_image_t* default_overlay_icon;
	struct {
		pixman_image_t* image; // use as reply message at load_icon_file.
		const char *key;       // use as send message at load_icon_file.
	} load_icon;
	struct {
		pid_t pid;
		bool is_wayland;
		char *image_name;
		size_t image_name_size;
	} find_image_name;
	struct {
		pid_t pid;
		char *app_id;
		uint32_t window_id;
	} associate_window_app_id;
	struct {
		char requestedClientLanguageId[32]; // 32 = RDPAPPLIST_LANG_SIZE.
		char currentClientLanguageId[32];
	} lang_info;
};

struct app_entry {
	struct desktop_shell *shell;
	char *file;
	char *name;
	char *exec;
	char *try_exec;
	char *working_dir;
	char *icon_name;
	char *icon_file;
	bool is_icon_file_svg;
	pixman_image_t* icon_image;
	uint32_t icon_retry_count;
};

/* list of folders to look for icon in specific orders */
/* TODO: follow icon search path desribed at "Icon Lookup" section at
	 https://specifications.freedesktop.org/icon-theme-spec/icon-theme-spec-latest.html */
char *icon_folder[] = {
	"/usr/share/pixmaps/",
	"/usr/share/icons/hicolor/96x96/apps/",
	"/usr/share/icons/hicolor/128x128/apps/",
	"/usr/share/icons/hicolor/48x48/apps/",
	"/usr/share/icons/hicolor/32x32/apps/",
	"/usr/share/icons/hicolor/24x24/apps/",
	"/usr/share/icons/hicolor/22x22/apps/",
	"/usr/share/icons/hicolor/16x16/apps/",
	"/var/lib/flatpak/exports/share/icons/hicolor/96x96/apps/",
	"/var/lib/flatpak/exports/share/icons/hicolor/128x128/apps/",
	"/var/lib/flatpak/exports/share/icons/hicolor/48x48/apps/",
	"/usr/share/icons/HighContrast/96x96/apps/",
	"/usr/share/icons/HighContrast/128x128/apps/",
	"/usr/share/icons/HighContrast/48x48/apps/",
	"/usr/share/icons/HighContrast/32x32/apps/",
	"/usr/share/icons/HighContrast/24x24/apps/",
	"/usr/share/icons/HighContrast/22x22/apps/",
	"/usr/share/icons/HighContrast/16x16/apps/",
	"/usr/share/icons/hicolor/scalable/apps/", /* use scalable(= svg) only when no png. */
	"/var/lib/flatpak/exports/share/icons/hicolor/scalable/apps/", /* use scalable(= svg) only when no png. */
	"/usr/share/icons/HighContrast/scalable/apps/", /* use scalable (= svg) only when no png. */
};

/* copy chars from 's' to 'd', up to count 'c', ensure 'd' is terminated by '\0' char */
/* returns number of chars copy to 'd' excluding terminating '\0' char */
/* this fucntion does not tell caller if d is truncated */
/* this function assume 's' is accesible up to count 'c' or terminated before count 'c', otherwise may fault */
/* this function does not pad remaining buffer at 'd'. */
static size_t
copy_string(char *d, size_t c, const char *s)
{
	size_t i = 0;

	assert(d);
	assert(c > 0); // c can't be zero or nagative.
	assert(s);

	while (c > 1) {
		if (*s == '\0')
			break;
		*d++ = *s++;
		i++; c--;
	}
	*d = '\0';

	return i;
}

static size_t
append_string(char *d, size_t c, const char *s)
{
	size_t i = 0;

	assert(d);
	assert(c > 0); // c can't be zero or nagative.
	assert(s);

	while (c > 1) {
		if (*d == '\0')
			break;
		d++;
		i++; c--;
	}

	if (c)
		i += copy_string(d, c, s);

	return i;
}

static void
attach_app_list_namespace(struct desktop_shell *shell)
{
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;
	assert(false == context->isAppListNamespaceAttached);
	if (context && context->app_list_pidfd >= 0) {
		assert(context->weston_pidfd >= 0);
		if (setns(context->app_list_pidfd, 0) == -1) {
			shell_rdp_debug_error(shell, "attach_app_list_namespace failed %s\n", strerror(errno));
		} else {
			context->isAppListNamespaceAttached = true;
		}
	}
}

static void
detach_app_list_namespace(struct desktop_shell *shell)
{
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;
	if (context && context->weston_pidfd >= 0 && context->isAppListNamespaceAttached) { 
		if (setns(context->weston_pidfd, 0) == -1) {
			/* TODO: when failed to go back, this is fatal, should terminate weston and restart? */
			shell_rdp_debug_error(shell, "detach_app_list_namespace failed %s\n", strerror(errno));
		} else {
			context->isAppListNamespaceAttached = false;
		}
	}
}

static bool 
is_dir_exist(char *file)
{
	struct stat buffer;
	return ((stat(file, &buffer) == 0) && S_ISDIR(buffer.st_mode));
}

static bool 
is_file_exist(char *file)
{
	struct stat buffer;
	return ((stat(file, &buffer) == 0) && S_ISREG(buffer.st_mode));
}

static char *
is_desktop_file(char *file)
{
	char *ext = strrchr(file, '.');
	if (ext) {
		if (strcmp(ext, ".desktop") == 0)
			return ext;
	}
	return NULL;
}

static bool
find_icon_file(struct app_entry *entry)
{
	struct desktop_shell *shell = entry->shell;
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;
	char buf[512];
	int len;
	char *icon_file = buf;

	assert(entry->icon_name);
	assert(entry->icon_file == NULL);
	assert(entry->icon_retry_count < MAX_ICON_RETRY_COUNT);

	/* if name is absolute path and file presents, use as-is */ 
	if (*entry->icon_name == '/') {
		if (is_file_exist(entry->icon_name)) {
			icon_file = entry->icon_name;
			goto Found;
		}
	} else {
		/* TODO: follow icon search path desribed at "Icon Lookup" section at
		 https://specifications.freedesktop.org/icon-theme-spec/icon-theme-spec-latest.html */
		for (int i = 0; i < (int)ARRAY_LENGTH(icon_folder); i++) {
			copy_string(buf, sizeof buf, icon_folder[i]);
			append_string(buf, sizeof buf, entry->icon_name);
			len = strlen(buf);

			/* first, try given file name as is */
			if (is_file_exist(buf))
				goto Found;

			/* if not found, try again with .png extension appended */
			append_string(buf, sizeof buf, ".png");
			if (is_file_exist(buf))
				goto Found;

#ifdef HAVE_LIBRSVG2
			/* if not found, try again with .svg extension appended */
			copy_string(&buf[len], sizeof buf - len, ".svg");
			if (is_file_exist(buf))
				goto Found;
#endif // HAVE_LIBRSVG2
		}
	}

	if (entry->icon_retry_count++ == 0)
		context->icon_retry_count++;
	else if (entry->icon_retry_count == MAX_ICON_RETRY_COUNT)
		context->icon_retry_count--;
	shell_rdp_debug(entry->shell, "%s: icon (%s) search retry:(%d) global:(%d)\n",
		__func__, entry->icon_name, entry->icon_retry_count, context->icon_retry_count);
	return false;

Found:
	len = strlen(icon_file);
	if (len > 4) {
		/* TODO: file contents should be verified */
		char *ext = &icon_file[len - 4];
		entry->is_icon_file_svg = strcasecmp(ext, ".svg") == 0;
	}

	if (entry->icon_retry_count)
		context->icon_retry_count--;

	entry->icon_file = strdup(icon_file);
	return (entry->icon_file != NULL);
}

static void
free_app_entry(void *arg)
{
	struct app_entry *e = (struct app_entry *)arg;
	if (e) {
		struct desktop_shell *shell = e->shell;
		struct app_list_context *context = (struct app_list_context *)shell->app_list_context;

		shell_rdp_debug(shell, "free_app_entry(): %s: %s\n", e->name, e->file);

		if (e->file) free(e->file);
		if (e->name) free(e->name);
		if (e->exec) free(e->exec);
		if (e->try_exec) free(e->try_exec);
		if (e->working_dir) free(e->working_dir);
		if (e->icon_name) free(e->icon_name);
		if (e->icon_file) free(e->icon_file);
		if (e->icon_image) pixman_image_unref(e->icon_image);
		if (e->icon_retry_count) context->icon_retry_count--;

		free(e);
	}
}

static void
send_app_entry(struct desktop_shell *shell, char *key, struct app_entry *entry,
		bool newApp, bool deleteApp, bool deleteProvider, bool in_sync, bool sync_start, bool sync_end)
{
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;
	struct weston_rdprail_app_list_data app_list_data = {};

	if (!shell->rdprail_api->notify_app_list)
		return;

	app_list_data.newAppId = newApp;
	app_list_data.deleteAppId = deleteApp;
	app_list_data.deleteAppProvider = deleteProvider;
	if (deleteProvider) {
		assert(!in_sync);
		assert(!sync_start);
		assert(!sync_end);
		app_list_data.appProvider = shell->distroName;
	} else if (deleteApp) {
		assert(!in_sync);
		assert(!sync_start);
		assert(!sync_end);
		app_list_data.appProvider = NULL;
		app_list_data.appId = key;
		app_list_data.appGroup = NULL;
	} else {
		/* new or updating app entry */
		assert(entry);
		app_list_data.inSync = in_sync;
		if (in_sync) {
			app_list_data.syncStart = sync_start;
			app_list_data.syncEnd = sync_end;
		} else {
			assert(!sync_start);
			assert(!sync_end);
		}
		app_list_data.appProvider = NULL;
		app_list_data.appId = key;
		app_list_data.appGroup = NULL;
		/* TODO: support "actions" as "tasks" in client side */
		/*       https://specifications.freedesktop.org/desktop-entry-spec/desktop-entry-spec-1.1.html#exec-variables */
		app_list_data.appExecPath = entry->try_exec ? entry->try_exec : entry->exec;
		app_list_data.appWorkingDir = entry->working_dir;
		app_list_data.appDesc = entry->name;
		app_list_data.appIcon = entry->icon_image;
		if (!app_list_data.appIcon)
			app_list_data.appIcon = context->default_icon;
		if (app_list_data.appIcon)
			pixman_image_ref(app_list_data.appIcon);
		if (shell->is_blend_overlay_icon_app_list &&
		    app_list_data.appIcon &&
		    app_list_data.appIcon != context->default_icon &&
		    context->default_overlay_icon)
			shell_blend_overlay_icon(shell,
						 app_list_data.appIcon,
						 context->default_overlay_icon);
	}

	shell->rdprail_api->notify_app_list(shell->rdp_backend, &app_list_data);

	if (app_list_data.appIcon)
		pixman_image_unref(app_list_data.appIcon);
}

static void
send_associate_window_app_id(struct desktop_shell *shell, pid_t pid, char *app_id, uint32_t window_id)
{
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;
	struct weston_rdprail_app_list_data app_list_data = {};
	struct app_entry *entry;
	char *app_exec = NULL;
	char *app_desc = NULL;

	if (!shell->rdprail_api->notify_app_list)
		return;

	entry = (struct app_entry *)HashTable_GetItemValue(context->table, app_id);
	if (entry) {
		app_exec = entry->try_exec ? entry->try_exec : entry->exec;
		app_desc = entry->name;
	}

	if (!app_exec) {
		/*TODO: obtain from /proc/[pid]/cmdline */
	}

	if (!app_desc)
		app_desc = app_id;

	app_list_data.associateWindowId = true;
	app_list_data.appId = app_id;
	app_list_data.appGroup = NULL;
	app_list_data.appExecPath = app_exec;
	app_list_data.appDesc = app_desc;
	app_list_data.appWindowId = window_id;

	shell->rdprail_api->notify_app_list(shell->rdp_backend, &app_list_data);
}

static void
retry_find_icon_file(struct desktop_shell *shell)
{
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;
	struct app_entry *entry;
	char **keys;
	char **cur;
	int num_keys;

	keys = NULL;
	num_keys = HashTable_GetKeys(context->table, (ULONG_PTR**)&keys);
	if (num_keys < 0)
		return;

	cur = keys;
	for (int i = 0; i < num_keys; i++, cur++) {
		entry = (struct app_entry *)HashTable_GetItemValue(context->table, (void *)*cur);
		if (entry &&
		    entry->icon_name &&
		    entry->icon_file == NULL &&
		    entry->icon_retry_count < MAX_ICON_RETRY_COUNT) {
			void *data = NULL;
			uint32_t data_len = 0;
			shell_rdp_debug(entry->shell, "%s: icon (%s) retry count (%d)\n",
				__func__, entry->icon_name, entry->icon_retry_count);
			attach_app_list_namespace(shell);
			if (find_icon_file(entry)) {
				if (entry->is_icon_file_svg) 
					data = load_file_svg(shell, entry->icon_file, &data_len);
				else
					entry->icon_image = load_image(entry->icon_file);
			}
			detach_app_list_namespace(shell);
			if (entry->is_icon_file_svg && data) 
				entry->icon_image = load_image_svg(shell, data, data_len, entry->icon_file);
			if (data)
				free(data);
			if (entry->icon_image)
				send_app_entry(shell, *cur, entry, false, false, false, false, false, false);
		}
	}

	if (keys)
		free(keys);
}

static void
trim_command_exec(char *s)
{
	/* https://specifications.freedesktop.org/desktop-entry-spec/desktop-entry-spec-1.1.html
	 * A command line may contain at most one %f, %u, %F or %U field code.
	 * If the application should not open any file the %f, %u, %F and %U field
	 * codes must be removed from the command line and ignored.
	 */
	char *p = strrchr(s,'%'); 
	if (p) {
		if (*(p+1) == 'f' || *(p+1) == 'u' ||
			*(p+1) == 'F' || *(p+1) == 'U')
			*p = '\0';
	}
}

static bool
update_app_entry(struct desktop_shell *shell, char *file, struct app_entry *entry)
{
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;
	char *lang_id = context->lang_info.currentClientLanguageId;
	char *s;
	GKeyFile *key_file;
	GError *err = NULL;

	key_file = g_key_file_new();
	if (!key_file)
		return false;

	entry->shell = shell;

	entry->file = strdup(file);
	if (!entry->file)
		goto exit;

	attach_app_list_namespace(shell);
	if (!g_key_file_load_from_file(key_file, file, G_KEY_FILE_NONE, &err)) {
		shell_rdp_debug(shell, "desktop file: %s is failed to be loaded: %s\n",
			entry->file, err && err->message ? err->message : "");
		detach_app_list_namespace(shell);
		goto exit;
	}
	detach_app_list_namespace(shell);

	if (!g_key_file_has_group(key_file, G_KEY_FILE_DESKTOP_GROUP)) {
		shell_rdp_debug(shell, "desktop file: %s is missing %s section\n",
			entry->file, G_KEY_FILE_DESKTOP_GROUP);
		goto exit;
	}

	if (g_key_file_get_boolean(key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_HIDDEN, NULL)) {
		shell_rdp_debug(shell, "desktop file: %s is hidden\n",
			entry->file);
		goto exit;
	}

	s = g_key_file_get_string(key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TYPE, NULL);
	if (s) {
		if (strcmp(s, "Application") != 0) {
			shell_rdp_debug(shell, "desktop file: %s is not app (%s)\n", entry->file, s);
			free(s);
			goto exit;
		}
		free(s);
	}
	if (g_key_file_get_boolean(key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NO_DISPLAY, NULL)) {
		shell_rdp_debug(shell, "desktop file: %s has NoDisplay specified\n", entry->file);
		goto exit; // NoDisplay app is not included.
	}
	if (g_key_file_get_boolean(key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TERMINAL, NULL)) {
		shell_rdp_debug(shell, "desktop file: %s is terminal based app\n", entry->file);
		goto exit; // terminal based app is not included.
	}
	/*TODO: OnlyShowIn/NotShowIn support for WSL environment. */
	/*      Need $XDG_CURRENT_DESKTOP keyword for WSL GUI environment */
	s = g_key_file_get_string(key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ONLY_SHOW_IN, NULL);
	if (s) {
		shell_rdp_debug(shell, "desktop file: %s has OnlyShowIn %s\n", entry->file, s);
		free(s);
		goto exit;
	}
	entry->name = g_key_file_get_locale_string(key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, lang_id, NULL);
	if (!entry->name) {
		/* name is required */
		shell_rdp_debug(shell, "desktop file: %s is missing Name key\n", entry->file);
		goto exit;
	}
	if (shell->is_appid_with_distro_name) {
		char *t;
		size_t len = strlen(entry->name);
		/* 4 = ' ' + '(' + ')' + null */
		len += (4 + shell->distroNameLength);
		t = zalloc(len);
		if (t) {
			copy_string(t, len, entry->name);
			append_string(t, len, " (");
			append_string(t, len, shell->distroName);
			append_string(t, len, ")");
			free(entry->name);
			entry->name = t;
		}
	}
	entry->exec = g_key_file_get_string(key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_EXEC, NULL);
	if (!entry->exec) {
		shell_rdp_debug(shell, "desktop file: %s is missing Exec key\n", entry->file);
		goto exit;
	}
	trim_command_exec(entry->exec);
	entry->try_exec = g_key_file_get_string(key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TRY_EXEC, NULL);
	if (entry->try_exec)
		trim_command_exec(entry->try_exec);
	entry->working_dir = g_key_file_get_string(key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_PATH, NULL);
	entry->icon_name = g_key_file_get_locale_string(key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON, lang_id, NULL);
	if (entry->icon_name) {
		void *data = NULL;
		uint32_t data_len = 0;
		attach_app_list_namespace(shell);
		if (find_icon_file(entry)) {
			if (entry->is_icon_file_svg) 
				data = load_file_svg(shell, entry->icon_file, &data_len);
			else
				entry->icon_image = load_image(entry->icon_file);
		}
		detach_app_list_namespace(shell);
		if (entry->is_icon_file_svg && data)
			entry->icon_image = load_image_svg(shell, data, data_len, entry->icon_file);
		if (data)
			free(data);
	}
	g_key_file_free(key_file);

	shell_rdp_debug(shell, "desktop file: %s\n", entry->file);
	shell_rdp_debug(shell, "    Name[%s]:%s\n", lang_id, entry->name);
	shell_rdp_debug(shell, "    Exec:%s\n", entry->exec);
	shell_rdp_debug(shell, "    TryExec:%s\n", entry->try_exec);
	shell_rdp_debug(shell, "    WorkingDir:%s\n", entry->working_dir);
	shell_rdp_debug(shell, "    Icon name:%s\n", entry->icon_name);
	shell_rdp_debug(shell, "    Icon SVG :%d\n", entry->is_icon_file_svg);
	shell_rdp_debug(shell, "    Icon file:%s\n", entry->icon_file);
	shell_rdp_debug(shell, "    Icon image:%p\n", entry->icon_image);

	return true;

exit:
	if (err)
		g_error_free(err);

	g_key_file_free(key_file);

	/* caller will clean up partially filled entry upon returning false */
	return false;
}

static bool
app_list_key_from_file(char *key, size_t key_length, char *file)
{
	char *ext = is_desktop_file(file);
	if (ext) {
		copy_string(key, key_length, file);
		key[ext-file] = '\0'; // drop ".desktop" extention for key.
		/* Despite wayland protocol specification notes below,
		   many applications specify only last component of
		   reserse DNS as app ID, so "FooViewer" only in below example,
		   thus here make only that part as key.
		   [from xdg-shell.xml]
		   <request name="set_app_id">
			The compositor shell will try to group application surfaces together
			by their app ID. As a best practice, it is suggested to select app
			ID's that match the basename of the application's .desktop file.
			For example, "org.freedesktop.FooViewer" where the .desktop file is
			"org.freedesktop.FooViewer.desktop".
		*/
		char *s = strrchr(key, '.');
		if (s &&
			s != key &&
			*(++s) != '\0')
			copy_string(key, key_length, s);
	}
	return ext != NULL;
}

static void
app_list_desktop_file_removed(struct desktop_shell *shell, char *file)
{
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;
	struct app_entry *entry;
	char key[512];

	if (!app_list_key_from_file(key, sizeof key, file))
		return;

	if (context->isRdpNotifyStarted) {
		entry = (struct app_entry *)HashTable_GetItemValue(context->table, (void*)key);
		if (entry)
			send_app_entry(shell, key, entry, false, true, false, false, false, false);
	}

	HashTable_Remove(context->table, key);
}

static void
app_list_desktop_file_changed(struct desktop_shell *shell, char *folder, char *file)
{
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;
	char key[512];
	char full_path[512];
	bool entry_filled = false;
	struct app_entry *entry;
	struct app_entry *entry_old;

	if (!app_list_key_from_file(key, sizeof key, file))
		return;

	copy_string(full_path, sizeof full_path, folder);
	append_string(full_path, sizeof full_path, "/");
	append_string(full_path, sizeof full_path, file);

	entry_old = (struct app_entry *)HashTable_GetItemValue(context->table, (void*)key);

	entry = zalloc(sizeof *entry);
	if (entry)
		entry_filled = update_app_entry(shell, full_path, entry);

	if (entry_filled) {
		shell_rdp_debug(shell, "app list entry updated: Key:%s, Name:%s\n", key, entry->name);
		if (entry_old) {
			if (HashTable_SetItemValue(context->table, key, (void*)entry) < 0) {
				/* failed to update with new entry, remove this desktop entry as data is stale */
				shell_rdp_debug(shell, "app list entry failed to update Key:%s\n", key);
				app_list_desktop_file_removed(shell, file);
				free_app_entry(entry);
			} else {
				free_app_entry(entry_old);
				if (context->isRdpNotifyStarted)
					send_app_entry(shell, key, entry, false, false, false, false, false, false);
			}
		} else {
#if WINPR_VERSION_MAJOR >= 3
			if (HashTable_Insert(context->table, key, (void *)entry) < 0) {
#else
			if (HashTable_Add(context->table, key, (void *)entry) < 0) {
#endif
				shell_rdp_debug(shell, "app list entry failed to insert to hash: Key:%s\n", key);
				free_app_entry(entry);
			} else if (context->isRdpNotifyStarted) {
				send_app_entry(shell, key, entry, true, false, false, false, false, false);
			}
		}
	} else if (entry) {
		shell_rdp_debug(shell, "app list entry failed to update: Key:%s\n", key);
		if (entry_old)
			app_list_desktop_file_removed(shell, file);
		free_app_entry(entry);
	}
}

static void
app_list_update_all(struct desktop_shell *shell, char *app_list_folder[])
{
	char path[512];
	DIR *dir;
	struct dirent *ent;
	char *folder;
	char *home;

	for (int i = 0; app_list_folder[i] != NULL; i++) {
		attach_app_list_namespace(shell);
		folder = app_list_folder[i];
		if (*folder == '~') {
			home = getenv("HOME");
			if (!home)
				continue;
			copy_string(path, sizeof path, home);
			append_string(path, sizeof path, folder+1); // skip '~'.
			folder = path;
		}
		shell_rdp_debug(shell, "app list folder[%d]: %s\n", i, folder);
		dir = opendir(folder);
		detach_app_list_namespace(shell);
		if (dir != NULL) {
			while ((ent = readdir(dir)) != NULL) {
				if (is_desktop_file(ent->d_name))
					app_list_desktop_file_changed(shell, folder, ent->d_name);
			}
			closedir (dir);
		}
	}
}

static void
app_list_start_rdp_notify(struct desktop_shell *shell, char *app_list_folder[])
{
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;
	struct app_entry *entry;
	char **keys;
	char **cur;
	int num_keys;

	if (!shell->rdprail_api->notify_app_list)
		return;

	if (strcmp(context->lang_info.currentClientLanguageId,
			context->lang_info.requestedClientLanguageId) != 0) {
		shell_rdp_debug(shell, "app_list_start_rdp_notify(): client language is changed from %s to %s\n",
				context->lang_info.currentClientLanguageId,
				context->lang_info.requestedClientLanguageId);
		strcpy(context->lang_info.currentClientLanguageId,
			context->lang_info.requestedClientLanguageId);
		/* update with requested language */
		app_list_update_all(shell, app_list_folder);
	}

	keys = NULL;
	num_keys = HashTable_GetKeys(context->table, (ULONG_PTR**)&keys);
	if (num_keys < 0)
		return;

	cur = keys;
	for (int i = 0; i < num_keys; i++, cur++) {
		entry = (struct app_entry *)HashTable_GetItemValue(context->table, (void *)*cur);
		if (entry)
			send_app_entry(shell, *cur, entry, true, false, false, true, (i == 0), (i+1 == num_keys));
	}

	free(keys);
}

static void
app_list_stop_rdp_notify(struct desktop_shell *shell)
{
	send_app_entry(shell, NULL, NULL, false, false, true, false, false, false);
}

static void
translate_to_windows_path(struct desktop_shell *shell, char *image_name, size_t image_name_size)
{
	bool is_succeeded = false;

	attach_app_list_namespace(shell);

	if (shell->use_wslpath && is_file_exist("/usr/bin/wslpath")) {
		pid_t pid;
		int pipe[2] = {};
		int imageNameLength, len;

		pipe2(pipe, O_CLOEXEC); 
		if (!pipe[0] || !pipe[1]) {
			shell_rdp_debug(shell, "app_list_monitor_thread: pipe2 failed: %s\n", strerror(errno));
			goto Exit;
		}

		pid = fork();
		if (pid == -1) {
			shell_rdp_debug(shell, "app_list_monitor_thread: fork() failed: %s\n", strerror(errno));
			close(pipe[0]);
			close(pipe[1]);
			goto Exit;
		}

		if (pid == 0) {
			if (dup2(pipe[1], STDOUT_FILENO) < 0) {
				shell_rdp_debug(shell, "app_list_monitor_thread: dup2 failed: %s\n", strerror(errno));
			} else {
				char *argv[] = {
					"/usr/bin/wslpath",
					"-w",
					image_name,
					0
				};

				close(pipe[0]);
				close(pipe[1]);

				if (execv(argv[0], argv) < 0)
					shell_rdp_debug(shell, "app_list_monitor_thread: execv failed: %s\n", strerror(errno));
			}
			_exit(EXIT_SUCCESS);
		}

		close(pipe[1]);

		imageNameLength = 0;
		while ((len = read(pipe[0], image_name + imageNameLength,
			image_name_size - imageNameLength)) != 0) {
			if (len < 0) {
				shell_rdp_debug(shell, "app_list_monitor_thread: read error: %s\n", strerror(errno));
				/* if already read some, clear it, otherwise leave as-is and fallback. */
				if (imageNameLength) {
					imageNameLength = 0;
					image_name[0] = '\0';
				}
				break;
			}
			imageNameLength += len;
		}

		close(pipe[0]);

		/* trim trailing '\n' */
		while (imageNameLength > 0 &&
			image_name[imageNameLength - 1] == '\n') {
			image_name[imageNameLength - 1] = '\0';
			imageNameLength--;
		}

		if (imageNameLength)
			is_succeeded = true;
	}

Exit:
	detach_app_list_namespace(shell);

	if (!is_succeeded) {
		/* fallback when wslpath doesn't exst, fork/pipe failed, or nothing read,
		   here simply patch '/' with '\'. */
		int i = 0;
		while (image_name[i] != '\0') {
			if (image_name[i] == '/')
				image_name[i] = '\\';
			i++;
		}
	}

	shell_rdp_debug_verbose(shell, "app_list_monitor_thread: Windows image_path:%s\n", image_name);
}

static DWORD WINAPI
app_list_monitor_thread(LPVOID arg)
{
	struct desktop_shell *shell = (struct desktop_shell *)arg;
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;
	struct app_entry *entry;
	/* TODO: obtain additional path from $XDG_DATA_DIRS, default path is defined here
	         but env variable at user distro is not accessible from system-distro,
	         thus, any additional path can be added by WESTON_RDPRAIL_SHELL_APP_LIST_PATH
	         using .wslgconfig */     
	#define CUSTOM_APP_LIST_FOLDER_INDEX 4
	#define MAX_APP_LIST_FOLDER 128
	char *app_list_folder[MAX_APP_LIST_FOLDER] = {
		"/usr/share/applications",
		"/usr/local/share/applications",
		"/var/lib/snapd/desktop/applications",
		"/var/lib/flatpak/exports/share/applications",
		NULL, /* always terminated witn NULL entry */
	};
	int fd[ARRAY_LENGTH(app_list_folder)] = {-1, -1, -1, -1};
	int wd[ARRAY_LENGTH(app_list_folder)] = {-1, -1, -1, -1};
	int app_list_folder_index[ARRAY_LENGTH(app_list_folder)] = {};
	char *home;
	char *folder;
	int len, cur;
	UINT error = 0;
	DWORD status = 0;
	DWORD num_events = 0;
	int num_watch = 0;
	HANDLE events[NUM_CONTROL_EVENT + ARRAY_LENGTH(app_list_folder)] = {};
	struct inotify_event *event;
	char buf[1024 * (sizeof *event + 16)];
	char path[512];

	if (is_system_distro()) {
		char *pidfd_path;
		/* running inside system-distro */
		shell_rdp_debug(shell, "app_list_monitor_thread: running in system-distro with user-distro: %s\n", shell->distroName);

		if (unshare(CLONE_FS) < 0)
			shell_rdp_debug_error(shell, "app_list_monitor_thread: unshare(CLONE_FS) failed %s\n", strerror(errno));

		/* obtain pidfd for current process */
		pidfd_path = "/proc/self/ns/mnt";
		shell_rdp_debug(shell, "app_list_monitor_thread: open(%s)\n", pidfd_path);
		context->weston_pidfd = open(pidfd_path, O_RDONLY | O_CLOEXEC);
		if (context->weston_pidfd < 0) {
			shell_rdp_debug_error(shell, "app_list_monitor_thread: open(%s) failed %s\n", pidfd_path, strerror(errno));
			goto Exit;
		}

		/* obtain pidfd for user-distro */
		pidfd_path = "/proc/2/ns/mnt";
		shell_rdp_debug(shell, "app_list_monitor_thread: open(%s)\n", pidfd_path);
		context->app_list_pidfd = open(pidfd_path, O_RDONLY | O_CLOEXEC);
		if (context->app_list_pidfd < 0) {
			shell_rdp_debug_error(shell, "app_list_monitor_thread: open(%s) failed %s\n", pidfd_path, strerror(errno));
			goto Exit;
		}
	} else {
		shell_rdp_debug(shell, "app_list_monitor_thread: running in user-distro: %s\n", shell->distroName);
	}

	/* fill up with control events first */
	events[num_events++] = context->stopEvent;
	events[num_events++] = context->startRdpNotifyEvent;
	events[num_events++] = context->stopRdpNotifyEvent;
	events[num_events++] = context->loadIconEvent;
	events[num_events++] = context->findImageNameEvent;
	events[num_events++] = context->associateWindowAppIdEvent;
	assert(num_events == NUM_CONTROL_EVENT);

	/* append optional folders */
	folder = getenv("WESTON_RDPRAIL_SHELL_APP_LIST_PATH");
	for (cur = CUSTOM_APP_LIST_FOLDER_INDEX; cur < MAX_APP_LIST_FOLDER-1; cur++) {
		if (folder && *folder != '\0') {
			char *s = strrchr(folder, ':');
			if (s) {
				app_list_folder[cur] = strndup(folder, s-folder);
				folder = s+1;
			} else {
				app_list_folder[cur] = strdup(folder);
				folder = NULL;
			}
		} else {
			break;
		}
	}
	assert(cur < MAX_APP_LIST_FOLDER);
	app_list_folder[cur] = NULL; /* terminated with NULL entry */

	if (shell->rdprail_api->notify_app_list) {
		for (int i = 0; app_list_folder[i] != NULL; i++) {
			fd[num_watch] = inotify_init();
			if (fd[num_watch] < 0) {
				shell_rdp_debug_error(shell, "app_list_monitor_thread: inotify_init[%d] failed %s\n", i, strerror(errno));
				continue;
			}

			attach_app_list_namespace(shell);
			folder = app_list_folder[i];
			if (*folder == '~') {
				home = getenv("HOME");
				if (!home) {
					detach_app_list_namespace(shell);
					close(fd[num_watch]);
					fd[num_watch] = -1;
					continue;
				}
				copy_string(path, sizeof path, home);
				append_string(path, sizeof path, folder+1); // skip '~'.
				folder = path;
			}

			if (!is_dir_exist(folder)) {
				shell_rdp_debug(shell, "app_list_monitor_thread: %s doesn't exist, skipping.\n", folder);
				detach_app_list_namespace(shell);
				close(fd[num_watch]);
				fd[num_watch] = -1;
				continue;
			}

			shell_rdp_debug(shell, "app_list_monitor_thread: inotify_add_watch(%s)\n", folder);
			wd[num_watch] = inotify_add_watch(fd[num_watch], folder, IN_CREATE|IN_DELETE|IN_MODIFY|IN_MOVED_TO|IN_MOVED_FROM);
			if (wd[num_watch] < 0) {
				shell_rdp_debug_error(shell, "app_list_monitor_thread: inotify_add_watch failed: %s\n", strerror(errno));
				detach_app_list_namespace(shell);
				close(fd[num_watch]);
				fd[num_watch] = -1;
				continue;
			}
			detach_app_list_namespace(shell);

			events[num_events] = GetFileHandleForFileDescriptor(fd[num_watch]);
			if (!events[num_events]) {
				shell_rdp_debug_error(shell, "app_list_monitor_thread: GetFileHandleForFileDescriptor failed\n");
				inotify_rm_watch(fd[num_watch], wd[num_watch]);
				wd[num_watch] = -1;
				close(fd[num_watch]);
				fd[num_watch] = -1;
				continue;
			}
			shell_rdp_debug(shell, "app_list_monitor_thread: monitor %s\n", folder);
			app_list_folder_index[num_watch] = i;
			num_events++;
			num_watch++;
		}
		assert(false == context->isAppListNamespaceAttached);

		/* first scan folders to update all existing .desktop files */
		if (num_watch)
			app_list_update_all(shell, app_list_folder);
	}

	/* now loop as changes are made or stop event is signaled */
	while (TRUE) {
		status = WaitForMultipleObjects(num_events, events, FALSE,
			context->icon_retry_count ? EVENT_TIMEOUT_MS : INFINITE);
		if (status == WAIT_FAILED) {
			error = GetLastError();
			break;
		}

		/* winpr doesn't support auto-reset event */
		ResetEvent(events[status - WAIT_OBJECT_0]);

		/* Timeout */
		if (status == WAIT_TIMEOUT) {
			retry_find_icon_file(shell);
			continue;
		}

		/* Stop Event */
		if (status == WAIT_OBJECT_0) {
			shell_rdp_debug(shell, "app_list_monitor_thread: stopEvent is signalled\n");
			break;
		}

		/* Start RDP notify event */
		if (status == WAIT_OBJECT_0 + 1) {
			shell_rdp_debug(shell, "app_list_monitor_thread: startRdpNotifyEvent is signalled. %d - %s\n",
				context->isRdpNotifyStarted, context->lang_info.requestedClientLanguageId);
			if (!context->isRdpNotifyStarted) {
				app_list_start_rdp_notify(shell, app_list_folder);
				context->isRdpNotifyStarted = true;
			}
			continue;
		}

		/* Stop RDP notify event */
		if (status == WAIT_OBJECT_0 + 2) {
			shell_rdp_debug(shell, "app_list_monitor_thread: stopRdpNotifyEvent is signalled. %d\n", context->isRdpNotifyStarted);
			if (context->isRdpNotifyStarted) {
				app_list_stop_rdp_notify(shell);
				context->isRdpNotifyStarted = false;
			}
			SetEvent(context->replyEvent);
			continue;
		}

		/* Load Icon event */
		if (status == WAIT_OBJECT_0 + 3) {
			shell_rdp_debug(shell, "app_list_monitor_thread: loadIconEvent is signalled. %s\n", context->load_icon.key);
			if (context->load_icon.key) {
				entry = (struct app_entry *)HashTable_GetItemValue(context->table, (void*)context->load_icon.key);
				if (entry && entry->icon_image) {
					context->load_icon.image = entry->icon_image;
					pixman_image_ref(context->load_icon.image);
				}
				shell_rdp_debug(shell, "app_list_monitor_thread: entry %p, image %p\n", entry, context->load_icon.image);
			}
			SetEvent(context->replyEvent);
			continue;
		}

		/* Find ImageName event */
		if (status == WAIT_OBJECT_0 + 4) {
			assert(context->find_image_name.image_name);
			assert(context->find_image_name.image_name_size);
			shell_rdp_debug_verbose(shell, "app_list_monitor_thread: findImageNameEvent is signalled. pid:%d\n",
				context->find_image_name.pid);

			/* read execuable name from /proc */
			context->find_image_name.image_name[0] = '\0';
			sprintf(path, "/proc/%d/exe", context->find_image_name.pid);
			if (!context->find_image_name.is_wayland)
				attach_app_list_namespace(shell);
			if (readlink(path, context->find_image_name.image_name, context->find_image_name.image_name_size) < 0)
				shell_rdp_debug(shell, "app_list_monitor_thread: readlink failed %s:%s\n", path, strerror(errno));
			if (!context->find_image_name.is_wayland)
				detach_app_list_namespace(shell);
			shell_rdp_debug_verbose(shell, "app_list_monitor_thread: Linux image_path:%s\n",
				context->find_image_name.image_name);

			/* If image name is provided, convert to Windows-style path. */
			if (context->find_image_name.image_name[0] != '\0') {
				translate_to_windows_path(shell,
					context->find_image_name.image_name,
					context->find_image_name.image_name_size);
			}

			SetEvent(context->replyEvent);
			continue;
		}

		/* Associate Window/AppId event */
		if (status == WAIT_OBJECT_0 + 5) {
			shell_rdp_debug_verbose(shell, "app_list_monitor_thread: associateWindowAppIdEvent is signalled. pid:%d, app_id:%s, window_id:0x%x\n",
				context->associate_window_app_id.pid,
				context->associate_window_app_id.app_id,
				context->associate_window_app_id.window_id);

			send_associate_window_app_id(shell,
				context->associate_window_app_id.pid,
				context->associate_window_app_id.app_id,
				context->associate_window_app_id.window_id);

			SetEvent(context->replyEvent);
			continue;
		}

		/* Somethings are changed in watch folders */
		if (shell->rdprail_api->notify_app_list && num_watch) {
			len = read(fd[status - WAIT_OBJECT_0 - NUM_CONTROL_EVENT], buf, sizeof buf); 
			cur = 0;
			while (cur < len) {
				event = (struct inotify_event *)&buf[cur];
				if (event->len &&
					(event->mask & IN_ISDIR) == 0 &&
					is_desktop_file(event->name)) {
					if (event->mask & (IN_CREATE|IN_MODIFY|IN_MOVED_TO)) {
						shell_rdp_debug(shell, "app_list_monitor_thread: file created/updated (%s)\n", event->name);
						app_list_desktop_file_changed(shell, app_list_folder[app_list_folder_index[status - WAIT_OBJECT_0 - NUM_CONTROL_EVENT]], event->name);
					}
					else if (event->mask & (IN_DELETE|IN_MOVED_FROM)) {
						shell_rdp_debug(shell, "app_list_monitor_thread: file removed (%s)\n", event->name);
						app_list_desktop_file_removed(shell, event->name);
					}
				}
				cur += (sizeof *event + event->len);
			}
		}
	}

Exit:
	assert(false == context->isAppListNamespaceAttached);

	for (int i = 0; i < num_watch; i++) {
		if (events[i + NUM_CONTROL_EVENT])
			CloseHandle(events[i + NUM_CONTROL_EVENT]);
		if (fd[i] != -1) {
			if (wd[i] != -1)
				inotify_rm_watch(fd[i], wd[i]);
			close(fd[i]);
		}
	}

	for (int i = CUSTOM_APP_LIST_FOLDER_INDEX; app_list_folder[i] != NULL; i++) {
		free(app_list_folder[i]);
		app_list_folder[i] = NULL;
	}

	if (context->weston_pidfd >= 0) {
		close(context->weston_pidfd);
		context->weston_pidfd = -1;
	}
	if (context->app_list_pidfd >= 0) {
		close(context->app_list_pidfd);
		context->app_list_pidfd = -1;
	}

	ExitThread(error);
	return error;
}

static void
start_app_list_monitor(struct desktop_shell *shell)
{
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;

	context->isRdpNotifyStarted = false;

	context->weston_pidfd = -1;
	context->app_list_pidfd = -1;

	context->stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!context->stopEvent)
		goto Error_Exit;

	/* bManualReset = TRUE, ideally here needs FALSE, but winpr doesn't support it */
	context->startRdpNotifyEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!context->startRdpNotifyEvent)
		goto Error_Exit;

	/* bManualReset = TRUE, ideally here needs FALSE, but winpr doesn't support it */
	context->stopRdpNotifyEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!context->stopRdpNotifyEvent)
		goto Error_Exit;

	/* bManualReset = TRUE, ideally here needs FALSE, but winpr doesn't support it */
	context->loadIconEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!context->loadIconEvent)
		goto Error_Exit;

	/* bManualReset = TRUE, ideally here needs FALSE, but winpr doesn't support it */
	context->findImageNameEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!context->findImageNameEvent)
		goto Error_Exit;

	/* bManualReset = TRUE, ideally here needs FALSE, but winpr doesn't support it */
	context->associateWindowAppIdEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!context->associateWindowAppIdEvent)
		goto Error_Exit;

	/* bManualReset = TRUE, ideally here needs FALSE, but winpr doesn't support it */
	context->replyEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!context->replyEvent)
		goto Error_Exit;

	context->thread =
		CreateThread(NULL, 0, app_list_monitor_thread, (void*)shell, 0, NULL);
	if (!context->thread)
		goto Error_Exit;

	return;

Error_Exit:
	if (context->replyEvent) {
		CloseHandle(context->replyEvent);
		context->replyEvent = NULL;
	}

	if (context->associateWindowAppIdEvent) {
		CloseHandle(context->associateWindowAppIdEvent);
		context->associateWindowAppIdEvent = NULL;
	}

	if (context->findImageNameEvent) {
		CloseHandle(context->findImageNameEvent);
		context->findImageNameEvent = NULL;
	}

	if (context->loadIconEvent) {
		CloseHandle(context->loadIconEvent);
		context->loadIconEvent = NULL;
	}

	if (context->stopRdpNotifyEvent) {
		CloseHandle(context->stopRdpNotifyEvent);
		context->stopRdpNotifyEvent = NULL;
	}

	if (context->startRdpNotifyEvent) {
		CloseHandle(context->startRdpNotifyEvent);
		context->startRdpNotifyEvent = NULL;
	}

	if (context->stopEvent) {
		CloseHandle(context->stopEvent);
		context->stopEvent = NULL;
	}

	return;
}

static void
stop_app_list_monitor(struct desktop_shell *shell)
{
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;

	if (context->stopRdpNotifyEvent) {
		SetEvent(context->stopRdpNotifyEvent);
		/* wait reply event to make sure worker thread process stopRdpNotify before stopEvent */
		WaitForSingleObject(context->replyEvent, INFINITE);
		/* no need to reset back since event is going to be destroyed */
	}

	if (context->stopEvent)
		SetEvent(context->stopEvent);

	if (context->thread)
		WaitForSingleObject(context->thread, INFINITE);

	if (context->thread) {
		CloseHandle(context->thread);
		context->thread = NULL;
	}

	if (context->replyEvent) {
		CloseHandle(context->replyEvent);
		context->replyEvent = NULL;
	}

	if (context->associateWindowAppIdEvent) {
		CloseHandle(context->associateWindowAppIdEvent);
		context->associateWindowAppIdEvent = NULL;
	}

	if (context->findImageNameEvent) {
		CloseHandle(context->findImageNameEvent);
		context->findImageNameEvent = NULL;
	}

	if (context->loadIconEvent) {
		CloseHandle(context->loadIconEvent);
		context->loadIconEvent = NULL;
	}

	if (context->stopRdpNotifyEvent) {
		CloseHandle(context->stopRdpNotifyEvent);
		context->stopRdpNotifyEvent = NULL;
	}

	if (context->stopEvent) {
		CloseHandle(context->stopEvent);
		context->stopEvent = NULL;
	}

	context->isRdpNotifyStarted = false;

	assert(context->weston_pidfd < 0);
	assert(context->app_list_pidfd < 0);
}
#endif // HAVE_WINPR && HAVE_GLIB

pixman_image_t* app_list_load_icon_file(struct desktop_shell *shell, const char *key)
{
#if HAVE_WINPR && HAVE_GLIB
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;
	pixman_image_t* image = NULL;

	if (context) {
		/* hand off to worker thread where can access user-distro files */
		assert(context->load_icon.image == NULL);
		assert(context->load_icon.key == NULL);
		context->load_icon.key = key;

		/* signal worker thread to load icon at worker thread */
		SetEvent(context->loadIconEvent);
		WaitForSingleObject(context->replyEvent, INFINITE);
		/* here must reset since winpr doesn't support auto reset event */
		ResetEvent(context->replyEvent);

		image = context->load_icon.image;
		context->load_icon.image = NULL;
		context->load_icon.key = NULL;

		return image;
	}
#endif
	return NULL;
}

void app_list_find_image_name(struct desktop_shell *shell, pid_t pid, char *image_name, size_t image_name_size, bool is_wayland)
{
#if HAVE_WINPR && HAVE_GLIB
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;

	if (context) {
		assert(context->find_image_name.pid == (pid_t) 0);
		assert(context->find_image_name.image_name == NULL);
		assert(context->find_image_name.image_name_size == 0);
		context->find_image_name.pid = pid;
		context->find_image_name.is_wayland = is_wayland;
		context->find_image_name.image_name = image_name;
		context->find_image_name.image_name_size = image_name_size;

		/* signal worker thread to load icon at worker thread */
		SetEvent(context->findImageNameEvent);
		WaitForSingleObject(context->replyEvent, INFINITE);
		/* here must reset since winpr doesn't support auto reset event */
		ResetEvent(context->replyEvent);

		context->find_image_name.pid = (pid_t) 0;
		context->find_image_name.is_wayland = false;
		context->find_image_name.image_name = NULL;
		context->find_image_name.image_name_size = 0;
	}
#endif
	return;
}

void app_list_associate_window_app_id(struct desktop_shell *shell, pid_t pid, char *app_id, uint32_t window_id)
{
#if HAVE_WINPR && HAVE_GLIB
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;

	if (context) {
		assert(context->associate_window_app_id.pid == (pid_t) 0);
		assert(context->associate_window_app_id.app_id == NULL);
		assert(context->associate_window_app_id.window_id == 0);
		context->associate_window_app_id.pid = pid;
		context->associate_window_app_id.app_id = app_id;
		context->associate_window_app_id.window_id = window_id;

		/* signal worker thread to load icon at worker thread */
		SetEvent(context->associateWindowAppIdEvent);
		WaitForSingleObject(context->replyEvent, INFINITE);
		/* here must reset since winpr doesn't support auto reset event */
		ResetEvent(context->replyEvent);

		context->associate_window_app_id.pid = (pid_t) 0;
		context->associate_window_app_id.app_id = NULL;
		context->associate_window_app_id.window_id = 0;
	}
#endif
	return;
}

bool app_list_start_backend_update(struct desktop_shell *shell, char *clientLanguageId)
{
#if HAVE_WINPR && HAVE_GLIB
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;
	if (context) {
		if (!clientLanguageId || *clientLanguageId == '\0')
			clientLanguageId = "en_US";
		copy_string(context->lang_info.requestedClientLanguageId,
			sizeof(context->lang_info.requestedClientLanguageId),
			clientLanguageId);
		SetEvent(context->startRdpNotifyEvent);
		return true;
	}
#endif
	return false;
}

void app_list_stop_backend_update(struct desktop_shell *shell)
{
#if HAVE_WINPR && HAVE_GLIB
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;
	if (context) {
		SetEvent(context->stopRdpNotifyEvent);
		WaitForSingleObject(context->replyEvent, INFINITE);
		/* here reset since winpr doesn't support auto reset event */
		ResetEvent(context->replyEvent);
	}
#endif
}

void app_list_init(struct desktop_shell *shell)
{
#if HAVE_WINPR && HAVE_GLIB
	struct app_list_context *context;
	wHashTable* table;
#if WINPR_VERSION_MAJOR >= 3
	wObject* obj;
#endif
	char *iconpath;

	shell->app_list_context = NULL;

	context = (struct app_list_context *)zalloc(sizeof *context);
	if (!context)
		return;

	table = HashTable_New(FALSE /* synchronized */);
	if (!table) {
		free(context);
		return;
	}

#if WINPR_VERSION_MAJOR >= 3
	if (!HashTable_SetupForStringData(table, false)) {
		free(context);
		return;
	}
	obj = HashTable_ValueObject(table);
	obj->fnObjectNew = NULL; // make sure value won't be cloned.
	obj->fnObjectFree = free_app_entry;
#else
	table->hash = HashTable_StringHash;
	table->keyCompare = HashTable_StringCompare;
	table->keyClone = HashTable_StringClone;
	table->keyFree = HashTable_StringFree;
	table->valueClone = NULL; // make sure value won't be cloned.
	table->valueFree = free_app_entry;
#endif

	context->table = table;
	shell->app_list_context = (void *)context;

	/* load default icon */
	iconpath = getenv("WSL2_DEFAULT_APP_ICON");
	if (iconpath && (strcmp(iconpath, "disabled") != 0))
		context->default_icon = load_image(iconpath);

	iconpath = getenv("WSL2_DEFAULT_APP_OVERLAY_ICON");
	if (iconpath && (strcmp(iconpath, "disabled") != 0))
		context->default_overlay_icon = load_image(iconpath);

	/* preblend default icon with overlay icon if requested */
	if (shell->is_blend_overlay_icon_app_list &&
	    context->default_icon &&
	    context->default_overlay_icon)
		shell_blend_overlay_icon(shell,
					 context->default_icon,
					 context->default_overlay_icon);

	/* set default language as "en_US". this will be updated once client connected */
	strcpy(context->lang_info.requestedClientLanguageId, "en_US");
	strcpy(context->lang_info.currentClientLanguageId,
		context->lang_info.requestedClientLanguageId);

	start_app_list_monitor(shell);
#else
	shell->app_list_context = NULL;
#endif // HAVE_WINPR && HAVE_GLIB
}

void app_list_destroy(struct desktop_shell *shell)
{
#if HAVE_WINPR && HAVE_GLIB
	struct app_list_context *context = (struct app_list_context *)shell->app_list_context;
	wHashTable* table;
	int count;

	if (context) {
		table = context->table;

		stop_app_list_monitor(shell);

		if (context->default_overlay_icon)
			pixman_image_unref(context->default_overlay_icon);

		if (context->default_icon)
			pixman_image_unref(context->default_icon);

		HashTable_Clear(table);
		count = HashTable_Count(table);
		assert(count == 0);
		HashTable_Free(table);

		free(context);
		shell->app_list_context = NULL;
	}
#else
	assert(!shell->app_list_context);
#endif // HAVE_WINPR && HAVE_GLIB
}
