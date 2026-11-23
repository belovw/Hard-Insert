#pragma once

#include "PluginProcessor.h"

class HardInsertEditor final : public juce::AudioProcessorEditor,
                               private juce::Timer
{
public:
    explicit HardInsertEditor (HardInsertProcessor&);
    ~HardInsertEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    HardInsertProcessor& processor;

    juce::Slider inputSlider, outputSlider;
    juce::Label inputLabel, outputLabel, statusLabel;
    juce::ToggleButton monoButton { "Force Mono (L+R)/2" };
    juce::TextButton measureButton { "Measure Latency" };

    juce::AudioProcessorValueTreeState::SliderAttachment inputAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment outputAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment monoAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HardInsertEditor)
};
