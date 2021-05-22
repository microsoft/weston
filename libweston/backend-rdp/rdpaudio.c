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
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <time.h>
#include <sys/un.h>
#include <fcntl.h>
#include <linux/vm_sockets.h>
#include "rdp.h"

static AUDIO_FORMAT rdp_audio_supported_audio_formats[] = {
		{ WAVE_FORMAT_PCM, 2, 44100, 176400, 4, 16, 0, NULL },
	};

#define AUDIO_LATENCY 5
#define AUDIO_FRAMES_PER_RDP_PACKET (44100 * AUDIO_LATENCY / 1000)

#define RDP_SINK_INTERFACE_VERSION 1

#define RDP_AUDIO_CMD_VERSION 0
#define RDP_AUDIO_CMD_TRANSFER 1
#define RDP_AUDIO_CMD_GET_LATENCY 2
#define RDP_AUDIO_CMD_RESET_LATENCY 3

typedef struct _rdp_audio_cmd_header {
	uint32_t cmd;
	union {
		uint32_t version;
		struct {
			uint32_t bytes;
			uint64_t timestamp;
		} transfer;
	uint64_t reserved[8];
	};
} rdp_audio_cmd_header;

static char*
AUDIO_FORMAT_to_String(UINT16 format)
{
	switch (format) {
	case WAVE_FORMAT_UNKNOWN:		    
		return "WAVE_FORMAT_UNKNOWN";
	case WAVE_FORMAT_PCM:			    
		return "WAVE_FORMAT_PCM";
	case WAVE_FORMAT_ADPCM:			    
		return "WAVE_FORMAT_ADPCM";
	case WAVE_FORMAT_IEEE_FLOAT:		    
		return "WAVE_FORMAT_IEEE_FLOAT";
	case WAVE_FORMAT_VSELP:			    
		return "WAVE_FORMAT_VSELP";
	case WAVE_FORMAT_IBM_CVSD:		    
		return "WAVE_FORMAT_IBM_CVSD";
	case WAVE_FORMAT_ALAW:			    
		return "WAVE_FORMAT_ALAW";
	case WAVE_FORMAT_MULAW:			    
		return "WAVE_FORMAT_MULAW";
	case WAVE_FORMAT_OKI_ADPCM:		    
		return "WAVE_FORMAT_OKI_ADPCM";
	case WAVE_FORMAT_DVI_ADPCM:		    
		return "WAVE_FORMAT_DVI_ADPCM";
	case WAVE_FORMAT_MEDIASPACE_ADPCM:	    
		return "WAVE_FORMAT_MEDIASPACE_ADPCM";
	case WAVE_FORMAT_SIERRA_ADPCM:		    
		return "WAVE_FORMAT_SIERRA_ADPCM";
	case WAVE_FORMAT_G723_ADPCM:		    
		return "WAVE_FORMAT_G723_ADPCM";
	case WAVE_FORMAT_DIGISTD:		    
		return "WAVE_FORMAT_DIGISTD";
	case WAVE_FORMAT_DIGIFIX:		    
		return "WAVE_FORMAT_DIGIFIX";
	case WAVE_FORMAT_DIALOGIC_OKI_ADPCM:	    
		return "WAVE_FORMAT_DIALOGIC_OKI_ADPCM";
	case WAVE_FORMAT_MEDIAVISION_ADPCM:	    
		return "WAVE_FORMAT_MEDIAVISION_ADPCM";
	case WAVE_FORMAT_CU_CODEC:		    
		return "WAVE_FORMAT_CU_CODEC";
	case WAVE_FORMAT_YAMAHA_ADPCM:		    
		return "WAVE_FORMAT_YAMAHA_ADPCM";
	case WAVE_FORMAT_SONARC:		    
		return "WAVE_FORMAT_SONARC";
	case WAVE_FORMAT_DSPGROUP_TRUESPEECH:	    
		return "WAVE_FORMAT_DSPGROUP_TRUESPEECH";
	case WAVE_FORMAT_ECHOSC1:		    
		return "WAVE_FORMAT_ECHOSC1";
	case WAVE_FORMAT_AUDIOFILE_AF36:	    
		return "WAVE_FORMAT_AUDIOFILE_AF36";
	case WAVE_FORMAT_APTX:			    
		return "WAVE_FORMAT_APTX";
	case WAVE_FORMAT_AUDIOFILE_AF10:	    
		return "WAVE_FORMAT_AUDIOFILE_AF10";
	case WAVE_FORMAT_PROSODY_1612:		    
		return "WAVE_FORMAT_PROSODY_1612";
	case WAVE_FORMAT_DOLBY_AC2:		    
		return "WAVE_FORMAT_DOLBY_AC2";
	case WAVE_FORMAT_GSM610:		    
		return "WAVE_FORMAT_GSM610";
	case WAVE_FORMAT_MSNAUDIO:		    
		return "WAVE_FORMAT_MSNAUDIO";
	case WAVE_FORMAT_ANTEX_ADPCME:		    
		return "WAVE_FORMAT_ANTEX_ADPCME";
	case WAVE_FORMAT_CONTROL_RES_VQLPC:	    
		return "WAVE_FORMAT_CONTROL_RES_VQLPC";
	case WAVE_FORMAT_DIGIREAL:		    
		return "WAVE_FORMAT_DIGIREAL";
	case WAVE_FORMAT_DIGIADPCM:		    
		return "WAVE_FORMAT_DIGIADPCM";
	case WAVE_FORMAT_CONTROL_RES_CR10:	    
		return "WAVE_FORMAT_CONTROL_RES_CR10";
	case WAVE_FORMAT_NMS_VBXADPCM:		    
		return "WAVE_FORMAT_NMS_VBXADPCM";
	case WAVE_FORMAT_ROLAND_RDAC:		    
		return "WAVE_FORMAT_ROLAND_RDAC";
	case WAVE_FORMAT_ECHOSC3:		    
		return "WAVE_FORMAT_ECHOSC3";
	case WAVE_FORMAT_ROCKWELL_ADPCM:	    
		return "WAVE_FORMAT_ROCKWELL_ADPCM";
	case WAVE_FORMAT_ROCKWELL_DIGITALK:	    
		return "WAVE_FORMAT_ROCKWELL_DIGITALK";
	case WAVE_FORMAT_XEBEC:			    
		return "WAVE_FORMAT_XEBEC";
	case WAVE_FORMAT_G721_ADPCM:		    
		return "WAVE_FORMAT_G721_ADPCM";
	case WAVE_FORMAT_G728_CELP:		    
		return "WAVE_FORMAT_G728_CELP";
	case WAVE_FORMAT_MSG723:		    
		return "WAVE_FORMAT_MSG723";
	case WAVE_FORMAT_MPEG:			    
		return "WAVE_FORMAT_MPEG";
	case WAVE_FORMAT_RT24:			    
		return "WAVE_FORMAT_RT24";
	case WAVE_FORMAT_PAC:			    
		return "WAVE_FORMAT_PAC";
	case WAVE_FORMAT_MPEGLAYER3:		    
		return "WAVE_FORMAT_MPEGLAYER3";
	case WAVE_FORMAT_LUCENT_G723:		    
		return "WAVE_FORMAT_LUCENT_G723";
	case WAVE_FORMAT_CIRRUS:		    
		return "WAVE_FORMAT_CIRRUS";
	case WAVE_FORMAT_ESPCM:			    
		return "WAVE_FORMAT_ESPCM";
	case WAVE_FORMAT_VOXWARE:		    
		return "WAVE_FORMAT_VOXWARE";
	case WAVE_FORMAT_CANOPUS_ATRAC:		    
		return "WAVE_FORMAT_CANOPUS_ATRAC";
	case WAVE_FORMAT_G726_ADPCM:		    
		return "WAVE_FORMAT_G726_ADPCM";
	case WAVE_FORMAT_G722_ADPCM:		    
		return "WAVE_FORMAT_G722_ADPCM";
	case WAVE_FORMAT_DSAT:			    
		return "WAVE_FORMAT_DSAT";
	case WAVE_FORMAT_DSAT_DISPLAY:		    
		return "WAVE_FORMAT_DSAT_DISPLAY";
	case WAVE_FORMAT_VOXWARE_BYTE_ALIGNED:	    
		return "WAVE_FORMAT_VOXWARE_BYTE_ALIGNED";
	case WAVE_FORMAT_VOXWARE_AC8:		    
		return "WAVE_FORMAT_VOXWARE_AC8";
	case WAVE_FORMAT_VOXWARE_AC10:		    
		return "WAVE_FORMAT_VOXWARE_AC10";
	case WAVE_FORMAT_VOXWARE_AC16:		    
		return "WAVE_FORMAT_VOXWARE_AC16";
	case WAVE_FORMAT_VOXWARE_AC20:		    
		return "WAVE_FORMAT_VOXWARE_AC20";
	case WAVE_FORMAT_VOXWARE_RT24:		    
		return "WAVE_FORMAT_VOXWARE_RT24";
	case WAVE_FORMAT_VOXWARE_RT29:		    
		return "WAVE_FORMAT_VOXWARE_RT29";
	case WAVE_FORMAT_VOXWARE_RT29HW:	    
		return "WAVE_FORMAT_VOXWARE_RT29HW";
	case WAVE_FORMAT_VOXWARE_VR12:		    
		return "WAVE_FORMAT_VOXWARE_VR12";
	case WAVE_FORMAT_VOXWARE_VR18:		    
		return "WAVE_FORMAT_VOXWARE_VR18";
	case WAVE_FORMAT_VOXWARE_TQ40:		    
		return "WAVE_FORMAT_VOXWARE_TQ40";
	case WAVE_FORMAT_SOFTSOUND:		    
		return "WAVE_FORMAT_SOFTSOUND";
	case WAVE_FORMAT_VOXWARE_TQ60:		    
		return "WAVE_FORMAT_VOXWARE_TQ60";
	case WAVE_FORMAT_MSRT24:		    
		return "WAVE_FORMAT_MSRT24";
	case WAVE_FORMAT_G729A:			    
		return "WAVE_FORMAT_G729A";
	case WAVE_FORMAT_MVI_MV12:		    
		return "WAVE_FORMAT_MVI_MV12";
	case WAVE_FORMAT_DF_G726:		    
		return "WAVE_FORMAT_DF_G726";
	case WAVE_FORMAT_DF_GSM610:		    
		return "WAVE_FORMAT_DF_GSM610";
	case WAVE_FORMAT_ISIAUDIO:		    
		return "WAVE_FORMAT_ISIAUDIO";
	case WAVE_FORMAT_ONLIVE:		    
		return "WAVE_FORMAT_ONLIVE";
	case WAVE_FORMAT_SBC24:			    
		return "WAVE_FORMAT_SBC24";
	case WAVE_FORMAT_DOLBY_AC3_SPDIF:	    
		return "WAVE_FORMAT_DOLBY_AC3_SPDIF";
	case WAVE_FORMAT_ZYXEL_ADPCM:		    
		return "WAVE_FORMAT_ZYXEL_ADPCM";
	case WAVE_FORMAT_PHILIPS_LPCBB:		    
		return "WAVE_FORMAT_PHILIPS_LPCBB";
	case WAVE_FORMAT_PACKED:		    
		return "WAVE_FORMAT_PACKED";
	case WAVE_FORMAT_RHETOREX_ADPCM:	    
		return "WAVE_FORMAT_RHETOREX_ADPCM";
	case WAVE_FORMAT_IRAT:			    
		return "WAVE_FORMAT_IRAT";
	case WAVE_FORMAT_VIVO_G723:		    
		return "WAVE_FORMAT_VIVO_G723";
	case WAVE_FORMAT_VIVO_SIREN:		    
		return "WAVE_FORMAT_VIVO_SIREN";
	case WAVE_FORMAT_DIGITAL_G723:		    
		return "WAVE_FORMAT_DIGITAL_G723";
	case WAVE_FORMAT_WMAUDIO2:		    
		return "WAVE_FORMAT_WMAUDIO2";
	case WAVE_FORMAT_WMAUDIO3:		    
		return "WAVE_FORMAT_WMAUDIO3";
	case WAVE_FORMAT_WMAUDIO_LOSSLESS:	    
		return "WAVE_FORMAT_WMAUDIO_LOSSLESS";
	case WAVE_FORMAT_CREATIVE_ADPCM:	    
		return "WAVE_FORMAT_CREATIVE_ADPCM";
	case WAVE_FORMAT_CREATIVE_FASTSPEECH8:	    
		return "WAVE_FORMAT_CREATIVE_FASTSPEECH8";
	case WAVE_FORMAT_CREATIVE_FASTSPEECH10:     
		return "WAVE_FORMAT_CREATIVE_FASTSPEECH10";
	case WAVE_FORMAT_QUARTERDECK:		    
		return "WAVE_FORMAT_QUARTERDECK";
	case WAVE_FORMAT_FM_TOWNS_SND:		    
		return "WAVE_FORMAT_FM_TOWNS_SND";
	case WAVE_FORMAT_BTV_DIGITAL:		    
		return "WAVE_FORMAT_BTV_DIGITAL";
	case WAVE_FORMAT_VME_VMPCM:		    
		return "WAVE_FORMAT_VME_VMPCM";
	case WAVE_FORMAT_OLIGSM:		    
		return "WAVE_FORMAT_OLIGSM";
	case WAVE_FORMAT_OLIADPCM:		    
		return "WAVE_FORMAT_OLIADPCM";
	case WAVE_FORMAT_OLICELP:		    
		return "WAVE_FORMAT_OLICELP";
	case WAVE_FORMAT_OLISBC:		    
		return "WAVE_FORMAT_OLISBC";
	case WAVE_FORMAT_OLIOPR:		    
		return "WAVE_FORMAT_OLIOPR";
	case WAVE_FORMAT_LH_CODEC:		    
		return "WAVE_FORMAT_LH_CODEC";
	case WAVE_FORMAT_NORRIS:		    
		return "WAVE_FORMAT_NORRIS";
	case WAVE_FORMAT_SOUNDSPACE_MUSICOMPRESS:   
		return "WAVE_FORMAT_SOUNDSPACE_MUSICOMPRESS";
	case WAVE_FORMAT_DVM:			    
		return "WAVE_FORMAT_DVM";
	case WAVE_FORMAT_AAC_MS:		    
		return "WAVE_FORMAT_AAC_MS";
	}

	return "WAVE_FORMAT_UNKNOWN";
}

