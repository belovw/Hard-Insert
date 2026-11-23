#include "PluginEditor.h"

namespace
{
    const juce::Colour kBackground (0xff1d1f22);
    const juce::Colour kPanel      (0xff26292d);
    const juce::Colour kText       (0xffe6e6e6);
    const juce::Colour kAccent     (0xff4fb3bf);

    //==============================================================================
    class LevelMeter final : public juce::Component
    {
    public:
        explicit LevelMeter (juce::String name) : label (std::move (name)) {}

        void setPeak (float newPeak)
        {
            level = juce::jmax (newPeak, level * 0.8f);
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto bounds = getLocalBounds().toFloat();
            g.setColour (kPanel.brighter (0.05f));
            g.fillRoundedRectangle (bounds, 3.0f);

            const float db = juce::Decibels::gainToDecibels (level, -60.0f);
            const float proportion = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);

            g.setColour (db > -0.5f ? juce::Colours::red
                                    : db > -12.0f ? juce::Colours::orange
                                                  : juce::Colours::limegreen.darker (0.2f));
            g.fillRoundedRectangle (bounds.withWidth (bounds.getWidth() * proportion), 3.0f);

            g.setColour (kText);
            g.setFont (juce::FontOptions (12.0f));
            g.drawText (label + ":  " + (level > 1.0e-6f ? juce::String (db, 1) + " dBFS" : juce::String ("-inf")),
                        getLocalBounds().reduced (8, 0), juce::Justification::centredLeft);
        }

    private:
        juce::String label;
        float level = 0.0f;
    };
}

//==============================================================================
/** Main page: hardware channels, trims, mono, safety buffer, ping, meters. */
class InsertPanel final : public juce::Component,
                          private juce::Timer
{
public:
    explicit InsertPanel (HardInsertProcessor& p)
        : proc (p),
          inputAttachment  (p.apvts, "inputTrim",  inputSlider),
          outputAttachment (p.apvts, "outputTrim", outputSlider),
          monoAttachment   (p.apvts, "monoMode",   monoButton)
    {
        using Channel = HardInsertProcessor::Channel;

        const auto setupCombo = [this] (juce::ComboBox& box, juce::Label& label, Channel which)
        {
            addAndMakeVisible (box);
            addAndMakeVisible (label);
            label.setColour (juce::Label::textColourId, kText);
            box.onChange = [this, &box, which] { proc.setChannel (which, box.getSelectedId() - 2); };
        };

        setupCombo (sendLeftBox,    sendLeftLabel,    Channel::sendLeft);
        setupCombo (sendRightBox,   sendRightLabel,   Channel::sendRight);
        setupCombo (returnLeftBox,  returnLeftLabel,  Channel::returnLeft);
        setupCombo (returnRightBox, returnRightLabel, Channel::returnRight);

        safetyBox.addItem ("Auto", kAutoId);

        for (int size : { 512, 1024, 2048, 4096, 8192, 16384 })
            safetyBox.addItem (juce::String (size) + " samples", size);

        safetyBox.onChange = [this]
        {
            const int id = safetyBox.getSelectedId();
            proc.setSafetyBuffer (id == kAutoId ? 0 : id);
        };
        safetyBox.setTooltip ("Buffering between the DAW and the interface, included in the PDC. "
                              "Auto sizes it from the DAW and device buffer sizes; "
                              "choose a larger value if you hear dropouts.");
        addAndMakeVisible (safetyBox);
        addAndMakeVisible (safetyLabel);
        safetyLabel.setColour (juce::Label::textColourId, kText);

        monoButton.setTooltip ("Sum the track to (L+R)/2 and use only the left send/return channels.");
        addAndMakeVisible (monoButton);

        for (auto* slider : { &inputSlider, &outputSlider })
        {
            slider->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 20);
            slider->setColour (juce::Slider::rotarySliderFillColourId, kAccent);
            addAndMakeVisible (*slider);
        }

        for (auto* label : { &inputLabel, &outputLabel })
        {
            label->setJustificationType (juce::Justification::centred);
            label->setColour (juce::Label::textColourId, kText);
            addAndMakeVisible (*label);
        }

        addAndMakeVisible (sendMeter);
        addAndMakeVisible (returnMeter);

        measureButton.setTooltip ("Sends one impulse through the hardware and measures when it comes back. "
                                  "Use a loopback cable or the gear itself.");
        measureButton.onClick = [this] { proc.startLatencyMeasurement(); };
        addAndMakeVisible (measureButton);

        for (auto* label : { &latencyLabel, &statusLabel })
        {
            label->setJustificationType (juce::Justification::topLeft);
            label->setColour (juce::Label::textColourId, kText);
            label->setFont (juce::FontOptions (13.0f));
            addAndMakeVisible (*label);
        }

        timerCallback();
        startTimerHz (15);
    }

    ~InsertPanel() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (kBackground);
        g.setColour (kPanel);
        g.fillRoundedRectangle (routingArea.toFloat(), 6.0f);
        g.fillRoundedRectangle (gainArea.toFloat(), 6.0f);
        g.fillRoundedRectangle (infoArea.toFloat(), 6.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);

        infoArea = area.removeFromBottom (118);
        area.removeFromBottom (10);
        routingArea = area.removeFromLeft (area.getWidth() / 2 - 5);
        area.removeFromLeft (10);
        gainArea = area;

        // Routing column
        auto r = routingArea.reduced (10);
        const auto row = [&r] (juce::Component& label, juce::Component& box)
        {
            auto line = r.removeFromTop (26);
            label.setBounds (line.removeFromLeft (110));
            box.setBounds (line);
            r.removeFromTop (6);
        };

        row (sendLeftLabel,    sendLeftBox);
        row (sendRightLabel,   sendRightBox);
        row (returnLeftLabel,  returnLeftBox);
        row (returnRightLabel, returnRightBox);
        monoButton.setBounds (r.removeFromTop (26));
        r.removeFromTop (6);
        row (safetyLabel, safetyBox);

        // Gain column
        auto gr = gainArea.reduced (10);
        auto meters = gr.removeFromBottom (48);
        sendMeter.setBounds (meters.removeFromTop (22));
        meters.removeFromTop (4);
        returnMeter.setBounds (meters.removeFromTop (22));

        auto labels = gr.removeFromTop (20);
        inputLabel.setBounds (labels.removeFromLeft (labels.getWidth() / 2));
        outputLabel.setBounds (labels);
        inputSlider.setBounds (gr.removeFromLeft (gr.getWidth() / 2).reduced (4));
        outputSlider.setBounds (gr.reduced (4));

        // Info area
        auto ir = infoArea.reduced (10);
        auto top = ir.removeFromTop (28);
        measureButton.setBounds (top.removeFromLeft (200));
        top.removeFromLeft (10);
        latencyLabel.setBounds (top.getX(), top.getY(), ir.getRight() - top.getX(), 44);
        ir.removeFromTop (20);
        statusLabel.setBounds (ir);
    }

