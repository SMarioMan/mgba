/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "AudioDevice.h"
#include "moc_AudioDevice.cpp"

#include "GBAApp.h"
#include "LogController.h"

#include <mgba/core/core.h>
#include <mgba/core/thread.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/sio/ring-thread.h>
#endif
#include <mgba/internal/gba/audio.h>

#include <QDebug>
#include <QDateTime>

using namespace QGBA;

AudioDevice::AudioDevice(QObject* parent)
	: QIODevice(parent)
	, m_context(nullptr)
{
	setOpenMode(ReadOnly);
    m_traceAudio = qEnvironmentVariableIsSet("MGBA_RING_AUDIO_TRACE");
    if (m_traceAudio) m_traceClock.start();
	mAudioBufferInit(&m_buffer, 0x4000, 2);
	mAudioBufferInit(&m_inputBuffer, 0x4000, 2);
	mAudioResamplerInit(&m_resampler, mINTERPOLATOR_SINC);
	m_updateTimer.setSingleShot(false);
	m_updateTimer.setInterval(1);
	connect(&m_updateTimer, &QTimer::timeout, this, &AudioDevice::update);
}

AudioDevice::~AudioDevice() {
	mAudioResamplerDeinit(&m_resampler);
	mAudioBufferDeinit(&m_buffer);
	mAudioBufferDeinit(&m_inputBuffer);
}

void AudioDevice::traceUnderrun() {
    if (!m_traceAudio) return;
    QMutexLocker lock(&m_audioMutex);
    if (!m_context || !m_context->core) return;
    mCoreSync* sync = &m_context->impl->sync;
    mCoreSyncLockAudio(sync);
    size_t source = mAudioBufferAvailable(m_context->core->getAudioBuffer(m_context->core));
    size_t threshold = sync->audioHighWater;
    mCoreSyncUnlockAudio(sync);
    qInfo("Audio underrun at %lld ms: device=%p delivered=%llu core=%zu input=%zu output=%zu threshold=%zu prefetchedBytes=%lld",
        static_cast<long long>(QDateTime::currentMSecsSinceEpoch()), static_cast<void*>(this),
        static_cast<unsigned long long>(deliveredFrames()), source,
        mAudioBufferAvailable(&m_inputBuffer), mAudioBufferAvailable(&m_buffer), threshold,
        static_cast<long long>(QIODevice::bytesAvailable()));
}

void AudioDevice::setFormat(const QAudioFormat& format) {
	QMutexLocker lock(&m_audioMutex);
	if (!m_context || !mCoreThreadIsActive(m_context)) {
		LOG(QT, INFO) << tr("Can't set format of context-less audio device");
		return;
	}
	mCoreSyncLockAudio(&m_context->impl->sync);
	mCore* core = m_context->core;
	mAudioResamplerSetSource(&m_resampler, core->getAudioBuffer(core), core->audioSampleRate(core), true);
	m_format = format;
	adjustResampler();
	mCoreSyncUnlockAudio(&m_context->impl->sync);
}

void AudioDevice::setBufferSamples(int samples) {
	QMutexLocker lock(&m_audioMutex);
	m_samples = samples;
}

void AudioDevice::setInput(mCoreThread* input) {
	QMutexLocker lock(&m_audioMutex);
	m_context = input;
#ifdef M_CORE_GBA
    // Topology changes recreate the audio output. Cache attachment while
    // input is assigned rather than racing the GUI's driver replacement
    // from a running sink callback.
    m_ringBuffered = GBASIORingThreadIsAttached(input);
#endif
}

qint64 AudioDevice::readData(char* data, qint64 maxSize) {
	QMutexLocker lock(&m_audioMutex);
	if (!m_context || !m_context->core) {
		LOG(QT, WARN) << tr("Audio device is missing its core");
		return 0;
	}

	if (!maxSize) {
		return 0;
	}

    processAudio();
    if (mAudioBufferAvailable(&m_buffer) < 32) {
        QThread::usleep(100);
        processAudio();
    }
	quint64 available = std::min<quint64>({
		mAudioBufferAvailable(&m_buffer),
		static_cast<quint64>(maxSize / sizeof(mStereoSample)),
		std::numeric_limits<int>::max()
	});
	mAudioBufferRead(&m_buffer, reinterpret_cast<int16_t*>(data), available);
	m_deliveredFrames.fetch_add(available);
    publishQueueDepth();
	m_updateTimer.start();
	return available * sizeof(mStereoSample);
}

qint64 AudioDevice::writeData(const char*, qint64) {
	LOG(QT, WARN) << tr("Writing data to read-only audio device");
	return 0;
}

bool AudioDevice::atEnd() const {
	return false;
}

qint64 AudioDevice::bytesAvailable() const {
	QMutexLocker lock(&m_audioMutex);
	if (!m_context || !m_context->core) {
		return true;
	}
	m_updateTimer.start();
	int available = mAudioBufferAvailable(&m_buffer);
	return available * sizeof(mStereoSample);
}

qint64 AudioDevice::bytesAvailable() {
	QMutexLocker lock(&m_audioMutex);
	if (!m_context || !m_context->core) {
		return true;
	}
    processAudio();
    int available = mAudioBufferAvailable(&m_buffer);
	m_updateTimer.start();
	return available * sizeof(mStereoSample);
}

