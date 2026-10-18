/* SPDX-License-Identifier: MPL-2.0 */
#include "platform/qt/ConfigController.h"
#include "platform/qt/CommandLineArguments.h"
#include "platform/qt/AudioProcessor.h"
#ifdef BUILD_QT_MULTIMEDIA
#include "platform/qt/AudioProcessorQt.h"
#endif
#include "platform/qt/CoreController.h"
#include "platform/qt/CoreManager.h"
#include "platform/qt/GBAApp.h"
#include "platform/qt/Window.h"
#include "platform/qt/DisplayGL.h"
#include "platform/qt/LogController.h"
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio/ring.h>
#include <mgba/feature/thread-proxy.h>
#include <mgba-util/vfs.h>
#include <QAction>
#include <QComboBox>
#include <QFile>
#include <QMenuBar>
#include <QDir>
#include <QEventLoop>
#include <mgba/core/input.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/input.h>
#include <QInputDialog>
#include <QTimer>
#include <QTemporaryDir>
#include <QPointer>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#ifdef BUILD_SDL
#include "platform/sdl/sdl-audio.h"
#endif

using namespace QGBA;

struct Inputs {
	CoreController* controller;
	QList<QPair<unsigned, unsigned>> events;
	int next = 0;
	unsigned held = 0;
    bool breakWire = false;
    Window* keyboardWindow = nullptr;
    unsigned firstSimulation = 0, lastSimulation = 0, speedStartFrame = 0;
    unsigned minSpeedFrame = 0;
    bool measureSpeed = false, deferMotion = false, speedComplete = false;
    unsigned finishScene = 0;
    // Publish milestones without interrupting eight producers on every GUI
    // poll. Measurement itself must not drain their host audio queues.
    std::shared_ptr<std::atomic<unsigned>> audioProgress = std::make_shared<std::atomic<unsigned>>(0);
    unsigned motionFrame = 3500, speedWindow = 600;
    std::chrono::steady_clock::time_point speedWallStart;
    std::chrono::steady_clock::time_point previousFrameWall;
    double wallFramesPerSecond = 0;
    double longestFrameGapMs = 0;
    unsigned delayedHostFrames = 0;
	static void frame(void* context) {
		Inputs* inputs = static_cast<Inputs*>(context);
		mCore* core = inputs->controller->thread()->core;
		unsigned frame = core->frameCounter(core);
        unsigned simulation = core->rawRead32(core, 0x03002E30, -1);
        bool racing = core->rawRead8(core, 0x03006BB5, -1) && simulation >= 128;
        if (inputs->measureSpeed && !inputs->speedComplete && racing && frame >= inputs->minSpeedFrame) {
            auto now = std::chrono::steady_clock::now();
            if (inputs->speedStartFrame) {
                double gap = std::chrono::duration<double, std::milli>(now - inputs->previousFrameWall).count();
                inputs->longestFrameGapMs = std::max(inputs->longestFrameGapMs, gap);
                if (gap > 25) ++inputs->delayedHostFrames;
            }
            inputs->previousFrameWall = now;
            if (!inputs->speedStartFrame) {
                inputs->speedStartFrame = frame;
                inputs->speedWallStart = std::chrono::steady_clock::now();
                inputs->firstSimulation = simulation;
                inputs->audioProgress->fetch_or(1, std::memory_order_release);
            } else if (frame >= inputs->speedStartFrame + inputs->speedWindow) {
                inputs->lastSimulation = simulation;
                inputs->wallFramesPerSecond = inputs->speedWindow / std::chrono::duration<double>(std::chrono::steady_clock::now() - inputs->speedWallStart).count();
                inputs->speedComplete = true;
                inputs->audioProgress->fetch_or(2, std::memory_order_release);
            }
        }
        if (inputs->finishScene && core->rawRead32(core, 0x03002E20, -1) == inputs->finishScene)
            inputs->audioProgress->fetch_or(4, std::memory_order_release);
        if (inputs->breakWire && frame >= 2600) {
            auto driver = reinterpret_cast<GBASIORingDriver*>(static_cast<GBA*>(core->board)->sio.driver);
            driver->ring->openInputs = frame < 2900 ? 1U << 4 : 0;
        }
		while (inputs->next < inputs->events.size() && inputs->events[inputs->next].first <= frame) {
            // Keep Battle stationary through the speed window; an eliminated
            // round's frozen counter must never be mistaken for half-speed.
            if (inputs->deferMotion && !inputs->speedComplete && inputs->events[inputs->next].first >= inputs->motionFrame) break;
			unsigned keys = inputs->events[inputs->next++].second;
            if (inputs->keyboardWindow) {
                unsigned previous = inputs->held;
                Window* window = inputs->keyboardWindow;
                QMetaObject::invokeMethod(window, [window, keys, previous]() {
                    const int keymap[10] = {Qt::Key_X, Qt::Key_Z, Qt::Key_Backspace, Qt::Key_Return,
                        Qt::Key_Right, Qt::Key_Left, Qt::Key_Up, Qt::Key_Down, Qt::Key_S, Qt::Key_A};
                    for (int i = 0; i < 10; ++i) {
                        if ((keys & (1U << i)) && !(previous & (1U << i))) QTest::keyPress(window, Qt::Key(keymap[i]));
                        if (!(keys & (1U << i)) && (previous & (1U << i))) QTest::keyRelease(window, Qt::Key(keymap[i]));
                    }
                }, Qt::QueuedConnection);
                inputs->held = keys;
                continue;
            }
			for (int key = 0; key < 10; ++key) {
				if ((keys & (1U << key)) && !(inputs->held & (1U << key))) inputs->controller->addKey(key);
				if (!(keys & (1U << key)) && (inputs->held & (1U << key))) inputs->controller->clearKey(key);
			}
			inputs->held = keys;
		}
	}
};

