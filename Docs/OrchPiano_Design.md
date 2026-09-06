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
 (Direct - a     │                                               │      ch 1-4 notation
  track or a     │  ┌─────────────┐      ┌──────────────────┐     │      ch 5-8 performance
  Bitwig merge   │  │ streaming   │  or  │ planning engine  │     │        (optional)
  bus, §6.2)     │  │ engine      │      │ (lookahead buf + │     │      + delay-comp CC
                 │  │ (greedy,    │      │  multi-pass)     │     │        → OrchCapture
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
  `juce::Thread` writer (OrchHarp `MarkerWriter` pattern), the delay-compensation CC broadcast to
  OrchCapture (§6.1 — no IPC, a plain CC).
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
| **3 — roles + importance drop** ✅ `0d89a5c`; cross-hand role fix 2026-09-07 | `ocpn::melodyIndex`/`bassIndex`/`tagRoles`/`importanceScores`/`handDifficulty`/`reduceHand`; drop doublings → over-budget → over-span → over-ceiling, melody/bass never dropped; `difficultyCeiling`, `keepBass/MelodyOctaves`, `w*` weight params; **4-voice output** (RH up/down, LH up/down on `outChannelBase..+3`); **decision-log sidecar** (`%TEMP%/orchpiano-decisions-<tag>.log`, off-thread `LogWriter`). Repair mode: no poly cap.

**2026-09-07, live-found real bug: a note can be flagged "redundant" because of a pitch in the OTHER hand entirely.** User asked why the opening E-major wind chord (kept together in one hand after the `kdeHandSplit` fix above) still lost 2 of its 4 notes, ending up as a bare octave instead of a real chord. Traced precisely: `reduceGroup` computes `melodyIndex`/`bassIndex`/`tagRoles`/`importanceScores` ONCE on the WHOLE onset group (both hands' notes together, since the flute's own onset lands at the identical tick as the chord's), THEN just slices the resulting role/importance arrays per hand by index - never re-deriving them from each hand's own note list after the split. For this passage: the group-wide `melodyIndex` correctly picks the flute (the overall highest note) as melody, but `tagRoles`' doubling pass ALSO ran across the full group and flagged the chord's own 5th (B2) as `Role::Doubling` purely because it shares a pitch CLASS with the melody note (B, an octave-plus away, about to leave for the other hand entirely) - a coincidence with zero musical relevance to this hand's own chord. That 5th had no `keepBassOctaves`-style protection (unlike the chord's genuine bass-octave duplicate, which does), so it was dropped outright in `reduceHand`'s doubling pass; the chord's 3rd was then also cut by the span-clamp once the 5th's removal left it as the only unprotected span extreme - collapsing a real chord to a bare octave. **Fixed**: `melodyIndex`/`bassIndex` still run once on the whole group (correctly identifying which hand, if either, holds the true overall melody/bass by pitch), but `tagRoles`/`importanceScores` are now re-run FRESH on each hand's own note list after the split, locating the group's melody/bass PITCH within that hand's own subset (if present) rather than reusing role/importance values computed with the other hand's notes still in view. A genuinely cross-hand doubling coincidence can no longer penalise a note for redundancy within a hand it doesn't even share; the existing "protect this hand's own top/bottom as a local lead/anchor when the true melody/bass isn't here" fallback is unaffected (still applies on top of the freshly-tagged roles). Confirmed against the actual decision log line-by-line by hand-tracing both `ocpn::tagRoles` and `ocpn::reduceHand` exactly, not just inferred from symptoms - the fix reproduces the reference MuseScore reduction's own voicing for this passage (root + 5th + octave, the 3rd correctly omitted the same way skilled reductions often voice a low, wide chord for clarity) without touching `keepBassOctaves`/span-clamp priority at all. No new pure-logic test (the bug was in the processor's *usage* of `ocpn::tagRoles`, not in the function itself, which behaves exactly as designed given whatever list it's handed - matches this session's pattern for processor-only bugs with no unit-test harness for that layer). Rebuilt clean, VST3 reinstalled (Bitwig confirmed closed). Not yet live-tested. | streaming | +15 assertions (52 total) |
| **4 — re-voicing + dynamics** ✅ `4bd749d` | `ocpn::revoiceFramework` (fold muddy inner notes up an 8ve) + `revoiceClose` (§8.2 close-position re-stack) + `revoice` 3-state; `intervalIsMuddy` wired in via `lowIntervalStrictness`; `dynamicRecoveryScale` + `dynamicContour` (thinned chord keeps its energy). Per hand, on ≥3-note groups. | streaming | revoice frame/close, LIL fixing, dynamic recovery |
| **5a — lookahead engine + adaptive split** ✅ `1041084`; `kdeHandSplit` rewrite 2026-09-07 | `lookaheadBeats` param (0 = streaming as before). `planBuf` (ppq-tagged event ring); every input event emitted a constant `lookaheadBeats` of musical time late via `flushPlanBuffer` (onset-group assembly from the buffer, `reduceGroup` shared with the streaming path, `emitSampleFor` maps ppq→sample). `setLatencySamples` reports the delay. Transport-stop drain (`drainAll`), backwards-jump (loop/relocate) buffer purge. `ocpn::kdeHandSplit` — smoothed pitch-density valley over the lookahead window, ±9 st of the `splitNote` prior → per-window adaptive hand split (the §14.5 fix). Editor status shows live split + buffer depth.

