# OrchPiano — Reduction & Voicing Rules

**Status:** design reference, written before any code (mirrors `OrchHarp/Docs/OrchHarp_Design.md`).
Scoping session 2026-09-03. This document is the spec for OrchPiano's **importance-drop + re-voice
stage** — the "smart pianist" that turns a dense or open-scored input into a playable, notation-clean
two-hand piano part.

Companion memory: `project_orchpiano_concept`, `project_orchharp_design` (source of the reusable
Voicing engine), `project_orchharp_phase3_concept` (the subtractive-orchestration philosophy),
`project_orch_system_ecosystem` (where OrchPiano sits), `project_orchcapture_scope` (the capture
path + the merged-stream tap this doc now depends on).

---

## 0. What OrchPiano is

A JUCE VST3 **MIDI effect**. Input = a note stream. Output = a playable two-hand piano part, MIDI
out → OrchCapture → Dorico. It exists for three jobs, all pulling the design the same way —
**conservative, human-playable, notation-friendly**:

`operatingMode` (3-way):

| Mode | Job | Behaviour | Engine (§1) |
|---|---|---|---|
| **Repair** | a piano part that won't physically play (span of a 12th, 6 voices, impossible stretch) | fix **only** the impossible. Touch nothing already playable. `revoice = Framework`, all idiom substitution off unless forced. | planning |
| **Reduce** | reverse-engineer an orchestration into an idiomatic piano reduction | full rule set: melody/bass ID, importance-drop, re-voice, figuration substitution, octave moves. | planning |
| **Transform** | rig member — orchestrate a composed field into a piano voice (like OrchHarp) | contour re-quantise, Center/Span register travel, generative re-voicing. Looser leash. | streaming |

`Hands` param = **Both** (default — one instance sees the whole texture, needed for L/R crossover
decisions) / **Left** / **Right** (two-instance rig with independent register automation).

---

## 1. Two engines — streaming vs planning

OrchPiano has **two decision procedures sharing one rule set** (§§4–11). Which one runs is set by
`lookaheadBeats`.

| | **Streaming engine** — `lookaheadBeats = 0` | **Planning engine** — `lookaheadBeats > 0` (default 8 = 2 bars in 4/4) |
|---|---|---|
| For | Transform mode / rig / live | Repair + Reduce modes |
| Latency | none | constant `lookaheadBeats` of musical time |
| Decision | greedy, note-by-note at each onset (what OrchHarp does today) | buffers the segment, analyses the whole content, then commits decisions as **deliberate choices** |
| Voice assignment | nearest-available-voice at onset | cost-minimising assignment across the whole window (minimise total leaps + crossings + rhythmic incoherence) |

Reduction is hard exactly where a greedy engine is blind: it can't see where a line is going, that
a "doubling" is about to carry the tune, or where the phrase ends. Reduction is not a
real-time-performance task — trading latency for intelligence is the right call here.

### 1.1 Planning-engine pipeline (per buffered segment)

Each pass feeds the next:

1. **Melody & bass identification** (§4) — done first; everything downstream protects the result.
2. **Part-track** the segment — voice assignment across the whole window, not per-note.
3. **Classify each vertical** — homophonic block / contrapuntal / mixed (§3), from the settled
   voice streams rather than a 100 ms local guess.
4. **Harmonic-rhythm detection** — where chords *actually* change (not every bass wiggle) → drives
   pedal (§11) and the "land the chord change on time" rule (§6.3).
5. **Phrase segmentation** — rests, agogic accents, density troughs, MC section signals (§1.3).
6. **Hand assignment per phrase** — plan the split once per phrase so the hands don't jump around.
7. **Ornament recognition** (§10.2) — collapse input trills/tremolos/grace groups to notation
   markers before they clog a voice.
8. **Drop / re-voice / octave-move** per §7–§8 — informed choices, content known first.
9. **Voice-leading cleanup** (§12.2) with full context.
10. **Dynamic-contour reconstruction** (§7.2).

Every decision from passes 8–10 is written to the **decision-log sidecar** (§14.2).

### 1.2 Determinism

The planning engine is **fully deterministic**: same input + same params → byte-identical output.
All tie-breaks use a seeded RNG (`planSeed` param, default 0). This is what makes the
supervise-and-iterate loop work — re-run, compare, adjust one weight, re-run. OrchHarp's
`selectVoices` is already pure/deterministic; keep that discipline.

### 1.3 Re-planning points (phrase boundaries)

The planner re-plans at: rests, agogic accents (long notes), density troughs, and — free from the
rig — **MC's narrative-position CC and pitch-field-broadcast changes**, which both signal a new
section.

### 1.4 Latency compensation — handled by the OrchCapture connection

Emitting everything `lookaheadBeats` late means the captured MIDI lands that much late. OrchPiano
reports its **constant delay** to OrchCapture; OrchCapture subtracts it from every event timestamp
on that lane before writing the SMF. Since OrchPiano already reads from OrchCapture's coordinator
(§2), the two are in the same IPC loop — the delay is one integer on a channel that already
exists. See §16.1.

---

## 2. Input sourcing — Direct vs OrchCapture merged stream

The rig use and simple reductions take MIDI straight off a track/bus. A **full-orchestra
reduction** has to see *every* part at once — and the only place the ecosystem assembles the whole
score into one stream is **OrchCapture's coordinator** (port 47826, lock-file discovery, all
off the message thread — `feedback_no_message_thread_ipc_at_scale`). Any real orchestral template
already runs OrchCapture, so this is a **sane match, not extra infrastructure**.

