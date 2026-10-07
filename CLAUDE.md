<!--
SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
SPDX-License-Identifier: GPL-3.0-or-later
SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
-->

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

Dependencies: `librtmidi-dev` (3.0.0), `librtaudio-dev` (**≥ 5.1** — needed for `RTAUDIO_JACK_DONT_CONNECT`; 5.2.0 on casas and the deployed nodes), `liboscpack-dev` (1.1.0), FFmpeg libs (via cuems-mediadecoder), `libsoxr-dev`. Deploy binaries with **stop the engine → cp → start** (the player is engine-spawned; swapping while it runs gives `Text file busy`).

## MTC sync

Reads MTC via its `mtcreceiver` submodule from ALSA `Midi Through Port-0`. After the mtcreceiver `rc_1` `aa44894` resync-hold fix, `mtcHead` is a continuous QF timebase, so audioplayer (raw `mtcHead` read, 2-frame tolerance) needs only the submodule bump — no code change. See the mtcreceiver CLAUDE.md for the 2s-skip root cause.

## Cue boundary / silence contract

The MTC-correction path in `audioCallback` (`src/audioplayer.cpp`) computes
`seekPosition = mtcHead + headOffset` and branches three ways:

- `seekPosition < 0` (**before file start** — a future-anchored cue whose
  `start_mtc` is still ahead of live MTC) → **holds silence** and keeps waiting;
  it does *not* set `endOfStream`/`outOfFile`, so the per-buffer silence-fill
  branch runs and the cue is not terminated. When MTC crosses the start it
  seeks to ~0 and plays. (Before the `869dyufeh` fix this case shared the
  past-end branch, wrongly logged `"Out of file boundaries!"` and killed the
  cue — that is why the engine historically had to defer `/mtcfollow` to reveal.)
- `0 ≤ seekPosition ≤ fileSize` → seek + play.
- `seekPosition > fileSize` (**past end**) → genuine end of stream
  (`"Out of file boundaries!"`, `endOfStream`/`outOfFile`).

This makes the player **self-gating**: it can follow MTC early and auto-start at
`start_mtc`, so the engine's per-cue reveal OSC becomes optional (kept as
belt-and-suspenders until the fleet runs the fixed binary).

## Field notes / gotchas

- **`debian/bookworm` is a DIVERGENT full branch** (was 19 ahead / 17 behind `master`, pins an old mtcreceiver, changelog behind the installed version). It must be reconciled with `master` before a clean `.deb` cut. On hosts running a hand-swapped binary, the package is `apt-mark hold`'d to protect it. Open follow-up: audioplayer deb 0.0.3-8 reconciliation.
- Player subprocess stdout lands in the node-engine "Subprocess output" journal, not a dedicated unit log.

**No JACK auto-connect (869fcvz85).** The stream is opened with `RTAUDIO_JACK_DONT_CONNECT` (`audioplayer.cpp`, `streamOps.flags`), so RtAudio never connects the outports to `system:playback_*` itself. The engine always wires the player to the mixer; an auto-connect landing after that wiring used to stay and double the cue (through the mixer and straight to the outputs). Consequence: a player started by hand, outside the engine, is silent until something connects it (`jack_connect`).
