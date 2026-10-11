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
that interval according to native serial timing. Audio/video consumer waits
occur outside the cable mutex so GUI pause/reset/close can interrupt peers.

`test-qt-ring_gui` uses real Qt windows and the File-menu dialog, then supplies
keypad inputs to each native core. Private fixtures enable Vs./Battle entry for
two/eight ring consoles and four standard consoles, with BIOS presence, native
human racer IDs, protocol errors, group pause, reset, middle-window removal,
late boot of a second console, and close/reopen checks. Complete-round and synchronized-physics validation
remain covered by the headless harness.

BIOS-backed GUI checks passed for both ring modes with two/eight windows and
both standard modes with four windows. Synchronized two-window playback and
staggered boot also passed. The default Qt build has 34 passing CTest targets;
its fixture-free GUI rows skip explicitly. Concurrent eight-window stress
runs on the shared cloud VM were below real time; these checks establish
functional behavior, not desktop throughput.

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
