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

#ifdef HAVE_LIBRSVG2
#include <librsvg/rsvg.h>
#endif // HAVE_LIBRSVG2

#include "shell.h"

#ifdef HAVE_LIBRSVG2
pixman_image_t *
load_image_svg(struct desktop_shell *shell, const void *data, uint32_t data_len, const char *filename)
{
	GError *error = NULL;
	RsvgHandle *rsvg = NULL;
	cairo_surface_t *surface = NULL;
	cairo_t *cr = NULL;
	pixman_image_t *image = NULL;
	cairo_status_t status;

	/* DEPRECATED: g_type_init(); rsvg_init(); */
	/* rsvg_init has been deprecated since version 2.36 and should not be used
	   in newly-written code. Use g_type_init() */
	/* g_type_init has been deprecated since version 2.36 and should not be used
	   in newly-written code. the type system is now initialised automatically. */
	rsvg = rsvg_handle_new_from_data(data, data_len, &error);
	if (!rsvg) {
		shell_rdp_debug(shell, "%s: rsvg_handle_new_from_file failed %s %s\n",
			__func__, filename, error ? error->message : "(no error message)");
		goto Exit;
	}

	RsvgDimensionData dimensionData;
	rsvg_handle_get_dimensions(rsvg, &dimensionData);

	image = pixman_image_create_bits(PIXMAN_a8r8g8b8,
			dimensionData.width,
			dimensionData.height,
			NULL,
			dimensionData.width * 4);
	if (!image) {
		shell_rdp_debug(shell, "%s: pixman_image_create_bits(%dx%d) failed %s\n",
			__func__, dimensionData.width, dimensionData.height, filename);
		goto Exit;
	}

	surface = cairo_image_surface_create_for_data(
			(unsigned char *)pixman_image_get_data(image),
			CAIRO_FORMAT_ARGB32,
			dimensionData.width,
			dimensionData.height,
			dimensionData.width * 4);
	if (!surface) {
		shell_rdp_debug(shell, "%s: cairo_image_surface_create(%dx%d) failed %s\n",
			__func__, dimensionData.width, dimensionData.height, filename);
		goto Exit;
	}

	cr = cairo_create(surface);
	if (!cr) {
		shell_rdp_debug(shell, "%s: cairo_create failed %s\n",
			__func__, filename);
		goto Exit;
	}

	if (!rsvg_handle_render_cairo(rsvg, cr)) {
		shell_rdp_debug(shell, "%s: rsvg_handle_render_cairo failed %s\n",
			__func__, filename);
		goto Exit;
	}

	pixman_image_ref(image);

Exit:
	status = cairo_status(cr);
	if (status != CAIRO_STATUS_SUCCESS) {
		shell_rdp_debug(shell, "%s: cairo status error %s\n",
			__func__, cairo_status_to_string(status));
	}

	if (cr)
		cairo_destroy(cr);

	if (surface)
		cairo_surface_destroy(surface);

	if (image) {
		if (pixman_image_unref(image))
			image = NULL;
	}

	/* DEPRECATED: rsvg_handle_free(rsvg); */
	/* rsvg_handle_free is deprecated and should not be used in
	   newly-written code. Use g_object_unref() instead. */
	if (rsvg)
		g_object_unref(rsvg);

	/* DEPRECATED: rsvg_term(); */
	/* rsvg_term has been deprecated since version 2.36 and should not be used
	   in newly-written code. There is no need to de-initialize librsvg. */

	if (error)
		g_error_free(error);

	return image;
}

void *
load_file_svg(struct desktop_shell *shell, const char *filename, uint32_t *data_len)
{
	FILE *fp;
	void *data = NULL;
	int len, ret;

	fp = fopen(filename, "rb");
	if (!fp) {
		shell_rdp_debug(shell, "%s: fopen failed %s %s\n",
			__func__, filename, strerror(errno));
		goto Fail;
	}

	if (fseek(fp, 0, SEEK_END) != 0) {
		shell_rdp_debug(shell, "%s: fseek failed %s %s\n",
			__func__, filename, strerror(errno));
		goto Fail;
	}
	len = ftell(fp);
	rewind(fp);

	data = malloc(len);
	if (!data) {
		shell_rdp_debug(shell, "%s: malloc(%d) failed %s %s\n",
			__func__, len, filename, strerror(errno));
		goto Fail;
	}

	ret = fread(data, 1, len, fp);
	if (ret != len) {
		shell_rdp_debug(shell, "%s: fread failed, expect %d but returned %d %s %s\n",
			__func__, len, ret, filename, strerror(errno));
		goto Fail;
	}

	goto Exit;

Fail:
	if (data)
		free(data);

	data = NULL;
	len = 0;

Exit:
	if (fp)
		fclose(fp);

	*data_len = len; 

	return data;
}
#else
pixman_image_t *
load_image_svg(struct desktop_shell *, const void *, uint32_t)
{
	return NULL;
}

void *
load_file_svg(struct desktop_shell *, const char *, uint32_t *data_len)
{
	*data_len = 0;
	return NULL;
}
#endif // HAVE_LIBRSVG2