`inputSource` (choice):

| Source | Path | Use |
|---|---|---|
| **Direct** | MIDI in from the track/bus OrchPiano sits on | Transform (rig), Repair, single-section reductions |
| **OrchCapture merged** | subscribe to the coordinator's live merged note stream over the existing coordinator IPC | full-orchestra Reduce — zero extra Bitwig routing |

### 2.1 What OrchCapture must add

- **Coordinator live merged-tap emit.** The coordinator already merges ~59 lanes off-thread for
  its SMF; it exposes that merged note stream to a subscriber (OrchPiano) over the same socket.
  New OrchCapture capability, natural extension.
- **Per-lane constant time-offset compensation** (§1.4) — OrchPiano reports its `lookaheadBeats`
  delay; the coordinator subtracts it from OrchPiano's own capture lane on export.
- **Feedback guard.** OrchPiano's output is itself captured by OrchCapture. The coordinator must
  **exclude OrchPiano's own output lane** from the merged stream it feeds back to OrchPiano, or
  the reduction eats its own tail. Tag the lane; exclude by tag.

### 2.2 Chain position

Transform mode: parallel to OrchHarp — on a piano track, after any ONF field-filtering, at the
CC49-analogue slot (note: **OrchConductor's CC20–54 map has no piano slot** — harp is CC49, piano
isn't an orchestral section; as a rig member OrchPiano either gets a dedicated CC assignment or
simply sits outside OrchConductor's gating). Reduce mode: fed by the coordinator, so it sits
logically *after* the whole note-path chain, consuming the merged result.

---

## 3. The detector: **homophonic block vs contrapuntal texture**

Every hard decision below (chord vs voice, drop vs re-stack) branches on the same test:

> **Are the onsets in this window synchronised?**
> Streaming engine: sliding window (`textureWindow`, ~60–120 ms) — active notes sharing onsets
> *and* releases → **homophonic block** → chord, re-voice (§8). Staggered/independent →
> **contrapuntal** → stream into voices, drop by importance (§7).
> Planning engine: same question, answered from the settled voice streams (§1.1 pass 3) — far more
> reliable.

Mixed textures (melody + block accompaniment) resolve per-role: the melody streams as its own
voice, the accompaniment block re-voices underneath it.

Rationale (Belkin; Class): a reduction is only "faithful to the musical text" if it keeps "the
intended level of rhythmic activity" — independent rhythm *is* the counterpoint and must survive
as a separate voice; synchronised rhythm is harmony and can be re-spaced freely.

---

## 4. Melody & bass identification (load-bearing — do this first)

The entire rule set pivots on this. In real orchestral music the melody is frequently **not** the
top note (cellos under tremolo, a horn under the strings, a tune doubled two octaves down). Pick
the wrong line and "never dropped" protects the wrong thing.

### 4.1 Trust the source when it's structured

- **Rig / MC-driven:** MC/MPL already knows phrase roles (the role/archetype system). OrchPiano
  takes explicit **`melodyChannels`** / **`bassChannels`** inputs and does **not** second-guess
  them. Free for MC-driven use.
- **OrchCapture merged:** the coordinator knows each lane's host track name — a "Vln I" / "Vc" /
  "Cb" lane carries a strong prior. Pass track-name hints through the merged stream.

### 4.2 Salience heuristic (raw MIDI, no structure)

Over the lookahead window, score each line's "melodic-ness":

- **+** stepwise / small-interval connection to the notes before and after it in its band
- **+** rhythmic activity (a moving line, not a pad)
- **+** registral extreme — highest, **or** an isolated low line well below the mass
- **+** velocity accent relative to the texture
- **+** melodic contour continuity across the phrase (planning engine only)
- **±** note length — **context-dependent, weak** (see below)

Highest-scoring line = melody. Bass = lowest **sustained, harmonically-functional** line (not a
passing low note). This is the skyline algorithm plus refinements — the planning engine can afford
the full version.

> **Note length is not a reliable melody cue** (empirical — §14.5, Brahms Op. 1/iv: the top note
> was also the longest in its onset group only **4 %** of the time). In a slow movement the melody
> *is* often the longest line; in a fast movement the long notes are **held pedal tones under an
> active top line** — the opposite. So weight length **positively only when the segment's note
> rate is low** (a slow-movement heuristic), otherwise near-zero. Rhythmic activity + registral
> top + contour continuity are the dependable cues.

### 4.3 Visible & overridable

The editor shows the line OrchPiano currently reads as the melody (and the bass), the way OrchHarp
shows sounding-vs-requested diagram. `melodyOverride` / `bassOverride` pin a channel or
pitch-range per section. A wrong guess here is the single biggest failure mode — surface it.

---

## 5. Textural-role model

Tag every sounding note with a role each block. Roles drive the drop order (§7) and the protect
list. Adapted from Open Music Theory's "core principles" and the standard orchestration role set:

| Role | Detection | Reduction treatment |
|---|---|---|
| **Melody** | §4 | **never dropped.** Held in top voice. Melodic octaves kept if playable. |
| **Bass** | §4 | **never dropped.** Held in lowest voice. Octave doubling *added* where it helps (§9). |
| **Inner harmony / pad** | sustained, low melodic activity, fills a chord tone already implied | first to thin. Static inner pedal tones → drop or let the pedal cover them. |
| **Figuration** | repeated/arpeggiated pattern, mid-register, rhythmically regular | keep the *gesture*, re-spell for the hand (§10). |
| **Ornament / color** | short, non-structural, high or percussive | drop unless trivially free; recognised ornaments → markers (§10.2). |
| **Doubling** | duplicates a pitch class already sounding in another octave | **drop first** (§7), unless a melodic or bass reinforcement octave. |
| **Countermelody** | independent line, melodic salience just below the melody's | protected like an inner voice with unique rhythm — never merged into a chord. |

---

## 6. Priority of preservation (the spine)

Consensus order across every source consulted (Belkin, Class, Padworski, tonebase). Keep, in this
order, as far as the hands allow:

1. **Melody** (the foreground line a listener hums).
2. **Bass line** ("keep the melody and the bass parts").
3. **Harmonic rhythm** — the chord *changes* land on time even if the voicing is thinned.
4. **Rhythmic activity level** — substitute figuration (§10), don't flatten to whole notes.
5. **Inner voices with independent motion** (real counterpoint).
6. **Characteristic tones** — 7ths, 9ths, added notes, cluster members — over plain triad doublings.
7. **Static inner harmony** (pads) — first to go.
8. **Doublings and resonance notes** — go before anything structural.

### 6.3 "Land the chord change on time"

From harmonic-rhythm detection (§1.1 pass 4): even when a chord's voicing is thinned to 2–3 notes,
the *moment it changes* must be articulated. If the hands are full, drop an inner tone from the
*outgoing* chord rather than delay the *incoming* one.

---

## 7. Drop-priority order (contrapuntal / mixed texture)

When voice count or hand span forces a cut, **drop in this order** (drop first → drop last). The
function-weighted filter from `project_orchharp_phase3_concept`, with a piano-specific weight model.

| # | Drop candidate | Condition / exception |
|---|---|---|
| 1 | **Exact octave/unison doubling** of a pitch class already sounding | *unless* melody octave or a wanted bass-reinforcement octave (`keepMelodyOctaves`, `keepBassOctaves`) |
| 2 | **Static inner pedal tone** already implied by the harmony | especially if the sustain pedal (§11) covers it audibly |
| 3 | **Least rhythmically active inner voice** | a held whole-note alto goes before a moving tenor |
| 4 | **Resonance / accent-only notes** | short reinforcements, tam-tam-style low notes |
| 5 | **Lower note of a span-breaking pair** | the lower-scored of the two notes exceeding hand span |
| 6 | **Interior chord tone with no independent motion** | keep one instance of each pitch class first |
| — | **NEVER drop:** melody, bass, any voice with a unique rhythmic profile, a detected countermelody | — |

### 7.1 Importance score

Per sounding note (all terms cheap). Streaming engine: instantaneous. Planning engine: every term
may look forward across the window.

```
score  =  w_top      * (isHighestInOnsetGroup)
        + w_bottom   * (isLowestSounding)
        + w_rhythm   * (onset/offset not shared with any other active note)   // independence
        + w_motion   * (abs semitone move from this voice's previous note, capped)
        + w_charTone * (interval from bass in {m7,M7,m9,M9,tritone,cluster})
        + w_velocity * (note velocity relative to the current texture mean)
        - w_double   * (pitch class already sounding in another octave)
        - w_static   * (note held > staticThreshold with no neighbours moving)
        + w_pin      * (channel / pitch-range pinned by user or MC)           // protect list
        + w_future   * (planning: this note/line becomes structural later in the window)
```

Keep the top *N* by score, where *N* = voices available for that hand after span + poly + LIL
(§10.1) constraints. Weights are advanced params with sane defaults.

**Constraint-ceiling alternative (from Nakamura & Sagayama, §14.4).** Instead of only tuning
weights, expose `difficultyCeiling` — the planner keeps the *highest-fidelity* subset whose
estimated difficulty (span, polyphony, hand-transition cost, tempo) stays under the ceiling. One
knob the user turns toward "easier" or "closer to the original." The importance score above ranks
candidates *within* that budget.

### 7.2 Dynamic-contour reconstruction

Dropping 30 notes from a tutti must not turn *ff* into *mf*. After the drop set is fixed, scale the
**surviving** notes' velocities so the kept chord's total perceived energy tracks the original
onset group's (roughly: `Σ kept ∝ Σ original`, capped at 127, floor so a thinned passage never
reads louder than a full one). Dorico reads velocity → dynamics on import, so this feeds the
notation, not just the audio tap. `dynamicContour` (Off / Preserve / Preserve+Mark — the last also
writes hairpin/dynamic markers to the sidecar).

