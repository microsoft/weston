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
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>

#include "rdp.h"

#include "libweston-internal.h"

#define RDP_INVALID_EVENT_SOURCE ((void*)(-1))

/* From MSDN, RegisterClipboardFormat API.
   Registered clipboard formats are identified by values in the range 0xC000 through 0xFFFF. */
#define CF_PRIVATE_RTF  49309 // fake format ID for "Rich Text Format".
#define CF_PRIVATE_HTML 49405 // fake format ID for "HTML Format".

                                  /*          1           2           3           4         5         6           7         8      */
                                  /*01234567890 1 2345678901234 5 67890123456 7 89012345678901234567890 1 234567890123456789012 3 4*/
char rdp_clipboard_html_header[] = "Version:0.9\r\nStartHTML:-1\r\nEndHTML:-1\r\nStartFragment:00000000\r\nEndFragment:00000000\r\n";
#define RDP_CLIPBOARD_FRAGMENT_START_OFFSET (53) //--------------------------------------------+                       |
#define RDP_CLIPBOARD_FRAGMENT_END_OFFSET (75) //----------------------------------------------------------------------+

/*
 * https://docs.microsoft.com/en-us/windows/win32/dataxchg/html-clipboard-format
 *
 * The fragment should be preceded and followed by the HTML comments and
 * (no space allowed between the !-- and the text) to conveniently
 * indicate where the fragment starts and ends. 
 */
char rdp_clipboard_html_fragment_start[] = "<!--StartFragment-->\r\n";
char rdp_clipboard_html_fragment_end[] = "<!--EndFragment-->\r\n";

struct rdp_clipboard_data_source;

typedef void *(*pfn_process_data)(struct rdp_clipboard_data_source *source, BOOL is_send);

struct rdp_clipboard_supported_format {
	UINT32 index;
	UINT32 format_id;
	char *format_name;
	char *mime_type;
	pfn_process_data pfn;
};

static void *clipboard_process_text(struct rdp_clipboard_data_source *, BOOL);
static void *clipboard_process_bmp(struct rdp_clipboard_data_source *, BOOL);
static void *clipboard_process_html(struct rdp_clipboard_data_source *, BOOL);

struct rdp_clipboard_supported_format clipboard_supported_formats[] = {
	{ 0, CF_UNICODETEXT,  NULL,               "text/plain;charset=utf-8", clipboard_process_text },
	{ 1, CF_DIB,          NULL,               "image/bmp",                clipboard_process_bmp  },
	{ 2, CF_PRIVATE_RTF,  "Rich Text Format", "text/rtf",                 clipboard_process_text }, // same as text
	{ 3, CF_PRIVATE_HTML, "HTML Format",      "text/html",                clipboard_process_html },
};
#define RDP_NUM_CLIPBOARD_FORMATS ARRAY_LENGTH(clipboard_supported_formats)

enum rdp_clipboard_data_source_state {
	RDP_CLIPBOARD_SOURCE_ALLOCATED = 0,
	RDP_CLIPBOARD_SOURCE_FORMATLIST_READY, /* format list is obtained from provider */
	RDP_CLIPBOARD_SOURCE_PUBLISHED, /* availablity of some or none clipboard data is notified to comsumer */
	RDP_CLIPBOARD_SOURCE_REQUEST_DATA, /* data request is sent to provider */
	RDP_CLIPBOARD_SOURCE_RECEIVED_DATA, /* data is received from provider, waiting data to be dispatched to consumer */
	RDP_CLIPBOARD_SOURCE_TRANSFERING, /* transfering data to consumer */
	RDP_CLIPBOARD_SOURCE_TRANSFERRED, /* complete transfering data to comsumer */
	RDP_CLIPBOARD_SOURCE_CANCEL_PENDING, /* data transfer cancel is requested */
	RDP_CLIPBOARD_SOURCE_CANCELED, /* data transfer is canceled */
	RDP_CLIPBOARD_SOURCE_FAILED, /* failure occured */
};

static char *
clipboard_data_source_state_to_string(enum rdp_clipboard_data_source_state state)
{
	switch (state) 
	{
	case RDP_CLIPBOARD_SOURCE_ALLOCATED:
		return "allocated";
	case RDP_CLIPBOARD_SOURCE_FORMATLIST_READY:
		return "format list ready";
	case RDP_CLIPBOARD_SOURCE_PUBLISHED:
		return "published";
	case RDP_CLIPBOARD_SOURCE_REQUEST_DATA:
		return "request data";
	case RDP_CLIPBOARD_SOURCE_RECEIVED_DATA:
		return "received data";
	case RDP_CLIPBOARD_SOURCE_TRANSFERING:
		return "transferring";
	case RDP_CLIPBOARD_SOURCE_TRANSFERRED:
		return "transferred";
	case RDP_CLIPBOARD_SOURCE_CANCEL_PENDING:
		return "cancel pending";
	case RDP_CLIPBOARD_SOURCE_CANCELED:
		return "cenceled";
	case RDP_CLIPBOARD_SOURCE_FAILED:
		return "failed";
	}
	assert(false);
	return "unknown";
}

struct rdp_clipboard_data_source {
	struct weston_data_source base;
	struct wl_event_source *event_source;
	struct wl_array data_contents;
	void *context;
	int refcount;
	int data_source_fd;
	int format_index;
	enum rdp_clipboard_data_source_state state;
	UINT32 inflight_write_count;
	void *inflight_data_to_write;
	size_t inflight_data_size;
	BOOL is_data_processed;
	BOOL is_canceled;
	UINT32 client_format_id_table[RDP_NUM_CLIPBOARD_FORMATS];
};

static void *
clipboard_process_text(struct rdp_clipboard_data_source *source, BOOL is_send)
{
	freerdp_peer *client = (freerdp_peer*)source->context;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct wl_array data_contents;

	wl_array_init(&data_contents);

	if (!source->is_data_processed) {
		if (is_send) {
			/* Linux to Windows (convert utf-8 to UNICODE) */
			/* Include terminating NULL in size */
			assert((source->data_contents.size + 1) <= source->data_contents.alloc);
			assert(((char*)source->data_contents.data)[source->data_contents.size] == '\0');
			source->data_contents.size++;

			/* obtain size in UNICODE */
			size_t data_size = MultiByteToWideChar(CP_UTF8, 0,
				(char*)source->data_contents.data,
				source->data_contents.size,
				NULL, 0);
			if (data_size < 1)
				goto error_return;

			data_size *= 2; // convert to size in bytes.
			if (!wl_array_add(&data_contents, data_size))
				goto error_return;

			/* convert to UNICODE */
			size_t data_size_in_char = MultiByteToWideChar(CP_UTF8, 0,
				(char*)source->data_contents.data,
				source->data_contents.size,
				(LPWSTR)data_contents.data,
				data_size);
			assert(data_contents.size == (data_size_in_char * 2));
		} else {
			/* Windows to Linux (UNICODE to utf-8) */
			LPWSTR data = (LPWSTR)source->data_contents.data;
			size_t data_size_in_char = source->data_contents.size / 2;

			/* Windows's data has trailing chars, which Linux doesn't expect. */
			while(data_size_in_char &&
				((data[data_size_in_char-1] == L'\0') || (data[data_size_in_char-1] == L'\n')))
				data_size_in_char -= 1;
			if (!data_size_in_char)
				goto error_return;

			/* obtain size in utf-8 */
			size_t data_size = WideCharToMultiByte(CP_UTF8, 0,
				(LPCWSTR)source->data_contents.data,
				data_size_in_char,
				NULL, 0,
				NULL, NULL);
			if (data_size < 1)
				goto error_return;

			if (!wl_array_add(&data_contents, data_size))
				goto error_return;

			/* convert to utf-8 */
			data_size = WideCharToMultiByte(CP_UTF8, 0,
				(LPCWSTR)source->data_contents.data,
				data_size_in_char,
				(char*)data_contents.data,
				data_size,
				NULL, NULL);
			assert(data_contents.size == data_size);
		}

		/* swap the data_contents with new one */
		wl_array_release(&source->data_contents);
		source->data_contents = data_contents;
		source->is_data_processed = TRUE;
	}

	rdp_debug_clipboard_verbose(b, "RDP %s (%p:%s): %s (%d bytes)\n",
		__func__, source, clipboard_data_source_state_to_string(source->state),
		is_send ? "send" : "receive", (UINT32)source->data_contents.size);

	return source->data_contents.data;

error_return:

	source->state = RDP_CLIPBOARD_SOURCE_FAILED;
	rdp_debug_clipboard_error(b, "RDP %s FAILED (%p:%s): %s (%d bytes)\n",
		__func__, source, clipboard_data_source_state_to_string(source->state),
		is_send ? "send" : "receive", (UINT32)source->data_contents.size);
	//rdp_debug_clipboard_verbose(b, "RDP clipboard_process_html FAILED (%p): %s \n\"%s\"\n (%d bytes)\n",
	//	source, is_send ? "send" : "receive",
	//	(char *)source->data_contents.data,
	//	(UINT32)source->data_contents.size);

	wl_array_release(&data_contents);

	return NULL;
}

