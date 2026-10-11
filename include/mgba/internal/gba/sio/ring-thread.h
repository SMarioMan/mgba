/* SPDX-License-Identifier: MPL-2.0 */
#ifndef GBA_SIO_RING_THREAD_H
#define GBA_SIO_RING_THREAD_H

#include <mgba/internal/gba/sio/ring.h>
#include <mgba/core/thread.h>
#include <mgba-util/threading.h>

CXX_GUARD_START

struct GBASIORingThreadCoordinator;
struct GBASIORingThreadNode {
	struct GBASIORingDriver driver;
	struct GBASIORingThreadCoordinator* coordinator;
	struct mCoreThread* thread;
	struct mTimingEvent quantum;
};

/* Execute bounded core iterations exclusively, in shared timestamp order.
 * WAIT requests yield without blocking a GUI interrupt, reset, or shutdown.
 * Attach/detach require all existing cores to be interrupted by the caller. */
struct GBASIORingThreadCoordinator {
	struct GBASIORing ring;
	struct GBASIORingThreadNode* nodes[GBA_RING_MAX_PLAYERS];
	Mutex mutex;
	int64_t clock;
	unsigned interval;
};

void GBASIORingThreadInit(struct GBASIORingThreadCoordinator*);
void GBASIORingThreadDeinit(struct GBASIORingThreadCoordinator*);
bool GBASIORingThreadAttach(struct GBASIORingThreadCoordinator*, struct GBASIORingThreadNode*, struct mCoreThread*);
void GBASIORingThreadDetach(struct GBASIORingThreadNode*);

CXX_GUARD_END
#endif