### 7.3 On accuracy

Streaming engine: fast/dense passages *will* be misjudged — output stays curatable, Dorico
**Write ▸ Reduce** + supervision is the backstop. The **planning engine largely removes this** for
Repair/Reduce, which is where it mattered. Full contrapuntal part-tracking beyond the lookahead
window is still music21 / Dorico's job.

---

## 8. Re-voicing open orchestral chords + whole-passage octave moves

The "reverse orchestration" feature. When §3 says *homophonic block*, do **not** just drop notes
where the orchestrator left them — **re-stack**. Rules from score-reading-at-the-keyboard practice
(Morris & Ferguson; Class; Belkin — "spacing almost always has to be changed").

### 8.1 `revoice` — 3-state

| Setting | Behaviour | Use |
|---|---|---|
| **Off** | literal pitches, drop-only, no re-stacking | pure transcription; fix spacing in Dorico |
| **Framework** *(default for Repair + Reduce)* | keep outer voices **exact**; collapse only octave/unison **doublings** and notes that **fail the low-interval-limit check** (§10.1). Leave already-playable inner voices alone. | conservative, "stay on the safe side" — kills mud and finger-wasting doublings, rearranges nothing already fine |
| **Close** *(default for Transform)* | full close-position re-stacking (§8.2) | genuinely open orchestral chords spread across 3–4 octaves |

