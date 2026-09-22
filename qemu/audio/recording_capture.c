/**
 * Copyright (C) 2026 MK124 and contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "recording_capture.h"

#include "audio/audio.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/notify.h"
#include "sysemu/sysemu.h"

#include "../recording_audio.h"

#include <sys/socket.h>

typedef struct RecordingCapture
{
	int fd;
	
	AudioState* audio;
	CaptureVoiceOut* voice;
	uint64_t nextTimeNs;
	
	Notifier exit;
} RecordingCapture;

static void sendStatus(RecordingCapture* capture, uint32_t kind)
{
	const M5RecordingAudio packet = { .kind = kind, .timeNs = m5RecordingTimeNs() };
	send(capture->fd, &packet, offsetof(M5RecordingAudio, samples), 0);
}

static void captureNotify(void* opaque, audcnotification_e command)
{
	RecordingCapture* capture = opaque;
	if (command == AUD_CNOTIFY_DISABLE) capture->nextTimeNs = 0;
}

static void captureDestroy(void* opaque) {}

static void capturePcm(void* opaque, const void* buffer, int size)
{
	RecordingCapture* capture = opaque;
	const int16_t* samples = buffer;
	unsigned frames = size / sizeof(int16_t);
	
	const uint64_t end = m5RecordingTimeNs();
	const uint64_t duration = (uint64_t) frames * 1000000000 / M5RecordingAudioRate;
	/* The backend can deliver adjacent blocks in a burst. Preserve their
	 * spacing; after a real gap, anchor to the host playback time again. */
	uint64_t time = MAX(capture->nextTimeNs, end > duration ? end - duration : 0);
	while (frames)
	{
		const unsigned count = MIN(frames, M5RecordingAudioFrames);
		M5RecordingAudio packet = { .timeNs = time, .frames = count, .kind = M5RecordingPcm };
		memcpy(packet.samples, samples, count * sizeof(int16_t));
		if (send(capture->fd, &packet, offsetof(M5RecordingAudio, samples) + count * sizeof(int16_t), 0) < 0)
		{
			/* Never stall the emulated CPU or physical audio for a recorder. */
			break;
		}
		samples += count;
		frames -= count;
		time += (uint64_t) count * 1000000000 / M5RecordingAudioRate;
	}
	capture->nextTimeNs = time;
}

static void captureCommand(void* opaque)
{
	RecordingCapture* capture = opaque;
	uint8_t enabled;
	while (recv(capture->fd, &enabled, 1, 0) == 1)
	{
		if (capture->voice)
		{
			AUD_del_capture(capture->voice, capture);
			capture->voice = NULL;
		}
		capture->nextTimeNs = 0;
		
		if (enabled)
		{
			struct audsettings settings = { M5RecordingAudioRate, 1, AUDIO_FORMAT_S16, HOST_BIG_ENDIAN };
			struct audio_capture_ops callbacks = { captureNotify, capturePcm, captureDestroy };
			capture->voice = AUD_add_capture(capture->audio, &settings, &callbacks, capture);
		}
		sendStatus(capture, enabled ? (capture->voice ? M5RecordingStarted : M5RecordingFailed) : M5RecordingStopped);
	}
}

static void captureExit(Notifier* notifier, void* data)
{
	RecordingCapture* capture = container_of(notifier, RecordingCapture, exit);
	notifier_remove(notifier);
	qemu_set_fd_handler(capture->fd, NULL, NULL, NULL);
	if (capture->voice) AUD_del_capture(capture->voice, capture);
	close(capture->fd);
	g_free(capture);
}

void m5RecordingCaptureInit(void)
{
	const char* descriptor = getenv("M5EMU_RECORDING_FD");
	if (!descriptor) return;
	
	char* end;
	const long fd = strtol(descriptor, &end, 10);
	if (!*descriptor || *end || fd < 0 || fd > INT_MAX) return;
	
	AudioState* audio = audio_state_by_name("audio", &error_warn);
	if (!audio) return;
	
	RecordingCapture* capture = g_new0(RecordingCapture, 1);
	capture->fd = fd;
	capture->audio = audio;
	capture->exit.notify = captureExit;
	
	qemu_set_fd_handler(fd, captureCommand, NULL, capture);
	qemu_add_exit_notifier(&capture->exit);
}
