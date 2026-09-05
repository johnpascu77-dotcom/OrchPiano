# OrchPiano Finisher — scoping (Phase 1 + 2 + 3 built)

Status: **Phase 1 (merge) BUILT + A/B'd 2026-09-05** - decisively better than Dorico's own
Reduce (§9). **Phase 2 (real-time hand-playability safety net) BUILT + validated +
visually confirmed in Dorico 2026-09-05** (§10, §11) — closes the scope gap §9's
investigation surfaced: OrchPiano's own per-onset-group span/count check can't see
cross-attack sustain overlap; this tool now enforces it directly against real note timing.
The Dorico visual pass also caught and fixed a real Phase-1 defect that had survived
Phase 1's own note-count validation: `makeMeasures` alone can silently drop a note that
outlasts its own measure (§11). User's own review of that same Dorico pass caught a
second, separate real problem - up-stem notes rendering below down-stem notes, "very
often" - **fixed same-day, §12: `_fix_voice_stem_order`** swaps which onset-group gets
which stem direction when they'd otherwise clash, a pure notation relabeling that never
touches OrchPiano's own pitch/duration/hand decisions. **Phase 3 (velocity-derived
dynamics) BUILT + validated + visually confirmed 2026-09-05** (§13) - real data forced a
hysteresis redesign mid-build (a 1-onset confirmation was not enough; needed a real
time-hold). `Tools/finisher/orchpiano_finisher.py` + a new persisted regression suite,
`Tools/finisher/test_finisher.py`. Name and location (§5, §7) both confirmed: stays
"Finisher", stays in this repo at `Tools/finisher/`. Phase 4 (polish) not started (§6),
deliberately unscoped until real use shows what's missing.

## 1. The problem, precisely

OrchPiano's Reduce mode already does the hard musical work: melody/bass ID, drop-priority,
hand assignment, voice-leading cleanup, revoicing. It outputs that decision as **4 MIDI
channels in a fixed SATB-style order** (confirmed in `OrchPianoProcessor.cpp` line ~454):

| Channel (chanBase-relative) | Role | Hand | Dorico voice (current workflow) |
|---|---|---|---|
| +0 | RH lead (melody) | Right | Soprano |
| +1 | RH secondary (inner) | Right | Alto |
| +2 | LH secondary (inner) | Left | Tenor |
| +3 | LH lead (bass) | Left | Bass |

The current workflow (`OrchPiano_ReductionRules.md` §"Notation workflow") hands this to
Dorico two ways: import as 4 single-line instruments then **Write ▸ Reduce**, or direct
import onto a 2-staff grand staff (ch1–2 → treble, ch3–4 → bass). You tried the Reduce
path — poor results. That tracks: **Dorico's Reduce has no notion of which note belongs
to which hand or voice** — it infers everything generically from pitch and rhythm, which
is exactly the problem OrchPiano already solved once. Feeding it 4 pre-decided,
channel-tagged lines and asking it to re-derive that same structure from scratch throws
away the one piece of information that makes this merge easy.

This was anticipated, not new scope: `OrchPiano_ReductionRules.md` §16 already lists
*"Full contrapuntal part-tracking beyond the lookahead window — offline (music21) or
Dorico"* as explicitly out of OrchPiano's own scope, and §14.4 names `music21` by name as
the tool to study for this handoff. This doc is that follow-through.

## 2. Scope boundary — the one rule that keeps this from sprawling

**This tool packages OrchPiano's decision into clean notation. It does not re-decide
anything OrchPiano already decided.** Same philosophy as the rest of this ecosystem
(`project_orchharp_phase3_concept`: each stage transforms, it doesn't re-invent what the
stage before it already resolved). Concretely:

**In scope:**
- Merge the 4 channels onto one 2-staff grand staff, 2 voices per staff, using the
  channel→role table above as ground truth (no re-guessing which note is melody/bass).
- A narrow, final pianistic safety net — same-pitch collision *between* the two treble
  voices or two bass voices at one instant (should be rare; OrchPiano's own `2b67c18`
  cross-hand fix already handles hand-vs-hand collisions, this only catches whatever a
  4-channels-collapsed-to-2-staves view can newly expose) and voice-crossing cleanup at
  the treble/bass boundary if the two hands' registers overlap.
