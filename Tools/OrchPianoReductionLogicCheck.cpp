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

    // --- Phase 4: re-voicing + dynamics ---------------------------
    {
        // revoiceFramework: a muddy inner interval folds up an octave; the
        // outer frame is untouched.
        std::vector<int> chord { 36, 40, 60 }; // C2 E2 C4 - E2 is a muddy M3 over C2
        auto fw = revoiceFramework (chord, LilStrictness::Strict);
        check (fw.front() == 36 && fw.back() == 60, "revoiceFramework keeps the outer frame exact");
        check (fw[1] > 40, "revoiceFramework lifts the muddy inner E2 up an octave");
        check (! intervalIsMuddy (fw.front(), fw[1], LilStrictness::Strict),
               "revoiceFramework result is no longer muddy at the bottom");

        // Off strictness -> unchanged.
        check (revoiceFramework (chord, LilStrictness::Off) == chord,
               "revoiceFramework is a no-op when strictness is Off");

        // Fewer than 3 notes -> unchanged.
        check (revoiceFramework ({ 40, 47 }, LilStrictness::Strict) == std::vector<int> { 40, 47 },
               "revoiceFramework needs at least 3 notes");

        // revoiceClose: outer frame exact, inners pulled up toward the top.
        std::vector<int> spread { 36, 48, 55, 84 }; // wide open voicing
        auto cl = revoiceClose (spread, LilStrictness::Loose);
        check (cl.front() == 36 && cl.back() == 84, "revoiceClose keeps the outer frame exact");
        check (cl[1] > 48 && cl[2] > 55, "revoiceClose pulls the inner voices up toward the top");
        check (cl.size() == spread.size(), "revoiceClose returns the same count (caller dedupes)");

        // dynamicRecoveryScale
        check (dynamicRecoveryScale (200, 200) == 1.0, "no drop -> scale 1.0");
        check (dynamicRecoveryScale (100, 400) > 1.0 && dynamicRecoveryScale (100, 400) <= 1.6,
               "half the energy dropped -> boost, capped at 1.6");
        check (dynamicRecoveryScale (0, 100) == 1.0, "guard: no kept energy -> 1.0");
        check (dynamicRecoveryScale (300, 200) == 1.0, "never scale below 1.0");
    }

    // --- Phase 5b: rhythm / static-pad importance + phrase starts ---
    {
        std::vector<int> g { 40, 55, 60, 76 };            // bass, 2 inners, melody
        std::vector<int> v (4, 80);
        const auto roles = tagRoles (g, melodyIndex (g, v), bassIndex (g));

        // Inner 55 short, inner 60 very long; the rest median-ish.
        std::vector<int> dur { 100, 30, 400, 100 };
        const auto with    = importanceScores (g, v, roles, {}, ImportanceWeights {}, dur);
        const auto without = importanceScores (g, v, roles, {}, ImportanceWeights {});

        check (with[1] > without[1], "a short inner voice scores higher with the rhythm term");
        check (with[2] < without[2], "a very long inner voice (pad) scores lower with the static term");
        check (with[3] == without[3], "the melody is unaffected by rhythm/static (it is not Inner)");
        check (without == importanceScores (g, v, roles, {}, ImportanceWeights {}, {}),
               "no durations -> rhythm/static terms are skipped");

        // phraseStarts
        std::vector<double> onsets { 0.0, 0.5, 1.0, 4.0, 4.5, 8.0 };
        const auto ps = phraseStarts (onsets, 1.0);   // gap >= 1 beat starts a phrase
        check (eq (ps, { 0, 3, 5 }), "phraseStarts: boundaries at index 0 and after each >=1-beat gap");
    }

    // --- Phase 5b-2: per-line voice streaming (Phase 6: 3rd line) --------
    {
        // A block chord (uniform durations, line 1 idle) -> one voice.
        check (eq (streamHandVoices ({ 60, 64, 67 }, { 100, 100, 100 }, -1, -1, -1, false, false, true, 2),
                   { 0, 0, 0 }), "streamHandVoices: block chord -> all line 0");

        // A held note under a short one -> two voices; the held one is line 1.
        check (eq (streamHandVoices ({ 55, 72 }, { 400, 80 }, -1, -1, -1, false, false, true, 2),
                   { 1, 0 }), "streamHandVoices RH: held lower note -> line 1, moving top -> line 0");
        check (eq (streamHandVoices ({ 48, 64 }, { 80, 400 }, -1, -1, -1, false, false, false, 2),
                   { 0, 1 }), "streamHandVoices LH: bass (lead) -> line 0, held upper -> line 1");

        // Line 1 still sounding -> stay in two voices even for a lone note; it
        // continues whichever line it is nearer.
        check (eq (streamHandVoices ({ 71 }, { 100 }, 72, 55, -1, true, false, true, 2),
                   { 0 }), "streamHandVoices: lone note near line 0 -> line 0 while line 1 holds");
        check (eq (streamHandVoices ({ 56 }, { 100 }, 72, 55, -1, true, false, true, 2),
                   { 1 }), "streamHandVoices: lone note near line 1 -> line 1");

        // No held-note evidence and line 1 idle -> one voice even for a 2-note group.
        check (eq (streamHandVoices ({ 60, 64 }, { 100, 110 }, -1, -1, -1, false, false, true, 2),
                   { 0, 0 }), "streamHandVoices: 2 similar-length notes, line 1 idle -> one voice");

        // maxLines == 2 (the "Max Voices" = 4 ceiling): even with tertiaryActive
        // asking for a 3rd line, the cap refuses it - never returns 2.
        check (eq (streamHandVoices ({ 48, 60, 72 }, { 400, 400, 80 }, -1, -1, -1, false, true, true, 2),
                   { 1, 0, 0 }), "streamHandVoices: maxLines 2 never assigns line 2, even if asked");

        // maxLines == 3 (the "Max Voices" = 6 ceiling), same input, same
        // tertiaryActive=true: now line 2 is available and used - lead (top,
        // short) -> 0, longest-held non-lead -> 1, the one left over -> 2.
        check (eq (streamHandVoices ({ 48, 60, 72 }, { 400, 400, 80 }, -1, -1, -1, false, true, true, 3),
                   { 1, 2, 0 }), "streamHandVoices: maxLines 3 with tertiaryActive uses all 3 lines");

        // maxLines == 3 but the texture doesn't need a 3rd line: after lead +
        // secondary are peeled off, the one remaining note is a lone leftover
        // (no pair to show a held/short split) and line 2 isn't already
        // ringing -> stays at 2 voices, ceiling not target.
        check (eq (streamHandVoices ({ 40, 52, 64, 76 }, { 100, 105, 110, 80 }, -1, -1, -1, true, false, true, 3),
                   { 0, 0, 1, 0 }), "streamHandVoices: maxLines 3, leftover pair has no held/short split -> line 2 unused");

        // maxLines == 3, only 2 notes total: no room for a 3rd line regardless
        // of the ceiling.
        check (eq (streamHandVoices ({ 55, 72 }, { 400, 80 }, -1, -1, -1, false, true, true, 3),
                   { 1, 0 }), "streamHandVoices: maxLines 3 with only 2 notes -> line 2 impossible");

        // maxLines == 3, lone note nearest line 2's last pitch (both line 1 and
        // line 2 already ringing) -> continues line 2.
        check (eq (streamHandVoices ({ 40 }, { 100 }, 72, 55, 41, true, true, true, 3),
                   { 2 }), "streamHandVoices: lone note nearest an already-ringing line 2 -> line 2");
    }

    // --- Phase 5c-2: figuration recognition ---------------------
    {
        // An octave tremolo: {60}, {72}, {60}, {72}, {60}, {72} at a steady 1/8.
        std::vector<std::vector<int>> trem { {60}, {72}, {60}, {72}, {60}, {72} };
        std::vector<double> tro { 0.0, 0.5, 1.0, 1.5, 2.0, 2.5 };
        const auto f1 = detectFigure (trem, tro, 0.6, 4);
        check (f1.type == FigureType::Tremolo, "detectFigure: octave alternation -> Tremolo");
        checkInt (f1.groups, 6, "detectFigure: spans all 6 hits");

        // A repeated note.
        std::vector<std::vector<int>> rep { {64}, {64}, {64}, {64}, {64} };
        std::vector<double> rpo { 0.0, 0.25, 0.5, 0.75, 1.0 };
        check (detectFigure (rep, rpo, 0.4, 4).type == FigureType::RepeatedNote,
               "detectFigure: same note repeated -> RepeatedNote");

        // Too slow -> not a figure.
        std::vector<std::vector<int>> slow { {60}, {72}, {60}, {72} };
        std::vector<double> slo { 0.0, 1.0, 2.0, 3.0 };
        check (detectFigure (slow, slo, 0.4, 4).type == FigureType::None,
               "detectFigure: 1-beat spacing is too slow for a figure");

        // Not enough hits.
        check (detectFigure ({ {60}, {72}, {60} }, { 0.0, 0.25, 0.5 }, 0.4, 4).type == FigureType::None,
               "detectFigure: 3 hits is below minGroups 4");

        // Pattern breaks -> run stops where it broke (still counts if >= minGroups).
        std::vector<std::vector<int>> brk { {60}, {72}, {60}, {72}, {65}, {72} };
        std::vector<double> bro { 0.0, 0.25, 0.5, 0.75, 1.0, 1.25 };
        checkInt (detectFigure (brk, bro, 0.4, 4).groups, 4, "detectFigure: run stops at the first mismatch");

        // 2026-09-06: live-found on a real 6/8 melodic line (decision log,
        // OrchPianoProcessor's call site) - a plain 4-note alternating
        // neighbor-tone figure (an entirely ordinary melodic shape, NOT an
        // orchestral tremolo) matches this same alternating-pitch pattern at
        // the same real-piano-note speed. OrchPianoProcessor.cpp's real call
        // site raised its minGroups from 4 to 8 specifically so a short
        // melodic gesture like this no longer qualifies (a genuine tremolo
        // reduction candidate runs well past a beat of continuous
        // alternation; four notes does not). This is that exact real figure,
        // confirming it correctly falls below the new floor.
        std::vector<std::vector<int>> turn { {73}, {75}, {73}, {75} };   // C#5 ~ D#5, 4 hits
        std::vector<double> tno { 0.0, 0.25, 0.5, 0.75 };
        check (detectFigure (turn, tno, 0.4, 8).type == FigureType::None,
               "detectFigure: a 4-hit melodic turn is NOT a tremolo under the real minGroups=8 floor");

        // 2026-09-06 (Phase 5c-2c): live-found on Grieg's "Morning Mood" - the
        // Cellos play a fast figure oscillating among F#2/G#2/A2/B2 (54/56/
        // 57/59, a 5-semitone span) under a sustained horn pedal. It never
        // strictly alternates between exactly two pitch sets, so it fell
        // through the existing Tremolo/RepeatedNote check untouched and
        // played back as erratic-sounding "semitonal shifting" on piano. The
        // shape below is representative of the real figure (irregular order,
        // narrow band, 8+ hits at a steady interval).
        std::vector<std::vector<int>> murm { {54}, {56}, {57}, {54}, {56}, {59}, {54}, {57} };
        std::vector<double> mo { 0.0, 0.2, 0.4, 0.6, 0.8, 1.0, 1.2, 1.4 };
        const auto fm = detectFigure (murm, mo, 0.4, 8, 7);
        check (fm.type == FigureType::Murmur,
               "detectFigure: a narrow-band non-alternating figure -> Murmur");
        checkInt (fm.groups, 8, "detectFigure: murmur spans all 8 hits");
        check (eq (fm.unionPitches, { 54, 56, 57, 59 }),
               "detectFigure: murmur's held chord is every distinct pitch touched");

        // maxMurmurSpanSemis <= 0 (the default) keeps every existing caller's
        // behaviour unchanged - no Murmur classification is attempted at all.
        check (detectFigure (murm, mo, 0.4, 8).type == FigureType::None,
               "detectFigure: murmur detection is off unless maxMurmurSpanSemis is passed");

        // A genuinely wide spread (here 30 semitones - too wide for Murmur's
        // own 7-semitone gate) that ALSO touches too many distinct pitch
        // classes (5: {4,7,10,6,...} - see below) to read as a broken chord
        // either must fall through as None - free chromatic wandering, not a
        // figure this code has any business reshaping.
        std::vector<std::vector<int>> wide { {40}, {51}, {64}, {41}, {55}, {70}, {42}, {58} };
        std::vector<double> wo { 0.0, 0.2, 0.4, 0.6, 0.8, 1.0, 1.2, 1.4 };
        check (detectFigure (wide, wo, 0.4, 8, 7).type == FigureType::None,
               "detectFigure: a wide spread touching too many pitch classes is NOT arpeggio either");

        // Strict alternation takes priority over Murmur/Arpeggio when both
        // would technically match - a real tremolo shouldn't be reclassified
        // just because maxMurmurSpanSemis happens to be passed in too.
        const auto fPriority = detectFigure (trem, tro, 0.6, 4, 7);
        check (fPriority.type == FigureType::Tremolo,
               "detectFigure: strict alternation wins over murmur/arpeggio when both would match");

        // 2026-09-07 (Phase 5c-2b): live-found on the SAME Grieg passage this
        // memory's Murmur entry already traces - a different bar, this time a
        // fast RH figure spanning 19 semitones (F4..C6) that touches nothing
        // but an F-major triad (F/A/C) in different octaves. Too wide for
        // Murmur's 7-semitone gate, but harmonically narrow (3 pitch classes)
        // - the case Murmur's own header comment named as "the separate,
        // not-yet-built arpeggioRespace case" from the day it was written.
        // Shape below: F5,A5,C6, F5,A4,C5, F5,A5,C6, F4 (9 hits, mirrors the
        // real capture's alternating-octave broken-chord contour).
        std::vector<std::vector<int>> arp {
            {77}, {81}, {84}, {77}, {69}, {72}, {77}, {81}, {84}
        };
        std::vector<double> ao { 0.0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0 };
        const auto fArp = detectFigure (arp, ao, 0.4, 8, 7);
        check (fArp.type == FigureType::Arpeggio,
               "detectFigure: a wide but harmonically narrow broken chord -> Arpeggio");
        checkInt (fArp.groups, 9, "detectFigure: arpeggio spans all 9 hits");

        // arpeggioHomePitch: the occurrence-weighted mean of every touched
        // pitch (77*3 + 81*2 + 84*2 + 69 + 72 = 702, / 9 hits = 78 exactly).
        const int home = arpeggioHomePitch (arp, fArp.groups);
        checkInt (home, 78, "arpeggioHomePitch: occurrence-weighted mean of the whole run");

        // foldNearestOctave: every touched pitch, folded independently
        // toward that single stable home, lands within a tritone of it and
        // keeps its own pitch class - the figure's real harmonic motion
        // (F/A/C moving between onsets) survives, just re-registered.
        bool allNearHome = true, allRightPitchClass = true;
        for (int p : arp[0])   // representative - real callers fold every note of every onset
        {
            const int folded = foldNearestOctave (p, home);
            if (std::abs (folded - home) > 6) allNearHome = false;
            if (mod12 (folded) != mod12 (p)) allRightPitchClass = false;
        }
        std::vector<int> foldedRun;
        for (int i = 0; i < fArp.groups; ++i)
            for (int p : arp[static_cast<size_t> (i)])
            {
                const int folded = foldNearestOctave (p, home);
                foldedRun.push_back (folded);
                if (std::abs (folded - home) > 6) allNearHome = false;
                if (mod12 (folded) != mod12 (p)) allRightPitchClass = false;
            }
        check (allNearHome, "foldNearestOctave: every note lands within a tritone of home");
        check (allRightPitchClass, "foldNearestOctave: pitch classes (harmonic identity) are unchanged");
        check (eq (foldedRun, { 77, 81, 72, 77, 81, 84, 77, 81, 72 }),
               "foldNearestOctave: reproduces the exact re-spaced sequence by hand-computed example");

        // 2026-09-07: an EARLIER design (fold each onset toward whichever
        // candidate was closest to the PREVIOUS respaced note, anchored to
        // the run's own first onset as "home") was tested against this
        // exact shape's real-world counterpart - a genuine Grieg passage,
        // bars 38-39, 25 real onsets - and found to degenerate: because 77
        // (F5) sits exactly one octave from that passage's own leading
        // pickup note (F6/89), EVERY subsequent F occurrence kept folding
        // right back onto that single fixed pitch, collapsing the whole
        // moving broken-chord run into one flat repeated note - confirmed by
        // running the old algorithm against the real captured tick/pitch
        // data, not just reasoning about it (see design doc). The
        // occurrence-weighted whole-run mean used here has no such
        // degenerate attractor - representative excerpt of that same real
        // shape (F/A/C cycling across 3 octaves, home lands mid-register):
        std::vector<std::vector<int>> arpReal {
            { 89 }, { 77, 81, 84 }, { 72, 77, 81 }, { 69, 72, 77 }, { 72, 77, 81 },
            { 77, 81, 84 }, { 72, 77, 81 }, { 69, 72, 77 }, { 65, 69, 72 }, { 69, 72, 77 },
        };
        std::vector<double> arpRealOnsets { 0.0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.25 };
        const auto fArpReal = detectFigure (arpReal, arpRealOnsets, 0.4, 8, 7);
        check (fArpReal.type == FigureType::Arpeggio,
               "detectFigure: the real bars-38-39 shape (leading pickup + cycling triad) -> Arpeggio");
        const int homeReal = arpeggioHomePitch (arpReal, fArpReal.groups);
        int minFolded = 1000, maxFolded = -1000;
        for (int i = 0; i < fArpReal.groups; ++i)
            for (int p : arpReal[static_cast<size_t> (i)])
            {
                const int folded = foldNearestOctave (p, homeReal);
                minFolded = std::min (minFolded, folded);
                maxFolded = std::max (maxFolded, folded);
            }
        check (maxFolded - minFolded <= 12,
               "arpeggioHomePitch+foldNearestOctave: the real passage's folded span fits in one octave "
               "(was 24 semitones raw)");
        // Confirms the degenerate-collapse failure mode is actually gone:
        // NOT every onset folds to the exact same pitch as the first.
        int distinctFolded = 0;
        {
            std::vector<int> seen;
            for (int i = 0; i < fArpReal.groups; ++i)
                for (int p : arpReal[static_cast<size_t> (i)])
                {
                    const int folded = foldNearestOctave (p, homeReal);
                    if (std::find (seen.begin(), seen.end(), folded) == seen.end())
                        seen.push_back (folded);
                }
            distinctFolded = static_cast<int> (seen.size());
        }
        check (distinctFolded >= 3,
               "arpeggioHomePitch+foldNearestOctave: the run keeps real harmonic motion (>=3 distinct "
               "folded pitches), not flattened to one repeated note");
    }

    // --- Phase 5a: adaptive hand split -----------------------------
    // 2026-09-07: rewritten from a smoothed-density valley (search window
    // fixed to +/- a constant of the prior) to the largest real gap between
    // actually-played pitches, with a real cluster on each side - see
    // ocpn::kdeHandSplit's header comment for the live-found bug that
    // motivated this (a fixed-width search window can be blind to a real
    // gap that sits further from the prior than the window reaches).
    {
        // Too little data -> return the prior untouched.
        checkInt (kdeHandSplit ({ 40, 80 }, 60), 60, "kdeHandSplit: <4 notes returns the prior");

        // A clear two-cluster window (a bass around 48, a treble around 72,
        // nothing near 60) -> the split lands in the gap between the two clusters.
        std::vector<int> twoClusters { 45, 46, 48, 48, 50, 70, 72, 72, 74, 76 };
        const int s = kdeHandSplit (twoClusters, 60);
        check (s >= 55 && s <= 65, "kdeHandSplit: split falls in the gap between the two clusters");

        // No real gap anywhere (a single dense low run, largest step is a
        // whole tone) -> no genuine hand-split boundary exists, stay at prior.
        std::vector<int> allLow { 30, 32, 34, 34, 36, 38, 40 };
        const int s2 = kdeHandSplit (allLow, 60);
        checkInt (s2, 60, "kdeHandSplit: no qualifying gap anywhere -> prior unchanged");

        // A window whose gap sits right at the prior keeps the prior.
        std::vector<int> gapAt60 { 52, 53, 55, 56, 64, 65, 67, 68 };
        const int s3 = kdeHandSplit (gapAt60, 60);
        check (std::abs (s3 - 60) <= 3, "kdeHandSplit: a gap at the prior keeps the split near it");

        // One stray outlier note must not fake a "cluster" and swing the
        // split to an extreme - the biggest raw gap sits right next to 100,
        // but a single note there is not a real second hand's worth of content.
        std::vector<int> outlier { 60, 61, 62, 63, 100 };
        const int s4 = kdeHandSplit (outlier, 60);
        checkInt (s4, 60, "kdeHandSplit: a lone outlier note doesn't hijack the split");

        // 2026-09-07: THE REAL BUG - Grieg's "Morning Mood" opening. A
        // sustained E-major wind chord (E2/B2/E3/G#3 = 52/59/64/68, an
        // octave-plus stack) under a flute melody starting an octave above
        // it (76-83). The true gap (68 to 76) sits 8-16 semitones from the
        // default prior (60) - the OLD fixed +/-9 window never reached it,
        // so it could only find a spurious dip INSIDE the chord itself
        // (confirmed live: 3 of the chord's 4 notes ended up sharing the
        // melody's hand and got wiped out by the over-span guard, collapsing
        // a real E-major chord down to a single bass note). The new gap-
        // based search must land the split BETWEEN the chord and the
        // melody, keeping the whole chord together.
        std::vector<int> griegChord { 52, 59, 64, 68 };
        std::vector<int> griegMelody { 76, 78, 80, 81, 83 };
        std::vector<int> griegPhrase = griegChord;
        griegPhrase.insert (griegPhrase.end(), griegMelody.begin(), griegMelody.end());
        const int s5 = kdeHandSplit (griegPhrase, 60);
        check (s5 > 68 && s5 < 76,
               "kdeHandSplit: Grieg opening - splits between the wind chord and the flute melody, "
               "not inside the chord (got " + std::to_string (s5) + ")");
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
