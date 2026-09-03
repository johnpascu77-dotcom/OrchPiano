#pragma once

#include <memory>
#include <vector>
#include <JuceHeader.h>

#include "OrchPianoProcessor.h"

class OrchPianoAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                            private juce::Timer
{
public:
    explicit OrchPianoAudioProcessorEditor (OrchPianoAudioProcessor&);
    ~OrchPianoAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    OrchPianoAudioProcessor& audioProcessor;

    juce::Label titleLabel, buildLabel, statusLabel;

    struct Row
    {
        juce::Label label;
        std::unique_ptr<juce::Component> control;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAtt;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAtt;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAtt;
    };
    std::vector<std::unique_ptr<Row>> rows;

    void addChoiceRow (const juce::String& paramId, const juce::String& text, const juce::StringArray& choices);
    void addSliderRow (const juce::String& paramId, const juce::String& text, double lo, double hi);
    void addToggleRow (const juce::String& paramId, const juce::String& text);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchPianoAudioProcessorEditor)
};
