# OrchPiano Finisher — scoping (Phase 1 built)

Status: **Phase 1 (merge only) BUILT, validated, and A/B'd against Dorico's own Reduce
2026-09-05** - decisively better (see §9). `Tools/finisher/orchpiano_finisher.py`. Name
and location (§5, §7) both confirmed: stays "Finisher", stays in this repo at
`Tools/finisher/`. Phase 2 (safety net) next; Phases 3-4 not started (§6).

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
| **2 — safety-net pass** | Same-voice-pair collision + boundary voice-crossing cleanup (§2). | Catches anything Phase 1's naive channel-to-voice mapping exposes that OrchPiano's own hand-scoped checks couldn't see. |
| **3 — dynamics** | Velocity → bucketed `<dynamics>` markings with hysteresis (§3). | Markings land at musically sensible points, not one per chord; A/B against the passed-through CC11 to confirm the velocity-derived version is actually more coherent, not just different. |
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
