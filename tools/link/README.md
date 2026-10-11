# Deterministic GBA link runner

Enable `BUILD_TEST=ON`, `BUILD_HEADLESS=ON`, and `BUILD_SUITE=ON` in CMake.
`mgba-link` uses the existing core and stock lockstep driver for 2–4 consoles;
`--ring` uses the independent normal-serial ring driver for 2–8 consoles.
A valid 16 KiB external BIOS is mandatory. BIOS boot is enabled; HLE fallback is
not used by the test runner.

```sh
source /workspace/.setup/activate.sh
cmake -S /workspace/mgba -B /workspace/.setup/mgba-build -DBUILD_TEST=ON
cmake --build /workspace/.setup/mgba-build -j5
/workspace/.setup/mgba-build/mgba-link \
  --bios /workspace/attachments/58098acf-8e29-4a0e-9936-f293c6a94520/gba.bin \
  --rom /workspace/mksc/baserom.gba --players 4 \
  --frames 1600 --input inputs.csv --screens /workspace/.setup/results/stock
```

Input CSV rows are `video_frame,console_index,held_key_mask_hex`, sorted by
frame, with zero-based console indices. A row replaces that console's held
keys until its next row. The masks are A=1, B=2, Select=4, Start=8, Right=10,
Left=20, Up=40, Down=80, R=100, L=200 (hex). Include explicit releases.
Frames include real BIOS startup. Inputs are applied separately at each
console's own frame boundary, so stock lockstep sleep/wake cannot starve input.

Output is one JSON object; emulator errors go to stderr. `--watch ADDRESS`
reads a 32-bit value from each console at the end. `--screens PREFIX` writes
240x160 PPM snapshots. Stock ROM-specific menu diagnostics refer to the AMKE
revision's known RAM layout; they are observational, not writes or assertions
about a modified game's layout. `game` reports its scene function, human count,
local controller ID, mode flags, and racer positions. These fields are meaningful
only in the corresponding stock scene; menu storage is released on race entry.
Stock `transfers` is not instrumented and is
reported as zero; only ring transfers have a counter.

`test_stock.py` and `inputs/stock-{vs,battle}.csv` exercise the original
four-player menus and gameplay. The test requires the original AMKE ROM and
supplied BIOS hashes, checks all four local IDs and independent held keys, and
compares the four racer positions across consoles. Run it with `--runner`,
`--rom`, `--bios`, and `--results`, as for `test_ring.py`. The fixture includes
BIOS startup and staggered character confirmation. It verifies gameplay entry
and synchronized positions, not completion of a race or every Battle rule.

`--expect-poc` fails unless the standalone ring homebrew reports correct IDs,
counts, >=100 commits, and zero errors/timeouts; it also rejects missed
receivers and clock collisions. `--expect-host N` changes the expected software
ID origin. `--break-frame N` opens the final return wire into console zero;
`--restore-frame N` reconnects it. These are physical-wire fault simulations,
not ROM memory injection. Use `test_ring.py` for input and recovery assertions.

The ring driver snapshots outgoing shift-register words at a transfer's first
edge and schedules per-core completion events at normal serial timing. It
preserves IRQ behavior and supports normal-8/normal-32 (the PoC exercises
normal-32). Unarmed receivers miss the transfer instead of being automatically
armed. Late mode changes and resets cancel the affected transfer. A second
clock owner attempting an overlapping transfer increments collisions.

Attached cores must run in one thread in timestamp order. The runner uses
a recurring 256-cycle scheduling event for the ring, including BIOS HALT,
and stock lockstep event boundaries for the normal cable. This driver is not integrated into Qt, a network lobby, or
savestates. Intermediate bit-edge visibility, partial shifts, electrical cable
characteristics, hot attachment, and multi-threaded ring emulation are outside
its present model.

`--dump ADDRESS` reads 32 words per console. `--keys-address ADDRESS` and
`--key-slots 1..8` select the held-key diagnostic vector for a shifted build.
`--barrier-state ADDRESS` accepts the MKSC ring state in IWRAM: after the
requested video-frame threshold, all consoles stop at the same future input
round, before physics starts. Failure to reach that barrier within 240 further
video frames exits with an error. This only controls emulator scheduling.
`displayRegisters` reports cached display IO, including write-only affine values.
`--trace-memory ADDRESS --trace-file PATH` observes a 16-byte range on console
zero and writes frame, PC, address, width, access kind, and written value as CSV;
this option requires `ENABLE_DEBUGGERS`.

MKSC READY probes can encounter consoles still rendering; those idle transfers
increase `missedReceivers`. Protected input trains must instead pass the game's
CRC, duplicate, sequence and commit checks. The homebrew's fully armed transfers
still require zero misses. Test ROMs should be copied to an immutable path before
starting a run: changing a memory-mapped ROM during emulation invalidates results.

The stock runner also accepts one console for ordinary GP regressions.
`--dump-words 1..4096` changes the snapshot size; snapshots use debugger reads
rather than executing game instructions. PPM files capture completed video
frames, avoiding a partly drawn framebuffer at a protocol barrier.
`--until-scene ADDRESS` waits until all cores reach the requested scene before
arming the end barrier; `--frames` then sets the maximum wait budget.
`--watch ADDRESS --until-watch-value VALUE` additionally waits for that 32-bit
value on every core, useful for cumulative results. Failed scene/barrier waits
still emit diagnostic JSON and screenshots and return a nonzero exit status.

`--autodrive-frame N` observes the native AMKE route and local driver's heading
and sends keypad steering after frame N. Large heading errors use A+B quick
turning. `--autofire` periodically presses L. These helpers never modify race
RAM, and are ROM-specific test controllers rather than emulator features for
arbitrary games. Emulator errors stop the run and fail it. Optional
`--debug-step-frame N` supports instruction-level failure diagnosis.
