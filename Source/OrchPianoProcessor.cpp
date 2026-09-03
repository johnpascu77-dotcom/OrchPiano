#include "OrchPianoProcessor.h"
#include "OrchPianoEditor.h"

#include <algorithm>
#include <map>

namespace
{
    constexpr int kProtectBothEnds = 3;
}

OrchPianoAudioProcessor::OrchPianoAudioProcessor()
    : AudioProcessor (BusesProperties()),
      parameters (*this, nullptr, "OrchPianoParameters", createParameterLayout())
{
    operatingModeParam   = parameters.getRawParameterValue ("operatingMode");
    handsParam           = parameters.getRawParameterValue ("hands");
    maxVoicesParam       = parameters.getRawParameterValue ("maxVoices");
    splitNoteParam       = parameters.getRawParameterValue ("splitNote");
    maxNotesPerHandParam = parameters.getRawParameterValue ("maxNotesPerHand");
    maxSpanParam         = parameters.getRawParameterValue ("maxSpan");
    crossoverSlackParam  = parameters.getRawParameterValue ("crossoverSlack");
    protectParam         = parameters.getRawParameterValue ("protect");
    dampSuccessiveParam  = parameters.getRawParameterValue ("dampSuccessive");
    onsetWindowMsParam   = parameters.getRawParameterValue ("onsetWindowMs");
    outChannelBaseParam  = parameters.getRawParameterValue ("outChannelBase");

    resetNoteMap();
}

OrchPianoAudioProcessor::~OrchPianoAudioProcessor() = default;

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
        juce::ParameterID { "maxNotesPerHand", 1 }, "Notes / Hand", 2, 6, 4));

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "maxSpan", 1 }, "Max Hand Span (st)", 8, 16, 14));

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "crossoverSlack", 1 }, "Crossover Slack (st)", 0, 12, 5));

    params.push_back (std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "protect", 1 }, "Protect",
        juce::StringArray { "None", "Lowest", "Highest", "Both Ends" }, kProtectBothEnds));

    params.push_back (std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "dampSuccessive", 1 }, "Damp On Next Attack", true));

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "onsetWindowMs", 1 }, "Onset Window (ms)", 5, 200, 90));

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "outChannelBase", 1 }, "Out Channel (Right; Left = +1)", 1, 15, 1));

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
    prevGroupNotes.clear();
    prevGroupHands.clear();
}

void OrchPianoAudioProcessor::dampAllRinging (juce::MidiBuffer& output, int sample)
{
    for (auto& t : activeNotes)
    {
        if (t.outputNote >= 0)
        {
            output.addEvent (juce::MidiMessage::noteOff (juce::jlimit (1, 16, t.outputChannel), t.outputNote),
                             juce::jmax (0, sample));
            t.outputNote = -2; // damped: swallow the eventual input note-off
        }
    }
}

void OrchPianoAudioProcessor::handleNoteOff (const juce::MidiMessage& message, int sample, juce::MidiBuffer& output)
{
    const int ch = message.getChannel();
    const int note = message.getNoteNumber();

    for (auto it = activeNotes.begin(); it != activeNotes.end(); ++it)
    {
        if (it->channel != ch || it->inputNote != note)
            continue;

        if (it->outputNote >= 0)
            output.addEvent (juce::MidiMessage::noteOff (juce::jlimit (1, 16, it->outputChannel), it->outputNote),
                             juce::jmax (0, sample));
        // outputNote == -2 (already damped) or -1 (dropped): just consume.
        activeNotes.erase (it);
        return;
    }
}

