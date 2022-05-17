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
#include "rdpaudio.h"
#include <libweston/libweston.h>
#include <shared/xalloc.h>

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
rdp_audio_setup_listener(void)
{
	char *sink_socket_path;
	int fd;
	struct sockaddr_un s;
	int bytes;
	int error;

	fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		weston_log("Couldn't create listener socket.\n");
		return -1;
	}

	sink_socket_path = getenv("PULSE_AUDIO_RDP_SINK");
	if (sink_socket_path == NULL || sink_socket_path[0] == '\0') {
		close(fd);
		weston_log("Environment variable PULSE_AUDIO_RDP_SINK not set.\n");
		return -1;
	}

	memset(&s, 0, sizeof(s));
	s.sun_family = AF_UNIX;
	bytes = sizeof(s.sun_path) - 1;
	snprintf(s.sun_path, bytes, "%s", sink_socket_path);

	remove(s.sun_path);

	weston_log("Pulse Audio Sink listener socket on %s\n", s.sun_path);
	error = bind(fd, (struct sockaddr *)&s, sizeof(struct sockaddr_un));
	if (error != 0) {
		close(fd);
		weston_log("Failed to bind to listener socket (%d).\n", error);
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
	struct audio_out_private *priv = context->data;

	if (priv->blockInfo[confirmBlockNum].ackReceivedTime != 0) {
		assert(priv->blockInfo[confirmBlockNum].ackPlayedTime == 0);
		priv->blockInfo[confirmBlockNum].ackPlayedTime = rdp_audio_timestamp();

		/* 
		 * Sum up all of the latency, we'll compute an average for the last period 
		 * requested by the sink.
		 */
		if (priv->nextValidBlock == -1 || priv->nextValidBlock == confirmBlockNum) {
			priv->nextValidBlock = -1;

			priv->accumulatedRenderedLatency +=
			priv->blockInfo[confirmBlockNum].ackPlayedTime -
			priv->blockInfo[confirmBlockNum].submissionTime;
			priv->accumulatedRenderedLatencyCount++;
		}

		uint64_t one = 1;
		if (write(priv->audioSem, &one, sizeof(one)) != sizeof(uint64_t)) {
			weston_log("RDP Audio error at confirm_block while writing to audioSem (%s)\n", strerror(errno));
			return ERROR_INTERNAL_ERROR;
		}

	} else {
		priv->blockInfo[confirmBlockNum].ackReceivedTime = rdp_audio_timestamp();

		priv->accumulatedNetworkLatency +=
			priv->blockInfo[confirmBlockNum].ackReceivedTime -
			priv->blockInfo[confirmBlockNum].submissionTime;
		priv->accumulatedNetworkLatencyCount++;
	}

	return 0;
}

static int 
rdp_audio_handle_version(
	struct audio_out_private *priv,
	UINT PAVersion)
{
	uint32_t version = RDP_SINK_INTERFACE_VERSION;
	ssize_t sizeSent;

	priv->PAVersion = PAVersion;

	weston_log("RDP Sink version (%d - %d)\n", PAVersion, version);

	sizeSent = send(priv->pulseAudioSinkFd, &version, 
			sizeof(version), MSG_DONTWAIT);
	if (sizeSent != sizeof(version)) {
		weston_log("RDP audio error responding to version request sent:%ld. %s\n",
			sizeSent, strerror(errno));
		return -1;
	}

	return 0;
}

