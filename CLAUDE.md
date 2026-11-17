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

**Engine start times are converted into this process's MTC day (24 h wrap, 1amfwm7fbqy).** A per-cue player
spawned after the engine passed 24 h used to put the start a day ahead and hold digital silence while logging
`MTC -> Play started` (lleida-diposit 2026-10-24). On this line both wires carry the start: `/start_tod` (the
engine's start as MTC position modulo 24 h, int; replaces `/offset_i`, normalised into [0, 86 400 000) with a WARNING)
and the legacy float `/offset` (k = 0 keeps `floor(f)` ms exactly). The OSC thread latches the raw value with its wire
(`dayalign::Wire`); `PlayerCore::resolvePendingOffset()` (`src/playercore.cpp`) resolves it in the callback with
`MtcReceiver::toLocalDay` (mtcreceiver `eb7ad4f`, nearest day, 12 h rule) and keeps it **pending while the head is not
authoritative** (the cue holds silence, a running pass keeps its offset). Pure helpers in `src/dayalign.h`, tests in
`test/test_dayalign.cpp`. Journal: `MTC day-align: on (12 h rule, tod)` at start, `MTC day realign k=…` once per k
(INFO; WARNING for k < 0 on `/offset`), `offset unresolved` after 2 s, implausible position. The old player line had
its own implementation of the same rule for `/offset` (`DayAlign::OffsetMailbox`, `695aa28`/`1f51e44`, rc_1 PR #8);
the 869ejxng3 merge of rc_1 keeps this line's code and drops that one (same contract, different callback).

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

**Self-gated start, CAPS `selfgate` (869feynrj, `cuems-RELATIONS/Plans/2026-11-03-audio-first-pass-head-869feynrj.md`
§4.1).** The engine follows early and the player starts itself, at the sample:
- **Padded start**: the first start since a `/mtcfollow 1` from HOLD, soxr pristine, when sample 0 falls inside the
  period (`−N·r < target < 0`): K = round(−target / r) zero frames, then soxr's output from sample 0
  (`AudioFstream::startPadded`; `process()` counts `owed`/`sum(rN)` over N − K). Clean, no fade. Without this the start
  waited for the next period and began at file position target ≥ 0, losing up to a period of head.
- **Snap** (869feynrj step 5): the target is re-derived every period from the *latest* MTC quarter frame, so its
  reception jitter can step a first start straight over the padded window (node01: +2 samples idle, up to 57 under
  load). A first start with `0 < target ≤ 2 ms` of file samples (`SNAP_MAX_MS`) starts clean at sample 0 instead
  (`AudioFstream::startAtZero`): head kept, `target` samples late, the lock loop pulls the offset in. Larger targets
  and every non-first start keep today's start from the target. The root cause (no filter on the tick jitter) is
  ClickUp 1amfwm7h5qk.
- **Equal start**: a `/start_tod` (or `/offset`) equal to the running one while PLAY/SEEKING is a no-op (`START_EQUAL`
  debug line, `startsEqual()`): no reset job, no second glitch on a re-GO.
- **`/cancelstart i:seq [i:fade_ms = 5]`**: the engine's cancel is final. HOLD/SEEKING: following off, silent. PLAY:
  fade over `fade_ms`, then HOLD and a RESET (soxr pristine again, so a re-follow pads again). PASTEND: following off
  (a running drain fades). No E10/E11/E22 during the fade. Acked on stdout `[OK] CANCEL <seq> <hold|play|pastend> <ms
  of output>` plus a journal line; a second cancel during a fade is acked `play` and changes nothing.
- **`[OK] START <start_ms_mod_day> <target> <file_rate>[ snap]`** on stdout at the first start since each follow
  (target in signed file samples: ≤ 0 on time, > 0 = that many samples late: head lost, or head kept when the trailing
  `snap` field is there), plus a journal line. Engines parse ≥ 3 fields, so the 4th is safe for older ones.
- **`[OK] LATENCY <frames> <rate> <period>[ override]`** written next to CAPS, before RUNNING (and again on
  `setOutputLatencyMs`): the latency the start target compensates.
Tests: `test/test_selfgate.cpp` (exact fake clock: the target is placed at any sample of a period).

**Looping audio with video: the guarantee (Ion, 2026-10-19).** Audio and video stay in
sync across the passes of a loop **only if (A) their durations are exactly the same, or
(B) they are in the same container** (one file; this player extracts the audio). Two
separate files of different lengths drift apart by the difference on every pass: an
operator error, not a bug. Audio extracted from a video into a separate file keeps that
video's audio-stream length (the VideoCue's period is its container's audio length), so that
pair stays together; a video-only file next to an audio file of another length does not. The
gapless, MTC-locked loop with one period per file is rev 3 of 869ejxng3 + 1amfwm7e6cw
(`cuems-RELATIONS/Plans/2026-10-19-audio-loop-varispeed-869ejxng3-1amfwm7e6cw.md`).

**MKV is unsupported for AudioCues in the 869ejxng3 release (Ion, 2026-10-28)** — until the
decode thread lands (ClickUp 1amfwm7fe42). The audio callback still demuxes and decodes in
place; in a matroska file the audio packets sit between large video packets, so a callback
occasionally reads past megabytes of video. Measured on test2 node01 (step 10, 30 Mb/s h264 +
AAC mkv, 512-frame periods): one callback of 21.3 ms in 10 min (twice the 10.7 ms period →
a jackd xrun) and 3.2 ms worst with a cold page cache (30 % of the period, over the 25 %
budget). A cue on an mkv still plays and loops, but may click. Use mp4/mov/wav, or extract
the audio. VideoCues on mkv are not affected (the videocomposer decodes in its own threads).
JACK periods below 512 frames are unsupported for the same reason (same task).

## Field notes / gotchas

- **Compressed audio aborted at arm until 0.0.3-13 (2026-10-22).** cuems-mediadecoder's
  `createSwrContextExplicit()` copied into an uninitialised `AVChannelLayout`; `av_channel_layout_copy()`
  uninits its destination, so stack garbage reading as a CUSTOM layout was `free()`d. Every release since at
  least 0.0.3-9 aborted on AudioCues whose decoder reports a known channel layout (MP3, Vorbis, Opus, FLAC,
  ALAC, AAC, PCM in MOV, audio in MP4/MKV); PCM in WAV/AVI/MKV (UNSPEC) was immune, and whether a given binary
  hit it depended on stack contents (0.0.3-12: MP3/Vorbis/Opus 50/50, AAC survived). Fixed by the pin
  `8f5c3c6`. Guarded by `MediaDecoderLayout.ResamplerSetupSurvivesAGarbageStack` (embedded fixtures,
  `test/fixtures/gen.sh`).
- **Run the suite and the 50-arm matrix on .111 before any release or mediadecoder pin bump.** There is no
  CI, and `debian/rules` builds with tests off, so nothing runs the regression test unless you do:
  `audioplayer_tests` plus `cuems-RELATIONS/baselines/hotfix-0.0.3-13-chlayout/t77_matrix.sh`.
- **`.gitmodules` says `branch = ffmpeg-4.0` for cuems-mediadecoder** (`740e733`, without the fix):
  `git submodule update --remote` would silently move the pin there. Bump pins explicitly.

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
