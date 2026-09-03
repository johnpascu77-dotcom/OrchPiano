#pragma once

#include <atomic>
#include <vector>
#include <JuceHeader.h>

#include "OrchPianoReductionLogic.h"

// OrchPiano - Phase 2 skeleton.
//
// Streaming engine only: buffers each onset group, assigns hands (register split
// + crossover dead zone), clamps each hand to a playable slice (ocpn::selectVoices
// - poly cap, span clamp, function-weighted protect), damps still-ringing notes
// on the next attack, and emits the Left hand on `outChannelBase + 1`, the Right
// on `outChannelBase`. Two Dorico staves.
//
// Not yet built (see Docs/OrchPiano_Design.md phases): importance-ordered dropping
// and role tagging (P3), close-position re-voicing + idiom substitution (P4), the
// lookahead planning engine + OrchCapture merged-stream input (P5), pedal, the
// decision-log sidecar, 4-voice output.
class OrchPianoAudioProcessor final : public juce::AudioProcessor
{
public:
    OrchPianoAudioProcessor();
    ~OrchPianoAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getParameters() { return parameters; }
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // ---- UI status readouts (message thread; plain atomic reads) ----------
    int getLastSeenForUi()   const { return lastSeen.load(); }
    int getLastKeptForUi()   const { return lastKept.load(); }
    int getLastLeftForUi()   const { return lastLeft.load(); }
    int getLastRightForUi()  const { return lastRight.load(); }

private:
    juce::AudioProcessorValueTreeState parameters;

    std::atomic<float>* operatingModeParam = nullptr;
    std::atomic<float>* handsParam          = nullptr;
    std::atomic<float>* maxVoicesParam      = nullptr;
    std::atomic<float>* splitNoteParam      = nullptr;
    std::atomic<float>* maxNotesPerHandParam = nullptr;
    std::atomic<float>* maxSpanParam        = nullptr;
    std::atomic<float>* crossoverSlackParam = nullptr;
    std::atomic<float>* protectParam        = nullptr;
    std::atomic<float>* dampSuccessiveParam = nullptr;
    std::atomic<float>* onsetWindowMsParam  = nullptr;
    std::atomic<float>* outChannelBaseParam = nullptr;

    // ---- note tracking (ONF pattern) ----
    struct TrackedNote
    {
        int channel = 0;       // input channel (note-off matched on this)
        int inputNote = 0;
        int outputNote = -1;   // emitted pitch, or -1 if dropped
        int outputChannel = 0;
    };
    std::vector<TrackedNote> activeNotes;

    // ---- open onset group (streaming engine) ----
    struct HeldOn
    {
        int channel = 1;
        int note = 0;
        juce::uint8 velocity = 100;
        int samplePos = 0;
    };
    std::vector<HeldOn> currentGroup;
    int currentGroupStartSample = 0;

    // hysteresis for the crossover dead zone: the previous group's assignment
    std::vector<int> prevGroupNotes, prevGroupHands;

    double sampleRate = 44100.0;
    bool wasPlaying = false;

    // ---- readouts ----
    std::atomic<int> lastSeen  { 0 };
    std::atomic<int> lastKept  { 0 };
    std::atomic<int> lastLeft  { -1 };
    std::atomic<int> lastRight { -1 };

    void resetNoteMap();
    void flushGroup (juce::MidiBuffer& output, int flushSample);
    void dampAllRinging (juce::MidiBuffer& output, int sample);
    void handleNoteOff (const juce::MidiMessage& message, int sample, juce::MidiBuffer& output);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchPianoAudioProcessor)
};