/* based off sample code at https://docs.microsoft.com/en-us/troubleshoot/cpp/add-html-code-clipboard
   But this missing a lot of corner cases, it must be rewritten with use of proper HTML parser */
/* TODO: This doesn't work for converting HTML from Firefox in Wayland mode to Windows in certain case,
   because Firefox sends "<meta http-equiv="content-type" content="text/html; charset=utf-8">...", thus
   here needs to property strip meta header and convert to the Windows clipboard style HTML. */
static void *
clipboard_process_html(struct rdp_clipboard_data_source *source, BOOL is_send)
{
	freerdp_peer *client = (freerdp_peer*)source->context;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct wl_array data_contents;

	wl_array_init(&data_contents);

	if (!source->is_data_processed) {
		char *cur = (char *)source->data_contents.data;
		cur = strstr(cur, "<html");
		if (!cur)
			goto error_return;

		if (!is_send) {
			/* Windows to Linux */
			size_t data_size = source->data_contents.size -
				(cur - (char *)source->data_contents.data);

			/* Windows's data has trailing chars, which Linux doesn't expect. */
			while(data_size && ((cur[data_size-1] == '\0') || (cur[data_size-1] == '\n')))
				data_size -= 1;

			if (!data_size)
				goto error_return;

			if (!wl_array_add(&data_contents, data_size+1)) // +1 for null
				goto error_return;

			memcpy(data_contents.data, cur, data_size);
			((char *)(data_contents.data))[data_size] = '\0';
			data_contents.size = data_size;
		} else {
			/* Linux to Windows */
			char *last, *buf;
			UINT32 fragment_start, fragment_end;

			if (!wl_array_add(&data_contents, source->data_contents.size+200))
				goto error_return;

			buf = (char *)data_contents.data;
			strcpy(buf, rdp_clipboard_html_header);
			last = cur;
			cur = strstr(cur, "<body");
			if (!cur)
				goto error_return;
			cur += 5;
			while(*cur != '>' && *cur != '\0')
				cur++;
			if (*cur == '\0')
				goto error_return;
			cur++; // include '>' 
			strncat(buf, last, cur-last);
			last = cur;
			fragment_start = strlen(buf);
			strcat(buf, rdp_clipboard_html_fragment_start);
			cur = strstr(cur, "</body");
			if (!cur)
				goto error_return;
			strncat(buf, last, cur-last);
			fragment_end = strlen(buf);
			strcat(buf, rdp_clipboard_html_fragment_end);
			strcat(buf, cur);

			cur = buf + RDP_CLIPBOARD_FRAGMENT_START_OFFSET;
			sprintf(cur, "%08u", fragment_start);
			*(cur+8) = '\r';
			cur = buf + RDP_CLIPBOARD_FRAGMENT_END_OFFSET;
			sprintf(cur, "%08u", fragment_end);
			*(cur+8) = '\r';

			data_contents.size = strlen(buf)+1; //+1 for NULL terminate.
		}

		/* swap the data_contents with new one */
		wl_array_release(&source->data_contents);
		source->data_contents = data_contents;
		source->is_data_processed = TRUE;
	}

	rdp_debug_clipboard_verbose(b, "RDP %s (%p:%s): %s (%d bytes)\n",
		__func__, source, clipboard_data_source_state_to_string(source->state),
		is_send ? "send" : "receive", (UINT32)source->data_contents.size);
	//rdp_debug_clipboard_verbose(b, "RDP clipboard_process_html (%p): %s \n\"%s\"\n (%d bytes)\n",
	//	source, is_send ? "send" : "receive",
	//	(char *)source->data_contents.data,
	//	(UINT32)source->data_contents.size);

	return source->data_contents.data;

error_return:

	source->state = RDP_CLIPBOARD_SOURCE_FAILED;
	rdp_debug_clipboard_error(b, "RDP %s FAILED (%p:%s): %s (%d bytes)\n",
		__func__, source, clipboard_data_source_state_to_string(source->state),
		is_send ? "send" : "receive", (UINT32)source->data_contents.size);
	//rdp_debug_clipboard_verbose(b, "RDP clipboard_process_html FAILED (%p): %s \n\"%s\"\n (%d bytes)\n",
	//	source, is_send ? "send" : "receive",
	//	(char *)source->data_contents.data,
	//	(UINT32)source->data_contents.size);

	wl_array_release(&data_contents);

	return NULL;
}

#define DIB_HEADER_MARKER     ((WORD) ('M' << 8) | 'B')
#define DIB_WIDTH_BYTES(bits) ((((bits) + 31) & ~31) >> 3)

