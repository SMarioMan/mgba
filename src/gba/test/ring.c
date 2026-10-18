/* SPDX-License-Identifier: MPL-2.0 */
#include "util/test/suite.h"
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/sio/ring.h>

static void finish(struct GBASIORingDriver* driver) {
	struct mTiming* timing = &driver->d.p->p->timing;
	mTimingDeschedule(timing, &driver->event);
	driver->event.callback(timing, driver, 0);
}

M_TEST_DEFINE(ownerCompletion) {
	for (bool cancelPeer = false; ; cancelPeer = true) {
		struct mCore* cores[2];
		struct GBASIORing ring;
		struct GBASIORingDriver drivers[2];
		GBASIORingInit(&ring);
		for (unsigned i = 0; i < 2; ++i) {
			cores[i] = GBACoreCreate();
			assert_true(cores[i]->init(cores[i]));
			mCoreInitConfig(cores[i], NULL);
			cores[i]->reset(cores[i]);
			assert_true(GBASIORingAttach(&ring, &drivers[i]));
			struct GBA* gba = cores[i]->board;
			GBASIOSetDriver(&gba->sio, &drivers[i].d);
			cores[i]->busWrite16(cores[i], 0x04000134, 0);
			cores[i]->busWrite16(cores[i], 0x04000128, 0x5002);
			cores[i]->busWrite32(cores[i], 0x04000120, 0x12345678 + i);
		}
		cores[1]->busWrite16(cores[1], 0x04000128, 0x5082);
		cores[0]->busWrite16(cores[0], 0x04000128, 0x5083);
		assert_int_equal(ring.pending, 2);
		finish(&drivers[0]);
		assert_true(ring.ownerFinished);
		assert_true(GBASIONormalIsStart(drivers[0].d.p->siocnt));
		assert_false(cores[0]->busRead16(cores[0], 0x04000202) & (1U << 7));
		if (cancelPeer) GBASIORingDetach(&drivers[1]);
		else finish(&drivers[1]);
		assert_int_equal(ring.pending, 0);
		assert_false(ring.ownerFinished);
		assert_false(GBASIONormalIsStart(drivers[0].d.p->siocnt));
		assert_true(cores[0]->busRead16(cores[0], 0x04000202) & (1U << 7));
		assert_int_equal(cores[0]->busRead32(cores[0], 0x04000120), 0x12345679);
		if (!cancelPeer) {
			cores[1]->busWrite16(cores[1], 0x04000128, 0x5082);
			cores[0]->busWrite16(cores[0], 0x04000128, 0x5083);
			assert_int_equal(ring.collisions, 0);
			finish(&drivers[1]);
			finish(&drivers[0]);
		}
		for (unsigned i = 0; i < 2; ++i) {
			if (drivers[i].ring) GBASIORingDetach(&drivers[i]);
			GBASIOSetDriver(&((struct GBA*)cores[i]->board)->sio, NULL);
			mCoreConfigDeinit(&cores[i]->config);
			cores[i]->deinit(cores[i]);
		}
		if (cancelPeer) break;
	}
}

