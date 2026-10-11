/* SPDX-License-Identifier: MPL-2.0 */
/* Deterministic BIOS-backed link runner. No ROM or BIOS is bundled. */
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba/internal/gba/sio/ring.h>
#include <mgba-util/vfs.h>
#ifdef ENABLE_DEBUGGERS
#include <mgba/debugger/debugger.h>
#endif

#include <errno.h>
#include <math.h>

struct Player {
	struct mLockstepUser user;
	struct mCore* core;
	struct GBASIOLockstepDriver stock;
	struct GBASIORingDriver ring;
	struct mTimingEvent quantum;
	mColor pixels[240 * 160], completedPixels[240 * 160];
	unsigned id;
	bool asleep;
};

static void captureFrame(void* context) {
    struct Player* player = context;
    memcpy(player->completedPixels, player->pixels, sizeof(player->pixels));
}

static uint32_t snapshotWord(struct mCore* core, uint32_t address) {
    struct GBA* gba = core->board;
    uint32_t value;
    if (address >= 0x07000000 && address < 0x07000400) {
        memcpy(&value, (uint8_t*)gba->video.oam.raw + (address & 0x3FC), 4);
        return value;
    }
    return core->rawRead32(core, address, -1);
}

static void yieldRingCore(struct mTiming* timing, void* context, uint32_t late) {
	struct Player* player = context;
	/* HALT normally processes events until an IRQ wakes the CPU. Bound that
	 * advance so one console cannot pass its neighbours' serial edges. */
	((struct GBA*) player->core->board)->earlyExit = true;
	mTimingSchedule(timing, &player->quantum, late < 256 ? 256 - late : 1);
}

struct Input {
	unsigned frame, player, keys;
};

#ifdef ENABLE_DEBUGGERS
struct MemoryTrace {
	struct mDebuggerModule module;
	FILE* output;
};

static void traceMemory(struct mDebuggerModule* module, enum mDebuggerEntryReason reason, struct mDebuggerEntryInfo* info) {
    module->isPaused = false;
    struct MemoryTrace* trace = (struct MemoryTrace*) module;
    if (reason == DEBUGGER_ENTER_BREAKPOINT || reason == DEBUGGER_ENTER_ILLEGAL_OP) {
        int32_t divisor = 0;
        module->p->core->readRegister(module->p->core, "r1", &divisor);
        if (reason == DEBUGGER_ENTER_ILLEGAL_OP || !divisor || (info && info->address < 8 && module->p->core->frameCounter(module->p->core) > 500)) {
            fprintf(stderr, "Debugger event %u frame %u:", reason, module->p->core->frameCounter(module->p->core));
            for (unsigned i = 0; i < 16; ++i) {
                char name[8]; int32_t value = 0;
                snprintf(name, sizeof(name), "r%u", i);
                module->p->core->readRegister(module->p->core, name, &value);
                fprintf(stderr, " %s=%08x", name, (unsigned)value);
            }
            fputc('\n', stderr);
        }
        return;
    }
    if (reason != DEBUGGER_ENTER_WATCHPOINT || !info) return;
	int32_t pc = 0;
	module->p->core->readRegister(module->p->core, "pc", &pc);
	fprintf(trace->output, "%u,%08x,%08x,%u,%u,%08x\n", module->p->core->frameCounter(module->p->core), (unsigned) pc, info->address, info->width, info->type.wp.accessType, info->type.wp.newValue);
}

#endif

static unsigned emulationErrors;

