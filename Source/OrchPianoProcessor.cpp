#include "OrchPianoProcessor.h"
#include "OrchPianoEditor.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <utility>

// ============================================================================

// Off-thread writer for the decision-log sidecar. Sleeps until the audio thread
// signals a transport stop, then flushes the take's drop log to a temp file for
// the user to review (which notes were dropped, from which bar, and why).
struct OrchPianoAudioProcessor::LogWriter : juce::Thread
{
    explicit LogWriter (OrchPianoAudioProcessor& ownerIn)
        : juce::Thread ("OrchPianoDecisionLog"), owner (ownerIn) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            wake.wait (-1);
            if (threadShouldExit())
                break;
            owner.writeDecisionFile();
        }
    }

    OrchPianoAudioProcessor& owner;
    juce::WaitableEvent wake;
};

// ============================================================================

OrchPianoAudioProcessor::OrchPianoAudioProcessor()
    : AudioProcessor (BusesProperties()),
      parameters (*this, nullptr, "OrchPianoParameters", createParameterLayout())
{
    operatingModeParam    = parameters.getRawParameterValue ("operatingMode");
    handsParam            = parameters.getRawParameterValue ("hands");
    maxVoicesParam        = parameters.getRawParameterValue ("maxVoices");
    splitNoteParam        = parameters.getRawParameterValue ("splitNote");
    maxNotesPerHandParam  = parameters.getRawParameterValue ("maxNotesPerHand");
    maxSpanParam          = parameters.getRawParameterValue ("maxSpan");
    crossoverSlackParam   = parameters.getRawParameterValue ("crossoverSlack");
    dampSuccessiveParam   = parameters.getRawParameterValue ("dampSuccessive");
    onsetWindowMsParam    = parameters.getRawParameterValue ("onsetWindowMs");
    outChannelBaseParam   = parameters.getRawParameterValue ("outChannelBase");
    lookaheadBeatsParam   = parameters.getRawParameterValue ("lookaheadBeats");
    delayCompensationCcParam = parameters.getRawParameterValue ("delayCompensationCc");
    difficultyCeilingParam = parameters.getRawParameterValue ("difficultyCeiling");
    keepBassOctavesParam  = parameters.getRawParameterValue ("keepBassOctaves");
    keepMelodyOctavesParam = parameters.getRawParameterValue ("keepMelodyOctaves");
    decisionLogParam      = parameters.getRawParameterValue ("decisionLog");
    handVoicesParam       = parameters.getRawParameterValue ("handVoices");
    revoiceParam          = parameters.getRawParameterValue ("revoice");
    lowIntervalStrictnessParam = parameters.getRawParameterValue ("lowIntervalStrictness");
    dynamicContourParam   = parameters.getRawParameterValue ("dynamicContour");
    maxRingBeatsParam     = parameters.getRawParameterValue ("maxRingBeats");
    wMelodyBassParam      = parameters.getRawParameterValue ("wMelodyBass");
    wVelocityParam        = parameters.getRawParameterValue ("wVelocity");
    wDoubleParam          = parameters.getRawParameterValue ("wDouble");
    excludeKsNotesParam   = parameters.getRawParameterValue ("excludeKsNotes");
    ksZoneMinParam        = parameters.getRawParameterValue ("ksZoneMin");
    ksZoneMaxParam        = parameters.getRawParameterValue ("ksZoneMax");
    repeatedNoteTremoloParam = parameters.getRawParameterValue ("repeatedNoteTremolo");

    resetNoteMap();

    logTag = juce::String::toHexString (juce::Random::getSystemRandom().nextInt()).paddedLeft ('0', 8);
    logWriter = std::make_unique<LogWriter> (*this);
    logWriter->startThread();
}

OrchPianoAudioProcessor::~OrchPianoAudioProcessor()
{
    if (logWriter != nullptr)
    {
        logWriter->signalThreadShouldExit();
        logWriter->wake.signal();
        logWriter->stopThread (2000);
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout OrchPianoAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Declaration order == Bitwig remote-control page order, 8 per page.
    params.push_back (std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "operatingMode", 1 }, "Mode",
        juce::StringArray { "Repair", "Reduce", "Transform" }, 1));

    params.push_back (std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "hands", 1 }, "Hands",
        juce::StringArray { "Both", "Left", "Right" }, 0));

    params.push_back (std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "maxVoices", 1 }, "Max Voices",
        juce::StringArray { "4", "6" }, 0));

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "splitNote", 1 }, "Hand Split Note", 0, 127, 60));

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "maxNotesPerHand", 1 }, "Notes / Hand (Reduce)", 2, 8, 4));

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "maxSpan", 1 }, "Max Hand Span (st)", 8, 16, 14));

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "crossoverSlack", 1 }, "Crossover Slack (st)", 0, 12, 5));

    params.push_back (std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "difficultyCeiling", 1 }, "Difficulty Ceiling (0 = off)",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.0f));

    params.push_back (std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "keepBassOctaves", 1 }, "Keep Bass Octaves",
        juce::StringArray { "Off", "Keep", "Add" }, 1));

    params.push_back (std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "keepMelodyOctaves", 1 }, "Keep Melody Octaves", true));

    params.push_back (std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "dampSuccessive", 1 }, "Damp On Next Attack", true));

    params.push_back (std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "decisionLog", 1 }, "Write Decision Log", true));

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "onsetWindowMs", 1 }, "Onset Window (ms)", 5, 200, 90));

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "lookaheadBeats", 1 }, "Lookahead (beats, 0 = live)", 0, 16, 4));

    // Reports the constant lookahead delay downstream (0..16 fits directly as
    // the CC value) so OrchCapture can shift its capture back by the same
    // amount instead of landing `lookaheadBeats` late. Matches OrchCapture's
    // `lookaheadCompensationCc` param default (113). 0 = off.
    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "delayCompensationCc", 1 }, "Delay Compensation CC# (0=off)", 0, 127, 113));

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "outChannelBase", 1 }, "Out Channel Base (voices +0..+5)", 1, 11, 1));

    params.push_back (std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "handVoices", 1 }, "Voices per Hand",
        juce::StringArray { "Auto (streamed)", "1 (clean 2-staff)", "2 (lead + accompaniment)" }, 0));

    params.push_back (std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "revoice", 1 }, "Re-voice",
        juce::StringArray { "Off", "Framework", "Close" }, 1));

    params.push_back (std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "lowIntervalStrictness", 1 }, "Low-Interval Strictness",
        juce::StringArray { "Off", "Loose", "Strict" }, 1));

    params.push_back (std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "dynamicContour", 1 }, "Dynamic Contour",
        juce::StringArray { "Off", "Preserve" }, 1));

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "maxRingBeats", 1 }, "Max Ring (beats, 0 = off)", 0, 8, 4));

    // Advanced - importance weights.
    params.push_back (std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "wMelodyBass", 1 }, "Weight: Melody/Bass",
        juce::NormalisableRange<float> (0.0f, 2.0f, 0.01f), 1.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "wVelocity", 1 }, "Weight: Velocity",
        juce::NormalisableRange<float> (0.0f, 2.0f, 0.01f), 0.5f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "wDouble", 1 }, "Weight: Doubling Penalty",
        juce::NormalisableRange<float> (0.0f, 2.0f, 0.01f), 1.0f));

    // Found 2026-09-05, live, from a full-rig capture: OrchPiano has no
    // concept of keyswitch/articulation-select notes at all - every incoming
    // pitch is treated as real musical content. If any leak through upstream
    // (OrchMerge now has its own per-Sender exclusion, but that's per
    // instrument and this rig uses several different destination zones), the
    // damage is worse than just extra notes: ocpn::bassIndex() picks the
    // single LOWEST pitch in the whole onset group as "the" bass note with
    // no floor at all, so a keyswitch note (always far lower than any real
    // bass pitch) gets misidentified as Bass and PROTECTED - reduceHand()'s
    // poly-cap loop explicitly refuses to drop Melody/Bass-tagged notes and
    // gives up once everything remaining is protected, which is how a
    // configured "Notes / Hand" cap of 4 was observed keeping far more than
    // 4. This is a backstop for the common single-zone case, not a
    // multi-zone solution - a rig with several different per-instrument
    // destination zones still wants OrchMerge's own per-Sender exclusion at
    // each instrument's own zone; this catches whatever gets through anyway.
    // Off by default - existing rigs without this problem see no change.
    params.push_back (std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "excludeKsNotes", 1 }, "Exclude Keyswitch Notes", false));
    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "ksZoneMin", 1 }, "KS Zone Min", 0, 127, 24));
    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "ksZoneMax", 1 }, "KS Zone Max", 0, 127, 35));

    // 2026-09-06 (Phase 5c-2d): a detected RepeatedNote figure is the raw
    // MIDI shape of an orchestral roll (timpani, tremolo strings) - render it
    // as an octave tremolo (confirmed idiomatic against a published piano
    // reduction) instead of a flat sustained hold. On by default; off falls
    // back to the pre-5c-2d behaviour for comparison.
    params.push_back (std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "repeatedNoteTremolo", 1 }, "Roll -> Octave Tremolo", true));

    return { params.begin(), params.end() };
}

void OrchPianoAudioProcessor::prepareToPlay (double newSampleRate, int)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    resetNoteMap();
}

void OrchPianoAudioProcessor::releaseResources() {}
bool OrchPianoAudioProcessor::isBusesLayoutSupported (const BusesLayout&) const { return true; }

