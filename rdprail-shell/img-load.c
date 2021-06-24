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

#ifdef HAVE_LIBRSVG2
#include <librsvg/rsvg.h>
#endif // HAVE_LIBRSVG2

#include "shell.h"

#include "shared/image-loader.h"

#ifdef HAVE_LIBRSVG2
static pixman_image_t *
load_svg(struct desktop_shell *shell, const char *filename)
{
	GError *error = NULL;
	RsvgHandle *rsvg = NULL;
	cairo_surface_t *surface = NULL;
	cairo_t *cr = NULL;
	pixman_image_t *image = NULL;

	/* DEPRECATED: g_type_init(); rsvg_init(); */
	/* rsvg_init has been deprecated since version 2.36 and should not be used
	   in newly-written code. Use g_type_init() */
	/* g_type_init has been deprecated since version 2.36 and should not be used
	   in newly-written code. the type system is now initialised automatically. */
	rsvg = rsvg_handle_new_from_file(filename, &error);
	if (!rsvg) {
		shell_rdp_debug(shell, "%s: rsvg_handle_new_from_file failed %s\n",
			__func__, filename);
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

	return image;
}
#endif // HAVE_LIBRSVG2

pixman_image_t *
load_icon_image(struct desktop_shell *shell, const char *filename)
{
	pixman_image_t *image;
	image = load_image(filename);
#ifdef HAVE_LIBRSVG2
	if (!image)
		image = load_svg(shell, filename);
#endif // HAVE_LIBRSVG2
	return image;
}
