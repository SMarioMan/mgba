/* SPDX-License-Identifier: MPL-2.0 */
#ifndef GBA_SIO_RING_H
#define GBA_SIO_RING_H

#include <mgba/internal/gba/sio.h>
#include <mgba/core/timing.h>

CXX_GUARD_START

#define GBA_RING_MAX_PLAYERS 8
/* Eight racers and one optional forwarding map display. */
#define GBA_RING_MAX_NODES (GBA_RING_MAX_PLAYERS + 1)

/* Single-threaded, shared-clock normal-mode cable: SO[i] -> SI[i+1].
 * Run attached cores in timestamp order. This is independent of the four
 * hardware multiplayer slots and does not change MAX_GBAS or savestates. */
struct GBASIORingDriver;
struct GBASIORing {
	struct GBASIORingDriver* players[GBA_RING_MAX_NODES];
	unsigned count;
	uint64_t transfers;
	uint64_t missedReceivers;
	uint64_t collisions;
	unsigned pending;
	struct GBASIORingDriver* clockOwner;
	bool ownerFinished;
	/* Test wiring: bit i opens the SO[i-1] -> SI[i] connection. */
	uint16_t openInputs;
};

struct GBASIORingDriver {
	struct GBASIODriver d;
	struct GBASIORing* ring;
	struct mTimingEvent event;
	unsigned slot;
	/* Align independently booted GUI cores on the shared cable clock. */
	int64_t timeOffset;
	uint32_t incoming;
	enum GBASIOMode transferMode;
	bool idleSo;
	bool active;
};

void GBASIORingInit(struct GBASIORing*);
bool GBASIORingAttach(struct GBASIORing*, struct GBASIORingDriver*);
void GBASIORingDetach(struct GBASIORingDriver*);

CXX_GUARD_END
#endif