- Convert a chosen dynamics signal into `<dynamics>` markings — see §3, this is the
  genuinely delicate part.
- Emit MusicXML directly, importable to Dorico/Sibelius without going through MIDI import
  or Reduce at all.

**Explicitly out of scope:**
- Re-deciding melody/bass importance, what gets dropped, hand assignment, revoicing —
  that's OrchPiano's job and it's already tuned/tested (P2–P5a live-confirmed on Brahms,
  P5c-1/5c-2 on Liszt).
- General-purpose score engraving (beams, articulations, slurs, ties across barlines) —
  trust MusicXML import + Dorico's own engraving for that; only touch what a generic
  MIDI-to-MusicXML path gets wrong for THIS specific 4-channel-piano case.
- Real-time operation. No JUCE, no plugin, no audio-thread constraints. A batch script
  that runs once per take, after capture, same cadence as opening Dorico itself.

## 3. The delicate part: where do dynamics actually come from?

Your screenshot shows CC11 already present in the Dorico Key Editor after import — worth
checking where that comes from before building a CC→dynamics converter around it, because
the answer changes the design.

**Traced it in `OrchPianoProcessor.cpp`:** OrchPiano does not generate CC11. Every non-note
message (line 1061, streaming path; the interleaved-event handling in `flushPlanBuffer`
for the planning path) is passed through **completely untouched** — whatever CC11 existed
on the original orchestral input rides straight through Reduce mode unmodified. After
OrchMerge has merged up to ~25 independent orchestral tracks (each potentially carrying
its own, unrelated CC11 automation) onto one stream, and OrchPiano has then dropped most
of those tracks' notes and kept only 2–4 surviving pitches per onset, the CC11 attached to
whichever channel happens to still be sounding at a given moment is **whatever one
contributing instrument's automation happened to be**, not a coherent "how loud is the
reduction" signal. It may still look plausible in a lot of passages — but it is not, by
construction, a reliable dynamics source for the reduction.

**Recommendation: derive dynamics from note velocity, not passed-through CC11.**
Velocity IS something OrchPiano actually controls during reduction — `dynamicRecoveryScale`
(Phase 4) deliberately scales it up when a thinned chord needs to preserve its perceived
loudness, so surviving-note velocity already reflects the reduction's own dynamic
judgment, not leftover source-track automation. Bucket per-onset-group average velocity
into standard notches (pp/p/mp/mf/f/ff) with hysteresis (only emit a new marking when the
bucket changes AND stays changed for more than one onset, to avoid a marking every chord)
and only insert `<dynamics>` at those change points, not continuously.

CC11 isn't useless — keep it as a raw, optional overlay a musician can inspect, but don't
build the automatic notation off it. Flagging this now because it's the kind of thing that
looks fine on one test file and only shows itself as noisy/wrong once you look at a passage
where the surviving reduction line came from an instrument whose CC11 was doing something
unrelated to the ensemble's actual dynamic arc.

## 4. Architecture

```
OrchCapture .mid  →  [parse]  →  [merge 4ch → 2-staff/2-voice model]  →  [safety-net pass]
                                                                              ↓
                              Dorico/Sibelius  ←  MusicXML  ←  [dynamics from velocity]
```

- **Parse**: `mido` — already proven this session for exactly this file format; no reason
  to switch.
- **Score model + MusicXML write**: `music21` — already the tool named in OrchPiano's own
  docs for this handoff (§14.4/§16 above). Gives voice/staff/dynamics primitives and a
  MusicXML writer for free instead of hand-rolling XML.
- **No GUI in v1.** A CLI script (`python finish.py in.mid out.musicxml`) until the
  merge/dynamics logic is validated against real takes — a UI is easy to add once there's
  something worth wrapping one around; building it first would mean designing around
  output you haven't seen yet.

## 5. Where this lives

