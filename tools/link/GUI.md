# Local ring links in the Qt GUI

Build the modified Qt frontend with `BUILD_QT=ON` and `BUILD_SDL=ON`.
The prepared cloud environment provides `/workspace/.setup/build-qt.sh`;
its executable is `/workspace/.setup/mgba-qt/qt/mgba-qt`.
Source `/workspace/.setup/activate.sh` before running it there.

1. Configure the GBA BIOS in Settings, enable its use, and keep BIOS boot enabled.
2. Choose **File → GBA link topology... → Ring link (up to 8 players)**.
3. Use **New multiplayer window** for each additional console and load
   `mksc-ring.gba` in every window. Configure each window's input bindings.
4. Select **Ring Link** in each game's link submenu. Once everyone is connected,
   press Start on one console and use the shared menus to select Vs. or Battle.

The topology is shared by all local windows and saved in the Qt configuration.
Standard GBA multiplayer retains its four-console limit. Switching a loaded
session's topology reconnects its emulated cables; do it before joining a game.
Switching eight open windows back to standard requires closing the extras.
Game Boy cores cannot join a GBA ring.

Ring Pause pauses/resumes the group. Reset and closing a participant cancel
pending transfers; the game can report the lost connection and return to its
connection flow. Keyboard/SDL input slots and per-player save IDs support eight
windows. This feature does not add network play or connected-session savestates.

## Scheduling and validation

The threaded adapter runs cores in shared timestamp order, aligns reset/late
boot clock epochs, and yields at a bounded interval. Armed transfers shorten
that interval according to native serial timing, capped at 2,048 cycles
(512 for a fast eight-bit transfer). Up to 4,096 slices execute
cooperatively on one worker, borrowing only peers actually asleep on the
adapter's WAIT condition. Interrupt acknowledgements wait until that peer's
slice finishes. This removes an operating-system thread handoff for each tiny
serial slice without changing cable timing or timestamp order.
The worker retains each waiting peer's borrow reservation for the batch and
caches its clock and serial interval. A new interrupt revokes that reservation
at the next slice. Serial mode/start changes update the interval; ordinary
interpreter returns do not rescan every cable port.

Audio/video consumer waits occur outside the cable mutex, on the output owner's
thread only. A worker yields to a peer needing its consumers instead of waiting
on that peer's output. This allows GUI pause/reset/close to interrupt the correct
worker, including when audio/video synchronization is enabled.
The first cable slot paces the group. Peer outputs use bounded resampling
correction for independent audio-device clocks. Interpolation runs outside
the producer mutex, and the existing threaded scanline renderer is enabled
by default for ring sessions unless explicitly configured otherwise.
Qt Multimedia ring outputs create their sink, PCM source and timers on a
dedicated audio worker for each window. Painting and settings dialogs do not
service those sinks. Stop completes on the worker before releasing its core;
suspended sinks discard their queued tail instead of draining a corked stream.
Changing topology recreates the audio outputs with the appropriate pacing.

`test-qt-ring_gui` uses real Qt windows and the File-menu dialog, then supplies
keypad inputs to each native core. Private fixtures enable Vs./Battle entry for
two/eight ring consoles and four standard consoles, with BIOS presence, native
human racer IDs, protocol errors, group pause, reset, middle-window removal,
late boot of a second console, and close/reopen checks. Complete-round and synchronized-physics validation
remain covered by the headless harness.

The previous release's BIOS-backed GUI checks passed for both ring modes with two/eight windows and
both standard modes with four windows. Synchronized two- and eight-window
playback and staggered boot also passed. The final private-fixture run passes
all 35 CTest targets, including all 13 GUI rows with no skips. A separate launch
using the distributed executable, bundled libraries and final boot-shared ROM
passes eight-window startup and reports 1.000 native simulation updates per
video frame on all eight consoles. These are software/offscreen checks; they
do not establish desktop wall-clock throughput. Without private fixtures, GUI
gameplay rows explicitly skip.

