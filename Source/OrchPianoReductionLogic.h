#pragma once

#include <array>
#include <string>
#include <vector>

// Pure reduction / voicing logic for OrchPiano. No JUCE, no MIDI buffer, no
// note-tracking, no randomness except a seed passed in. This is the part the
// reduction-logic check tool exercises directly.
//
// Shares DNA with OrchHarp's `ohrp::` Voicing engine (selectVoices: register
// split + polyphony cap + span clamp + function-weighted protect) - that logic
// is identical to what a piano hand needs, so it is ported here near-verbatim.
// Piano-specific additions live alongside it: the low-interval-limit anti-mud
// table, octave folding into a hand range, and left/right hand assignment with
// a crossover dead zone.
//
// Everything here is deterministic, so two plugin instances (a two-hand L/R rig)
// running the same input reach the same decisions without any IPC.
//
// Design: Docs/OrchPiano_Design.md  |  Rules: Docs/OrchPiano_ReductionRules.md
namespace ocpn
{
    // ---- basics --------------------------------------------------------------
    int mod12 (int value) noexcept;
    int clampNote (int note) noexcept;      // -> [0, 127]

    // ---- Low Interval Limits (Kennan, The Technique of Orchestration;
    //      corroborated by Adler and the FunJazz / Robin-Hoffmann charts).
    //      Below the returned bottom note the interval turns to mud and must be
    //      opened, dropped, or octave-shifted up. See ReductionRules §10.1. ----

    enum class LilStrictness
    {
        Off = 0,   // never flag anything
        Loose,     // the table minus a concert-grand tolerance
        Strict     // the table as published
    };

    // Lowest usable bottom MIDI note for an interval of `intervalSemitones`
    // (1 = m2 .. 16 = M10). 0 for the octave (no theoretical limit) and for
    // anything wider than a major tenth.
    int lowIntervalFloorMidi (int intervalSemitones) noexcept;

    // True when `bottomNote`..`topNote` sits below its low-interval limit.
    // `bottomNote` <= `topNote` expected; a non-positive interval is never muddy.
    bool intervalIsMuddy (int bottomNote, int topNote, LilStrictness strictness) noexcept;

    // Concert-grand tolerance subtracted from the table in Loose mode (semitones).
    inline constexpr int kLooseToleranceSemis = 3;

    // ---- octave folding ---------------------------------------------------
    //
    // Move `note` by whole octaves to sit inside [lo, hi]. If the window is
    // narrower than an octave and the note can't land inside, fold it as close
    // as octaves allow, then clamp to the nearer bound. Result stays [0, 127].
    int octaveFoldInto (int note, int lo, int hi) noexcept;

    // ---- Voicing (ported from ohrp::) -----------------------------------
    //
    // Reduce one onset group (a "hand placement") to what one hand could play:
    // a per-hand register slice, a polyphony cap, and a span clamp - the drops
    // weighted by musical function (protect the outer voices, drop from the
    // inside out).

    struct VoiceConfig
    {
        int hand = 0;          // 0 Both, 1 Left, 2 Right
        int splitMode = 1;     // 0 Off (=Both), 1 Block (register), 2 Interlock
        int maxVoices = 4;     // per hand, per onset group
        int maxSpanSemis = 14; // a 9th (piano-safe default; OrchHarp used 16)
        int protect = 3;       // 0 None, 1 KeepLowest, 2 KeepHighest, 3 KeepBothEnds
        int splitNote = 60;    // Block: below -> Left, at/above -> Right. Also
                               // routes a lone note so a monophonic line isn't
                               // dropped wholesale by one instance of a rig.
    };

    // `sortedNotes` ascending, one onset group. Returns the surviving indices
    // into it, ascending. Pure & deterministic.
    std::vector<int> selectVoices (const std::vector<int>& sortedNotes, const VoiceConfig& config);

    // ---- hand assignment ------------------------------------------------
    //
    // Assign each note of an ascending group to a hand: 1 = Left, 2 = Right.
    // A note within `crossoverSlack` semitones of `splitNote` is in the dead
    // zone - there it keeps the hand a nearby note used last (hysteresis via
    // `prevNotes`/`prevHands`, same length, ascending) so a line that brushes
    // the split isn't yanked between hands; with no history it falls back to a
    // plain split at `splitNote`.
    std::vector<int> assignHands (const std::vector<int>& sortedNotes,
                                  int splitNote,
                                  int crossoverSlack,
                                  const std::vector<int>& prevNotes = {},
                                  const std::vector<int>& prevHands = {});

