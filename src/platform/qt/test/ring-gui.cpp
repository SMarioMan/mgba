/* SPDX-License-Identifier: MPL-2.0 */
#include "platform/qt/ConfigController.h"
#include "platform/qt/CoreController.h"
#include "platform/qt/CoreManager.h"
#include "platform/qt/GBAApp.h"
#include "platform/qt/Window.h"
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio/ring.h>
#include <QAction>
#include <QComboBox>
#include <QFile>
#include <QMenuBar>
#include <QDir>
#include <mgba/core/input.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/input.h>
#include <QInputDialog>
#include <QTimer>
#include <QTemporaryDir>
#include <QTest>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

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
	static void frame(void* context) {
		Inputs* inputs = static_cast<Inputs*>(context);
		mCore* core = inputs->controller->thread()->core;
		unsigned frame = core->frameCounter(core);
        unsigned simulation = core->rawRead32(core, 0x03002E30, -1);
        bool racing = core->rawRead8(core, 0x03006BB5, -1) && simulation >= 128;
        if (inputs->measureSpeed && !inputs->speedComplete && racing && frame >= inputs->minSpeedFrame) {
            if (!inputs->speedStartFrame) {
                inputs->speedStartFrame = frame;
                inputs->firstSimulation = simulation;
            } else if (frame >= inputs->speedStartFrame + 600) {
                inputs->lastSimulation = simulation;
                inputs->speedComplete = true;
            }
        }
        if (inputs->breakWire && frame >= 2600) {
            auto driver = reinterpret_cast<GBASIORingDriver*>(static_cast<GBA*>(core->board)->sio.driver);
            driver->ring->openInputs = frame < 2900 ? 1U << 4 : 0;
        }
		while (inputs->next < inputs->events.size() && inputs->events[inputs->next].first <= frame) {
            // Keep Battle stationary through the speed window; an eliminated
            // round's frozen counter must never be mistaken for half-speed.
            if (inputs->deferMotion && !inputs->speedComplete && inputs->events[inputs->next].first >= 3500) break;
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
    void borrowedFailureLifetime() {
        // A crashed or exiting borrowed core must remain exclusively owned
        // until its callback returns, before the GUI can detach its cable.
        for (bool shutdown : {false, true}) {
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
            bool borrowed = false;
            std::thread worker([&] {
                borrowed = mCoreThreadRunWaiting(&thread, [](mCoreThread* t) {
                    Gate* g = static_cast<Gate*>(t->userData);
                    if (g->shutdown) mCoreThreadEnd(t);
                    else mCoreThreadMarkCrashed(t);
                    std::unique_lock<std::mutex> lock(g->mutex);
                    g->entered = true;
                    g->condition.notify_all();
                    g->condition.wait(lock, [&] { return g->release; });
                });
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
        char profile[] = "--instance-settings", hidden[] = "--hide-menu", biosFlag[] = "-b";
        QByteArray bios = qEnvironmentVariable("MGBA_RING_GUI_BIOS").toUtf8();
        config->setOption("gba.bios", "");
        char* argv[] = {arg0, flag, count, profile, directory.data(), hidden, biosFlag, bios.data(), romArg.data()};
        QVERIFY(config->parseArguments(9, argv));
        QVERIFY(app->multiplayer()->setRingTopology(true));
        QVERIFY(app->newWindow());
        app->initMultiplayer();
        QCOMPARE(app->windows().size(), 8);
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
        // Cycle Mario through DK -> Wario -> Bowser and around twice via real
        // Qt key events, then return to Mario before the normal ready press.
        for (unsigned j = 0; j < 16; ++j) {
            inputs[0].events.append(qMakePair(1850U + 30*j, 16U));
            inputs[0].events.append(qMakePair(1865U + 30*j, 0U));
        }
        std::sort(inputs[0].events.begin(), inputs[0].events.end());
        auto windows = app->windows();
        for (int i = 0; i < 8; ++i) {
            Window* window = windows[i];
            auto controller = window->controller();
            QVERIFY(controller);
            QCOMPARE(window->config()->getOption("bios"), QString::fromUtf8(bios));
            QTRY_VERIFY_WITH_TIMEOUT(controller->hasStarted(), 10000);
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
            double rate = double(inputs[i].lastSimulation - inputs[i].firstSimulation) / 600;
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
		QFile fixture(ringTopology ? qEnvironmentVariable("MGBA_RING_GUI_INPUTS") + QString("/%1-%2.csv").arg(mode).arg(players)
		                          : qEnvironmentVariable("MGBA_RING_GUI_STOCK_INPUTS") + QString("/stock-%1.csv").arg(mode));
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
			inputs[slot].events.append(qMakePair(frame + (lateBoot && !slot && frame > 715 ? 760 : 0), columns[2].toUInt(nullptr, 16)));
		}
		QList<Window*> windows;
		for (int i = 0; i < players; ++i) {
			Window* window = i ? app->newWindow() : first;
			QVERIFY(window);
			windows.append(window);
			CoreController* controller = manager.loadGame(rom);
			QVERIFY(controller);
			inputs[i].controller = controller;
            inputs[i].breakWire = wireLoss && !i;
            inputs[i].measureSpeed = ringTopology && !wireLoss;
            inputs[i].deferMotion = ringTopology && !wireLoss;
			controller->setPaused(true);
			mCoreCallbacks callbacks{};
			callbacks.context = &inputs[i];
			callbacks.videoFrameStarted = Inputs::frame;
			controller->thread()->core->addCoreCallbacks(controller->thread()->core, &callbacks);
			window->setController(controller);
			QTRY_VERIFY_WITH_TIMEOUT(controller->hasStarted(), 10000);
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
			}
		}
		QCOMPARE(app->multiplayer()->attached(), players);
		if (ringTopology && players == 2) {
			QVERIFY(app->multiplayer()->setRingTopology(false));
			QVERIFY(app->multiplayer()->setRingTopology(true));
		}
		for (Window* window : windows) window->controller()->setPaused(false);
		if (players == 8) { QVERIFY(!app->newWindow()); QVERIFY(!app->multiplayer()->setRingTopology(false)); }
		bool entered = false;
		QElapsedTimer timer;
		timer.start();
		qint64 nextProgress = 30000;
		while (!entered && timer.elapsed() < 600000) {
			QTest::qWait(20);
			QList<CoreController::Interrupter> interrupters;
			for (Window* window : windows) interrupters.append(window->controller());
			entered = true;
			for (int i = 0; i < players; ++i) {
				mCore* core = windows[i]->controller()->thread()->core;
				if (timer.elapsed() >= nextProgress) qInfo("GUI player %d: frame %u, humans %u", i, core->frameCounter(core), core->rawRead8(core, 0x0300361B, -1));
				QVERIFY(static_cast<GBA*>(core->board)->biosVf);
				QVERIFY(!mCoreThreadHasExited(windows[i]->controller()->thread()));
                QVERIFY(!mCoreThreadHasCrashed(windows[i]->controller()->thread()));
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
				if (core->frameCounter(core) < 4901 || (ringTopology && !wireLoss && !inputs[i].speedComplete) || core->rawRead8(core, 0x0300361B, -1) != unsigned(players)) { entered = false; continue; }
				QCOMPARE(core->rawRead8(core, 0x0300361C, -1), unsigned(i));
				QVERIFY(core->rawRead16(core, 0x0300361E, -1) & (mode == "vs" ? 0x400 : 0x800));
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
                    double rate = double(inputs[i].lastSimulation - inputs[i].firstSimulation) / 600;
                    qInfo("GUI player %d native game speed %.3f updates/video frame", i + 1, rate);
                    QVERIFY2(rate >= 0.95, "GUI gameplay runs below 95% speed");
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
	if (qEnvironmentVariableIsEmpty("QTEST_FUNCTION_TIMEOUT")) qputenv("QTEST_FUNCTION_TIMEOUT", "660000");
	QTemporaryDir configDirectory;
	if (!configDirectory.isValid()) return 1;
	// Linux/macOS config discovery honors XDG_CONFIG_HOME. Keep concurrent
	// test processes and their saved topology/MRU/output paths independent.
	qputenv("XDG_CONFIG_HOME", configDirectory.path().toUtf8());
	ConfigController config;
	config.setQtOption("displayDriver", 0);
	bool sync = qEnvironmentVariableIntValue("MGBA_RING_GUI_SYNC") != 0;
	config.setOption("audioSync", sync);
	config.setOption("videoSync", sync);
	config.setOption("pauseOnFocusLost", false);
	config.setOption("pauseOnMinimize", false);
	config.setOption("mute", true);
	config.setOption("useBios", true);
	config.setOption("skipBios", false);
	config.setOption("gba.bios", qEnvironmentVariable("MGBA_RING_GUI_BIOS").toUtf8().constData());
	GBAApp app(argc, argv, &config);
	RingGuiTest test;
	test.config = &config;
	return QTest::qExec(&test, argc, argv);
}
#include "ring-gui.moc"