### 8.2 Close-position rules (`revoice = Close`)

1. **Keep the outer framework exactly** — lowest and highest notes inviolate (register + melody).
2. **Reduce to one instance per pitch class** — prefer the register where that pitch class was
   *most prominent* (loudest / most-doubled / on the beat).
3. **Collapse inner pitch classes toward the melody** — close-ish position in the octave *below the
   top note*. Aim for one hand.
4. **Bass framework in the left hand** — bass note, optionally + octave (§9) or a tenth. Avoid a
   bare low third/sixth (§10.1).
5. **Keep characteristic tones** (7/9/11/added/cluster) over a plain fifth or a doubled third.
6. **Respect keyboard spacing** — upper voices within ~an octave of each other; the gap to the bass
   may open wide.
7. **If it still doesn't fit one hand** — Roll (fast arpeggiation, OrchHarp's Phase-2a gliss
   scheduler / in-block strum), or drop the innermost non-characteristic tone.

Contrapuntal passages **skip §8 entirely** — the "chord" framing is wrong there.

### 8.3 Whole-passage octave transposition (planning-engine move)

Distinct from Center/Span travel. If the planner finds a **whole voice sits outside the comfortable
hand range for an entire phrase** and transposing it by an octave loses nothing structural (no
collision with another voice, stays within the field), move the whole voice for that phrase and
mark it — Dorico `8va`/`8vb`, or just place it where it lands. Standard reduction practice
(piccolo material down, a bass line up out of mud). `octaveMovePassages` (bool, planning only);
every move logged to the sidecar.

---

## 9. Octave doubling — when to keep, when to cut

Default instinct is **cut** (§7.1 penalises doublings), but three cases *keep or add* an octave
(Class, tonebase):

| Case | Action | Rationale |
|---|---|---|
| **Bass in an accompaniment** | keep / **add** LH octave | "the color becomes distinguishingly more 'orchestral' if the bass is played in octaves" — warmth, not volume (Class) |
| **Melodic octaves** (melody already in 8ves in the orchestra) | keep if playable, else keep the **upper** octave | melody identity |
| **Isolated low percussion / timpani hit** | play in octaves, drop the initial attack an octave (Class) | body without mud |
| **Right-hand octave *run*** | **eliminate the lower octave** | playability (tonebase) |
| **Left-hand octave *run*** | **eliminate the upper octave** | playability (tonebase) |
| Everything else | cut the doubling | "remove those unison notes" (Padworski) |

Params: `keepBassOctaves` (Off / Keep / Add), `keepMelodyOctaves` (bool), `octaveRunThinning` (bool).

---

## 10. Figuration & idiom

### 10.1 Substitution — keep the gesture, change the spelling

(Belkin: "quick repeated notes can become tremolos … arpeggios are often re-spaced")