static int
rdp_audio_setup_listener(RdpPeerContext *peerCtx)
{
	struct rdp_backend *b = peerCtx->rdpBackend;
	char *sink_socket_path;
	int fd;
	struct sockaddr_un s;
	int bytes;
	int error;

	fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		rdp_debug_error(b, "Couldn't create listener socket.\n");
		return -1;
	}

	sink_socket_path = getenv("PULSE_AUDIO_RDP_SINK");
	if (sink_socket_path == NULL || sink_socket_path[0] == '\0') {
		close(fd);
		rdp_debug_error(b, "Environment variable PULSE_AUDIO_RDP_SINK not set.\n");
		return -1;
	}

	memset(&s, 0, sizeof(s));
	s.sun_family = AF_UNIX;
	bytes = sizeof(s.sun_path) - 1;
	snprintf(s.sun_path, bytes, "%s", sink_socket_path);

	remove(s.sun_path);

	rdp_debug(b, "Pulse Audio Sink listener socket on %s\n", s.sun_path);
	error = bind(fd, (struct sockaddr *)&s, sizeof(struct sockaddr_un));
	if (error != 0) {
		close(fd);
		rdp_debug_error(b, "Failed to bind to listener socket (%d).\n", error);
		return -1;
	}
    
	listen(fd, 100);
	return fd;
}

