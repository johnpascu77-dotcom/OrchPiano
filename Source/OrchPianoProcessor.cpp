#include "OrchPianoProcessor.h"
#include "OrchPianoEditor.h"

#include <algorithm>
#include <map>

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
        juce::ParameterID { "outChannelBase", 1 }, "Out Channel Base (voices +0..+3)", 1, 13, 1));

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

    return { params.begin(), params.end() };
}

void OrchPianoAudioProcessor::prepareToPlay (double newSampleRate, int)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    resetNoteMap();
}

void OrchPianoAudioProcessor::releaseResources() {}
bool OrchPianoAudioProcessor::isBusesLayoutSupported (const BusesLayout&) const { return true; }

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
        for (int i = 0; i < 2; ++i)
            l[i] = {};
}

void OrchPianoAudioProcessor::resetFigureState()
{
    figureEndPpq = -1.0e18;
    figSetA.clear();
    figSetB.clear();
    figGroupsToEmit = 0;
    pendingHardOffs.clear();
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

    // A note that ends naturally: stop re-striking it.
    pendingRestrikes.erase (std::remove_if (pendingRestrikes.begin(), pendingRestrikes.end(),
        [ch, note] (const PendingRestrike& p) { return p.inCh == ch && p.inNote == note; }),
        pendingRestrikes.end());

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
                                           double figureReleasePpq)
{
    if (group.empty())
        return;

    const int sample = juce::jmax (0, emitSample);
    const bool isFigure = figureReleasePpq > groupPpq + 1.0e-6;
    const int figureDurTicks = isFigure
        ? juce::jmax (1, juce::roundToInt ((figureReleasePpq - groupPpq) * 100.0)) : 0;

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

    // --- roles + importance on the whole group ---
    const int melIdx  = ocpn::melodyIndex (notes, vels);
    const int bassIdx = ocpn::bassIndex (notes);
    const auto roles  = ocpn::tagRoles (notes, melIdx, bassIdx);
    const auto imp    = ocpn::importanceScores (notes, vels, roles, prevKeptNotes, w,
                                                haveDur ? durs : std::vector<int> {});

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
        std::vector<ocpn::Role> subRoles;
        std::vector<double> subImp;
        for (size_t i = 0; i < notes.size(); ++i)
        {
            if (handAssign[i] != hand) continue;
            sub.push_back (notes[i]);
            subVel.push_back (vels[i]);
            subDur.push_back (durs[i]);
            subRoles.push_back (roles[i]);
            subImp.push_back (imp[i]);
        }
        if (sub.empty())
            continue;

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
        // LH -> chanBase+2).
        const int line0Ch = juce::jlimit (1, 16, hand == 2 ? chanBase     : chanBase + 3);
        const int line1Ch = juce::jlimit (1, 16, hand == 2 ? chanBase + 1 : chanBase + 2);
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
            voice = ocpn::streamHandVoices (keptOut, keptDur,
                                            lines[0].lastPitch, lines[1].lastPitch,
                                            secActive, hand == 2);
        }
        else if (handVoices == 2)   // forced positional split
        {
            const int leadKi = (hand == 2) ? static_cast<int> (keptOut.size()) - 1 : 0;
            for (int i = 0; i < static_cast<int> (voice.size()); ++i)
                voice[static_cast<size_t> (i)] = (i == leadKi) ? 0 : 1;
        }
        // handVoices == 1: all voice 0 (default).

        int line0Emit = -1, line1Emit = -1;
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

            const int outCh = (voice[ki] == 1) ? line1Ch : line0Ch;

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
            activeNotes.push_back ({ src.channel, inPitch, pitch, outCh });

            // A collapsed-figure note holds to figureReleasePpq and has no
            // buffered note-off (it is consumed) - schedule a hard release.
            const int noteDur = isFigure ? figureDurTicks : subDur[static_cast<size_t> (idx)];
            if (isFigure)
                pendingHardOffs.push_back ({ outCh, pitch, figureReleasePpq });

            // maxRingBeats: a note held longer than the limit is re-articulated
            // every that-many beats (piano tone decays). durTicks is ppq*100.
            if (maxRing > 0 && noteDur > static_cast<int> ((maxRing + 0.5) * 100.0))
            {
                pendingRestrikes.push_back ({ outCh, pitch, src.channel, inPitch, vel,
                                              groupPpq + maxRing, groupPpq + noteDur / 100.0 });
                if (doLog)
                    logEvent (groupPpq, "re-strike " + nn (pitch) + " every " + juce::String (maxRing)
                              + " beats (held " + juce::String (noteDur / 100.0, 1) + ")");
            }

            (voice[ki] == 1 ? line1Emit : line0Emit) = pitch;

            keptNotes.push_back (pitch);
            keptHands.push_back (hand);
            keptForMotion.push_back (pitch);
            ++emitted;

            if (subRoles[static_cast<size_t> (idx)] == ocpn::Role::Melody) melodyOut = pitch;
            if (subRoles[static_cast<size_t> (idx)] == ocpn::Role::Bass)   bassOut   = pitch;
        }

        if (line0Emit >= 0) { lines[0].lastPitch = line0Emit; lines[0].lastActivePpq = groupPpq; }
        if (line1Emit >= 0) { lines[1].lastPitch = line1Emit; lines[1].lastActivePpq = groupPpq; }

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
                planPhraseSplit = ocpn::kdeHandSplit (phrasePitches, priorSplit, 9);
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
                std::vector<std::vector<int>> gN;
                std::vector<double> gO;
                double curPpq = -1.0e18;
                for (const auto& e : planBuf)
                {
                    if (! e.msg.isNoteOn())
                        continue;
                    if (e.ppq - curPpq > onsetWindowPpq)
                    {
                        if (gN.size() >= 20) break;
                        gN.emplace_back();
                        gO.push_back (e.ppq);
                        curPpq = e.ppq;
                    }
                    gN.back().push_back (e.msg.getNoteNumber());
                }
                for (auto& g : gN) { std::sort (g.begin(), g.end()); g.erase (std::unique (g.begin(), g.end()), g.end()); }

                constexpr double kMaxFigIntervalBeats = 0.4;   // 16ths / 32nds
                const auto fig = ocpn::detectFigure (gN, gO, kMaxFigIntervalBeats, 4);
                if (fig.type != ocpn::FigureType::None)
                {
                    figureEndPpq    = gp + fig.spanBeats;
                    figSetA         = gN[0];
                    figSetB         = (fig.type == ocpn::FigureType::Tremolo && gN.size() > 1) ? gN[1] : gN[0];
                    figGroupsToEmit = (fig.type == ocpn::FigureType::Tremolo) ? 2 : 1;
                    if (doLog)
                    {
                        auto setStr = [] (const std::vector<int>& s)
                        {
                            juce::String r;
                            for (int p : s) r << (r.isEmpty() ? "" : "+") << juce::MidiMessage::getMidiNoteName (p, true, true, 3);
                            return r;
                        };
                        logEvent (gp, juce::String (fig.type == ocpn::FigureType::Tremolo ? "tremolo  " : "repeated ")
                                      + setStr (figSetA)
                                      + (fig.type == ocpn::FigureType::Tremolo ? (" ~ " + setStr (figSetB)) : juce::String())
                                      + "  (" + juce::String (fig.groups) + " hits, "
                                      + juce::String (fig.spanBeats, 1) + " beats) -> held");
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
            std::vector<int> pset;
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
                pset.push_back (m.getNoteNumber());
                ++n;
            }
            std::sort (pset.begin(), pset.end());
            pset.erase (std::unique (pset.begin(), pset.end()), pset.end());

            const bool inFigure = gp < figureEndPpq - 1.0e-6;
            const bool isFigGroup = inFigure && (pset == figSetA || pset == figSetB);

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

            if (isFigGroup && figGroupsToEmit <= 0)
            {
                planBuf.erase (planBuf.begin(), planBuf.begin() + static_cast<long> (n)); // a repeat - consumed
            }
            else
            {
                double releasePpq = 0.0;
                if (isFigGroup) { releasePpq = figureEndPpq; --figGroupsToEmit; }
                reduceGroup (group, planPhraseSplit, emitSampleFor (gp), gp, output, releasePpq);
                planBuf.erase (planBuf.begin(), planBuf.begin() + static_cast<long> (n));
            }
        }
        else if (front.msg.isNoteOff())
        {
            const int offNote = front.msg.getNoteNumber();
            const bool figOff = front.ppq < figureEndPpq - 1.0e-6
                && (std::find (figSetA.begin(), figSetA.end(), offNote) != figSetA.end()
                 || std::find (figSetB.begin(), figSetB.end(), offNote) != figSetB.end());
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

    for (auto it = pendingRestrikes.begin(); it != pendingRestrikes.end();)
    {
        bool done = false;
        while (! done && it->nextPpq <= cutoff && it->nextPpq < it->endPpq)
        {
            // Only re-strike if the note is still sounding (its real note-off
            // hasn't been processed).
            bool live = false;
            for (const auto& t : activeNotes)
                if (t.outputNote == it->pitch && t.outputChannel == it->outCh
                    && t.channel == it->inCh && t.inputNote == it->inNote)
                    { live = true; break; }
            if (! live) { done = true; break; }

            const int s = juce::jlimit (0, juce::jmax (0, numSamples - 1),
                juce::roundToInt ((it->nextPpq + lookaheadPpq - blockStartPpq) / ppqPerSample));
            output.addEvent (juce::MidiMessage::noteOff (it->outCh, it->pitch), juce::jmax (0, s - 1));
            output.addEvent (juce::MidiMessage::noteOn (it->outCh, it->pitch,
                                                       static_cast<juce::uint8> (it->vel)), s);
            it->nextPpq += step;
        }

        if (done || it->nextPpq >= it->endPpq)
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
