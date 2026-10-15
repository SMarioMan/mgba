/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "sdl-audio.h"

#include <mgba/core/core.h>
#include <mgba/core/thread.h>
#if defined(M_CORE_GBA) && !defined(DISABLE_THREADING)
#include <mgba/internal/gba/sio/ring-thread.h>
#endif

mLOG_DEFINE_CATEGORY(SDL_AUDIO, "SDL Audio", "platform.sdl.audio");

#if SDL_VERSION_ATLEAST(3, 0, 0)
static void _mSDLAudioCallback(void* context, SDL_AudioStream* stream, int additionalLen, int totalLen);
#else
static void _mSDLAudioCallback(void* context, Uint8* data, int len);
#endif

bool mSDLInitAudio(struct mSDLAudio* context, struct mCoreThread* threadContext) {
	context->ringBuffered = false;
#if defined(M_CORE_GBA) && !defined(DISABLE_THREADING)
	context->ringBuffered = GBASIORingThreadIsAttached(threadContext);
#endif
#if defined(_WIN32) && SDL_VERSION_ATLEAST(2, 0, 8) && !SDL_VERSION_ATLEAST(3, 0, 0)
	if (!getenv("SDL_AUDIODRIVER")) {
		_putenv_s("SDL_AUDIODRIVER", "directsound");
	}
#endif
	if (!SDL_OK(SDL_InitSubSystem(SDL_INIT_AUDIO))) {
		mLOG(SDL_AUDIO, ERROR, "Could not initialize SDL sound system: %s", SDL_GetError());
		return false;
	}

#if defined(M_CORE_GBA) && !defined(DISABLE_THREADING)
    /* Eight sub-frame audio devices otherwise wake the host hundreds of
     * times per second while one executor supplies every console. One
     * 1024-frame callback amortizes that cost without enlarging the queue. */
    if (context->ringBuffered && context->samples < 1024)
        context->samples = 1024;
#endif
	memset(&context->desiredSpec, 0, sizeof(context->desiredSpec));
	memset(&context->obtainedSpec, 0, sizeof(context->obtainedSpec));
	context->desiredSpec.freq = context->sampleRate;
	context->desiredSpec.channels = 2;
#if SDL_VERSION_ATLEAST(3, 0, 0)
	context->desiredSpec.format = SDL_AUDIO_S16;
	char hint[21];
	snprintf(hint, sizeof(hint), "%" PRIz "u", context->samples);
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, hint);
	context->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &context->desiredSpec, _mSDLAudioCallback, context);
	if (!context->stream) {
#else
	context->desiredSpec.callback = _mSDLAudioCallback;
	context->desiredSpec.userdata = context;
	context->desiredSpec.samples = context->samples;
	context->desiredSpec.format = AUDIO_S16SYS;
#if SDL_VERSION_ATLEAST(2, 0, 0)
	context->deviceId = SDL_OpenAudioDevice(0, 0, &context->desiredSpec, &context->obtainedSpec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (context->deviceId == 0) {
#else
	if (SDL_OpenAudio(&context->desiredSpec, &context->obtainedSpec) < 0) {
#endif
#endif
		mLOG(SDL_AUDIO, ERROR, "Could not open SDL sound system");
		return false;
	}
	context->core = NULL;

#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_GetAudioStreamFormat(context->stream, &context->obtainedSpec, NULL);
#endif
	if (context->obtainedSpec.channels == 0) {
		// This should never happen, but let's make sure
#if SDL_VERSION_ATLEAST(3, 0, 0)
		SDL_DestroyAudioStream(context->stream);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
		SDL_PauseAudioDevice(context->deviceId, 1);
		SDL_CloseAudioDevice(context->deviceId);
#else
		SDL_PauseAudio(1);
		SDL_CloseAudio();
#endif
		return false;
	}

	mAudioBufferInit(&context->buffer, context->samples, context->obtainedSpec.channels);
	mAudioBufferInit(&context->inputBuffer, 0x4000, context->obtainedSpec.channels);
	mAudioResamplerInit(&context->resampler, mINTERPOLATOR_SINC);
	mAudioResamplerSetDestination(&context->resampler, &context->buffer, context->obtainedSpec.freq);

	if (threadContext) {
		context->thread = threadContext;
		context->outputSamples = context->underrunSamples = 0;
		context->core = threadContext->core;
		context->sync = &threadContext->impl->sync;

		mSDLResumeAudio(context);
	}

	return true;
}

void mSDLDeinitAudio(struct mSDLAudio* context) {
	UNUSED(context);
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_DestroyAudioStream(context->stream);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_PauseAudioDevice(context->deviceId, 1);
	SDL_CloseAudioDevice(context->deviceId);
#else
	SDL_PauseAudio(1);
	SDL_CloseAudio();
#endif
	mAudioBufferDeinit(&context->buffer);
	mAudioBufferDeinit(&context->inputBuffer);
	mAudioResamplerDeinit(&context->resampler);
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void mSDLPauseAudio(struct mSDLAudio* context) {
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_PauseAudioStreamDevice(context->stream);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_PauseAudioDevice(context->deviceId, 1);
#else
	UNUSED(context);
	SDL_PauseAudio(1);
#endif
}

void mSDLResumeAudio(struct mSDLAudio* context) {
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_ResumeAudioStreamDevice(context->stream);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_PauseAudioDevice(context->deviceId, 0);
#else
	UNUSED(context);
	SDL_PauseAudio(0);
#endif
}

#if SDL_VERSION_ATLEAST(3, 0, 0)
static void _mSDLAudioCallback(void* context, SDL_AudioStream* stream, int additionalLen, int len) {
	UNUSED(additionalLen);
#else
static void _mSDLAudioCallback(void* context, Uint8* data, int len) {
#endif
	struct mSDLAudio* audioContext = context;
	if (!context || !audioContext->core) {
#if SDL_VERSION_ATLEAST(3, 0, 0)
		return;
#else
		memset(data, 0, len);
		return;
#endif
	}
	struct mAudioBuffer* buffer = audioContext->core->getAudioBuffer(audioContext->core);
	unsigned sampleRate = audioContext->core->audioSampleRate(audioContext->core);
	bool buffered = audioContext->ringBuffered;
	double fauxClock = 1;
	if (audioContext->sync) {
		if (audioContext->sync->fpsTarget > 0 && audioContext->core) {
			fauxClock = mCoreCalculateFramerateRatio(audioContext->core, audioContext->sync->fpsTarget);
		}
		mCoreSyncLockAudio(audioContext->sync);
		size_t queuedSamples = audioContext->samples;
#if defined(M_CORE_GBA) && !defined(DISABLE_THREADING)
		if (buffered && queuedSamples < (unsigned) audioContext->obtainedSpec.freq / 10)
			queuedSamples = audioContext->obtainedSpec.freq / 10;
#endif
		audioContext->sync->audioHighWater = queuedSamples + audioContext->resampler.highWaterMark + audioContext->resampler.lowWaterMark + (audioContext->samples >> 6);
		audioContext->sync->audioHighWater *= sampleRate / (fauxClock * audioContext->obtainedSpec.freq);

		if (audioContext->resampler.source) {
			size_t destSize = mAudioBufferCapacity(audioContext->resampler.source);
			if (audioContext->sync->audioHighWater >= destSize) {
				audioContext->sync->audioHighWater = destSize;
			}
		}
	}
    if (buffered && audioContext->sync) {
        int16_t copied[512 * 2];
        size_t space = mAudioBufferCapacity(&audioContext->inputBuffer) - mAudioBufferAvailable(&audioContext->inputBuffer);
        while (space && mAudioBufferAvailable(buffer)) {
            size_t count = space < 512 ? space : 512;
            count = mAudioBufferRead(buffer, copied, count);
            mAudioBufferWrite(&audioContext->inputBuffer, copied, count);
            space -= count;
        }
        size_t budget = audioContext->sync->audioHighWater;
        bool external = audioContext->sync->externalPacing;
        size_t queued = mAudioBufferAvailable(&audioContext->inputBuffer);
        size_t consumed = (len / (2 * audioContext->obtainedSpec.channels)) * sampleRate / (fauxClock * audioContext->obtainedSpec.freq);
        size_t remaining = queued > consumed ? queued - consumed : 0;
        audioContext->sync->audioHighWater = budget > remaining ? budget - remaining : 1;
        mCoreSyncConsumeAudio(audioContext->sync);
        /* Sinc interpolation is substantially more expensive than copying
         * PCM. It must not hold a producer mutex on the serialized ring CPU
         * executor, which would stall every console behind one audio device. */
        double sourceRate = sampleRate / fauxClock;
        // Only the reference output paces emulation. Other device clocks may
        // drift; a bounded, gentle resampling correction holds their queues.
        if (external) {
            double target = sampleRate / (10 * fauxClock);
            double correction = ((double) queued - target) / (target * 100);
            if (correction > 0.005) correction = 0.005;
            if (correction < -0.005) correction = -0.005;
            sourceRate *= 1 + correction;
        }
        mAudioResamplerSetSource(&audioContext->resampler, &audioContext->inputBuffer, sourceRate, true);
        mAudioResamplerProcess(&audioContext->resampler);
        mCoreSyncLockAudio(audioContext->sync);
        remaining = mAudioBufferAvailable(&audioContext->inputBuffer);
        audioContext->sync->audioHighWater = budget > remaining ? budget - remaining : 1;
        mCoreSyncConsumeAudio(audioContext->sync);
    } else {
        mAudioResamplerSetSource(&audioContext->resampler, buffer, sampleRate / fauxClock, true);
        mAudioResamplerProcess(&audioContext->resampler);
        if (audioContext->sync) mCoreSyncConsumeAudio(audioContext->sync);
    }
#if SDL_VERSION_ATLEAST(3, 0, 0)
	int16_t data[2048];
	while (len > 0) {
		int thisRead = sizeof(data);
		if (len < thisRead) {
			thisRead = len;
		}
		int available = mAudioBufferRead(&audioContext->buffer, data, thisRead / 4) * 4;
		if (!available) {
			break;
		}
		SDL_PutAudioStreamData(stream, data, available);
		len -= available;
	}
#else
	len /= 2 * audioContext->obtainedSpec.channels;
	int available = mAudioBufferRead(&audioContext->buffer, (int16_t*) data, len);

	audioContext->outputSamples += len;
	if (available < len) {
		audioContext->underrunSamples += len - available;
		memset(((short*) data) + audioContext->obtainedSpec.channels * available, 0, (len - available) * audioContext->obtainedSpec.channels * sizeof(short));
	}
#endif
}
