/* SPDX-License-Identifier: MPL-2.0 */
#ifndef DISABLE_THREADING
#include <mgba/internal/gba/sio/ring-thread.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/core/core.h>
#include <mgba-util/audio-buffer.h>
#ifdef ENABLE_DEBUGGERS
#include <mgba/debugger/debugger.h>
#endif

#define RING_IDLE_QUANTUM 16384

static int64_t _resetEpoch(struct GBASIORingThreadCoordinator* coordinator) {
	/* A new console may open while other windows are already executing.
	 * Choose a future frame boundary for its reset timestamp, keeping LCD
	 * phases aligned without changing the physical serial clock or CPU rate. */
	return ((coordinator->clock + VIDEO_TOTAL_LENGTH - 1) / VIDEO_TOTAL_LENGTH) * VIDEO_TOTAL_LENGTH;
}

static unsigned _interval(struct GBASIORingThreadCoordinator* coordinator) {
	unsigned interval = RING_IDLE_QUANTUM;
	for (unsigned i = 0; i < coordinator->ring.count; ++i) {
		struct GBASIO* sio = coordinator->nodes[i]->driver.d.p;
		if ((sio->mode == GBA_SIO_NORMAL_8 || sio->mode == GBA_SIO_NORMAL_32) && GBASIONormalIsStart(sio->siocnt)) {
			unsigned cycles = GBASIOTransferCycles(sio->mode, sio->siocnt, 1) / 4;
			if (cycles > 512) cycles = 512;
			if (cycles < interval) interval = cycles;
		}
	}
	return interval;
}

static struct GBASIORingThreadNode* _node(struct mCoreThread* thread) {
	struct GBA* gba = thread->core->board;
	return (struct GBASIORingThreadNode*) gba->sio.driver;
}

static int64_t _time(struct GBASIORingThreadNode* node) {
	return mTimingGlobalTime(node->thread->core->timing) + node->driver.timeOffset;
}

static struct GBASIORingThreadNode* _next(struct GBASIORingThreadCoordinator* coordinator) {
	struct GBASIORingThreadNode* next = NULL;
	for (unsigned i = 0; i < coordinator->ring.count; ++i) {
		struct GBASIORingThreadNode* node = coordinator->nodes[i];
		if (!next || _time(node) < _time(next)) next = node;
	}
	return next;
}

static void _yield(struct mTiming* timing, void* context, uint32_t late) {
	UNUSED(late);
	struct GBASIORingThreadNode* node = context;
	((struct GBA*) node->thread->core->board)->earlyExit = true;
	mTimingSchedule(timing, &node->quantum, node->coordinator->interval);
}

static void _advance(struct mCoreThread* thread) {
	struct GBASIORingThreadNode* node = _node(thread);
	struct GBASIORingThreadCoordinator* coordinator = node->coordinator;
	thread->impl->sync.deferWait = true;
#ifdef ENABLE_DEBUGGERS
	if (thread->core->debugger) mDebuggerRun(thread->core->debugger);
	else
#endif
	thread->core->runLoop(thread->core);
	thread->impl->sync.deferWait = false;
	if (_time(node) > coordinator->clock) coordinator->clock = _time(node);
	unsigned interval = _interval(coordinator);
	if (interval != coordinator->interval) {
		coordinator->interval = interval;
		for (unsigned i = 0; i < coordinator->ring.count; ++i) {
			struct GBASIORingThreadNode* peer = coordinator->nodes[i];
			mTimingDeschedule(peer->thread->core->timing, &peer->quantum);
			mTimingSchedule(peer->thread->core->timing, &peer->quantum, interval);
		}
	}
}

static bool _outputReady(struct mCoreThread* thread) {
	struct mCoreSync* sync = &thread->impl->sync;
	MutexLock(&sync->videoFrameMutex);
	bool ready = !sync->videoFrameWait || !sync->videoFramePending;
	MutexUnlock(&sync->videoFrameMutex);
	if (!ready) return false;
	MutexLock(&sync->audioBufferMutex);
	ready = !sync->audioWait || !sync->audioHighWater ||
	        mAudioBufferAvailable(thread->core->getAudioBuffer(thread->core)) < sync->audioHighWater;
	MutexUnlock(&sync->audioBufferMutex);
	return ready;
}

