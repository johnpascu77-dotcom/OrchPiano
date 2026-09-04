# OrchPiano Finisher — scoping (proposed, not yet built)

Status: **SCOPING ONLY**, 2026-09-05. No code written. Working name "Finisher" —
open question, see §7.

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

## 7. Open questions for the next session

- Working name — "Finisher" is a placeholder used throughout this doc, not a proposal.
- §5's repo-location call — confirm or override.
- §3's velocity-vs-CC11 recommendation — confirm before Phase 3, since it's the one
  design choice here that isn't a straightforward "do what OrchPiano already decided."
- Does `music21` need to be added as a dependency somewhere tracked (a `requirements.txt`
  in `Tools/finisher/`), or is an ad-hoc environment fine for a personal tool?
- Should Phase 1's "is it actually better than Dorico's Reduce" check be eyeball-only, or
  worth a rough objective metric (OrchPiano_ReductionRules.md §14.4 already has a
  pitch-class-histogram-similarity metric defined for a different comparison — reusable
  here as a sanity check, not required)?