bool OrchPianoAudioProcessor::isNoteInKsExclusionZone (int note) const noexcept
{
    if (excludeKsNotesParam == nullptr || excludeKsNotesParam->load() < 0.5f)
        return false;

    int lo = ksZoneMinParam != nullptr ? juce::roundToInt (ksZoneMinParam->load()) : 24;
    int hi = ksZoneMaxParam != nullptr ? juce::roundToInt (ksZoneMaxParam->load()) : 35;
    if (lo > hi)
        std::swap (lo, hi);
    return note >= lo && note <= hi;
}

void OrchPianoAudioProcessor::resetNoteMap()
{
    activeNotes.clear();
    currentGroup.clear();
    planBuf.clear();
    pendingRestrikes.clear();
    resetFigureState();
    planPhraseSplit = -1;
    planLastOnsetPpq = -1.0e18;
    prevGroupNotes.clear();
    prevGroupHands.clear();
    prevKeptNotes.clear();
    resetVoiceLines();
    lastSentDelayCcValue = -1;
    lastDelayCcSentPpq = -1.0e18;
}

void OrchPianoAudioProcessor::resetVoiceLines()
{
    for (auto* l : { rhLine, lhLine })
        for (int i = 0; i < 3; ++i)
            l[i] = {};
}

double OrchPianoAudioProcessor::snapUpToBar (double ppq) const noexcept
{
    const double bpb = juce::jmax (1.0, beatsPerBar);
    const double bar = std::ceil (ppq / bpb - 1.0e-6);   // already-on-a-boundary stays put
    return bar * bpb;
}

void OrchPianoAudioProcessor::resetFigureState()
{
    figureEndPpq = -1.0e18;
    figSetA.clear();
    figSetB.clear();
    figGroupsToEmit = 0;
    currentFigureType = ocpn::FigureType::None;
    figConsumedIdentities.clear();
    pendingHardOffs.clear();
    pendingTremolos.clear();
}

void OrchPianoAudioProcessor::dampAllRinging (juce::MidiBuffer& output, int sample)
{
    for (auto& t : activeNotes)
    {
        if (t.outputNote >= 0)
        {
            output.addEvent (juce::MidiMessage::noteOff (juce::jlimit (1, 16, t.outputChannel), t.outputNote),
                             juce::jmax (0, sample));
            t.outputNote = -2;
        }
    }
}

void OrchPianoAudioProcessor::handleNoteOff (const juce::MidiMessage& message, int sample, juce::MidiBuffer& output)
{
    const int ch = message.getChannel();
    const int note = message.getNoteNumber();

    // 2026-09-06: no longer blindly erases pendingRestrikes by the INPUT
    // note's identity here - live-found bug. When a short input attack gets
    // "promoted" into a continuing held voice (streamHandVoices), that SAME
    // short input still has its own natural, quick note-off arriving on
    // schedule. It correctly does NOT release the (deliberately still-
    // ringing) output note - activeNotes below only erases on an actual
    // match - but the old unconditional erase-by-input-identity ran
    // regardless of that, silently cancelling the just-armed maxRingBeats
    // checkpoint for a note that was about to become a long sustain.
    // Confirmed via the decision log: zero re-strikes fired for a genuine
    // ~55-beat Grieg pedal point despite the checkpoint being scheduled.
    // No explicit cleanup is needed here at all: drainRestrikes already
    // self-terminates a checkpoint correctly the moment activeNotes
    // genuinely no longer contains a live match (proven by every OTHER
    // re-strike in the same take working correctly) - this function only
    // needs to get activeNotes right, which it already does below.

    for (auto it = activeNotes.begin(); it != activeNotes.end(); ++it)
    {
        if (it->channel != ch || it->inputNote != note)
            continue;

        if (it->outputNote >= 0)
            output.addEvent (juce::MidiMessage::noteOff (juce::jlimit (1, 16, it->outputChannel), it->outputNote),
                             juce::jmax (0, sample));
        activeNotes.erase (it);
        return;
    }
}

void OrchPianoAudioProcessor::logEvent (double ppq, const juce::String& text)
{
    const double bar = beatsPerBar > 0.0 ? ppq / beatsPerBar + 1.0 : 1.0;
    const juce::ScopedLock sl (decisionLock);
    if (decisionLines.size() < 8192)
        decisionLines.push_back ("bar " + juce::String (bar, 2) + "   " + text);
}

void OrchPianoAudioProcessor::logDrop (double ppq, const ocpn::DropRecord& d)
{
    static const char* reason[] = { "doubling", "over voice budget", "over span", "over difficulty" };
    logEvent (ppq, "drop     " + juce::MidiMessage::getMidiNoteName (d.note, true, true, 3)
                   + " (" + juce::String (d.note) + ")   " + reason[static_cast<int> (d.reason)]);
}

void OrchPianoAudioProcessor::writeDecisionFile()
{
    std::vector<juce::String> lines;
    {
        const juce::ScopedLock sl (decisionLock);
        lines.swap (decisionLines);
    }
    if (lines.empty())
        return;

    juce::String body;
    body << "OrchPiano decision log - " << juce::Time::getCurrentTime().toString (true, true) << "\n";
    body << lines.size() << " events (drop / revoice / octave+ / dynamics)\n\n";
    for (const auto& l : lines)
        body << l << "\n";

    juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getChildFile ("orchpiano-decisions-" + logTag + ".log")
        .replaceWithText (body);
}

// ---- the core: reduce one onset group ------------------------------------

void OrchPianoAudioProcessor::flushGroup (juce::MidiBuffer& output, int flushSample,
                                          double blockStartPpq, double ppqPerSample)
{
    if (currentGroup.empty())
        return;

    const int sample = juce::jmax (0, flushSample);
    const double groupPpq = juce::jmax (0.0, blockStartPpq + flushSample * ppqPerSample);
    const int priorSplit = splitNoteParam != nullptr ? juce::roundToInt (splitNoteParam->load()) : 60;
    reduceGroup (currentGroup, priorSplit, sample, groupPpq, output);
    currentGroup.clear();
}