static void *
clipboard_process_bmp(struct rdp_clipboard_data_source *source, BOOL is_send)
{
	freerdp_peer *client = (freerdp_peer*)source->context;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	void *ret = NULL;
	BITMAPFILEHEADER *bmfh = NULL;
	BITMAPINFOHEADER *bmih = NULL;
	UINT32 color_table_size = 0;
	size_t original_data_size = source->data_contents.size;
	BOOL was_data_processed = source->is_data_processed;
	struct wl_array data_contents;

	wl_array_init(&data_contents);

	if (is_send) {
		/* Linux to Windows */
		if (source->data_contents.size <= sizeof(BITMAPFILEHEADER))
			goto error_return;
		
		bmfh = (BITMAPFILEHEADER *)source->data_contents.data;
		bmih = (BITMAPINFOHEADER *)(bmfh + 1);
		if (bmih->biCompression == BI_BITFIELDS)
			color_table_size = sizeof(RGBQUAD) * 3;
		else
			color_table_size = sizeof(RGBQUAD) * bmih->biClrUsed;

		/* size must be adjusted only once */
		if (!source->is_data_processed) {
			source->data_contents.size -= sizeof(*bmfh);
			source->is_data_processed = TRUE;
		}

		ret = (void *)bmih; // Skip BITMAPFILEHEADER.
	} else {
		/* Windows to Linux */
		if (!source->is_data_processed) {
			BITMAPFILEHEADER _bmfh = {};
			bmih = (BITMAPINFOHEADER *)source->data_contents.data;
			bmfh = &_bmfh;
			if (bmih->biCompression == BI_BITFIELDS)
				color_table_size = sizeof(RGBQUAD) * 3;
			else
				color_table_size = sizeof(RGBQUAD) * bmih->biClrUsed;

			bmfh->bfType = DIB_HEADER_MARKER;
			bmfh->bfOffBits = sizeof(*bmfh) + bmih->biSize + color_table_size;
			if (bmih->biSizeImage)
				bmfh->bfSize = bmfh->bfOffBits + bmih->biSizeImage;
			else if (bmih->biCompression == BI_BITFIELDS || bmih->biCompression == BI_RGB)
				bmfh->bfSize = bmfh->bfOffBits +
					(DIB_WIDTH_BYTES(bmih->biWidth * bmih->biBitCount) * abs(bmih->biHeight));
			else
				goto error_return;

			if (!wl_array_add(&data_contents, bmfh->bfSize))
				goto error_return;
			assert(data_contents.size == bmfh->bfSize);

			memcpy(data_contents.data, bmfh, sizeof(*bmfh));
			memcpy((char *)data_contents.data + sizeof(*bmfh), source->data_contents.data, bmih->biSizeImage - sizeof(*bmfh));

			/* swap the data_contents with new one */
			wl_array_release(&source->data_contents);
			source->data_contents = data_contents;
			source->is_data_processed = TRUE;

			bmfh = (BITMAPFILEHEADER *)source->data_contents.data;
			bmih = (BITMAPINFOHEADER *)(bmfh + 1);
		} else {
			bmfh = (BITMAPFILEHEADER *)source->data_contents.data;
			bmih = (BITMAPINFOHEADER *)(bmfh + 1);
			if (bmih->biCompression == BI_BITFIELDS)
				color_table_size = sizeof(RGBQUAD) * 3;
			else
				color_table_size = sizeof(RGBQUAD) * bmih->biClrUsed;
		}

		ret = source->data_contents.data;
	}

	assert(ret);
	assert(bmfh);
	assert(bmih);

	rdp_debug_clipboard_verbose(b, "RDP %s (%p:%s): %s (%d bytes)\n",
		__func__, source, clipboard_data_source_state_to_string(source->state),
		is_send ? "send" : "receive",
		(UINT32)source->data_contents.size);
	rdp_debug_clipboard_verbose_continue(b, "    BITMAPFILEHEADER.bfType:0x%x\n", bmfh->bfType);
	rdp_debug_clipboard_verbose_continue(b, "    BITMAPFILEHEADER.bfSize:%d\n", bmfh->bfSize);
	rdp_debug_clipboard_verbose_continue(b, "    BITMAPFILEHEADER.bfOffBits:%d\n", bmfh->bfOffBits);
	rdp_debug_clipboard_verbose_continue(b, "    BITMAPINFOHEADER.biSize:%d\n", bmih->biSize);
	rdp_debug_clipboard_verbose_continue(b, "    BITMAPINFOHEADER.biWidth:%d\n", bmih->biWidth);
	rdp_debug_clipboard_verbose_continue(b, "    BITMAPINFOHEADER.biHeight:%d, y-Up:%s\n", abs(bmih->biHeight), bmih->biHeight < 0 ? "yes" : "no");
	rdp_debug_clipboard_verbose_continue(b, "    BITMAPINFOHEADER.biPlanes:%d\n", bmih->biPlanes);
	rdp_debug_clipboard_verbose_continue(b, "    BITMAPINFOHEADER.biBitCount:%d\n", bmih->biBitCount);
	rdp_debug_clipboard_verbose_continue(b, "    BITMAPINFOHEADER.biCompression:%d\n", bmih->biCompression);
	rdp_debug_clipboard_verbose_continue(b, "    BITMAPINFOHEADER.biSizeImage:%d\n", bmih->biSizeImage);
	rdp_debug_clipboard_verbose_continue(b, "    BITMAPINFOHEADER.biXPelsPerMeter:%d\n", bmih->biXPelsPerMeter);
	rdp_debug_clipboard_verbose_continue(b, "    BITMAPINFOHEADER.biYPelsPerMeter:%d\n", bmih->biYPelsPerMeter);
	rdp_debug_clipboard_verbose_continue(b, "    BITMAPINFOHEADER.biClrUsed:%d\n", bmih->biClrUsed);
	rdp_debug_clipboard_verbose_continue(b, "    BITMAPINFOHEADER.biClrImportant:%d\n", bmih->biClrImportant);
	BITMAPINFO *bmi = (BITMAPINFO *)bmih;
	for (UINT32 i = 0; i < color_table_size / sizeof(RGBQUAD); i++) {
		rdp_debug_clipboard_verbose_continue(b, "    BITMAPINFO.bmiColors[%d]:%02x:%02x:%02x:%02x\n",
					   i,
					  (UINT32)bmi->bmiColors[i].rgbReserved,
					  (UINT32)bmi->bmiColors[i].rgbRed,
					  (UINT32)bmi->bmiColors[i].rgbGreen,
					  (UINT32)bmi->bmiColors[i].rgbBlue);
	}
	if (bmih->biBitCount == 32) {
		DWORD *bits = (DWORD*)((char*)bmfh + bmfh->bfOffBits);
		assert(bits == (DWORD*)(&bmi->bmiColors[color_table_size/sizeof(RGBQUAD)]));
		//for (UINT32 i = 0; i < 4; i++) {
		//	rdp_debug_clipboard_verbose_continue(b, "    %08x %08x %08x %08x %08x %08x %08x %08x\n",
		//		bits[0],bits[1],bits[2],bits[3],bits[4],bits[5],bits[6],bits[7]);
		//	bits += 8;
		//}
	} else if (bmih->biBitCount == 24) {
		BYTE *bits = (BYTE*)bmfh + bmfh->bfOffBits;
		assert(bits == (BYTE*)(&bmi->bmiColors[color_table_size/sizeof(RGBQUAD)]));
		//for (UINT32 i = 0; i < 4; i++) {
		//	rdp_debug_clipboard_verbose_continue(b, "    %02x%02x%02x %02x%02x%02x  %02x%02x%02x  %02x%02x%02x  %02x%02x%02x  %02x%02x%02x  %02x%02x%02x  %02x%02x%02x\n",
		//		bits[ 0],bits[ 1],bits[ 2], bits[ 3],bits[ 4],bits[ 5], bits[ 6],bits[ 7],bits[ 8], bits[ 9],bits[10],bits[11],
		//		bits[12],bits[13],bits[14], bits[15],bits[16],bits[17], bits[18],bits[19],bits[20], bits[21],bits[22],bits[23]);
		//	bits += 24;
		//}
	}
	rdp_debug_clipboard_verbose_continue(b, "    sizeof(BITMAPFILEHEADER):%d\n", (UINT32) sizeof(BITMAPFILEHEADER));
	rdp_debug_clipboard_verbose_continue(b, "    sizeof(BITMAPINFOHEADER):%d\n", (UINT32) sizeof(BITMAPINFOHEADER));
	rdp_debug_clipboard_verbose_continue(b, "    original_data_size:%d\n", (UINT32) original_data_size);
	rdp_debug_clipboard_verbose_continue(b, "    new_data_size:%d\n", (UINT32) source->data_contents.size);
	rdp_debug_clipboard_verbose_continue(b, "    data_processed:%d -> %d\n", was_data_processed, source->is_data_processed);

	return ret;

error_return:

	source->state = RDP_CLIPBOARD_SOURCE_FAILED;
	rdp_debug_clipboard_error(b, "RDP %s FAILED (%p:%s): %s (%d bytes)\n",
		__func__, source, clipboard_data_source_state_to_string(source->state),
		is_send ? "send" : "receive", (UINT32)source->data_contents.size);

	wl_array_release(&data_contents);

	return NULL;
}

static char *
clipboard_format_id_to_string(UINT32 formatId, bool is_server_format_id)
{
	switch (formatId)
	{
		case CF_RAW:
			return "CF_RAW";
		case CF_TEXT:
			return "CF_TEXT";
		case CF_BITMAP:
			return "CF_BITMAP";
		case CF_METAFILEPICT:
			return "CF_METAFILEPICT";
		case CF_SYLK:
			return "CF_SYLK";
		case CF_DIF:
			return "CF_DIF";
		case CF_TIFF:
			return "CF_TIFF";
		case CF_OEMTEXT:
			return "CF_OEMTEX";
		case CF_DIB:
			return "CF_DIB";
		case CF_PALETTE:
			return "CF_PALETTE";
		case CF_PENDATA:
			return "CF_PENDATA";
		case CF_RIFF:
			return "CF_RIFF";
		case CF_WAVE:
			return "CF_WAVE";
		case CF_UNICODETEXT:
			return "CF_UNICODETEXT";
		case CF_ENHMETAFILE:
			return "CF_ENHMETAFILE";
		case CF_HDROP:
			return "CF_HDROP";
		case CF_LOCALE:
			return "CF_LOCALE";
		case CF_DIBV5:
			return "CF_DIBV5";

		case CF_OWNERDISPLAY:
			return "CF_OWNERDISPLAY";
		case CF_DSPTEXT:
			return "CF_DSPTEXT";
		case CF_DSPBITMAP:
			return "CF_DSPBITMAP";
		case CF_DSPMETAFILEPICT:
			return "CF_DSPMETAFILEPICT";
		case CF_DSPENHMETAFILE:
			return "CF_DSPENHMETAFILE";
	}

	if (formatId >= CF_PRIVATEFIRST && formatId <= CF_PRIVATELAST)
		return "CF_PRIVATE";

	if (formatId >= CF_GDIOBJFIRST && formatId <= CF_GDIOBJLAST)
		return "CF_GDIOBJ";

	if (is_server_format_id) {
		if (formatId == CF_PRIVATE_HTML)
			return "CF_PRIVATE_HTML";

		if (formatId == CF_PRIVATE_RTF)
			return "CF_PRIVATE_RTF";
	} else {
		/* From MSDN, RegisterClipboardFormat API.
		   Registered clipboard formats are identified by values in the range 0xC000 through 0xFFFF. */
		if (formatId >= 0xC000 && formatId <= 0xFFFF)
			return "Client side Registered Clipboard Format";
	}

	return "Unknown format";
}

/* find supported index in supported format table by format id from client */
static int
clipboard_find_supported_format_by_format_id(UINT32 format_id)
{
	for (UINT i = 0; i < RDP_NUM_CLIPBOARD_FORMATS; i++) {
		struct rdp_clipboard_supported_format *format = &clipboard_supported_formats[i];
		if (format_id == format->format_id) {
			assert(i == format->index);
			return format->index;
		}
	}
	return -1;
}

/* find supported index in supported format table by format id and name from client */
static int
clipboard_find_supported_format_by_format_id_and_name(UINT32 format_id, const char *format_name)
{
	for (UINT i = 0; i < RDP_NUM_CLIPBOARD_FORMATS; i++) {
		struct rdp_clipboard_supported_format *format = &clipboard_supported_formats[i];
		/* when our supported format table has format name, only format name must match,
		   format id provided from client is ignored (but it may be saved by caller for future use.
		   When our supported format table doesn't have format name, only format id must match,
		   format name (if provided from client) is ignored */
		if ((format->format_name == NULL && format_id == format->format_id) ||
			(format->format_name && format_name && strcmp(format_name, format->format_name) == 0)) {
			assert(i == format->index);
			return format->index;
		}
	}
	return -1;
}