With the same pre-fix ROM and two-console Vs. input fixture (4,300 emulated
frames), GUI entry took 58,939 ms with the original adapter and 9,369 ms with
cooperative scheduling, about 6.3 times faster. These were offscreen runs on
the cloud VM; they isolate emulator scheduling from the ROM clock/rendering
changes and do not establish eight-window desktop frame rates.

The Windows-safe `_ring_write` callback rename from `tokenring` commit
`64ebbcf38` is included. The game-side rendering fixes are also required: the
stock opponent-marker switch only handles three opponents, and native remote
texture scheduling can starve slots when ring barriers consume video frames.
The GUI test rejects invalid sprite shapes during race entry.

```sh
export QT_QPA_PLATFORM=offscreen SDL_AUDIODRIVER=dummy
export MGBA_RING_GUI_ROM=/path/to/mksc-ring.gba
export MGBA_RING_GUI_BIOS=/path/to/gba.bin
export MGBA_RING_GUI_INPUTS=/path/to/ring-input-fixtures
export MGBA_RING_GUI_STOCK_INPUTS=/path/to/mgba/tools/link/inputs
ctest --test-dir build -R ring_gui --output-on-failure
```

Ring fixtures are `vs-2.csv`, `battle-2.csv`, `vs-8.csv`, and `battle-8.csv`
produced by `mksc/tools/ring/test_game.py`. Set `MGBA_RING_GUI_SYNC=1` to
exercise audio/video synchronization. Tests copy the ROM and keep save outputs
in a temporary directory. Without private fixtures, gameplay rows explicitly
skip; a default CTest pass is not evidence of a played GUI session.

Physical cable/electrical validation is still unavailable. See the game's
`docs/ring-investigation.md` for full scope and hardware limitations.

## Scripted launch and independent settings

```sh
mgba-qt --ring-players 8 --instance-settings ./ring-settings --hide-menu \
  --bios gba.bin mksc-ring-boot-shared.gba
```

Use `mGBA.exe` instead of `mgba-qt` on Windows. `--ring-players N` accepts
2 through 8 and one GBA ROM filename, which it loads in every window. It selects
the ring topology automatically. Press Start in P1's connection menu to host.
The title of each window ends in `P1`, `P2`, etc., in cable/window order.
The usual BIOS, configuration overrides, scale, patch, and script arguments
also apply to the launched windows.

`--instance-settings DIR` creates `P1/config.ini` and `P1/qt.ini`, through P8,
within DIR. Configure each window's keyboard/gamepad bindings in Settings;
they persist separately and are loaded again on the next launch. Each active
window keeps its own settings directory, including when another window closes
and its directory is reused. SDL preferred-device loading updates that window's
slot without clearing the other seven slots. Explicit command-line BIOS and
configuration overrides apply to all participants, over their saved settings.

`--hide-menu` keeps the windows windowed. Press Alt to reveal the menu bar;
it hides after the menu closes or focus leaves. Ctrl+M toggles its visibility.

The MKSC builder supports `--boot-ring` and `--shared-music`.
See the ROM's `tools/ring/README.md` for the music policies and build commands.

The ring coordinator aligns reset epochs to a shared LCD-frame boundary.
Emulated CPU and serial frequencies remain unchanged. Audio-device failure
starts a timed fallback consumer, so a console cannot stall with a full audio
buffer after opening multiple windows.

`scriptedLaunch` in the Qt fixture exercises eight unpaused windows, explicit
`--bios`, hidden menus/Alt, character selection via Qt key events, a native
physics-rate measurement, independent binding persistence, and settings-slot
reuse. Set `MGBA_RING_GUI_BOOT_ROM` to a direct-boot ROM for that test.
The regular Vs./Battle GUI cases measure 600 video frames after the native
race counter begins advancing; movement is deferred during that measurement
so a completed Battle's frozen counter cannot produce a false speed failure.

For backend startup diagnostics, `MGBA_RING_GUI_DISPLAY=1` requests an OpenGL
window and `MGBA_RING_GUI_HARDWARE=1` requests the hardware GBA renderer.
`MGBA_RING_GUI_THREADED=0|1` controls the native software rendering worker.
`MGBA_RING_GUI_STARTUP_FRAMES=60..5100` makes `scriptedLaunch` run a shorter
eight-window startup/closure check. Hardware checks wait for attachment and
verify the active GBA renderer ID on every console, rather than just compiled
support. `threadedVideoReset` separately exercises repeated software resets
and renderer reloads using the supplied ROM and real BIOS.

