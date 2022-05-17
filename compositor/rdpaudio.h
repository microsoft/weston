/*
 * Copyright © 2022 Microsoft
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

#ifndef RDP_AUDIO_H
#define RDP_AUDIO_H

#include <libweston/libweston.h>
#include <libweston/weston-log.h>
#include <freerdp/server/rdpsnd.h>
#include <freerdp/server/audin.h>

#define rdp_audio_debug(p, ...) \
	weston_log_scope_printf((p)->debug, __VA_ARGS__)

typedef struct _rdp_audio_block_info {
	UINT64 submissionTime;
	UINT64 ackReceivedTime;
	UINT64 ackPlayedTime;
} rdp_audio_block_info;

struct audio_out_private {
	RdpsndServerContext* rdpsnd_server_context;
	struct weston_log_scope *debug;
	BOOL audioExitSignal;
	int pulseAudioSinkListenerFd;
	int pulseAudioSinkFd;
	pthread_t pulseAudioSinkThread;
	int bytesPerFrame;
	UINT audioBufferSize;
	BYTE* audioBuffer;
	BYTE lastBlockSent;
	UINT64 lastNetworkLatency;
	UINT64 accumulatedNetworkLatency;
	UINT accumulatedNetworkLatencyCount;
	UINT64 lastRenderedLatency;
	UINT64 accumulatedRenderedLatency;
	UINT accumulatedRenderedLatencyCount;
	rdp_audio_block_info blockInfo[256];
	int nextValidBlock;
	UINT PAVersion;
	int audioSem;
};

struct audio_in_private {
	audin_server_context* audin_server_context;
	struct weston_log_scope *debug;
	BOOL audioInExitSignal;
	int pulseAudioSourceListenerFd;
	int pulseAudioSourceFd;
	int closeAudioSourceFd;
	pthread_t pulseAudioSourceThread;
	BOOL isAudioInStreamOpened;
};

void *
rdp_audio_out_init(struct weston_compositor *c, HANDLE vcm);

void
rdp_audio_out_destroy(void *audio_out_private);

void *
rdp_audio_in_init(struct weston_compositor *c, HANDLE vcm);

void
rdp_audio_in_destroy(void *audio_in_private);


#endif
