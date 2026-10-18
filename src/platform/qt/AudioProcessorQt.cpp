/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "AudioProcessorQt.h"
#include "moc_AudioProcessorQt.cpp"

#include "AudioDevice.h"
#include "LogController.h"

#include <QAudioOutput>
#include <QDateTime>
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QMediaDevices>
#endif

#include <mgba/core/core.h>
#include <mgba/core/thread.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/sio/ring-thread.h>
#endif

using namespace QGBA;

AudioProcessorQt::AudioProcessorQt(QObject* parent)
	: AudioProcessor(parent)
{
    m_tracePump = qEnvironmentVariableIsSet("MGBA_RING_AUDIO_TRACE");
    m_pumpClock.start();
}

AudioProcessorQt::~AudioProcessorQt() {
    stop();
    if (m_audioThread) {
        m_audioThread->quit();
        m_audioThread->wait();
    }
}

void AudioProcessorQt::setInput(std::shared_ptr<CoreController> controller) {
	AudioProcessor::setInput(std::move(controller));
#ifdef M_CORE_GBA
    if (!m_worker && GBASIORingThreadIsAttached(input())) {
        // Eight windows must not make sink feeding wait for GUI painting or
        // settings dialogs. Create and destroy the sink, source and timers
        // on the same worker; retain the controller until stop has joined
        // every outstanding audio operation.
        stopOutput();
        m_audioThread = std::make_unique<QThread>();
        m_audioThread->setObjectName("mGBA ring audio");
        m_worker = new QObject;
        m_worker->moveToThread(m_audioThread.get());
        connect(m_audioThread.get(), &QThread::finished, m_worker, &QObject::deleteLater);
        m_audioThread->start();
    }
#endif
    auto updateInput = [this]() {
	if (m_device) {
		m_device->setInput(input());
		if (m_audioOutput) {
			m_device->setFormat(m_audioOutput->format());
		}
	}
    };
    if (dispatchToWorker()) QMetaObject::invokeMethod(m_worker, updateInput, Qt::BlockingQueuedConnection);
    else updateInput();
}

void AudioProcessorQt::stop() {
    if (dispatchToWorker()) QMetaObject::invokeMethod(m_worker, [this]() { stopOutput(); }, Qt::BlockingQueuedConnection);
    else stopOutput();
    AudioProcessor::stop();
}

void AudioProcessorQt::stopOutput() {
    m_audioPump.reset();
    m_pushDevice = nullptr;
    m_pendingPCM.clear();
    m_previousPump = 0;
    m_waitingForPCM = m_sinkHasPlayed = m_recoverQueued = false;
	if (m_audioOutput) {
		// Discard queued playback when a core stops. Draining a suspended
		// PulseAudio sink can wait forever because its stream is corked.
		m_audioOutput->reset();
		m_audioOutput.reset();
	}
	if (m_device) {
		m_device.reset();
	}
}

