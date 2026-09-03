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
}