/* find supported index in supported format table by mime */
static int
clipboard_find_supported_format_by_mime_type(const char *mime_type)
{
	for (UINT i = 0; i < RDP_NUM_CLIPBOARD_FORMATS; i++) {
		struct rdp_clipboard_supported_format *format = &clipboard_supported_formats[i];
		if (strcmp(mime_type, format->mime_type) == 0) {
			assert(i == format->index);
			return format->index;
		}
	}
	return -1;
}

static void
clipboard_data_source_unref(struct rdp_clipboard_data_source *source)
{
	freerdp_peer *client = (freerdp_peer*)source->context;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	char **p;

	assert(source->refcount);
	source->refcount--;

	rdp_debug_clipboard(b, "RDP %s (%p:%s): refcount:%d\n",
		__func__, source, clipboard_data_source_state_to_string(source->state), source->refcount);

	if (source->refcount > 0)
		return;

	if (source->event_source)
		wl_event_source_remove(source->event_source);

	if (source->data_source_fd != -1)
		close(source->data_source_fd);

	wl_array_release(&source->data_contents);

	wl_signal_emit(&source->base.destroy_signal,
		       &source->base);

	wl_array_for_each(p, &source->base.mime_types)
		free(*p);

	wl_array_release(&source->base.mime_types);

	free(source);
}

/******************************************\
 * FreeRDP format data response functions *
\******************************************/

/* Inform client data request is succeeded with data */
static UINT
clipboard_client_send_format_data_response(RdpPeerContext *peerCtx, struct rdp_clipboard_data_source *source, void *data, UINT32 size)
{
	struct rdp_backend *b = peerCtx->rdpBackend;
	CLIPRDR_FORMAT_DATA_RESPONSE formatDataResponse = {};

	rdp_debug_clipboard(b, "Client: %s (%p:%s) format_index:%d %s (%d bytes)\n",
		__func__, source, clipboard_data_source_state_to_string(source->state), source->format_index,
		clipboard_supported_formats[source->format_index].mime_type, size);

	formatDataResponse.msgType = CB_FORMAT_DATA_RESPONSE;
	formatDataResponse.msgFlags = CB_RESPONSE_OK;
	formatDataResponse.dataLen = size;
	formatDataResponse.requestedFormatData = data;
	peerCtx->clipboard_server_context->ServerFormatDataResponse(peerCtx->clipboard_server_context, &formatDataResponse);
	/* if here failed to send response, what can we do ? */

	/* now client can send new data request */
	peerCtx->clipboard_data_request_event_source = NULL;

	return 0;
}

/* Inform client data request is failed */
static UINT
clipboard_client_send_format_data_response_fail(RdpPeerContext *peerCtx, struct rdp_clipboard_data_source *source)
{
	struct rdp_backend *b = peerCtx->rdpBackend;
	CLIPRDR_FORMAT_DATA_RESPONSE formatDataResponse = {};

	rdp_debug_clipboard(b, "Client: %s (%p:%s)\n",
		__func__, source, clipboard_data_source_state_to_string(source->state));

	formatDataResponse.msgType = CB_FORMAT_DATA_RESPONSE;
	formatDataResponse.msgFlags = CB_RESPONSE_FAIL;
	formatDataResponse.dataLen = 0;
	formatDataResponse.requestedFormatData = NULL;
	peerCtx->clipboard_server_context->ServerFormatDataResponse(peerCtx->clipboard_server_context, &formatDataResponse);
	/* if here failed to send response, what can we do ? */

	/* now client can send new data request */
	peerCtx->clipboard_data_request_event_source = NULL;

	return 0;
}

/***************************************\
 * Compositor file descritor callbacks *
\***************************************/

/* Send server clipboard data to client when server side application sent them via pipe. */
static int
clipboard_data_source_read(int fd, uint32_t mask, void *arg)
{
	struct rdp_clipboard_data_source *source = (struct rdp_clipboard_data_source *)arg;
	freerdp_peer *client = (freerdp_peer*)source->context;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	char *data;
	int len, size;
	void *data_to_send;

	rdp_debug_clipboard_verbose(b, "RDP %s (%p:%s) fd:%d\n",
		__func__, source, clipboard_data_source_state_to_string(source->state), fd);

	ASSERT_COMPOSITOR_THREAD(b);

	assert(source->data_source_fd == fd);

	/* event source is not removed here, but it will be removed when read is completed,
	   until it's completed this function will be called whenever next chunk of data is
	   available for read in pipe. */
	assert(source->event_source);

	/* if buffer is less than 1024 bytes remaining, request another 1024 bytes minimum */
	/* but actual reallocated buffer size will be increased by ^2 */
	if (source->data_contents.alloc - source->data_contents.size < 1024) {
		if (!wl_array_add(&source->data_contents, 1024)) {
			goto error_exit;
		}
		source->data_contents.size -= 1024;
	}

	source->state = RDP_CLIPBOARD_SOURCE_TRANSFERING;
	data = (char*)source->data_contents.data + source->data_contents.size;
	size = source->data_contents.alloc - source->data_contents.size - 1; // -1 leave space for NULL-terminate.
	len = read(fd, data, size);
	if (len == 0) {
		/* all data from source is read, so completed. */
		source->state = RDP_CLIPBOARD_SOURCE_TRANSFERRED;
		rdp_debug_clipboard(b, "RDP %s (%p:%s): read completed (%ld bytes)\n",
			__func__, source, clipboard_data_source_state_to_string(source->state), source->data_contents.size);
		if (!source->data_contents.size)
			goto error_exit;
		/* process data before sending to client */
		if (clipboard_supported_formats[source->format_index].pfn)
			data_to_send = clipboard_supported_formats[source->format_index].pfn(source, TRUE);
		else
			data_to_send = source->data_contents.data;
		/* send clipboard data to client */
		if (data_to_send)
			clipboard_client_send_format_data_response(peerCtx, source, data_to_send, source->data_contents.size);
		else
			goto error_exit;
		/* make sure this is the last reference, so event source is removed at unref */
		assert(source->refcount == 1);
		clipboard_data_source_unref(source);
	} else if (len < 0) {
		source->state = RDP_CLIPBOARD_SOURCE_FAILED;
		rdp_debug_clipboard_error(b, "RDP %s (%p:%s) read failed (%s)\n",
			__func__, source, clipboard_data_source_state_to_string(source->state), strerror(errno));
		goto error_exit;
	} else {
		source->data_contents.size += len;
		((char*)source->data_contents.data)[source->data_contents.size] = '\0';
		rdp_debug_clipboard_verbose(b, "RDP %s (%p:%s) read (%zu bytes)\n",
			__func__, source, clipboard_data_source_state_to_string(source->state), source->data_contents.size);
	}
	return 0;

error_exit:
	clipboard_client_send_format_data_response_fail(peerCtx, source);
	/* make sure this is the last reference, so event source is removed at unref */
	assert(source->refcount == 1);
	clipboard_data_source_unref(source);
	return 0; 
}