void OrchPianoAudioProcessor::reduceGroup (const std::vector<HeldOn>& group,
                                           int splitNote,
                                           int emitSample, double groupPpq,
                                           juce::MidiBuffer& output,
                                           double figureReleasePpq,
                                           bool figureIsRoll)
{
    if (group.empty())
        return;

    const int sample = juce::jmax (0, emitSample);
    const bool isFigure = figureReleasePpq > groupPpq + 1.0e-6;

    // Unique pitches, ascending; keep the loudest onset per pitch.
    std::map<int, HeldOn> byPitch;
    for (const auto& h : group)
    {
        auto e = byPitch.find (h.note);
        if (e == byPitch.end() || h.velocity > e->second.velocity)
            byPitch[h.note] = h;
    }

    std::vector<int> notes, vels, durs;
    notes.reserve (byPitch.size());
    for (const auto& kv : byPitch)
    {
        notes.push_back (kv.first);
        vels.push_back (kv.second.velocity);
        durs.push_back (kv.second.durTicks);
    }
    const bool haveDur = std::any_of (durs.begin(), durs.end(), [] (int d) { return d > 0; });

    const int    mode      = operatingModeParam   != nullptr ? juce::roundToInt (operatingModeParam->load()) : 1;
    const bool   repair    = mode == 0;
    const int    handsMode = handsParam           != nullptr ? juce::roundToInt (handsParam->load()) : 0;
    const int    slack     = crossoverSlackParam  != nullptr ? juce::roundToInt (crossoverSlackParam->load()) : 5;
    const int    perHand   = maxNotesPerHandParam != nullptr ? juce::roundToInt (maxNotesPerHandParam->load()) : 4;
    const int    maxSpan   = maxSpanParam         != nullptr ? juce::roundToInt (maxSpanParam->load()) : 14;
    const int    chanBase  = outChannelBaseParam  != nullptr ? juce::roundToInt (outChannelBaseParam->load()) : 1;
    // "Max Voices" (4/6) is a ceiling on independent notation LINES per hand
    // (2 vs 3) - a different axis from "Notes / Hand (Reduce)" (`perHand`
    // above), which caps chord density within a hand before it's split across
    // however many lines are available here.
    const int    voicesPerHand = (maxVoicesParam != nullptr && juce::roundToInt (maxVoicesParam->load()) == 1) ? 3 : 2;
    const int    handVoices = handVoicesParam     != nullptr ? juce::roundToInt (handVoicesParam->load()) : 0; // 0 Auto, 1 one, 2 two
    const bool   damp      = dampSuccessiveParam  != nullptr && dampSuccessiveParam->load() >= 0.5f;
    const bool   doLog     = decisionLogParam     != nullptr && decisionLogParam->load() >= 0.5f;
    const float  ceiling   = (repair || difficultyCeilingParam == nullptr) ? 0.0f : difficultyCeilingParam->load();
    const int    keepBassO = keepBassOctavesParam != nullptr ? juce::roundToInt (keepBassOctavesParam->load()) : 1;
    const bool   keepMelO  = keepMelodyOctavesParam == nullptr || keepMelodyOctavesParam->load() >= 0.5f;
    const int    revoice   = revoiceParam         != nullptr ? juce::roundToInt (revoiceParam->load()) : 1;
    const bool   dynContour = dynamicContourParam != nullptr && dynamicContourParam->load() >= 0.5f;
    const int    maxRing   = maxRingBeatsParam    != nullptr ? juce::roundToInt (maxRingBeatsParam->load()) : 0;
    const auto   lilStrict = static_cast<ocpn::LilStrictness> (
        lowIntervalStrictnessParam != nullptr ? juce::jlimit (0, 2, juce::roundToInt (lowIntervalStrictnessParam->load())) : 1);

    ocpn::ImportanceWeights w;
    const float wmb = wMelodyBassParam != nullptr ? wMelodyBassParam->load() : 1.0f;
    w.top = wmb; w.bottom = wmb;
    w.velocity = wVelocityParam != nullptr ? wVelocityParam->load() : 0.5f;
    w.doublePenalty = wDoubleParam != nullptr ? wDoubleParam->load() : 1.0f;

    // --- melody/bass identified on the whole group, but roles + importance
    // recomputed PER HAND below (2026-09-07 fix - see the per-hand loop's
    // comment for the real bug this replaced) ---
    const int melIdx  = ocpn::melodyIndex (notes, vels);
    const int bassIdx = ocpn::bassIndex (notes);
    const int groupMelodyPitch = melIdx  >= 0 ? notes[static_cast<size_t> (melIdx)]  : -1;
    const int groupBassPitch   = bassIdx >= 0 ? notes[static_cast<size_t> (bassIdx)] : -1;

    const auto handAssign = ocpn::assignHands (notes, splitNote, slack, prevGroupNotes, prevGroupHands);

    // Auto voice streaming damps per output channel in the emit loop (so a held
    // inner voice survives the melody's next attack); the fixed modes damp
    // globally (successive notes on one voice per hand).
    if (damp && handVoices != 0)
        dampAllRinging (output, sample);

    std::vector<int> keptNotes, keptHands, keptForMotion;
    int emitted = 0, melodyOut = -1, bassOut = -1, droppedCount = 0;

    // Shared across both hands (not reset per hand): each hand's own revoice
    // pass only knows its own frame, so an L-hand note and an R-hand note can
    // independently revoice onto the identical final pitch near the split
    // boundary (crossoverSlack widens exactly this overlap zone). Tracking
    // emitted pitches across the whole group, not just within one hand,
    // catches that case the same way the existing within-hand check catches
    // two inner voices collapsing onto one pitch.
    std::vector<int> emittedPitchesThisGroup;

    for (int hand = 1; hand <= 2; ++hand)
    {
        if (handsMode == 1 && hand != 1) continue;
        if (handsMode == 2 && hand != 2) continue;

        std::vector<int> sub, subVel, subDur;
        for (size_t i = 0; i < notes.size(); ++i)
        {
            if (handAssign[i] != hand) continue;
            sub.push_back (notes[i]);
            subVel.push_back (vels[i]);
            subDur.push_back (durs[i]);
        }
        if (sub.empty())
            continue;

        // 2026-09-07: roles (and therefore importance) are now tagged FRESH
        // on this hand's own note list, not sliced from a role set computed
        // on the whole (both-hands) group before the split. Live-found real
        // bug on Grieg's "Morning Mood": the group-wide melodyIndex() picks
        // the single overall highest note as melody (here, the flute an
        // octave-plus above a sustained accompaniment chord) - correct for
        // THAT note, but tagRoles' doubling pass then ALSO ran across the
        // whole group, and flagged the chord's own 5th as "Doubling" purely
        // because it shared a pitch CLASS with the melody note that was
        // about to leave for the other hand entirely. The 5th then had no
        // octave-doubling protection (unlike the chord's bass-octave note,
        // protected by keepBassOctaves) and was dropped outright - collapsing
        // a genuine chord down to a bare octave. A cross-hand pitch-class
        // coincidence must not penalise a note for "redundancy" within a
        // hand it doesn't even share. Re-deriving roles from ONLY this
        // hand's own notes removes that false doubling entirely; the true
        // melody/bass note (by pitch, found in `sub` if it landed here)
        // still gets its role correctly, so the normal single-hand case is
        // unaffected.
        int subMelIdx  = -1, subBassIdx = -1;
        for (size_t i = 0; i < sub.size(); ++i)
        {
            if (subMelIdx  < 0 && groupMelodyPitch >= 0 && sub[i] == groupMelodyPitch) subMelIdx  = static_cast<int> (i);
            if (subBassIdx < 0 && groupBassPitch   >= 0 && sub[i] == groupBassPitch)   subBassIdx = static_cast<int> (i);
        }
        auto subRoles = ocpn::tagRoles (sub, subMelIdx, subBassIdx);
        auto subImp   = ocpn::importanceScores (sub, subVel, subRoles, prevKeptNotes, w,
                                                haveDur ? subDur : std::vector<int> {});

        // If the group's melody/bass didn't land in this hand, the top/bottom of
        // this hand's slice still gets protected as a local lead/anchor.
        bool hasMelody = false, hasBass = false;
        for (auto r : subRoles) { hasMelody |= (r == ocpn::Role::Melody); hasBass |= (r == ocpn::Role::Bass); }
        if (! hasMelody && hand == 2) subRoles.back()  = ocpn::Role::Melody;
        if (! hasBass   && hand == 1) subRoles.front() = ocpn::Role::Bass;

        ocpn::ReduceConfig cfg;
        cfg.maxVoices = perHand;
        cfg.unlimited = repair;
        cfg.maxSpanSemis = maxSpan;
        cfg.difficultyCeiling = ceiling;
        cfg.keepMelodyOctaves = keepMelO;
        cfg.keepBassOctaves = keepBassO;

        std::vector<ocpn::DropRecord> dropped;
        const auto keep = ocpn::reduceHand (sub, subRoles, subImp, cfg, dropped);

        for (const auto& d : dropped)
        {
            ++droppedCount;
            if (doLog) logDrop (groupPpq, d);
        }
        if (keep.empty())
            continue;

        // --- Phase 4: re-voice this hand's kept notes ---
        std::vector<int> keptPitch;
        int sumKeptVel = 0, sumHandVel = 0;
        for (auto vv : subVel) sumHandVel += vv;
        for (int idx : keep) { keptPitch.push_back (sub[static_cast<size_t> (idx)]); sumKeptVel += subVel[static_cast<size_t> (idx)]; }

        std::vector<int> outPitch = keptPitch;
        if (revoice == 1)      outPitch = ocpn::revoiceFramework (keptPitch, lilStrict);
        else if (revoice == 2) outPitch = ocpn::revoiceClose (keptPitch, lilStrict);

        const double velScale = dynContour ? ocpn::dynamicRecoveryScale (sumKeptVel, sumHandVel) : 1.0;

        if (doLog && velScale > 1.08)
            logEvent (groupPpq, "dynamics x" + juce::String (velScale, 2)
                                + " (hand " + juce::String (hand == 2 ? "R" : "L") + ", thinned chord)");

        auto nn = [] (int n) { return juce::MidiMessage::getMidiNoteName (n, true, true, 3); };

        // --- Phase 5b-2: voice assignment for this hand's kept notes ---
        // Channels: line 0 = the lead (RH melody -> chanBase / LH bass ->
        // chanBase+3); line 1 = the secondary inner voice (RH -> chanBase+1 /
        // LH -> chanBase+2). line 2 (Phase 6, only reachable when "Max Voices"
        // = 6 raises voicesPerHand to 3) = a second inner voice, appended
        // after the original 4 rather than renumbering them, so the +0..+3
        // mapping the Finisher tool and any other downstream consumer keys
        // off of never changes: RH -> chanBase+4, LH -> chanBase+5.
        const int line0Ch = juce::jlimit (1, 16, hand == 2 ? chanBase     : chanBase + 3);
        const int line1Ch = juce::jlimit (1, 16, hand == 2 ? chanBase + 1 : chanBase + 2);
        const int line2Ch = juce::jlimit (1, 16, hand == 2 ? chanBase + 4 : chanBase + 5);
        VoiceLineRT* lines = (hand == 2) ? rhLine : lhLine;

        std::vector<int> keptOut, keptDur;
        for (size_t ki = 0; ki < keep.size(); ++ki)
        {
            keptOut.push_back (ocpn::clampNote (ki < outPitch.size() ? outPitch[ki] : keptPitch[ki]));
            keptDur.push_back (subDur[static_cast<size_t> (keep[ki])]);
        }

        std::vector<int> voice (keptOut.size(), 0);
        if (handVoices == 0)   // Auto - stream
        {
            const bool secActive = lines[1].lastPitch >= 0
                                && groupPpq - lines[1].lastActivePpq < 2.0;
            const bool terActive = lines[2].lastPitch >= 0
                                && groupPpq - lines[2].lastActivePpq < 2.0;
            voice = ocpn::streamHandVoices (keptOut, keptDur,
                                            lines[0].lastPitch, lines[1].lastPitch, lines[2].lastPitch,
                                            secActive, terActive, hand == 2, voicesPerHand);
        }
        else if (handVoices == 2)   // forced positional split
        {
            const int leadKi = (hand == 2) ? static_cast<int> (keptOut.size()) - 1 : 0;
            for (int i = 0; i < static_cast<int> (voice.size()); ++i)
                voice[static_cast<size_t> (i)] = (i == leadKi) ? 0 : 1;
        }
        // handVoices == 1: all voice 0 (default).

        int line0Emit = -1, line1Emit = -1, line2Emit = -1;
        const size_t priorActiveCount = activeNotes.size();   // for per-channel damp

        for (size_t ki = 0; ki < keep.size(); ++ki)
        {
            const int idx = keep[ki];
            const int inPitch = sub[static_cast<size_t> (idx)];
            const int pitch = keptOut[ki];
            const auto& src = byPitch[inPitch];

            // Re-voice can collapse two inner notes onto one pitch (within a
            // hand) or two hands can independently revoice onto the same
            // pitch near the split boundary (across hands) - drop the dup
            // either way, whichever hand got there first keeps it.
            if (std::find (emittedPitchesThisGroup.begin(), emittedPitchesThisGroup.end(), pitch)
                != emittedPitchesThisGroup.end())
            {
                activeNotes.push_back ({ src.channel, inPitch, -1, 0 });
                if (doLog) logEvent (groupPpq, "revoice  " + nn (inPitch) + " folded onto " + nn (pitch));
                continue;
            }
            emittedPitchesThisGroup.push_back (pitch);

            if (doLog && pitch != inPitch)
                logEvent (groupPpq, "revoice  " + nn (inPitch) + " -> " + nn (pitch) + "   "
                          + (revoice == 1 ? "framework: low-interval fix" : "close-position re-stack"));

            const int outCh = (voice[ki] == 2) ? line2Ch : (voice[ki] == 1) ? line1Ch : line0Ch;

            // Damp on next attack: per output channel in Auto (a held inner voice
            // on the other channel is left ringing). Only prior-group notes.
            if (damp && handVoices == 0)
            {
                for (size_t ai = 0; ai < priorActiveCount && ai < activeNotes.size(); ++ai)
                {
                    auto& t = activeNotes[ai];
                    if (t.outputNote >= 0 && t.outputChannel == outCh)
                    {
                        output.addEvent (juce::MidiMessage::noteOff (outCh, t.outputNote), sample);
                        t.outputNote = -2;
                    }
                }
            }

            const int vel = juce::jlimit (1, 127,
                juce::roundToInt (subVel[static_cast<size_t> (idx)] * velScale));

            output.addEvent (juce::MidiMessage::noteOn (outCh, pitch, static_cast<juce::uint8> (vel)), sample);
            // seq: this emission's unique id - see PendingRestrike's comment
            // in the header for why (channel, inputNote, outCh, pitch) alone
            // is not enough to identify a specific occurrence of a repeated note.
            const juce::int64 emitSeq = nextNoteSeq++;
            activeNotes.push_back ({ src.channel, inPitch, pitch, outCh, emitSeq });

            // 2026-09-06 (Phase 5c-2d): a RepeatedNote figure is the raw MIDI
            // shape of an orchestral roll (timpani, tremolo strings) - render
            // it as a genuine octave tremolo (confirmed idiomatic against a
            // published reduction) rather than a flat sustained hold. Owns
            // its whole lifecycle (alternation AND final release) via
            // drainTremolos, so it must NOT also get the plain hard-off or
            // maxRingBeats treatment below - both would fight the same
            // output note on separate schedules. Guarded to a safe top pitch
            // so an unusually high roll can't push the octave-up partner off
            // a practical keyboard range; falls back to the old flat-hold
            // behaviour in that case.
            bool scheduledAsTremolo = false;
            if (isFigure && figureIsRoll)
            {
                constexpr int kTremoloMaxHighPitch = 108;   // top of practical piano range
                constexpr double kTremoloStepBeats = 0.25;  // fixed 16th notes
                if (pitch + 12 <= kTremoloMaxHighPitch)
                {
                    pendingTremolos.push_back ({ outCh, pitch, pitch + 12, vel,
                                                 src.channel, inPitch,
                                                 groupPpq + kTremoloStepBeats, figureReleasePpq,
                                                 kTremoloStepBeats, false });
                    scheduledAsTremolo = true;
                    if (doLog)
                        logEvent (groupPpq, "tremolo  scheduled " + nn (pitch) + " ~ " + nn (pitch + 12)
                                  + "  (to bar-ppq " + juce::String (figureReleasePpq, 2) + ")");
                }
            }

            // A collapsed-figure note holds to figureReleasePpq and has no
            // buffered note-off (it is consumed) - schedule a hard release.
            if (isFigure && ! scheduledAsTremolo)
                pendingHardOffs.push_back ({ outCh, pitch, figureReleasePpq });

            // maxRingBeats: schedule a live check-and-restrike `maxRing` beats
            // from now, unconditionally - NOT gated on this note's known
            // duration. Live-found bug (2026-09-06, Grieg "Morning Mood" via
            // the decision log): the old gate compared subDur (this note's
            // matching note-off found by scanning FORWARD WITHIN THE
            // LOOKAHEAD BUFFER, planBuf) against the ring limit - but a note
            // whose real release sits further ahead than lookaheadBeats
            // simply has NO match in the buffer yet, so subDur reads 0 and
            // the gate silently never opens. That's exactly backwards: the
            // safety net built for "this note is ringing too long" was blind
            // to any note whose true length exceeded the very lookahead
            // window meant to measure it - confirmed live on a genuine
            // ~55-beat sustain (a real Grieg pedal point) that produced zero
            // re-strikes despite maxRingBeats=4. Fixed by not trying to know
            // the total length in advance at all: always arm a checkpoint,
            // and let drainRestrikes decide against LIVE activeNotes state
            // whether the note is still actually ringing when the checkpoint
            // arrives - correct regardless of how long the note turns out to
            // be, lookahead window or not.
            // 2026-09-06: the checkpoint is snapped forward to the next bar
            // boundary rather than left at the raw groupPpq+maxRing offset -
            // see snapUpToBar's header comment. Landing exactly on a downbeat
            // is what makes a re-strike read as a deliberate restatement
            // instead of a stray extra note.
            // A tremolo-scheduled note is already kept "alive" by its own
            // alternation above - an independent maxRingBeats re-strike on
            // the same activeNotes entry would race with it.
            if (maxRing > 0 && ! scheduledAsTremolo)
                pendingRestrikes.push_back ({ outCh, pitch, src.channel, inPitch, vel,
                                              snapUpToBar (groupPpq + maxRing), emitSeq });

            (voice[ki] == 2 ? line2Emit : (voice[ki] == 1 ? line1Emit : line0Emit)) = pitch;

            keptNotes.push_back (pitch);
            keptHands.push_back (hand);
            keptForMotion.push_back (pitch);
            ++emitted;

            if (subRoles[static_cast<size_t> (idx)] == ocpn::Role::Melody) melodyOut = pitch;
            if (subRoles[static_cast<size_t> (idx)] == ocpn::Role::Bass)   bassOut   = pitch;
        }

        if (line0Emit >= 0) { lines[0].lastPitch = line0Emit; lines[0].lastActivePpq = groupPpq; }
        if (line1Emit >= 0) { lines[1].lastPitch = line1Emit; lines[1].lastActivePpq = groupPpq; }
        if (line2Emit >= 0) { lines[2].lastPitch = line2Emit; lines[2].lastActivePpq = groupPpq; }

        // keepBassOctaves == Add: put an octave under the bass if there isn't
        // one already and it doesn't fall below its low-interval limit.
        if (keepBassO == 2 && hand == 1 && ! keep.empty())
        {
            const int bassPitch = sub[static_cast<size_t> (keep.front())];
            const int lower = bassPitch - 12;
            const int lhDownCh = juce::jlimit (1, 16, chanBase + 3);
            bool haveLower = false;
            for (int idx : keep) if (sub[static_cast<size_t> (idx)] == lower) haveLower = true;
            if (! haveLower && lower >= 21
                && ! ocpn::intervalIsMuddy (lower, bassPitch, lilStrict))
            {
                output.addEvent (juce::MidiMessage::noteOn (lhDownCh, lower,
                    static_cast<juce::uint8> (subVel[static_cast<size_t> (keep.front())])), sample);
                activeNotes.push_back ({ byPitch[bassPitch].channel, bassPitch, lower, lhDownCh });
                ++emitted;
                if (doLog) logEvent (groupPpq, "octave+  added " + nn (lower) + " under the bass");
            }
        }
    }

    prevGroupNotes.swap (keptNotes);
    prevGroupHands.swap (keptHands);
    prevKeptNotes.swap (keptForMotion);

    lastSeen.store (static_cast<int> (notes.size()));
    lastKept.store (emitted);
    lastMelody.store (melodyOut);
    lastBass.store (bassOut);
    lastDropped.store (droppedCount);

}

