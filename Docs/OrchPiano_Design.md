# OrchPiano — Design & Build Plan

Companion to [`OrchPiano_ReductionRules.md`](OrchPiano_ReductionRules.md), which holds the *rule*
detail (drop order, importance score, LIL table, re-voicing, pedal model, literature). This file is
the **build plan**: architecture, file layout, phased milestones, parameter table, deviations log.
Mirrors `OrchHarp/Docs/OrchHarp_Design.md`.

Repo: `C:\AudioDev\Repos\OrchPiano`, plugin code `Ocpn`, VST3, `IS_MIDI_EFFECT`. Built with
VS 2022 / JUCE at `C:/JUCE/JUCE`:

```
cmake -S . -B build
cmake --build build --config Release --target OrchPiano_VST3
cmake --build build --config Release --target OrchPianoReductionLogicCheck
```

`COPY_PLUGIN_AFTER_BUILD FALSE` — copy the VST3 by hand from
`build/OrchPiano_artefacts/Release/VST3/` with Bitwig closed (host holds the `.vst3`).

---

## 1. Architecture

```
                 ┌───────────────────────────────────────────────┐
   MIDI in  ───▶ │  OrchPianoAudioProcessor                       │ ───▶ MIDI out
 (Direct, or     │                                               │      ch 1-4 notation
  OrchCapture    │  ┌─────────────┐      ┌──────────────────┐     │      ch 5-8 performance
  merged tap)    │  │ streaming   │  or  │ planning engine  │     │        (optional)
                 │  │ engine      │      │ (lookahead buf + │     │
                 │  │ (greedy,    │      │  multi-pass)     │     │
                 │  │  live)      │      └──────────────────┘     │
                 │  └─────────────┘             │                 │
                 │         └──────────┬─────────┘                 │
                 │             OrchPianoReductionLogic (ocpn::)   │  ← pure, tested
                 │             + decision-log sidecar writer      │
                 └───────────────────────────────────────────────┘
```

- **`Source/OrchPianoReductionLogic.{h,cpp}`** — pure `ocpn::` namespace. No JUCE, no MIDI buffer,
  no randomness except a seeded PRNG passed in. Everything the check tool exercises: LIL table,
  voice selection, hand assignment, octave fold, importance score, close-position re-voicing,
  contour re-quantise, KDE split point. **Same discipline as `ohrp::`** — deterministic, so two
  instances agree without IPC.
- **`Source/OrchPianoProcessor.{h,cpp}`** — APVTS params, MIDI I/O, note tracking (ONF
  `TrackedNote` vector pattern), the two engines, transport-edge handling, the decision-log
  `juce::Thread` writer (OrchHarp `MarkerWriter` pattern), OrchCapture IPC subscription (Phase 5).
- **`Source/OrchPianoEditor.{h,cpp}`** — `TabbedComponent` + `LayoutPanel` (layout delegated to
  the editor via a lambda), the melody/bass readout component, `juce::Timer` status refresh.
- **`Tools/OrchPianoReductionLogicCheck.cpp`** — console check, `[PASS]/[FAIL]` counter, returns
  non-zero on any failure. Grows every phase.

### 1.1 The two engines

| | Streaming (`lookaheadBeats = 0`) | Planning (`lookaheadBeats > 0`) |
|---|---|---|
| Path | per-onset-group, in `processBlock` like OrchHarp's `flushVoiceGroup` | events buffered into a `ppq`-tagged ring; a plan is (re)computed for `[now, now+LA]`; the segment `[now-LA, now]` that is fully seen is committed and emitted |
| Emission timing | immediate | delayed by a **constant** `lookaheadBeats` of musical time |
| Determinism | yes | yes (seeded) |

The planning engine's emit is just the streaming path fed from the buffer with the plan's
decisions already attached to each note — so `flushVoiceGroup` / `emitResolved` are shared.

---

## 2. File / CMake layout

```
OrchPiano/
  .gitignore                 (copy of OrchHarp's)
  CMakeLists.txt
  Docs/
    OrchPiano_Design.md          (this file)
    OrchPiano_ReductionRules.md   (the rules + literature)
    OrchPiano_UsageNotes.md       (Phase 6 — you-at-the-UI voice, like OrchHarp's)
  Source/
    OrchPianoReductionLogic.h/.cpp
    OrchPianoProcessor.h/.cpp
    OrchPianoEditor.h/.cpp
  Tools/
    OrchPianoReductionLogicCheck.cpp
```