static int
rdp_audio_handle_transfer(
	struct audio_out_private *priv,
	UINT bytesLeft,
	UINT64 timestamp)
{
	int nbFrames = bytesLeft / priv->bytesPerFrame;
	UINT bytesRead = 0;
	ssize_t sizeRead = 0;

	if (bytesLeft > priv->audioBufferSize) {
		if(priv->audioBuffer)
			free(priv->audioBuffer);

		priv->audioBuffer = zalloc(bytesLeft);
		if (!priv->audioBuffer) {
			weston_log("RDP Audio error zalloc(%d) failed.\n", bytesLeft);
			return -1;
		}
		priv->audioBufferSize = bytesLeft;
	}

	assert((bytesLeft % priv->bytesPerFrame) == 0);

	/*
	 * Read the expected amount of data over the sink before sending it to RDP
	 */
	while (bytesLeft > 0) {
		sizeRead = read(priv->pulseAudioSinkFd, 
				priv->audioBuffer + bytesRead, bytesLeft);
		if (sizeRead <= 0) {
			weston_log("RDP Audio error while reading data from sink socket sizeRead:%ld. %s\n", sizeRead, strerror(errno));
			return -1;
		}
		bytesRead += sizeRead;
		bytesLeft -= sizeRead;
	}

	BYTE* audioBuffer = priv->audioBuffer;
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
		if (read(priv->audioSem, &dummy, sizeof(dummy)) != sizeof(uint64_t)) {
			weston_log("RDP Audio error at handle_transfer while reading from audioSem (%s)\n", strerror(errno));
			return -1;
		}

		/*
		 * Setup tracking of all block sent by RDP so we can compute latency later
		 * when those block gets acknowledge by the client.
		 *
		 * Set 0 to timestamp to disable A/V sync at client side.
		 */
		BYTE block_no = priv->rdpsnd_server_context->block_no;
		priv->blockInfo[block_no].submissionTime = timestamp;
		priv->blockInfo[block_no].ackReceivedTime = 0;
		priv->blockInfo[block_no].ackPlayedTime = 0;
		if (priv->rdpsnd_server_context->SendSamples(priv->rdpsnd_server_context,
							    audioBuffer,
							    MIN(nbFrames, AUDIO_FRAMES_PER_RDP_PACKET),
							    0) != 0) {
			weston_log("RDP Audio error while SendSamples\n");
			return -1;
		}

		if (block_no == priv->rdpsnd_server_context->block_no) {
			/*
			 * Didn't submit any audio this time around, adjust our semaphore.
			 */
			uint64_t one = 1;
			if (write(priv->audioSem, &one, sizeof(one)) != sizeof(uint64_t)) {
				weston_log("RDP Audio error at handle_transfer while writing to audioSem (%s)\n", strerror(errno));
				return -1;
			}
		} else {
			/*
			 * There shouldn't be more than one packet of audio sent by RDP.
			 */
			assert((block_no > priv->rdpsnd_server_context->block_no) || (block_no+1) == priv->rdpsnd_server_context->block_no);
			assert((block_no < priv->rdpsnd_server_context->block_no) || (block_no==255 && priv->rdpsnd_server_context->block_no==0));
		}

		audioBuffer += AUDIO_FRAMES_PER_RDP_PACKET * priv->bytesPerFrame;
		nbFrames -= AUDIO_FRAMES_PER_RDP_PACKET;
	}

	return 0;
}