// ---- Phase 5: drain the lookahead buffer -------------------------------

void OrchPianoAudioProcessor::flushPlanBuffer (juce::MidiBuffer& output, double blockStartPpq,
                                               double lookaheadPpq, double ppqPerSample,
                                               int numSamples, int onsetWindowSamples)
{
    // Cap the onset window well below any real gap between distinct chords, so a
    // bad tempo reading on an edge block can't fuse a whole passage into one
    // "chord".
    const double onsetWindowPpq = juce::jlimit (0.01, 0.25, onsetWindowSamples * ppqPerSample);
    const double cutoff = (blockStartPpq + numSamples * ppqPerSample) - lookaheadPpq;
    const bool doLog = decisionLogParam != nullptr && decisionLogParam->load() >= 0.5f;

    auto emitSampleFor = [&] (double ppq)
    {
        if (ppqPerSample <= 0.0)
            return 0;
        return juce::jlimit (0, juce::jmax (0, numSamples - 1),
                             juce::roundToInt ((ppq + lookaheadPpq - blockStartPpq) / ppqPerSample));
    };

    while (! planBuf.empty() && planBuf.front().ppq <= cutoff)
    {
        const auto& front = planBuf.front();

        if (front.msg.isNoteOn())
        {
            const double gp = front.ppq;

            // Phrase boundary? A gap of >= 1 beat with no onset starts a new
            // phrase - recompute the hand split from the phrase's pitches and
            // hold it stable until the next boundary; also re-seed the crossover
            // hysteresis / motion history.
            constexpr double kPhraseGapBeats = 1.0;
            if (planPhraseSplit < 0 || gp - planLastOnsetPpq >= kPhraseGapBeats)
            {
                std::vector<int> phrasePitches;
                double prev = gp;
                for (const auto& e : planBuf)
                {
                    if (! e.msg.isNoteOn() || e.ppq < gp)
                        continue;
                    if (e.ppq - prev >= kPhraseGapBeats && e.ppq > gp)
                        break;                       // next phrase - stop
                    phrasePitches.push_back (e.msg.getNoteNumber());
                    prev = e.ppq;
                }
                const int priorSplit = splitNoteParam != nullptr ? juce::roundToInt (splitNoteParam->load()) : 60;
                planPhraseSplit = ocpn::kdeHandSplit (phrasePitches, priorSplit);
                adaptiveSplit.store (planPhraseSplit);
                prevGroupNotes.clear();
                prevGroupHands.clear();
                prevKeptNotes.clear();
                resetVoiceLines();
            }
            planLastOnsetPpq = gp;

            // Figuration: if not already inside a figure, probe for a tremolo /
            // repeated-note run starting here.
            if (gp >= figureEndPpq - 1.0e-6)
            {
                // 2026-09-07: the onset-grouping window used to bucket note-ons
                // into gN/gO below must NOT be the user's full onsetWindowPpq -
                // that value legitimately goes as high as 0.25 beat (200ms
                // "Onset Window" at a typical tempo) to tolerate real jitter
                // across ~13-25 independently-clocked orchestral Senders when
                // grouping a genuine simultaneous CHORD. But a real repeated-
                // note/tremolo figure's own natural spacing (a 16th note) is
                // ALSO exactly 0.25 beat, tempo-independent (ppq, not ms) - so
                // at the high end of that same slider, this grouping loop
                // stops ever starting a NEW group between consecutive 16th-
                // note hits (gap 0.25 beat, never strictly > a 0.25-beat
                // window) and silently swallows the ENTIRE roll into ONE
                // giant merged onset (gN.size()==1), which detectFigure()
                // immediately rejects (< minGroups) with no figure ever
                // detected - confirmed by simulating this exact algorithm
                // against a real captured Timpani-roll run: detection
                // succeeded at every onsetWindowMs from 20-178 and failed
                // only at 200 (the slider's own max), reproducing a real
                // "octave tremolo just stopped working" report where the
                // build/toggle were both confirmed correct. A user widening
                // Onset Window for an unrelated legitimate reason (sloppy
                // chord jitter) should never silently disable figure
                // detection - cap this specific grouping window well below
                // any real figure's own hit spacing, independent of the
                // user's chord-onset setting.
                constexpr double kFigureGroupOnsetPpqCap = 0.15;
                const double figGroupOnsetPpq = juce::jmin (onsetWindowPpq, kFigureGroupOnsetPpqCap);

                std::vector<std::vector<int>> gN;
                std::vector<double> gO;
                // Parallel to gN (same indices, NOT sorted/deduped like gN
                // below) - which (channel, inputNote) actually produced each
                // pitch. Needed so a figure only ever suppresses the note-offs
                // of the SPECIFIC attacks it consumed - see the note-off
                // handling below for why pitch alone isn't enough.
                std::vector<std::vector<std::pair<int, int>>> gIdent;
                double curPpq = -1.0e18;
                // 2026-09-07: filter to the ANCHOR's own MIDI channel only -
                // planBuf interleaves every instrument in the whole merged
                // orchestra (an OrchMerge tap has ~13-25 independent
                // sources), and this scan previously built gN from the raw,
                // all-instrument stream. Confirmed directly against the
                // real orchestral score: EVERY genuine Timpani roll in this
                // piece has other instruments attacking during it (one has
                // 30 real Violin onsets inside a single 7-beat roll) - any
                // one of those landing inside this scan's own onset window
                // inserted a foreign, unrelated pitch into gN[i], instantly
                // breaking the strict A-B-A-B alternation check the moment
                // it happened, well before minGroups could ever be reached.
                // OrchMerge's own Sender/Hub are a byte-for-byte, channel-
                // preserving pass-through (confirmed by reading that
                // repo's source directly, not assumed - see Phase 5c-2d's
                // own design-doc trace), so a genuine repeating figure's
                // own attacks all share ONE MIDI channel throughout - the
                // anchor onset (this scan's own trigger, `front`/gp) fixes
                // which channel to follow; every other channel's note-ons
                // are skipped entirely for classification purposes (they
                // still get their own ordinary reduceGroup treatment
                // elsewhere, untouched - this only narrows what THIS scan
                // looks at).
                const int figAnchorChannel = front.msg.getChannel();
                for (const auto& e : planBuf)
                {
                    if (! e.msg.isNoteOn() || e.msg.getChannel() != figAnchorChannel)
                        continue;
                    if (e.ppq - curPpq > figGroupOnsetPpq)
                    {
                        if (gN.size() >= 20) break;
                        gN.emplace_back();
                        gIdent.emplace_back();
                        gO.push_back (e.ppq);
                        curPpq = e.ppq;
                    }
                    gN.back().push_back (e.msg.getNoteNumber());
                    gIdent.back().push_back ({ e.msg.getChannel(), e.msg.getNoteNumber() });
                }
                for (auto& g : gN) { std::sort (g.begin(), g.end()); g.erase (std::unique (g.begin(), g.end()), g.end()); }

                constexpr double kMaxFigIntervalBeats = 0.4;   // 16ths / 32nds
                // 2026-09-06: minGroups was 4 - live-found (real 6/8 melodic
                // line, decision log) that a plain 4-note alternating figure
                // (a neighbor-tone turn, an entirely ordinary melodic shape)
                // matches detectFigure's tremolo pattern just as well as a
                // real orchestral string tremolo does, and gets misclassified
                // the same way: held as a sustained dyad, one note (the 3rd)
                // silently eaten (figGroupsToEmit hits 0). This was flagged as
                // a risk from the day this was built ("false-positive on fast
                // scales, needs test") - confirmed live. Genuine tremolo
                // reduction candidates run well past a beat of continuous
                // alternation; a short melodic gesture like a turn or mordent
                // does not. Raised to 8 (>= ~2 beats of alternation at the
                // 0.4-beat max interval) to require that distinction before
                // collapsing anything.
                constexpr int kMinFigureGroups = 8;
                // 2026-09-06 (Phase 5c-2c): narrow-band "murmur" figures (see
                // ocpn::detectFigure's header comment) - a genuinely wide
                // arpeggio that exceeds a hand's span is the separate,
                // not-yet-built arpeggioRespace case, so this stays inside
                // roughly a 5th (7 semitones): wide enough for the real Grieg
                // Cello figure that motivated this (a 5-semitone span), not so
                // wide it starts swallowing a texture that actually needs
                // re-spacing rather than a static held chord.
                constexpr int kMaxMurmurSpanSemis = 7;
                auto fig = ocpn::detectFigure (gN, gO, kMaxFigIntervalBeats, kMinFigureGroups,
                                               kMaxMurmurSpanSemis);
                // 2026-09-07: arpeggioRespace REVERTED live-test - user's own
                // words: "not good at all... all the arpeggios were reduced
                // to repeated notes." The whole premise it was built on was
                // wrong: the user's original "double vision" complaint was
                // about the MusicXML/Dorico ENGRAVING of these dense chords
                // (a Finisher/music21 notation choice), NOT about OrchPiano's
                // actual note content, which the user confirmed was already
                // correct beforehand ("the notes themselves in the previous
                // takes were ok"). Folding every real note toward one home
                // pitch is fundamentally lossy about the passage's own
                // contour - fine on the small hand-picked synthetic/real
                // excerpts this was verified against, but on a real, longer
                // take it can and did flatten genuine moving broken-chord
                // motion into audibly repeated notes. ocpn::detectFigure /
                // arpeggioHomePitch / foldNearestOctave are left in place
                // (tested, inert) for a future NOTATION-ONLY redesign in
                // OrchPianoFinisher instead - this call site simply never
                // acts on an Arpeggio classification.
                if (fig.type == ocpn::FigureType::Arpeggio)
                    fig.type = ocpn::FigureType::None;
                currentFigureType = fig.type;
                if (fig.type != ocpn::FigureType::None)
                {
                    figureEndPpq = gp + fig.spanBeats;
                    // 2026-09-06: record exactly which (channel, inputNote)
                    // attacks were consumed into this figure, spanning only
                    // the CONFIRMED run (fig.groups) - the loop above may have
                    // buffered more groups than actually matched. Live-found
                    // bug: the note-off suppression below used to test pitch
                    // membership in the held chord alone, which for a Murmur
                    // figure's broad multi-pitch union coincidentally matched
                    // a completely unrelated instrument's note-off sharing one
                    // of those pitches (a real Horns A2 pedal note ending
                    // right as a same-pitch Cello murmur figure was active) -
                    // silently swallowing that unrelated note's OWN release
                    // and leaving its output note ringing indefinitely.
                    figConsumedIdentities.clear();
                    for (int gi = 0; gi < fig.groups && gi < static_cast<int> (gIdent.size()); ++gi)
                        for (const auto& id : gIdent[static_cast<size_t> (gi)])
                            figConsumedIdentities.push_back (id);
                    if (fig.type == ocpn::FigureType::Murmur)
                    {
                        figSetA         = fig.unionPitches;   // already sorted + deduped
                        figSetB         = figSetA;
                        figGroupsToEmit = 1;
                    }
                    else if (fig.type == ocpn::FigureType::Arpeggio)
                    {
                        // 2026-09-07 (Phase 5c-2b): unlike Tremolo/Murmur/
                        // RepeatedNote (which COLLAPSE the run - most onsets
                        // are consumed as silent repeats of 1-2 held chords),
                        // Arpeggio must PRESERVE every onset's own real note
                        // COUNT and rhythm - only its REGISTER changes. One
                        // stable "home" pitch computed once for the whole
                        // run; every real note at every onset within it gets
                        // independently octave-folded toward that home at
                        // the reduceGroup() call site below (not collapsed
                        // to a single representative pitch - see
                        // arpeggioHomePitch's header comment for why an
                        // earlier per-onset design was rejected).
                        // figGroupsToEmit = the whole run's length means the
                        // <=0 suppression path below is never taken for any
                        // of its onsets.
                        figArpeggioHome = ocpn::arpeggioHomePitch (gN, fig.groups);
                        figSetA.clear();
                        figSetB.clear();
                        figGroupsToEmit = fig.groups;
                    }
                    else
                    {
                        figSetA         = gN[0];
                        figSetB         = (fig.type == ocpn::FigureType::Tremolo && gN.size() > 1) ? gN[1] : gN[0];
                        figGroupsToEmit = (fig.type == ocpn::FigureType::Tremolo) ? 2 : 1;
                    }
                    if (doLog)
                    {
                        auto setStr = [] (const std::vector<int>& s)
                        {
                            juce::String r;
                            for (int p : s) r << (r.isEmpty() ? "" : "+") << juce::MidiMessage::getMidiNoteName (p, true, true, 3);
                            return r;
                        };
                        if (fig.type == ocpn::FigureType::Arpeggio)
                        {
                            logEvent (gp, "arpeggio respaced around "
                                          + juce::MidiMessage::getMidiNoteName (figArpeggioHome, true, true, 3)
                                          + "  (" + juce::String (fig.groups) + " hits, "
                                          + juce::String (fig.spanBeats, 1) + " beats)");
                        }
                        else
                        {
                            const char* label = fig.type == ocpn::FigureType::Tremolo  ? "tremolo  "
                                              : fig.type == ocpn::FigureType::Murmur   ? "murmur   "
                                                                                        : "repeated ";
                            logEvent (gp, juce::String (label)
                                          + setStr (figSetA)
                                          + (fig.type == ocpn::FigureType::Tremolo ? (" ~ " + setStr (figSetB)) : juce::String())
                                          + "  (" + juce::String (fig.groups) + " hits, "
                                          + juce::String (fig.spanBeats, 1) + " beats) -> held"
                                          + (fig.type == ocpn::FigureType::Murmur ? " chord" : juce::String()));
                        }
                    }
                }
                else
                {
                    figureEndPpq = -1.0e18;
                }
            }

            // Onset group: every note-on within one onset window, wherever it
            // sits in the buffer - NOT just a leading run. planBuf interleaves
            // note-ons and note-offs from every source in arrival order (an
            // orchestral merge has ~13+ independent senders), so an unrelated
            // note ending at nearly the same instant can land between two
            // note-ons that both belong to this same chord/unison. The old
            // version stopped the scan dead at the first non-note-on it saw,
            // silently truncating the group - the rest of a genuinely
            // simultaneous chord then started a BRAND NEW group at nearly the
            // identical ppq, re-emitting the same pitches a second time.
            // Confirmed live: this was the actual cause of "duplicate unison
            // notes", not OrchMerge relay jitter (which was a real, separate,
            // smaller issue, already fixed there).
            std::vector<HeldOn> group;
            std::vector<size_t> interleaved; // non-note-on entries within the window
            size_t n = 0;
            while (n < planBuf.size()
                   && planBuf[n].ppq - gp <= onsetWindowPpq
                   && group.size() < 24)               // no real piano onset is bigger
            {
                const auto& m = planBuf[n].msg;

                if (! m.isNoteOn())
                {
                    interleaved.push_back (n);
                    ++n;
                    continue;
                }

                // Duration: scan forward for this note's matching note-off.
                int durTicks = 0;
                for (size_t j = n + 1; j < planBuf.size(); ++j)
                {
                    const auto& off = planBuf[j].msg;
                    if ((off.isNoteOff() || (off.isNoteOn() && off.getVelocity() == 0))
                        && off.getChannel() == m.getChannel()
                        && off.getNoteNumber() == m.getNoteNumber())
                    {
                        durTicks = juce::jmax (1, juce::roundToInt ((planBuf[j].ppq - planBuf[n].ppq) * 100.0));
                        break;
                    }
                }
                group.push_back ({ m.getChannel(), m.getNoteNumber(), m.getVelocity(), 0, durTicks });
                ++n;
            }

            const bool inFigure = gp < figureEndPpq - 1.0e-6;
            // 2026-09-06: matched by (channel, note) IDENTITY against
            // figConsumedIdentities (every attack captured into the figure at
            // detection time), not by pitch-set membership - see that
            // member's header comment. A pitch-only test couldn't tell "this
            // IS one of the figure's own repeat attacks" from "an unrelated
            // instrument happens to share a pitch with the held chord" -
            // most exploitable by Murmur's broad multi-pitch union, but the
            // same flaw existed for Tremolo/RepeatedNote too, just narrower.
            const bool isFigGroup = inFigure && ! group.empty()
                && std::all_of (group.begin(), group.end(), [this] (const HeldOn& h)
                    {
                        return std::find (figConsumedIdentities.begin(), figConsumedIdentities.end(),
                                          std::make_pair (h.channel, h.note)) != figConsumedIdentities.end();
                    });

            // Apply whatever interleaved (non-note-on) messages were skipped
            // over, in their original order, before the chord they didn't
            // belong to - same handling the outer loop would have given them
            // as their own front() entries.
            for (size_t idx : interleaved)
            {
                const auto& im = planBuf[idx].msg;
                if (im.isNoteOff() || (im.isNoteOn() && im.getVelocity() == 0))
                    handleNoteOff (im, emitSampleFor (planBuf[idx].ppq), output);
                else
                    output.addEvent (im, emitSampleFor (planBuf[idx].ppq));
            }

            if (isFigGroup && figGroupsToEmit <= 0 && currentFigureType != ocpn::FigureType::Arpeggio)
            {
                planBuf.erase (planBuf.begin(), planBuf.begin() + static_cast<long> (n)); // a repeat - consumed
            }
            else
            {
                double releasePpq = 0.0;
                bool figureIsRoll = false;
                if (isFigGroup)
                {
                    // 2026-09-07: Arpeggio never holds to the run's end - see
                    // the group-override just below and the note-off
                    // handling's figOff comment. releasePpq stays 0.0
                    // (isFigure false inside reduceGroup) so this onset's
                    // note gets completely ordinary release treatment, same
                    // as if it weren't part of a figure at all.
                    if (currentFigureType != ocpn::FigureType::Arpeggio)
                        releasePpq = figureEndPpq;
                    // 2026-09-06: gated to a SINGLE repeated pitch only (the
                    // real timpani-roll case) - user's explicit call after a
                    // live-found near-miss: a repeated MULTI-note chord
                    // (figSetA.size() > 1, e.g. a genuine "repeated D#4+E4"
                    // dyad) would otherwise get each of its notes' own
                    // octave-tremolo treatment independently, which can land
                    // squarely on the same pitches as completely unrelated
                    // real content nearby (confirmed live: a genuine
                    // clarinet trill happened to sit exactly at the would-be
                    // octave partners of that dyad - not this feature's own
                    // output, but close enough to raise the exact collision
                    // risk this guard avoids).
                    figureIsRoll = currentFigureType == ocpn::FigureType::RepeatedNote
                        && figSetA.size() == 1
                        && repeatedNoteTremoloParam != nullptr && repeatedNoteTremoloParam->load() >= 0.5f;
                    --figGroupsToEmit;
                }

                // 2026-09-07 (Phase 5c-2b): re-register this onset's own
                // real notes toward the run's stable home register - EVERY
                // real note folded independently (not reduced to one), so a
                // genuine multi-part orchestral doubling survives as
                // multiple (now closely-spaced) real notes. Two notes that
                // happen to fold onto the identical pitch are merged for
                // free by reduceGroup's own existing byPitch "keep the
                // louder one" dedup - the same path any other onset with a
                // coincidental unison already goes through, nothing new
                // needed here.
                if (isFigGroup && currentFigureType == ocpn::FigureType::Arpeggio)
                {
                    for (auto& h : group)
                        h.note = ocpn::clampNote (ocpn::foldNearestOctave (h.note, figArpeggioHome));
                }

                reduceGroup (group, planPhraseSplit, emitSampleFor (gp), gp, output, releasePpq, figureIsRoll);
                planBuf.erase (planBuf.begin(), planBuf.begin() + static_cast<long> (n));
            }
        }
        else if (front.msg.isNoteOff())
        {
            // 2026-09-06: matched by (channel, note) IDENTITY, not pitch
            // alone - see figConsumedIdentities' comment above. A pitch-only
            // test wrongly suppressed a completely unrelated instrument's
            // note-off whenever it happened to share a pitch with the held
            // figure (only really likely for Murmur's broad multi-pitch
            // union; Tremolo/RepeatedNote's tight 1-2 pitch sets made this
            // rare in practice, but the fix is the same for all three).
            const int offNote = front.msg.getNoteNumber();
            const int offCh   = front.msg.getChannel();
            // 2026-09-07: Arpeggio is excluded here - unlike the other three
            // figure types, it never holds a note to the run's end (each
            // onset is its own real, independently-releasing note - see the
            // reduceGroup() call site's releasePpq comment), so its
            // consumed identities' own real note-offs must reach
            // handleNoteOff() normally, not be swallowed as "the figure
            // holds this note".
            const bool figOff = currentFigureType != ocpn::FigureType::Arpeggio
                && front.ppq < figureEndPpq - 1.0e-6
                && std::find (figConsumedIdentities.begin(), figConsumedIdentities.end(),
                              std::make_pair (offCh, offNote)) != figConsumedIdentities.end();
            if (figOff)
            {
                planBuf.erase (planBuf.begin());   // the figure holds this note
            }
            else
            {
                handleNoteOff (front.msg, emitSampleFor (front.ppq), output);
                planBuf.erase (planBuf.begin());
            }
        }
        else
        {
            output.addEvent (front.msg, emitSampleFor (front.ppq));
            planBuf.erase (planBuf.begin());
        }
    }

    planBufCount.store (static_cast<int> (planBuf.size()));
}

