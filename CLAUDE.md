<!-- SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- SPDX-FileContributor: Ion Reguera <ion@stagelab.coop> -->

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
  (`"Out of file boundaries! seek=… ms, end=… ms"`, logged **once per entry**,
  `endOfStream`/`outOfFile`).

`fileSize` (`AudioFstream::getFileSize()`) is in bytes of what the stream
**delivers**: 32-bit float frames of the **output** channel count. The stream
always has 2 ports, so a mono file is reopened as stereo; before the
1amfwm7e4wv hotfix `fileSize` counted the file's own channels, a mono file at
the JACK rate reported half its length, and the first MTC re-sync past the
middle silenced the cue (lleida-diposit, 2026-10).

This makes the player **self-gating**: it can follow MTC early and auto-start at
`start_mtc`, so the engine's per-cue reveal OSC becomes optional (kept as
belt-and-suspenders until the fleet runs the fixed binary).

**Looping audio with video: the guarantee (Ion, 2026-10-19).** Audio and video stay in
sync across the passes of a loop **only if (A) their durations are exactly the same, or
(B) they are in the same container** (one file; this player extracts the audio). Two
separate files of different lengths drift apart by the difference on every pass: an
operator error, not a bug. Audio extracted from a video to a separate file is longer by
the codec's padding (0–106 ms on `test_video_files/`), so that pair is never case A. The
gapless, MTC-locked loop with one period per file is rev 3 of 869ejxng3 + 1amfwm7e6cw
(`cuems-RELATIONS/Plans/2026-10-19-audio-loop-varispeed-869ejxng3-1amfwm7e6cw.md`).

## Field notes / gotchas

- **A re-sync is an 80 ms jump.** The player follows MTC within 2 frames
  (`MTC_FRAMES_TOLERANCE`); beyond that it seeks to the MTC position, skipping
  (audio slow) or repeating (audio fast) ~80 ms. How often depends on the
  audio clock against the MTC clock (`CLOCK_MONOTONIC` in libmtcmaster).
  Measured 2026-10-19 from ALSA `hw_ptr`/`tstamp`: Behringer UMC204HD
  **−240 ppm** → a jump every ~5.5 min; onboard HDA (test2 node01) −31 ppm →
  every ~42 min. Anything that misbehaves "only past some point of a file" is
  exposed once the file is longer than that interval. Smoothing it is ClickUp
  1amfwm7e6cw.

- **Releases are cut from `debian/bookworm`, which differs from `rc_1` in `src/`**: it carries `f35d539` (journal log priorities, stderr duplicates dropped) and other submodule pins. A fix lands on `rc_1` through a `fix/<slug>` PR and reaches the release through a `debian/<slug>` branch that merges it into `debian/bookworm` plus the changelog entry (869fcvzag PRs #2/#3, 1amfwm7e1pq). Expect a conflict wherever a fix touches a line next to a removed `std::cerr`. In a linked worktree `debian/rules` does not initialise the submodules (`.git` is a file there): run `git submodule update --init --recursive` first. On hosts running a hand-swapped binary, the package is `apt-mark hold`'d to protect it: check with `dpkg -V cuems-audioplayer`.
- Player subprocess stdout lands in the node-engine "Subprocess output" journal, not a dedicated unit log.

**No JACK auto-connect (869fcvz85).** The stream is opened with `RTAUDIO_JACK_DONT_CONNECT` (`audioplayer.cpp`, `streamOps.flags`), so RtAudio never connects the outports to `system:playback_*` itself. The engine always wires the player to the mixer; an auto-connect landing after that wiring used to stay and double the cue (through the mixer and straight to the outputs). Consequence: a player started by hand, outside the engine, is silent until something connects it (`jack_connect`).
