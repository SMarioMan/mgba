/* SPDX-License-Identifier: MPL-2.0 */
/* Deterministic BIOS-backed link runner. No ROM or BIOS is bundled. */
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba/internal/gba/sio/ring.h>
#include <mgba-util/vfs.h>

#include <errno.h>

struct Player {
	struct mLockstepUser user;
	struct mCore* core;
	struct GBASIOLockstepDriver stock;
	struct GBASIORingDriver ring;
	mColor pixels[240 * 160];
	unsigned id;
	bool asleep;
};

struct Input {
	unsigned frame, player, keys;
};

static void logMessage(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	if (level & (mLOG_ERROR | mLOG_FATAL)) {
		fprintf(stderr, "%s: ", mLogCategoryName(category));
		vfprintf(stderr, format, args);
		fputc('\n', stderr);
	}
}

static void sleepPlayer(struct mLockstepUser* user) {
	((struct Player*) user)->asleep = true;
}

static void wakePlayer(struct mLockstepUser* user) {
	((struct Player*) user)->asleep = false;
}

static int playerId(struct mLockstepUser* user) {
	return ((struct Player*) user)->id;
}

static bool number(const char* text, unsigned long* out) {
	char* end;
	errno = 0;
	*out = strtoul(text, &end, 0);
	return !errno && *text && !*end;
}