static UINT64 
rdp_audio_timestamp()
{
	struct timespec time;
	clock_gettime(CLOCK_REALTIME, &time);
	return time.tv_sec * 1000000 + time.tv_nsec / 1000;
}

static UINT 
rdp_audio_client_confirm_block(
	RdpsndServerContext* context, 
	BYTE confirmBlockNum,
	UINT16 wtimestamp)
{
	RdpPeerContext *peerCtx = (RdpPeerContext*)context->data;
	struct rdp_backend *b = peerCtx->rdpBackend;

	if (peerCtx->blockInfo[confirmBlockNum].ackReceivedTime != 0) {
		assert(peerCtx->blockInfo[confirmBlockNum].ackPlayedTime == 0);
		peerCtx->blockInfo[confirmBlockNum].ackPlayedTime = rdp_audio_timestamp();

		/* 
		 * Sum up all of the latency, we'll compute an average for the last period 
		 * requested by the sink.
		 */
		if (peerCtx->nextValidBlock == -1 || peerCtx->nextValidBlock == confirmBlockNum) {
			peerCtx->nextValidBlock = -1;

			peerCtx->accumulatedRenderedLatency +=
			peerCtx->blockInfo[confirmBlockNum].ackPlayedTime -
			peerCtx->blockInfo[confirmBlockNum].submissionTime;
			peerCtx->accumulatedRenderedLatencyCount++;
		}

		uint64_t one = 1;
		if (write(peerCtx->audioInSem, &one, sizeof(one)) != sizeof(uint64_t)) {
			rdp_debug_error(b, "RDP Audio error at confirm_block while writing to audioInSem (%s)\n", strerror(errno));
			return ERROR_INTERNAL_ERROR;
		}

	} else {
		peerCtx->blockInfo[confirmBlockNum].ackReceivedTime = rdp_audio_timestamp();

		peerCtx->accumulatedNetworkLatency +=
			peerCtx->blockInfo[confirmBlockNum].ackReceivedTime -
			peerCtx->blockInfo[confirmBlockNum].submissionTime;
		peerCtx->accumulatedNetworkLatencyCount++;
	}

	return 0;
}