private:
    void timerCallback() override
    {
        refreshChannelLists();

        using Channel = HardInsertProcessor::Channel;
        syncSelection (sendLeftBox,    proc.getChannel (Channel::sendLeft));
        syncSelection (sendRightBox,   proc.getChannel (Channel::sendRight));
        syncSelection (returnLeftBox,  proc.getChannel (Channel::returnLeft));
        syncSelection (returnRightBox, proc.getChannel (Channel::returnRight));

        const int setting = proc.getSafetyBuffer();
        const int wantedId = setting > 0 ? setting : kAutoId;

        if (safetyBox.indexOfItemId (wantedId) < 0)
            safetyBox.addItem (juce::String (setting) + " samples", wantedId);

        if (safetyBox.getSelectedId() != wantedId)
            safetyBox.setSelectedId (wantedId, juce::dontSendNotification);

        safetyBox.changeItemText (kAutoId, "Auto (" + juce::String (proc.getEffectiveSafetyBuffer()) + " samples)");

        const bool mono = monoButton.getToggleState();
        sendRightBox.setEnabled (! mono);
        returnRightBox.setEnabled (! mono);
        sendRightLabel.setEnabled (! mono);
        returnRightLabel.setEnabled (! mono);

        const bool measuring = proc.isMeasuring();
        measureButton.setEnabled (! measuring);
        measureButton.setButtonText (measuring ? "Measuring..." : "Measure Latency (ping)");

        latencyLabel.setText (proc.getLatencyText(), juce::dontSendNotification);
        statusLabel.setText (proc.getStatusText(), juce::dontSendNotification);

        sendMeter.setPeak (proc.takeSendPeak());
        returnMeter.setPeak (proc.takeReturnPeak());
    }

    void refreshChannelLists()
    {
        auto* device = proc.getDeviceManager().getCurrentAudioDevice();

        juce::String signature;
        juce::StringArray outputNames, inputNames;
        juce::BigInteger activeOutputs, activeInputs;

        if (device != nullptr)
        {
            outputNames   = device->getOutputChannelNames();
            inputNames    = device->getInputChannelNames();
            activeOutputs = device->getActiveOutputChannels();
            activeInputs  = device->getActiveInputChannels();
            signature << device->getTypeName() << "|" << device->getName() << "|"
                      << activeOutputs.toString (16) << "|" << activeInputs.toString (16);
        }

        if (signature == channelSignature)
            return;

        channelSignature = signature;

        using Channel = HardInsertProcessor::Channel;
        fillCombo (sendLeftBox,    outputNames, activeOutputs, proc.getChannel (Channel::sendLeft));
        fillCombo (sendRightBox,   outputNames, activeOutputs, proc.getChannel (Channel::sendRight));
        fillCombo (returnLeftBox,  inputNames,  activeInputs,  proc.getChannel (Channel::returnLeft));
        fillCombo (returnRightBox, inputNames,  activeInputs,  proc.getChannel (Channel::returnRight));
    }

    static void fillCombo (juce::ComboBox& box, const juce::StringArray& names,
                           const juce::BigInteger& active, int selected)
    {
        box.clear (juce::dontSendNotification);
        box.addItem ("- none -", 1);

        for (int ch = 0; ch < names.size(); ++ch)
            if (active[ch])
                box.addItem (juce::String (ch + 1) + ": " + names[ch], ch + 2);

        if (selected >= 0 && box.indexOfItemId (selected + 2) < 0)
            box.addItem (juce::String (selected + 1) + ": (not available)", selected + 2);

        box.setSelectedId (selected >= 0 ? selected + 2 : 1, juce::dontSendNotification);
    }

    static void syncSelection (juce::ComboBox& box, int channel)
    {
        const int id = channel >= 0 ? channel + 2 : 1;

        if (box.getSelectedId() != id && box.indexOfItemId (id) >= 0)
            box.setSelectedId (id, juce::dontSendNotification);
    }

    static constexpr int kAutoId = 1;

    HardInsertProcessor& proc;

    juce::Label sendLeftLabel    { {}, "Send L (out)" };
    juce::Label sendRightLabel   { {}, "Send R (out)" };
    juce::Label returnLeftLabel  { {}, "Return L (in)" };
    juce::Label returnRightLabel { {}, "Return R (in)" };
    juce::Label safetyLabel      { {}, "Safety buffer" };
    juce::ComboBox sendLeftBox, sendRightBox, returnLeftBox, returnRightBox, safetyBox;
    juce::ToggleButton monoButton { "Force Mono (L+R)/2" };

    juce::Slider inputSlider, outputSlider;
    juce::Label inputLabel  { {}, "Input Trim" };
    juce::Label outputLabel { {}, "Output Trim" };
    LevelMeter sendMeter { "Send" }, returnMeter { "Return" };

    juce::TextButton measureButton { "Measure Latency (ping)" };
    juce::Label latencyLabel, statusLabel;

    juce::AudioProcessorValueTreeState::SliderAttachment inputAttachment, outputAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment monoAttachment;

    juce::Rectangle<int> routingArea, gainArea, infoArea;
    juce::String channelSignature { "-" };

    JUCE_DECLARE_NON_COPYABLE (InsertPanel)
};