class RingGuiTest : public QObject {
Q_OBJECT
    Inputs inputs[8];
private slots:
    void workerLogDelivery() {
        LogController logger(mLOG_WARN);
        QSignalSpy messages(&logger, &LogController::logPosted);
        bool deliveredOnOwner = false;
        connect(&logger, &LogController::logPosted, &logger, [&]() {
            deliveredOnOwner = QThread::currentThread() == logger.thread();
        }, Qt::DirectConnection);
        int category = mLogCategoryById("platform.qt");
        QVERIFY(category >= 0);
        std::thread producer([&]() {
            logger.postLog(mLOG_DEBUG, category, "filtered worker message");
            logger.postLog(mLOG_WARN, category, "worker message");
        });
        producer.join();
        // The producer must enqueue without touching filters, file streams
        // or signal subscribers owned by this test's GUI thread.
        QCOMPARE(messages.count(), 0);
        QTRY_COMPARE(messages.count(), 1);
        QVERIFY(deliveredOnOwner);
        QCOMPARE(messages[0][2].toString(), QString("worker message"));
    }
    void threadedVideoFailedConsumer() {
#if defined(DISABLE_THREADING) || defined(MINIMAL_CORE)
        QSKIP("Threaded GBA rendering is disabled in this build");
#else
        mVideoThreadProxy proxy{};
        mVideoThreadProxyCreate(&proxy);
        proxy.d.paletteSize = proxy.d.vramSize = proxy.d.oamSize = 16;
        proxy.d.parsePacket = [](mVideoLogger*, const mVideoLoggerDirtyInfo*) { return false; };
        mVideoLoggerRendererInit(&proxy.d);
        mVideoLoggerDirtyInfo packet{};
        QVERIFY(proxy.d.writeData(&proxy.d, &packet, sizeof(packet)));
        bool stopped = false;
        for (int i = 0; i < 1000 && !stopped; ++i) {
            MutexLock(&proxy.mutex);
            stopped = proxy.threadState == PROXY_THREAD_STOPPED;
            ConditionWake(&proxy.toThreadCond);
            MutexUnlock(&proxy.mutex);
            if (!stopped) QTest::qWait(1);
        }
        // A failed worker must also be joined before freeing its queue.
        mVideoLoggerRendererDeinit(&proxy.d);
        QVERIFY(stopped);
        QCOMPARE(proxy.dirtyQueue.data, nullptr);
#endif
    }
    void threadedVideoReset() {
#if defined(DISABLE_THREADING) || defined(MINIMAL_CORE)
        QSKIP("Threaded GBA rendering is disabled in this build");
#else
        QByteArray rom = qEnvironmentVariable("MGBA_RING_GUI_BOOT_ROM").toUtf8();
        QByteArray bios = qEnvironmentVariable("MGBA_RING_GUI_BIOS").toUtf8();
        if (rom.isEmpty() || bios.isEmpty()) QSKIP("Set MGBA_RING_GUI_BOOT_ROM and MGBA_RING_GUI_BIOS");
        std::vector<mColor> pixels(240 * 160);
        auto destroy = [](mCore* core) { mCoreConfigDeinit(&core->config); core->deinit(core); };
        std::unique_ptr<mCore, decltype(destroy)> core(mCoreCreate(mPLATFORM_GBA), destroy);
        QVERIFY(core && core->init(core.get()));
        mCoreInitConfig(core.get(), "threaded-video-reset-test");
        mCoreConfigSetOverrideIntValue(&core->config, "threadedVideo", 1);
        mCoreConfigSetOverrideIntValue(&core->config, "useBios", 1);
        mCoreConfigSetOverrideIntValue(&core->config, "skipBios", 0);
        mCoreLoadConfig(core.get());
        core->setVideoBuffer(core.get(), pixels.data(), 240);
        VFile* vf = VFileOpen(bios.constData(), O_RDONLY);
        QVERIFY(vf);
        QVERIFY(core->loadBIOS(core.get(), vf, 0));
        QVERIFY(mCoreLoadFile(core.get(), rom.constData()));
        // Recreating an active proxy used to overwrite its consumer state
        // before stopping the rendering worker. Exercise both entry points.
        for (int reset = 0; reset < 4; ++reset) {
            core->reset(core.get());
            for (int frame = 0; frame < 30; ++frame) core->runFrame(core.get());
            core->reloadConfigOption(core.get(), "hwaccelVideo", nullptr);
            for (int frame = 0; frame < 30; ++frame) core->runFrame(core.get());
            QCOMPARE(core->frameCounter(core.get()), uint32_t(60));
            QVERIFY(static_cast<GBA*>(core->board)->biosVf);
        }
#endif
    }
    void writableArguments() {
        // Include power-of-two argc values: the old Windows vector then had
        // no spare allocation for the mandatory terminating pointer.
        for (int count = 1; count <= 32; ++count) {
            QStringList strings;
            for (int i = 0; i < count; ++i)
                strings.append(QString::fromUtf8("C:/Games/マリオ カート %1.gba").arg(i));
            CommandLineArguments arguments(strings);
            char** argv = arguments.data();
            QCOMPARE(argv[count], nullptr);
            for (int i = 0; i < count; ++i) QCOMPARE(QByteArray(argv[i]), strings[i].toUtf8());
            // Simulate Qt removing an option, leaving a repeated pointer.
            // Destruction must free each original buffer exactly once.
            if (count > 1) argv[0] = argv[count - 1];
            argv[count] = nullptr;
        }
    }
    void borrowedFailureLifetime() {
        // A crashed or exiting borrowed core must remain exclusively owned
        // until its callback returns, before the GUI can detach its cable.
        for (bool batch : {false, true}) for (bool shutdown : {false, true}) {
            struct Gate {
                std::mutex mutex;
                std::condition_variable condition;
                bool entered = false, release = false;
                bool shutdown;
                std::atomic<bool> interrupted{false};
            } gate;
            gate.shutdown = shutdown;
            mCore* core = mCoreCreate(mPLATFORM_GBA);
            QVERIFY(core && core->init(core));
            mCoreConfigInit(&core->config, nullptr);
            mCoreThread thread{};
            thread.core = core;
            thread.userData = &gate;
            thread.resetOverride = [](mCoreThread*) {};
            thread.runLoopOverride = [](mCoreThread* t) { mCoreThreadWaitFromThread(t); };
            QVERIFY(mCoreThreadStart(&thread));
            bool waiting = false;
            for (unsigned i = 0; i < 1000 && !waiting; ++i) {
                MutexLock(&thread.impl->stateMutex);
                waiting = thread.impl->waitingOnCondition && thread.impl->requested == mTHREAD_REQ_WAIT;
                MutexUnlock(&thread.impl->stateMutex);
                if (!waiting) QTest::qWait(1);
            }
            QVERIFY(waiting);
            bool borrowed = false, ranRevoked = false;
            std::thread worker([&] {
                ThreadCallback action = [](mCoreThread* t) {
                    Gate* g = static_cast<Gate*>(t->userData);
                    if (g->shutdown) mCoreThreadEnd(t);
                    else mCoreThreadMarkCrashed(t);
                    std::unique_lock<std::mutex> lock(g->mutex);
                    g->entered = true;
                    g->condition.notify_all();
                    g->condition.wait(lock, [&] { return g->release; });
                };
                if (batch) {
                    borrowed = mCoreThreadBorrowWaiting(&thread);
                    if (borrowed) {
                        borrowed = mCoreThreadRunBorrowed(&thread, action);
                        ranRevoked = mCoreThreadRunBorrowed(&thread, [](mCoreThread*) {});
                        mCoreThreadReturnBorrowed(&thread);
                    }
                } else borrowed = mCoreThreadRunWaiting(&thread, action);
            });
            {
                std::unique_lock<std::mutex> lock(gate.mutex);
                gate.condition.wait(lock, [&] { return gate.entered; });
            }
            std::thread gui([&] { mCoreThreadInterrupt(&thread); gate.interrupted = true; });
            QTest::qWait(25);
            bool premature = gate.interrupted;
            {
                std::lock_guard<std::mutex> lock(gate.mutex);
                gate.release = true;
                gate.condition.notify_all();
            }
            worker.join();
            gui.join();
            mCoreThreadContinue(&thread);
            mCoreThreadEnd(&thread);
            mCoreThreadJoin(&thread);
            core->deinit(core);
            QVERIFY(borrowed);
            QVERIFY(!ranRevoked);
            QVERIFY2(!premature, "GUI interruption returned before the borrowed failure callback finished");
        }
    }
    void scriptedLaunch() {
        auto app = GBAApp::app();
        app->setQuitOnLastWindowClosed(false);
        QTemporaryDir settings;
        QString rom = qEnvironmentVariable("MGBA_RING_GUI_BOOT_ROM");
        if (rom.isEmpty()) QSKIP("Set MGBA_RING_GUI_BOOT_ROM to a --boot-ring ROM");
        QByteArray directory = settings.path().toUtf8(), romArg = rom.toUtf8();
        char arg0[] = "mgba-qt", flag[] = "--ring-players", count[] = "8";
        char profile[] = "--instance-settings", hidden[] = "--hide-menu", biosFlag[] = "--bios";
        QByteArray bios = qEnvironmentVariable("MGBA_RING_GUI_BIOS").toUtf8();
        config->setOption("gba.bios", "");
        char* argv[] = {arg0, flag, count, profile, directory.data(), hidden, biosFlag, bios.data(), romArg.data(), nullptr};
        QVERIFY(config->parseArguments(9, argv));
        QVERIFY(app->multiplayer()->setRingTopology(true));
        QVERIFY(app->newWindow());
        app->initMultiplayer();
        QCOMPARE(app->windows().size(), 8);
        // A short startup replay isolates backend initialization from ROM menu
        // input. The ordinary full gameplay/settings regression stays intact.
        int startupFrames = qEnvironmentVariableIntValue("MGBA_RING_GUI_STARTUP_FRAMES");
        if (startupFrames) {
            QVERIFY(startupFrames >= 60 && startupFrames <= 5100);
            for (Window* window : app->windows()) {
                auto controller = window->controller();
                QVERIFY(controller);
                if (qEnvironmentVariableIntValue("MGBA_RING_GUI_HARDWARE"))
                    QVERIFY2(controller->supportsFeature(CoreController::Feature::OPENGL), "Hardware GBA renderer is disabled in this build");
                QTRY_VERIFY_WITH_TIMEOUT(controller->hasStarted(), 10000);
                QTRY_VERIFY_WITH_TIMEOUT(controller->frameCounter() >= uint64_t(startupFrames), 120000);
                if (qEnvironmentVariableIntValue("MGBA_RING_GUI_HARDWARE"))
                    QTRY_VERIFY_WITH_TIMEOUT(controller->hardwareAccelerated(), 10000);
                QVERIFY(!mCoreThreadHasCrashed(controller->thread()));
                CoreController::Interrupter interrupter(controller.get());
                auto gba = static_cast<GBA*>(controller->thread()->core->board);
                QVERIFY(gba->biosVf);
                if (qEnvironmentVariableIntValue("MGBA_RING_GUI_HARDWARE")) {
                    QVERIFY(controller->hardwareAccelerated());
                    // OPENGL_MAGIC from the actual GBA renderer, rather than
                    // merely checking that the build supports acceleration.
                    QCOMPARE(gba->video.renderer->rendererId(gba->video.renderer), uint32_t(0x6E726C67));
                }
            }
            if (qEnvironmentVariableIntValue("MGBA_RING_GUI_RESET_BACKENDS")) {
                for (Window* window : app->windows()) {
                    auto controller = window->controller();
                    QSignalSpy reset(controller.get(), &CoreController::didReset);
                    controller->reset();
                    QTRY_VERIFY_WITH_TIMEOUT(!reset.isEmpty(), 10000);
                    QTRY_VERIFY_WITH_TIMEOUT(controller->frameCounter() >= 60 && controller->frameCounter() < 100000, 30000);
                    CoreController::Interrupter interrupter(controller.get());
                    QVERIFY(!mCoreThreadHasCrashed(controller->thread()));
                    auto gba = static_cast<GBA*>(controller->thread()->core->board);
                    if (qEnvironmentVariableIntValue("MGBA_RING_GUI_HARDWARE"))
                        QCOMPARE(gba->video.renderer->rendererId(gba->video.renderer), uint32_t(0x6E726C67));
                }
            }
            for (Window* window : app->windows()) window->close();
            QTRY_COMPARE_WITH_TIMEOUT(app->windows().size(), 0, 10000);
            QTRY_COMPARE_WITH_TIMEOUT(app->multiplayer()->attached(), 0, 10000);
            return;
        }
        QFile fixture(qEnvironmentVariable("MGBA_RING_GUI_INPUTS") + "/vs-8.csv");
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        for (Inputs& input : inputs) input = {};
        while (!fixture.atEnd()) {
            auto fields = fixture.readLine().trimmed().split(',');
            if (fields.size() != 3) continue;
            unsigned frame = fields[0].toUInt(), slot = fields[1].toUInt();
            if (frame < 1500) continue; // This ROM boots directly into Ring Link.
            inputs[slot].events.append(qMakePair(frame >= 2000 ? frame + 600 : frame, fields[2].toUInt(nullptr,16)));
        }
        auto windows = app->windows();
        for (int i = 0; i < 8; ++i) {
            Window* window = windows[i];
            auto controller = window->controller();
            QVERIFY(controller);
            QCOMPARE(window->config()->getOption("bios"), QString::fromUtf8(bios));
            QTRY_VERIFY_WITH_TIMEOUT(controller->hasStarted(), 10000);
#if defined(BUILD_GL) || defined(BUILD_GLES2) || defined(BUILD_GLES3) || defined(USE_EPOXY)
            if (qEnvironmentVariableIntValue("MGBA_RING_GUI_DISPLAY") == 1)
                QVERIFY2(window->findChild<DisplayGL*>(), "Requested OpenGL display was not created");
#endif
            QVERIFY(window->windowTitle().endsWith(QString(" - P%1").arg(i+1)));
            QVERIFY(!window->menuBar()->isVisible());
            inputs[i].controller = controller.get();
            inputs[i].measureSpeed = true;
            inputs[i].minSpeedFrame = 4300;
            if (!i) inputs[i].keyboardWindow = window;
            mCoreCallbacks callbacks{};
            callbacks.context = &inputs[i]; callbacks.videoFrameStarted = Inputs::frame;
            CoreController::Interrupter interrupter(controller.get());
            controller->thread()->core->addCoreCallbacks(controller->thread()->core, &callbacks);
        }
        // Alt reveals the menu in a window, without entering fullscreen.
        QTest::keyClick(windows[0], Qt::Key_Alt);
        QVERIFY(windows[0]->menuBar()->isVisible());
        windows[0]->showMenu(false);
        QElapsedTimer timer; timer.start();
        bool finished = false;
        while (!finished && timer.elapsed() < 600000) {
            QTest::qWait(50);
            finished = true;
            for (Window* w : windows) {
                QVERIFY(!mCoreThreadHasCrashed(w->controller()->thread()));
                QVERIFY(!mCoreThreadHasExited(w->controller()->thread()));
                if (w->controller()->frameCounter() < 5100) finished = false;
            }
        }
        QVERIFY(finished);
        for (int i = 0; i < 8; ++i) {
            auto controller = windows[i]->controller();
            CoreController::Interrupter interrupter(controller.get());
            auto core = controller->thread()->core;
            QVERIFY(static_cast<GBA*>(core->board)->biosVf);
            QCOMPARE(core->rawRead8(core, 0x0300361B, -1), 8U);
            QCOMPARE(core->rawRead32(core, 0x03006B98, -1), 0U);
            QCOMPARE(core->rawRead32(core, 0x03006B9C, -1), 0U);
            QVERIFY(inputs[i].speedComplete);
            double rate = double(inputs[i].lastSimulation - inputs[i].firstSimulation) / inputs[i].speedWindow;
            qInfo("Scripted P%d speed %.3f native updates/video frame", i+1, rate);
            QVERIFY(rate >= 0.95);
            // Independently persist each keyboard map, then load it afresh.
            mInputBindKey(windows[i]->inputController()->map(), InputController::KEYBOARD, Qt::Key_F1+i, GBA_KEY_A);
            windows[i]->inputController()->saveConfiguration(InputController::KEYBOARD);
            ConfigController reloaded(nullptr, QDir(settings.path()).filePath(QString("P%1").arg(i+1)));
            mInputMap map; mInputMapInit(&map, &GBAInputInfo);
            QVERIFY(mInputMapLoad(&map, InputController::KEYBOARD, reloaded.input()));
            QCOMPARE(mInputMapKey(&map, InputController::KEYBOARD, Qt::Key_F1+i), int(GBA_KEY_A));
            if (i) QCOMPARE(mInputMapKey(&map, InputController::KEYBOARD, Qt::Key_F1), -1);
            mInputMapDeinit(&map);
        }
        QString closedDirectory = windows[1]->config()->instanceDirectory();
        windows[1]->close();
        QTRY_COMPARE_WITH_TIMEOUT(app->windows().size(), 7, 10000);
        Window* replacement = app->newWindow();
        QVERIFY(replacement);
        QCOMPARE(replacement->config()->instanceDirectory(), closedDirectory);
        QSet<QString> directories;
        for (Window* w : app->windows()) directories.insert(w->config()->instanceDirectory());
        QCOMPARE(directories.size(), 8);

    }
	void menuLifetime() {
		GBAApp::app()->setQuitOnLastWindowClosed(false);
		for (int i = 0; i < 3; ++i) {
			Window* window = GBAApp::app()->newWindow();
			QVERIFY(window);
			window->close();
			QTRY_COMPARE(GBAApp::app()->windows().size(), 0);
		}
	}
	void cleanup() {
		for (Window* window : GBAApp::app()->windows()) window->close();
		QTRY_COMPARE_WITH_TIMEOUT(GBAApp::app()->multiplayer()->attached(), 0, 10000);
		QTRY_COMPARE_WITH_TIMEOUT(GBAApp::app()->windows().size(), 0, 10000);
	}
	void ring_data() {
		QTest::addColumn<int>("players");
		QTest::addColumn<QString>("mode");
		QTest::addColumn<bool>("ringTopology");
		QTest::addColumn<bool>("lateBoot");
        QTest::addColumn<bool>("wireLoss");
		for (int players : {2, 8}) for (const QString& mode : {QString("vs"), QString("battle")})
			QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(players))) << players << mode << true << false << false;
        if (qEnvironmentVariableIntValue("MGBA_RING_GUI_VISUAL")) {
            for (int players = 2; players <= 8; ++players) {
                for (const QString& mode : {QString("vs"), QString("battle"), QString("gp")}) {
                    if ((players == 2 || players == 8) && mode != "gp") continue;
                    QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(players))) << players << mode << true << false << false;
                }
            }
        }
		QTest::newRow("vs-8-loss-start") << 8 << QString("vs") << true << false << true;
        QTest::newRow("vs-2-late") << 2 << QString("vs") << true << true << false;
		for (const QString& mode : {QString("vs"), QString("battle")})
			QTest::newRow(qPrintable(QString("stock-%1-4").arg(mode))) << 4 << mode << false << false << false;
	}

	void ring() {
		if (qEnvironmentVariableIsEmpty("MGBA_RING_GUI_ROM") || qEnvironmentVariableIsEmpty("MGBA_RING_GUI_INPUTS"))
			QSKIP("Set MGBA_RING_GUI_ROM, MGBA_RING_GUI_BIOS, and MGBA_RING_GUI_INPUTS to private validation fixtures");
		QFETCH(int, players);
		QFETCH(QString, mode);
		QFETCH(bool, ringTopology);
        bool qtAudioCheck = qEnvironmentVariable("MGBA_RING_GUI_AUDIO") == "qt";
        bool audioCheck = qtAudioCheck || qEnvironmentVariableIntValue("MGBA_RING_GUI_AUDIO") != 0;
        bool sdlAudioCheck = audioCheck && !qtAudioCheck;
        unsigned finishScene = qEnvironmentVariable("MGBA_RING_GUI_FINISH_SCENE").toUInt(nullptr, 0);
		QFETCH(bool, lateBoot);
        QFETCH(bool, wireLoss);
		GBAApp* app = GBAApp::app();
		app->setQuitOnLastWindowClosed(false);
		CoreManager manager;
		QTemporaryDir romDirectory;
		QString rom = romDirectory.filePath("mksc-ring.gba");
		QVERIFY(QFile::copy(qEnvironmentVariable("MGBA_RING_GUI_ROM"), rom));
		// Give temporary test ROMs explicit output paths, as the normal GUI's
		// temporary-file guard otherwise opens a relocation dialog.
		for (const char* key : {"savegamePath", "savestatePath", "screenshotPath", "cheatsPath"})
			config->setOption(key, romDirectory.path().toUtf8().constData());
		manager.setMultiplayerController(app->multiplayer());
		manager.setConfig(config->config());
		Window* first = app->newWindow();
		QVERIFY(first);
		// Exercise the actual File-menu topology dialog, including its selection.
		QAction* topology = nullptr;
		for (QAction* action : first->findChildren<QAction*>()) if (action->text() == "GBA link topology...") topology = action;
		QVERIFY(topology);
		QTimer::singleShot(0, [ringTopology]() {
			auto dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
			if (dialog) { dialog->findChild<QComboBox*>()->setCurrentIndex(ringTopology ? 1 : 0); dialog->accept(); }
		});
		topology->trigger();
		QCOMPARE(app->multiplayer()->ringTopology(), ringTopology);
		QCOMPARE(app->multiplayer()->maxPlayers(), ringTopology ? 8 : 4);
		if (!ringTopology && qEnvironmentVariableIsEmpty("MGBA_RING_GUI_STOCK_INPUTS")) QSKIP("Set MGBA_RING_GUI_STOCK_INPUTS to the stock input fixtures directory");
        QString fixturePath = qEnvironmentVariable("MGBA_RING_GUI_REPLAY");
        if (fixturePath.isEmpty()) fixturePath = ringTopology ? qEnvironmentVariable("MGBA_RING_GUI_INPUTS") + QString("/%1-%2.csv").arg(mode).arg(players)
                                                             : qEnvironmentVariable("MGBA_RING_GUI_STOCK_INPUTS") + QString("/stock-%1.csv").arg(mode);
		QFile fixture(fixturePath);
		QVERIFY(fixture.open(QIODevice::ReadOnly));
		for (Inputs& input : inputs) input = {};
		for (const QByteArray& row : fixture.readAll().split('\n')) {
			auto columns = row.split(',');
			if (columns.size() != 3) continue;
			unsigned slot = columns[1].toUInt();
			QVERIFY(slot < unsigned(players));
			unsigned frame = columns[0].toUInt();
			// Enter the first console's title menu normally before waiting; a
			// delayed initial Start would confirm the default Single Player item.
            unsigned shiftedFrame = frame + (lateBoot && !slot && frame > 715 ? 760 : 0);
            if (frame == 3500) inputs[slot].motionFrame = shiftedFrame;
			inputs[slot].events.append(qMakePair(shiftedFrame, columns[2].toUInt(nullptr, 16)));
		}
		QList<Window*> windows;
		for (int i = 0; i < players; ++i) {
			Window* window = i ? app->newWindow() : first;
			QVERIFY(window);
			windows.append(window);
			CoreController* controller = manager.loadGame(rom);
			QVERIFY(controller);
			inputs[i].controller = controller;
            if (qEnvironmentVariableIntValue("MGBA_RING_GUI_VISUAL")) {
                unsigned lastEventFrame = 0;
                for (const Inputs& input : inputs) {
                    if (!input.events.isEmpty()) lastEventFrame = std::max(lastEventFrame, input.events.last().first);
                }
                // A transition fixture may leave its first race much later.
                // Measure the final scene only after replaying the full CSV.
                inputs[i].minSpeedFrame = lastEventFrame + 180;
            }
            inputs[i].breakWire = wireLoss && !i;
            inputs[i].finishScene = finishScene;
            inputs[i].measureSpeed = ringTopology && !wireLoss;
            inputs[i].deferMotion = ringTopology && !wireLoss && !audioCheck && !qEnvironmentVariableIntValue("MGBA_RING_GUI_MOTION");
            if (qEnvironmentVariableIntValue("MGBA_RING_GUI_MOTION")) inputs[i].speedWindow = 3600;
            if (audioCheck) inputs[i].speedWindow = std::max(600, qEnvironmentVariableIsSet("MGBA_RING_GUI_AUDIO_FRAMES") ? qEnvironmentVariableIntValue("MGBA_RING_GUI_AUDIO_FRAMES") : 3600);
			controller->setPaused(true);
			mCoreCallbacks callbacks{};
			callbacks.context = &inputs[i];
			callbacks.videoFrameStarted = Inputs::frame;
			controller->thread()->core->addCoreCallbacks(controller->thread()->core, &callbacks);
			window->setController(controller);
			QTRY_VERIFY_WITH_TIMEOUT(controller->hasStarted(), 10000);
#if defined(BUILD_GL) || defined(BUILD_GLES2) || defined(BUILD_GLES3) || defined(USE_EPOXY)
            if (qEnvironmentVariableIntValue("MGBA_RING_GUI_DISPLAY") == 1)
                QVERIFY2(window->findChild<DisplayGL*>(), "Requested OpenGL display was not created");
#endif
            // The core starts before Window::gameStarted's queued signal.
            // Wait for its audio setup before topology changes can recreate
            // the output; otherwise the test retains a startup-era pointer.
            QTRY_VERIFY_WITH_TIMEOUT(window->audioProcessor(), 10000);
            if (qEnvironmentVariableIntValue("MGBA_RING_GUI_HARDWARE")) {
                QVERIFY2(controller->supportsFeature(CoreController::Feature::OPENGL), "Hardware GBA renderer is disabled in this build");
                QTRY_VERIFY_WITH_TIMEOUT(controller->hardwareAccelerated(), 10000);
                CoreController::Interrupter interrupter(controller);
                auto gba = static_cast<GBA*>(controller->thread()->core->board);
                QCOMPARE(gba->video.renderer->rendererId(gba->video.renderer), uint32_t(0x6E726C67));
            }
            if (qEnvironmentVariableIsSet("MGBA_RING_GUI_THREADED") &&
                qEnvironmentVariableIsSet("MGBA_RING_GUI_HARDWARE") &&
                !qEnvironmentVariableIntValue("MGBA_RING_GUI_HARDWARE")) {
                CoreController::Interrupter interrupter(controller);
                // With software GBA rendering, the native worker owns the
                // core's video logger. Verify the active path on every console,
                // rather than merely checking the inherited setting.
                QCOMPARE(controller->thread()->core->videoLogger != nullptr,
                         qEnvironmentVariableIntValue("MGBA_RING_GUI_THREADED") != 0);
            }
			if (lateBoot && !i) {
				controller->setPaused(false);
				// The GUI counter briefly holds UINT64_MAX during reset. Wait for
				// an actual completed boot before opening the later console.
				QTRY_VERIFY_WITH_TIMEOUT(controller->frameCounter() >= 760 && controller->frameCounter() < 100000, 30000);
				controller->setPaused(true);
				QTRY_VERIFY_WITH_TIMEOUT(controller->isPaused(), 10000);
				CoreController::Interrupter interrupter(controller);
				unsigned bootFrames = controller->thread()->core->frameCounter(controller->thread()->core);
				QVERIFY(bootFrames >= 760);
				for (auto& event : inputs[0].events) if (event.first > 715) event.first += bootFrames - 760;
                inputs[0].motionFrame += bootFrames - 760;
			}
		}
		QCOMPARE(app->multiplayer()->attached(), players);
        if (qEnvironmentVariableIntValue("MGBA_RING_GUI_VISUAL")) {
            QScreen* screen = QGuiApplication::primaryScreen();
            QVERIFY(screen);
            QRect area = screen->availableGeometry();
            int columns = players <= 2 ? players : players <= 6 ? 3 : 4;
            int rows = (players + columns - 1) / columns;
            for (int i = 0; i < players; ++i) {
                windows[i]->resize(area.width() / columns - 12, area.height() / rows - 36);
                windows[i]->move(area.left() + (i % columns) * area.width() / columns,
                                 area.top() + (i / columns) * area.height() / rows);
                QVERIFY(windows[i]->isVisible());
            }
        }
		if (ringTopology && players == 2) {
			QVERIFY(app->multiplayer()->setRingTopology(false));
			QVERIFY(app->multiplayer()->setRingTopology(true));
		}
        bool audioMeasured = false;
        uint64_t initialDroppedSamples[8] = {};
#ifdef BUILD_SDL
        struct AudioOutput {
            mSDLAudio device{};
            uint64_t initialOutputSamples = 0;
            uint64_t initialUnderrunSamples = 0;
            bool active = false;
            ~AudioOutput() { if (active) mSDLDeinitAudio(&device); }
        } audio[8];
        if (sdlAudioCheck) {
            for (int i = 0; i < players; ++i) {
                // Replace the window's dummy consumer; two readers of the
                // same FIFO would manufacture underruns in this measurement.
                QTRY_VERIFY_WITH_TIMEOUT(windows[i]->audioProcessor(), 10000);
                AudioProcessor* previousAudio = windows[i]->audioProcessor();
                previousAudio->stop();
                QObject::disconnect(windows[i]->controller().get(), nullptr, previousAudio, nullptr);
                audio[i].device.samples = 512;
                audio[i].device.sampleRate = 48000;
                audio[i].active = mSDLInitAudio(&audio[i].device, windows[i]->controller()->thread());
                QVERIFY2(audio[i].active, SDL_GetError());
                CoreController::Interrupter interrupter(windows[i]->controller());
                windows[i]->controller()->thread()->impl->sync.audioWait = true;
            }
        }
#else
        QVERIFY2(!sdlAudioCheck, "SDL delivery validation requires the SDL backend");
#endif
#ifdef BUILD_QT_MULTIMEDIA
        QPointer<AudioProcessorQt> qtAudio[8];
        if (qtAudioCheck) {
            for (int i = 0; i < players; ++i) {
                QTRY_VERIFY_WITH_TIMEOUT(windows[i]->audioProcessor(), 10000);
                qtAudio[i] = qobject_cast<AudioProcessorQt*>(windows[i]->audioProcessor());
                QVERIFY(qtAudio[i]);
                QVERIFY2(qtAudio[i]->outputThread() != QThread::currentThread(),
                         "Ring output must not depend on the GUI event loop");
                CoreController::Interrupter interrupter(windows[i]->controller());
                windows[i]->controller()->thread()->impl->sync.audioWait = true;
            }
        }
#else
        QVERIFY2(!qtAudioCheck, "Qt delivery validation requires Qt Multimedia");
#endif
		for (Window* window : windows) window->controller()->setPaused(false);
		if (players == 8) { QVERIFY(!app->newWindow()); QVERIFY(!app->multiplayer()->setRingTopology(false)); }
		bool entered = false;
		QElapsedTimer timer;
		timer.start();
		qint64 nextProgress = 30000;
		while (!entered && timer.elapsed() < (qEnvironmentVariableIntValue("MGBA_RING_GUI_VISUAL") ? 1800000 : 600000)) {
            if (audioCheck) {
                // qWait polls GUI events between sleeps. With video sync the
                // reference producer waits for those polls, manufacturing
                // stalls that do not occur in the application's event loop.
                QEventLoop desktop;
                QTimer::singleShot(250, &desktop, &QEventLoop::quit);
                desktop.exec();
            } else QTest::qWait(20);
            if (audioCheck) {
                bool allRacing = true, allComplete = true;
                unsigned required = 2 | (finishScene ? 4 : 0);
                for (int i = 0; i < players; ++i) {
                    unsigned progress = inputs[i].audioProgress->load(std::memory_order_acquire);
                    allRacing &= (progress & 1) != 0;
                    allComplete &= (progress & required) == required;
                }
                if (allRacing && !audioMeasured) {
                    for (int i = 0; i < players; ++i) {
                        mCoreThread* thread = windows[i]->controller()->thread();
                        mCoreSyncLockAudio(&thread->impl->sync);
                        initialDroppedSamples[i] = static_cast<GBA*>(thread->core->board)->audio.droppedSamples;
                        mCoreSyncUnlockAudio(&thread->impl->sync);
#ifdef BUILD_SDL
                        if (sdlAudioCheck) {
                            // Observe counters under the callback lock. Cycling
                            // eight device clocks here creates its own stall.
                            mSDLGetAudioStats(&audio[i].device, &audio[i].initialOutputSamples,
                                             &audio[i].initialUnderrunSamples);
                        }
#endif
#ifdef BUILD_QT_MULTIMEDIA
                        if (qtAudioCheck) {
                            QVERIFY(qtAudio[i]);
                            QCOMPARE(windows[i]->audioProcessor(), qtAudio[i].data());
                            qtAudio[i]->resetDeliveryStats();
                        }
#endif
                    }
                    audioMeasured = true;
                }
                if (!allComplete) {
                    if (timer.elapsed() >= nextProgress) {
                        qInfo("GUI audio: collecting uninterrupted %u-frame moving window", inputs[0].speedWindow);
                        nextProgress += 30000;
                    }
                    continue;
                }
                // End delivery measurement before interrupting any producer
                // for the final state checks. Interrupting one ring CPU stops
                // its peers too; leaving devices running would count that
                // deliberate debugger-style stop as gameplay starvation.
                // Stop peers first so the reference output keeps pacing the
                // remaining devices until their measurements have ended.
                for (int i = players - 1; i >= 0; --i) {
#ifdef BUILD_SDL
                    if (sdlAudioCheck) mSDLPauseAudio(&audio[i].device);
#endif
#ifdef BUILD_QT_MULTIMEDIA
                    if (qtAudioCheck) {
                        QVERIFY(qtAudio[i]);
                        qtAudio[i]->pause();
                    }
#endif
                }
            }
            QList<CoreController::Interrupter> interrupters;
            for (Window* window : windows) interrupters.append(window->controller());
			entered = true;
			for (int i = 0; i < players; ++i) {
				mCore* core = windows[i]->controller()->thread()->core;
				if (timer.elapsed() >= nextProgress) qInfo("GUI player %d: frame %u, humans %u", i, core->frameCounter(core), core->rawRead8(core, 0x0300361B, -1));
				QVERIFY(static_cast<GBA*>(core->board)->biosVf);
				QVERIFY(!mCoreThreadHasExited(windows[i]->controller()->thread()));
                QVERIFY(!mCoreThreadHasCrashed(windows[i]->controller()->thread()));
                QCOMPARE(windows[i]->controller()->thread()->impl->sync.lockstep, ringTopology);
                QCOMPARE(windows[i]->controller()->thread()->impl->sync.externalPacing, ringTopology && i != 0);
                if (wireLoss) {
                    if (core->frameCounter(core) < 3600 || core->rawRead8(core, 0x03006BAD, -1)) {
                        entered = false;
                    } else {
                        QCOMPARE(core->rawRead8(core, 0x03006BAB, -1), 8U);
                        QVERIFY(core->rawRead32(core, 0x03006B98, -1) ||
                                core->rawRead32(core, 0x03006B9C, -1));
                    }
                    continue;
                }
                if (finishScene && core->rawRead32(core, 0x03002E20, -1) != finishScene) { entered = false; continue; }
                if (finishScene) {
                    QCOMPARE(core->rawRead8(core, 0x03003620, -1), 2U);
                    QCOMPARE(core->rawRead8(core, 0x03003621, -1), 32U);
                    uint32_t host = core->rawRead32(core, 0x03003638, -1);
                    QCOMPARE(core->rawRead8(core, host + 0x4C, -1) & 7, 6U);
                    QVERIFY(core->rawRead8(core, host + 0xC7, -1) >= 0x83);
                }
				if (core->frameCounter(core) < 4901 || (ringTopology && !wireLoss && !inputs[i].speedComplete) || core->rawRead8(core, 0x0300361B, -1) != unsigned(players)) { entered = false; continue; }
				QCOMPARE(core->rawRead8(core, 0x0300361C, -1), unsigned(i));
				QVERIFY(core->rawRead16(core, 0x0300361E, -1) & (mode == "vs" ? 0x400 : mode == "gp" ? 0x100 : 0x800));
				// The stock opponent-marker switch used to produce a null cell
				// for a fourth visible opponent and flood OAM with invalid shapes.
				GBA* gba = static_cast<GBA*>(core->board);
				for (unsigned object = 0; object < 128; ++object) {
					uint16_t attr0 = gba->video.oam.raw[object * 4];
					if ((attr0 & 0x300) == 0x200) continue;
					QVERIFY2((attr0 >> 14) != 3, "Linked race contains a corrupted sprite shape");
				}
				for (int racer = 0; racer < players; ++racer) {
					uint32_t driver = core->rawRead32(core, 0x03003638 + racer * 4, -1);
					QVERIFY(driver >= 0x02000000 && driver < 0x03008000);
					QCOMPARE(core->rawRead8(core, driver + 0xEC, -1), unsigned(racer));
				}
                if (ringTopology && !wireLoss) {
                    double rate = double(inputs[i].lastSimulation - inputs[i].firstSimulation) / inputs[i].speedWindow;
                    qInfo("GUI player %d: %.3f updates/video frame, %.2f video frames/wall second", i + 1, rate, inputs[i].wallFramesPerSecond);
                    if (audioCheck) qInfo("GUI player %d: longest host frame gap %.2f ms, %u gaps above 25 ms", i + 1, inputs[i].longestFrameGapMs, inputs[i].delayedHostFrames);
                    QVERIFY2(rate >= 0.95, "GUI gameplay runs below 95% speed");
                    if (audioCheck) {
                        uint64_t dropped = gba->audio.droppedSamples - initialDroppedSamples[i];
                        qInfo("GUI source P%d: %llu dropped PCM samples", i + 1, (unsigned long long)dropped);
                        QVERIFY2(!dropped, "The emulation FIFO discarded generated PCM");
                    }
                }
				if (ringTopology) {
					QCOMPARE(core->rawRead32(core, 0x03006B98, -1), 0U); // Protocol errors
					QCOMPARE(core->rawRead32(core, 0x03006B9C, -1), 0U); // Timeouts
					QVERIFY(core->rawRead32(core, 0x03006B90, -1) > 100); // Committed inputs
				}
			}
			if (timer.elapsed() >= nextProgress) nextProgress += 30000;
		}
		QVERIFY2(entered, "GUI windows did not enter a linked race through keypad input");
		qInfo("GUI %s/%d %s with real BIOS in %lld ms", qPrintable(mode), players,
              wireLoss ? "survived a start-time wire loss" : "entered", timer.elapsed());
#ifdef BUILD_SDL
        if (sdlAudioCheck) {
            QVERIFY(audioMeasured);
            for (int i = 0; i < players; ++i) {
                mSDLPauseAudio(&audio[i].device);
                qInfo("GUI SDL P%d: %llu output samples, %llu underrun samples", i + 1,
                      (unsigned long long) (audio[i].device.outputSamples - audio[i].initialOutputSamples),
                      (unsigned long long) (audio[i].device.underrunSamples - audio[i].initialUnderrunSamples));
            }
            for (int i = 0; i < players; ++i) {
                QVERIFY(audio[i].device.outputSamples - audio[i].initialOutputSamples > 48000 * 30);
                QCOMPARE(audio[i].device.underrunSamples - audio[i].initialUnderrunSamples, uint64_t(0));
            }
        }
#endif
#ifdef BUILD_QT_MULTIMEDIA
        if (qtAudioCheck) {
            QVERIFY(audioMeasured);
            for (int i = 0; i < players; ++i) {
                qInfo("GUI Qt P%d: %llu delivered frames, %llu underrun events", i + 1,
                      (unsigned long long)qtAudio[i]->deliveredFrames(),
                      (unsigned long long)qtAudio[i]->underrunEvents());
            }
            for (int i = 0; i < players; ++i) {
                QVERIFY(qtAudio[i]->deliveredFrames() > qtAudio[i]->sampleRate() * 30);
                QCOMPARE(qtAudio[i]->underrunEvents(), uint64_t(0));
            }
        }
#endif
		if (ringTopology) {
			QAction* pause = nullptr;
			for (QAction* action : first->findChildren<QAction*>()) if (QString(action->text()).remove('&') == "Pause") pause = action;
			QVERIFY(pause);
			pause->trigger();
			for (Window* window : windows) QTRY_VERIFY_WITH_TIMEOUT(window->controller()->isPaused(), 10000);
			pause->trigger();
			for (Window* window : windows) QTRY_VERIFY_WITH_TIMEOUT(!window->controller()->isPaused(), 10000);
			// Reset while peers remain connected; reset must not strand the cable lock.
			first->controller()->reset();
			if (players == 8) {
				Window* middle = windows.takeAt(3);
				middle->close();
				QTRY_COMPARE_WITH_TIMEOUT(app->multiplayer()->attached(), 7, 10000);
			}
		}
		// Interrupt/close every participant, including an active middle cable node.
		// A suspended audio device must also close without trying to drain a
		// corked stream. Exercise that lifecycle with the real Qt backend.
#ifdef BUILD_QT_MULTIMEDIA
        if (qtAudioCheck) {
            for (Window* window : windows) window->audioProcessor()->pause();
        }
#endif
		for (Window* window : windows) window->close();
		QTRY_COMPARE_WITH_TIMEOUT(app->multiplayer()->attached(), 0, 10000);
		QTRY_COMPARE_WITH_TIMEOUT(app->windows().size(), 0, 10000);
	}