static int 
rdp_audio_handle_version(
	RdpPeerContext *peerCtx,
	UINT PAVersion)
{
	struct rdp_backend *b = peerCtx->rdpBackend;
	uint32_t version = RDP_SINK_INTERFACE_VERSION;
	ssize_t sizeSent;

	peerCtx->PAVersion = PAVersion;

	rdp_debug(b, "RDP Sink version (%d - %d)\n", PAVersion, version);

	sizeSent = send(peerCtx->pulseAudioSinkFd, &version, 
			sizeof(version), MSG_DONTWAIT);
	if (sizeSent != sizeof(version)) {
		rdp_debug_error(b, "RDP audio error responding to version request sent:%ld. %s\n",
			sizeSent, strerror(errno));
		return -1;
	}

	return 0;
}

static int
rdp_audio_handle_transfer(
	RdpPeerContext *peerCtx,
	UINT bytesLeft,
	UINT64 timestamp)
{
	struct rdp_backend *b = peerCtx->rdpBackend;
	int nbFrames = bytesLeft / peerCtx->bytesPerFrame;
	UINT bytesRead = 0;
	ssize_t sizeRead = 0;

	if (bytesLeft > peerCtx->audioBufferSize) {
		if(peerCtx->audioBuffer)
			free(peerCtx->audioBuffer);

		peerCtx->audioBuffer = zalloc(bytesLeft);
		if (!peerCtx->audioBuffer) {
			rdp_debug_error(b, "RDP Audio error zalloc(%d) failed.\n", bytesLeft);
			return -1;
		}
		peerCtx->audioBufferSize = bytesLeft;
	}

	assert((bytesLeft % peerCtx->bytesPerFrame) == 0);

	/*
	 * Read the expected amount of data over the sink before sending it to RDP
	 */
	while (bytesLeft > 0) {
		sizeRead = read(peerCtx->pulseAudioSinkFd, 
				peerCtx->audioBuffer + bytesRead, bytesLeft);
		if (sizeRead <= 0) {
			rdp_debug_error(b, "RDP Audio error while reading data from sink socket sizeRead:%ld. %s\n", sizeRead, strerror(errno));
			return -1;
		}
		bytesRead += sizeRead;
		bytesLeft -= sizeRead;
	}

	BYTE* audioBuffer = peerCtx->audioBuffer;
	while (nbFrames > 0) {
		/*
		 * Ensure we don't overrun our audio buffers.
		 *
		 * SendSamples may not submit audio every time, it may accumulate audio and
		 * submit on subsequent call. We've sent latency such that it should never
		 * submit more than one packet of audio over the RDP channel for one
		 * of our incoming audio packet from pulse.
		 */
		uint64_t dummy;
		if (read(peerCtx->audioInSem, &dummy, sizeof(dummy)) != sizeof(uint64_t)) {
			rdp_debug_error(b, "RDP Audio error at handle_transfer while reading from audioInSem (%s)\n", strerror(errno));
			return -1;
		}

		/*
		 * Setup tracking of all block sent by RDP so we can compute latency later
		 * when those block gets acknowledge by the client.
		 *
		 * Set 0 to timestamp to disable A/V sync at client side.
		 */
		BYTE block_no = peerCtx->rdpsnd_server_context->block_no;
		peerCtx->blockInfo[block_no].submissionTime = timestamp;
		peerCtx->blockInfo[block_no].ackReceivedTime = 0;
		peerCtx->blockInfo[block_no].ackPlayedTime = 0;
		if (peerCtx->rdpsnd_server_context->SendSamples(peerCtx->rdpsnd_server_context,
							    audioBuffer,
							    MIN(nbFrames, AUDIO_FRAMES_PER_RDP_PACKET),
							    0) != 0) {
			rdp_debug_error(b, "RDP Audio error while SendSamples\n");
			return -1;
		}

		if (block_no == peerCtx->rdpsnd_server_context->block_no) {
			/*
			 * Didn't submit any audio this time around, adjust our semaphore.
			 */
			uint64_t one = 1;
			if (write(peerCtx->audioInSem, &one, sizeof(one)) != sizeof(uint64_t)) {
				rdp_debug_error(b, "RDP Audio error at handle_transfer while writing to audioInSem (%s)\n", strerror(errno));
				return -1;
			}
		} else {
			/*
			 * There shouldn't be more than one packet of audio sent by RDP.
			 */
			assert((block_no > peerCtx->rdpsnd_server_context->block_no) || (block_no+1) == peerCtx->rdpsnd_server_context->block_no);
			assert((block_no < peerCtx->rdpsnd_server_context->block_no) || (block_no==255 && peerCtx->rdpsnd_server_context->block_no==0));
		}

		audioBuffer += AUDIO_FRAMES_PER_RDP_PACKET * peerCtx->bytesPerFrame;
		nbFrames -= AUDIO_FRAMES_PER_RDP_PACKET;
	}

	return 0;
}