void OrchPianoAudioProcessor::drainRestrikes (juce::MidiBuffer& output, double blockStartPpq,
                                              double lookaheadPpq, double ppqPerSample,
                                              int numSamples, int maxRingBeats)
{
    if (pendingRestrikes.empty() || ppqPerSample <= 0.0)
        return;

    const double cutoff = (blockStartPpq + numSamples * ppqPerSample) - lookaheadPpq;
    const double step = juce::jmax (1, maxRingBeats);
    const bool doLog = decisionLogParam != nullptr && decisionLogParam->load() >= 0.5f;

    for (auto it = pendingRestrikes.begin(); it != pendingRestrikes.end();)
    {
        // No precomputed end - keep re-striking and rescheduling for as long
        // as the note is genuinely still live, checked fresh each time
        // against activeNotes (never against a precomputed "expected end",
        // which is exactly what silently missed the bug this fixes).
        //
        // Matched by `seq`, NOT just the (channel, inputNote, outCh, pitch)
        // tuple - see PendingRestrike's header comment. For a genuinely
        // repeated note (real fast repeated figure, not one sustain), that
        // tuple recurs on every strike; matching on it alone let a checkpoint
        // for one long-since-ended strike falsely latch onto a LATER,
        // unrelated strike's activeNotes entry and re-fire forever.
        bool stillLive = true;
        while (stillLive && it->nextPpq <= cutoff)
        {
            bool live = false;
            for (const auto& t : activeNotes)
                if (t.seq == it->seq && t.outputNote == it->pitch && t.outputChannel == it->outCh
                    && t.channel == it->inCh && t.inputNote == it->inNote)
                    { live = true; break; }
            if (! live) { stillLive = false; break; }

            const int s = juce::jlimit (0, juce::jmax (0, numSamples - 1),
                juce::roundToInt ((it->nextPpq + lookaheadPpq - blockStartPpq) / ppqPerSample));
            output.addEvent (juce::MidiMessage::noteOff (it->outCh, it->pitch), juce::jmax (0, s - 1));
            output.addEvent (juce::MidiMessage::noteOn (it->outCh, it->pitch,
                                                       static_cast<juce::uint8> (it->vel)), s);
            if (doLog)
                // 2026-09-06: input channel/note included alongside the
                // pitch - added while chasing a persistent re-strike chain
                // that survived several OrchPiano-side fixes with no figure
                // active nearby; that turned out to have an entirely upstream
                // cause (an idle OrchMerge Sender transmitting a phantom
                // note-on with no clip on its track - see
                // project_orchmerge_concept memory), but knowing the exact
                // source identity is what let the user spot it, so it stays.
                logEvent (it->nextPpq, "re-strike " + juce::MidiMessage::getMidiNoteName (it->pitch, true, true, 3)
                          + " (still ringing past " + juce::String (maxRingBeats) + " beats)"
                          + "  [in ch" + juce::String (it->inCh) + " note" + juce::String (it->inNote) + "]");
            it->nextPpq = snapUpToBar (it->nextPpq + step);
        }

        if (! stillLive)
            it = pendingRestrikes.erase (it);
        else
            ++it;
    }
}

