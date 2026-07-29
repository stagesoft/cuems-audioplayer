# cuems-audioplayer

Part of the **CUEMS** ecosystem — see the [`cuems-RELATIONS`](https://github.com/stagesoft/cuems-RELATIONS) repo for the system index, architecture diagram, and protocol/port map.

## Role

Per-cue audio player: **spawned by NodeEngine** (not a systemd service — `settings.xml` gives its exec path; node-engine respawns it), plays back media via JACK, syncs to MTC. C++17. Uses RtAudio + RtMidi and oscpack for OSC; supports WAV/MP3/AAC/FLAC/OGG and extracts audio from video files (MP4/AVI/MKV/MOV) via the **cuems-mediadecoder** submodule (FFmpeg). Resampling via libsoxr.

Depends on JACK + **cuems-jack-volume** (the `0_mixer` OSC volume client — see the `jack-volume` repo). Vendored git submodules include `mtcreceiver`, `oscreceiver`, `cuemslogger`, and `cuems-mediadecoder`.

## Build

```bash
git submodule update --init --recursive
mkdir -p build && cd build && cmake .. && make -j$(nproc)
```

Dependencies: `librtmidi-dev` (3.0.0), `librtaudio-dev` (5.0.0), `liboscpack-dev` (1.1.0), FFmpeg libs (via cuems-mediadecoder), `libsoxr-dev`. Deploy binaries with **stop the engine → cp → start** (the player is engine-spawned; swapping while it runs gives `Text file busy`).

## MTC sync

Reads MTC via its `mtcreceiver` submodule from ALSA `Midi Through Port-0`. After the mtcreceiver `rc_1` `aa44894` resync-hold fix, `mtcHead` is a continuous QF timebase, so audioplayer (raw `mtcHead` read, 2-frame tolerance) needs only the submodule bump — no code change. See the mtcreceiver CLAUDE.md for the 2s-skip root cause.

## Field notes / gotchas

- **`debian/bookworm` is a DIVERGENT full branch** (was 19 ahead / 17 behind `master`, pins an old mtcreceiver, changelog behind the installed version). It must be reconciled with `master` before a clean `.deb` cut. On hosts running a hand-swapped binary, the package is `apt-mark hold`'d to protect it. Open follow-up: audioplayer deb 0.0.3-8 reconciliation.
- Player subprocess stdout lands in the node-engine "Subprocess output" journal, not a dedicated unit log.
