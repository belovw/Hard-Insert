#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

class InsertPanel;
class DevicePanel;

class HardInsertEditor final : public juce::AudioProcessorEditor
{
public:
    explicit HardInsertEditor (HardInsertProcessor&);
    ~HardInsertEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    HardInsertProcessor& processorRef;

    std::unique_ptr<InsertPanel> insertPanel;
    std::unique_ptr<DevicePanel> devicePanel;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HardInsertEditor)
};