public:
	ConfigController* config = nullptr;
};

int main(int argc, char** argv) {
	// Eight native cores can exceed QtTest's five-minute default on shared
	// cloud CPUs; keep its watchdog beyond the explicit gameplay deadline.
	if (qEnvironmentVariableIsEmpty("QTEST_FUNCTION_TIMEOUT"))
        qputenv("QTEST_FUNCTION_TIMEOUT", qEnvironmentVariableIntValue("MGBA_RING_GUI_VISUAL") ? "1860000" : "660000");
	QTemporaryDir configDirectory;
	if (!configDirectory.isValid()) return 1;
	// Linux/macOS config discovery honors XDG_CONFIG_HOME. Keep concurrent
	// test processes and their saved topology/MRU/output paths independent.
	qputenv("XDG_CONFIG_HOME", configDirectory.path().toUtf8());
	ConfigController config;
	config.setQtOption("displayDriver", qEnvironmentVariableIsSet("MGBA_RING_GUI_DISPLAY") ? qEnvironmentVariableIntValue("MGBA_RING_GUI_DISPLAY") : 0);
    if (qEnvironmentVariableIsSet("MGBA_RING_GUI_THREADED")) config.setOption("threadedVideo", qEnvironmentVariableIntValue("MGBA_RING_GUI_THREADED"));
    if (qEnvironmentVariableIsSet("MGBA_RING_GUI_HARDWARE")) config.setOption("hwaccelVideo", qEnvironmentVariableIntValue("MGBA_RING_GUI_HARDWARE"));
    bool qtAudio = qEnvironmentVariable("MGBA_RING_GUI_AUDIO") == "qt";
    bool audioCheck = qtAudio || qEnvironmentVariableIntValue("MGBA_RING_GUI_AUDIO");
    if (qtAudio) config.setQtOption("audioDriver", 0);
    else if (audioCheck || qEnvironmentVariableIntValue("MGBA_RING_GUI_MOTION")) config.setQtOption("audioDriver", -1);
    if (qEnvironmentVariableIsSet("MGBA_RING_GUI_SAMPLE_RATE")) {
        int rate = qEnvironmentVariableIntValue("MGBA_RING_GUI_SAMPLE_RATE");
        if (rate < 8000 || rate > 192000) return 1;
        config.setOption("sampleRate", rate);
    }
	bool sync = qEnvironmentVariableIntValue("MGBA_RING_GUI_SYNC") != 0;
	config.setOption("audioSync", sync);
	config.setOption("videoSync", sync);
	config.setOption("pauseOnFocusLost", false);
	config.setOption("pauseOnMinimize", false);
	config.setOption("mute", !audioCheck);
	config.setOption("useBios", true);
	config.setOption("skipBios", false);
	config.setOption("gba.bios", qEnvironmentVariable("MGBA_RING_GUI_BIOS").toUtf8().constData());
	GBAApp app(argc, argv, &config);
	RingGuiTest test;
	test.config = &config;
	return QTest::qExec(&test, argc, argv);
}
#include "ring-gui.moc"
