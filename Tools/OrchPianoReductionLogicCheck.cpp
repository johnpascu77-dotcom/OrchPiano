#include "OrchPianoReductionLogic.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    int failures = 0;

    void check (bool condition, const std::string& label)
    {
        if (condition)
        {
            std::cout << "[PASS] " << label << "\n";
        }
        else
        {
            std::cerr << "[FAIL] " << label << "\n";
            ++failures;
        }
    }

    void checkInt (int got, int expected, const std::string& label)
    {
        check (got == expected, label + " (got " + std::to_string (got)
                                + ", expected " + std::to_string (expected) + ")");
    }

    std::string vecToString (const std::vector<int>& v)
    {
        std::string s = "[";
        for (size_t i = 0; i < v.size(); ++i)
            s += (i ? "," : "") + std::to_string (v[i]);
        return s + "]";
    }

    bool eq (const std::vector<int>& a, const std::vector<int>& b) { return a == b; }
}

int main()
{
    using namespace ocpn;

    std::cout << "OrchPianoReductionLogicCheck\n---------------------------\n";

    // --- basics ---------------------------------------------------------
    {
        checkInt (mod12 (-1), 11, "mod12 wraps negatives");
        checkInt (clampNote (200), 127, "clampNote ceiling");
        checkInt (clampNote (-5), 0, "clampNote floor");
    }

    // --- Low Interval Limits ------------------------------------------
    {
        checkInt (lowIntervalFloorMidi (3),  48, "m3 floor = C3 (48)");
        checkInt (lowIntervalFloorMidi (4),  46, "M3 floor = Bb2 (46)");
        checkInt (lowIntervalFloorMidi (7),  34, "P5 floor = Bb1 (34)");
        checkInt (lowIntervalFloorMidi (9),  43, "M6 floor = G2 (43)");
        checkInt (lowIntervalFloorMidi (12), 0,  "octave has no low limit");
        checkInt (lowIntervalFloorMidi (20), 0,  "wider than M10 has no low limit");
        checkInt (lowIntervalFloorMidi (0),  0,  "interval 0 has no limit");

        // Strict: the table as published.
        check (  intervalIsMuddy (45, 48, LilStrictness::Strict), "A2+C3 (m3, bottom below C3) is muddy, Strict");
        check (! intervalIsMuddy (48, 51, LilStrictness::Strict), "C3+Eb3 (m3 at the C3 floor) is not muddy, Strict");
        check (  intervalIsMuddy (36, 40, LilStrictness::Strict), "C2+E2 (M3 way below Bb2) is muddy, Strict");
        check (! intervalIsMuddy (48, 60, LilStrictness::Strict), "an octave low down is never muddy");

        // Loose: table minus the 3-semitone concert-grand tolerance.
        check (! intervalIsMuddy (45, 48, LilStrictness::Loose), "A2+C3 m3 is tolerated in Loose (floor 48-3=45)");
        check (  intervalIsMuddy (44, 47, LilStrictness::Loose), "Ab2+B2 m3 below the Loose floor is still muddy");

        // Off: never flags.
        check (! intervalIsMuddy (24, 27, LilStrictness::Off), "Off strictness never flags mud");

        // Non-positive / inverted interval is never muddy.
        check (! intervalIsMuddy (60, 60, LilStrictness::Strict), "a unison is not an interval to flag");
        check (! intervalIsMuddy (60, 55, LilStrictness::Strict), "inverted args are not muddy");
    }

    // --- octave folding --------------------------------------------
    {
        checkInt (octaveFoldInto (60, 48, 72), 60, "note already in the window is unchanged");
        checkInt (octaveFoldInto (84, 48, 72), 72, "note an octave high folds to the top and clamps");
        checkInt (octaveFoldInto (36, 48, 72), 48, "note an octave low folds up and clamps");
        checkInt (octaveFoldInto (74, 48, 72), 62, "note just above the window folds down one octave");
        // Window narrower than an octave: fold toward the centre, then clamp.
        checkInt (octaveFoldInto (80, 60, 64), 64, "narrow window: fold as close as octaves allow, clamp to bound");
        check (octaveFoldInto (200, 48, 72) <= 127, "octaveFoldInto stays in MIDI range");
    }

    // --- selectVoices (ported ohrp behaviour) ---------------------
    {
        std::vector<int> chord4 { 48, 55, 62, 69 }; // C3 G3 D4 A4, straddles the C4 split

        VoiceConfig block { };
        block.splitMode = 1; block.maxVoices = 4; block.maxSpanSemis = 36; block.splitNote = 60;

        block.hand = 1; // Left
        check (eq (selectVoices (chord4, block), { 0, 1 }), "Block split: Left keeps notes below the split");
        block.hand = 2; // Right
        check (eq (selectVoices (chord4, block), { 2, 3 }), "Block split: Right keeps notes at/above the split");

        // A lone note is routed by register, not dropped by one instance.
        block.hand = 1;
        check (eq (selectVoices ({ 55 }, block), { 0 }), "lone note below split -> Left keeps it");
        check (selectVoices ({ 72 }, block).empty(),      "lone note above split -> Left drops it");

        block.hand = 0; // Both ignores splitMode
        check (selectVoices (chord4, block).size() == 4, "hand=Both keeps the whole group");

        // Poly cap, protect both ends: drop from the inside.
        std::vector<int> chord5 { 48, 52, 59, 64, 67 };
        VoiceConfig cap { };
        cap.hand = 0; cap.splitMode = 0; cap.maxVoices = 3; cap.maxSpanSemis = 36; cap.protect = 3;
        const auto capped = selectVoices (chord5, cap);
        check (capped.size() == 3 && capped.front() == 0 && capped.back() == 4,
               "poly cap 3, KeepBothEnds: keeps the two ends + one interior " + vecToString (capped));

        // Span clamp: 20-semitone spread, limit 14, both ends protected -> ends win.
        std::vector<int> wide { 48, 55, 60, 68 }; // span 20
        VoiceConfig sp { };
        sp.hand = 0; sp.maxVoices = 12; sp.maxSpanSemis = 14; sp.protect = 3;
        const auto spanned = selectVoices (wide, sp);
        check (spanned.front() == 0 && spanned.back() == 3,
               "span clamp, both ends protected: protect wins, ends kept " + vecToString (spanned));
    }

    // --- assignHands -------------------------------------------------
    {
        check (eq (assignHands ({ 48, 55, 62, 69 }, 60, 0), { 1, 1, 2, 2 }),
               "plain split at 60: below -> L, at/above -> R");

        // Dead zone with no history: fall back to the plain split.
        check (eq (assignHands ({ 58, 62 }, 60, 5), { 1, 2 }),
               "crossover dead zone, no history: split at splitNote");

        // Dead-zone note keeps the hand a nearby recent note used.
        check (eq (assignHands ({ 62 }, 60, 5, { 61 }, { 1 }), { 1 }),
               "dead-zone note follows the nearest recent note's hand (L)");
        check (eq (assignHands ({ 58 }, 60, 5, { 59 }, { 2 }), { 2 }),
               "dead-zone note follows the nearest recent note's hand (R)");

        // Clearly-placed notes ignore history.
        check (eq (assignHands ({ 40, 90 }, 60, 5, { 59 }, { 2 }), { 1, 2 }),
               "notes outside the dead zone ignore history");
    }

    // --- Phase 3: roles ------------------------------------------------
    {
        std::vector<int> g { 48, 55, 60, 64, 72 }; // C3 G3 C4 E4 C5
        std::vector<int> v { 90, 80, 80, 80, 110 };

        checkInt (melodyIndex (g, v), 4, "melody = registral top by default");
        // A much louder note within an octave below the top wins.
        checkInt (melodyIndex ({ 60, 67, 72 }, { 127, 60, 60 }), 0,
                  "melody = a note >=25 louder within an 8ve below the top");
        checkInt (bassIndex (g), 0, "bass = lowest of a multi-note group");
        checkInt (bassIndex ({ 60 }), -1, "no bass for a lone note");

        const auto roles = tagRoles (g, melodyIndex (g, v), bassIndex (g));
        check (roles[4] == Role::Melody, "top tagged Melody");
        check (roles[0] == Role::Bass,   "bottom tagged Bass");
        // 48, 60, 72 are all pitch class 0 - the non-core copies are doublings.
        check (roles[2] == Role::Doubling, "middle C is a doubling of the bass pc");
        check (roles[1] == Role::Inner && roles[3] == Role::Inner, "G3 and E4 are inner voices");
    }

    // --- Phase 3: importance ordering --------------------------------
    {
        std::vector<int> g { 40, 52, 55, 76 };
        std::vector<int> v { 80, 80, 80, 100 };
        const auto roles = tagRoles (g, melodyIndex (g, v), bassIndex (g));
        const auto imp = importanceScores (g, v, roles, {}, ImportanceWeights {});
        // melody (idx3) and bass (idx0) outrank the inner voices (1,2).
        check (imp[3] > imp[1] && imp[3] > imp[2], "melody scores above inner voices");
        check (imp[0] > imp[1] && imp[0] > imp[2], "bass scores above inner voices");
    }

    // --- Phase 3: handDifficulty ------------------------------------
    {
        check (handDifficulty (2, 0)  < 0.05, "2 notes, no span -> trivial");
        check (handDifficulty (6, 16) > 0.95, "6 notes, an octave+ span -> maximal");
        check (handDifficulty (4, 8)  > handDifficulty (3, 8), "more notes -> harder");
        check (handDifficulty (4, 14) > handDifficulty (4, 6), "wider span -> harder");
    }

    // --- Phase 3: reduceHand ---------------------------------------
    {
        // C3 D3 G3 D4 G4 - D4 doubles the inner D3 and is neither a melody nor a
        // bass octave, so it is the first to go.
        std::vector<int> h { 48, 50, 55, 62, 67 };
        std::vector<int> hv { 80, 80, 80, 80, 95 };
        auto roles = tagRoles (h, melodyIndex (h, hv), bassIndex (h));
        auto imp   = importanceScores (h, hv, roles, {}, ImportanceWeights {});

        ReduceConfig cfg;
        cfg.maxVoices = 4; cfg.maxSpanSemis = 24; cfg.keepMelodyOctaves = true; cfg.keepBassOctaves = 1;

        std::vector<DropRecord> dropped;
        auto keep = reduceHand (h, roles, imp, cfg, dropped);
        check (! dropped.empty() && dropped[0].reason == DropReason::Doubling,
               "reduceHand drops a non-octave doubling first");
        check (std::find (keep.begin(), keep.end(), 3) == keep.end(), "the doubled inner D4 is gone");
        // melody (G4, idx4) and bass (C3, idx0) always survive.
        check (std::find (keep.begin(), keep.end(), 0) != keep.end(), "bass kept");
        check (std::find (keep.begin(), keep.end(), 4) != keep.end(), "melody kept");

        // A doubling that IS a melody / bass octave is protected by the flags.
        std::vector<int> h2 { 48, 52, 55, 60, 64 }; // C4 = bass 8ve, E4-ish
        std::vector<int> h2v { 80, 80, 80, 80, 95 };
        auto r2 = tagRoles (h2, melodyIndex (h2, h2v), bassIndex (h2));
        auto i2 = importanceScores (h2, h2v, r2, {}, ImportanceWeights {});
        ReduceConfig keepOct; keepOct.maxVoices = 6; keepOct.maxSpanSemis = 24;
        keepOct.keepMelodyOctaves = true; keepOct.keepBassOctaves = 1;
        std::vector<DropRecord> d2;
        auto k2 = reduceHand (h2, r2, i2, keepOct, d2);
        check (d2.empty(), "keep-octave flags protect melody/bass doublings from the drop");

        // Unlimited (Repair): the poly cap does not fire.
        std::vector<int> big { 36, 40, 43, 48, 52, 55, 60 };
        std::vector<int> bv (big.size(), 80);
        auto br = tagRoles (big, melodyIndex (big, bv), bassIndex (big));
        auto bi = importanceScores (big, bv, br, {}, ImportanceWeights {});
        ReduceConfig rep; rep.unlimited = true; rep.maxVoices = 4; rep.maxSpanSemis = 36;
        std::vector<DropRecord> repDrop;
        auto repKeep = reduceHand (big, br, bi, rep, repDrop);
        bool anyBudget = false;
        for (auto& d : repDrop) anyBudget |= (d.reason == DropReason::OverVoiceBudget);
        check (! anyBudget, "Repair (unlimited): no over-budget drops");

        // difficulty ceiling forces drops.
        ReduceConfig dc; dc.maxVoices = 12; dc.maxSpanSemis = 36; dc.difficultyCeiling = 0.3f;
        std::vector<DropRecord> dcDrop;
        auto dcKeep = reduceHand (big, br, bi, dc, dcDrop);
        check (dcKeep.size() < big.size(), "difficulty ceiling 0.3 forces the hand thinner");
    }

    std::cout << "---------------------------\n";
    if (failures == 0)
    {
        std::cout << "[PASS] OrchPianoReductionLogicCheck passed.\n";
        return 0;
    }

    std::cerr << "[FAIL] " << failures << " failure(s).\n";
    return 1;
}
