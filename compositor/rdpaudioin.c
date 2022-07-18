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
#include <time.h>
#include <sys/un.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <linux/vm_sockets.h>
#include "rdpaudio.h"
#include <libweston/libweston.h>
#include <shared/xalloc.h>

static AUDIO_FORMAT rdp_audioin_supported_audio_formats[] = {
		{ WAVE_FORMAT_PCM, 1, 44100, 88200, 2, 16, 0, NULL },
	};

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
rdp_audioin_setup_listener(struct audio_in_private *priv)
{
	char *source_socket_path;
	int fd;
	struct sockaddr_un s;
	int bytes;
	int error;

	fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		weston_log("Couldn't create audioin listener socket.\n");
		return -1;
	}

	source_socket_path = getenv("PULSE_AUDIO_RDP_SOURCE");
	if (source_socket_path == NULL || source_socket_path[0] == '\0') {
		close(fd);
		weston_log("Environment variable PULSE_AUDIO_RDP_SOURCE not set.\n");
		return -1;
	}

	memset(&s, 0, sizeof(s));
	s.sun_family = AF_UNIX;
	bytes = sizeof(s.sun_path) - 1;
	snprintf(s.sun_path, bytes, "%s", source_socket_path);

	remove(s.sun_path);

	rdp_audio_debug(priv, "Pulse Audio source listener socket on %s\n", s.sun_path);
	error = bind(fd, (struct sockaddr *)&s, sizeof(struct sockaddr_un));
	if (error != 0) {
		close(fd);
		weston_log("Failed to bind to listener socket for audioin (%d).\n", error);
		return -1;
	}
    
	listen(fd, 100);
	return fd;
}

static UINT 
rdp_audioin_client_opening(audin_server_context* context)
{
	struct audio_in_private *priv = context->data;
	int format = -1;
	int i, j;
    
	rdp_audio_debug(priv, "RDP Audio Open: %d audio formats supported.\n",
			(int)context->num_client_formats);

	for (i = 0; i < (int)context->num_client_formats; i++) {
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
				rdp_audio_debug(priv, "RDPAudioIn - Agreed on format %d.\n", i);
				format = i;
				break;
			}
		}
	}

	if (format == -1) {
		weston_log("RDPAudioIn - No agreeded format.\n");
		return ERROR_INVALID_DATA;
	}

	context->SelectFormat(context, format);
	priv->isAudioInStreamOpened = TRUE;

	return 0;
}

static UINT 
rdp_audioin_client_open_result(
	audin_server_context* context, 
	UINT32 result)
{
	struct audio_in_private *priv = context->data;

	rdp_audio_debug(priv, "RDP AudioIn Open Result (%d)\n", result);
	return 0;
}

static UINT
rdp_audioin_client_receive_samples(
	audin_server_context* context,
	const AUDIO_FORMAT* format, 
	wStream* buf,
	size_t nframes)
{
	struct audio_in_private *priv = context->data;

	if (!priv->isAudioInStreamOpened || priv->pulseAudioSourceFd == -1) {
		weston_log("RDPAudioIn - audio stream is not opened.\n");
		return 0;
	}

	if (nframes) {
		assert(format->wFormatTag == WAVE_FORMAT_PCM);
		assert(format->nChannels == 1);
		assert(format->nSamplesPerSec == 44100);
		assert(format->wBitsPerSample == 16);
		assert(buf != NULL);

		int bytes = nframes * format->wBitsPerSample / 8;
		int sent = send(priv->pulseAudioSourceFd, buf->buffer, bytes, 0);
		if (sent != bytes) {
			rdp_audio_debug(priv, "RDP AudioIn source send failed (sent:%d, bytes:%d) %s\n",
					sent, bytes, strerror(errno));

			/* Unblock worker thread to close pipe to pulseaudio */
			uint64_t one=1;
			if (write(priv->closeAudioSourceFd, &one, sizeof(one)) != sizeof(uint64_t)) {
				weston_log("RDP AudioIn error at receive_samples while writing to closeAudioSourceFd (%s)\n", strerror(errno));
				return ERROR_INTERNAL_ERROR;
			}

			if (sent <= 0) {
				/* return error to FreeRDP as failed to send samples to pulseaudio. */
				return ERROR_INTERNAL_ERROR;
			}
		}
	}

	return 0;
}

static void*
rdp_audioin_source_thread(void *context)
{
	struct audio_in_private *priv = context;

	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
 
	assert(priv->closeAudioSourceFd != -1);
	assert(priv->pulseAudioSourceListenerFd != -1); 
    
	for (;;) {
		rdp_audio_debug(priv, "AudioIn source_thread: Listening for audio in connection.\n");

		if (priv->audioInExitSignal) {
			rdp_audio_debug(priv, "AudioIn source_thread is asked to exit (accept loop)\n");
			break;
		}

		/*
		 * Wait for a connection on our listening socket
		 */   
		priv->pulseAudioSourceFd = accept(priv->pulseAudioSourceListenerFd, NULL, NULL);
		if (priv->pulseAudioSourceFd < 0) {
			weston_log("AudioIn source thread: Listener connection error (%s)\n", strerror(errno));
			continue;
		} else {
			rdp_audio_debug(priv, "AudioIn connection successful on socket (%d).\n", priv->pulseAudioSourceFd);
			if (priv->audin_server_context->Open(priv->audin_server_context)) {
				rdp_audio_debug(priv, "RDP AudioIn opened.\n");
				/*
				 * Wait for the connection to be closed
				 */
				uint64_t dummy;
				if (read(priv->closeAudioSourceFd, &dummy, sizeof(dummy)) != sizeof(uint64_t)) {
					weston_log("RDP AudioIn wait on eventfd failed. thread exiting. %s\n", strerror(errno));
					break;
				}
				priv->audin_server_context->Close(priv->audin_server_context);
				rdp_audio_debug(priv, "RDP AudioIn closed.\n");
			} else {
				weston_log("Failed to open audio in connection with RDP client.\n");
			}

			close(priv->pulseAudioSourceFd);
			priv->pulseAudioSourceFd = -1;
		}
	}

	if (priv->audin_server_context->IsOpen(priv->audin_server_context))
		priv->audin_server_context->Close(priv->audin_server_context);

	if (priv->pulseAudioSourceFd != -1) {
		close(priv->pulseAudioSourceFd);
		priv->pulseAudioSourceFd = -1;
	}

	return NULL;
}