/* Send client's clipboard data to the requesting application at server side */
static int
clipboard_data_source_write(int fd, uint32_t mask, void *arg)
{
	struct rdp_clipboard_data_source *source = (struct rdp_clipboard_data_source *)arg;
	freerdp_peer *client = (freerdp_peer *) source->context;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct weston_seat *seat = peerCtx->item.seat;
	struct wl_event_loop *loop = wl_display_get_event_loop(seat->compositor->wl_display);
	void *data_to_write;
	size_t data_size;
	ssize_t size;

	rdp_debug_clipboard_verbose(b, "RDP %s (%p:%s) fd:%d\n", __func__,
		source, clipboard_data_source_state_to_string(source->state), fd);

	ASSERT_COMPOSITOR_THREAD(b);

	assert(source->data_source_fd == fd);
	assert(source == peerCtx->clipboard_inflight_client_data_source);

	/* remove event source now, and if write is failed with EAGAIN, queue back to display loop. */ 
	wl_event_source_remove(source->event_source);
	source->event_source = NULL;

	if (source->is_canceled == FALSE && source->data_contents.data && source->data_contents.size) {
		if (source->inflight_data_to_write) {
			assert(source->inflight_data_size);
			rdp_debug_clipboard_verbose(b, "RDP %s (%p:%s) retry write retry count:%d\n",
				__func__, source, clipboard_data_source_state_to_string(source->state), source->inflight_write_count);
			data_to_write = source->inflight_data_to_write;
			data_size = source->inflight_data_size;
		} else {
			fcntl(source->data_source_fd, F_SETFL, O_WRONLY | O_NONBLOCK);
			if (clipboard_supported_formats[source->format_index].pfn)
				data_to_write = clipboard_supported_formats[source->format_index].pfn(source, FALSE);
			else
				data_to_write = source->data_contents.data;
			data_size = source->data_contents.size;
		}
		while (data_to_write && data_size) {
			source->state = RDP_CLIPBOARD_SOURCE_TRANSFERING;
			size = write(fd, data_to_write, data_size);
			if (size <= 0) {
				if (errno != EAGAIN) {
					source->state = RDP_CLIPBOARD_SOURCE_FAILED;
					rdp_debug_clipboard_error(b, "RDP %s (%p:%s) write failed %s\n",
						__func__, source, clipboard_data_source_state_to_string(source->state), strerror(errno));
					break;
				}
				source->inflight_data_to_write = data_to_write;
				source->inflight_data_size = data_size;
				source->inflight_write_count++;
				source->event_source =
					wl_event_loop_add_fd(loop, source->data_source_fd, WL_EVENT_WRITABLE,
							clipboard_data_source_write, source);
				if (!source->event_source) {
					source->state = RDP_CLIPBOARD_SOURCE_FAILED;
					rdp_debug_clipboard_error(b, "RDP %s (%p:%s) wl_event_loop_add_fd failed\n",
						__func__, source, clipboard_data_source_state_to_string(source->state));
					break;
				}
				return 0;
			} else {
				assert(data_size >= (size_t)size);
				data_size -= size;
				data_to_write = (char *)data_to_write + size;
				rdp_debug_clipboard_verbose(b, "RDP %s (%p:%s) wrote %ld bytes, remaining %ld bytes\n",
					__func__, source, clipboard_data_source_state_to_string(source->state), size, data_size);
				if (!data_size) {
					source->state = RDP_CLIPBOARD_SOURCE_TRANSFERRED;
					rdp_debug_clipboard(b, "RDP %s (%p:%s) write completed (%ld bytes)\n",
						__func__, source, clipboard_data_source_state_to_string(source->state), source->data_contents.size);
				}
			}
		}
	} else if (source->is_canceled) {
		source->state = RDP_CLIPBOARD_SOURCE_CANCELED;
		rdp_debug_clipboard_verbose(b, "RDP %s (%p:%s)\n",
			__func__, source, clipboard_data_source_state_to_string(source->state));
	}

	close(source->data_source_fd);
	source->data_source_fd = -1;
	source->inflight_write_count = 0;
	source->inflight_data_to_write = NULL;
	source->inflight_data_size = 0;
	clipboard_data_source_unref(source);
	peerCtx->clipboard_inflight_client_data_source = NULL;

	return 0;
}

/***********************************\
 * Clipboard data-device callbacks *
\***********************************/

/* data-device informs the given data format is accepted */
static void
clipboard_data_source_accept(struct weston_data_source *base,
		   uint32_t time, const char *mime_type)
{
	struct rdp_clipboard_data_source *source = (struct rdp_clipboard_data_source *)base;
	freerdp_peer *client = (freerdp_peer*)source->context;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;

	rdp_debug(b, "RDP %s (base:%p) mime-type:\"%s\"\n", __func__, base, mime_type);
}

/* data-device informs the application requested the specified format data in given data_source (= client's clipboard) */
static void
clipboard_data_source_send(struct weston_data_source *base,
		 const char *mime_type, int32_t fd)
{
	struct rdp_clipboard_data_source *source = (struct rdp_clipboard_data_source *)base;
	freerdp_peer *client = (freerdp_peer*)source->context;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct weston_seat *seat = peerCtx->item.seat;
	struct wl_event_loop *loop = wl_display_get_event_loop(seat->compositor->wl_display);
	CLIPRDR_FORMAT_DATA_REQUEST formatDataRequest = {};
	int index;

	rdp_debug_clipboard(b, "RDP %s (%p:%s) fd:%d, mime-type:\"%s\"\n",
		__func__, source, clipboard_data_source_state_to_string(source->state), fd, mime_type);

	ASSERT_COMPOSITOR_THREAD(b);

	if (peerCtx->clipboard_inflight_client_data_source) {
		/* Here means server side (Linux application) request clipboard data,
		   but server hasn't completed with previous request yet.
		   If this happens, punt to idle loop and reattempt. */
		source->state = RDP_CLIPBOARD_SOURCE_FAILED;
		rdp_debug_clipboard_error(b, "\n\n\nRDP %s (%p:%s) vs (%p): outstanding RDP data request (client to server)\n\n\n",
			__func__, source, clipboard_data_source_state_to_string(source->state), peerCtx->clipboard_inflight_client_data_source);
		goto error_return_close_fd;
	}

	if (source->base.mime_types.size == 0) {
		source->state = RDP_CLIPBOARD_SOURCE_TRANSFERRED;
		rdp_debug_clipboard(b, "RDP %s (%p:%s) source has no data\n",
			__func__, source, clipboard_data_source_state_to_string(source->state));
		goto error_return_close_fd;
	}

	index = clipboard_find_supported_format_by_mime_type(mime_type);
	if (index >= 0 &&			/* check supported by this RDP bridge */
		source->client_format_id_table[index]) {	/* check supported by current data source from client */
		peerCtx->clipboard_inflight_client_data_source = source;
		source->refcount++; // reference while request inflight.
		source->data_source_fd = fd;
		assert(source->inflight_write_count == 0);
		assert(source->inflight_data_to_write == NULL);
		assert(source->inflight_data_size == 0);
		if (index == source->format_index) {
			/* data is already in data_contents, no need to pull from client */
			assert(source->event_source == NULL);
			source->state = RDP_CLIPBOARD_SOURCE_RECEIVED_DATA;
			source->event_source =
				wl_event_loop_add_fd(loop, source->data_source_fd, WL_EVENT_WRITABLE,
						clipboard_data_source_write, source);
			if (!source->event_source) {
				source->state = RDP_CLIPBOARD_SOURCE_FAILED;
				rdp_debug_clipboard_error(b, "RDP %s (%p:%s) wl_event_loop_add_fd failed\n",
					__func__, source, clipboard_data_source_state_to_string(source->state));
				goto error_return_unref_source;
			}
		} else {
			/* purge cached data */
			wl_array_release(&source->data_contents);
			wl_array_init(&source->data_contents);
			source->is_data_processed = FALSE;
			/* update requesting format property */
			source->format_index = index;
			/* request clipboard data from client */
			formatDataRequest.msgType = CB_FORMAT_DATA_REQUEST;
			formatDataRequest.dataLen = 4;
			formatDataRequest.requestedFormatId = source->client_format_id_table[index];
			source->state = RDP_CLIPBOARD_SOURCE_REQUEST_DATA;
			rdp_debug_clipboard(b, "RDP %s (%p:%s) request \"%s\" index:%d formatId:%d %s\n",
				__func__, source, clipboard_data_source_state_to_string(source->state), mime_type, index,
				formatDataRequest.requestedFormatId,
				clipboard_format_id_to_string(formatDataRequest.requestedFormatId, false));
			if (peerCtx->clipboard_server_context->ServerFormatDataRequest(peerCtx->clipboard_server_context, &formatDataRequest) != 0)
				goto error_return_unref_source;
		}
	} else {
		source->state = RDP_CLIPBOARD_SOURCE_FAILED;
		rdp_debug_clipboard_error(b, "RDP %s (%p:%s) specified format \"%s\" index:%d formatId:%d is not supported by client\n",
			__func__, source, clipboard_data_source_state_to_string(source->state),
			mime_type, index, source->client_format_id_table[index]);
		goto error_return_close_fd;
	}

	return;

error_return_unref_source:
	source->data_source_fd = -1;
	assert(source->inflight_write_count == 0);
	assert(source->inflight_data_to_write == NULL);
	assert(source->inflight_data_size == 0);
	clipboard_data_source_unref(source);
	assert(peerCtx->clipboard_inflight_client_data_source == source);
	peerCtx->clipboard_inflight_client_data_source = NULL;

error_return_close_fd:
	close(fd);

	return;
}