For moving, host-clock audio validation, set `MGBA_RING_GUI_AUDIO=1` for
eight SDL outputs or `MGBA_RING_GUI_AUDIO=qt` for the actual Qt Multimedia
outputs. Run `ring:vs-8` with a real audio server; the dummy SDL driver does
not support eight devices. The default measurement lasts 3,600 gameplay
frames. `MGBA_RING_GUI_AUDIO_FRAMES=18000` extends it to five minutes.
These checks fail on missing SDL samples or Qt sink underruns, independently
of the simulation update rate. Run them without concurrent builds or CPU
stress tests so the result measures the emulator itself.
Set `MGBA_RING_AUDIO_TRACE=1` to log queue depths at Qt underruns, source-mutex
waits or interpolation passes exceeding 20 ms, and audio timer gaps over 40 ms.
These diagnostics are disabled by default. Queued PCM at an underrun separates
late sink feeding from an empty emulation FIFO; host scheduling must also be
measured before attributing a long callback to emulator CPU work.

## Instruction profiles

The CLI profiler observes instructions inside the ordinary CPU run-loop quantum,
without adding cooperative link yields. Captures identify this with
`profileScheduling: "quantum"`. Legacy instruction-stepping profiles changed
scheduling and must not be mixed into performance comparisons. The game-side
`test_profile.py` compares complete eight-player snapshots and per-video timing
records with observation enabled and disabled; both are identical. Normal
GUI/CLI execution continues to use the original uninstrumented run loop.

## Visible real-time regression review

The MKSC checkout provides `tools/ring/review_gui.py`. Point `--gui-test` at this
build's `qt/test-qt-ring_gui` (`.exe` on Windows), then supply `--rom`, `--bios`,
`--mode vs|battle|gp`, `--players 2..8`, and optionally `--seconds 35..300`.
It runs the real GUI regression with tiled windows, real-time pacing, audible
Qt outputs, and temporary settings/saves. `--fixture` replays an existing keypad
CSV; `--log` preserves its actual test output and exit status. See the MKSC
`tools/ring/README.md` for complete commands and replay limitations. The normal
CTest matrix is unchanged; the additional 2–8/GP rows are enabled for visual
review only.

### Windows startup diagnostics

The Windows entry point keeps the UTF-8 buffers independently of Qt's writable
argument array. The array always includes `argv[argc] == nullptr`, and the
original CRT `__argv` is restored before the temporary array is destroyed.
`writableArguments` in `qt/test-qt-ring_gui` checks Unicode paths and argument
counts 1–32, including pointer rearrangement and power-of-two allocation
boundaries. These portable ownership checks pass under AddressSanitizer and
UndefinedBehaviorSanitizer on Linux. They do not establish that the reported
Windows `0xc0000374` startup regression is resolved; that exception identifies
heap corruption but supplies no allocation or corruption stack.

The Windows ring launcher now leaves the native `threadedVideo` worker off
when it has not been configured explicitly. The user bisected the heap crash
to `07c2af334491c6d0db84c63ff9f90c6c22d81ef9` (repaired-history ID
`4db58d156`), which enabled this worker automatically, and verified that the
same launch succeeds with `-C threadedVideo=0`. Other ring scheduling and audio
improvements remain enabled. Other platforms retain automatic rendering workers.
An explicit `-C threadedVideo=1` remains available for isolating the Windows
worker defect. The worker's underlying Windows corruption and the performance
of the synchronous fallback have not yet been validated as resolved.

A sanitizer run with hardware rendering reproduced a separate unsafe handoff:
rebuilding the active video proxy changed its backend while the old native
worker was still reading queued packets, reaching the GL renderer before its
memory was initialized. Renderer changes and resets now drain and stop the
old consumer before reusing that proxy. Eight hardware-rendered windows and
repeated software resets/reloads pass ASan/UBSan on Linux. Native Windows
validation is still required before re-enabling its worker default.