static void _run(struct mCoreThread* thread) {
	struct GBASIORingThreadNode* node = _node(thread);
	struct GBASIORingThreadCoordinator* coordinator = node->coordinator;
	mCoreSyncWaitDeferred(&thread->impl->sync, thread->core->getAudioBuffer(thread->core));
	MutexLock(&coordinator->mutex);
	struct GBASIORingThreadNode* next = _next(coordinator);
	/* Execute short serial slices cooperatively while the other owning
	 * threads sleep on WAIT. Preserve the same timestamp ordering without
	 * paying for a kernel thread handoff for each 32-bit cable word. */
	for (unsigned slice = 0; next && slice < 64; ++slice) {
		if (!_outputReady(next->thread)) break;
		if (next == node) _advance(thread);
		else if (!mCoreThreadRunWaiting(next->thread, _advance)) break;
		next = _next(coordinator);
	}
	if (next != node) mCoreThreadWaitFromThread(thread);
	if (next) mCoreThreadStopWaiting(next->thread);
	MutexUnlock(&coordinator->mutex);
	/* Consumer waits must not hold the cable lock: the GUI may interrupt a
	 * different participant while processing a reset, pause, or close. */
	/* Only the output owner's thread may block on its consumers. GUI
	 * interruption wakes that thread's sync conditions, never another core's. */
	mCoreSyncWaitDeferred(&thread->impl->sync, thread->core->getAudioBuffer(thread->core));
}

static void _reset(struct mCoreThread* thread) {
	struct GBASIORingThreadNode* node = _node(thread);
	struct GBASIORingThreadCoordinator* coordinator = node->coordinator;
	MutexLock(&coordinator->mutex);
	thread->core->reset(thread->core);
	node->driver.timeOffset = _resetEpoch(coordinator) - mTimingGlobalTime(thread->core->timing);
	mTimingSchedule(thread->core->timing, &node->quantum, coordinator->interval);
	MutexUnlock(&coordinator->mutex);
}

void GBASIORingThreadInit(struct GBASIORingThreadCoordinator* coordinator) {
	memset(coordinator, 0, sizeof(*coordinator));
	GBASIORingInit(&coordinator->ring);
	coordinator->interval = RING_IDLE_QUANTUM;
	MutexInit(&coordinator->mutex);
}

void GBASIORingThreadDeinit(struct GBASIORingThreadCoordinator* coordinator) {
	MutexDeinit(&coordinator->mutex);
}

static void _cancel(struct GBASIORingThreadCoordinator* coordinator) {
	for (unsigned i = 0; i < coordinator->ring.count; ++i) {
		struct GBASIORingDriver* driver = &coordinator->nodes[i]->driver;
		driver->d.reset(&driver->d);
	}
}

bool GBASIORingThreadAttach(struct GBASIORingThreadCoordinator* coordinator, struct GBASIORingThreadNode* node, struct mCoreThread* thread) {
	if (thread->runLoopOverride || thread->resetOverride || coordinator->ring.count == GBA_RING_MAX_PLAYERS) return false;
	MutexLock(&coordinator->mutex);
	_cancel(coordinator);
	memset(node, 0, sizeof(*node));
	bool ok = GBASIORingAttach(&coordinator->ring, &node->driver);
	if (ok) {
		node->thread = thread;
		node->coordinator = coordinator;
		coordinator->nodes[node->driver.slot] = node;
		node->driver.timeOffset = _resetEpoch(coordinator) - mTimingGlobalTime(thread->core->timing);
		node->quantum.context = node;
		node->quantum.callback = _yield;
		node->quantum.name = "GUI ring scheduling quantum";
		node->quantum.priority = 0xFF;
		thread->core->setPeripheral(thread->core, mPERIPH_GBA_LINK_PORT, &node->driver.d);
		thread->runLoopOverride = _run;
		thread->resetOverride = _reset;
		mTimingSchedule(thread->core->timing, &node->quantum, coordinator->interval);
		/* Wake the first core after a paused/removed participant held the ring. */
		mCoreThreadStopWaiting(_next(coordinator)->thread);
	}
	MutexUnlock(&coordinator->mutex);
	return ok;
}

void GBASIORingThreadDetach(struct GBASIORingThreadNode* node) {
	struct GBASIORingThreadCoordinator* coordinator = node->coordinator;
	MutexLock(&coordinator->mutex);
	_cancel(coordinator);
	unsigned slot = node->driver.slot;
	mTimingDeschedule(node->thread->core->timing, &node->quantum);
	node->thread->runLoopOverride = NULL;
	node->thread->resetOverride = NULL;
	GBASIORingDetach(&node->driver);
	for (unsigned i = slot; i < coordinator->ring.count; ++i) coordinator->nodes[i] = coordinator->nodes[i + 1];
	coordinator->nodes[coordinator->ring.count] = NULL;
	mCoreThreadStopWaiting(node->thread);
	struct GBASIORingThreadNode* next = _next(coordinator);
	if (next) mCoreThreadStopWaiting(next->thread);
	MutexUnlock(&coordinator->mutex);
}

#endif