void OrchPianoAudioProcessor::flushGroup (juce::MidiBuffer& output, int flushSample)
{
    if (currentGroup.empty())
        return;

    const int sample = juce::jmax (0, flushSample);

    // Unique pitches, ascending; keep the loudest onset per pitch.
    std::map<int, HeldOn> byPitch;
    for (const auto& h : currentGroup)
    {
        auto existing = byPitch.find (h.note);
        if (existing == byPitch.end() || h.velocity > existing->second.velocity)
            byPitch[h.note] = h;
    }

    std::vector<int> sortedNotes;
    sortedNotes.reserve (byPitch.size());
    for (const auto& kv : byPitch)
        sortedNotes.push_back (kv.first);

    const int handsMode  = handsParam        != nullptr ? juce::roundToInt (handsParam->load()) : 0;
    const int splitNote  = splitNoteParam    != nullptr ? juce::roundToInt (splitNoteParam->load()) : 60;
    const int slack      = crossoverSlackParam != nullptr ? juce::roundToInt (crossoverSlackParam->load()) : 5;
    const int perHand    = maxNotesPerHandParam != nullptr ? juce::roundToInt (maxNotesPerHandParam->load()) : 4;
    const int maxSpan    = maxSpanParam      != nullptr ? juce::roundToInt (maxSpanParam->load()) : 14;
    const int protect    = protectParam      != nullptr ? juce::roundToInt (protectParam->load()) : kProtectBothEnds;
    const int chanBase   = outChannelBaseParam != nullptr ? juce::roundToInt (outChannelBaseParam->load()) : 1;
    const bool damp      = dampSuccessiveParam != nullptr && dampSuccessiveParam->load() >= 0.5f;

    const auto handAssign = ocpn::assignHands (sortedNotes, splitNote, slack, prevGroupNotes, prevGroupHands);

    if (damp)
        dampAllRinging (output, sample);

    std::vector<int> keptNotes, keptHands;
    int emitted = 0, topLeft = -1, topRight = -1;

    for (int hand = 1; hand <= 2; ++hand)
    {
        if (handsMode == 1 && hand != 1) continue; // Left only
        if (handsMode == 2 && hand != 2) continue; // Right only

        std::vector<int> sub;
        for (size_t i = 0; i < sortedNotes.size(); ++i)
            if (handAssign[i] == hand)
                sub.push_back (sortedNotes[i]);

        if (sub.empty())
            continue;

        ocpn::VoiceConfig cfg;
        cfg.hand = 0;            // already register-split by assignHands
        cfg.splitMode = 0;
        cfg.maxVoices = perHand;
        cfg.maxSpanSemis = maxSpan;
        cfg.protect = protect;

        const auto keep = ocpn::selectVoices (sub, cfg);
        const int outCh = juce::jlimit (1, 16, hand == 2 ? chanBase : chanBase + 1);

        for (int idx : keep)
        {
            const int pitch = sub[static_cast<size_t> (idx)];
            const auto& src = byPitch[pitch];

            output.addEvent (juce::MidiMessage::noteOn (outCh, pitch, src.velocity), sample);
            activeNotes.push_back ({ src.channel, pitch, pitch, outCh });

            keptNotes.push_back (pitch);
            keptHands.push_back (hand);
            ++emitted;
            if (hand == 1) topLeft  = juce::jmax (topLeft,  pitch);
            if (hand == 2) topRight = juce::jmax (topRight, pitch);
        }
    }

    prevGroupNotes.swap (keptNotes);
    prevGroupHands.swap (keptHands);

    lastSeen.store (static_cast<int> (sortedNotes.size()));
    lastKept.store (emitted);
    lastLeft.store (topLeft);
    lastRight.store (topRight);

    currentGroup.clear();
}

void OrchPianoAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    buffer.clear();

    const int numSamples = buffer.getNumSamples();

    bool playing = false;
    if (auto* ph = getPlayHead())
        if (const auto pos = ph->getPosition())
            playing = pos->getIsPlaying();

    const int onsetWindowSamples = juce::jmax (1, juce::roundToInt (
        (onsetWindowMsParam != nullptr ? juce::jlimit (5, 200, juce::roundToInt (onsetWindowMsParam->load())) : 90)
        * sampleRate / 1000.0));

    const bool transform = operatingModeParam != nullptr && juce::roundToInt (operatingModeParam->load()) == 2;

    juce::MidiBuffer output;

    // Transport edges: flush a half-built group, damp anything ringing.
    if (! playing && wasPlaying)
    {
        flushGroup (output, 0);
        dampAllRinging (output, 0);
        activeNotes.clear();
        prevGroupNotes.clear();
        prevGroupHands.clear();
    }
    else if (playing && ! wasPlaying)
    {
        prevGroupNotes.clear();
        prevGroupHands.clear();
    }
    wasPlaying = playing;

    // Transform mode is not built yet - pass everything through untouched so the
    // plugin is transparent rather than wrong.
    if (transform)
    {
        wasPlaying = playing;
        return;
    }

    for (const auto metadata : midiMessages)
    {
        const auto message = metadata.getMessage();
        const int samplePosition = metadata.samplePosition;

        if (message.isNoteOn())
        {
            if (! currentGroup.empty()
                && samplePosition - currentGroupStartSample > onsetWindowSamples)
                flushGroup (output, currentGroupStartSample);

            if (currentGroup.empty())
                currentGroupStartSample = samplePosition;

            currentGroup.push_back ({ message.getChannel(), message.getNoteNumber(),
                                      message.getVelocity(), samplePosition });
            continue;
        }

        if (message.isNoteOff())
        {
            if (! currentGroup.empty())
                flushGroup (output, currentGroupStartSample);
            handleNoteOff (message, samplePosition, output);
            continue;
        }

        if (message.isAllNotesOff() || message.isAllSoundOff())
        {
            currentGroup.clear();
            dampAllRinging (output, samplePosition);
            activeNotes.clear();
            output.addEvent (message, samplePosition);
            continue;
        }

        output.addEvent (message, samplePosition); // CCs, pitch bend, etc. pass through
    }

    // Close the open group at the block end (a chord split by a buffer boundary
    // becomes two mini-groups a few ms apart - DAW chords land on one tick).
    if (! currentGroup.empty())
        flushGroup (output, juce::jmax (0, numSamples - 1));

    midiMessages.swapWith (output);
}

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