/* data-device informs the given data source is not longer referenced by compositor */
static void
clipboard_data_source_cancel(struct weston_data_source *base)
{
	struct rdp_clipboard_data_source *source = (struct rdp_clipboard_data_source *) base;
	freerdp_peer *client = (freerdp_peer*)source->context;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;

	rdp_debug_clipboard(b, "RDP %s (%p:%s)\n",
		__func__, source, clipboard_data_source_state_to_string(source->state));

	ASSERT_COMPOSITOR_THREAD(b);

	if (source == peerCtx->clipboard_inflight_client_data_source) {
		source->is_canceled = TRUE;
		source->state = RDP_CLIPBOARD_SOURCE_CANCEL_PENDING;
		rdp_debug_clipboard(b, "RDP %s (%p:%s): still inflight\n",
			__func__, source, clipboard_data_source_state_to_string(source->state));
		assert(source->refcount > 1);
	} else {
		/* everything outside of the base has to be cleaned up */
		source->state = RDP_CLIPBOARD_SOURCE_CANCELED;
		rdp_debug_clipboard_verbose(b, "RDP %s (%p:%s)\n",
			__func__, source, clipboard_data_source_state_to_string(source->state));
		assert(source->event_source == NULL);
		wl_array_release(&source->data_contents);
		wl_array_init(&source->data_contents);
		source->is_data_processed = FALSE;
		source->format_index = -1;
		memset(source->client_format_id_table, 0, sizeof(source->client_format_id_table));
		source->inflight_write_count = 0;
		source->inflight_data_to_write = NULL;
		source->inflight_data_size = 0;
		if (source->data_source_fd != -1) {
			close(source->data_source_fd);
			source->data_source_fd = -1;
		}
	}
}

/**********************************\
 * Compositor idle loop callbacks *
\**********************************/

/* Publish client's available clipboard formats to compositor (make them visible to applications in server) */
static void
clipboard_data_source_publish(void *arg)
{
	struct rdp_clipboard_data_source *source = (struct rdp_clipboard_data_source *)arg;
	freerdp_peer *client = (freerdp_peer*)source->context;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct rdp_clipboard_data_source *source_prev;

	rdp_debug_clipboard(b, "RDP %s (%p:%s)\n",
		__func__, source, clipboard_data_source_state_to_string(source->state));

	ASSERT_COMPOSITOR_THREAD(b);

	/* here is going to publish new data, if previous data from us is still referenced,
	   unref it after selection */
	source_prev = peerCtx->clipboard_client_data_source;
	peerCtx->clipboard_client_data_source = source;

	source->event_source = NULL;
	source->base.accept = clipboard_data_source_accept;
	source->base.send = clipboard_data_source_send;
	source->base.cancel = clipboard_data_source_cancel;
	source->state = RDP_CLIPBOARD_SOURCE_PUBLISHED;
	weston_seat_set_selection(peerCtx->item.seat, &source->base,
				wl_display_next_serial(b->compositor->wl_display));

	if (source_prev)
		clipboard_data_source_unref(source_prev);

	return;
}

/* Request the specified clipboard data from data-device at server side */
static void
clipboard_data_source_request(void *arg)
{
	RdpPeerContext *peerCtx = (RdpPeerContext *)arg;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct weston_seat *seat = peerCtx->item.seat;
	struct weston_data_source *selection_data_source = seat->selection_data_source;
	struct wl_event_loop *loop = wl_display_get_event_loop(seat->compositor->wl_display);
	struct rdp_clipboard_data_source *source = NULL;
	int p[2] = {};
	const char *requested_mime_type, **mime_type;
	int index;
	BOOL found_requested_format;

	ASSERT_COMPOSITOR_THREAD(b);

	/* set to invalid, so it still validate incoming request, but won't free event source at error. */
	peerCtx->clipboard_data_request_event_source = RDP_INVALID_EVENT_SOURCE;

	index = peerCtx->clipboard_last_requested_format_index;
	assert(index >= 0 && index < (int)RDP_NUM_CLIPBOARD_FORMATS);
	requested_mime_type = clipboard_supported_formats[index].mime_type;
	rdp_debug_clipboard(b, "RDP %s (base:%p) requested mime type:\"%s\"\n",
		__func__, selection_data_source, requested_mime_type);

	found_requested_format = FALSE;
	wl_array_for_each(mime_type, &selection_data_source->mime_types) {
		rdp_debug_clipboard(b, "RDP %s (base:%p) available formats: %s\n",
			__func__, selection_data_source, *mime_type);
		if (strcmp(requested_mime_type, *mime_type) == 0) {
			found_requested_format = TRUE;
			break;
		}
	}
	if (!found_requested_format) {
		rdp_debug_clipboard(b, "RDP %s (base:%p) requested format not found format:\"%s\"\n",
			__func__, selection_data_source, requested_mime_type);
		goto error_exit_response_fail;
	}

	source = zalloc(sizeof *source);
	if (!source)
		goto error_exit_response_fail;

	/* By now, the server side data availablity is already notified
	   to client by clipboard_set_selection(). */
	source->state = RDP_CLIPBOARD_SOURCE_PUBLISHED;
	rdp_debug_clipboard(b, "RDP %s (%p:%s)\n",
		__func__, source, clipboard_data_source_state_to_string(source->state));
	wl_signal_init(&source->base.destroy_signal);
	wl_array_init(&source->base.mime_types);
	wl_array_init(&source->data_contents);
	source->is_data_processed = FALSE;
	source->context = (void*)peerCtx->item.peer;
	source->refcount = 1; // decremented when data sent to client.
	source->data_source_fd = -1;
	source->format_index = index;

	if (pipe2(p, O_CLOEXEC) == -1)
		goto error_exit_free_source;

	source->data_source_fd = p[0];

	/* Request data from data source */
	source->state = RDP_CLIPBOARD_SOURCE_REQUEST_DATA;
	selection_data_source->send(selection_data_source, requested_mime_type, p[1]);
	/* p[1] should be closed by data source */

	/* wait until data is ready on pipe */
	source->event_source =
		wl_event_loop_add_fd(loop, p[0], WL_EVENT_READABLE,
					clipboard_data_source_read, source);
	if (!source->event_source) {
		source->state = RDP_CLIPBOARD_SOURCE_FAILED;
		rdp_debug_clipboard_error(b, "RDP %s (%p:%s) wl_event_loop_add_fd failed.\n",
			__func__, source, clipboard_data_source_state_to_string(source->state));
		goto error_exit_free_source;
	}

	return;

error_exit_free_source:
	clipboard_data_source_unref(source);

error_exit_response_fail:
	clipboard_client_send_format_data_response_fail(peerCtx, NULL);

	return;
}

/*************************************\
 * Compositor notification callbacks *
\*************************************/

/* Compositor notify new clipboard data is going to be copied to clipboard, and its supported formats */
static void
clipboard_set_selection(struct wl_listener *listener, void *data)
{
	RdpPeerContext *peerCtx = 
		container_of(listener, RdpPeerContext, clipboard_selection_listener);
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct weston_seat *seat = data;
	struct weston_data_source *selection_data_source = seat->selection_data_source;
	CLIPRDR_FORMAT_LIST formatList = {};
	CLIPRDR_FORMAT format[RDP_NUM_CLIPBOARD_FORMATS] = {};
	const char **mime_type;
	int index, num_supported_format = 0, num_avail_format = 0;

	rdp_debug_clipboard(b, "RDP %s (base:%p)\n", __func__, selection_data_source);

	ASSERT_COMPOSITOR_THREAD(b);

	if (selection_data_source == NULL) {
		return;
	} else if (selection_data_source->accept == clipboard_data_source_accept) {
		/* Callback for our data source. */
		return;
	}

	/* another data source (from server side) gets selected,
	   no longer need previous data from us */
	if (peerCtx->clipboard_client_data_source) {
		clipboard_data_source_unref(peerCtx->clipboard_client_data_source);
		peerCtx->clipboard_client_data_source = NULL;
	}

	wl_array_for_each(mime_type, &selection_data_source->mime_types) {
		rdp_debug_clipboard(b, "RDP %s (base:%p) available formats[%d]: %s\n",
			__func__, selection_data_source, num_avail_format, *mime_type);
		num_avail_format++;
	}

	/* check supported clipboard formats */
	wl_array_for_each(mime_type, &selection_data_source->mime_types) {
		index = clipboard_find_supported_format_by_mime_type(*mime_type);
		if (index >= 0) {
			format[num_supported_format].formatId = clipboard_supported_formats[index].format_id;
			format[num_supported_format].formatName = clipboard_supported_formats[index].format_name;
			rdp_debug_clipboard(b, "RDP %s (base:%p) supported formats[%d]: %d: %s\n",
				__func__,
				selection_data_source,
				num_supported_format,
				format[num_supported_format].formatId,
				format[num_supported_format].formatName ? \
					format[num_supported_format].formatName : \
					clipboard_format_id_to_string(format[num_supported_format].formatId, true));
			num_supported_format++;
		}
	}

	if (num_supported_format) {
		/* let client knows formats are available in server clipboard */
		formatList.msgType = CB_FORMAT_LIST;
		formatList.numFormats = num_supported_format;
		formatList.formats = &format[0];
		peerCtx->clipboard_server_context->ServerFormatList(peerCtx->clipboard_server_context, &formatList);
	} else {
		rdp_debug_clipboard(b, "RDP %s (base:%p) no supported formats\n", __func__, selection_data_source);
	}

	return;
}