static int 
rdp_audio_handle_get_latency(
	RdpPeerContext *peerCtx)
{
	struct rdp_backend *b = peerCtx->rdpBackend;
	UINT networkLatency;
	UINT renderedLatency;
	ssize_t sizeSent;

	if (peerCtx->accumulatedNetworkLatencyCount > 0) {
		networkLatency = peerCtx->accumulatedNetworkLatency / peerCtx->accumulatedNetworkLatencyCount;
		peerCtx->lastNetworkLatency = networkLatency;
		peerCtx->accumulatedNetworkLatency = 0;
		peerCtx->accumulatedNetworkLatencyCount = 0;
	} else {
		networkLatency = peerCtx->lastNetworkLatency;
	}

	if (peerCtx->accumulatedRenderedLatencyCount > 0) {
		renderedLatency = peerCtx->accumulatedRenderedLatency / peerCtx->accumulatedRenderedLatencyCount;
		peerCtx->lastRenderedLatency = renderedLatency;
		peerCtx->accumulatedRenderedLatency = 0;
		peerCtx->accumulatedRenderedLatencyCount = 0;
	} else {
		renderedLatency = peerCtx->lastRenderedLatency;
	}

	if (renderedLatency > networkLatency)
		renderedLatency -= networkLatency;

	sizeSent = send(peerCtx->pulseAudioSinkFd, &renderedLatency,
				sizeof(renderedLatency), MSG_DONTWAIT);
	if (sizeSent != sizeof(renderedLatency)) {
		rdp_debug_error(b, "RDP audio error responding to latency request sent:%ld. %s\n",
			sizeSent, strerror(errno));
		return -1;
	}

	return 0;
}