    // ---- Phase 3: roles, importance, function-weighted drop -------------
    //
    // The streaming engine sees one onset group at a time (plus what it emitted
    // for the previous group). So these are the within-group + short-history
    // versions of ReductionRules §4-§7; the full contextual model (does this
    // line become structural later, rhythmic-independence, static-pad detection)
    // is the planning engine, Phase 5.

    enum class Role
    {
        Melody = 0,
        Bass,
        Inner,
        Doubling   // this pitch class already sounds in another octave of the group
    };

    struct ImportanceWeights
    {
        float top          = 1.0f;
        float bottom       = 1.0f;
        float velocity     = 0.5f;
        float charTone     = 0.4f;   // 7ths / 9ths / tritone above the bass
        float motion       = 0.4f;   // stepwise continuation of a previous note
        float doublePenalty = 1.0f;
        float rhythm       = 0.4f;   // an inner note shorter than the group median = an active line (planning engine)
        float staticPad    = 0.4f;   // an inner note much longer than the median = pad fill (planning engine)
    };

    // Index into an ascending onset group that carries the melody. Registral top
    // is the dependable streaming cue (ReductionRules §14.5: "longest note" fired
    // only 4% of the time); a much louder note within an octave below the top
    // overrides. `velocities` parallels `sortedNotes`. Returns -1 for an empty
    // group.
    int melodyIndex (const std::vector<int>& sortedNotes,
                     const std::vector<int>& velocities) noexcept;

    // Lowest sounding note is the bass, for a group of >= 2 notes. -1 otherwise.
    int bassIndex (const std::vector<int>& sortedNotes) noexcept;

    // Per-note role tags for an ascending group. `melodyIdx` / `bassIdx` from the
    // two functions above (-1 = none).
    std::vector<Role> tagRoles (const std::vector<int>& sortedNotes,
                                int melodyIdx, int bassIdx);

    // Per-note importance. `prevKept` = the pitches the engine emitted for the
    // previous onset group (motion term); empty is fine. `durations` (parallel to
    // `sortedNotes`, any consistent unit - only relative size matters) enables
    // the rhythm / static-pad terms; empty (streaming engine) skips them.
    std::vector<double> importanceScores (const std::vector<int>& sortedNotes,
                                          const std::vector<int>& velocities,
                                          const std::vector<Role>& roles,
                                          const std::vector<int>& prevKept,
                                          const ImportanceWeights& weights,
                                          const std::vector<int>& durations = {});

    // Phrase-boundary detection: given ascending onset positions (any unit),
    // returns the indices at which a new phrase starts (index 0 always, plus any
    // onset preceded by a gap >= `gapThreshold`). Used by the planning engine to
    // hold the hand split + hysteresis stable within a phrase.
    std::vector<int> phraseStarts (const std::vector<double>& onsets, double gapThreshold);

    // ---- Phase 5b-2: per-line voice streaming (Phase 6: 3rd line) -------
    //
    // Assign one hand's kept notes (ascending, with durations) to voice line 0
    // (the lead - melody for the RH, bass for the LH), line 1 (secondary
    // inner), or - only when `maxLines` is 3 (the "Max Voices" = 6 ceiling) -
    // line 2 (a second inner line). Each candidate line is added only when the
    // *remaining* notes still show the same need: a held note sitting under a
    // shorter one (`maxDur >= 2*minDur`) among the not-yet-assigned notes, or
    // that line is already sounding (`secondaryActive` / `tertiaryActive`).
    // Otherwise everything stays on line 0 (no spurious rests) - `maxLines` is
    // a ceiling, the engine uses the fewest lines the texture needs, same
    // principle as the `maxVoices` param itself. The processor owns each
    // line's state across groups and passes its last pitch (-1 = inactive)
    // for continuity. `leadIsTop` true for the RH, false for the LH. Returns
    // 0/1/2 per note; never returns 2 when `maxLines < 3`.
    std::vector<int> streamHandVoices (const std::vector<int>& handNotes,
                                       const std::vector<int>& handDurations,
                                       int line0LastPitch,
                                       int line1LastPitch,
                                       int line2LastPitch,
                                       bool secondaryActive,
                                       bool tertiaryActive,
                                       bool leadIsTop,
                                       int maxLines);

    // ---- Phase 5c-2: figuration recognition ---------------------------
    //
    // A measured tremolo or a fast repeated note comes in as many small onset
    // groups; notated it is two notes/chords with a tremolo beam, or one note.
    // Detect the pattern over a run of upcoming groups so the planning engine
    // can collapse it to held notes + a marker.