/*********************\
 * FreeRDP callbacks *
\*********************/

/* client reports the path of temp folder */
static UINT
clipboard_client_temp_directory(CliprdrServerContext* context, const CLIPRDR_TEMP_DIRECTORY* tempDirectory)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	rdp_debug_clipboard(b, "Client: %s %s\n", __func__, tempDirectory->szTempDir);
	return 0;
}

/* client reports thier clipboard capabilities */
static UINT
clipboard_client_capabilities(CliprdrServerContext* context, const CLIPRDR_CAPABILITIES* capabilities)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	rdp_debug_clipboard(b, "Client: clipboard capabilities: cCapabilitiesSet:%d\n", capabilities->cCapabilitiesSets);
	for (UINT32 i = 0; i < capabilities->cCapabilitiesSets; i++) {
		CLIPRDR_CAPABILITY_SET* capabilitySets = &capabilities->capabilitySets[i];
		switch (capabilitySets->capabilitySetType) {
			case CB_CAPSTYPE_GENERAL:
			{
				CLIPRDR_GENERAL_CAPABILITY_SET *generalCapabilitySet = (CLIPRDR_GENERAL_CAPABILITY_SET *)capabilitySets;
				rdp_debug_clipboard(b, "Client: clipboard capabilities[%d]: General\n", i);
				rdp_debug_clipboard(b, "    Version:%d\n", generalCapabilitySet->version);
				rdp_debug_clipboard(b, "    GeneralFlags:0x%x\n", generalCapabilitySet->generalFlags);
				if (generalCapabilitySet->generalFlags & CB_USE_LONG_FORMAT_NAMES)
					rdp_debug_clipboard(b, "        CB_USE_LONG_FORMAT_NAMES\n");
				if (generalCapabilitySet->generalFlags & CB_STREAM_FILECLIP_ENABLED)
					rdp_debug_clipboard(b, "        CB_STREAM_FILECLIP_ENABLED\n");
				if (generalCapabilitySet->generalFlags & CB_FILECLIP_NO_FILE_PATHS)
					rdp_debug_clipboard(b, "        CB_FILECLIP_NO_FILE_PATHS\n");
				if (generalCapabilitySet->generalFlags & CB_CAN_LOCK_CLIPDATA)
					rdp_debug_clipboard(b, "        CB_CAN_LOCK_CLIPDATA\n");
				break;
			}
			default:
				return -1;
		}
	}
	return 0;
}

/* client reports the supported format list in client's clipboard */
static UINT
clipboard_client_format_list(CliprdrServerContext* context, const CLIPRDR_FORMAT_LIST* formatList)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct rdp_clipboard_data_source *source = NULL;
	BOOL isPublished = FALSE;
	char **p, *s;

	ASSERT_NOT_COMPOSITOR_THREAD(b);

	rdp_debug_clipboard(b, "Client: %s clipboard format list: numFormats:%d\n", __func__, formatList->numFormats);
	for (UINT32 i = 0; i < formatList->numFormats; i++) {
		CLIPRDR_FORMAT* format = &formatList->formats[i];
		rdp_debug_clipboard(b, "Client: %s clipboard formats[%d]: formatId:%d, formatName:%s\n",
			__func__, i, format->formatId,
			format->formatName ? format->formatName : clipboard_format_id_to_string(format->formatId, false));
	}

	source = zalloc(sizeof *source);
	if (source) {
		source->state = RDP_CLIPBOARD_SOURCE_ALLOCATED;
		rdp_debug_clipboard(b, "Client: %s (%p:%s) allocated\n",
			__func__, source, clipboard_data_source_state_to_string(source->state));
		wl_signal_init(&source->base.destroy_signal);
		wl_array_init(&source->base.mime_types);
		wl_array_init(&source->data_contents);
		source->context = (void*) client;
		source->refcount = 1; // decremented when another source is selected.
		source->data_source_fd = -1;
		source->format_index = -1;

		for (UINT32 i = 0; i < formatList->numFormats; i++) {
			CLIPRDR_FORMAT* format = &formatList->formats[i];
			int index = clipboard_find_supported_format_by_format_id_and_name(format->formatId, format->formatName);
			if (index >= 0) {
				/* save format id given from client, client can handle its own format id for private format. */
				source->client_format_id_table[index] = format->formatId; 
				s = strdup(clipboard_supported_formats[index].mime_type);
				if (s) {
					p = wl_array_add(&source->base.mime_types, sizeof *p);
					if (p) {
						rdp_debug_clipboard(b, "Client: %s (%p:%s) mine_type:\"%s\" index:%d formatId:%d\n",
							__func__, source, clipboard_data_source_state_to_string(source->state),
							s, index, format->formatId);
						*p = s;
					} else {
						rdp_debug_clipboard(b, "Client: %s (%p:%s) wl_array_add failed\n",
							__func__, source, clipboard_data_source_state_to_string(source->state));
						free(s);
					}
				} else {
					rdp_debug_clipboard(b, "Client: %s (%p:%s) strdup failed\n",
						__func__, source, clipboard_data_source_state_to_string(source->state));
				}
			}
		}

		if (formatList->numFormats != 0 &&
		    source->base.mime_types.size == 0) {
			rdp_debug_clipboard(b, "Client: %s (%p:%s) no formats are supported\n",
				__func__, source, clipboard_data_source_state_to_string(source->state));
		}

		source->state = RDP_CLIPBOARD_SOURCE_FORMATLIST_READY;
		source->event_source =
			rdp_defer_rdp_task_to_display_loop(peerCtx,
				clipboard_data_source_publish,
				source);
		if (source->event_source) {
			isPublished = TRUE;
		} else {
			source->state = RDP_CLIPBOARD_SOURCE_FAILED;
			rdp_debug_clipboard_error(b, "Client: %s (%p:%s) rdp_defer_rdp_task_to_display_loop failed\n",
				__func__, source, clipboard_data_source_state_to_string(source->state));
		}
	}

	CLIPRDR_FORMAT_LIST_RESPONSE formatListResponse = {};
	formatListResponse.msgType = CB_FORMAT_LIST_RESPONSE;
	formatListResponse.msgFlags = source ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
	formatListResponse.dataLen = 0;
	if (peerCtx->clipboard_server_context->ServerFormatListResponse(peerCtx->clipboard_server_context, &formatListResponse) != 0) {
		source->state = RDP_CLIPBOARD_SOURCE_FAILED;
		rdp_debug_clipboard_error(b, "Client: %s (%p:%s) ServerFormatListResponse failed\n",
			__func__, source, clipboard_data_source_state_to_string(source->state));
		return -1;
	}

	if (!isPublished && source)
		clipboard_data_source_unref(source);

	return 0;
}

/* client responded with clipboard data asked by server */
static UINT
clipboard_client_format_data_response(CliprdrServerContext* context, const CLIPRDR_FORMAT_DATA_RESPONSE* formatDataResponse)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct wl_event_loop *loop = wl_display_get_event_loop(b->compositor->wl_display);
	struct rdp_clipboard_data_source *source = peerCtx->clipboard_inflight_client_data_source;
	BOOL Success = FALSE;

	rdp_debug_clipboard(b, "Client: %s (%p:%s) flags:%d, dataLen:%d\n",
		__func__, source, clipboard_data_source_state_to_string(source->state),
		formatDataResponse->msgFlags, formatDataResponse->dataLen);

	ASSERT_NOT_COMPOSITOR_THREAD(b);

	if (source) {
		if (source->event_source || (source->inflight_write_count != 0)) {
			/* here means client responded more than once for single data request */
			source->state = RDP_CLIPBOARD_SOURCE_FAILED;
			rdp_debug_clipboard_error(b, "Client: %s (%p:%s) middle of write loop:%p, %d\n",
				__func__, source, clipboard_data_source_state_to_string(source->state),
				source->event_source, source->inflight_write_count);
			return -1;
		}

		if (formatDataResponse->msgFlags == CB_RESPONSE_OK) {
			/* Recieved data from client, cache to data source */
			if (wl_array_add(&source->data_contents, formatDataResponse->dataLen+1)) {
				memcpy(source->data_contents.data,
					formatDataResponse->requestedFormatData,
					formatDataResponse->dataLen);
				source->data_contents.size = formatDataResponse->dataLen;
				/* regardless data type, make sure it ends with NULL */
				((char*)source->data_contents.data)[source->data_contents.size] = '\0';
				/* data is ready, waiting to be written to destination */
				source->state = RDP_CLIPBOARD_SOURCE_RECEIVED_DATA;
				Success = TRUE;
			} else {
				source->state = RDP_CLIPBOARD_SOURCE_FAILED;
			}
		} else {
			source->state = RDP_CLIPBOARD_SOURCE_FAILED;
		}
		rdp_debug_clipboard_verbose(b, "Client: %s (%p:%s)\n",
			__func__, source, clipboard_data_source_state_to_string(source->state));

		if (Success) {
			assert(source->event_source == NULL);
			source->event_source =
				wl_event_loop_add_fd(loop, source->data_source_fd, WL_EVENT_WRITABLE,
						clipboard_data_source_write, source);
			if (!source->event_source) {
				source->state = RDP_CLIPBOARD_SOURCE_FAILED;
				rdp_debug_clipboard_error(b, "Client: %s (%p:%s) wl_event_loop_add_fd failed\n",
					__func__, source, clipboard_data_source_state_to_string(source->state));
			}
		}

		if (!source->event_source) {
			wl_array_release(&source->data_contents);
			wl_array_init(&source->data_contents);
			source->is_data_processed = FALSE;
			source->format_index = -1;
			memset(source->client_format_id_table, 0, sizeof(source->client_format_id_table));
			assert(source->inflight_write_count == 0);
			assert(source->inflight_data_to_write == NULL);
			assert(source->inflight_data_size == 0);
			close(source->data_source_fd);
			source->data_source_fd = -1;
			clipboard_data_source_unref(source);
			peerCtx->clipboard_inflight_client_data_source = NULL;
		}
	} else {
		rdp_debug_clipboard(b, "Client: %s client send data without server asking. protocol error", __func__);
		return -1;
	}

	return 0;
}

