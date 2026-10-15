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
        connect(m_audioOutput.get(), &QAudioOutput::stateChanged, m_worker ? m_worker : this,
#else
        connect(m_audioOutput.get(), &QAudioSink::stateChanged, m_worker ? m_worker : this,
#endif
                [this](QAudio::State state) {
            // Qt backends differ in whether starvation also sets UnderrunError.
            // After delivery begins, any Idle transition means the sink ran dry.
            if (state == QAudio::IdleState && m_device && m_device->deliveredFrames()) {
                m_underrunEvents.fetch_add(1);
                m_device->traceUnderrun();
            }
        });
	}

	if (m_audioOutput->state() == QAudio::SuspendedState) {
		m_audioOutput->resume();
	} else {
		m_device->setBufferSamples(m_samples);
		m_device->setInput(input());
		m_device->setFormat(m_audioOutput->format());
		m_audioOutput->start(m_device.get());
	}
	return m_audioOutput->state() == QAudio::ActiveState && m_audioOutput->error() == QAudio::NoError;
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
