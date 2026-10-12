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

M_TEST_SUITE_DEFINE(GBARing, cmocka_unit_test(ownerCompletion))