static void signalhandler(int sig) {
	weston_log("RDP Audio: %s(%d)\n", __func__, sig);
	return;
}

static void*
rdp_audio_pulse_audio_sink_thread(void *context)
{
	RdpPeerContext *peerCtx = (RdpPeerContext*)context; 
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct sigaction act;
	sigset_t set;

	sigemptyset(&set);
	if (sigaddset(&set, SIGUSR2) == -1) {
		rdp_debug_error(b, "Audio sink thread: sigaddset(SIGUSR2) failed.\n");
		return NULL;
	}
	if (pthread_sigmask(SIG_UNBLOCK, &set, NULL) != 0) {
		rdp_debug_error(b, "Audio sink thread: pthread_sigmask(SIG_UNBLOCK,SIGUSR2) failed.\n");
		return NULL;
	}
	act.sa_flags = 0;
	act.sa_mask = set;
	act.sa_handler = &signalhandler;
	if (sigaction(SIGUSR2, &act, NULL) == -1) {
		rdp_debug_error(b, "Audio sink thread: sigaction(SIGUSR2) failed.\n");
		return NULL;
	}

	assert(peerCtx->pulseAudioSinkListenerFd != 0);

	for (;;) {
		rdp_debug(b, "Audio sink thread: Listening for audio connection.\n");

		if (peerCtx->audioExitSignal) {
			rdp_debug(b, "Audio sink thread is asked to exit (accept loop)\n");
			break;
		}

		/*
		 * Wait for a connection on our listening socket
		 */   
		assert(peerCtx->pulseAudioSinkFd < 0);
		peerCtx->pulseAudioSinkFd = accept(peerCtx->pulseAudioSinkListenerFd, 
							NULL, NULL);
		if (peerCtx->pulseAudioSinkFd < 0) {
			rdp_debug_error(b, "Audio sink thread: Listener connection error (%s)\n", strerror(errno));
			continue;
		} else {
			rdp_debug(b, "Audio sink thread: connection successful on socket (%d).\n", 
					peerCtx->pulseAudioSinkFd);
		}
    
		/*
		 * Read audio from the socket and stream to the RDP Client.
		 */
		for (;;) {
			rdp_audio_cmd_header header;
			ssize_t sizeRead;

			sizeRead = read(peerCtx->pulseAudioSinkFd, &header, sizeof(header));
			/* pulseaudio RDP sink always send sizeof(header) regardless command type. */
			if (sizeRead != sizeof(header)) {
				rdp_debug_error(b, "Audio sink thread: error while reading from sink socket sizeRead:%ld. %s\n",
					sizeRead, strerror(errno));
				break;
			} else if (header.cmd == RDP_AUDIO_CMD_VERSION) {
				rdp_debug_verbose(b, "Audio sink command RDP_AUDIO_CMD_VERSION: %d\n", header.version);
				if (rdp_audio_handle_version(peerCtx, header.version) < 0)
					break;
			} else if (header.cmd == RDP_AUDIO_CMD_TRANSFER) {
				rdp_debug_verbose(b, "Audio sink command RDP_AUDIO_CMD_TRANSFER: %d\n", header.transfer.bytes);
				if (rdp_audio_handle_transfer(peerCtx, header.transfer.bytes, header.transfer.timestamp) < 0)
					break;
			} else if (header.cmd == RDP_AUDIO_CMD_GET_LATENCY) {
				rdp_debug_verbose(b, "Audio sink command RDP_AUDIO_CMD_GET_LATENCY\n");
				if (rdp_audio_handle_get_latency(peerCtx) < 0)
					break;
			} else if (header.cmd == RDP_AUDIO_CMD_RESET_LATENCY) {
				rdp_debug_verbose(b, "Audio sink command RDP_AUDIO_CMD_RESET_LATENCY\n");
				peerCtx->nextValidBlock = peerCtx->rdpsnd_server_context->block_no;
				peerCtx->lastNetworkLatency = 0;
				peerCtx->accumulatedNetworkLatency = 0;
				peerCtx->accumulatedNetworkLatencyCount = 0;
				peerCtx->lastRenderedLatency = 0;
				peerCtx->accumulatedRenderedLatency = 0;
				peerCtx->accumulatedRenderedLatencyCount = 0;
			} else {
				rdp_debug_error(b, "Audio sink thread: unknown command from sink.\n");
				break;
			}
		}

		close(peerCtx->pulseAudioSinkFd);
		peerCtx->pulseAudioSinkFd = -1;
	}

	assert(peerCtx->pulseAudioSinkFd < 0);

	return NULL;
}