void OrchPianoAudioProcessor::drainHardOffs (juce::MidiBuffer& output, double blockStartPpq,
                                             double lookaheadPpq, double ppqPerSample, int numSamples)
{
    if (pendingHardOffs.empty() || ppqPerSample <= 0.0)
        return;

    const double cutoff = (blockStartPpq + numSamples * ppqPerSample) - lookaheadPpq;

    for (auto it = pendingHardOffs.begin(); it != pendingHardOffs.end();)
    {
        if (it->ppq > cutoff)
        {
            ++it;
            continue;
        }

        const int s = juce::jlimit (0, juce::jmax (0, numSamples - 1),
            juce::roundToInt ((it->ppq + lookaheadPpq - blockStartPpq) / ppqPerSample));

        for (auto a = activeNotes.begin(); a != activeNotes.end();)
        {
            if (a->outputNote == it->pitch && a->outputChannel == it->outCh)
            {
                output.addEvent (juce::MidiMessage::noteOff (it->outCh, it->pitch), s);
                a = activeNotes.erase (a);
            }
            else
                ++a;
        }
        // stop re-striking this note too
        pendingRestrikes.erase (std::remove_if (pendingRestrikes.begin(), pendingRestrikes.end(),
            [&] (const PendingRestrike& r) { return r.pitch == it->pitch && r.outCh == it->outCh; }),
            pendingRestrikes.end());
        it = pendingHardOffs.erase (it);
    }
}