//==============================================================================
/** Second page: the audio interface used by the plugin (shared by all instances). */
class DevicePanel final : public juce::Component
{
public:
    explicit DevicePanel (juce::AudioDeviceManager& manager)
        : selector (manager, 0, hi::SharedDevice::kMaxChannels, 0, hi::SharedDevice::kMaxChannels,
                    false, false, false, false)
    {
        hint.setText ("This device is opened by the plugin itself and shared by every Hard-Insert instance. "
                      "Use the interface's ASIO driver if it allows a second client; otherwise use "
                      "Windows Audio on the same interface. The sample rate must match the DAW.",
                      juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, kText);
        hint.setFont (juce::FontOptions (13.0f));
        hint.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (hint);

        selector.setSize (500, 420);
        viewport.setViewedComponent (&selector, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
    }

    void paint (juce::Graphics& g) override { g.fillAll (kBackground); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);
        hint.setBounds (area.removeFromTop (52));
        area.removeFromTop (6);
        viewport.setBounds (area);
        selector.setSize (area.getWidth() - viewport.getScrollBarThickness(), juce::jmax (selector.getHeight(), 300));
    }

private:
    juce::Label hint;
    juce::AudioDeviceSelectorComponent selector;
    juce::Viewport viewport;

    JUCE_DECLARE_NON_COPYABLE (DevicePanel)
};

//==============================================================================
HardInsertEditor::HardInsertEditor (HardInsertProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    // Opening the editor is enough to configure the device.
    p.connectToDevice();

    insertPanel = std::make_unique<InsertPanel> (p);
    devicePanel = std::make_unique<DevicePanel> (p.getDeviceManager());

    tabs.addTab ("Insert",       kBackground, insertPanel.get(), false);
    tabs.addTab ("Audio Device", kBackground, devicePanel.get(), false);
    tabs.setTabBarDepth (30);
    tabs.setOutline (0);
    addAndMakeVisible (tabs);

    // Open the device page first if nothing is connected yet.
    if (p.getDeviceManager().getCurrentAudioDevice() == nullptr)
        tabs.setCurrentTabIndex (1);

    setResizable (true, true);
    setResizeLimits (560, 460, 1200, 1000);
    setSize (640, 500);
}

HardInsertEditor::~HardInsertEditor()
{
    tabs.clearTabs();
}

void HardInsertEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);
    g.setColour (kAccent);
    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    g.drawText ("HARD-INSERT", getLocalBounds().removeFromTop (30).withTrimmedRight (12),
                juce::Justification::centredRight);
}

void HardInsertEditor::resized()
{
    tabs.setBounds (getLocalBounds());
}
