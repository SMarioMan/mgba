/* Copyright (c) 2013-2014 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include "AudioProcessor.h"
#include <atomic>
#include <QThread>

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
#include <QAudioOutput>
#else
#include <QAudioSink>
#include <QTimer>
#endif

class QAudioOutput;

namespace QGBA {

class AudioDevice;

class AudioProcessorQt : public AudioProcessor {
Q_OBJECT

public:
	AudioProcessorQt(QObject* parent = nullptr);
	~AudioProcessorQt() override;

	virtual unsigned sampleRate() const override;
    uint64_t deliveredFrames() const;
    uint64_t underrunEvents() const { return m_underrunEvents.load(); }
    void resetDeliveryStats();
    QThread* outputThread() const { return m_worker ? m_worker->thread() : thread(); }

public slots:
	virtual void setInput(std::shared_ptr<CoreController> input) override;
	virtual void stop() override;
	virtual bool start() override;
	virtual void pause() override;

	virtual void setBufferSamples(int samples) override;
	virtual void inputParametersChanged() override;

	virtual void requestSampleRate(unsigned) override;

private:
    bool dispatchToWorker() const { return m_worker && QThread::currentThread() != m_worker->thread(); }
    void stopOutput();
    std::unique_ptr<QThread> m_audioThread;
    QObject* m_worker = nullptr;
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
	std::unique_ptr<QAudioSink> m_audioOutput;
#else
	std::unique_ptr<QAudioOutput> m_audioOutput;
#endif
	std::unique_ptr<AudioDevice> m_device;
    std::atomic<uint64_t> m_underrunEvents{0};
	size_t m_samples = 1024;
	unsigned m_sampleRate = 44100;
};

}
