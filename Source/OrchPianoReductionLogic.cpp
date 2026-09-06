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

    // ---- Phase 3: roles, importance, function-weighted drop -----------

    int melodyIndex (const std::vector<int>& sortedNotes,
                     const std::vector<int>& velocities) noexcept
    {
        const int k = static_cast<int> (sortedNotes.size());
        if (k == 0)
            return -1;

        const int topIdx = k - 1;
        const int topNote = sortedNotes[static_cast<size_t> (topIdx)];
        const int topVel  = topIdx < static_cast<int> (velocities.size())
            ? velocities[static_cast<size_t> (topIdx)] : 100;

        // A note within an octave below the top, at least 25 louder, wins.
        int best = topIdx, bestVel = topVel;
        for (int i = topIdx - 1; i >= 0; --i)
        {
            if (topNote - sortedNotes[static_cast<size_t> (i)] > 12)
                break;
            const int v = i < static_cast<int> (velocities.size())
                ? velocities[static_cast<size_t> (i)] : 100;
            if (v >= topVel + 25 && v > bestVel)
            {
                best = i;
                bestVel = v;
            }
        }
        return best;
    }

    int bassIndex (const std::vector<int>& sortedNotes) noexcept
    {
        return sortedNotes.size() >= 2 ? 0 : -1;
    }

    std::vector<Role> tagRoles (const std::vector<int>& sortedNotes,
                                int melodyIdx, int bassIdx)
    {
        const int k = static_cast<int> (sortedNotes.size());
        std::vector<Role> roles (static_cast<size_t> (std::max (0, k)), Role::Inner);

        if (melodyIdx >= 0 && melodyIdx < k) roles[static_cast<size_t> (melodyIdx)] = Role::Melody;
        if (bassIdx   >= 0 && bassIdx   < k && bassIdx != melodyIdx)
            roles[static_cast<size_t> (bassIdx)] = Role::Bass;

        // Octave / unison doublings: mark the "extra" instance, preferring to
        // keep whichever is Melody or Bass.
        for (int i = 0; i < k; ++i)
        {
            for (int j = i + 1; j < k; ++j)
            {
                const int a = sortedNotes[static_cast<size_t> (i)];
                const int b = sortedNotes[static_cast<size_t> (j)];
                if ((b - a) % 12 != 0)
                    continue;

                const bool jCore = roles[static_cast<size_t> (j)] == Role::Melody
                                || roles[static_cast<size_t> (j)] == Role::Bass;
                const int extra = jCore ? i : j;
                if (roles[static_cast<size_t> (extra)] == Role::Inner)
                    roles[static_cast<size_t> (extra)] = Role::Doubling;
            }
        }

        return roles;
    }

    std::vector<double> importanceScores (const std::vector<int>& sortedNotes,
                                          const std::vector<int>& velocities,
                                          const std::vector<Role>& roles,
                                          const std::vector<int>& prevKept,
                                          const ImportanceWeights& w,
                                          const std::vector<int>& durations)
    {
        const int k = static_cast<int> (sortedNotes.size());
        std::vector<double> out (static_cast<size_t> (std::max (0, k)), 0.0);
        if (k == 0)
            return out;

        const bool haveDur = static_cast<int> (durations.size()) == k;
        double medianDur = 0.0;
        if (haveDur)
        {
            std::vector<int> d = durations;
            std::sort (d.begin(), d.end());
            medianDur = d[static_cast<size_t> (k / 2)];
        }

        double meanVel = 0.0;
        for (int i = 0; i < k; ++i)
            meanVel += (i < static_cast<int> (velocities.size()) ? velocities[static_cast<size_t> (i)] : 100);
        meanVel /= k;

        const int bassNote = sortedNotes[0];

        for (int i = 0; i < k; ++i)
        {
            const size_t si = static_cast<size_t> (i);
            double s = 0.0;

            if (roles[si] == Role::Melody)      s += w.top * 2.0;
            else if (i == k - 1)                s += w.top;

            if (roles[si] == Role::Bass)        s += w.bottom * 2.0;
            else if (i == 0)                    s += w.bottom;

            const double v = (i < static_cast<int> (velocities.size()) ? velocities[si] : 100);
            s += w.velocity * (v - meanVel) / 32.0;

            const int iv = mod12 (sortedNotes[si] - bassNote);
            if (iv == 1 || iv == 2 || iv == 6 || iv == 10 || iv == 11)
                s += w.charTone;

            if (! prevKept.empty())
            {
                int nearest = 1 << 20;
                for (int p : prevKept)
                    nearest = std::min (nearest, std::abs (p - sortedNotes[si]));
                if (nearest > 0 && nearest <= 7)
                    s += w.motion * (1.0 - nearest / 7.0);
            }

            if (roles[si] == Role::Doubling)
                s -= w.doublePenalty;

            // Rhythm / static-pad (planning engine only): an inner voice shorter
            // than the group median is an active line; much longer is pad fill.
            if (haveDur && roles[si] == Role::Inner && medianDur > 0.0)
            {
                const double rel = durations[si] / medianDur;
                if (rel < 0.7)  s += w.rhythm    * (0.7 - rel) / 0.7;
                if (rel > 1.5)  s -= w.staticPad * std::min (1.0, (rel - 1.5) / 1.5);
            }

            out[si] = s;
        }

        return out;
    }

    std::vector<int> phraseStarts (const std::vector<double>& onsets, double gapThreshold)
    {
        std::vector<int> starts;
        for (int i = 0; i < static_cast<int> (onsets.size()); ++i)
        {
            if (i == 0 || onsets[static_cast<size_t> (i)] - onsets[static_cast<size_t> (i - 1)] >= gapThreshold)
                starts.push_back (i);
        }
        return starts;
    }

    std::vector<int> streamHandVoices (const std::vector<int>& handNotes,
                                       const std::vector<int>& handDurations,
                                       int line0LastPitch,
                                       int line1LastPitch,
                                       int line2LastPitch,
                                       bool secondaryActive,
                                       bool tertiaryActive,
                                       bool leadIsTop,
                                       int maxLines)
    {
        const int k = static_cast<int> (handNotes.size());
        std::vector<int> v (static_cast<size_t> (std::max (0, k)), 0);
        if (k == 0)
            return v;

        maxLines = std::clamp (maxLines, 1, 3);

        auto durOf = [&] (int i) {
            return i < static_cast<int> (handDurations.size()) ? handDurations[static_cast<size_t> (i)] : 0;
        };

        // Does the given (still line-0) subset show a held-note-under-a-
        // shorter-one pattern, or is its line already ringing?
        auto needsAnotherLine = [&] (const std::vector<int>& idxs, bool activeFlag)
        {
            if (activeFlag) return true;
            if (static_cast<int> (idxs.size()) < 2) return false;
            int mn = durOf (idxs[0]), mx = mn;
            for (int i : idxs) { mn = std::min (mn, durOf (i)); mx = std::max (mx, durOf (i)); }
            return mn > 0 && mx >= 2 * mn;
        };

        // Pick the note (from candidates) closest to lastPitch; with no prior
        // pitch to judge continuity by, the longest-held candidate instead.
        auto pickVoiceNote = [&] (const std::vector<int>& candidates, int lastPitch)
        {
            int best = -1;
            if (lastPitch >= 0)
            {
                int bestDist = 1 << 20;
                for (int i : candidates)
                {
                    const int d = std::abs (handNotes[static_cast<size_t> (i)] - lastPitch);
                    if (d < bestDist) { bestDist = d; best = i; }
                }
            }
            else
            {
                int bestDur = -1;
                for (int i : candidates) { const int d = durOf (i); if (d > bestDur) { bestDur = d; best = i; } }
            }
            return best;
        };

        std::vector<int> allIdx (static_cast<size_t> (k));
        for (int i = 0; i < k; ++i) allIdx[static_cast<size_t> (i)] = i;
        if (! needsAnotherLine (allIdx, secondaryActive))
            return v;                             // one voice, everything to line 0

        const int leadIdx = leadIsTop ? k - 1 : 0;

        if (k == 1)
        {
            // The lone note continues whichever already-active line it is
            // closer to; ties (or no active alternate line) stay on line 0.
            int bestLine = 0;
            int bestDist = line0LastPitch >= 0 ? std::abs (handNotes[0] - line0LastPitch) : 1 << 20;
            if (line1LastPitch >= 0)
            {
                const int d = std::abs (handNotes[0] - line1LastPitch);
                if (d < bestDist) { bestDist = d; bestLine = 1; }
            }
            if (maxLines >= 3 && line2LastPitch >= 0)
            {
                const int d = std::abs (handNotes[0] - line2LastPitch);
                if (d < bestDist) { bestDist = d; bestLine = 2; }
            }
            v[0] = bestLine;
            return v;
        }

        std::vector<int> nonLead;
        for (int i = 0; i < k; ++i) if (i != leadIdx) nonLead.push_back (i);

        // Pick the secondary note: the non-lead note closest to line 1's last
        // pitch, else the longest-held non-lead note, else the other extreme.
        int secIdx = pickVoiceNote (nonLead, line1LastPitch);
        if (secIdx < 0)
            secIdx = leadIsTop ? 0 : k - 1;
        v[static_cast<size_t> (secIdx)] = 1;

        // A third line only ever comes out of what line 1 didn't already
        // claim, and only when the ceiling allows it.
        if (maxLines >= 3)
        {
            std::vector<int> afterSecondary;
            for (int i : nonLead) if (i != secIdx) afterSecondary.push_back (i);

            if (needsAnotherLine (afterSecondary, tertiaryActive))
            {
                const int terIdx = pickVoiceNote (afterSecondary, line2LastPitch);
                if (terIdx >= 0)
                    v[static_cast<size_t> (terIdx)] = 2;
            }
        }

        return v;                                 // everything else stays on line 0
    }

    double handDifficulty (int noteCount, int spanSemis) noexcept
    {
        const double byCount = std::clamp ((noteCount - 2) / 4.0, 0.0, 1.0);
        const double bySpan  = std::clamp (spanSemis / 16.0, 0.0, 1.0);
        return std::clamp (0.45 * byCount + 0.55 * bySpan, 0.0, 1.0);
    }

    namespace
    {
        bool isOctaveOf (int a, int b) noexcept
        {
            return a != b && (std::abs (a - b) % 12) == 0;
        }
    }

    std::vector<int> reduceHand (const std::vector<int>& sortedNotes,
                                 const std::vector<Role>& roles,
                                 const std::vector<double>& importance,
                                 const ReduceConfig& cfg,
                                 std::vector<DropRecord>& dropped)
    {
        const int k = static_cast<int> (sortedNotes.size());
        std::vector<int> kept;
        for (int i = 0; i < k; ++i)
            kept.push_back (i);
        if (k <= 1)
            return kept;

        auto noteAt      = [&] (int idxInKept) { return sortedNotes[static_cast<size_t> (kept[static_cast<size_t> (idxInKept)])]; };
        auto roleAt      = [&] (int idxInKept) { return roles[static_cast<size_t> (kept[static_cast<size_t> (idxInKept)])]; };
        auto impAt       = [&] (int idxInKept) { return importance[static_cast<size_t> (kept[static_cast<size_t> (idxInKept)])]; };
        auto isProtected = [&] (int idxInKept)
        {
            const auto r = roleAt (idxInKept);
            return r == Role::Melody || r == Role::Bass;
        };

        int melodyNote = -1, bassNote = -1;
        for (int i = 0; i < k; ++i)
        {
            if (roles[static_cast<size_t> (i)] == Role::Melody) melodyNote = sortedNotes[static_cast<size_t> (i)];
            if (roles[static_cast<size_t> (i)] == Role::Bass)   bassNote   = sortedNotes[static_cast<size_t> (i)];
        }

        auto dropAt = [&] (int idxInKept, DropReason why)
        {
            dropped.push_back ({ noteAt (idxInKept), why });
            kept.erase (kept.begin() + idxInKept);
        };

        // --- 1. doublings ---------------------------------------------
        for (int i = static_cast<int> (kept.size()) - 1; i >= 0; --i)
        {
            if (roleAt (i) != Role::Doubling)
                continue;
            const int n = noteAt (i);
            if (cfg.keepMelodyOctaves && melodyNote >= 0 && isOctaveOf (n, melodyNote)) continue;
            if (cfg.keepBassOctaves != 0 && bassNote >= 0 && isOctaveOf (n, bassNote))  continue;
            dropAt (i, DropReason::Doubling);
        }

        auto span = [&] ()
        {
            return kept.empty() ? 0 : noteAt (static_cast<int> (kept.size()) - 1) - noteAt (0);
        };
        auto lowestImportanceUnprotected = [&] () -> int
        {
            int victim = -1;
            double worst = 1e18;
            for (int i = 0; i < static_cast<int> (kept.size()); ++i)
            {
                if (isProtected (i))
                    continue;
                if (impAt (i) < worst) { worst = impAt (i); victim = i; }
            }
            return victim;
        };

        // --- 2. polyphony cap ---------------------------------------
        if (! cfg.unlimited)
        {
            while (static_cast<int> (kept.size()) > std::max (1, cfg.maxVoices))
            {
                const int victim = lowestImportanceUnprotected();
                if (victim < 0) break;
                dropAt (victim, DropReason::OverVoiceBudget);
            }
        }

        // --- 3. span clamp: drop the lower-importance non-protected
        //        note that is a current span extreme. ------------------
        while (span() > cfg.maxSpanSemis && kept.size() > 1)
        {
            const int last = static_cast<int> (kept.size()) - 1;
            int victim = -1;
            double worst = 1e18;
            for (int i : { 0, last })
            {
                if (isProtected (i)) continue;
                if (impAt (i) < worst) { worst = impAt (i); victim = i; }
            }
            if (victim < 0)
                break; // both extremes protected - protect wins over span
            dropAt (victim, DropReason::OverSpan);
        }

        // --- 4. difficulty ceiling --------------------------------
        if (cfg.difficultyCeiling > 0.0f)
        {
            while (kept.size() > 1
                   && handDifficulty (static_cast<int> (kept.size()), span()) > cfg.difficultyCeiling)
            {
                const int victim = lowestImportanceUnprotected();
                if (victim < 0) break;
                dropAt (victim, DropReason::OverDifficulty);
            }
        }

        return kept;
    }

    // ---- Phase 4: re-voicing + dynamics ------------------------------

    std::vector<int> revoiceFramework (const std::vector<int>& sortedNotes, LilStrictness strictness)
    {
        std::vector<int> out = sortedNotes;
        const int k = static_cast<int> (out.size());
        if (k < 3 || strictness == LilStrictness::Off)
            return out;

        const int bottom = out.front();
        const int top    = out.back();

        // Fold each inner note up by octaves until it no longer forms a muddy
        // interval with the bottom, without crossing the top. Output stays
        // index-for-index with the input - the caller pairs them - so it is NOT
        // re-sorted; the outer frame (indices 0 and k-1) is never touched.
        for (int i = 1; i < k - 1; ++i)
        {
            int n = out[static_cast<size_t> (i)];
            int guard = 0;
            while (intervalIsMuddy (bottom, n, strictness) && n + 12 < top && guard++ < 8)
                n += 12;
            out[static_cast<size_t> (i)] = n;
        }

        return out;
    }

    std::vector<int> revoiceClose (const std::vector<int>& sortedNotes, LilStrictness strictness)
    {
        const int k = static_cast<int> (sortedNotes.size());
        if (k < 3)
            return sortedNotes;

        const int bottom = sortedNotes.front();
        const int top    = sortedNotes.back();

        // Outer frame exact; each inner note re-placed in close position stacked
        // downward from just below the top, keeping its own pitch class, kept out
        // of the bass mud. Output stays index-for-index with the input (the
        // caller pairs them and drops any duplicate pitch) - NOT re-sorted.
        std::vector<int> out (static_cast<size_t> (k), 0);
        out.front() = bottom;
        out.back()  = top;

        int ceiling = top - 1;
        for (int i = 1; i < k - 1; ++i)
        {
            const int pc = mod12 (sortedNotes[static_cast<size_t> (i)]);
            int n = ceiling - mod12 (ceiling - pc);   // highest note <= ceiling with this pc
            if (n <= bottom)
                n += 12;
            int guard = 0;
            while (intervalIsMuddy (bottom, n, strictness) && n + 12 < top && guard++ < 8)
                n += 12;
            n = std::clamp (n, bottom + 1, top - 1);
            out[static_cast<size_t> (i)] = n;
            ceiling = n - 1;
            if (ceiling <= bottom)
                ceiling = top - 1;      // out of room - wrap back up (collisions get dropped)
        }

        return out;
    }

    double dynamicRecoveryScale (int sumKeptVelocity, int sumOriginalVelocity) noexcept
    {
        if (sumKeptVelocity <= 0 || sumOriginalVelocity <= sumKeptVelocity)
            return 1.0;
        const double ratio = static_cast<double> (sumOriginalVelocity) / sumKeptVelocity;
        return std::clamp (std::sqrt (ratio), 1.0, 1.6);
    }

    int kdeHandSplit (const std::vector<int>& windowPitches, int prior) noexcept
    {
        if (static_cast<int> (windowPitches.size()) < 4)
            return prior;

        std::vector<int> sortedPitches (windowPitches);
        std::sort (sortedPitches.begin(), sortedPitches.end());
        std::vector<int> distinct (sortedPitches);
        distinct.erase (std::unique (distinct.begin(), distinct.end()), distinct.end());
        if (distinct.size() < 2)
            return prior;

        // A genuine hand-split gap reads narrower than a 3rd in real piano
        // writing almost never - below this, it's more likely spacing within
        // one texture than two hands' worth of separate material.
        constexpr int kMinGapSemis = 4;
        // Guards against one stray outlier note faking a "cluster" on its
        // own and swinging the split to an extreme (e.g. windowPitches =
        // {60,61,62,63,100} - the biggest raw gap sits at the 100, but one
        // note is not a real second hand's worth of content).
        constexpr int kMinNotesPerSide = 2;

        int bestGap = 0, bestMid = prior;
        for (size_t i = 1; i < distinct.size(); ++i)
        {
            const int gap = distinct[i] - distinct[i - 1];
            if (gap < kMinGapSemis)
                continue;
            const int mid = (distinct[i] + distinct[i - 1]) / 2;

            const auto belowCount = std::count_if (sortedPitches.begin(), sortedPitches.end(),
                [mid] (int p) { return p <= mid; });
            const auto aboveCount = static_cast<int> (sortedPitches.size()) - belowCount;
            if (belowCount < kMinNotesPerSide || aboveCount < kMinNotesPerSide)
                continue;

            if (gap > bestGap
                || (gap == bestGap && std::abs (mid - prior) < std::abs (bestMid - prior)))
            {
                bestGap = gap;
                bestMid = mid;
            }
        }

        return bestGap > 0 ? bestMid : prior;
    }

    FigureMatch detectFigure (const std::vector<std::vector<int>>& groupNotes,
                              const std::vector<double>& onsets,
                              double maxIntervalBeats,
                              int minGroups,
                              int maxMurmurSpanSemis)
    {
        FigureMatch m;
        const int n = static_cast<int> (groupNotes.size());
        if (n < std::max (3, minGroups) || static_cast<int> (onsets.size()) != n)
            return m;
        if (groupNotes[0].empty())
            return m;

        const double iv0 = onsets[1] - onsets[0];
        if (iv0 <= 1.0e-6 || iv0 > maxIntervalBeats)
            return m;

        const auto& A = groupNotes[0];
        const auto& B = groupNotes[1];

        int run = 2;
        for (int i = 2; i < n; ++i)
        {
            const double iv = onsets[static_cast<size_t> (i)] - onsets[static_cast<size_t> (i - 1)];
            if (iv <= 1.0e-6 || std::abs (iv - iv0) > 0.35 * iv0)
                break;
            if (groupNotes[static_cast<size_t> (i)] != ((i % 2 == 0) ? A : B))
                break;
            ++run;
        }

        if (run >= minGroups)
        {
            m.groups    = run;
            m.spanBeats = (onsets[static_cast<size_t> (run - 1)] - onsets[0]) + iv0;
            m.type      = (A == B) ? FigureType::RepeatedNote : FigureType::Tremolo;
            return m;
        }

        // Murmur: the strict A-B-A-B alternation above didn't reach minGroups
        // (a genuine narrow-band accompaniment figure rarely repeats exactly
        // two pitch sets - it wanders among 3+ neighbouring pitches). Retry
        // with the same interval-regularity requirement, but track the
        // RUNNING union of every pitch touched so far instead of matching a
        // fixed A/B pair - the run continues as long as that union stays
        // within maxMurmurSpanSemis semitones (a genuinely wide arpeggio that
        // exceeds a hand's span is the separate, not-yet-built
        // arpeggioRespace case, so this stays deliberately narrow).
        if (maxMurmurSpanSemis > 0)
        {
            int lo = A.front(), hi = A.front();
            for (int p : A) { lo = std::min (lo, p); hi = std::max (hi, p); }

            int mrun = 1;
            for (int i = 1; i < n; ++i)
            {
                const double iv = onsets[static_cast<size_t> (i)] - onsets[static_cast<size_t> (i - 1)];
                if (iv <= 1.0e-6 || iv > maxIntervalBeats || std::abs (iv - iv0) > 0.35 * iv0)
                    break;
                const auto& g = groupNotes[static_cast<size_t> (i)];
                if (g.empty())
                    break;

                int newLo = lo, newHi = hi;
                for (int p : g) { newLo = std::min (newLo, p); newHi = std::max (newHi, p); }
                if (newHi - newLo > maxMurmurSpanSemis)
                    break;

                lo = newLo; hi = newHi;
                ++mrun;
            }

            if (mrun >= minGroups)
            {
                std::vector<int> uni;
                for (int i = 0; i < mrun; ++i)
                    for (int p : groupNotes[static_cast<size_t> (i)])
                        uni.push_back (p);
                std::sort (uni.begin(), uni.end());
                uni.erase (std::unique (uni.begin(), uni.end()), uni.end());

                m.groups       = mrun;
                m.spanBeats    = (onsets[static_cast<size_t> (mrun - 1)] - onsets[0]) + iv0;
                m.type         = FigureType::Murmur;
                m.unionPitches = std::move (uni);
            }
        }

        return m;
    }
}
