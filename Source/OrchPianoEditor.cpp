#include "OrchPianoEditor.h"
#include "OrchPianoBuildTimestamp.h"

namespace
{
    constexpr int kRowH = 26;
    constexpr int kPad  = 12;
}

OrchPianoAudioProcessorEditor::OrchPianoAudioProcessorEditor (OrchPianoAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    titleLabel.setText ("OrchPiano", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    addAndMakeVisible (titleLabel);

    // ORCHPIANO_BUILD_TIMESTAMP comes from a header CMake regenerates on
    // EVERY build (see GenerateBuildTimestamp.cmake) - unlike __DATE__/
    // __TIME__ baked into this one .cpp, it stays accurate even when an
    // incremental build only recompiled a different file for its own fix.
    buildLabel.setText ("Build: Max Voices wired to a real 3rd per-hand line (was dead)"
                         "  |  compiled " ORCHPIANO_BUILD_TIMESTAMP, juce::dontSendNotification);
    buildLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    buildLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible (buildLabel);

    addChoiceRow ("operatingMode", "Mode", { "Repair", "Reduce", "Transform" });
    addChoiceRow ("hands", "Hands", { "Both", "Left", "Right" });
    addChoiceRow ("maxVoices", "Max Voices", { "4", "6" });
    addChoiceRow ("handVoices", "Voices per Hand", { "Auto (streamed)", "1 (clean)", "2 (positional)" });
    addSliderRow ("splitNote", "Hand Split Note", 0, 127);
    addSliderRow ("maxNotesPerHand", "Notes / Hand (Reduce)", 2, 8);
    addSliderRow ("maxSpan", "Max Hand Span (st)", 8, 16);
    addSliderRow ("crossoverSlack", "Crossover Slack (st)", 0, 12);
    addSliderRow ("lookaheadBeats", "Lookahead (beats, 0=live)", 0, 16);
    addSliderRow ("delayCompensationCc", "Delay Comp. CC# (0=off)", 0, 127);
    addSliderRow ("difficultyCeiling", "Difficulty Ceiling (0=off)", 0.0, 1.0);
    addChoiceRow ("revoice", "Re-voice", { "Off", "Framework", "Close" });
    addChoiceRow ("lowIntervalStrictness", "Low-Interval Strictness", { "Off", "Loose", "Strict" });
    addChoiceRow ("dynamicContour", "Dynamic Contour", { "Off", "Preserve" });
    addSliderRow ("maxRingBeats", "Max Ring (beats, 0=off)", 0, 8);
    addChoiceRow ("keepBassOctaves", "Keep Bass Octaves", { "Off", "Keep", "Add" });
    addToggleRow ("keepMelodyOctaves", "Keep Melody Octaves");
    addToggleRow ("dampSuccessive", "Damp On Next Attack");
    addToggleRow ("decisionLog", "Write Decision Log");
    addSliderRow ("onsetWindowMs", "Onset Window (ms)", 5, 200);
    addSliderRow ("outChannelBase", "Out Channel Base (+0..+5)", 1, 11);
    addSliderRow ("wMelodyBass", "Weight: Melody/Bass", 0.0, 2.0);
    addSliderRow ("wVelocity", "Weight: Velocity", 0.0, 2.0);
    addSliderRow ("wDouble", "Weight: Doubling Penalty", 0.0, 2.0);

    statusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::aqua);
    addAndMakeVisible (statusLabel);

    setSize (420, kPad * 3 + 40 + kRowH * static_cast<int> (rows.size()) + 30);
    startTimerHz (12);
}

OrchPianoAudioProcessorEditor::~OrchPianoAudioProcessorEditor() { stopTimer(); }

void OrchPianoAudioProcessorEditor::addChoiceRow (const juce::String& paramId, const juce::String& text,
                                                  const juce::StringArray& choices)
{
    auto row = std::make_unique<Row>();
    row->label.setText (text, juce::dontSendNotification);
    addAndMakeVisible (row->label);

    auto box = std::make_unique<juce::ComboBox>();
    box->addItemList (choices, 1);
    addAndMakeVisible (*box);
    row->comboAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getParameters(), paramId, *box);
    row->control = std::move (box);

    rows.push_back (std::move (row));
}

void OrchPianoAudioProcessorEditor::addSliderRow (const juce::String& paramId, const juce::String& text,
                                                  double lo, double hi)
{
    auto row = std::make_unique<Row>();
    row->label.setText (text, juce::dontSendNotification);
    addAndMakeVisible (row->label);

    const double interval = (hi - lo) <= 2.0 ? 0.01 : 1.0;
    auto s = std::make_unique<juce::Slider>(
        interval < 1.0 ? juce::Slider::LinearHorizontal : juce::Slider::IncDecButtons,
        juce::Slider::TextBoxLeft);
    s->setRange (lo, hi, interval);
    s->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 48, kRowH - 6);
    addAndMakeVisible (*s);
    row->sliderAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getParameters(), paramId, *s);
    row->control = std::move (s);

    rows.push_back (std::move (row));
}

void OrchPianoAudioProcessorEditor::addToggleRow (const juce::String& paramId, const juce::String& text)
{
    auto row = std::make_unique<Row>();
    row->label.setText (text, juce::dontSendNotification);
    addAndMakeVisible (row->label);

    auto b = std::make_unique<juce::ToggleButton>();
    addAndMakeVisible (*b);
    row->buttonAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getParameters(), paramId, *b);
    row->control = std::move (b);

    rows.push_back (std::move (row));
}

void OrchPianoAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1e1e22));
}

void OrchPianoAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (kPad);

    titleLabel.setBounds (area.removeFromTop (26));
    buildLabel.setBounds (area.removeFromTop (16));
    area.removeFromTop (6);

    for (auto& row : rows)
    {
        auto r = area.removeFromTop (kRowH);
        row->label.setBounds (r.removeFromLeft (180));
        row->control->setBounds (r.reduced (2, 2));
    }

    area.removeFromTop (6);
    statusLabel.setBounds (area.removeFromTop (20));
}

void OrchPianoAudioProcessorEditor::timerCallback()
{
    const int seen    = audioProcessor.getLastSeenForUi();
    const int kept    = audioProcessor.getLastKeptForUi();
    const int dropped = audioProcessor.getLastDroppedForUi();
    const int mel     = audioProcessor.getLastMelodyForUi();
    const int bass    = audioProcessor.getLastBassForUi();

    auto noteName = [] (int n) {
        return n < 0 ? juce::String ("-")
                     : juce::MidiMessage::getMidiNoteName (n, true, true, 3);
    };

    const int split = audioProcessor.getAdaptiveSplitForUi();
    const int buf   = audioProcessor.getPlanBufferForUi();

    statusLabel.setText ("group " + juce::String (seen) + " in / " + juce::String (kept)
                         + " kept / " + juce::String (dropped) + " dropped   melody "
                         + noteName (mel) + "  bass " + noteName (bass)
                         + "   split " + noteName (split) + "  buf " + juce::String (buf),
                         juce::dontSendNotification);
}