void OrchPianoAudioProcessor::drainTremolos (juce::MidiBuffer& output, double blockStartPpq,
                                             double lookaheadPpq, double ppqPerSample, int numSamples)
{
    if (pendingTremolos.empty() || ppqPerSample <= 0.0)
        return;

    const double cutoff = (blockStartPpq + numSamples * ppqPerSample) - lookaheadPpq;
    const bool doLog = decisionLogParam != nullptr && decisionLogParam->load() >= 0.5f;

    for (auto it = pendingTremolos.begin(); it != pendingTremolos.end();)
    {
        bool finished = false;

        while (it->nextPpq <= cutoff)
        {
            const int s = juce::jlimit (0, juce::jmax (0, numSamples - 1),
                juce::roundToInt ((it->nextPpq + lookaheadPpq - blockStartPpq) / ppqPerSample));
            const int soundingPitch = it->highPhaseNow ? it->highPitch : it->lowPitch;

            if (it->nextPpq >= it->endPpq - 1.0e-6)
            {
                // End of the figure's span: release whichever pitch is
                // currently sounding and clear the matching activeNotes
                // entry (by original INPUT identity, not by whichever pitch
                // happens to be sounding right now - the two can differ
                // depending on which phase this ended on).
                output.addEvent (juce::MidiMessage::noteOff (it->outCh, soundingPitch), s);
                for (auto a = activeNotes.begin(); a != activeNotes.end(); ++a)
                {
                    if (a->channel == it->inCh && a->inputNote == it->inNote)
                    {
                        activeNotes.erase (a);
                        break;
                    }
                }
                if (doLog)
                    logEvent (it->nextPpq, "tremolo  ended, released "
                              + juce::MidiMessage::getMidiNoteName (soundingPitch, true, true, 3));
                finished = true;
                break;
            }

            const int nextPitch = it->highPhaseNow ? it->lowPitch : it->highPitch;
            output.addEvent (juce::MidiMessage::noteOff (it->outCh, soundingPitch), juce::jmax (0, s - 1));
            output.addEvent (juce::MidiMessage::noteOn (it->outCh, nextPitch,
                                                       static_cast<juce::uint8> (it->vel)), s);
            if (doLog)
                logEvent (it->nextPpq, "tremolo  alternate to "
                          + juce::MidiMessage::getMidiNoteName (nextPitch, true, true, 3));
            it->highPhaseNow = ! it->highPhaseNow;
            it->nextPpq += it->stepBeats;
        }

        it = finished ? pendingTremolos.erase (it) : std::next (it);
    }
}

// ---- processBlock -------------------------------------------------------

void OrchPianoAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    buffer.clear();

    const int numSamples = buffer.getNumSamples();

    bool playing = false;
    double bpm = 120.0;
    double blockStartPpq = integratedPpq;
    double hostBeatsPerBar = 4.0;
    if (auto* ph = getPlayHead())
    {
        if (const auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            if (const auto b = pos->getBpm(); b && *b > 0.0) bpm = *b;
            if (const auto p = pos->getPpqPosition()) blockStartPpq = *p;
            if (const auto ts = pos->getTimeSignature(); ts && ts->numerator > 0 && ts->denominator > 0)
                hostBeatsPerBar = ts->numerator * 4.0 / ts->denominator;
        }
    }
    const double ppqPerSample = sampleRate > 0.0 ? (bpm / 60.0) / sampleRate : 0.0;
    integratedPpq = blockStartPpq + numSamples * ppqPerSample;

    const int onsetWindowSamples = juce::jmax (1, juce::roundToInt (
        (onsetWindowMsParam != nullptr ? juce::jlimit (5, 200, juce::roundToInt (onsetWindowMsParam->load())) : 90)
        * sampleRate / 1000.0));

    const bool transform = operatingModeParam != nullptr && juce::roundToInt (operatingModeParam->load()) == 2;
    const int  lookaheadBeats = lookaheadBeatsParam != nullptr ? juce::roundToInt (lookaheadBeatsParam->load()) : 0;
    const double lookaheadPpq = juce::jmax (0, lookaheadBeats); // 1 beat == 1 quarter-note == 1 ppq unit
    const bool planning = lookaheadPpq > 0.0 && ! transform;

    // NOTE: the lookahead delay is deliberately NOT reported via
    // setLatencySamples(). At a slow tempo `lookaheadBeats` is several seconds -
    // past the host's latency-compensation ceiling (Bitwig caps at 2 s) - and
    // re-reporting it as the tempo reading wobbles makes the transport jitter.
    // OrchPiano is a non-real-time reduction tool: its constant delay is taken
    // out downstream (OrchCapture per-lane time-offset compensation, Phase 5d, or
    // the user nudges the captured clip back by `lookaheadBeats`).

    juce::MidiBuffer output;

    if (! playing && wasPlaying)
    {
        flushGroup (output, 0, blockStartPpq, ppqPerSample);
        // The planning buffer at this point holds only the not-yet-ripe tail
        // (~lookaheadBeats of material that never got its full context). Dumping
        // it all at one instant makes a loud phantom cluster and it can't be
        // captured post-stop anyway - drop it. Run the take a bar or two past the
        // last real note; P5d (OrchCapture time-offset) will let it drain cleanly.
        planBuf.clear();
    pendingRestrikes.clear();
    resetFigureState();
        planBufCount.store (0);
        dampAllRinging (output, 0);
        activeNotes.clear();
        prevGroupNotes.clear();
        prevGroupHands.clear();
        prevKeptNotes.clear();
        if (logWriter != nullptr)
            logWriter->wake.signal();          // flush the take's decision log
    }
    else if (playing && ! wasPlaying)
    {
        prevGroupNotes.clear();
        prevGroupHands.clear();
        prevKeptNotes.clear();
        planBuf.clear();
    pendingRestrikes.clear();
    resetFigureState();
        planPhraseSplit = -1;
        planLastOnsetPpq = -1.0e18;
        resetVoiceLines();
        beatsPerBar = hostBeatsPerBar > 0.0 ? hostBeatsPerBar : 4.0;
        const juce::ScopedLock sl (decisionLock);
        decisionLines.clear();
    }
    // Transport jumped backwards (loop / relocate) while playing: the buffer is
    // stale - drop it and release anything ringing.
    if (planning && playing && wasPlaying && blockStartPpq + 0.5 < lastBlockStartPpq && ! planBuf.empty())
    {
        planBuf.clear();
    pendingRestrikes.clear();
    resetFigureState();
        planPhraseSplit = -1;
        planLastOnsetPpq = -1.0e18;
        dampAllRinging (output, 0);
        prevGroupNotes.clear();
        prevGroupHands.clear();
        prevKeptNotes.clear();
        resetVoiceLines();
    }
    lastBlockStartPpq = blockStartPpq;
    wasPlaying = playing;

    // Phase 5d: report the constant lookahead delay downstream so OrchCapture
    // (or anything else that cares) can shift its capture back by the same
    // amount. Sent at transport start, on any value change, and re-sent every 4
    // bars as a safety net for a late-loading listener. A plain CC, observed and
    // passed through - never consumed - by anything downstream.
    if (planning && playing)
    {
        const int compCc = delayCompensationCcParam != nullptr
            ? juce::roundToInt (delayCompensationCcParam->load()) : 0;
        if (compCc > 0)
        {
            const bool valueChanged = lookaheadBeats != lastSentDelayCcValue;
            const bool dueForResend = blockStartPpq - lastDelayCcSentPpq >= 4.0 * juce::jmax (1.0, beatsPerBar);
            if (valueChanged || dueForResend)
            {
                output.addEvent (juce::MidiMessage::controllerEvent (1, compCc, juce::jlimit (0, 127, lookaheadBeats)), 0);
                lastSentDelayCcValue = lookaheadBeats;
                lastDelayCcSentPpq = blockStartPpq;
            }
        }
    }
    else if (! playing)
    {
        lastSentDelayCcValue = -1;
        lastDelayCcSentPpq = -1.0e18;
    }

    if (transform)
        return; // not built yet - transparent pass-through

    for (const auto metadata : midiMessages)
    {
        const auto message = metadata.getMessage();
        const int samplePosition = metadata.samplePosition;

        if (message.isAllNotesOff() || message.isAllSoundOff())
        {
            currentGroup.clear();
            planBuf.clear();
    pendingRestrikes.clear();
    resetFigureState();
            dampAllRinging (output, samplePosition);
            activeNotes.clear();
            output.addEvent (message, samplePosition);
            continue;
        }

        // Dropped entirely (not forwarded to output either) rather than
        // passed through like OrchMerge's Sender does for its own downstream
        // instrument - a piano reduction has no legitimate use for a
        // keyswitch/articulation-select note, and letting it reach role
        // tagging is actively harmful: ocpn::bassIndex() has no floor at all
        // and will misidentify it as the group's bass note (see
        // createParameterLayout for the live finding this fixes). Applies
        // identically to note-on and note-off so a pair is never split.
        if ((message.isNoteOn() || message.isNoteOff())
            && isNoteInKsExclusionZone (message.getNoteNumber()))
            continue;

        if (planning)
        {
            planBuf.push_back ({ blockStartPpq + samplePosition * ppqPerSample, message });
            continue;
        }

        // ---- streaming engine (lookahead 0) ----
        if (message.isNoteOn())
        {
            if (! currentGroup.empty()
                && samplePosition - currentGroupStartSample > onsetWindowSamples)
                flushGroup (output, currentGroupStartSample, blockStartPpq, ppqPerSample);

            if (currentGroup.empty())
                currentGroupStartSample = samplePosition;

            currentGroup.push_back ({ message.getChannel(), message.getNoteNumber(),
                                      message.getVelocity(), samplePosition });
            continue;
        }

        if (message.isNoteOff())
        {
            if (! currentGroup.empty())
                flushGroup (output, currentGroupStartSample, blockStartPpq, ppqPerSample);
            handleNoteOff (message, samplePosition, output);
            continue;
        }

        output.addEvent (message, samplePosition); // CCs etc. pass through
    }

    if (planning)
    {
        const int maxRing = maxRingBeatsParam != nullptr ? juce::roundToInt (maxRingBeatsParam->load()) : 0;
        flushPlanBuffer (output, blockStartPpq, lookaheadPpq, ppqPerSample, numSamples, onsetWindowSamples);
        drainHardOffs (output, blockStartPpq, lookaheadPpq, ppqPerSample, numSamples);
        drainRestrikes (output, blockStartPpq, lookaheadPpq, ppqPerSample, numSamples, maxRing);
        drainTremolos (output, blockStartPpq, lookaheadPpq, ppqPerSample, numSamples);
    }
    else if (! currentGroup.empty())
    {
        // Close the open group at the block end, emitting at its own onset sample.
        flushGroup (output, juce::jlimit (0, juce::jmax (0, numSamples - 1), currentGroupStartSample),
                    blockStartPpq, ppqPerSample);
    }

    midiMessages.swapWith (output);
}

// ---- boilerplate -------------------------------------------------------

juce::AudioProcessorEditor* OrchPianoAudioProcessor::createEditor()
{
    return new OrchPianoAudioProcessorEditor (*this);
}

bool OrchPianoAudioProcessor::hasEditor() const { return true; }
const juce::String OrchPianoAudioProcessor::getName() const { return JucePlugin_Name; }
bool OrchPianoAudioProcessor::acceptsMidi() const { return true; }
bool OrchPianoAudioProcessor::producesMidi() const { return true; }
bool OrchPianoAudioProcessor::isMidiEffect() const { return true; }
double OrchPianoAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int OrchPianoAudioProcessor::getNumPrograms() { return 1; }
int OrchPianoAudioProcessor::getCurrentProgram() { return 0; }
void OrchPianoAudioProcessor::setCurrentProgram (int) {}
const juce::String OrchPianoAudioProcessor::getProgramName (int) { return {}; }
void OrchPianoAudioProcessor::changeProgramName (int, const juce::String&) {}

void OrchPianoAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = std::unique_ptr<juce::XmlElement> (parameters.copyState().createXml()))
        copyXmlToBinary (*xml, destData);
}

void OrchPianoAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = std::unique_ptr<juce::XmlElement> (getXmlFromBinary (data, sizeInBytes)))
        if (xml->hasTagName (parameters.state.getType()))
            parameters.replaceState (juce::ValueTree::fromXml (*xml));

    resetNoteMap();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OrchPianoAudioProcessor();
}