int main(int argc, char** argv) {
	struct mLogger logger = { .log = logMessage };
	mLogSetDefaultLogger(&logger);
	const char *rom = NULL, *bios = NULL, *inputPath = NULL, *prefix = NULL;
	unsigned players = 0, frames = 600;
	unsigned expectHost = 0, breakFrame = UINT_MAX, restoreFrame = UINT_MAX;
	uint32_t watch = 0;
	bool ringMode = false, expectPoc = false;
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--ring")) { ringMode = true; continue; }
		if (!strcmp(argv[i], "--expect-poc")) { expectPoc = true; continue; }
		if (i + 1 == argc) { fprintf(stderr, "Missing option value\n"); return 2; }
		const char* option = argv[i++];
		const char* value = argv[i];
		unsigned long n;
		if (!strcmp(option, "--rom")) rom = value;
		else if (!strcmp(option, "--bios")) bios = value;
		else if (!strcmp(option, "--input")) inputPath = value;
		else if (!strcmp(option, "--screens")) prefix = value;
		else if (!strcmp(option, "--players") && number(value, &n) && n <= 8) players = n;
		else if (!strcmp(option, "--frames") && number(value, &n) && n <= 100000) frames = n;
		else if (!strcmp(option, "--watch") && number(value, &n) && n <= UINT32_MAX) watch = n;
		else if (!strcmp(option, "--expect-host") && number(value, &n) && n < 8) expectHost = n;
		else if (!strcmp(option, "--break-frame") && number(value, &n) && n < UINT_MAX) breakFrame = n;
		else if (!strcmp(option, "--restore-frame") && number(value, &n) && n < UINT_MAX) restoreFrame = n;
		else { fprintf(stderr, "Invalid option: %s %s\n", option, value); return 2; }
	}
	if (!rom || !bios || players < 2 || players > (ringMode ? 8U : 4U) || !frames || expectHost >= players || (expectPoc && !ringMode) || (breakFrame != UINT_MAX && (!ringMode || restoreFrame <= breakFrame))) {
		fprintf(stderr, "Usage: mgba-link --rom FILE --bios FILE --players 2..4 [--ring (2..8)] [--frames N] [--input CSV] [--screens PREFIX] [--watch ADDRESS] [--expect-poc]\n");
		return 2;
	}
	struct Input* inputs = NULL;
	size_t nInputs = 0;
	if (inputPath) {
		FILE* f = fopen(inputPath, "r");
		if (!f) { perror(inputPath); return 2; }
		char line[128];
		while (fgets(line, sizeof(line), f)) {
			if (line[0] == '#' || line[0] == '\n') continue;
			struct Input event;
			char extra;
			if (sscanf(line, "%u,%u,%x %c", &event.frame, &event.player, &event.keys, &extra) != 3 || event.player >= players || event.keys > 0x3FF || (nInputs && event.frame < inputs[nInputs - 1].frame)) {
				fprintf(stderr, "Invalid or unsorted input CSV row: %s", line); fclose(f); free(inputs); return 2;
			}
			struct Input* grown = realloc(inputs, (nInputs + 1) * sizeof(*inputs));
			if (!grown) { fclose(f); free(inputs); return 2; }
			inputs = grown;
			inputs[nInputs++] = event;
		}
		fclose(f);
	}
	struct Player* p = calloc(players, sizeof(*p));
	if (!p) { free(inputs); return 2; }
	struct GBASIOLockstepCoordinator stock;
	struct GBASIORing ring;
	GBASIOLockstepCoordinatorInit(&stock);
	GBASIORingInit(&ring);
	unsigned initialized = 0;
	int result = 1;
	for (unsigned i = 0; i < players; ++i) {
		p[i].id = i;
		p[i].core = GBACoreCreate();
		struct mCore* core = p[i].core;
		if (!core || !core->init(core)) { fprintf(stderr, "Core init failed\n"); goto done; }
		++initialized;
		mCoreInitConfig(core, "link-test");
		mCoreConfigSetDefaultIntValue(&core->config, "useBios", true);
		mCoreConfigSetDefaultIntValue(&core->config, "skipBios", false);
		mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "ignore");
		mCoreConfigSetDefaultIntValue(&core->config, "mute", true);
		mCoreLoadConfig(core);
		core->setVideoBuffer(core, p[i].pixels, 240);
		struct VFile* vf = VFileOpen(bios, O_RDONLY);
		if (!vf || vf->size(vf) != 16384 || !GBAIsBIOS(vf)) {
			if (vf) vf->close(vf);
			fprintf(stderr, "A valid 16 KiB GBA BIOS is required\n"); goto done;
		}
		if (!core->loadBIOS(core, vf, 0) || !mCoreLoadFile(core, rom)) {
			fprintf(stderr, "BIOS/ROM load failed\n"); goto done;
		}
		struct GBA* gba = core->board;
		if (ringMode) {
			if (!GBASIORingAttach(&ring, &p[i].ring)) goto done;
			GBASIOSetDriver(&gba->sio, &p[i].ring.d);
		} else {
			p[i].user.sleep = sleepPlayer;
			p[i].user.wake = wakePlayer;
			p[i].user.requestedId = playerId;
			GBASIOLockstepDriverCreate(&p[i].stock, &p[i].user);
			GBASIOLockstepCoordinatorAttach(&stock, &p[i].stock);
			GBASIOSetDriver(&gba->sio, &p[i].stock.d);
		}
		core->reset(core);
	}
	/* Each input row sets held keys for one console at its own video frame.
	 * Advancing the least-ahead runnable core bounds skew to an instruction
	 * or hardware HALT interval. Stock lockstep sleep/wake is respected. */
	bool* applied = calloc(nInputs ? nInputs : 1, sizeof(*applied));
	if (!applied) goto done;
	uint64_t steps = 0;
	unsigned lastMinFrame = 0;
	uint64_t stagnantSteps = 0;
	for (;;) {
		int selected = -1;
		uint64_t earliest = UINT64_MAX;
		bool complete = true;
		unsigned minFrame = UINT_MAX;
		for (unsigned i = 0; i < players; ++i) {
			unsigned f = p[i].core->frameCounter(p[i].core);
			if (f < minFrame) minFrame = f;
		}
		if (minFrame >= breakFrame) ring.openInputs = 1;
		if (minFrame >= restoreFrame) ring.openInputs = 0;
		if (minFrame != lastMinFrame) {
			lastMinFrame = minFrame;
			stagnantSteps = 0;
		} else if (++stagnantSteps > (ringMode ? 4000000U : 1000000U)) {
			fprintf(stderr, "No video-frame progress; nodes:");
			for (unsigned i = 0; i < players; ++i) {
				int32_t pc = 0;
				p[i].core->readRegister(p[i].core, "pc", &pc);
				fprintf(stderr, " %u(frame=%u,pc=%08x,sleep=%u)", i, p[i].core->frameCounter(p[i].core), (unsigned) pc, p[i].asleep);
			}
			fputc('\n', stderr);
			free(applied);
			goto done;
		}
		for (unsigned i = 0; i < players; ++i) {
			struct mCore* core = p[i].core;
			unsigned frame = core->frameCounter(core);
			if (frame < frames) complete = false;
			for (size_t e = 0; e < nInputs; ++e) {
				if (!applied[e] && inputs[e].player == i && frame >= inputs[e].frame) {
					core->setKeys(core, inputs[e].keys);
					applied[e] = true;
				}
			}
			if (p[i].asleep) continue;
			uint64_t now = mTimingGlobalTime(core->timing);
			if (now < earliest) { earliest = now; selected = i; }
		}
		if (complete) break;
		if (selected < 0 || ++steps > (uint64_t) frames * players * 400000) {
			fprintf(stderr, "Link deadlock or instruction budget exhausted\n"); free(applied); goto done;
		}
		if (ringMode) p[selected].core->step(p[selected].core);
		else p[selected].core->runLoop(p[selected].core);
	}
	free(applied);
	result = 0;
	printf("{\"topology\":\"%s\",\"players\":%u,\"frames\":%u,\"steps\":%llu,\"transfers\":%llu,\"missedReceivers\":%llu,\"collisions\":%llu,\"nodes\":[", ringMode ? "ring" : "stock", players, frames, (unsigned long long) steps, (unsigned long long) ring.transfers, (unsigned long long) ring.missedReceivers, (unsigned long long) ring.collisions);
	for (unsigned i = 0; i < players; ++i) {
		struct mCore* core = p[i].core;
		uint32_t magic = core->busRead32(core, 0x02000000);
		uint32_t id = core->busRead32(core, 0x02000004);
		uint32_t count = core->busRead32(core, 0x02000008);
		uint32_t rounds = core->busRead32(core, 0x0200000C);
		uint32_t errors = core->busRead32(core, 0x02000010);
		uint32_t timeouts = core->busRead32(core, 0x02000014);
		if (expectPoc && (magic != 0x52494E47 || id != (i + players - expectHost) % players || count != players || rounds < 100 || errors || timeouts)) result = 1;
		printf("%s{\"slot\":%u,\"frame\":%u,\"siocnt\":%u,\"watch\":%u,\"poc\":{\"magic\":%u,\"id\":%u,\"count\":%u,\"rounds\":%u,\"errors\":%u,\"timeouts\":%u,\"frameCycles\":%u,\"inputs\":[", i ? "," : "", i, core->frameCounter(core), core->busRead16(core, 0x04000128), watch ? core->busRead32(core, watch) : 0, magic, id, count, rounds, errors, timeouts, core->busRead32(core, 0x02000040));
		for (unsigned j = 0; j < 8; ++j) printf("%s%u", j ? "," : "", core->busRead32(core, 0x02000020 + j * 4));
		printf("]}");
		if (!ringMode) {
			uint32_t menu = core->busRead32(core, 0x0203EBE8);
			printf(",\"menu\":{\"address\":%u,\"playersMinusOne\":%u,\"localId\":%u,\"state\":%u,\"selection\":[", menu, core->busRead32(core, menu + 8), core->busRead32(core, menu + 12), core->busRead32(core, menu + 0xD84));
			for (unsigned j = 0; j < 4; ++j) printf("%s%u", j ? "," : "", core->busRead32(core, menu + 0x4FC + j * 4));
			printf("],\"characters\":[");
			for (unsigned j = 0; j < 4; ++j) printf("%s%u", j ? "," : "", core->busRead32(core, menu + 0x4EC + j * 4));
			printf("],\"heldKeys\":[");
			for (unsigned j = 0; j < 4; ++j) printf("%s%u", j ? "," : "", core->busRead16(core, 0x03004F30 + j * 2));
			printf("]}");
		}
		printf("}");
		if (prefix) {
			char path[4096];
			if (snprintf(path, sizeof(path), "%s-%u.ppm", prefix, i) >= (int) sizeof(path)) { result = 1; continue; }
			FILE* f = fopen(path, "wb");
			if (!f) { perror(path); result = 1; continue; }
			fprintf(f, "P6\n240 160\n255\n");
			for (unsigned j = 0; j < 240 * 160; ++j) {
				mColor c = p[i].pixels[j];
#ifdef COLOR_16_BIT
#ifdef COLOR_5_6_5
				unsigned char rgb[] = { (c & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, ((c >> 11) & 31) * 255 / 31 };
#else
				unsigned char rgb[] = { (c & 31) * 255 / 31, ((c >> 5) & 31) * 255 / 31, ((c >> 10) & 31) * 255 / 31 };
#endif
#else
				unsigned char rgb[] = { c & 255, (c >> 8) & 255, (c >> 16) & 255 };
#endif
				if (fwrite(rgb, 1, 3, f) != 3) result = 1;
			}
			if (fclose(f)) result = 1;
		}
	}
	printf("]}\n");
	if (expectPoc && (!ring.transfers || ring.collisions || ring.missedReceivers)) result = 1;
done:
	for (unsigned i = 0; i < initialized; ++i) {
		if (ringMode && p[i].ring.ring) GBASIORingDetach(&p[i].ring);
		mCoreConfigDeinit(&p[i].core->config);
		p[i].core->deinit(p[i].core);
	}
	GBASIOLockstepCoordinatorDeinit(&stock);
	free(p);
	free(inputs);
	mLogSetDefaultLogger(NULL);
	return result;
}
