#include "OrchPianoReductionLogic.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ocpn
{
    // ---- basics -----------------------------------------------------------

    int mod12 (int value) noexcept
    {
        return ((value % 12) + 12) % 12;
    }

    int clampNote (int note) noexcept
    {
        return std::clamp (note, 0, 127);
    }

    // ---- Low Interval Limits --------------------------------------------

    int lowIntervalFloorMidi (int intervalSemitones) noexcept
    {
        // Index 0 unused; 1 = m2 .. 16 = M10. 0 = "no limit".
        // Note names from ReductionRules §10.1, resolved to MIDI (C4 = 60).
        static constexpr std::array<int, 17> table {
            0,   //  0  -
            52,  //  1  m2   E3
            51,  //  2  M2   Eb3
            48,  //  3  m3   C3
            46,  //  4  M3   Bb2
            46,  //  5  P4   Bb2
            46,  //  6  TT   Bb2
            34,  //  7  P5   Bb1
            44,  //  8  m6   Ab2
            43,  //  9  M6   G2
            41,  // 10  m7   F2
            41,  // 11  M7   F2
            0,   // 12  P8   (no theoretical limit)
            40,  // 13  m9   E2
            39,  // 14  M9   Eb2
            36,  // 15  m10  C2
            34   // 16  M10  Bb1
        };

        if (intervalSemitones < 1 || intervalSemitones > 16)
            return 0;
        return table[static_cast<size_t> (intervalSemitones)];
    }

    bool intervalIsMuddy (int bottomNote, int topNote, LilStrictness strictness) noexcept
    {
        if (strictness == LilStrictness::Off)
            return false;

        const int interval = topNote - bottomNote;
        if (interval <= 0 || interval > 16)
            return false;

        const int floorMidi = lowIntervalFloorMidi (interval);
        if (floorMidi <= 0)
            return false; // octave / no limit

        const int effective = strictness == LilStrictness::Loose
            ? floorMidi - kLooseToleranceSemis
            : floorMidi;

        return bottomNote < effective;
    }

    // ---- octave folding ------------------------------------------------

    int octaveFoldInto (int note, int lo, int hi) noexcept
    {
        if (lo > hi)
            std::swap (lo, hi);

        // Pick the octave transposition of `note` whose distance to the window
        // [lo, hi] is smallest (0 if it lands inside), tie-broken toward the
        // smallest transposition. Then clamp - so a window narrower than an
        // octave still resolves to its nearer bound. Deterministic.
        auto windowDist = [lo, hi] (int n) { return n < lo ? lo - n : (n > hi ? n - hi : 0); };

        int best = note, bestDist = windowDist (note), bestK = 0;
        for (int k = 1; k <= 11; ++k)
        {
            for (const int kk : { k, -k })
            {
                const int cand = note + 12 * kk;
                if (cand < 0 || cand > 127)
                    continue;
                const int d = windowDist (cand);
                if (d < bestDist || (d == bestDist && std::abs (kk) < std::abs (bestK)))
                {
                    bestDist = d;
                    best = cand;
                    bestK = kk;
                }
            }
        }

        return clampNote (std::clamp (best, lo, hi));
    }

    // ---- Voicing (ported from ohrp::selectVoices) ----------------------

    std::vector<int> selectVoices (const std::vector<int>& sortedNotes, const VoiceConfig& config)
    {
        const int k = static_cast<int> (sortedNotes.size());
        if (k == 0)
            return {};

        // --- 1. hand slice ----------------------------------------------
        std::vector<int> kept;
        const bool both = config.hand == 0 || config.splitMode == 0;
        const bool wantLeft = config.hand == 1;

        if (both)
        {
            for (int i = 0; i < k; ++i)
                kept.push_back (i);
        }
        else if (k == 1)
        {
            const bool noteIsLeft = sortedNotes[0] < config.splitNote;
            if (noteIsLeft == wantLeft)
                kept.push_back (0);
        }
        else if (config.splitMode == 2) // Interlock
        {
            const int parity = wantLeft ? 0 : 1; // left = even from the bottom
            for (int i = 0; i < k; ++i)
                if (i % 2 == parity)
                    kept.push_back (i);
        }
        else // Block: partition by register, not by count
        {
            for (int i = 0; i < k; ++i)
            {
                const bool noteIsLeft = sortedNotes[static_cast<size_t> (i)] < config.splitNote;
                if (noteIsLeft == wantLeft)
                    kept.push_back (i);
            }
        }

        if (kept.empty())
            return kept;

        auto isProtected = [&] (int idxInKept)
        {
            if (kept.size() <= 1) return true;
            const bool first = idxInKept == 0;
            const bool last  = idxInKept == static_cast<int> (kept.size()) - 1;
            switch (config.protect)
            {
                case 1:  return first;
                case 2:  return last;
                case 3:  return first || last;
                default: return false;
            }
        };

        auto medianNote = [&] ()
        {
            const int lo = sortedNotes[static_cast<size_t> (kept.front())];
            const int hi = sortedNotes[static_cast<size_t> (kept.back())];
            return (lo + hi) / 2;
        };

        // --- 2. polyphony cap: drop the note nearest the median, skipping
        //        protected ends, until we fit. -------------------------
        while (static_cast<int> (kept.size()) > std::max (1, config.maxVoices))
        {
            const int mid = medianNote();
            int victim = -1, victimDist = 1 << 20;
            for (int i = 0; i < static_cast<int> (kept.size()); ++i)
            {
                if (isProtected (i))
                    continue;
                const int d = std::abs (sortedNotes[static_cast<size_t> (kept[static_cast<size_t> (i)])] - mid);
                if (d < victimDist) { victimDist = d; victim = i; }
            }
            if (victim < 0)
                break;
            kept.erase (kept.begin() + victim);
        }

        // --- 3. span clamp: drop the surviving note furthest from the
        //        protected anchor, until the span fits (protect wins). ---
        auto span = [&] ()
        {
            return sortedNotes[static_cast<size_t> (kept.back())]
                 - sortedNotes[static_cast<size_t> (kept.front())];
        };

        while (span() > config.maxSpanSemis && kept.size() > 1)
        {
            int anchor = medianNote();
            if (config.protect == 1) anchor = sortedNotes[static_cast<size_t> (kept.front())];
            if (config.protect == 2) anchor = sortedNotes[static_cast<size_t> (kept.back())];

            int victim = -1, victimDist = -1;
            for (int i = 0; i < static_cast<int> (kept.size()); ++i)
            {
                if (isProtected (i))
                    continue;
                const int d = std::abs (sortedNotes[static_cast<size_t> (kept[static_cast<size_t> (i)])] - anchor);
                if (d > victimDist) { victimDist = d; victim = i; }
            }
            if (victim < 0)
                break;
            kept.erase (kept.begin() + victim);
        }

        return kept;
    }

    // ---- hand assignment ---------------------------------------------

    std::vector<int> assignHands (const std::vector<int>& sortedNotes,
                                  int splitNote,
                                  int crossoverSlack,
                                  const std::vector<int>& prevNotes,
                                  const std::vector<int>& prevHands)
    {
        const int slack = std::max (0, crossoverSlack);
        const bool haveHistory = ! prevNotes.empty()
                              && prevNotes.size() == prevHands.size();

        std::vector<int> hands;
        hands.reserve (sortedNotes.size());

        for (int note : sortedNotes)
        {
            if (note < splitNote - slack)          { hands.push_back (1); continue; }
            if (note >= splitNote + slack)         { hands.push_back (2); continue; }

            // Dead zone: keep the hand the nearest recent note used.
            if (haveHistory)
            {
                int best = -1, bestDist = std::numeric_limits<int>::max();
                for (size_t i = 0; i < prevNotes.size(); ++i)
                {
                    const int d = std::abs (prevNotes[i] - note);
                    if (d < bestDist) { bestDist = d; best = prevHands[i]; }
                }
                if (best == 1 || best == 2) { hands.push_back (best); continue; }
            }

            hands.push_back (note < splitNote ? 1 : 2);
        }

        return hands;
    }
}