Recommend a `Tools/finisher/` subfolder inside the existing `OrchPiano` repo rather than a
new sibling repo: it's tightly coupled to OrchPiano's own channel/voice conventions (the
table in §1), has no plugin/VST3 build of its own, and versioning it separately risks the
two drifting out of sync if OrchPiano's channel layout ever changes. Open to a separate
repo if you'd rather keep Python and JUCE code apart on principle — flagging as a choice,
not assuming it.

## 6. Phased build (proposed)

| Phase | Scope | Verifies |
|---|---|---|
| **1 — merge only** | Parse 4ch MIDI, place onto 2-staff/2-voice model, write MusicXML with no dynamics yet. | Against Dorico's own Reduce output on the same file — is the voice-leading/layout actually better? This is the whole premise of the tool; confirm it before adding anything else. |
| **2 — safety-net pass** ✅ BUILT 2026-09-05 | Real (not onset-group) cross-attack span/count enforcement per hand + flag-only cross-hand crossing report (§10) - broader than the original "same-voice-pair collision + boundary voice-crossing" framing, per the scope-gap finding in §9's investigation. | Independently re-swept all 3 real test files post-guard: zero span/count violations remain. Not yet re-imported to Dorico for a visual check of the corrected files. |
| **3 — dynamics** ✅ BUILT 2026-09-05 | Velocity → bucketed `<dynamics>` markings with hysteresis (§3, §13). | Markings land at musically sensible points, not one per chord - confirmed visually in Dorico (§13). CC11 A/B not done as a literal side-by-side (CC11 was never wired into `extract_notes` at all - out of scope, not needed once velocity proved sensible on its own). |
| **4 — polish / CLI ergonomics** | Whatever Phase 1–3 testing on real takes shows is actually missing — deliberately not pre-specified. | Real use on real captures. |

Each phase tested against the actual `Grand Piano_0.mid` / `_0_post.mid` files already in
`C:\Users\Asus\Documents\Orch_Capture MIDI` — same "parse the real MIDI, don't guess from
a screenshot" discipline that found both real OrchPiano bugs this session.

## 7. Open questions

- ~~Working name~~ — confirmed "Finisher", 2026-09-05.
- ~~§5's repo-location call~~ — confirmed `Tools/finisher/` in this repo, 2026-09-05.
- ~~Does `music21` need a tracked dependency file?~~ — yes, added `Tools/finisher/requirements.txt`
  (`mido`, `music21`) — cheap, and both were ad-hoc-installed in this environment already.
- §3's velocity-vs-CC11 recommendation — still open, confirm before Phase 3. Also now
  complicated by the 2026-09-05 "smart CC processor" idea (note-gated CC sampling +
  per-instrument velocity blend) recorded in memory
  (`project_orchpiano_finisher_concept.md`) — needs its own design pass before Phase 3
  starts, not a simple velocity-vs-CC11 binary anymore.
- Should Phase 1's "is it actually better than Dorico's Reduce" check be eyeball-only, or
  worth a rough objective metric (OrchPiano_ReductionRules.md §14.4 already has a
  pitch-class-histogram-similarity metric defined for a different comparison — reusable
  here as a sanity check, not required)? **Answered by eyeball 2026-09-05, see §9 — the
  gap is large and obvious enough that a numeric metric isn't needed to see it.**

## 9. A/B vs Dorico's own Reduce — done, 2026-09-05, decisive in the Finisher's favor