CMake mirrors OrchHarp exactly: `juce_add_plugin(OrchPiano … PLUGIN_CODE Ocpn …
IS_MIDI_EFFECT TRUE COPY_PLUGIN_AFTER_BUILD FALSE)` + a `juce_add_console_app`
(`OrchPianoReductionLogicCheck`) linking `juce_core` + the logic `.cpp`.

---

## 3. Phased build

Each phase ends green on the check tool and (from Phase 2) loadable in Bitwig.

| Phase | Scope | Engine | Check-tool additions |
|---|---|---|---|
| **1 — mechanical core** ✅ `0dec153` | `ocpn::` LIL table + `intervalIsMuddy`; `octaveFoldInto`; `selectVoices` (ported from `ohrp::`); `assignHands` (split + `crossoverSlack` hysteresis). | — | 37 assertions |
| **2 — plugin skeleton** ✅ `0dec153` (Bitwig-confirmed 2026-09-03) | Processor + Editor load as VST3, streaming onset-group hand-split reducer, 2 output channels, `dampSuccessive`, transport-edge safety. | streaming | — |
| **3 — roles + importance drop** ✅ `0d89a5c` | `ocpn::melodyIndex`/`bassIndex`/`tagRoles`/`importanceScores`/`handDifficulty`/`reduceHand`; drop doublings → over-budget → over-span → over-ceiling, melody/bass never dropped; `difficultyCeiling`, `keepBass/MelodyOctaves`, `w*` weight params; **4-voice output** (RH up/down, LH up/down on `outChannelBase..+3`); **decision-log sidecar** (`%TEMP%/orchpiano-decisions-<tag>.log`, off-thread `LogWriter`). Repair mode: no poly cap. | streaming | +15 assertions (52 total) |
| **4 — re-voicing + idiom** | `ocpn::revoiceClose` (§8.2) + `revoice` 3-state; `octaveMovePassages`; figuration substitution (`repeatedNoteTremolo`, `arpeggioRespace`, `stringResustain`); ornament recognition; dynamic-contour reconstruction. | streaming | close-position rules, LIL-driven re-stack, tremolo/arp spelling |
| **5 — planning engine** | lookahead ring buffer; the §1.1 pipeline (part-track → classify → harmonic rhythm → phrase seg → per-phrase hand split via KDE → drop/re-voice → voice-lead cleanup); latency-compensation report; `inputSource` (Direct / OrchCapture merged) + coordinator IPC subscription. **OrchCapture-side changes** (merged-tap emit, time-offset compensation, feedback guard) land here in parallel. | planning | KDE split point, part-tracking cost, phrase segmentation, plan determinism |
| **6 — polish** | `OrchPiano_UsageNotes.md`; editor tabs (Mode / Voicing / Reduction / Pedal); melody/bass override UI; validation corpus run against the Beethoven-symphony reduction MIDIs. | both | — |

Transform-mode (rig) features (Center/Span travel, contour, field-CC read) port from OrchHarp
**after** Phase 5 — Reduce/Repair are the priority.

---

## 4. Parameter table (target — declaration order = Bitwig remote-page order, 8/page)

Page 1 (the essentials):

| # | id | name | type | default |
|---|---|---|---|---|
| 1 | `operatingMode` | Mode | choice Repair/Reduce/Transform | Reduce |
| 2 | `hands` | Hands | choice Both/Left/Right | Both |
| 3 | `maxVoices` | Max Voices | choice 4/6 | 4 |
| 4 | `splitNote` | Hand Split Note | int 0..127 | 60 |
| 5 | `maxNotesPerHand` | Notes / Hand | int 2..6 | 4 |
| 6 | `maxSpan` | Max Hand Span (st) | int 8..16 | 14 |
| 7 | `lookaheadBeats` | Lookahead (beats) | int 0..16 | 8 |
| 8 | `revoice` | Re-voice | choice Off/Framework/Close | Framework |

Page 2 (reduction):

