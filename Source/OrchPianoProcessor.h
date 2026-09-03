#pragma once

#include <atomic>
#include <memory>
#include <utility>
#include <vector>
#include <JuceHeader.h>

#include "OrchPianoReductionLogic.h"

// OrchPiano - Phase 3.
//
// Streaming engine: buffers each onset group, identifies melody + bass, tags
// roles (melody / bass / inner / doubling), assigns hands (register split +
// crossover dead zone), then reduces each hand by musical importance - drop
// doublings first, then lowest-importance inner notes over the voice budget /
// span / difficulty ceiling; melody and bass are never dropped. Emits four
// Dorico voices on `outChannelBase .. +3` (RH up/down, LH up/down). Damps
// still-ringing notes on the next attack. Writes a decision-log sidecar
// (%TEMP%/orchpiano-decisions-<tag>.log) on transport stop for supervisor review.
//
// Not yet built (see Docs/OrchPiano_Design.md): close-position re-voicing +
// idiom substitution (P4), the lookahead planning engine + OrchCapture
// merged-stream input + per-phrase KDE hand split (P5), pedal, Transform mode.
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
    int getLastSeenForUi()    const { return lastSeen.load(); }
    int getLastKeptForUi()    const { return lastKept.load(); }
    int getLastMelodyForUi()  const { return lastMelody.load(); }
    int getLastBassForUi()    const { return lastBass.load(); }
    int getLastDroppedForUi() const { return lastDropped.load(); }

private:
    juce::AudioProcessorValueTreeState parameters;

    std::atomic<float>* operatingModeParam   = nullptr;
    std::atomic<float>* handsParam            = nullptr;
    std::atomic<float>* maxVoicesParam        = nullptr;
    std::atomic<float>* splitNoteParam        = nullptr;
    std::atomic<float>* maxNotesPerHandParam  = nullptr;
    std::atomic<float>* maxSpanParam          = nullptr;
    std::atomic<float>* crossoverSlackParam   = nullptr;
    std::atomic<float>* dampSuccessiveParam   = nullptr;
    std::atomic<float>* onsetWindowMsParam    = nullptr;
    std::atomic<float>* outChannelBaseParam   = nullptr;
    std::atomic<float>* difficultyCeilingParam = nullptr;
    std::atomic<float>* keepBassOctavesParam  = nullptr;
    std::atomic<float>* keepMelodyOctavesParam = nullptr;
    std::atomic<float>* decisionLogParam      = nullptr;
    std::atomic<float>* handVoicesParam       = nullptr;
    std::atomic<float>* revoiceParam          = nullptr;
    std::atomic<float>* lowIntervalStrictnessParam = nullptr;
    std::atomic<float>* dynamicContourParam   = nullptr;
    std::atomic<float>* wMelodyBassParam      = nullptr;
    std::atomic<float>* wVelocityParam        = nullptr;
    std::atomic<float>* wDoubleParam          = nullptr;

    // ---- note tracking (ONF pattern) ----
    struct TrackedNote
    {
        int channel = 0;       // input channel (note-off matched on this)
        int inputNote = 0;
        int outputNote = -1;   // emitted pitch, -1 dropped, -2 already damped
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

    std::vector<int> prevGroupNotes, prevGroupHands; // crossover hysteresis
    std::vector<int> prevKeptNotes;                  // motion term

    double sampleRate = 44100.0;
    bool wasPlaying = false;

    // ---- bar clock (for the decision log) ----
    double integratedPpq = 0.0;
    double beatsPerBar = 4.0;

    // ---- decision-log sidecar (OrchHarp MarkerWriter pattern) ----
    struct LogWriter;
    std::unique_ptr<LogWriter> logWriter;
    juce::String logTag;
    std::vector<juce::String> decisionLines;
    juce::CriticalSection decisionLock;
    void writeDecisionFile();   // worker thread only

    // ---- readouts ----
    std::atomic<int> lastSeen    { 0 };
    std::atomic<int> lastKept    { 0 };
    std::atomic<int> lastMelody  { -1 };
    std::atomic<int> lastBass    { -1 };
    std::atomic<int> lastDropped { 0 };

    void resetNoteMap();
    void flushGroup (juce::MidiBuffer& output, int flushSample, double blockStartPpq, double ppqPerSample);
    void dampAllRinging (juce::MidiBuffer& output, int sample);
    void handleNoteOff (const juce::MidiMessage& message, int sample, juce::MidiBuffer& output);
    void logEvent (double ppq, const juce::String& text);
    void logDrop (double ppq, const ocpn::DropRecord& d);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchPianoAudioProcessor)
};