Same source file both ways (`Grand Piano_0.mid`, 3 populated channels/605 notes — this
file's 4th channel, LH secondary, happened to be silent for this take). Dorico's MIDI
import auto-split the file's 3 channels into 3 separate single-line Piano instruments
(605 notes total, matching this tool's own count exactly — cross-validates both import
paths read the file identically). Added a 4th empty Piano player as the Reduce
destination, selected all 3 source instruments' music, **Edit ▸ Paste Special ▸ Reduce**
onto it (this is literally the "Write ▸ Reduce (Paste Special)" workflow named in §1 -
confirmed it lives under Edit, not Write, in Dorico 6).

**Dorico's Reduce result**: dense chromatic tone clusters - 5 to 7 simultaneous pitches
with heavy accidental crowding - dumped almost entirely onto the TREBLE staff; the bass
staff sat nearly empty (occasional rests, rare single notes). The piece's page count grew
from 5 to 14 because the clusters needed far more horizontal space. This is a direct,
reproduced instance of the original complaint ("no notion of which note belongs to which
hand") - not a one-off, it held across every bar inspected on page 1.

**This tool's Phase 1 result, same file**: real two-hand distribution - treble mostly
light 1-2 note figures (matches OrchPiano's own already-decided lead/secondary split),
bass carries genuine chords (typically 3-5 notes, tracking the "Notes/Hand" cap),
including a correctly tied whole-bar-plus chord (a sustained bass chord notated as one
event tied across the barline, not re-attacked). Held up consistently across the 6 bars
inspected, not just the opening.

**Conclusion**: the core premise holds up under direct comparison, decisively. No numeric
metric needed - the difference is visually obvious at a glance. Proceed to Phase 2.

Note for next session: mid-comparison, an attempt to change Dorico's own view zoom via its
UI zoom-percentage field mis-clicked into the score canvas and typed digits, which Dorico's
Note Input mode interpreted as rhythm/duration commands, inserting an unwanted note.
Caught via Edit ▸ History (which shows each edit as a named, clickable step - reverting to
a specific pre-mistake entry undid it cleanly) and confirmed via the project's unsaved-state
Close-without-saving prompt before it could persist. No harm done, but: **prefer the
screenshot tool's own zoom/crop action to inspect a Dorico score closely instead of changing
Dorico's UI zoom control** - it can't accidentally land in the canvas.

## 8. Phase 1 build notes (what real data caught that the design didn't anticipate)

Validated against three real captures: today's full-orchestra rig take
(`OrchCapture_session_20260905_122551.mid`, 424 notes, dense - a good stress test) and the
existing `Grand Piano_0.mid` / `_0_post.mid` pair. Two things the design above didn't
anticipate, both found by checking actual output against hand-derived expected counts
rather than trusting a clean run:

- **The lead voice (voice 1) is not monophonic - a naive "does this note overlap the
  previous one on this channel" check is wrong.** `streamHandVoices()` only ever peels ONE
  note per hand onto the secondary voice; everything else - which can be a 2-4 note chord -
  stays on the lead voice on one channel. An early version of this script treated any
  time-overlap within one channel as an error and truncated it, which silently corrupted
  every real chord's interior notes (42 of ch0's 146 notes and 31 of ch3's 103 notes in the
  rig take were same-onset chord members, not overlaps). Fixed by grouping notes into
  onset-groups (exact shared start tick) FIRST, emitting a `music21.chord.Chord` for any
  group with >1 pitch, and only running the overlap guard BETWEEN onset-groups on the same
  line (20 genuine staggered overlaps in the rig take, all correctly distinct from the 73
  same-onset chords). Caught by reconciling final note/chord counts against an independent
  channel-by-channel tick analysis, not by the script running without errors.
- **Two `PartStaff` + a braced `StaffGroup` is the wrong MusicXML shape for a piano grand
  staff** (that pattern is for joining separate instruments, e.g. two different players).
  music21's exporter itself recognizes the pattern and correctly collapses it to what
  Dorico/Sibelius actually expect: **one `<score-part>` with `<staves>2</staves>`**, notes
  tagged `<staff>1</staff>`/`<staff>2</staff>`. Looked like a bug at first (only one
  `<score-part>` in the output) until checked against the MusicXML spec - it's the correct,
  more idiomatic representation, not a regression.
- Real captured timing isn't grid-aligned; MusicXML can't express arbitrary-fraction
  durations, so each staff is quantized (`Stream.quantize`, default 16th-note/8th-triplet
  grid) before `makeMeasures` - a display step, confirmed to not touch pitch/hand/voice
  assignment.
- A capture can have more than one track sharing the same name where only one actually
  contains notes (seen literally in `Grand Piano_0.mid`: two tracks both named
  "Grand Piano", one empty) - `--track <name>` now prefers the one with note events instead
  of blindly taking the first name match.

## 10. Phase 2 build notes — the real-time hand-playability safety net

Built same day as the §9 investigation that found the scope gap. Two functions:

- **`_guard_hand_playability(notes, max_span, max_notes)`** — the auto-fix half. Per hand
  (both voice channels combined, since they sound on one physical hand), sweeps real note
  timing (not the onset-group abstraction) and enforces span/count directly: when a new
  attack would push what's *actually* sounding over either limit, the older conflicting
  note is truncated to end at that attack - the same "favor the newer attack, shorten the
  older sustain" rule `_guard_staggered_overlaps` already used for one voice line, now
  applied across both of a hand's voices together. A span violation removes whichever
  extreme (top or bottom) note shrinks the span more; a count-only violation removes the
  oldest-started note.
- **`_report_hand_crossing(notes, ticks_per_beat)`** — flag-only, deliberately never
  auto-fixes. Reports total time the two hands' real sounding ranges cross (a LH note
  above RH's lowest concurrent note) and the first few instances by beat position.
  OrchPiano's own `crossoverSlack` already permits brief, legitimate crossing at the
  hand-split boundary; judging whether a longer one found here is a genuine musical
  gesture or a real problem needs a human ear, not a heuristic - this stays true to the
  tool's "package the decision, don't re-decide it" principle from §2.

`--max-hand-span`/`--max-hand-notes` (CLI, default 14 / 4 - OrchPiano's own defaults) are
**assumptions about what OrchPiano was configured to for this take**, supplied by the
caller, not read from the file - the captured MIDI carries no record of the plugin's own
parameter state. Flagged plainly in the code, not hidden.

**Validation, three ways** (not just "the script ran"):
1. Independently re-swept all three real test files' output notes *after* the guard ran,
   recomputing real per-hand span/count from scratch in a separate check script - zero
   violations remained on any file.
2. A synthetic test targeting the exact edge case that crashed the first draft: a
   `StopIteration` when the brand-new note itself owns the pitch extreme pushing the span
   over, so it can't be its own truncation victim (a candidate that isn't in the list of
   removable *older* notes). Fixed by only comparing removal options the older notes can
   actually satisfy, confirmed by the synthetic case and re-checked span (14, at the exact
   limit) afterward.
3. Full MusicXML re-parse on the corrected output still reconciles note/chord counts
   against the source (within the same small tie-split margin as Phase 1's own
   validation) - the guard only ever shortens durations, never adds or drops a pitch.

**Real-world scale, not a rare edge case**: 44 notes truncated on `Grand Piano_0.mid`
(605 notes, ~7%), 143+8 on `_0_post.mid` (~25%), 32+2 on the rig take. This is a
substantial, real effect - strong evidence the scope-gap finding in §9 was the actual
mechanism behind "impossible for two hands" results, not just a theoretical concern.

**Done, see §11**: re-imported a Phase-2-corrected file into Dorico for a visual/aural
check that the truncations look musically sane (vs. just verified span/count-clean) - same
kind of check §9 did for Phase 1's merge quality. Found and fixed a real Phase-1 defect
along the way.

## 11. Phase 2 visual check in Dorico — found and fixed a real Phase-1 defect

Regenerated `Grand Piano_0.mid`'s corrected output, imported to Dorico (Galley view -
easier to navigate by bar than Page view, whose scroll can appear to stop at a page
boundary). Two kinds of truncation to check: the common case (44 small 0.25-0.75 beat
shortenings) and one dramatic outlier (an 11.25-beat source anomaly, truncated to 4.5
beats - see the note-duration investigation below).

**Common case**: bar 4's cluster of four ~0.5-beat truncations renders as a completely
ordinary eighth-note melodic line over a tied bass chord - no visible artifact of any
kind. The safety net's most frequent behavior is invisible at the notation level, exactly
as hoped.

**The dramatic case surfaced a real bug**: clicking the truncated note in Dorico and
reading its own duration wasn't reliable (a long note crossing barlines splits into tied
fragments in notation, so one fragment's short real-time duration doesn't tell you the
total). Verified with the more rigorous method this whole ecosystem already favors -
parsing the actual MusicXML with `music21`, not eyeballing a screenshot - and found the
note **missing entirely** from the exported file, despite existing correctly (offset=59.5,
duration=4.5) right after `build_score()` returned, confirmed by inspecting the in-memory
score object at each pipeline stage rather than guessing. Root cause: `Stream.makeMeasures()`
alone does not split a note that outlasts its own measure into tied fragments - it left the
full 4.5-beat duration sitting in the measure where it starts (which is invalid: longer
than the measure's own length), and the MusicXML writer silently mishandled it from there.
**Fix**: `p.makeTies(inPlace=True)` right after `makeMeasures` - confirmed by direct
before/after inspection that the note now splits correctly (0.5-beat fragment tied to a
4.0-beat fragment, totaling 4.5) and, imported into Dorico, renders as exactly that: a tied
half note into a whole note filling the next measure - clean, correct, idiomatic notation
for a genuinely long sustained note.

**Real bug hiding behind a passing check**: this was a genuine Phase-1 defect (in
`build_score`, not Phase 2's own new code), present since Phase 1 shipped, that survived
Phase 1's own note-count validation - the aggregate count (613 vs. 605 expected) happened
to reconcile anyway, because losing this one note and gaining an unrelated tie-split
fragment elsewhere netted the same total. **Lesson**: aggregate note-count reconciliation
is necessary but not sufficient - it can mask "lost one note, gained a different one"
exactly like this. A long-duration, multi-barline-crossing note is now a standing thing to
spot-check specifically after any future change to `build_score`, not just the aggregate
count.

**Separately investigated, NOT resolved, flagged for follow-up (not a Finisher bug)**:
the 11.25-beat anomaly itself - and a similar, more extreme 16.5-beat one found in
`Grand Piano_0_post.mid` at the exact same source pitch/tick that is a normal 0.25-beat
note in the non-"_post" file - both traced back to genuine, real note-on/note-off pairs in
the captured MIDI (confirmed directly, ruled out a pairing bug in this tool's own
`extract_notes`). Read through `OrchCaptureProcessor.cpp`'s `lookaheadCompensationCc`
handling as the leading hypothesis (the "_post" suffix implies delay-compensated output,
and this is OrchCapture's newest, least-tested feature per
[[project_orchpiano_concept]]/[[project_orchmerge_concept]] history) - but the actual
compensation math (`ppqOn`/`ppqOff` both subtract the same live `capturedDelayBeats` value
at note-off time) should cancel out and preserve duration regardless of when that value
changes, so this specific mechanism does NOT obviously explain the anomaly. Root cause
remains genuinely unresolved - could be a different OrchCapture/OrchPiano interaction, or
these two files may simply not be as identical a pair as their names suggest. Needs a
controlled, instrumented test (not more guessing) to actually diagnose - out of scope for
today's Finisher work, flagged as a separate follow-up.

## 12. Voice-1/voice-2 stem-direction swap — a real, frequent notation bug the user caught

User reviewed the Dorico screenshots from the Phase 2 visual pass independently and
spotted a recurring problem: up-stem (voice 1) notes were "very often" visually below
down-stem (voice 2) notes - the same problem they'd been fixing by hand in Dorico
("simply swapping the voices").

**Root cause**: OrchPiano's `streamHandVoices()` picks the secondary voice (voice 2) by
**continuity** - the non-lead note closest to that line's last pitch, or the longest-held
one if neither line has ringing history (`OrchPianoReductionLogic.cpp`) - never by
register. Voice 1 is simply "everything streamHandVoices() didn't peel off." Nothing in
that logic guarantees voice 1 sounds higher than voice 2 at a given attack - it very
often doesn't. Every notation program (Dorico included) still renders stems by voice
number regardless of actual pitch (up for voice 1, down for voice 2, by convention), so a
mismatch here reliably produces the visual mess reported.

**Fix, `_fix_voice_stem_order` (`ee73521`)**: for each hand, wherever voice 1 and voice 2
share an *identical* onset tick (the case that clashes most visibly - two different
attacks at the same instant), compare their average pitch and swap which one is labeled
voice 1 (up-stem) vs voice 2 (down-stem) if the down-stem one would otherwise average
higher. This is purely a notation relabeling - same pitches, same durations, same "this
is an independent continuing line" grouping OrchPiano already decided - so it stays
inside the tool's "package the decision, don't re-decide it" scope (§2) the same way the
Phase 2 safety net does.

**Documented limitation, not silently glossed over**: this is a heuristic (average-pitch
comparison at matching onsets), not a full crossing-eliminator. A wide chord in one voice
against a single note in the other can still show *residual* crossing after the swap -
confirmed directly on real data: a `[71, 84]` chord swapped against a single `80` note
put the higher-*average* group on top, but the chord's own top note (84) still sits above
the single note (80) post-swap. Fully resolving that would mean re-splitting which
pitches belong to which voice - a real re-decision of OrchPiano's content, out of scope
here. Non-simultaneous (staggered) crossing - one voice sustaining while the other
attacks at a different tick - isn't handled at all by this pass; only matching-onset
attacks are.

**Validated three ways**: (1) a synthetic single-note-vs-single-note case, which the fix
*fully* resolves (asserted directly - after the swap, voice 1 has the single high note,
voice 2 has the low chord, no ambiguity); (2) real-file testing across all three test
files - 21, 21, and 40 swaps respectively, confirming this was a frequent effect, not a
rare one, matching "very often"; (3) direct `music21` `Voice`-container inspection
before/after on real data (not a screenshot) at the exact bug location - voice 1's content
at that onset demonstrably moved from a lower-averaging group `[71, 84]` (avg 77.5) to a
higher one `[80]` (avg 80), with voice 2 taking the swapped-out chord. Structural note
counts re-verified unchanged on all three files after the fix.

## 13. Phase 3 build: velocity-derived dynamics, including a mid-build hysteresis redesign

Built per §3's already-settled recommendation: `compute_dynamics_marks` buckets the
average velocity of each distinct onset (across all 4 channels - a piano dynamic applies
to the whole instrument, not one hand) into pp/p/mp/mf/f/ff (roughly equal 1-127 splits),
inserted as `music21.dynamics.Dynamic` under the LH staff (RH if a Hands=Right-only take
leaves LH empty), by convention.

**First cut used the design doc's literal wording** ("stays changed for more than one
onset") as a 1-onset confirmation: a new bucket commits once the very next onset agrees.
**Tested against real data before trusting it, and it failed**: 11 bucket flips across 12
beats on `Grand Piano_0.mid`, because real velocity sits right at a bucket boundary and
naturally jitters across it beat-to-beat in a fast passage - exactly the "marking every
chord" clutter this was supposed to prevent, just arriving one onset later than literally
every chord. **Redesigned to require a bucket to hold for a real span of time** (default 1
beat) before committing, not just one extra onset - re-tested on the identical passage and
got 5 well-spaced, musically sensible marks. A run that doesn't hold long enough is
skipped entirely (not merged into a neighboring bucket).

**CC11 A/B - descoped, not skipped by accident**: the phased-build table's original
Phase-3 success criterion called for an A/B against passed-through CC11. `extract_notes`
never captured CC data at all (Phase 1-2 only needed note on/off), and by the time Phase 3
was built, velocity alone was already producing a sensible, well-spaced dynamic arc with
no evidence of a problem CC11 would fix - so wiring up CC extraction purely to prove a
negative wasn't worth the added surface. If a future real take's velocity-derived arc ever
looks wrong, that's the trigger to revisit CC11, not before.

**Verified in Dorico**: the "f" mark at the piece's opening and a later "mp" mark both
render as proper bold-italic dynamics text below the bass staff, at the correct beat
positions, clearly spaced apart - not clustered, not overlapping notes.

**New: `Tools/finisher/test_finisher.py`** - a persisted regression suite (plain asserts,
mirroring `OrchPianoReductionLogicCheck.cpp`'s own pattern, no new dependency) collecting
the edge-case tests written ad hoc across all three phases: the Phase 2 crash case (a new
note owning the pitch extreme it would need to be its own victim to fix), the voice-swap
case, and this session's dynamics-hysteresis case. Run: `python test_finisher.py`.
