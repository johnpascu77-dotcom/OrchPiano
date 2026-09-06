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
// Dorico voices on `outChannelBase .. +3` (RH up/down, LH up/down) - six when
// "Max Voices" = 6 raises the per-hand line ceiling to 3, adding a second
// inner voice per hand on `outChannelBase +4/+5` (appended, not renumbered,
// so the original +0..+3 mapping never changes for anything downstream keyed
// off it). Damps still-ringing notes on the next attack. Writes a decision-log sidecar
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
    int getAdaptiveSplitForUi() const { return adaptiveSplit.load(); }
    int getPlanBufferForUi()  const { return planBufCount.load(); }

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
    std::atomic<float>* lookaheadBeatsParam   = nullptr;
    std::atomic<float>* delayCompensationCcParam = nullptr;
    std::atomic<float>* difficultyCeilingParam = nullptr;
    std::atomic<float>* keepBassOctavesParam  = nullptr;
    std::atomic<float>* keepMelodyOctavesParam = nullptr;
    std::atomic<float>* decisionLogParam      = nullptr;
    std::atomic<float>* handVoicesParam       = nullptr;
    std::atomic<float>* revoiceParam          = nullptr;
    std::atomic<float>* lowIntervalStrictnessParam = nullptr;
    std::atomic<float>* dynamicContourParam   = nullptr;
    std::atomic<float>* maxRingBeatsParam     = nullptr;
    std::atomic<float>* wMelodyBassParam      = nullptr;
    std::atomic<float>* wVelocityParam        = nullptr;
    std::atomic<float>* wDoubleParam          = nullptr;
    std::atomic<float>* excludeKsNotesParam   = nullptr;
    std::atomic<float>* ksZoneMinParam        = nullptr;
    std::atomic<float>* ksZoneMaxParam        = nullptr;

    bool isNoteInKsExclusionZone (int note) const noexcept;

    // ---- note tracking (ONF pattern) ----
    struct TrackedNote
    {
        int channel = 0;       // input channel (note-off matched on this)
        int inputNote = 0;
        int outputNote = -1;   // emitted pitch, -1 dropped, -2 already damped
        int outputChannel = 0;
        // 2026-09-06: which emission this entry IS - see PendingRestrike::seq.
        // Left at -1 (never matches a real checkpoint) for entries that don't
        // arm one (folded/octave-add).
        juce::int64 seq = -1;
    };
    std::vector<TrackedNote> activeNotes;
    juce::int64 nextNoteSeq = 1;   // monotonic; 0 never issued, so 0 stays "no seq"

    // ---- Phase 5b-2/6: per-line voice state (planning engine, per phrase) --
    // Index 2 (line 2) is only ever written when "Max Voices" = 6 raises the
    // per-hand line ceiling to 3 - see ocpn::streamHandVoices.
    struct VoiceLineRT { int lastPitch = -1; double lastActivePpq = -1.0e18; };
    VoiceLineRT rhLine[3], lhLine[3];
    void resetVoiceLines();

    // ---- Phase 5c: maxRingBeats re-strike (planning engine) ----
    // A held note longer than `maxRingBeats` is re-articulated every that-many
    // beats (piano tone decays; an 8-beat tie is neither idiomatic nor what a
    // pianist plays).
    //
    // 2026-09-06: no longer carries a precomputed `endPpq` - an earlier
    // version only armed this at all when the note's total duration was
    // already known from the lookahead buffer, which is exactly backwards
    // for the case this exists to catch (a note whose real length exceeds
    // the buffer's own lookahead window has no such known duration, so the
    // safety net silently never engaged - live-found on a genuine ~55-beat
    // sustain in Grieg's "Morning Mood" that produced zero re-strikes).
    // Every emitted note now arms a checkpoint unconditionally; drainRestrikes
    // decides against LIVE activeNotes state whether the note is still
    // actually ringing when each checkpoint arrives, re-striking and
    // rescheduling indefinitely for as long as it is - correct regardless of
    // whether the note's eventual true length was ever knowable in advance.
    //
    // 2026-09-06 (second live-found bug, same day): (channel, inputNote,
    // outputChannel, outputNote) is NOT a unique identity for a genuinely
    // repeated note (a real fast repeated bass figure / roll, not a single
    // sustain) - the SAME 4 values recur on every strike. Matching only on
    // that tuple meant an OLD checkpoint, armed for one specific strike that
    // had already ended cleanly via its own real note-off, could spuriously
    // match a LATER, unrelated strike's activeNotes entry (identical tuple,
    // different note) and conclude "still live" - re-striking forever and
    // compounding, since every subsequent strike arms its OWN checkpoint too.
    // Confirmed live: a real repeated A2 in Grieg's "Morning Mood" produced a
    // cascading, ever-growing flood of "re-strike A2" every ~1/12 bar instead
    // of periodic re-strikes on one genuine sustain. `seq` disambiguates:
    // each emission gets a unique id shared between its TrackedNote and its
    // PendingRestrike, so a checkpoint only ever matches the EXACT occurrence
    // it was armed for, and self-terminates the moment that specific
    // occurrence's own entry is gone - regardless of how many other,
    // identically-pitched occurrences are active at the same instant.
    struct PendingRestrike
    {
        int outCh = 0, pitch = 0, inCh = 0, inNote = 0, vel = 100;
        double nextPpq = 0.0;
        juce::int64 seq = 0;
    };
    std::vector<PendingRestrike> pendingRestrikes;

    // ---- Phase 5c-2: figuration (tremolo / repeated note / murmur) collapse ----
    // A detected figure is emitted as its first 1-2 chords held to `figureEndPpq`
    // (re-struck by maxRingBeats); the repeats are consumed. The held notes have
    // no buffered note-off (it is consumed too), so a hard-off releases them.
    double figureEndPpq = -1.0e18;
    std::vector<int> figSetA, figSetB;      // sorted pitch lists
    int figGroupsToEmit = 0;
    // 2026-09-06 (Phase 5c-2c): which kind of figure is currently held - a
    // Murmur group is usually a single note out of the larger held chord
    // (figSetA), not an exact match to it, so membership needs to be tested
    // differently (subset, not set equality) than Tremolo/RepeatedNote.
    ocpn::FigureType currentFigureType = ocpn::FigureType::None;
    struct PendingHardOff { int outCh = 0, pitch = 0; double ppq = 0.0; };
    std::vector<PendingHardOff> pendingHardOffs;
    void resetFigureState();

    // ---- open onset group (streaming engine) ----
    struct HeldOn
    {
        int channel = 1;
        int note = 0;
        juce::uint8 velocity = 100;
        int samplePos = 0;
        int durTicks = 0;   // planning engine fills this (ppq*100); 0 = unknown
    };
    std::vector<HeldOn> currentGroup;
    int currentGroupStartSample = 0;

    std::vector<int> prevGroupNotes, prevGroupHands; // crossover hysteresis
    std::vector<int> prevKeptNotes;                  // motion term

    // ---- Phase 5: lookahead planning engine ----
    // When lookaheadBeats > 0, every input event is buffered with its ppq and
    // emitted a constant `lookaheadBeats` of musical time later; the reduction
    // for each onset group then runs with the following ~2 bars visible (the
    // KDE hand split, and - Phase 5b - part-tracking / figuration / phrase work).
    struct PlanEvent { double ppq = 0.0; juce::MidiMessage msg; };
    std::vector<PlanEvent> planBuf;   // sorted by ppq
    double lastBlockStartPpq = -1.0e18;
    int planPhraseSplit = -1;         // hand split held stable across the current phrase
    double planLastOnsetPpq = -1.0e18;
    int lastSentDelayCcValue = -1;    // Phase 5d: delay-compensation CC state
    double lastDelayCcSentPpq = -1.0e18;
    std::atomic<int> adaptiveSplit { 60 };
    std::atomic<int> planBufCount { 0 };

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
    void reduceGroup (const std::vector<HeldOn>& group, int splitNote,
                      int emitSample, double groupPpq, juce::MidiBuffer& output,
                      double figureReleasePpq = 0.0);
    void flushPlanBuffer (juce::MidiBuffer& output, double blockStartPpq, double lookaheadPpq,
                          double ppqPerSample, int numSamples, int onsetWindowSamples);
    void drainRestrikes (juce::MidiBuffer& output, double blockStartPpq, double lookaheadPpq,
                         double ppqPerSample, int numSamples, int maxRingBeats);
    void drainHardOffs (juce::MidiBuffer& output, double blockStartPpq, double lookaheadPpq,
                        double ppqPerSample, int numSamples);
    void dampAllRinging (juce::MidiBuffer& output, int sample);
    void handleNoteOff (const juce::MidiMessage& message, int sample, juce::MidiBuffer& output);
    void logEvent (double ppq, const juce::String& text);
    void logDrop (double ppq, const ocpn::DropRecord& d);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchPianoAudioProcessor)
};