**2026-09-07, live-found real bug: the fixed ±9 search window can be completely blind to the true split.** User asked why OrchPiano's Reduce mode collapsed the opening of Grieg's "Morning Mood" - a sustained E-major wind chord (E2/B2/E3/G#3, an octave-plus stack, Bassoon+Clarinets) under a Flute melody starting an octave above it - down to a single bass note, when it's clearly meant to be a genuine LH chord (confirmed directly against the MuseScore reference: the actual Grieg reduction holds this passage as a real 3-note block chord). Traced via the real decision log (`drop E3/B2/G#3 ... hand R`) to `assignHands` putting 3 of the chord's 4 notes in the RIGHT hand, where they lose an over-span fight against the real melody (a 24-semitone span). Root-caused by re-implementing `kdeHandSplit` exactly in Python and running it on this passage's real pitch data: the true gap between the chord (tops out at 68) and the melody (starts at 76) sits 8-16 semitones from the default `splitNote` prior (60) - **entirely outside the old fixed ±9 search window (51-69)**, which was structurally incapable of ever seeing it. Left blind to the real gap, the density search could only find a shallow, spurious local dip INSIDE the chord's own narrow spread (pitch 56, between the bass E2 and the B2/E3/G#3 cluster above it) - splitting a single chord across both hands instead of correctly keeping it together in one. Confirmed this wasn't a one-off: the pattern recurs at every chord re-attack through bar 4 in the same real capture. **Widening the fixed window was considered and rejected** - it just moves the same failure mode to a different (still-arbitrary) distance; a wider-still separation in a different piece would fail identically. **Rewrote the algorithm entirely** (user's explicit call, asked directly rather than assumed): instead of a Gaussian-smoothed density valley bounded to a fixed window, `kdeHandSplit` now finds the **largest real gap between actually-played pitches** anywhere in the phrase's full observed range, requiring the gap to be at least a minor 3rd (`kMinGapSemis`=4 - narrower reads as spacing within one texture, not two hands' material) with a genuine cluster of at least 2 notes on each side (`kMinNotesPerSide`=2 - guards against one stray outlier note faking a "cluster" and swinging the split to an extreme, e.g. a lone note far above everything else). No behavior-relevant distance-from-prior cap remains - the gap-width + real-content-on-both-sides requirement is the quality control, not proximity to a default. `maxDriftSemis` parameter removed entirely (was the source of the bug, not a fix for it) - call site simplified to `kdeHandSplit(phrasePitches, priorSplit)`. 6 pure-logic assertions for this function now (was 4, net +2): all 3 pre-existing cases re-verified to still pass under the new algorithm (with one test's comment corrected - "no qualifying gap" replaces the old "constrained to drift" framing, since the mechanism achieving that same numeric outcome changed), a new outlier-guard case, and a dedicated regression using the exact real Grieg pitch data confirming the split now lands at 72 - squarely between the chord (68) and the melody (76), keeping the whole E-major chord together in one hand. 106/106 total pure-logic assertions green. Rebuilt clean, VST3 reinstalled (Bitwig confirmed closed). Not yet live-tested. | planning | +6 (kdeHandSplit) |
| **5b-1 — phrase-stable split + rhythm/pad importance** ✅ `194362f` | `ocpn::phraseStarts` (onset-gap boundaries); planning engine holds the KDE hand split **stable across a phrase** (recomputed at each ≥1-beat gap) instead of per-window, and re-seeds crossover/motion history at boundaries. `HeldOn.durTicks` filled from the buffer's matching note-off; `importanceScores` gains `w_rhythm` (short inner = active line, keep) + `w_static` (long inner = pad, drop) from durations. `reduceGroup` now takes an explicit `splitNote` (streaming passes the param, planning passes the phrase split). | planning | +5 (rhythm/static/phraseStarts) |
| **5b-2 — real per-line voice streaming** ✅ `67c7786` | `ocpn::streamHandVoices` — per hand, assign each kept note to line 0 (lead) or line 1 (secondary inner); line 1 only when a held note sits under a shorter one (`maxDur ≥ 2·minDur`) or line 1 is still sounding, else everything → line 0 (no spurious rests). `VoiceLineRT` state per hand persists across groups, resets at phrase boundaries. `handVoices` param now 3-way **Auto (streamed, default)** / 1 / 2. Voice-aware `dampSuccessive` in Auto: per output-channel (a held inner voice survives the melody's next attack). Channels: line 0 = RH `chanBase` / LH `chanBase+3`; line 1 = RH `chanBase+1` / LH `chanBase+2`. | planning | +6 (streamHandVoices) |
| **5c-1 — maxRingBeats re-strike** ✅ `fa61a07`; live-state fix 2026-09-06; `handleNoteOff` fix 2026-09-06; `seq`-identity fix 2026-09-06; bar-snap fix 2026-09-06 | `maxRingBeats` param (planning only). A note held longer than the limit is **re-articulated** every that-many beats (`pendingRestrikes`, `drainRestrikes`) — piano tone decays, and a long tie is neither idiomatic nor what a pianist plays. **2026-09-06, live-found via the decision log on Grieg's "Morning Mood"**: the original gate only armed a re-strike when the note's total duration was already known — found by scanning FORWARD from the note-on to a matching note-off WITHIN THE LOOKAHEAD BUFFER (`planBuf`, bounded by `lookaheadBeats`). A note whose real release sits further ahead than that buffer's own horizon has no match yet, so the known duration reads 0 and the gate never opens - exactly backwards for the case this exists to catch. Confirmed live: a genuine ~55-beat sustain (a real pedal point; `lookaheadBeats`=8 at the time) produced zero re-strikes despite `maxRingBeats`=4. Fixed by dropping the precomputed-duration dependency entirely: every emitted note now arms a checkpoint unconditionally (`PendingRestrike` no longer carries an `endPpq`), and `drainRestrikes` decides against LIVE `activeNotes` state whether the note is still actually ringing each time a checkpoint arrives - re-striking and rescheduling indefinitely for as long as it is, correct regardless of whether the note's eventual true length was ever knowable in advance. Logging moved from the (now-removed) scheduling-time message to `drainRestrikes` itself, firing only when a re-strike genuinely happens: `re-strike <note> (still ringing past N beats)`. **2026-09-06, second bug on the SAME symptom (the live-state fix above was necessary but not sufficient)**: after installing the fix above and re-capturing the identical Grieg passage, the exact same ~55-beat hung note (identical ticks) was still present — but the fresh decision log showed the new re-strike mechanism DID work correctly for 15 OTHER notes in the same take (logging `still ringing past N beats` as designed), with none at all for this one. Traced to `handleNoteOff()`: it unconditionally erased any `pendingRestrikes` entry matching the INPUT note's identity (`p.inCh`/`p.inNote`) the instant that input note released — regardless of whether the corresponding OUTPUT note (`activeNotes`) was actually still ringing. `streamHandVoices` can "promote" a short input attack into a continuing held output voice (an ongoing inner voice re-used across onset groups without a fresh note-on); that short input still gets its own natural, on-schedule note-off, which correctly leaves `activeNotes` alone (no identity match there) but was *also*, via this separate erase-by-input-identity block, silently cancelling the just-armed `maxRingBeats` checkpoint for the output note it had originated — before that note ever got a chance to be re-struck. Fixed by removing the blind `pendingRestrikes` erase entirely: no explicit cleanup is needed there at all, since `drainRestrikes`'s own live-check against `activeNotes` (already proven correct by the 15 working re-strikes) self-terminates a checkpoint the moment `activeNotes` genuinely no longer contains a live match. `handleNoteOff`'s only remaining job is the unchanged `activeNotes` release loop below. **2026-09-06, third bug, made VISIBLE by fix #2 above**: re-testing after fix #2 turned the (now-silent) hung note into something worse — the same bars 23-45 span filled with a cascading, ever-growing flood of `re-strike A2` in the decision log, spaced at fractions of a beat and multiplying bar over bar, instead of one clean periodic re-strike. Root cause: `PendingRestrike` and `TrackedNote` identified a note purely by the tuple (`channel`, `inputNote`, `outputChannel`, `outputNote`) — fine for a single long sustain (exactly one occurrence exists, ever), but that same tuple **recurs on every strike of a genuinely repeated note** (this passage is a real fast repeated/pedal figure, not one sustain — ties into the still-open Timpani-tremolo question above). A checkpoint armed for one specific strike, whose OWN quick real note-off had already correctly closed its exact `activeNotes` entry, could — once fix #2 stopped the old blanket erase — survive to its 4-beat check and find a *different*, unrelated LATER strike's entry instead, since that later entry shares the identical tuple; matching purely on the tuple can't tell the two apart. It would then conclude "still live," re-strike, and reschedule +4 beats again — and every one of the many other strikes in the passage arms its *own* such checkpoint too, so the false matches compound rather than settle. **Fixed** by giving each real emission a monotonic `seq` id, stamped on both its `TrackedNote` and its `PendingRestrike`; `drainRestrikes`'s live-check now requires `seq` to match too, so a checkpoint only ever recognizes the *exact* occurrence it was armed for and silently self-terminates the instant that one specific entry is gone — regardless of how many other, identically-pitched occurrences happen to be sounding at the same instant. The genuine-single-sustain case (only one `seq` ever exists) is unaffected. Confirmed live: the user's next capture showed one clean, evenly-spaced re-strike chain, no cascade. **2026-09-06, fourth issue — a real design gap surfaced by the fix chain finally working**: with the re-strike mechanism now correct, the SCREENSHOT of the Dorico render showed a mechanical repeating dotted-half-tied-to-eighth figure across many consecutive bars. Cause: `maxRingBeats` reschedules at a fixed beat-count (`+= step`) from the note's own onset, with no awareness of the meter - in 6/8 (3-beat bars) with `maxRingBeats`=4, the re-strike drifts through a different fractional bar-offset every cycle (only 1 in 3 lands on a downbeat), reading as an arbitrary, undeliberate restatement rather than a clean one. **Fixed**: new `snapUpToBar()` helper (uses the existing `beatsPerBar` bar-clock member) rounds every scheduled/rescheduled checkpoint UP to the start of the next bar at or after its raw target, both at initial arming (`reduceGroup`) and at each reschedule (`drainRestrikes`). Every re-strike now lands exactly on a downbeat. Known trade-off, accepted deliberately: this can push the actual ring time up to almost a full bar past `maxRingBeats`'s literal value - judged the right trade for a genuinely long-pedal use case (this feature's original motivating scenario, per its own header comment); revisit if a short-interval `maxRingBeats` tuning for a fast passage ever needs finer granularity than a bar. | planning | (processor-side; no `ocpn`) |
| **5c-2 — tremolo / repeated-note collapse** ✅ `2d0175d`; minGroups 4→8 fix 2026-09-06 | `ocpn::detectFigure` — over the next ≤20 buffered onset groups, a run of ≥8 hits (was ≥4 — see below) at a regular interval ≤0.4 beat alternating between two pitch sets → Tremolo (A≠B) or RepeatedNote (A==B). The planning engine emits the first 1–2 chords **held to the run's end** (re-struck by `maxRingBeats`, released by a scheduled hard-off), consumes the repeats + their note-offs, and logs `tremolo A~B (N hits, K beats) -> held`. **2026-09-06, live-found via the decision log on a real 6/8 melodic line**: a plain 4-note alternating neighbor-tone figure (an ordinary melodic turn, not an orchestral tremolo) matched the same shape at the same speed and got collapsed the same way - held as a dyad, its 3rd onset silently eaten once `figGroupsToEmit` hit 0. This was the risk flagged from Phase 5c-2's own introduction ("false-positive on fast scales, needs test"), now confirmed live. Raised the real call site's `minGroups` (`OrchPianoProcessor.cpp`) from 4 to 8 - roughly 2+ beats of continuous alternation, well past a short melodic gesture, still comfortably inside genuine tremolo territory - so a 4-note turn no longer qualifies; `detectFigure` itself is unchanged (still testable at any `minGroups`, see `OrchPianoReductionLogicCheck.cpp`'s dedicated regression case for this exact figure). `arpeggioRespace` + `octaveMovePassages` → 5c-2b. | planning | +6 (detectFigure) |
| **5c-2b — arpeggio / octave moves** | `arpeggioRespace` (wide arpeggio > hand → re-spaced); `octaveMovePassages` (whole-voice octave shift for a phrase out of hand range). | planning | — |
| **5c-2c — narrow-band murmur figures** ✅ `pending commit`; identity-based note-off fix 2026-09-06 | `ocpn::detectFigure` gained a third `FigureType::Murmur`, tried only when the strict Tremolo/RepeatedNote alternation above fails to reach `minGroups`. **2026-09-06, live-found on Grieg's "Morning Mood"**: after the maxRingBeats chain of fixes above resolved bars 23-45's pedal point cleanly, the user reported new "strange semitonal shifting" nearby. Parsed the user's own attached orchestral MIDI directly (not guessed): bars ~20-28 have the **Cellos** playing a fast figure oscillating among F#2/G#2/A2/B2 (54/56/57/59, a 5-semitone span) under the sustained horn pedal — a real accompaniment figure, not a trill (the user had already checked every instrument for one) and not strictly alternating between two pitch sets, so it fell straight through the existing figure-collapse untouched and played back nearly literally, sounding like erratic note-to-note jumping on piano. Rather than guess a notational treatment, checked the user's own MuseScore reference reduction of this exact passage (`Musescore_morning-mood-piano-version.mxl`, parsed as MusicXML): its left hand renders this figure as a **plain static held chord** outlining the harmony (e.g. F2+C3+A3 held a half note), never the literal fast repetition — confirming the same idea Tremolo already applies (hold the touched pitches, drop the repeats) generalises correctly here. `detectFigure` now also tries: a run of ≥`minGroups` groups at the same regular-interval tolerance, where the RUNNING UNION of every pitch touched so far stays within `maxMurmurSpanSemis` (real call site: 7, a 5th — wide enough for the real Cello figure, narrow enough to stay clear of a genuinely wide arpeggio, which is the separate, still-unbuilt `arpeggioRespace` case above). On a match, the processor holds the full union pitch set as one sustained chord for the run's span (`figGroupsToEmit=1`, same `pendingHardOffs`/`maxRingBeats` machinery as RepeatedNote) instead of the first 1-2 chords; `isFigGroup`'s membership test became subset-of-the-held-chord rather than exact-set-equality for this case specifically (a Murmur onset group is usually one note out of the larger chord, not the whole thing), leaving Tremolo/RepeatedNote's exact-match behavior untouched. New parameter defaults to off (`maxMurmurSpanSemis <= 0` skips the check) so every existing caller/test is unaffected unless it opts in. 8 new regression cases in `OrchPianoReductionLogicCheck.cpp` (95 total, was 87) using the real touched-pitch shape, confirming: correct Murmur classification + union-pitch chord, the off-by-default gate, a genuinely wide non-alternating spread is correctly NOT swept into Murmur, and strict alternation still wins priority when both would technically match. Rebuilt clean, VST3 reinstalled (Bitwig confirmed closed).