static void ringTransfer(unsigned count, bool wide) {
	unsigned owner = count / 2;
	uint32_t mask = wide ? UINT32_MAX : 255;
	uint32_t dataRegister = wide ? 0x04000120 : 0x0400012A;
	struct mCore* cores[GBA_RING_MAX_PLAYERS];
	struct GBASIORingDriver drivers[GBA_RING_MAX_PLAYERS], excess;
	struct GBASIORing ring;
	GBASIORingInit(&ring);
	for (unsigned i = 0; i < count; ++i) {
		cores[i] = GBACoreCreate();
		assert_true(cores[i]->init(cores[i]));
		mCoreInitConfig(cores[i], NULL);
		cores[i]->reset(cores[i]);
		assert_true(GBASIORingAttach(&ring, &drivers[i]));
		GBASIOSetDriver(&((struct GBA*) cores[i]->board)->sio, &drivers[i].d);
		cores[i]->busWrite16(cores[i], 0x04000134, 0);
		cores[i]->busWrite16(cores[i], 0x04000128, wide ? 0x5002 : 0x4002);
		if (wide) cores[i]->busWrite32(cores[i], dataRegister, 0x13572460 + i);
		else cores[i]->busWrite16(cores[i], dataRegister, 0x60 + i);
	}
	if (count == GBA_RING_MAX_PLAYERS) assert_false(GBASIORingAttach(&ring, &excess));
	for (unsigned i = 0; i < count; ++i)
		if (i != owner) cores[i]->busWrite16(cores[i], 0x04000128, wide ? 0x5082 : 0x4082);
	cores[owner]->busWrite16(cores[owner], 0x04000128, wide ? 0x5083 : 0x4083);
	assert_int_equal(ring.pending, count);
	finish(&drivers[owner]);
	assert_true(ring.ownerFinished);
	for (unsigned i = 0; i < count; ++i)
		if (i != owner) finish(&drivers[i]);
	assert_int_equal(ring.pending, 0);
	assert_int_equal(ring.collisions, 0);
	for (unsigned i = 0; i < count; ++i) {
		assert_int_equal((wide ? cores[i]->busRead32(cores[i], dataRegister) : cores[i]->busRead16(cores[i], dataRegister) & 255),
			(0x13572460 + (i + count - 1) % count) & mask);
		if (wide) cores[i]->busWrite32(cores[i], dataRegister, 0x24681350 + i);
		else cores[i]->busWrite16(cores[i], dataRegister, 0x50 + i);
	}
	/* Exercise the highest supported wire bit, not just the low eight. */
	ring.openInputs = 1U << (count - 1);
	for (unsigned i = 0; i < count; ++i)
		if (i != owner) cores[i]->busWrite16(cores[i], 0x04000128, wide ? 0x5082 : 0x4082);
	cores[owner]->busWrite16(cores[owner], 0x04000128, wide ? 0x5083 : 0x4083);
	for (unsigned i = 0; i < count; ++i) finish(&drivers[i]);
	assert_int_equal(ring.pending, 0);
	for (unsigned i = 0; i < count; ++i) {
		assert_int_equal((wide ? cores[i]->busRead32(cores[i], dataRegister) : cores[i]->busRead16(cores[i], dataRegister) & 255), i == count - 1
			? mask : (0x24681350 + (i + count - 1) % count) & mask);
		GBASIORingDetach(&drivers[i]);
		GBASIOSetDriver(&((struct GBA*) cores[i]->board)->sio, NULL);
		mCoreConfigDeinit(&cores[i]->config);
		cores[i]->deinit(cores[i]);
	}
}

M_TEST_DEFINE(sixteenPlayerRing) {
	for (unsigned count = 2; count <= GBA_RING_MAX_PLAYERS; ++count) {
		ringTransfer(count, false);
		ringTransfer(count, true);
	}
}

M_TEST_DEFINE(missedCapture) {
    struct mCore* cores[2];
    struct GBASIORingDriver drivers[2];
    struct GBASIORing ring;
    GBASIORingInit(&ring);
    for (unsigned i = 0; i < 2; ++i) {
        cores[i] = GBACoreCreate();
        assert_true(cores[i]->init(cores[i]));
        mCoreInitConfig(cores[i], NULL);
        cores[i]->reset(cores[i]);
        assert_true(GBASIORingAttach(&ring, &drivers[i]));
        GBASIOSetDriver(&((struct GBA*) cores[i]->board)->sio, &drivers[i].d);
        cores[i]->busWrite16(cores[i], 0x04000134, 0);
        cores[i]->busWrite16(cores[i], 0x04000128, 0x5002);
    }
    ring.captureMisses = true;
    for (unsigned i = 0; i < GBA_RING_MISS_SAMPLES + 3; ++i) {
        // The unarmed receiver contributes its idle line and no event.
        cores[0]->busWrite16(cores[0], 0x04000128, 0x5083);
        assert_int_equal(ring.pending, 1);
        finish(&drivers[0]);
    }
    assert_int_equal(ring.missedReceivers, GBA_RING_MISS_SAMPLES + 3);
    assert_int_equal(ring.missSampleCount, GBA_RING_MISS_SAMPLES);
    const struct GBA* peer = cores[1]->board;
    for (unsigned i = 0; i < ring.missSampleCount; ++i) {
        assert_int_equal(ring.missSamples[i].slot, 1);
        assert_int_equal(ring.missSamples[i].pc, peer->cpu->gprs[ARM_PC]);
        assert_int_equal(ring.missSamples[i].lr, peer->cpu->gprs[ARM_LR]);
        assert_int_equal(ring.missSamples[i].frame, peer->video.frameCounter);
    }
    ring.captureMisses = false;
    ring.missSampleCount = 0;
    cores[0]->busWrite16(cores[0], 0x04000128, 0x5083);
    finish(&drivers[0]);
    assert_int_equal(ring.missSampleCount, 0);
    for (unsigned i = 0; i < 2; ++i) {
        GBASIORingDetach(&drivers[i]);
        GBASIOSetDriver(&((struct GBA*) cores[i]->board)->sio, NULL);
        mCoreConfigDeinit(&cores[i]->config);
        cores[i]->deinit(cores[i]);
    }
}

M_TEST_SUITE_DEFINE(GBARing, cmocka_unit_test(ownerCompletion), cmocka_unit_test(sixteenPlayerRing), cmocka_unit_test(missedCapture))