/* client responded on the format list sent by server */
static UINT
clipboard_client_format_list_response(CliprdrServerContext* context, const CLIPRDR_FORMAT_LIST_RESPONSE* formatListResponse)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	rdp_debug_clipboard(b, "Client: %s msgFlags:0x%x\n", __func__, formatListResponse->msgFlags);
	return 0;
}

/* client requested the data of specificed format in server clipboard */
static UINT
clipboard_client_format_data_request(CliprdrServerContext* context, const CLIPRDR_FORMAT_DATA_REQUEST* formatDataRequest)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	int index;

	rdp_debug_clipboard(b, "Client: %s requestedFormatId:%d - %s\n",
			__func__, formatDataRequest->requestedFormatId,
			clipboard_format_id_to_string(formatDataRequest->requestedFormatId, true));

	ASSERT_NOT_COMPOSITOR_THREAD(b);

	if (peerCtx->clipboard_data_request_event_source) {
		rdp_debug_clipboard_error(b, "Client: %s (outstanding event:%p) client requests data while server hasn't responded previous request yet. protocol error.\n",
			__func__, peerCtx->clipboard_data_request_event_source);
		return -1;
	}

	/* Make sure clients requested the format we knew */
	index = clipboard_find_supported_format_by_format_id(formatDataRequest->requestedFormatId);
	if (index >= 0) {
		peerCtx->clipboard_last_requested_format_index = index;
		peerCtx->clipboard_data_request_event_source =
			rdp_defer_rdp_task_to_display_loop(peerCtx, clipboard_data_source_request, peerCtx);
		if (!peerCtx->clipboard_data_request_event_source) {
			rdp_debug_clipboard_error(b, "Client: %s rdp_defer_rdp_task_to_display_loop failed\n", __func__);
			goto error_return;
		}
	} else {
		rdp_debug_clipboard_error(b, "Client: %s client requests data format the server never reported in format list response. protocol error.\n", __func__);
		return -1;
	}

	return 0;

error_return:
	/* send FAIL response to client */
	if (clipboard_client_send_format_data_response_fail(peerCtx, NULL) != 0)
		return -1;
	return 0;
}

/********************\
 * Public functions *
\********************/

int 
rdp_clipboard_init(freerdp_peer* client)
{
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct weston_seat *seat = peerCtx->item.seat;
	char *s;

	assert(seat);

	ASSERT_COMPOSITOR_THREAD(b);

	b->debugClipboard = weston_log_ctx_add_log_scope(b->compositor->weston_log_ctx,
							 "rdp-backend-clipboard",
							 "Debug messages from RDP backend clipboard\n",
							  NULL, NULL, NULL);
	if (b->debugClipboard) {
		s = getenv("WESTON_RDP_DEBUG_CLIPBOARD_LEVEL");
		if (s) {
			if (!safe_strtoint(s, &b->debugClipboardLevel))
				b->debugClipboardLevel = RDP_DEBUG_CLIPBOARD_LEVEL_DEFAULT;
			else if (b->debugClipboardLevel > RDP_DEBUG_LEVEL_VERBOSE)
				b->debugClipboardLevel = RDP_DEBUG_LEVEL_VERBOSE;
		} else {
			/* by default, clipboard scope is disabled, so when it's enabled,
			   log with verbose mode to assist debugging */
			b->debugClipboardLevel = RDP_DEBUG_LEVEL_VERBOSE; // RDP_DEBUG_CLIPBOARD_LEVEL_DEFAULT;
		}
	}
	rdp_debug_clipboard(b, "RDP backend: WESTON_RDP_DEBUG_CLIPBOARD_LEVEL: %d\n", b->debugClipboardLevel);

	peerCtx->clipboard_server_context = cliprdr_server_context_new(peerCtx->vcm);
	if (!peerCtx->clipboard_server_context) 
		goto error;

	peerCtx->clipboard_server_context->custom = (void *)client;
	peerCtx->clipboard_server_context->TempDirectory = clipboard_client_temp_directory;
	peerCtx->clipboard_server_context->ClientCapabilities = clipboard_client_capabilities;
	peerCtx->clipboard_server_context->ClientFormatList = clipboard_client_format_list;
	peerCtx->clipboard_server_context->ClientFormatListResponse = clipboard_client_format_list_response;
	//peerCtx->clipboard_server_context->ClientLockClipboardData
	//peerCtx->clipboard_server_context->ClientUnlockClipboardData
	peerCtx->clipboard_server_context->ClientFormatDataRequest = clipboard_client_format_data_request;
	peerCtx->clipboard_server_context->ClientFormatDataResponse = clipboard_client_format_data_response;
	//peerCtx->clipboard_server_context->ClientFileContentsRequest
	//peerCtx->clipboard_server_context->ClientFileContentsResponse
	peerCtx->clipboard_server_context->useLongFormatNames = FALSE; // ASCII8 format name only (No Windows-style 2 bytes Unicode).
	peerCtx->clipboard_server_context->streamFileClipEnabled = FALSE;
	peerCtx->clipboard_server_context->fileClipNoFilePaths = FALSE;
	peerCtx->clipboard_server_context->canLockClipData = TRUE;
	if (peerCtx->clipboard_server_context->Start(peerCtx->clipboard_server_context) != 0)
		goto error;

	peerCtx->clipboard_selection_listener.notify = clipboard_set_selection;
	wl_signal_add(&seat->selection_signal,
			&peerCtx->clipboard_selection_listener);

	return 0;

error:
	if (peerCtx->clipboard_server_context) {
		cliprdr_server_context_free(peerCtx->clipboard_server_context);
		peerCtx->clipboard_server_context = NULL;
	}

	if (b->debugClipboard) {
		weston_log_scope_destroy(b->debugClipboard);
		b->debugClipboard = NULL;
	}

	return -1;
}

void
rdp_clipboard_destroy(RdpPeerContext *peerCtx)
{
	struct rdp_backend *b = peerCtx->rdpBackend;

	if (peerCtx->clipboard_selection_listener.notify) {
		wl_list_remove(&peerCtx->clipboard_selection_listener.link);
		peerCtx->clipboard_selection_listener.notify = NULL;
	}
	if (peerCtx->clipboard_data_request_event_source &&
		peerCtx->clipboard_data_request_event_source != RDP_INVALID_EVENT_SOURCE) {
		wl_event_source_remove(peerCtx->clipboard_data_request_event_source);
		peerCtx->clipboard_data_request_event_source = NULL;
	}

	if (peerCtx->clipboard_inflight_client_data_source) {
		clipboard_data_source_unref(peerCtx->clipboard_inflight_client_data_source);
		peerCtx->clipboard_inflight_client_data_source = NULL;
	}
	if (peerCtx->clipboard_client_data_source) {
		clipboard_data_source_unref(peerCtx->clipboard_client_data_source);
		peerCtx->clipboard_client_data_source = NULL;
	}

	if (peerCtx->clipboard_server_context) {
		peerCtx->clipboard_server_context->Stop(peerCtx->clipboard_server_context);
		cliprdr_server_context_free(peerCtx->clipboard_server_context);
		peerCtx->clipboard_server_context = NULL;
	}

	if (b->debugClipboard) {
		weston_log_scope_destroy(b->debugClipboard);
		b->debugClipboard = NULL;
	}
}