static void 
rdp_audio_client_activated(RdpsndServerContext* context)
{
	RdpPeerContext *peerCtx = (RdpPeerContext*)context->data;
	struct rdp_backend *b = peerCtx->rdpBackend;
	int format = -1;
	int i, j;
    
	rdp_debug(b, "rdp_audio_server_activated: %d audio formats supported.\n", 
			context->num_client_formats);

	for (i = 0; i < context->num_client_formats; i++) {
		rdp_debug(b, "\t[%d] - Format(%s) - Bits(%d), Channels(%d), Frequency(%d)\n",
			  i,
			  AUDIO_FORMAT_to_String(context->client_formats[i].wFormatTag),
			  context->client_formats[i].wBitsPerSample,
			  context->client_formats[i].nChannels,
			  context->client_formats[i].nSamplesPerSec);

		for (j = 0; j < (int)context->num_server_formats; j++) {
			if ((context->client_formats[i].wFormatTag == context->server_formats[j].wFormatTag) &&
			    (context->client_formats[i].nChannels == context->server_formats[j].nChannels) &&
			    (context->client_formats[i].nSamplesPerSec == context->server_formats[j].nSamplesPerSec)) {
				rdp_debug(b, "RDPAudio - Agreed on format %d.\n", i);
				format = i;
				break;
			}
		}
	}

	if (format != -1) {
		peerCtx->nextValidBlock = -1;
		peerCtx->bytesPerFrame = (context->client_formats[format].wBitsPerSample / 8) * context->client_formats[format].nChannels;
		context->latency = AUDIO_LATENCY;

		rdp_debug(b, "rdp_audio_server_activated: bytesPerFrame:%d, latency:%d\n", 
				peerCtx->bytesPerFrame, context->latency);

		context->SelectFormat(context, format);
		context->SetVolume(context, 0x7FFF, 0x7FFF);

		peerCtx->pulseAudioSinkListenerFd = rdp_audio_setup_listener(peerCtx);
		if (peerCtx->pulseAudioSinkListenerFd < 0) {
			rdp_debug_error(b, "RDPAudio - Failed to create listener socket\n");
		} else if (pthread_create(&peerCtx->pulseAudioSinkThread, NULL, rdp_audio_pulse_audio_sink_thread, (void*)peerCtx) < 0) {
			rdp_debug_error(b, "RDPAudio - Failed to start Pulse Audio Sink Thread. No audio will be available.\n");
		}
	} else {
		rdp_debug_error(b, "RDPAudio - No agreeded format.\n");
	}
}