| # | id | name | type | default |
|---|---|---|---|---|
| 9 | `difficultyCeiling` | Difficulty Ceiling (0 = off) | float 0..1 | 0 |
| 10 | `lowIntervalStrictness` | Low-Interval Strictness | choice Off/Loose/Strict | Loose |
| 11 | `maxRingBeats` | Max Ring (beats) | int 1..8 | 4 |
| 12 | `keepBassOctaves` | Keep Bass Octaves | choice Off/Keep/Add | Keep |
| 13 | `keepMelodyOctaves` | Keep Melody Octaves | bool | true |
| 14 | `dynamicContour` | Dynamic Contour | choice Off/Preserve/Preserve+Mark | Preserve |
| 15 | `crossoverSlack` | Crossover Slack (st) | int 0..12 | 5 |
| 16 | `voiceLeading` | Voice-Leading Cleanup | choice Off/Safe/Full | Safe |

Page 3 (pedal + I/O):

| # | id | name | type | default |
|---|---|---|---|---|
| 17 | `pedalMode` | Pedal | choice Off/Manual/Follow Harmony/Follow Bass | Off |
| 18 | `performanceTap` | Performance Tap (ch 5-8) | bool | false |
| 19 | `dampSuccessive` | Damp On Next Attack | bool | true |
| 20 | `inputSource` | Input Source | choice Direct/OrchCapture Merged | Direct |
| 21 | `melodyChannels` | Melody Channel(s) mask | int (bitmask 0 = auto) | 0 |
| 22 | `bassChannels` | Bass Channel(s) mask | int (bitmask 0 = auto) | 0 |
| 23 | `planSeed` | Plan Seed | int 0..9999 | 0 |
| 24 | `decisionLog` | Write Decision Log | bool | true |

Advanced (page 4+, importance weights): `w_top w_bottom w_rhythm w_motion w_charTone w_velocity
w_double w_static w_future` — floats 0..2, sane defaults per `OrchPiano_ReductionRules.md` §7.1.
Idiom toggles: `repeatedNoteTremolo arpeggioRespace stringResustain ornamentRecognition
octaveMovePassages glissAsRun tremoloStyle`.

Field-CC (Transform, port from OrchHarp): `fieldCc` 110, `fieldChannel` 1.

Not params (state): decision-log tag, OrchCapture lane names.

---

## 5. Deviations log

*(kept like OrchHarp §12 — every place the build departs from the spec, and why. Empty at Phase 1
start.)*

- **P1:** `selectVoices` ported near-verbatim from `ohrp::` — the register-split / poly-cap /
  span-clamp / protect logic is identical to what a piano hand needs. Piano-specific additions
  (LIL, hand assignment, octave fold) are new `ocpn::` functions alongside it, not edits to it.
- **P2:** 2 output channels (one per hand), not 4 — superseded in P3.
- **P3:** `selectVoices` is **not used by the processor** any more — `reduceHand` (importance-driven)
  replaces it in `flushGroup`. `selectVoices` stays in `ocpn::` (tested) for the Transform path / as
  a reference; revisit whether to keep it after P5.
- **P3:** melody = **registral top** only (plus a loud-note override) — the full §4.2 salience model
  (contour continuity, isolated-low-line bass-vs-melody, tempo-weighted note length) needs the
  lookahead window → P5. `melodyChannels`/`bassChannels` source hints not wired yet (P5, with
  `inputSource`).
- **P3:** voice-within-hand split is **positional** (RH: highest kept → up-stem, rest → down-stem;
  LH: lowest → down-stem, rest → up-stem), not real per-line streaming → P5.
- **P3:** `w_rhythm` / `w_static` / `w_future` from §7.1 are **not** in the streaming importance
  score (need durations / the window) → P5. `charTone` uses interval-class {1,2,6,10,11} above the
  bass as a cheap proxy (slightly over-rewards seconds).
- **P3:** decision-log sidecar is a plain `.log` (distinct name/format from OrchHarp's `bar:label`
  marker sidecar) so OrchCapture does **not** ingest it as markers — it is for the user to read.

---

## 6. Cross-repo dependencies

OrchCapture (`project_orchcapture_scope`), all landing at Phase 5:
1. Coordinator **live merged-tap emit** — expose the merged note stream to a subscriber.
2. **Per-lane constant time-offset compensation** — subtract OrchPiano's reported `lookaheadBeats`
   delay from its capture lane on export.
3. **Feedback guard** — exclude OrchPiano's own output lane from the merged stream it feeds back.

No MC change needed (Transform-mode field read reuses MC's existing 2-CC broadcast, as OrchHarp
Phase 2d does).
