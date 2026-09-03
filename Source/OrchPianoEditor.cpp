#include "OrchPianoEditor.h"

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

    buildLabel.setText ("Build: Phase 2 (streaming hand-split reducer)", juce::dontSendNotification);
    buildLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    buildLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible (buildLabel);

    addChoiceRow ("operatingMode", "Mode", { "Repair", "Reduce", "Transform" });
    addChoiceRow ("hands", "Hands", { "Both", "Left", "Right" });
    addChoiceRow ("maxVoices", "Max Voices", { "4", "6" });
    addSliderRow ("splitNote", "Hand Split Note", 0, 127);
    addSliderRow ("maxNotesPerHand", "Notes / Hand", 2, 6);
    addSliderRow ("maxSpan", "Max Hand Span (st)", 8, 16);
    addSliderRow ("crossoverSlack", "Crossover Slack (st)", 0, 12);
    addChoiceRow ("protect", "Protect", { "None", "Lowest", "Highest", "Both Ends" });
    addToggleRow ("dampSuccessive", "Damp On Next Attack");
    addSliderRow ("onsetWindowMs", "Onset Window (ms)", 5, 200);
    addSliderRow ("outChannelBase", "Out Channel (Right; Left = +1)", 1, 15);

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

    auto s = std::make_unique<juce::Slider>(juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft);
    s->setRange (lo, hi, 1.0);
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
    const int seen  = audioProcessor.getLastSeenForUi();
    const int kept  = audioProcessor.getLastKeptForUi();
    const int left  = audioProcessor.getLastLeftForUi();
    const int right = audioProcessor.getLastRightForUi();

    auto noteName = [] (int n) {
        return n < 0 ? juce::String ("-")
                     : juce::MidiMessage::getMidiNoteName (n, true, true, 3);
    };

    statusLabel.setText ("group: " + juce::String (seen) + " in / " + juce::String (kept)
                         + " kept   L top " + noteName (left) + "   R top " + noteName (right),
                         juce::dontSendNotification);
}