    enum class FigureType { None = 0, Tremolo, RepeatedNote };

    struct FigureMatch
    {
        FigureType type = FigureType::None;
        int    groups    = 0;     // onset groups the run spans
        double spanBeats = 0.0;   // total musical duration of the run
    };

    // `groupNotes` = a sequence of ascending, de-duplicated pitch lists;
    // `onsets` = their positions (beats), parallel. Detects whether a figure
    // starts at index 0: a run of >= `minGroups` groups at a regular interval
    // <= `maxIntervalBeats`, alternating between two pitch sets A (even) and B
    // (odd). A == B -> RepeatedNote, else Tremolo.
    FigureMatch detectFigure (const std::vector<std::vector<int>>& groupNotes,
                              const std::vector<double>& onsets,
                              double maxIntervalBeats,
                              int minGroups);

    // Estimate 0..1 how hard an n-note chord spanning `spanSemis` is for one
    // hand (streaming proxy: count + span; the planning engine adds a
    // hand-transition term).
    double handDifficulty (int noteCount, int spanSemis) noexcept;

    enum class DropReason { Doubling = 0, OverVoiceBudget, OverSpan, OverDifficulty };

    struct DropRecord
    {
        int note = 0;
        DropReason reason = DropReason::Doubling;
    };

    struct ReduceConfig
    {
        int   maxVoices = 4;          // per hand
        bool  unlimited = false;      // Repair mode: no polyphony cap
        int   maxSpanSemis = 14;
        float difficultyCeiling = 0.0f;   // 0 = off
        bool  keepMelodyOctaves = true;
        int   keepBassOctaves = 1;    // 0 Off, 1 Keep (don't drop existing), 2 Add
    };

    // Reduce one hand's ascending note list (already register-sliced). Parallel
    // `roles` / `importance` for the same sub-list. Never drops a Melody or Bass
    // note. Returns kept indices (ascending); appends what was removed and why to
    // `dropped`. Pure & deterministic.
    std::vector<int> reduceHand (const std::vector<int>& sortedNotes,
                                 const std::vector<Role>& roles,
                                 const std::vector<double>& importance,
                                 const ReduceConfig& config,
                                 std::vector<DropRecord>& dropped);

    // ---- Phase 4: re-voicing + dynamics --------------------------------
    //
    // Applied per hand to the notes reduceHand kept. The streaming engine treats
    // any group of >= 3 simultaneous notes as a chord; the homophonic-vs-
    // contrapuntal classification proper (and figuration substitution, whole-
    // passage octave moves, ornament recognition) needs the lookahead window and
    // is Phase 5.

    enum class Revoice { Off = 0, Framework, Close };

    // Framework: keep the outer notes (lowest + highest) exactly; where an inner
    // note forms a muddy interval below its low-interval limit, fold that inner
    // note up an octave until the stack is clean (or no octave helps). Same
    // count, same outer notes. `sortedNotes` ascending.
    std::vector<int> revoiceFramework (const std::vector<int>& sortedNotes, LilStrictness strictness);

    // Close: outer frame exact; the inner pitch classes re-stacked in close
    // position in the octave below the top note (one instance per pitch class),
    // then LIL-checked against the bass. Returns adjusted pitches, ascending;
    // the count can shrink if two inner pitch classes land on one note.
    std::vector<int> revoiceClose (const std::vector<int>& sortedNotes, LilStrictness strictness);

    // Velocity scale so a thinned chord keeps its perceived energy: from the
    // summed velocities of the kept notes vs the whole original onset group.
    // >= 1.0 (never quieter), capped so a heavy drop can't blow up. Loudness
    // tracks ~sqrt(energy).
    double dynamicRecoveryScale (int sumKeptVelocity, int sumOriginalVelocity) noexcept;

    // ---- Phase 5: adaptive hand split (planning engine) ----------------
    //
    // Where a fixed split point is wrong ~2 chords in 5 (ReductionRules §14.5),
    // the planning engine picks the split per lookahead window from where the
    // notes actually sit: build a smoothed pitch-density curve over the window
    // and take its deepest valley - constrained to +/- `maxDriftSemis` of the
    // `prior` split so a thin or ambiguous window can't swing it wildly. With
    // fewer than 4 window notes, returns `prior` unchanged.
    int kdeHandSplit (const std::vector<int>& windowPitches,
                      int prior,
                      int maxDriftSemis = 9) noexcept;
}
