/* SPDX-License-Identifier: MPL-2.0 */
#include "platform/qt/ConfigController.h"
#include "platform/qt/CoreController.h"
#include "platform/qt/CoreManager.h"
#include "platform/qt/GBAApp.h"
#include "platform/qt/Window.h"
#include <mgba/internal/gba/gba.h>
#include <QAction>
#include <QComboBox>
#include <QFile>
#include <QInputDialog>
#include <QTimer>
#include <QTemporaryDir>
#include <QTest>

using namespace QGBA;

struct Inputs {
	CoreController* controller;
	QList<QPair<unsigned, unsigned>> events;
	int next = 0;
	unsigned held = 0;
	static void frame(void* context) {
		Inputs* inputs = static_cast<Inputs*>(context);
		mCore* core = inputs->controller->thread()->core;
		unsigned frame = core->frameCounter(core);
		while (inputs->next < inputs->events.size() && inputs->events[inputs->next].first <= frame) {
			unsigned keys = inputs->events[inputs->next++].second;
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
		for (int players : {2, 8}) for (const QString& mode : {QString("vs"), QString("battle")})
			QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(players))) << players << mode << true << false;
		QTest::newRow("vs-2-late") << 2 << QString("vs") << true << true;
		for (const QString& mode : {QString("vs"), QString("battle")})
			QTest::newRow(qPrintable(QString("stock-%1-4").arg(mode))) << 4 << mode << false << false;
	}

	void ring() {
		if (qEnvironmentVariableIsEmpty("MGBA_RING_GUI_ROM") || qEnvironmentVariableIsEmpty("MGBA_RING_GUI_INPUTS"))
			QSKIP("Set MGBA_RING_GUI_ROM, MGBA_RING_GUI_BIOS, and MGBA_RING_GUI_INPUTS to private validation fixtures");
		QFETCH(int, players);
		QFETCH(QString, mode);
		QFETCH(bool, ringTopology);
		QFETCH(bool, lateBoot);
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
				if (core->frameCounter(core) < 4300 || core->rawRead8(core, 0x0300361B, -1) != unsigned(players)) { entered = false; continue; }
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
				if (ringTopology) {
					QCOMPARE(core->rawRead32(core, 0x03006B98, -1), 0U); // Protocol errors
					QCOMPARE(core->rawRead32(core, 0x03006B9C, -1), 0U); // Timeouts
					QVERIFY(core->rawRead32(core, 0x03006B90, -1) > 100); // Committed inputs
				}
			}
			if (timer.elapsed() >= nextProgress) nextProgress += 30000;
		}
		QVERIFY2(entered, "GUI windows did not enter a linked race through keypad input");
		qInfo("GUI %s/%d entered with real BIOS in %lld ms", qPrintable(mode), players, timer.elapsed());
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