void *
rdp_audio_in_init(struct weston_compositor *c, HANDLE vcm)
{
	struct audio_in_private *priv;

	priv = xzalloc(sizeof *priv);
	priv->audin_server_context = audin_server_context_new(vcm);
	if (!priv->audin_server_context) {
		weston_log("RDPAudioIn - Couldn't initialize audio virtual channel.\n");
		return NULL;
	}
        priv->debug = weston_compositor_add_log_scope(c, "rdp-audio-in",
                                                      "Debug messages for RDP audio input\n",
                                                      NULL, NULL, NULL);

	priv->audioInExitSignal = FALSE;
	priv->pulseAudioSourceThread = 0;
	priv->pulseAudioSourceListenerFd = -1;
	priv->pulseAudioSourceFd = -1;
	priv->closeAudioSourceFd = -1;

	// this will be freed by FreeRDP at audin_server_context_free.
	AUDIO_FORMAT *audio_formats = malloc(sizeof rdp_audioin_supported_audio_formats);
	if (!audio_formats) {
		weston_log("RDPAudioIn - Couldn't allocate memory for audio formats.\n");
		goto Error_Exit;
	}
	memcpy(audio_formats, rdp_audioin_supported_audio_formats, sizeof rdp_audioin_supported_audio_formats);

	priv->audin_server_context->data = (void*)priv;
	priv->audin_server_context->Opening = rdp_audioin_client_opening;
	priv->audin_server_context->OpenResult = rdp_audioin_client_open_result;
	priv->audin_server_context->ReceiveSamples = rdp_audioin_client_receive_samples;
	priv->audin_server_context->num_server_formats = ARRAYSIZE(rdp_audioin_supported_audio_formats);
	priv->audin_server_context->server_formats = audio_formats;
	priv->audin_server_context->dst_format = &rdp_audioin_supported_audio_formats[0];
	priv->audin_server_context->frames_per_packet = rdp_audioin_supported_audio_formats[0].nSamplesPerSec / 100; // 10ms per packet

	priv->closeAudioSourceFd = eventfd(0, EFD_CLOEXEC);
	if (priv->closeAudioSourceFd < 0) {
		weston_log("RDPAudioIn - Couldn't initialize eventfd.\n");
		goto Error_Exit;
	}

	priv->pulseAudioSourceListenerFd = rdp_audioin_setup_listener(priv);
	if (priv->pulseAudioSourceListenerFd < 0) {
		weston_log("RDPAudioIn - rdp_audioin_setup_listener failed.\n");
		goto Error_Exit;
	}

	if (pthread_create(&priv->pulseAudioSourceThread, NULL, rdp_audioin_source_thread, (void*)priv) < 0) {
		weston_log("RDPAudioIn - Failed to start Pulse Audio Source Thread. No audio in will be available.\n");
		goto Error_Exit;
	}

	return priv;

Error_Exit:
	if (priv->debug)
		weston_log_scope_destroy(priv->debug);

	if (priv->pulseAudioSourceListenerFd != -1) {
		close(priv->pulseAudioSourceListenerFd);
		priv->pulseAudioSourceListenerFd = -1;
	}

	if (priv->closeAudioSourceFd != -1) {
		close(priv->closeAudioSourceFd);
		priv->closeAudioSourceFd = -1;
	}

	if (priv->audin_server_context) {
		audin_server_context_free(priv->audin_server_context);
		priv->audin_server_context = NULL;
	}
	free(priv);

	return NULL; // Continue without audio
}

void
rdp_audio_in_destroy(void *audio_in_private)
{
	struct audio_in_private *priv = audio_in_private;
	if (priv->audin_server_context) {

		if (priv->pulseAudioSourceThread) {
			priv->audioInExitSignal = TRUE;
			shutdown(priv->pulseAudioSourceListenerFd, SHUT_RDWR);
			shutdown(priv->closeAudioSourceFd, SHUT_RDWR);
			pthread_cancel(priv->pulseAudioSourceThread);
			pthread_join(priv->pulseAudioSourceThread, NULL);

			if (priv->pulseAudioSourceListenerFd != -1) {
				close(priv->pulseAudioSourceListenerFd);
				priv->pulseAudioSourceListenerFd = -1;
			}

			if (priv->closeAudioSourceFd != -1) {
				close(priv->closeAudioSourceFd);
				priv->closeAudioSourceFd = -1;
			}

			priv->pulseAudioSourceThread = 0;
		}

		assert(priv->pulseAudioSourceListenerFd < 0);
		assert(priv->closeAudioSourceFd < 0);

		assert(!priv->audin_server_context->IsOpen(priv->audin_server_context));
		audin_server_context_free(priv->audin_server_context);
		priv->audin_server_context = NULL;
	}
	free(priv);
}