void AudioDevice::update() {
    QMutexLocker lock(&m_audioMutex);
    if (!m_context || !m_context->core) return;
    if (m_traceAudio) {
        qint64 now = m_traceClock.nsecsElapsed();
        if (m_previousUpdate && now - m_previousUpdate > 40000000)
            qInfo("Audio timer gap: device=%p gap=%.2f ms", static_cast<void*>(this),
                  (now - m_previousUpdate) / 1000000.0);
        m_previousUpdate = now;
    }
    bool wasAvailable = mAudioBufferAvailable(&m_buffer);
    processAudio();
    bool becameAvailable = !wasAvailable && mAudioBufferAvailable(&m_buffer);
    if (mAudioBufferFull(&m_buffer)) {
        m_updateTimer.stop();
        m_previousUpdate = 0;
    }
    lock.unlock();
    if (becameAvailable) emit readyRead();
}

void AudioDevice::processAudio() {
    mCore* core = m_context->core;
    mCoreSync* sync = &m_context->impl->sync;
    qint64 beforeLock = m_traceAudio ? m_traceClock.nsecsElapsed() : 0;
    mCoreSyncLockAudio(sync);
    qint64 afterLock = m_traceAudio ? m_traceClock.nsecsElapsed() : 0;
    bool buffered = m_ringBuffered;
    adjustResampler();
    if (!buffered) {
        mAudioResamplerSetSource(&m_resampler, core->getAudioBuffer(core), core->audioSampleRate(core), true);
        mAudioResamplerProcess(&m_resampler);
        mCoreSyncConsumeAudio(sync);
        return;
    }
    mAudioBuffer* source = core->getAudioBuffer(core);
    size_t space = mAudioBufferCapacity(&m_inputBuffer) - mAudioBufferAvailable(&m_inputBuffer);
    int16_t copied[512 * 2];
    while (space && mAudioBufferAvailable(source)) {
        size_t count = std::min(space, size_t(512));
        count = mAudioBufferRead(source, copied, count);
        mAudioBufferWrite(&m_inputBuffer, copied, count);
        space -= count;
    }
    double sourceRate = core->audioSampleRate(core);
    size_t pending = mAudioBufferAvailable(&m_inputBuffer) +
        mAudioBufferAvailable(&m_buffer) * sourceRate / m_resampler.destRate;
    size_t budget = sync->audioHighWater;
    bool external = sync->externalPacing;
    sync->audioHighWater = budget > pending ? budget - pending : 1;
    mCoreSyncConsumeAudio(sync);
    qint64 afterCopy = m_traceAudio ? m_traceClock.nsecsElapsed() : 0;
    // Interpolation is protected by the consumer's mutex, not the emulation
    // producer mutex. A slow output device must not monopolize the ring CPU.
    if (external) {
        double target = budget;
        double correction = (double(pending) - target) / (target * 100);
        correction = std::max(-0.005, std::min(0.005, correction));
        sourceRate *= 1 + correction;
    }
    mAudioResamplerSetSource(&m_resampler, &m_inputBuffer, sourceRate, true);
    mAudioResamplerProcess(&m_resampler);
    if (m_traceAudio) {
        qint64 afterResample = m_traceClock.nsecsElapsed();
        if (afterResample - beforeLock > 20000000)
            qInfo("Audio processing delay: device=%p mutex=%.2f copy=%.2f resample=%.2f ms",
                  static_cast<void*>(this), (afterLock - beforeLock) / 1000000.0,
                  (afterCopy - afterLock) / 1000000.0, (afterResample - afterCopy) / 1000000.0);
    }
}

void AudioDevice::publishQueueDepth() {
#ifdef M_CORE_GBA
    if (!m_ringBuffered) return;
    mCore* core = m_context->core;
    mCoreSync* sync = &m_context->impl->sync;
    mCoreSyncLockAudio(sync);
    adjustResampler();
    double sourceRate = core->audioSampleRate(core);
    size_t pending = mAudioBufferAvailable(&m_inputBuffer) +
        mAudioBufferAvailable(&m_buffer) * sourceRate / m_resampler.destRate;
    size_t budget = sync->audioHighWater;
    sync->audioHighWater = budget > pending ? budget - pending : 1;
    // readData has just removed output samples. Release the reference CPU
    // immediately; waiting for the next GUI timer leaves its old full-queue
    // threshold in place even though the sink has already consumed that PCM.
    mCoreSyncConsumeAudio(sync);
#endif
}

void AudioDevice::adjustResampler() {
	mCore* core = m_context->core;
	double fauxClock = mCoreCalculateFramerateRatio(m_context->core, m_context->impl->sync.fpsTarget);
	mAudioResamplerSetDestination(&m_resampler, &m_buffer, m_format.sampleRate() * fauxClock);
	size_t samples = m_samples;
#ifdef M_CORE_GBA
	// One linked frame publishes eight outputs in a burst. A sub-frame
	// queue makes device callback jitter starve otherwise full-speed peers.
	if (m_ringBuffered)
		samples = std::max(samples, size_t(m_format.sampleRate() / 10));
#endif
	m_context->impl->sync.audioHighWater = samples + m_resampler.highWaterMark + m_resampler.lowWaterMark;
	m_context->impl->sync.audioHighWater *= core->audioSampleRate(core) / (m_format.sampleRate() * fauxClock);

	if (m_resampler.source) {
		size_t destSize = mAudioBufferCapacity(m_resampler.source);
		if (m_context->impl->sync.audioHighWater >= destSize) {
			m_context->impl->sync.audioHighWater = destSize;
		}
	}
}