**2026-09-06, live-found regression this feature introduced: a real, unrelated note's note-off got silently swallowed.** After the maxRingBeats fix chain confirmed clean, the user's next re-test still heard "held and restated, strange note" bars 29-45 - and, crucially, pointed out the pitch doesn't even exist in the score during much of that span (confirmed independently: parsing the raw orchestral MIDI's ACTUAL sounding intervals for A2 shows real multi-bar silence gaps there, e.g. bar 30-32, 36-38, 39.8-42 - yet OrchPiano's output held it continuously). Root cause, found by re-reading the figure code with that specific question in mind: the note-off suppression that keeps a collapsed figure's "repeat" note-offs from prematurely releasing the held chord (`figOff` in `flushPlanBuffer`) tested PITCH membership in the held chord (`figSetA`/`figSetB`) only - not identity. Murmur's chord is a broad multi-pitch union (here `{54,56,57,59}`), covering the exact pitch (A2/57) of a completely unrelated, genuine **Horns** pedal note sounding nearby. When that Horns note's own real note-off arrived while the Cello murmur figure (a different instrument entirely) was still active, it matched `figSetA` by pitch alone and was silently discarded as "belongs to the figure, consumed" - never reaching `handleNoteOff` at all, so its `activeNotes` entry (and by extension every `maxRingBeats` checkpoint anchored to it) never closed, right through bars where the real note had already stopped. Same theoretical flaw existed for Tremolo/RepeatedNote too (narrower pitch sets made it far less likely to collide by chance) and for the analogous note-ON "is this a repeat, consume it" test (`isFigGroup`). **Fixed both, uniformly**: new `figConsumedIdentities` (a `vector<pair<channel,note>>`) records the EXACT attacks captured into a figure at detection time (parallel `gIdent` built alongside `gN`/`gO`); both the note-off suppression and the `isFigGroup` "repeat" test now check identity membership in this list instead of pitch membership in `figSetA`/`figSetB` - an unrelated instrument sharing a pitch with the held chord no longer matches either check. Rebuilt clean, VST3 reinstalled (Bitwig confirmed closed).