static int 
rdp_audio_handle_get_latency(struct audio_out_private *priv)
{
	UINT networkLatency;
	UINT renderedLatency;
	ssize_t sizeSent;

	if (priv->accumulatedNetworkLatencyCount > 0) {
		networkLatency = priv->accumulatedNetworkLatency / priv->accumulatedNetworkLatencyCount;
		priv->lastNetworkLatency = networkLatency;
		priv->accumulatedNetworkLatency = 0;
		priv->accumulatedNetworkLatencyCount = 0;
	} else {
		networkLatency = priv->lastNetworkLatency;
	}

	if (priv->accumulatedRenderedLatencyCount > 0) {
		renderedLatency = priv->accumulatedRenderedLatency / priv->accumulatedRenderedLatencyCount;
		priv->lastRenderedLatency = renderedLatency;
		priv->accumulatedRenderedLatency = 0;
		priv->accumulatedRenderedLatencyCount = 0;
	} else {
		renderedLatency = priv->lastRenderedLatency;
	}

	if (renderedLatency > networkLatency)
		renderedLatency -= networkLatency;

	sizeSent = send(priv->pulseAudioSinkFd, &renderedLatency,
				sizeof(renderedLatency), MSG_DONTWAIT);
	if (sizeSent != sizeof(renderedLatency)) {
		weston_log("RDP audio error responding to latency request sent:%ld. %s\n",
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
	struct audio_out_private *priv = context;
	struct sigaction act;
	sigset_t set;

	sigemptyset(&set);
	if (sigaddset(&set, SIGUSR2) == -1) {
		weston_log("Audio sink thread: sigaddset(SIGUSR2) failed.\n");
		return NULL;
	}
	if (pthread_sigmask(SIG_UNBLOCK, &set, NULL) != 0) {
		weston_log("Audio sink thread: pthread_sigmask(SIG_UNBLOCK,SIGUSR2) failed.\n");
		return NULL;
	}
	act.sa_flags = 0;
	act.sa_mask = set;
	act.sa_handler = &signalhandler;
	if (sigaction(SIGUSR2, &act, NULL) == -1) {
		weston_log("Audio sink thread: sigaction(SIGUSR2) failed.\n");
		return NULL;
	}

	assert(priv->pulseAudioSinkListenerFd != 0);

	for (;;) {
		rdp_audio_debug(priv, "Audio sink thread: Listening for audio connection.\n");

		if (priv->audioExitSignal) {
			rdp_audio_debug(priv, "Audio sink thread is asked to exit (accept loop)\n");
			break;
		}

		/*
		 * Wait for a connection on our listening socket
		 */   
		assert(priv->pulseAudioSinkFd < 0);
		priv->pulseAudioSinkFd = accept(priv->pulseAudioSinkListenerFd, 
							NULL, NULL);
		if (priv->pulseAudioSinkFd < 0) {
			weston_log("Audio sink thread: Listener connection error (%s)\n", strerror(errno));
			continue;
		} else {
			rdp_audio_debug(priv, "Audio sink thread: connection successful on socket (%d).\n",
					priv->pulseAudioSinkFd);
		}

		/*
		 * Read audio from the socket and stream to the RDP Client.
		 */
		for (;;) {
			rdp_audio_cmd_header header;
			ssize_t sizeRead;

			sizeRead = read(priv->pulseAudioSinkFd, &header, sizeof(header));
			/* pulseaudio RDP sink always send sizeof(header) regardless command type. */
			if (sizeRead != sizeof(header)) {
				weston_log("Audio sink thread: error while reading from sink socket sizeRead:%ld. %s\n",
					sizeRead, strerror(errno));
				break;
			} else if (header.cmd == RDP_AUDIO_CMD_VERSION) {
				rdp_audio_debug(priv, "Audio sink command RDP_AUDIO_CMD_VERSION: %d\n", header.version);
				if (rdp_audio_handle_version(priv, header.version) < 0)
					break;
			} else if (header.cmd == RDP_AUDIO_CMD_TRANSFER) {
				rdp_audio_debug(priv, "Audio sink command RDP_AUDIO_CMD_TRANSFER: %d\n", header.transfer.bytes);
				if (rdp_audio_handle_transfer(priv, header.transfer.bytes, header.transfer.timestamp) < 0)
					break;
			} else if (header.cmd == RDP_AUDIO_CMD_GET_LATENCY) {
				rdp_audio_debug(priv, "Audio sink command RDP_AUDIO_CMD_GET_LATENCY\n");
				if (rdp_audio_handle_get_latency(priv) < 0)
					break;
			} else if (header.cmd == RDP_AUDIO_CMD_RESET_LATENCY) {
				rdp_audio_debug(priv, "Audio sink command RDP_AUDIO_CMD_RESET_LATENCY\n");
				priv->nextValidBlock = priv->rdpsnd_server_context->block_no;
				priv->lastNetworkLatency = 0;
				priv->accumulatedNetworkLatency = 0;
				priv->accumulatedNetworkLatencyCount = 0;
				priv->lastRenderedLatency = 0;
				priv->accumulatedRenderedLatency = 0;
				priv->accumulatedRenderedLatencyCount = 0;
			} else {
				weston_log("Audio sink thread: unknown command from sink.\n");
				break;
			}
		}

		close(priv->pulseAudioSinkFd);
		priv->pulseAudioSinkFd = -1;
	}

	assert(priv->pulseAudioSinkFd < 0);

	return NULL;
}

static void 
rdp_audio_client_activated(RdpsndServerContext* context)
{
	struct audio_out_private *priv = context->data;
	int format = -1;
	int i, j;

	rdp_audio_debug(priv, "rdp_audio_server_activated: %d audio formats supported.\n",
			context->num_client_formats);

	for (i = 0; i < context->num_client_formats; i++) {
		rdp_audio_debug(priv, "\t[%d] - Format(%s) - Bits(%d), Channels(%d), Frequency(%d)\n",
				i,
				AUDIO_FORMAT_to_String(context->client_formats[i].wFormatTag),
				context->client_formats[i].wBitsPerSample,
				context->client_formats[i].nChannels,
				context->client_formats[i].nSamplesPerSec);

		for (j = 0; j < (int)context->num_server_formats; j++) {
			if ((context->client_formats[i].wFormatTag == context->server_formats[j].wFormatTag) &&
			    (context->client_formats[i].nChannels == context->server_formats[j].nChannels) &&
			    (context->client_formats[i].nSamplesPerSec == context->server_formats[j].nSamplesPerSec)) {
				rdp_audio_debug(priv, "RDPAudio - Agreed on format %d.\n", i);
				format = i;
				break;
			}
		}
	}

	if (format != -1) {
		priv->nextValidBlock = -1;
		priv->bytesPerFrame = (context->client_formats[format].wBitsPerSample / 8) * context->client_formats[format].nChannels;
		context->latency = AUDIO_LATENCY;

		rdp_audio_debug(priv, "rdp_audio_server_activated: bytesPerFrame:%d, latency:%d\n", 
				priv->bytesPerFrame, context->latency);

		context->SelectFormat(context, format);
		context->SetVolume(context, 0x7FFF, 0x7FFF);

		priv->pulseAudioSinkListenerFd = rdp_audio_setup_listener();
		if (priv->pulseAudioSinkListenerFd < 0) {
			weston_log("RDPAudio - Failed to create listener socket\n");
		} else if (pthread_create(&priv->pulseAudioSinkThread, NULL, rdp_audio_pulse_audio_sink_thread, (void*)priv) < 0) {
			weston_log("RDPAudio - Failed to start Pulse Audio Sink Thread. No audio will be available.\n");
		}
	} else {
		weston_log("RDPAudio - No agreeded format.\n");
	}
}

void *
rdp_audio_out_init(struct weston_compositor *c, HANDLE vcm)
{
	struct audio_out_private *priv;
	char *s;

	priv = xzalloc(sizeof *priv);
	priv->rdpsnd_server_context = rdpsnd_server_context_new(vcm);
	if (!priv->rdpsnd_server_context) {
		weston_log("RDPAudio - Couldn't initialize audio virtual channel.\n");
		return NULL;
	}

	priv->debug = weston_compositor_add_log_scope(c, "rdp-audio",
						      "Debug messages for RDP audio output\n",
						      NULL, NULL, NULL);

	priv->audioExitSignal = FALSE;
	priv->pulseAudioSinkThread = 0;
	priv->pulseAudioSinkListenerFd = -1;
	priv->pulseAudioSinkFd = -1;
	priv->audioBuffer = NULL;

	priv->audioSem = eventfd(256, EFD_SEMAPHORE | EFD_CLOEXEC);
	if (!priv->audioSem) {
		weston_log("RDPAudio - Couldn't initialize event semaphore.\n");
		goto Error_Exit;
	}

	/* this will be freed by FreeRDP at rdpsnd_server_context_free. */
	AUDIO_FORMAT *audio_formats = malloc(sizeof rdp_audio_supported_audio_formats);
	if (!audio_formats) {
		weston_log("RDPAudio - Couldn't allocate memory for audio formats.\n");
		goto Error_Exit;
	}
	memcpy(audio_formats, rdp_audio_supported_audio_formats, sizeof rdp_audio_supported_audio_formats);
    
	priv->rdpsnd_server_context->data = (void*)priv;
	priv->rdpsnd_server_context->Activated = rdp_audio_client_activated;
	priv->rdpsnd_server_context->ConfirmBlock = rdp_audio_client_confirm_block;
	priv->rdpsnd_server_context->num_server_formats = ARRAYSIZE(rdp_audio_supported_audio_formats);
	priv->rdpsnd_server_context->server_formats = audio_formats;
	priv->rdpsnd_server_context->src_format = &rdp_audio_supported_audio_formats[0];
#if HAVE_RDPSND_DYNAMIC_VIRTUAL_CHANNEL
	priv->rdpsnd_server_context->use_dynamic_virtual_channel = TRUE;
	s = getenv("WESTON_RDP_DISABLE_AUDIO_PLAYBACK_DYNAMIC_VIRTUAL_CHANNEL");
	if (s) {
		if (strcmp(s, "true") == 0) {
			priv->rdpsnd_server_context->use_dynamic_virtual_channel = FALSE;
			weston_log("RDPAudio - force static channel.\n");
		}
	}
#endif // HAVE_RDPAUDIO_DYNAMIC_VIRTUAL_CHANNEL

	/* Calling Initialize does Start as well */
	if (priv->rdpsnd_server_context->Initialize(priv->rdpsnd_server_context, TRUE) != 0)
		goto Error_Exit;

	return priv;

Error_Exit:
	if (priv->debug)
		weston_log_scope_destroy(priv->debug);

	if (priv->audioSem != -1) {
		close(priv->audioSem);
		priv->audioSem = -1;
	}

	if (priv->rdpsnd_server_context) {
		rdpsnd_server_context_free(priv->rdpsnd_server_context);
		priv->rdpsnd_server_context = NULL;
	}

	free(priv);
	return NULL;
}

void
rdp_audio_out_destroy(void *audio_out_private)
{
	struct audio_out_private *priv = audio_out_private;

	if (priv->rdpsnd_server_context) {

		if (priv->pulseAudioSinkThread) {
			priv->audioExitSignal = TRUE;
			shutdown(priv->pulseAudioSinkListenerFd, SHUT_RDWR);
			shutdown(priv->pulseAudioSinkFd, SHUT_RDWR);
			pthread_kill(priv->pulseAudioSinkThread, SIGUSR2);   
			pthread_join(priv->pulseAudioSinkThread, NULL);

			if (priv->pulseAudioSinkListenerFd != -1) {
				close(priv->pulseAudioSinkListenerFd);
				priv->pulseAudioSinkListenerFd = -1;
			}

			if (priv->pulseAudioSinkFd != -1) {
				close(priv->pulseAudioSinkFd);
				priv->pulseAudioSinkFd = -1;
			}

			if (priv->audioBuffer) {
				free(priv->audioBuffer);
				priv->audioBuffer = NULL;
			}

			priv->pulseAudioSinkThread = 0;
		}

		assert(priv->pulseAudioSinkListenerFd < 0);
		assert(priv->pulseAudioSinkFd < 0);
		assert(priv->audioBuffer == NULL);

		priv->rdpsnd_server_context->Close(priv->rdpsnd_server_context);
		priv->rdpsnd_server_context->Stop(priv->rdpsnd_server_context);

		if (priv->audioSem != -1) {
			close(priv->audioSem);
			priv->audioSem = -1;
		}

		rdpsnd_server_context_free(priv->rdpsnd_server_context);
		priv->rdpsnd_server_context = NULL;
	}
	free(priv);
}