bool AudioProcessorQt::start() {
    if (dispatchToWorker()) {
        bool started = false;
        QMetaObject::invokeMethod(m_worker, [this, &started]() { started = start(); }, Qt::BlockingQueuedConnection);
        return started;
    }
	if (!input()) {
		LOG(QT, WARN) << tr("Can't start an audio processor without input");
		return false;
	}

	if (!m_device) {
		m_device = std::make_unique<AudioDevice>(m_worker ? m_worker : this);
	}

	if (!m_audioOutput) {
		QAudioFormat format;
		format.setSampleRate(m_sampleRate);
		format.setChannelCount(2);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
		format.setSampleSize(16);
		format.setCodec("audio/pcm");
		format.setByteOrder(QAudioFormat::Endian(QSysInfo::ByteOrder));
		format.setSampleType(QAudioFormat::SignedInt);

		m_audioOutput = std::make_unique<QAudioOutput>(format);
		m_audioOutput->setCategory("game");
#else
		format.setSampleFormat(QAudioFormat::Int16);

		QAudioDevice device(QMediaDevices::defaultAudioOutput());
		m_audioOutput = std::make_unique<QAudioSink>(device, format);
		LOG(QT, INFO) << tr("Audio outputting to %1").arg(device.description());
		int bufferBytes = m_sampleRate / 10;
#ifdef M_CORE_GBA
		if (GBASIORingThreadIsAttached(input())) bufferBytes *= sizeof(mStereoSample);
#endif
		m_audioOutput->setBufferSize(bufferBytes);
#endif
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        if (m_worker) m_audioOutput->setBufferSize(m_sampleRate / 10 * sizeof(mStereoSample));
#endif
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        connect(m_audioOutput.get(), &QAudioOutput::stateChanged, m_worker ? m_worker : this,
#else
        connect(m_audioOutput.get(), &QAudioSink::stateChanged, m_worker ? m_worker : this,
#endif
                [this](QAudio::State state) {
            // Qt backends differ in whether starvation also sets UnderrunError.
            // After delivery begins, any Idle transition means the sink ran dry.
            if (state == QAudio::ActiveState) m_sinkHasPlayed = true;
            if (state == QAudio::IdleState && m_sinkHasPlayed && !m_waitingForPCM && m_device && m_device->deliveredFrames()) {
                if (m_tracePump) qInfo("Qt ring sink idle at %lld ms: output=%p currentState=%d error=%d free=%lld buffer=%lld elapsed=%lld processed=%lld",
                    static_cast<long long>(QDateTime::currentMSecsSinceEpoch()), static_cast<void*>(this),
                    int(m_audioOutput->state()), int(m_audioOutput->error()),
                    static_cast<long long>(m_audioOutput->bytesFree()), static_cast<long long>(m_audioOutput->bufferSize()),
                    static_cast<long long>(m_audioOutput->elapsedUSecs()), static_cast<long long>(m_audioOutput->processedUSecs()));
                m_underrunEvents.fetch_add(1);
                m_device->traceUnderrun();
                if (m_audioPump && !m_recoverQueued) {
                    m_recoverQueued = true;
                    // A starved backend may keep its read clock ahead of the
                    // incoming stream. Restart its queue before re-priming,
                    // otherwise every small write can underflow again.
                    QMetaObject::invokeMethod(m_worker, [this]() {
                        if (!m_recoverQueued || !m_audioOutput) return;
                        m_recoverQueued = false;
                        m_pushDevice = nullptr;
                        m_waitingForPCM = true;
                        m_sinkHasPlayed = false;
                        m_audioOutput->reset();
                        primeOutput();
                    }, Qt::QueuedConnection);
                }
            }
        });
	}

    if (m_waitingForPCM && m_audioPump) {
        pumpAudio();
        m_audioPump->start();
        return true;
    }
	if (m_audioOutput->state() == QAudio::SuspendedState) {
		m_audioOutput->resume();
        if (m_audioPump) m_audioPump->start();
	} else {
		m_device->setBufferSamples(m_samples);
		m_device->setInput(input());
		m_device->setFormat(m_audioOutput->format());
        if (m_worker) {
            // Qt's pull adapter may feed only once per 20 ms. Independent
            // linked outputs need less callback jitter, without increasing
            // latency. Poll on the audio worker every 5 ms instead; this
            // also replaces each source's separate 1 ms refill timer.
            m_device->setPollingMode(true);
            m_recoverQueued = false;
            if (!primeOutput()) return false;
            if (!m_audioPump) {
                m_audioPump = std::make_unique<QTimer>(m_worker);
                m_audioPump->setTimerType(Qt::PreciseTimer);
                m_audioPump->setInterval(5);
                connect(m_audioPump.get(), &QTimer::timeout, m_worker, [this]() { pumpAudio(); });
            }
            pumpAudio();
            m_audioPump->start();
        } else m_audioOutput->start(m_device.get());
	}
    // A push sink starts idle until its paused core supplies the first PCM.
	return (m_waitingForPCM || m_audioOutput->state() == QAudio::ActiveState ||
            (m_pushDevice && m_audioOutput->state() == QAudio::IdleState)) &&
            m_audioOutput->error() == QAudio::NoError;
}

bool AudioProcessorQt::primeOutput() {
    m_waitingForPCM = true;
    m_sinkHasPlayed = false;
    m_pushDevice = m_audioOutput->start();
    if (!m_pushDevice) {
        m_waitingForPCM = false;
        return false;
    }
    // Open now so startup failures still reach the normal driver fallback,
    // but stop the device clock while an attached core is paused/loading.
    m_audioOutput->suspend();
    m_sinkHasPlayed = false;
    return true;
}

void AudioProcessorQt::pumpAudio() {
    if (!m_device || !m_audioOutput) return;
    qint64 started = m_tracePump ? m_pumpClock.nsecsElapsed() : 0;
    if (m_tracePump && m_previousPump && started - m_previousPump > 20000000)
        qInfo("Qt audio pump gap at %lld ms: output=%p gap=%.2f ms free=%lld buffer=%lld pending=%d",
              static_cast<long long>(QDateTime::currentMSecsSinceEpoch()), static_cast<void*>(this),
              (started - m_previousPump) / 1000000.0,
              static_cast<long long>(m_audioOutput->bytesFree()),
              static_cast<long long>(m_audioOutput->bufferSize()), int(m_pendingPCM.size()));
    m_previousPump = started;
    // Drain the emulation FIFO even while the hardware queue is full. Peer
    // cores follow the ring clock and cannot wait for their individual sink;
    // leaving PCM in their small core FIFO would discard generated samples.
    qint64 queued = m_device->bytesAvailable() + m_pendingPCM.size();
    if (m_waitingForPCM) {
        // A newly attached core can remain paused during menu/topology setup.
        // Starting the hardware clock then creates a lasting playback deficit
        // on an independently paced peer. Prime the existing 100 ms budget.
        size_t primeFrames = std::min(size_t(m_sampleRate / 10), m_device->bufferCapacity());
        if (queued < qint64(primeFrames * sizeof(mStereoSample))) return;
        m_sinkHasPlayed = false;
        // Start with the backend's normal prebuffering. Resuming the empty
        // probe stream can explicitly trigger playback before the first
        // write (PulseAudio does this), leaving its read clock ahead of PCM.
        m_audioOutput->reset();
        m_pushDevice = m_audioOutput->start();
        if (!m_pushDevice) return;
        m_waitingForPCM = false;
    }
    if (!m_pushDevice) return;
    qint64 sourceDone = m_tracePump ? m_pumpClock.nsecsElapsed() : 0;
    qint64 available = m_audioOutput->bytesFree();
    while (available >= qint64(sizeof(mStereoSample))) {
        if (m_pendingPCM.isEmpty()) {
            qint64 count = std::min<qint64>(available, 4096);
            count -= count % sizeof(mStereoSample);
            m_pendingPCM = m_device->read(count);
            if (m_pendingPCM.isEmpty()) break;
        }
        qint64 written = m_pushDevice->write(m_pendingPCM.constData(),
            std::min<qint64>(available, m_pendingPCM.size()));
        if (written <= 0) break;
        // A partial backend write retains the rest for the next callback.
        m_pendingPCM.remove(0, int(written));
        available = m_audioOutput->bytesFree();
    }
    if (m_tracePump) {
        qint64 finished = m_pumpClock.nsecsElapsed();
        if (finished - started > 20000000)
            qInfo("Qt audio pump delay at %lld ms: output=%p source=%.2f backend=%.2f ms free=%lld buffer=%lld",
                  static_cast<long long>(QDateTime::currentMSecsSinceEpoch()), static_cast<void*>(this),
                  (sourceDone - started) / 1000000.0, (finished - sourceDone) / 1000000.0,
                  static_cast<long long>(available), static_cast<long long>(m_audioOutput->bufferSize()));
    }
}

uint64_t AudioProcessorQt::deliveredFrames() const {
    if (dispatchToWorker()) {
        uint64_t frames = 0;
        QMetaObject::invokeMethod(m_worker, [this, &frames]() { frames = deliveredFrames(); }, Qt::BlockingQueuedConnection);
        return frames;
    }
    return m_device ? m_device->deliveredFrames() : 0;
}

void AudioProcessorQt::resetDeliveryStats() {
    if (dispatchToWorker()) {
        QMetaObject::invokeMethod(m_worker, [this]() { resetDeliveryStats(); }, Qt::BlockingQueuedConnection);
        return;
    }
    m_underrunEvents.store(0);
    if (m_device) m_device->resetDeliveryStats();
}

void AudioProcessorQt::pause() {
    if (dispatchToWorker()) {
        QMetaObject::invokeMethod(m_worker, [this]() { pause(); }, Qt::BlockingQueuedConnection);
        return;
    }
	if (m_audioPump) m_audioPump->stop();
	if (m_audioOutput) {
		m_audioOutput->suspend();
	}
}

void AudioProcessorQt::setBufferSamples(int samples) {
    if (dispatchToWorker()) {
        QMetaObject::invokeMethod(m_worker, [this, samples]() { setBufferSamples(samples); }, Qt::BlockingQueuedConnection);
        return;
    }
	m_samples = samples;
	if (m_device) {
		m_device->setBufferSamples(samples);
	}
}

void AudioProcessorQt::inputParametersChanged() {
    if (dispatchToWorker()) {
        QMetaObject::invokeMethod(m_worker, [this]() { inputParametersChanged(); }, Qt::BlockingQueuedConnection);
        return;
    }
	if (m_device) {
		m_device->setFormat(m_audioOutput->format());
		m_device->setBufferSamples(m_samples);
	}
}

void AudioProcessorQt::requestSampleRate(unsigned rate) {
    if (dispatchToWorker()) {
        QMetaObject::invokeMethod(m_worker, [this, rate]() { requestSampleRate(rate); }, Qt::BlockingQueuedConnection);
        return;
    }
	m_sampleRate = rate;
	if (m_device) {
		QAudioFormat format(m_audioOutput->format());
		format.setSampleRate(rate);
		m_device->setFormat(format);
	}
}

unsigned AudioProcessorQt::sampleRate() const {
    if (dispatchToWorker()) {
        unsigned rate = 0;
        QMetaObject::invokeMethod(m_worker, [this, &rate]() { rate = sampleRate(); }, Qt::BlockingQueuedConnection);
        return rate;
    }
	if (!m_audioOutput) {
		return 0;
	}
	return m_audioOutput->format().sampleRate();
}