| Input figure | Piano substitute | Param |
|---|---|---|
| Fast repeated notes (faster than `repeatLimitMs`) | measured tremolo between the note and a chord tone | `repeatedNoteTremolo` |
| Wide arpeggio spanning > hand | re-spaced arpeggio within reach, or hand-alternation | `arpeggioRespace` |
| Sustained string chord (piano can't sustain/crescendo) | re-strike at `resustainInterval`; optionally roll to soften attack | `stringResustain` |
| Any note held longer than **~4 beats** (piano tone decays — BERT paper, §14.4) | re-strike or release; never notate a piano note ringing past ~4 beats without a re-articulation or a tie the pedal justifies | `maxRingBeats` (default 4) |
| String tremolo | keep as tremolo, or bisbigliando-style for *pp* (borrow OrchHarp's bisbigliando engine) | `tremoloStyle` |
| Glissando (un-notatable exactly) | chromatic run within the window, pedal through | `glissAsRun` |

Repair mode applies only what physical playability needs; Reduce applies the lot; Transform-leaning.

**Low Interval Limits — the anti-mud table.** Below these bottom notes an interval turns to mud and
must be **opened, dropped, or octave-shifted up** (Kennan, *The Technique of Orchestration*;
corroborated by Adler and the FunJazz / Robin Hoffmann charts). "Bottom note" = the lower pitch;
holds whether the root is sounded or only implied.

| Interval | Lowest usable bottom note | ≈ MIDI |
|---|---|---|
| minor 2nd | E3 | 52 |
| major 2nd | E♭3 | 51 |
| minor 3rd | C3 | 48 |
| major 3rd | B♭2 | 46 |
| perfect 4th | B♭2 | 46 |
| tritone | B♭2 | 46 |
| perfect 5th | B♭1 | 34 |
| minor 6th | A♭2 | 44 |
| major 6th | G2 | 43 |
| minor 7th | F2 | 41 |
| major 7th | F2 | 41 |
| **octave** | no theoretical limit (practical ≈ C1 / 24) | — |
| minor 9th | E2 | 40 |
| major 9th | E♭2 | 39 |
| minor 10th | C2 | 36 |
| major 10th | B♭1 | 34 |

**Application:** on re-voice (§8) or hand-split (§12), check every interval against its bottom note.
Fail → (a) move the upper note up an octave into close position, (b) drop it if it's a
non-characteristic doubling, (c) open the LH to a tenth (fifths and tenths are the safe wide bass
intervals). Thirds and sixths are the dangerous ones low down.

**Caveats (all sources agree):** loose guidelines. Concert grand tolerates lower than an upright;
*pp* strings lower than *ff* brass; a deliberate low cluster is valid. `lowIntervalStrictness`
(Off / Loose / Strict) + a per-passage bypass — "keep the strain when it's expressive."

### 10.2 Recognition — collapse input ornaments to notation markers

The other half of figuration handling. If the **input already contains** a trill (rapid fixed
alternation), a measured tremolo, or a grace-note group, the planning engine (pass 7) recognises
it and emits **two notes + a notation marker to the sidecar** (`bar:trill`, `bar:tremolo`,
`bar:acciaccatura`) instead of 32 individual notes clogging a voice. Cleaner in Dorico, and it
frees a voice slot. `ornamentRecognition` (bool, planning only).

---

## 11. Pedal & sustain — two taps, the notation tap never sustains

The audio-path / notation-path split (`project_orch_system_ecosystem`: "two genuinely separate
taps off the same source"). Both taps from **one instance** (§11.2).

- **Notation tap** (ch 1–4 → OrchCapture → Dorico): monophonic lines **damped on next attack**
  (OrchHarp's `dampSuccessive`) → clean successive rhythm. **Genuine long input notes respected in
  Repair/Reduce** — a string whole-note stays a whole-note; only *pedal-generated* sustain is
  stripped. The pedal is a **`bar:Ped` / `bar:*` sidecar-marker lane** (OrchHarp Phase-2b
  mechanism) → Dorico pedal lines, zero rhythmic pollution.
- **Performance tap** (ch 5–8, optional): real CC64 + sustained note-offs, so the sample library
  *sounds* pedalled. The "feed a monophonic line, pedal makes it audibly harmonic" case lives
  entirely here. Chordal formations stay as written (intentional harmony); the pedal only blurs the
  single-line material into resonance.

### 11.1 `pedalMode`

| Mode | Rule |
|---|---|
| **Off** | no pedal, no markers |
| **Manual** | user draws pedal regions (CC / automation / keyswitch) |
| **Follow Harmony** | lift + re-press on MC 2-CC pitch-field-broadcast change (OrchHarp Phase-2d reads the same CCs — no MC change). Rig-integrated. |
| **Follow Bass** | lift + re-press on bass-note change — the traditional rule, good for tonal-ish input |

With the planning engine, harmonic-rhythm detection (§1.1 pass 4) drives Follow-Harmony directly.
Guard: never hold the pedal across a dissonance the field says is *not* a chord change.

### 11.2 `performanceTap` — one instance, second channel block (NOT a build-time role)

The value is "same reduction, two renderings" — note choices **must** be identical. One instance
decides the reduction **once**, then emits ch 1–4 clean + ch 5–8 the *same notes* with
pedal-sustain + optional light humanize when `performanceTap` is on (bool, default off). Two
instances set to different "roles" is **rejected** — they'd drift, and every setting would need
tuning twice.

---

## 12. Voice model & hand split

- **Up to 4 output voices** (`maxVoices` toggle **4 / 6** — §12.1), **2 per hand**, one MIDI
  channel each: ch1 RH-up, ch2 RH-down, ch3 LH-up, ch4 LH-down. Maps 1:1 onto Dorico/Sibelius
  standard piano voicing. Performance tap mirrors this on ch 5–8.
- `maxVoices` is a **ceiling, not a target** — the engine uses the fewest voices the texture needs.
- **Hand assignment**: streaming engine uses `splitNote` (automatable) + **`crossoverSlack`** — a
  voice may cross the split by N semitones before reassignment, so a brief dip below middle C
  doesn't yank the line between hands. **Planning engine** instead sets the split *dynamically* by
  **per-beat pitch clustering** (kernel density estimation on each beat's pitch histogram — the
  natural gap is the split; BERT paper, §14.4), re-fixed per phrase (§1.1 pass 6). `splitNote` is
  the fallback / manual override.
- **Notation workflow** (primary): import the 4 voices as 4 single-line instruments → select all →
  **Write ▸ Reduce** onto one Piano. Dorico distributes voices across the brace and merges
  rhythmically-compatible material. Direct import (ch1–2 → treble, ch3–4 → bass) also works
  (channel layout matches). OrchPiano names the OrchCapture lanes ("OrchPiano — Soprano / Alto /
  Tenor / Bass") so Dorico opens with a labelled roadmap. Sibelius: Reduce plug-in + Arrange.

### 12.1 The 4 / 6 toggle

| | **4 voices (2/hand)** — default "Playable" | **6 voices (3/hand)** — "Analytical" |
|---|---|---|
| Notation | 1:1 with standard piano voicing; cleanest Reduce | 3 voices/staff — hard to read, messy Reduce, more manual cleanup |
| Playability | ≈ what a pianist plays (melody / accomp / inner / bass) | a hand rarely sustains 3 independent rhythms |
| Fidelity | forces a real reduction — the engine must decide what matters | preserves a genuine 5–6-voice fugue or divisi; near-literal |
| Use when | almost always | source is genuinely 5–6 real independent voices **and** you want a study score |

The planning engine manages 6 more coherently than streaming would, but 4 stays the default.

### 12.2 Light voice-leading cleanup — `voiceLeading` (Off / Safe / Full)

**In scope** (mechanical, safe): octave choice minimises the leap from a voice's previous note; no
voice crossing; no adjacent-voice unisons (merge — OrchHarp's exclude-duplicates guard); keyboard
spacing (uppers within an octave, bass gap may open); common tones stay in their voice.

**Out of scope:** parallel-5th/8ve rewriting, suspension/resolution logic, any stylistic judgement
(meaningless for atonal material anyway).

Repair/Reduce → **Safe** default (crossings + collisions only, keeps your pitches).
Transform → **Full** default (also normalises spacing).

---

## 13. Playability constraints (the "stay on the safe side" numbers)

| Constraint | Default | Max | Note |
|---|---|---|---|
| Hand span | 9th (14 st) | 10th (16 st) | matches OrchHarp `maxSpan 16` |
| Notes per hand | **Reduce/Transform: 4** · **Repair: off (12)** | 5 (Reduce) | 5 only with thumb. **Repair must not cap** — real piano writing hits 5–9 notes/hand routinely (empirical §14.5: ~30 % of the Brahms is 5–9 poly) and it is already playable via spread/roll/pedal. The cap is for reducing *orchestral* density, not repairing a piano part. |
| Independent voices per staff | 2 | 3 | 3 forces a merge unless `maxVoices 6` |
| Over-span resolution | Roll | — | never drop the outer voices to fix a span |
| L/R crossover | `crossoverSlack` ~5 st | — | brief dips don't reassign the hand |
| Low intervals | `lowIntervalStrictness = Loose` | — | §10.1 |
| Max ring time | `maxRingBeats = 4` | — | §10.1, BERT paper |
| Lookahead | 8 beats (Repair/Reduce) / 0 (Transform) | ~16 | §1 |
| Difficulty ceiling | `difficultyCeiling` off | — | §7.1, Nakamura constraint framing |

**Hand-transition cost, not just static reach** (Nakamura finding, §14.4): in dense passages the
planning engine scores the *motion* between consecutive hand positions (a reachable chord that
demands an impossible jump from the previous one still fails). Static span is the streaming
engine's only check; the planner adds the transition term. A full vertical+horizontal fingering-
feasibility model is a **v2** refinement.

Black/white-key ergonomics and rapid crossover choreography: **deferred past v1**.

---

## 14. Determinism, the decision log, and how you know it's any good

### 14.1 Deterministic + seeded

See §1.2. Non-negotiable — it's what makes supervise-and-iterate work.

### 14.2 Decision-log sidecar

The planning engine makes every drop / re-voice / octave-move / roll / ornament-collapse choice
explicitly, so it costs almost nothing to write them out. On transport stop, a dedicated
`juce::Thread` (OrchHarp's `MarkerWriter` pattern) writes a plain-text log:

```
bar 12  drop   Ob.2 (octave doubling of Fl.1, pc E)
bar 12  revoice horns close-position under melody (span 34→13)
bar 14  8va    alto voice +12 for the phrase (out of hand range, no collision)
bar 16  roll   11-note chord → RH strum 40ms
bar 18  trill  recognised Vln I measured trill → 2 notes + marker
bar 20  drop   inner A (static pad, pedal covers)
```

You review the log, catch the bad calls, adjust a weight or fix in Dorico. High-value for a
supervised tool. `decisionLog` (bool, default on for Repair/Reduce).

### 14.3 Validation strategy & reference corpus

OrchPiano's rules are far more subjective than OrchHarp's pedal logic — "is this a *good*
reduction?" isn't a unit test. Two layers:

- **Pure-logic checks** (`OrchPianoLogicCheck`, OrchHarp's console-check pattern) for the
  mechanical parts: LIL-table application, span clamp, voice-crossing removal, doubling detection,
  hand-split determinism, `revoice = Framework` never touches a playable inner voice, dynamic
  floor/ceiling.
- **Reference corpus** — pairs of (orchestral score, trusted piano reduction). **The best test set
  is the user's own work**: compose at the piano → orchestrate → (already done by hand for harp)
  reduce. Feed the orchestration to OrchPiano, diff the output against the original piano version.
  Supplement with published vocal-score reductions (functional, plain — not Liszt-style virtuoso
  transcriptions, the wrong target).
- **Prefer MusicXML over MIDI for references.** MusicXML carries the engraver's actual
  `<staff>` and `<voice>` tags — *ground truth* for hand assignment and voice streaming, which
  MIDI cannot give (a MIDI rip like §14.5's Brahms is a single undifferentiated stream). Workflow:
  run OrchPiano on the piece, compare its per-note hand/voice assignment to the reference XML's
  staff/voice → a hard accuracy % per bar. Dorico exports MusicXML from both an orchestral score
  and a reduction, so an **(orchestral MusicXML, reduction MusicXML) pair** is the ideal Reduce-path
  test unit. PDF is not machine-usable without OMR — eyeball reference only.
- **Metrics** (from the literature, §14.4):
  - *Objective:* **pitch-class-histogram similarity in a sliding 2-beat window** (0–1) between
    OrchPiano's output and the reference — the standard automatic metric, cheap, a good regression
    number per section.
  - *Subjective* (1–5, the four standard axes): **Musical Fidelity**, **Performance Difficulty**,
    **Naturalness** (does it read as a real piano score), **Reduction Quality** (how much manual
    post-editing it needs). Score a handful of test pieces by hand each milestone.

### 14.4 Prior art — second mining pass (findings)

The academic literature on automatic piano reduction is small but directly on point. What's
reusable:

**Nakamura & Sagayama** (*Merged-Output HMM*, ICMC 2015 → *Statistical Piano Reduction Controlling
Performance Difficulty*, APSIPA 2018):
- **Framing:** *maximise musical fidelity **subject to a difficulty ceiling***, not a free-floating
  weighted sum. Difficulty is one continuous controllable value. → adopt as an option: a
  `difficultyCeiling` param the planner respects (§7.1), cleaner than only tuning weights.
- Fidelity = a *prior piano-score model* (what real piano scores look like) × an *edit model* (how
  ensembles get edited down). OrchPiano's rule set **is** a hand-built edit model; §14.3's corpus
  is where a learned prior could eventually come from.
- **Key result:** modelling **sequential pitch dependence + fingering *motion*** improves quality
  **specifically in dense/high-difficulty passages** — static span checks aren't enough there.
  Validates the planning engine modelling hand *transitions*, not just per-chord reach (§13).
- Difficulty depends on skill **and tempo** — the same notes are harder faster. A tempo-aware
  strictness is a real refinement (later).

**"Towards Practical Automatic Piano Reduction" (BERT, 2025)** — concrete, borrowable heuristics:
- **Two-step: simplify (per-note keep/discard) → harmonise (fill the voicing back out).** Mirrors
  OrchPiano's drop (§7) then re-voice (§8). Worth keeping the two stages cleanly separate in code.
- **Skyline algorithm** for melody + bass = "structurally essential" — confirms §4.2.
- **Max note duration = 4 beats** (piano tone decays) — a concrete number for `stringResustain` /
  the respect-long-notes rule: re-strike or release anything held longer.
- **Hand separation by per-beat pitch clustering (kernel density estimation)** — the natural gap in
  each beat's pitch histogram is the split point. Better than a fixed `splitNote`; make it the
  planning-engine method, `splitNote` the streaming fallback (§12).
- **> 4 simultaneous pitches per hand → delete doubled notes** — confirms poly cap + drop-doublings-
  first.
- **Range preservation:** RH shouldn't sink below / LH rise above the original's boundaries when
  transposing — bounds §8.3's octave moves.

**Melody-channel selection** (Ozcan, Isikhan & Alpkocak) — picking the melodic channel(s) from a
polyphonic MIDI source: feeds §4.1's `melodyChannels` auto-detect.

**Fingering models** (Nakamura et al. HMM; Balliauw et al. physics-based; pitch-difference match
model) — *vertical cost* (chord stretch) + *horizontal cost* (position transition), DP over a grid
graph. OrchPiano needn't *output* fingerings, but a lightweight **vertical+horizontal feasibility
check** is a stronger playability gate than static span — it catches "playable as a chord, not
reachable in sequence." **v2 refinement.**

**Also:** study **Dorico's own Reduce** behaviour and **music21's** `chordify()` / voice tools to
match the handoff.

### 14.5 Empirical notes — test corpus

Numbers measured from real files, to pin defaults and catch wrong assumptions.

**Brahms, Piano Sonata No. 1 Op. 1 / iv** (`brahms_opus1_4_format0.mid`, piano-midi.de rip;
Type-0, one channel; 5435 notes / 417 s; range MIDI 24–103):

| Measure | Value | Design consequence |
|---|---|---|
| Simultaneous polyphony | mean 3.7, **5–9 notes ~30 % of the time**, max 9 | **Repair must not poly-cap** (§13) — this is playable piano writing, not orchestral density. |
| Onset groups | 2044; 33 % single-note, **50 % have ≥ 3 notes** | plenty of both textures — the homophonic/contrapuntal detector (§3) is exercised hard. |
| Natural hand-split point (biggest internal gap per chord) | median 59, q25 54, q75 66 — **but 39 % of chords split > 7 st from MIDI 60** | a **fixed** `splitNote` is wrong ~2 chords in 5 → this *is* the "OK not great" the user saw. The **per-phrase KDE split** (§12, planning engine) is the fix, and the data quantifies the payoff. |
| Chords entirely above MIDI 67 | 121 | a fixed split at 60 dumps all of these on the right hand (5–8-note treble cluster, empty left). Adaptive split handles it. |
| Top note is also the longest in its group | **4 %** | note length is a bad melody cue for a *fast* movement — the long notes are held pedal tones under the active top line. See §4.2. |
| Track/channel structure | none (Type-0) | worst case for melody/bass ID; the real target (Dorico → MIDI export) carries staff/voice per track and is easier. |

Verdict on MIDI rips as a corpus: fine for early **hand-split** and **melody-salience** tuning,
not for the Reduce path (already-pianistic input) and harder than the real use case (no channels).
For real validation use **MusicXML pairs** — see §14.3 and the format note there.

---

## 15. Parameter surface (first cut, for the design pass)

Grouped the way OrchHarp's editor tabs are:

- **Mode**: `operatingMode` (Repair / Reduce / Transform), `hands` (Both / L / R),
  `lookaheadBeats`, `inputSource` (Direct / OrchCapture merged), `planSeed`
- **Melody/Bass**: `melodyChannels`, `bassChannels`, `melodyOverride`, `bassOverride`
- **Voicing**: `maxVoices` (4/6), `splitNote`, `crossoverSlack`, `voiceLeading` (Off/Safe/Full),
  `maxSpan`, `maxNotesPerHand`, `overSpanMode` (Roll / Drop-inner)
- **Reduction**: `textureWindow`, `revoice` (Off/Framework/Close), importance weights
  (`w_top … w_future`, incl. `w_velocity`), `difficultyCeiling` (off / 0–1), `keepMelodyOctaves`,
  `keepBassOctaves` (Off/Keep/Add), `octaveRunThinning`, `octaveMovePassages`, `staticThreshold`,
  `dynamicContour` (Off/Preserve/Preserve+Mark)
- **Idiom**: `repeatedNoteTremolo`, `arpeggioRespace`, `stringResustain`, `maxRingBeats` (default 4),
  `tremoloStyle`, `glissAsRun`, `ornamentRecognition`, `lowIntervalStrictness` (Off/Loose/Strict),
  per-passage bypass CC
- **Pedal**: `pedalMode` (Off/Manual/Follow Harmony/Follow Bass), `performanceTap` (bool),
  `dampSuccessive` (bool)
- **Output**: `decisionLog` (bool), OrchCapture lane names
- **Field input** (reuse OrchHarp Phase-2d wholesale): `fieldCc` 110, `fieldChannel` 1

### 15.1 OrchCapture dependencies (required before the Reduce workflow works)

1. **Coordinator live merged-tap emit** (§2.1) — expose the merged note stream to a subscriber.
2. **Per-lane constant time-offset compensation** (§1.4) — subtract OrchPiano's reported delay.
3. **Feedback guard** (§2.1) — exclude OrchPiano's own lane from the stream fed back to it.

---

## 16. Explicitly OUT of scope (agreed, keep it honest)

- Fully offline file-in/file-out mode — bounded in-plugin lookahead instead.
- Full contrapuntal part-tracking beyond the lookahead window — offline (music21) or Dorico.
- Deciding *which orchestral lines matter* — the field's quality / user supervision
  (`project_orchharp_phase3_concept`: "subtraction reveals structure, it doesn't invent it").
- Rhythmic augmentation / diminution — MC/ArcSet's job (same call as OrchHarp Phase 3).
- Parallel-motion correction, functional-harmony voice-leading — §12.2.
- Black/white-key fingering ergonomics — post-v1.
- Style-aware figuration (Alberti-ify, stride, etc.) — post-v1, Transform-mode idea.
- Sympathetic-resonance / una-corda modelling for the audio tap — a piano library does its own.
- A dedicated OrchConductor CC slot for piano — decide at rig-integration time (§2.2).

---

## 17. Sources

- Alan Belkin, *Score Reduction* — https://alanbelkinmusic.com/score-reduction/
- Kevin Class, *On the Creation and Performance of Orchestral Reductions* — https://www.kevinclass.com/on-the-creation-and-performance-of-orchestral-reductions
- Kevin Padworski, *The Art of the (Piano) Reduction* (Parts 1 & 2) — https://kevinpadworski.com/2015/08/20/the-art-of-the-piano-reduction/
- tonebase, *How to Play Orchestral Reductions at the Piano* — https://www.tonebase.co/piano-blog-posts/how-to-play-orchestral-reductions-at-the-piano
- *Reduction (music)* — https://en.wikipedia.org/wiki/Reduction_(music)
- Open Music Theory, *Core Principles of Orchestration* — https://viva.pressbooks.pub/openmusictheory/chapter/core-principles-of-orchestration/
- *Low Interval Limits* chart, FunJazz Piano Lessons — https://funnelljazz.eu/wp-content/uploads/2020/12/Low-Interval-Limits.pdf
- Robin Hoffmann, *Low Interval Limits* — https://www.robin-hoffmann.com/dfsb/low-interval-limits/
- Kent Kennan & Donald Grantham, *The Technique of Orchestration* (LIL table, figuration/idiom)
- Samuel Adler, *The Study of Orchestration* (doubling conventions, bass spacing)
- R. O. Morris & Howard Ferguson, *Preparatory Exercises in Score Reading*
- "The Idiomatic Orchestra", *Unisono and Doubling* — https://theidiomaticorchestra.net/unisono-and-doubling/

**Automatic piano reduction — academic (second mining pass):**
- Nakamura & Sagayama, *Automatic Piano Reduction from Ensemble Scores Based on a Merged-Output HMM*, ICMC 2015 — https://eita-nakamura.github.io/articles/Nakamura-Sagayama_AutomaticPianoReduction_ICMC2015.pdf
- Nakamura, Saito & Sagayama, *Statistical Piano Reduction Controlling Performance Difficulty*, APSIPA Trans. 2018 — https://arxiv.org/abs/1808.05006
- *Towards Practical Automatic Piano Reduction using BERT with Semi-supervised Learning*, 2025 — https://arxiv.org/html/2512.21324
- *Automatic System for the Arrangement of Piano Reductions* (Chiu et al.) — IEEE
- Ozcan, Isikhan & Alpkocak, MIDI melody-channel selection from polyphonic sources
- Piano-fingering cost models: Nakamura et al. (HMM); Balliauw et al. (physics/hand-size); *Estimation of Playable Piano Fingering by Pitch-difference Fingering Match Model* — https://arxiv.org/pdf/2108.09058