int 
rdp_audio_init(RdpPeerContext *peerCtx)
{
	struct rdp_backend *b = peerCtx->rdpBackend;
	char *s;

	peerCtx->rdpsnd_server_context = rdpsnd_server_context_new(peerCtx->vcm);
	if (!peerCtx->rdpsnd_server_context) {
		rdp_debug_error(b, "RDPAudio - Couldn't initialize audio virtual channel.\n");
		return 0; // Continue without audio
	}

	peerCtx->audioExitSignal = FALSE;
	peerCtx->pulseAudioSinkThread = 0;
	peerCtx->pulseAudioSinkListenerFd = -1;
	peerCtx->pulseAudioSinkFd = -1;
	peerCtx->audioBuffer = NULL;

	peerCtx->audioInSem = eventfd(256, EFD_SEMAPHORE | EFD_CLOEXEC);
	if (!peerCtx->audioInSem) {
		rdp_debug_error(b, "RDPAudio - Couldn't initialize event semaphore.\n");
		goto Error_Exit;
	}

	/* this will be freed by FreeRDP at rdpsnd_server_context_free. */
	AUDIO_FORMAT *audio_formats = malloc(sizeof rdp_audio_supported_audio_formats);
	if (!audio_formats) {
		rdp_debug_error(b, "RDPAudio - Couldn't allocate memory for audio formats.\n");
		goto Error_Exit;
	}
	memcpy(audio_formats, rdp_audio_supported_audio_formats, sizeof rdp_audio_supported_audio_formats);
    
	peerCtx->rdpsnd_server_context->data = (void*)peerCtx;
	peerCtx->rdpsnd_server_context->Activated = rdp_audio_client_activated;
	peerCtx->rdpsnd_server_context->ConfirmBlock = rdp_audio_client_confirm_block;
	peerCtx->rdpsnd_server_context->num_server_formats = ARRAYSIZE(rdp_audio_supported_audio_formats);
	peerCtx->rdpsnd_server_context->server_formats = audio_formats;
	peerCtx->rdpsnd_server_context->src_format = &rdp_audio_supported_audio_formats[0];
#if HAVE_RDPSND_DYNAMIC_VIRTUAL_CHANNEL
	peerCtx->rdpsnd_server_context->use_dynamic_virtual_channel = TRUE;
	s = getenv("WESTON_RDP_DISABLE_AUDIO_PLAYBACK_DYNAMIC_VIRTUAL_CHANNEL");
	if (s) {
		if (strcmp(s, "true") == 0) {
			peerCtx->rdpsnd_server_context->use_dynamic_virtual_channel = FALSE;
			rdp_debug_error(b, "RDPAudio - force static channel.\n");
		}
	}
#endif // HAVE_RDPAUDIO_DYNAMIC_VIRTUAL_CHANNEL

	/* Calling Initialize does Start as well */
	if (peerCtx->rdpsnd_server_context->Initialize(peerCtx->rdpsnd_server_context, TRUE) != 0)
		goto Error_Exit;

	return 0;

Error_Exit:
	if (peerCtx->audioInSem != -1) {
		close(peerCtx->audioInSem);
		peerCtx->audioInSem = -1;
	}

	if (peerCtx->rdpsnd_server_context) {
		rdpsnd_server_context_free(peerCtx->rdpsnd_server_context);
		peerCtx->rdpsnd_server_context = NULL;
	}

	return 0; // Continue without audio
}

void
rdp_audio_destroy(RdpPeerContext *peerCtx)
{
	if (peerCtx->rdpsnd_server_context) {

		if (peerCtx->pulseAudioSinkThread) {
			peerCtx->audioExitSignal = TRUE;
			shutdown(peerCtx->pulseAudioSinkListenerFd, SHUT_RDWR);
			shutdown(peerCtx->pulseAudioSinkFd, SHUT_RDWR);
			pthread_kill(peerCtx->pulseAudioSinkThread, SIGUSR2);   
			pthread_join(peerCtx->pulseAudioSinkThread, NULL);

			if (peerCtx->pulseAudioSinkListenerFd != -1) {
				close(peerCtx->pulseAudioSinkListenerFd);
				peerCtx->pulseAudioSinkListenerFd = -1;
			}

			if (peerCtx->pulseAudioSinkFd != -1) {
				close(peerCtx->pulseAudioSinkFd);
				peerCtx->pulseAudioSinkFd = -1;
			}

			if (peerCtx->audioBuffer) {
				free(peerCtx->audioBuffer);
				peerCtx->audioBuffer = NULL;
			}

			peerCtx->pulseAudioSinkThread = 0;
		}

		assert(peerCtx->pulseAudioSinkListenerFd < 0);
		assert(peerCtx->pulseAudioSinkFd < 0);
		assert(peerCtx->audioBuffer == NULL);

		peerCtx->rdpsnd_server_context->Close(peerCtx->rdpsnd_server_context);
		peerCtx->rdpsnd_server_context->Stop(peerCtx->rdpsnd_server_context);

		if (peerCtx->audioInSem != -1) {
			close(peerCtx->audioInSem);
			peerCtx->audioInSem = -1;
		}

		rdpsnd_server_context_free(peerCtx->rdpsnd_server_context);
		peerCtx->rdpsnd_server_context = NULL;
	}
}