**RESOLVED 2026-09-06, but not by this fix.** A temporary diagnostic (input channel/note added to the `drainRestrikes` re-strike log line, kept permanently - see 5c-1's log format) was added to trace the still-persisting chain's identity, but before it was even used, the user independently found the true root cause: an **OrchMerge Sender instance with no clip on its track** was transmitting a real phantom note-on to the Hub (see the OrchMerge project memory, "stuck-note bug REOPENED"). Disabling that Sender instance made the same take come out completely clean. Every OrchPiano-side fix in this row and 5c-1 is real and worth keeping (each fixed a genuine, independently-verified bug), but none of them was the cause of this specific symptom - it was upstream in OrchMerge the whole time. Lesson for future debugging: an unexplained sustained/repeating note with no traceable real instrument means check for an idle Sender in the rig before assuming OrchPiano is at fault. | planning | +8 (detectFigure Murmur) |
| **5c-2d — timpani-roll → octave tremolo** ✅ `pending commit`; single-pitch gate 2026-09-06; LIVE-CONFIRMED 2026-09-07 | New `repeatedNoteTremolo` param (default on). Resolves the session's original Timpani-tremolo design question ("teach OrchPiano about these rolls"), asked at the very start of this Grieg investigation and answered only after the maxRingBeats/Murmur saga above was fully resolved (the real hung-note bug turned out to be an upstream OrchMerge Sender leak, not this). **Detection was already solved twice over, no new code needed**: (a) checked OrchMerge's actual source (`OrchMergeProcessor.cpp`) - the Sender is a byte-for-byte MIDI pass-through and the Hub only clamps+forwards `message.getChannel()`, never remaps it, so a per-instrument channel identity survives the whole chain intact if the rig keeps each instrument on its own channel; (b) more robust and used instead: a genuine orchestral roll (confirmed against the raw score - the Timpani part is nothing but the same pitch struck dozens of times, far faster than any pianist plays) is already exactly what `ocpn::detectFigure` classifies as `FigureType::RepeatedNote` - no register gate, no channel dependency, works today with zero changes. **Representation was the actual gap**: `RepeatedNote` previously collapsed a roll to one flat sustained hold, losing the "repeated attack" character entirely. Confirmed the correct piano convention is an octave tremolo (both directly from the user's earlier reference screenshot of a published Grieg reduction, AND by design symmetry with 5c-2c's MuseScore-verified approach). User confirmed the two open design choices directly: always transpose UP an octave (rolls sit low, going up stays in range) and a fixed 16th-note alternation rate (simple, always playable, regardless of the source roll's real - far faster - rate). New self-contained `PendingTremolo`/`drainTremolos` mechanism: on a `RepeatedNote` figure's one real emission, if `pitch+12` stays under a safe ceiling (108), schedules an alternation between `pitch` and `pitch+12` every 0.25 beat until the figure's release point, ending with a proper note-off + `activeNotes` cleanup (matched by the ORIGINAL input identity, not whichever phase happens to be sounding when it ends - the two can differ). A tremolo-scheduled note gets NEITHER the normal flat `pendingHardOffs` release NOR a `maxRingBeats` re-strike - both would otherwise race the alternation on separate schedules for the same output note. Falls back to the old flat-hold behavior if the octave-up partner would exceed the safe ceiling, or if the param is turned off (kept as an A/B toggle). Rebuilt clean (95/95 pure-logic assertions unaffected - this is processor-only, no `ocpn::` change), VST3 reinstalled (Bitwig confirmed closed).

**2026-09-06, live-test near-miss: gated to a single repeated PITCH only, not a repeated chord.** Added diagnostic logging (`tremolo scheduled/alternate to/ended` lines) to `reduceGroup`/`drainTremolos` while chasing an apparent "stuck, not alternating" symptom in a live capture - which turned out, on tracing against the raw orchestral score, to be a real, unrelated **clarinet trill** (a genuine semitone trill with a natural decrescendo in its velocities, not this feature's output at all - a near-miss worth recording: don't trust raw-MIDI pitch coincidence over checking the actual score). That same investigation surfaced a real, separate gap the log DID confirm: the decision log showed a genuine `RepeatedNote` figure elsewhere classified as `repeated D#4+E4` - a 2-note DYAD repeating, not a single pitch. Since the per-note emission loop applies the tremolo treatment independently to every note in a "kept" set, an unfiltered dyad would get TWO simultaneous tremolos (D#4~D#5 and E4~E5) - which in this instance would have landed on the exact same pitches as that real clarinet trill nearby, a genuine collision risk (two independent streams targeting identical notes) even though it wasn't this specific bug. **User's call, asked directly rather than assumed**: the octave-tremolo treatment should apply only to a single repeated pitch (the real timpani-roll case), never a repeated chord. Fixed by adding `figSetA.size() == 1` to the `figureIsRoll` gate in `flushPlanBuffer` - a genuine roll (single pitch, `detectFigure`'s A==B with a 1-element set) still gets the tremolo; a repeated multi-note figure now falls back to the pre-5c-2d flat-hold behavior untouched. Rebuilt clean, VST3 reinstalled (Bitwig confirmed closed).

**LIVE-CONFIRMED 2026-09-07, textbook-clean.** Next capture's decision log shows exactly the
intended split: `repeated D#4+E4 (11 hits, 2.3 beats) -> held` (the chord case, ×3, correctly
NOT tremolo'd - flat hold as designed) alongside `repeated B1 (20 hits, 5.0 beats) -> held` /
`tremolo scheduled B1 ~ B2` immediately followed by 17 clean `tremolo alternate to` lines and a
final `tremolo ended, released B2` - B1 (pitch 47) is the real Timpani part's own pitch from the
raw score, confirming this is the genuine roll case working end-to-end. Verified directly in the
exported MIDI too (not just the log): B1(47)/B2(59) alternate perfectly at exactly 240 ticks
(16th notes) apart, constant velocity 52, clean note-off-before-note-on throughout, ends with a
proper release, and real subsequent content (a natural diminuendo on repeated B1 alone) picks up
immediately after with no artifacts. Same capture reconfirmed the whole maxRingBeats chain is
still clean (A2 pedal: one periodic re-strike chain, no cascade, no leaks) and the new per-
identity channel diagnostic (5c-1) traced that pedal to channel 13 (Cellos) for anyone who
revisits it. Phase 5c-2d closed out. | planning | (processor-side; no `ocpn`) |
| **5c-3 — ornaments + dynamics marks** | input trill / grace-group → notation marker not note-spam; carry source velocity shaping to Dorico dynamics (`dynamicContour` "Preserve+Mark"). | planning | ornament detection |
| **5d — OrchCapture delay compensation** ✅ `16bbcb5` | **Re-scoped, see §6.1.** `delayCompensationCc` param (default 113): OrchPiano reports its constant `lookaheadBeats` delay on this CC (0..16 fits directly), sent at transport start / on value change / re-sent every 4 bars. **OrchCapture-side (its own repo):** `lookaheadCompensationCc` param (default 113, matches) — observes the CC (still passes it through untouched) and subtracts the reported beats from every captured note's onset/release, so the take lands at its real position instead of `lookaheadBeats` late. | planning | — |
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
- **P3 (post-test):** `handVoices` param added, default **1 (one voice per hand)** — the first
  Dorico test (Brahms Op.1/iv) showed the always-on 2-voice-per-hand split peppering the upper
  voice with rests. `07652a0` also fixed a block-end onset-group flush emitting at the buffer edge.
- **P4:** `revoice` / dynamic contour run **per hand** on the notes the hand kept, and treat any
  ≥3-note group as a chord. The real homophonic-vs-contrapuntal classification (and applying
  Close to the whole RH as one block) needs the lookahead window → P5.
- **P4:** `revoiceClose` returns the same note count with possible duplicate pitches (two inner
  PCs landing on one slot); the processor drops the duplicate and consumes its note-off.
- **P4:** `dynamicContour` has only Off/Preserve — "Preserve+Mark" (hairpin markers to the
  sidecar) deferred to P6.
- **P4:** figuration substitution, `octaveMovePassages`, ornament recognition **moved to P5**
  (all need cross-group context). `maxRingBeats` re-strike likewise — P5.
- **P5a:** on transport **stop** the buffered tail (~`lookaheadBeats` of music) is drained with
  zero delay, so it lands compressed at the stop point — timing of the last ~2 bars is wrong but
  nothing is lost / no stuck notes. Proper fix is the OrchCapture per-lane time-offset
  compensation (P5d) — for now the take should run a couple of bars past the last note you care
  about.
- **P5a (bugfix, post-test):** `setLatencySamples` was reporting the lookahead delay to the host →
  Bitwig **"Latency Compensation Overflow"** (its ceiling is 2 s; `lookaheadBeats 8` @ 121 BPM ≈
  4 s) + a jittery transport (re-reported every block as the tempo reading wobbled). **Removed** —
  OrchPiano's delay is absorbed downstream (P5d / manual clip nudge), never by the host. Default
  `lookaheadBeats` lowered 8 → **4** (1 bar) to halve the delay the user must compensate.
- **P5a:** adaptive split is **per lookahead window**, not per phrase (P5b), and clamped to ±9 st
  of the `splitNote` param so `splitNote` is now a *prior*, not an absolute.

---

## 6. Cross-repo dependencies

### 6.1 Re-scoped at 5d (2026-09-04) — the coordinator merged-tap idea was unnecessary

The original plan (§1 architecture diagram, §2, the P5d row before this edit) had OrchPiano
*subscribing* to OrchCapture's coordinator for a live merged view of the whole orchestra, needing
new coordinator-side streaming + a feedback guard to keep OrchPiano from consuming its own output.

That solves a problem that doesn't exist: **OrchPiano is already a MIDI effect that reads whatever
it's fed** (its `Direct` input path, built since Phase 2). A **Bitwig MIDI bus** — every orchestral
track's MIDI routed to one bus, OrchPiano sitting on it — already delivers "the whole score" with
zero new code, and OrchCaptureLink's coordinator is architecturally push-at-end-of-take anyway
(`OrchCaptureLink.h`: "on every completed take"), not a live stream — building one would have been
real new surface area on a repo the ecosystem docs call **complete**.

What was genuinely missing was narrower: **OrchPiano's own captured output lands `lookaheadBeats`
late.** Fixed with the CC-broadcast pattern this ecosystem already uses everywhere (MC's pitch
field, OrchHarp's field read) — no coordinator change, no feedback guard needed (there is no
subscription to feed back into):
1. OrchPiano reports its delay on a plain CC (§5d row above).
2. OrchCapture observes the same CC (still passes it through, stays transparent) and subtracts it
   at capture time - both `finalizeOpenNotes` and the note-off branch in `processBlock`, so it
   applies whether a take stops mid-note or ends cleanly. `OrchCaptureProcessor.{h,cpp}`, commit
   in that repo.

No MC change needed (Transform-mode field read reuses MC's existing 2-CC broadcast, as OrchHarp
Phase 2d does). No OrchCaptureLink / coordinator change. `inputSource`, `melodyChannels`,
`bassChannels` are dropped from the plan - Direct input on a merge bus covers the orchestral case.

### 6.2 Using it - the merge-bus workflow (Reduce mode, full orchestra)

1. Route every orchestral track's MIDI output to one Bitwig bus track (or however Bitwig sends the
   note path to a spare instrument track's MIDI-in - each track keeps its own audio chain
   untouched, this is a MIDI tap only).
2. OrchPiano sits on that bus track, `operatingMode = Reduce`, `lookaheadBeats > 0`.
3. Its own output (piano reduction) goes on to an OrchCapture instance as normal, same CC# on both
   (`delayCompensationCc` on OrchPiano == `lookaheadCompensationCc` on OrchCapture, both default
   113) - the capture lands at the right bar without a manual nudge.