static void logMessage(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	/* CpuSet alignment diagnostics still execute the BIOS implementation.
	 * Unsupported BIOS calls, unlike those diagnostics, fail the run. */
	if ((level & (mLOG_ERROR | mLOG_FATAL)) ||
	    (!strcmp(mLogCategoryName(category), "GBA BIOS") && (level & mLOG_STUB))) {
        if (++emulationErrors > 20) return;
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

struct Route {
    int x[128], y[128], count[128];
    unsigned checkpoints;
    bool inMenu;
};

/* A test driver sends only keypad input. It observes the native checkpoint
 * route and kart heading, and never changes race RAM or human/CPU flags. */
static void drivePlayer(struct mCore* core, unsigned slot, struct Route* route, bool autofire) {
    if ((core->busRead16(core, 0x04000000) & 7) == 3) {
        if (!route->inMenu) core->setKeys(core, 0);
        route->inMenu = true;
        return;
    }
    route->inMenu = false;
    slot = core->busRead8(core, 0x0300361C);
    uint32_t driver = core->busRead32(core, 0x03003638 + slot * 4);
    if (driver < 0x03000000 || driver >= 0x03007E00) return;
    if (!route->checkpoints) {
        unsigned n = core->busRead16(core, 0x03002E58);
        uint32_t track = core->busRead32(core, 0x030033B8);
        uint32_t routeOffset = core->busRead32(core, track + 0xCC);
        if (n < 2 || n > 128 || !routeOffset || track < 0x08000000 || track >= 0x0A000000) return;
        uint32_t header = track + routeOffset;
        uint32_t points = header + core->busRead8(core, header + 3) + (core->busRead8(core, header + 4) << 8);
        route->checkpoints = n;
        for (unsigned cp = 0; cp < n; ++cp) {
            route->x[cp] = core->busRead16(core, points + cp * 8) * 8;
            route->y[cp] = core->busRead16(core, points + cp * 8 + 2) * 8;
            route->count[cp] = 1;
        }
    }
    unsigned cp = core->busRead8(core, driver + 0x90) & 127;
    unsigned target = (cp + 1) % route->checkpoints;
    if (!route->count[target]) return;
    int x = (int32_t) core->busRead32(core, driver) >> 16;
    int y = (int32_t) core->busRead32(core, driver + 4) >> 16;
    double direction = atan2(route->x[target] - x, y - route->y[target]);
    int desired = (int) (direction * 65536.0 / (2 * 3.141592653589793));
    int16_t error = desired - core->busRead16(core, driver + 0x7C);
    unsigned keys = abs(error) > 8000 ? 3 : 1;
    if (error > 700) keys |= 16;
    if (error < -700) keys |= 32;
    if (autofire && core->frameCounter(core) % 120 == 0) keys |= 512;
    core->setKeys(core, keys);
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
	const char *rom = NULL, *bios = NULL, *inputPath = NULL, *prefix = NULL, *tracePath = NULL;
	unsigned players = 0, frames = 600;
	unsigned expectHost = 0, breakFrame = UINT_MAX, restoreFrame = UINT_MAX;
	uint32_t watch = 0, untilWatchValue = 0;
    bool untilWatch = false;
	uint32_t traceAddress = 0;
	uint32_t dumpAddress = 0, keysAddress = 0x03004F30;
	uint32_t barrierState = 0, untilScene = 0;
	unsigned dumpWords = 32;
	unsigned keySlots = 4, autodriveFrame = UINT_MAX, debugStepFrame = UINT_MAX;
	bool ringMode = false, expectPoc = false, autofire = false, hle = false;
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--hle")) { hle = true; continue; }
		if (!strcmp(argv[i], "--autofire")) { autofire = true; continue; }
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
		else if (!strcmp(option, "--trace-file")) tracePath = value;
		else if (!strcmp(option, "--until-scene") && number(value, &n) && n >= 0x08000000 && n < 0x0A000000) untilScene = n;
		else if (!strcmp(option, "--barrier-state") && number(value, &n) && n >= 0x03000000 && n <= 0x03007F88) barrierState = n;
		else if (!strcmp(option, "--trace-memory") && number(value, &n) && n < UINT32_MAX - 16) traceAddress = n;
		else if (!strcmp(option, "--dump-words") && number(value, &n) && n > 0 && n <= 4096) dumpWords = n;
		else if (!strcmp(option, "--dump") && number(value, &n) && n < UINT32_MAX - 128) dumpAddress = n;
		else if (!strcmp(option, "--keys-address") && number(value, &n) && n < UINT32_MAX - 16) keysAddress = n;
		else if (!strcmp(option, "--key-slots") && number(value, &n) && n > 0 && n <= 8) keySlots = n;
		else if (!strcmp(option, "--players") && number(value, &n) && n <= 8) players = n;
		else if (!strcmp(option, "--debug-step-frame") && number(value, &n) && n < UINT_MAX) debugStepFrame = n;
		else if (!strcmp(option, "--autodrive-frame") && number(value, &n) && n < UINT_MAX) autodriveFrame = n;
		else if (!strcmp(option, "--frames") && number(value, &n) && n <= 100000) frames = n;
		else if (!strcmp(option, "--until-watch-value") && number(value, &n) && n <= UINT32_MAX) { untilWatchValue = n; untilWatch = true; }
		else if (!strcmp(option, "--watch") && number(value, &n) && n <= UINT32_MAX) watch = n;
		else if (!strcmp(option, "--expect-host") && number(value, &n) && n < 8) expectHost = n;
		else if (!strcmp(option, "--break-frame") && number(value, &n) && n < UINT_MAX) breakFrame = n;
		else if (!strcmp(option, "--restore-frame") && number(value, &n) && n < UINT_MAX) restoreFrame = n;
		else { fprintf(stderr, "Invalid option: %s %s\n", option, value); return 2; }
	}
	if (!rom || (!hle && !bios) || (hle && bios) || players < (ringMode ? 2U : 1U) || players > (ringMode ? 8U : 4U) || !frames || expectHost >= players || (dumpAddress > UINT32_MAX - dumpWords * 4) || (untilWatch && (!watch || !untilScene)) || (untilScene && !barrierState) || (barrierState && !ringMode) || (expectPoc && !ringMode) || (breakFrame != UINT_MAX && (!ringMode || restoreFrame <= breakFrame))) {
		fprintf(stderr, "Usage: mgba-link --rom FILE (--bios FILE | --hle) --players 1..4 [--ring (2..8)] [--frames N] [--input CSV] [--screens PREFIX] [--watch ADDRESS] [--expect-poc]\n");
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
#ifdef ENABLE_DEBUGGERS
	struct mDebugger debugger;
	struct MemoryTrace trace = { .module = { .type = DEBUGGER_CUSTOM, .entered = traceMemory } };
	bool debuggerAttached = false;
#else
    if (tracePath || traceAddress) { fprintf(stderr, "Memory tracing requires ENABLE_DEBUGGERS\n"); goto done; }
#endif
	for (unsigned i = 0; i < players; ++i) {
		p[i].id = i;
		p[i].core = GBACoreCreate();
		struct mCore* core = p[i].core;
		if (!core || !core->init(core)) { fprintf(stderr, "Core init failed\n"); goto done; }
		++initialized;
		mCoreInitConfig(core, "link-test");
		mCoreConfigSetDefaultIntValue(&core->config, "useBios", !hle);
		mCoreConfigSetDefaultIntValue(&core->config, "skipBios", hle);
		mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "ignore");
		mCoreConfigSetDefaultIntValue(&core->config, "mute", true);
		mCoreLoadConfig(core);
		core->opts.useBios = !hle;
		core->opts.skipBios = hle;
		core->setVideoBuffer(core, p[i].pixels, 240);
        struct mCoreCallbacks callbacks = { .context = &p[i], .videoFrameEnded = captureFrame };
        core->addCoreCallbacks(core, &callbacks);
		if (!hle) {
			struct VFile* vf = VFileOpen(bios, O_RDONLY);
			if (!vf || vf->size(vf) != 16384 || !GBAIsBIOS(vf)) {
				if (vf) vf->close(vf);
				fprintf(stderr, "A valid 16 KiB GBA BIOS is required\n"); goto done;
			}
			if (!core->loadBIOS(core, vf, 0)) {
				fprintf(stderr, "BIOS/ROM load failed\n"); goto done;
			}
		}
		if (!mCoreLoadFile(core, rom)) { fprintf(stderr, "ROM load failed\n"); goto done; }
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
		if ((hle && gba->biosVf) || (!hle && !gba->biosVf)) { fprintf(stderr, "Requested BIOS mode was not applied\n"); goto done; }
		if (ringMode) {
			p[i].quantum = (struct mTimingEvent) { .context = &p[i], .callback = yieldRingCore, .name = "Ring runner scheduling quantum", .priority = 0xFF };
			mTimingSchedule(core->timing, &p[i].quantum, 256);
		}
	}
#ifdef ENABLE_DEBUGGERS
	if (tracePath && traceAddress) {
		trace.output = fopen(tracePath, "w");
		if (!trace.output) { perror(tracePath); goto done; }
		mDebuggerInit(&debugger);
		mDebuggerAttach(&debugger, p[0].core);
		mDebuggerAttachModule(&debugger, &trace.module);
		debuggerAttached = true;
		struct mWatchpoint wp = { .segment = -1, .minAddress = traceAddress, .maxAddress = traceAddress + 16, .type = WATCHPOINT_RW };
		if (debugger.platform->setWatchpoint(debugger.platform, &trace.module, &wp) < 0) goto done;
        struct mBreakpoint bp = { .segment = -1, .address = 0x3B4, .type = BREAKPOINT_HARDWARE };
        if (debugger.platform->setBreakpoint(debugger.platform, &trace.module, &bp) < 0) goto done;
        bp.address = 0;
        if (debugger.platform->setBreakpoint(debugger.platform, &trace.module, &bp) < 0) goto done;
        bp.address = 4;
        if (debugger.platform->setBreakpoint(debugger.platform, &trace.module, &bp) < 0) goto done;
	}
#endif
	/* Input rows affect only the hardware keypad. The ring scheduling event
     * bounds even BIOS HALT intervals to 256 cycles; stock sleep/wake is respected. */
	bool* applied = calloc(nInputs ? nInputs : 1, sizeof(*applied));
	if (!applied) goto done;
	uint64_t steps = 0;
	unsigned lastMinFrame = 0;
    struct InstructionHistory { uint32_t pc, instruction, sp, lr, top; } history[1024] = {{0}};
    unsigned historyIndex = 0;
    unsigned sceneCheckFrame = UINT_MAX;
    bool allSceneMatch = false, snapshotFailed = false;
	uint64_t stagnantSteps = 0;
	uint32_t barrierRound = 0;
	bool frozen[GBA_RING_MAX_PLAYERS] = { false };
    struct Route routes[GBA_RING_MAX_PLAYERS] = {0};
    unsigned drivenFrame[GBA_RING_MAX_PLAYERS] = {0};
	for (;;) {
        if (emulationErrors) {
            fprintf(stderr, "Emulator reported an error; stopping the run\n");
            free(applied); goto done;
        }
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
		if (barrierState && minFrame > frames + 240) {
            fprintf(stderr, "Requested game barrier was not reached within 240 video frames\n");
            snapshotFailed = true;
            break;
        }
        if (untilScene && minFrame != sceneCheckFrame) {
            sceneCheckFrame = minFrame;
            allSceneMatch = true;
            for (unsigned i = 0; i < players; ++i)
                if (p[i].core->busRead32(p[i].core, 0x03002E20) != untilScene ||
                    (untilWatch && p[i].core->busRead32(p[i].core, watch) != untilWatchValue)) allSceneMatch = false;
        }
        if (untilScene && !allSceneMatch && minFrame >= frames) {
            fprintf(stderr, "Requested scene was not reached within the video-frame budget\n");
            break;
        }
        if (barrierState && (untilScene ? allSceneMatch : minFrame >= frames) && !barrierRound) {
			for (unsigned i = 0; i < players; ++i) {
				uint32_t round = p[i].core->busRead32(p[i].core, barrierState + 80);
				if (round > barrierRound) barrierRound = round;
			}
			barrierRound += 2;
		}
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
			if (barrierRound && core->busRead32(core, barrierState + 80) == barrierRound &&
			    core->busRead8(core, barrierState + 113) && !core->busRead8(core, barrierState + 112) &&
			    !core->busRead8(core, barrierState + 119)) frozen[i] = true;
			if (barrierState ? !frozen[i] : frame < frames) complete = false;
			for (size_t e = 0; e < nInputs; ++e) {
				if (!applied[e] && inputs[e].player == i && frame >= inputs[e].frame) {
					core->setKeys(core, inputs[e].keys);
					applied[e] = true;
				}
			}
            if (frame >= autodriveFrame && frame != drivenFrame[i]) {
                drivePlayer(core, i, &routes[i], autofire);
                drivenFrame[i] = frame;
            }
			if (p[i].asleep || frozen[i]) continue;
			uint64_t now = mTimingGlobalTime(core->timing);
			if (now < earliest) { earliest = now; selected = i; }
		}
		if (complete) break;
		if (selected < 0 || ++steps > (uint64_t) frames * players * 400000) {
			fprintf(stderr, "Link deadlock or instruction budget exhausted\n"); free(applied); goto done;
		}
#ifdef ENABLE_DEBUGGERS
        if (selected == 0 && debuggerAttached && p[0].core->frameCounter(p[0].core) >= debugStepFrame)
            {
            int32_t pc, sp, lr;
            p[0].core->readRegister(p[0].core, "pc", &pc);
            p[0].core->readRegister(p[0].core, "sp", &sp);
            p[0].core->readRegister(p[0].core, "lr", &lr);
            history[historyIndex++ & 1023] = (struct InstructionHistory) { pc, p[0].core->busRead32(p[0].core, pc & ~1), sp, lr, p[0].core->busRead32(p[0].core, sp) };
            if ((uint32_t)sp < 0x03000000 || (uint32_t)sp >= 0x03008000) {
                for (unsigned h = 0; h < 1024; ++h) {
                    unsigned index = (historyIndex + h) & 1023;
                    fprintf(stderr, "History %08x ins=%08x sp=%08x lr=%08x top=%08x\n", history[index].pc, history[index].instruction, history[index].sp, history[index].lr, history[index].top);
                }
                fprintf(stderr, "Invalid stack at frame %u\n", p[0].core->frameCounter(p[0].core));
                free(applied); goto done;
            }
            mDebuggerRun(&debugger);
        }
        else
#endif
		    p[selected].core->runLoop(p[selected].core);
	}
	free(applied);
	result = snapshotFailed || (untilScene && !allSceneMatch) ? 1 : 0;
	printf("{\"bios\":\"%s\",\"topology\":\"%s\",\"players\":%u,\"frames\":%u,\"steps\":%llu,\"transfers\":%llu,\"missedReceivers\":%llu,\"collisions\":%llu,\"nodes\":[", hle ? "hle" : "real", ringMode ? "ring" : "stock", players, frames, (unsigned long long) steps, (unsigned long long) ring.transfers, (unsigned long long) ring.missedReceivers, (unsigned long long) ring.collisions);
	for (unsigned i = 0; i < players; ++i) {
		struct mCore* core = p[i].core;
		int32_t pc = 0;
		core->readRegister(core, "pc", &pc);
		int32_t lr = 0, sp = 0;
		core->readRegister(core, "lr", &lr);
		core->readRegister(core, "sp", &sp);
		uint32_t magic = core->busRead32(core, 0x02000000);
		uint32_t id = core->busRead32(core, 0x02000004);
		uint32_t count = core->busRead32(core, 0x02000008);
		uint32_t rounds = core->busRead32(core, 0x0200000C);
		uint32_t errors = core->busRead32(core, 0x02000010);
		uint32_t timeouts = core->busRead32(core, 0x02000014);
		if (expectPoc && (magic != 0x52494E47 || id != (i + players - expectHost) % players || count != players || rounds < 100 || errors || timeouts)) result = 1;
		printf("%s{\"slot\":%u,\"frame\":%u,\"siocnt\":%u,\"watch\":%u,\"poc\":{\"magic\":%u,\"id\":%u,\"count\":%u,\"rounds\":%u,\"errors\":%u,\"timeouts\":%u,\"frameCycles\":%u,\"inputs\":[", i ? "," : "", i, core->frameCounter(core), core->busRead16(core, 0x04000128), watch ? core->busRead32(core, watch) : 0, magic, id, count, rounds, errors, timeouts, core->busRead32(core, 0x02000040));
		for (unsigned j = 0; j < 8; ++j) printf("%s%u", j ? "," : "", core->busRead32(core, 0x02000020 + j * 4));
		printf("]},\"pc\":%u,\"lr\":%u,\"sp\":%u,\"irq\":%u,\"ime\":%u,\"gameFrames\":%u", (unsigned) pc, (unsigned) lr, (unsigned) sp, core->busRead16(core, 0x04000200), core->busRead16(core, 0x04000208), core->busRead32(core, 0x03002E34));
        if (routes[i].checkpoints) {
            printf(",\"route\":[");
            for (unsigned cp = 0; cp < routes[i].checkpoints; ++cp)
                printf("%s[%d,%d,%d]", cp ? "," : "", routes[i].x[cp], routes[i].y[cp], routes[i].count[cp]);
            printf("]");
        }
        if (barrierState) {
            printf(",\"barrier\":{\"round\":%u,\"errors\":%u,\"timeouts\":%u,\"id\":%u,\"count\":%u}",
                   core->busRead32(core, barrierState + 80), core->busRead32(core, barrierState + 88),
                   core->busRead32(core, barrierState + 92), core->busRead8(core, barrierState + 106),
                   core->busRead8(core, barrierState + 107));
        }
		if (dumpAddress) {
			printf(",\"dump\":[");
			for (unsigned j = 0; j < dumpWords; ++j) printf("%s%u", j ? "," : "", snapshotWord(core, dumpAddress + j * 4));
			printf("]");
		}
        {
            struct GBA* gba = core->board;
            printf(",\"displayRegisters\":[");
            for (unsigned j = 0; j < 48; ++j) printf("%s%u", j ? "," : "", gba->memory.io[j]);
            printf("]");
        }
		{
			printf(",\"game\":{\"sceneMain\":%u,\"humanCount\":%u,\"localId\":%u,\"raceFlags\":%u,\"drivers\":[", core->busRead32(core, 0x03002E20), core->busRead8(core, 0x0300361B), core->busRead8(core, 0x0300361C), core->busRead16(core, 0x0300361E));
			for (unsigned j = 0; j < 8; ++j) {
				uint32_t driver = core->busRead32(core, 0x03003638 + j * 4);
				bool valid = (driver >= 0x02000000 && driver < 0x0203FFBC) || (driver >= 0x03000000 && driver < 0x03007FBC);
				printf("%s{\"address\":%u,\"position\":[%d,%d,%d]", j ? "," : "", driver,
				       valid ? (int32_t) core->busRead32(core, driver) : 0,
				       valid ? (int32_t) core->busRead32(core, driver + 4) : 0,
				       valid ? (int32_t) core->busRead32(core, driver + 8) : 0);
                printf(",\"id\":%u,\"character\":%u,\"yaw\":%u,\"checkpoint\":%u,\"lap\":%u,\"balloons\":%u,\"graphicsSource\":%u}",
                       valid ? core->busRead8(core, driver + 0xEC) : 255,
                       valid ? core->busRead8(core, driver + 0x4C) : 255,
                       valid ? core->busRead16(core, driver + 0x7C) : 0,
                       valid ? core->busRead8(core, driver + 0x90) & 127 : 0,
                       valid ? (int) core->busRead8(core, driver + 0xC7) - 127 : 0,
                       valid ? core->busRead8(core, driver + 0x17D) : 0,
                       valid ? core->busRead32(core, driver + 0xD0) : 0);
			}
			printf("]}");
			uint32_t menu = core->busRead32(core, 0x0203EBE8);
            bool menuActive = menu >= 0x02000000 && menu <= 0x0203F278;
			printf(",\"menu\":{\"address\":%u,\"active\":%s,\"playersMinusOne\":%u,\"localId\":%u,\"state\":%u,\"selection\":[", menu, menuActive ? "true" : "false", menuActive ? core->busRead32(core, menu + 8) : 0, menuActive ? core->busRead32(core, menu + 12) : 0, menuActive ? core->busRead32(core, menu + 0xD84) : 0);
			for (unsigned j = 0; j < 4; ++j) printf("%s%u", j ? "," : "", menuActive ? core->busRead32(core, menu + 0x4FC + j * 4) : 0);
			printf("],\"characters\":[");
			for (unsigned j = 0; j < 4; ++j) printf("%s%u", j ? "," : "", menuActive ? core->busRead32(core, menu + 0x4EC + j * 4) : 0);
			printf("],\"heldKeys\":[");
			for (unsigned j = 0; j < keySlots; ++j) printf("%s%u", j ? "," : "", core->busRead16(core, keysAddress + j * 2));
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
				mColor c = p[i].completedPixels[j];
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
#ifdef ENABLE_DEBUGGERS
	if (debuggerAttached) {
		mDebuggerDetachModule(&debugger, &trace.module);
		p[0].core->detachDebugger(p[0].core);
		mDebuggerDeinit(&debugger);
	}
	if (trace.output) fclose(trace.output);
#endif
	for (unsigned i = 0; i < initialized; ++i) {
		if (ringMode) mTimingDeschedule(p[i].core->timing, &p[i].quantum);
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
