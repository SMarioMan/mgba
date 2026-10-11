/* SPDX-License-Identifier: MPL-2.0 */
#include <mgba/internal/gba/sio/ring.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>

static bool _handles(struct GBASIODriver* d, enum GBASIOMode mode) {
	UNUSED(d);
	return mode == GBA_SIO_NORMAL_8 || mode == GBA_SIO_NORMAL_32;
}

static void _updateInputs(struct GBASIORing* ring) {
	for (unsigned i = 0; i < ring->count; ++i) {
		struct GBASIORingDriver* p = ring->players[i];
		struct GBASIORingDriver* prev = ring->players[(i + ring->count - 1) % ring->count];
		if (p->d.p && _handles(&p->d, p->d.p->mode)) {
			p->d.p->siocnt = GBASIONormalSetSi(p->d.p->siocnt, prev->idleSo || (ring->openInputs & (1U << i)));
		}
	}
}

static void _finish(struct mTiming* timing, void* context, uint32_t late) {
	UNUSED(timing);
	struct GBASIORingDriver* p = context;
	p->active = false;
	--p->ring->pending;
	/* A cancelled transfer or mode switch must not clobber unrelated IO. */
	if (p->d.p->mode == p->transferMode && GBASIONormalIsStart(p->d.p->siocnt)) {
		if (p->transferMode == GBA_SIO_NORMAL_32) {
			GBASIONormal32FinishTransfer(p->d.p, p->incoming, late);
		} else {
			GBASIONormal8FinishTransfer(p->d.p, p->incoming, late);
		}
	}
	p->idleSo = GBASIONormalIsIdleSo(p->d.p->siocnt);
	_updateInputs(p->ring);
}

static bool _start(struct GBASIODriver* d) {
	/* writeSIOCNT receives the new clock and start bits; sio.c calls start
	 * before committing those bits, so launch from that callback instead. */
	return !_handles(d, d->p->mode);
}

static uint16_t _write(struct GBASIODriver* d, uint16_t value) {
	struct GBASIORingDriver* p = (struct GBASIORingDriver*) d;
	struct GBASIORing* ring = p->ring;
	bool wasArmed = GBASIONormalIsStart(d->p->siocnt);
	bool armed = GBASIONormalIsStart(value);
	if (!wasArmed) {
		/* SO's inactive level is held until the first clock edge, even when
		 * arming also changes the level to use after completion. */
		if (!armed) {
			p->idleSo = GBASIONormalIsIdleSo(value);
		}
	}
	_updateInputs(ring);
	struct GBASIORingDriver* prev = ring->players[(p->slot + ring->count - 1) % ring->count];
	value = GBASIONormalSetSi(value & 0x7F8F, prev->idleSo || (ring->openInputs & (1U << p->slot)));
	if (!armed || wasArmed || !GBASIONormalIsSc(value)) {
		return value;
	}
	if (ring->pending) {
		++ring->collisions;
		return GBASIONormalClearStart(value);
	}
	uint32_t outgoing[GBA_RING_MAX_PLAYERS];
	bool receiving[GBA_RING_MAX_PLAYERS];
	for (unsigned i = 0; i < ring->count; ++i) {
		struct GBASIO* sio = ring->players[i]->d.p;
		receiving[i] = sio->mode == d->p->mode && (i == p->slot || GBASIONormalIsStart(sio->siocnt));
		if (!receiving[i]) {
			outgoing[i] = ring->players[i]->idleSo ? UINT32_MAX : 0;
			++ring->missedReceivers;
		} else if (d->p->mode == GBA_SIO_NORMAL_32) {
			outgoing[i] = sio->p->memory.io[GBA_REG(SIODATA32_LO)] | ((uint32_t) sio->p->memory.io[GBA_REG(SIODATA32_HI)] << 16);
		} else {
			outgoing[i] = sio->p->memory.io[GBA_REG(SIODATA8)] & 0xFF;
		}
	}
	int64_t finish = mTimingGlobalTime(&d->p->p->timing) + p->timeOffset + GBASIOTransferCycles(d->p->mode, value, 1);
	++ring->transfers;
	for (unsigned i = 0; i < ring->count; ++i) {
		if (!receiving[i]) {
			continue;
		}
		struct GBASIORingDriver* receiver = ring->players[i];
		receiver->incoming = (ring->openInputs & (1U << i)) ? UINT32_MAX : outgoing[(i + ring->count - 1) % ring->count];
		receiver->transferMode = d->p->mode;
		receiver->active = true;
		++ring->pending;
		struct mTiming* timing = &receiver->d.p->p->timing;
		int64_t delay = finish - ((int64_t) mTimingGlobalTime(timing) + receiver->timeOffset);
		mTimingSchedule(timing, &receiver->event, delay > 0 ? (int32_t) delay : 0);
	}
	return value;
}

static int _connected(struct GBASIODriver* d) {
	struct GBASIORingDriver* p = (struct GBASIORingDriver*) d;
	/* Normal mode sees a serial neighbour, not hardware multiplayer IDs. */
	return p->ring->count > 1 ? 1 : 0;
}

static void _reset(struct GBASIODriver* d) {
	struct GBASIORingDriver* p = (struct GBASIORingDriver*) d;
	mTimingDeschedule(&d->p->p->timing, &p->event);
	if (p->active) {
		--p->ring->pending;
	}
	p->active = false;
	p->idleSo = true;
	_updateInputs(p->ring);
}

static void _mode(struct GBASIODriver* d, enum GBASIOMode mode) {
	UNUSED(mode);
	_reset(d);
}

void GBASIORingInit(struct GBASIORing* ring) {
	memset(ring, 0, sizeof(*ring));
}

bool GBASIORingAttach(struct GBASIORing* ring, struct GBASIORingDriver* p) {
	if (ring->count == GBA_RING_MAX_PLAYERS || ring->pending) {
		return false;
	}
	for (unsigned i = 0; i < ring->count; ++i) {
		if (ring->players[i] == p) {
			return false;
		}
	}
	memset(p, 0, sizeof(*p));
	p->ring = ring;
	p->slot = ring->count;
	p->idleSo = true;
	p->d.reset = _reset;
	p->d.deinit = _reset;
	p->d.setMode = _mode;
	p->d.handlesMode = _handles;
	p->d.connectedDevices = _connected;
	p->d.writeSIOCNT = _write;
	p->d.start = _start;
	p->event.context = p;
	p->event.callback = _finish;
	p->event.name = "GBA ring serial completion";
	p->event.priority = 0x80;
	ring->players[ring->count++] = p;
	return true;
}

void GBASIORingDetach(struct GBASIORingDriver* p) {
	if (!p->ring) {
		return;
	}
	struct GBASIORing* ring = p->ring;
	if (p->d.p && p->d.p->driver == &p->d) {
		GBASIOSetDriver(p->d.p, NULL);
	}
	for (unsigned i = p->slot; i + 1 < ring->count; ++i) {
		ring->players[i] = ring->players[i + 1];
		ring->players[i]->slot = i;
	}
	ring->players[--ring->count] = NULL;
	p->ring = NULL;
	_updateInputs(ring);
}
