#include "PluginEditor.h"

HardInsertEditor::HardInsertEditor (HardInsertProcessor& p)
    : AudioProcessorEditor (&p),
      processor (p),
      inputAttachment (p.apvts, "inputTrim", inputSlider),
      outputAttachment (p.apvts, "outputTrim", outputSlider),
      monoAttachment (p.apvts, "monoMode", monoButton)
{
    setSize (420, 290);

    for (auto* slider : { &inputSlider, &outputSlider })
    {
        slider->setSliderStyle (juce::Slider::RotaryVerticalDrag);
        slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 18);
        slider->setTextValueSuffix (" dB");
        addAndMakeVisible (*slider);
    }

    inputLabel.setText ("Input Trim", juce::dontSendNotification);
    outputLabel.setText ("Output Trim", juce::dontSendNotification);
    inputLabel.setJustificationType (juce::Justification::centred);
    outputLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setJustificationType (juce::Justification::centredLeft);

    addAndMakeVisible (inputLabel);
    addAndMakeVisible (outputLabel);
    addAndMakeVisible (statusLabel);
    addAndMakeVisible (monoButton);
    addAndMakeVisible (measureButton);

    measureButton.onClick = [this] { processor.requestLatencyMeasurement(); };

    startTimerHz (10);
}

HardInsertEditor::~HardInsertEditor()
{
    stopTimer();
}

void HardInsertEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1e1e1e));
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (18.0f));
    g.drawText ("Hard-Insert", getLocalBounds().removeFromTop (36), juce::Justification::centred);
}

void HardInsertEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    area.removeFromTop (30);

    auto knobRow = area.removeFromTop (160);
    auto left = knobRow.removeFromLeft (knobRow.getWidth() / 2);

    inputLabel.setBounds (left.removeFromTop (20));
    inputSlider.setBounds (left);
    outputLabel.setBounds (knobRow.removeFromTop (20));
    outputSlider.setBounds (knobRow);

    area.removeFromTop (10);
    monoButton.setBounds (area.removeFromTop (28));
    area.removeFromTop (8);
    measureButton.setBounds (area.removeFromTop (30));
    area.removeFromTop (8);
    statusLabel.setBounds (area.removeFromTop (24));
}

void HardInsertEditor::timerCallback()
{
    processor.pollMeasurement();
    statusLabel.setText (processor.getStatusText(), juce::dontSendNotification);
    measureButton.setEnabled (! processor.isMeasuring());
}
