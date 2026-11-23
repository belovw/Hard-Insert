#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr const char* kInputTrimId  = "inputTrim";
    constexpr const char* kOutputTrimId = "outputTrim";
    constexpr const char* kMonoModeId   = "monoMode";

    constexpr const char* kSafetyBufferProp = "safetyBuffer";
    constexpr int kDefaultSafetyBuffer = 0;   // Auto

    const char* channelProperty (HardInsertProcessor::Channel c)
    {
        switch (c)
        {
            case HardInsertProcessor::Channel::sendLeft:    return "sendLeft";
            case HardInsertProcessor::Channel::sendRight:   return "sendRight";
            case HardInsertProcessor::Channel::returnLeft:  return "returnLeft";
            case HardInsertProcessor::Channel::returnRight: return "returnRight";
        }

        return "sendLeft";
    }

    constexpr float kPingLevel = 0.9f;
    constexpr int kMeasureCaptureSize = 1 << 19;
}

//==============================================================================
HardInsertProcessor::HardInsertProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "HardInsert", createParameterLayout())
{
    inputTrimParam  = apvts.getRawParameterValue (kInputTrimId);
    outputTrimParam = apvts.getRawParameterValue (kOutputTrimId);
    monoParam       = apvts.getRawParameterValue (kMonoModeId);

    measureCapture.resize ((size_t) kMeasureCaptureSize, 0.0f);

    applySettingsToPort();
    updateReportedLatency();
    startTimerHz (10);
}

HardInsertProcessor::~HardInsertProcessor()
{
    stopTimer();

    // After this returns the device thread no longer calls into the port.
    if (connected)
        sharedDevice->getManager().removeAudioCallback (&port);
}

void HardInsertProcessor::connectToDevice()
{
    if (connected)
        return;

    sharedDevice->ensureOpen();
    sharedDevice->getManager().addAudioCallback (&port);
    connected = true;
}

juce::AudioProcessorValueTreeState::ParameterLayout HardInsertProcessor::createParameterLayout()
{
    using Range = juce::NormalisableRange<float>;

    const auto dbText = [] (float value, int) { return juce::String (value, 1) + " dB"; };

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { kInputTrimId, 1 }, "Input Trim", Range (-24.0f, 24.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbText)));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { kOutputTrimId, 1 }, "Output Trim", Range (-24.0f, 24.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbText)));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { kMonoModeId, 1 }, "Force Mono", false));

    return layout;
}

//==============================================================================
void HardInsertProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    dawRate.store (sampleRate);
    dawBlockSize.store (juce::jmax (1, samplesPerBlock));
    deviceWanted.store (true);   // the timer opens the device on the message thread

    const int chunk = juce::jmax (512, samplesPerBlock);
    sendBuffer.setSize (2, chunk, false, false, true);
    returnBuffer.setSize (2, chunk, false, false, true);

    inputGain.reset (sampleRate, 0.02);
    outputGain.reset (sampleRate, 0.02);
    inputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (inputTrimParam->load()));
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputTrimParam->load()));

    port.requestResync();
    refreshSafetyBuffer();
    updateReportedLatency();
}

bool HardInsertProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void HardInsertProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int total = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    const int chunk = sendBuffer.getNumSamples();

    if (numChannels == 0 || total == 0)
        return;

    for (int ch = 2; ch < numChannels; ++ch)
        buffer.clear (ch, 0, total);

    if (chunk == 0)
    {
        buffer.clear();
        return;
    }

    inputGain.setTargetValue (juce::Decibels::decibelsToGain (inputTrimParam->load()));
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (outputTrimParam->load()));

    float* left  = buffer.getWritePointer (0);
    float* right = numChannels > 1 ? buffer.getWritePointer (1) : nullptr;

    // Hosts (FL Studio in particular) may send blocks larger than announced.
    for (int pos = 0; pos < total; pos += chunk)
    {
        const int n = juce::jmin (chunk, total - pos);
        processChunk (left + pos, right != nullptr ? right + pos : nullptr, n);
    }
}

void HardInsertProcessor::processChunk (float* left, float* right, int n) noexcept
{
    float* sendL = sendBuffer.getWritePointer (0);
    float* sendR = sendBuffer.getWritePointer (1);
    float* retL  = returnBuffer.getWritePointer (0);
    float* retR  = returnBuffer.getWritePointer (1);

    const bool mono = monoParam->load() > 0.5f || right == nullptr;

    // 1. Send signal: input trim, optional (L+R)/2 sum.
    juce::FloatVectorOperations::copy (sendL, left, n);
    juce::FloatVectorOperations::copy (sendR, right != nullptr ? right : left, n);

    for (int i = 0; i < n; ++i)
    {
        const float g = inputGain.getNextValue();
        sendL[i] *= g;
        sendR[i] *= g;
    }

    if (mono)
    {
        juce::FloatVectorOperations::add (sendL, sendR, n);
        juce::FloatVectorOperations::multiply (sendL, 0.5f, n);
    }

    // 2. Latency measurement: replace the send signal with a single impulse.
    int stage = measureStage.load();

    if (stage == kMeasureRequested)
    {
        measurePos = 0;
        measureSafety = port.getSafetyBuffer();
        measureResyncs = port.getResyncCount();
        measureLength = juce::jmin ((int) measureCapture.size(),
                                    measureSafety + port.getDeviceRoundTripEstimate() + (int) dawRate.load());
        measureFailed.store (false);
        stage = kMeasureCapturing;
        measureStage.store (stage);
    }

    const bool measuring = (stage == kMeasureCapturing);

    if (measuring)
    {
        juce::FloatVectorOperations::clear (sendL, n);
        juce::FloatVectorOperations::clear (sendR, n);

        if (measurePos == 0)
        {
            sendL[0] = kPingLevel;
            sendR[0] = kPingLevel;
        }
    }
    else
    {
        updatePeak (sendPeak, sendL, n);

        if (! mono)
            updatePeak (sendPeak, sendR, n);
    }

    // 3. Exchange with the hardware. The device must run at the DAW sample rate.
    const double deviceRate = port.getDeviceSampleRate();
    const bool mismatch = port.isDeviceRunning() && std::abs (deviceRate - dawRate.load()) > 1.0;
    rateMismatch.store (mismatch);

    bool running = false;

    if (mismatch)
    {
        juce::FloatVectorOperations::clear (retL, n);
        juce::FloatVectorOperations::clear (retR, n);
    }
    else
    {
        running = port.exchange (sendL, mono ? nullptr : sendR, retL, retR, n);
    }

    // 4. Capture the ping, keep the track silent while measuring.
    if (measuring)
    {
        if (! running || port.getResyncCount() != measureResyncs)
        {
            measureFailed.store (true);
            measureStage.store (kMeasureCaptured);
        }
        else
        {
            const int count = juce::jmin (n, measureLength - measurePos);
            std::copy (retL, retL + count, measureCapture.begin() + measurePos);
            measurePos += count;

            if (measurePos >= measureLength)
                measureStage.store (kMeasureCaptured);
        }

        juce::FloatVectorOperations::clear (left, n);

        if (right != nullptr)
            juce::FloatVectorOperations::clear (right, n);

        for (int i = 0; i < n; ++i)
            outputGain.getNextValue();

        return;
    }

    updatePeak (returnPeak, retL, n);

    if (! mono)
        updatePeak (returnPeak, retR, n);

    // 5. Return signal back to the track: output trim, mono return on both sides.
    const float* outR = mono ? retL : retR;

    for (int i = 0; i < n; ++i)
    {
        const float g = outputGain.getNextValue();
        left[i] = retL[i] * g;

        if (right != nullptr)
            right[i] = outR[i] * g;
    }
}

void HardInsertProcessor::updatePeak (std::atomic<float>& peak, const float* data, int n) noexcept
{
    const auto range = juce::FloatVectorOperations::findMinAndMax (data, n);
    const float value = juce::jmax (std::abs (range.getStart()), std::abs (range.getEnd()));

    if (value > peak.load())
        peak.store (value);
}

//==============================================================================
int HardInsertProcessor::getChannel (Channel c) const
{
    return (int) apvts.state.getProperty (channelProperty (c), -1);
}

void HardInsertProcessor::setChannel (Channel c, int physicalChannel)
{
    apvts.state.setProperty (channelProperty (c), juce::jmax (-1, physicalChannel), nullptr);
    applySettingsToPort();
}

int HardInsertProcessor::getSafetyBuffer() const
{
    return (int) apvts.state.getProperty (kSafetyBufferProp, kDefaultSafetyBuffer);
}

void HardInsertProcessor::setSafetyBuffer (int samples)
{
    apvts.state.setProperty (kSafetyBufferProp, juce::jmax (0, samples), nullptr);
    applySettingsToPort();
    updateReportedLatency();
}

void HardInsertProcessor::applySettingsToPort()
{
    port.setChannels (getChannel (Channel::sendLeft), getChannel (Channel::sendRight),
                      getChannel (Channel::returnLeft), getChannel (Channel::returnRight));

    safetySetting.store (getSafetyBuffer());
    refreshSafetyBuffer();
}

void HardInsertProcessor::refreshSafetyBuffer() noexcept
{
    // Auto: enough cushion for one DAW block + one device block + jitter on each side.
    const int setting = safetySetting.load();
    const int p = setting > 0 ? setting
                              : hi::HardwarePort::autoSafetyBuffer (dawBlockSize.load(), port.getDeviceBufferSize());

    if (port.getSafetyBuffer() != p)
        port.setSafetyBuffer (p);   // resets the loop; the PDC follows in updateReportedLatency()
}

//==============================================================================
void HardInsertProcessor::startLatencyMeasurement()
{
    int expected = kMeasureIdle;

    if (measureStage.compare_exchange_strong (expected, kMeasureRequested))
        measureMessage = "Measuring...";
}

void HardInsertProcessor::timerCallback()
{
    if (deviceWanted.load() && ! connected)
        connectToDevice();

    if (measureStage.load() == kMeasureCaptured)
    {
        analyseMeasurement();
        measureStage.store (kMeasureIdle);
    }

    refreshSafetyBuffer();   // the device buffer size may have changed
    updateReportedLatency();
}

void HardInsertProcessor::analyseMeasurement()
{
    if (measureFailed.load())
    {
        measureMessage = "Measurement failed: the audio loop is not running or dropped out. Try again.";
        return;
    }

    const int length = measureLength;

    // The ping comes back as the strongest peak (same as cross-correlation with an impulse).
    int peakIndex = 0;
    float peakAbs = 0.0f;

    for (int i = 0; i < length; ++i)
    {
        const float a = std::abs (measureCapture[(size_t) i]);

        if (a > peakAbs)
        {
            peakAbs = a;
            peakIndex = i;
        }
    }

    double noiseSum = 0.0;
    int noiseCount = 0;

    for (int i = 0; i < length; ++i)
    {
        if (std::abs (i - peakIndex) > 256)
        {
            noiseSum += std::abs (measureCapture[(size_t) i]);
            ++noiseCount;
        }
    }

    const float noise = noiseCount > 0 ? (float) (noiseSum / noiseCount) : 0.0f;

    if (peakAbs < 1.0e-4f || peakAbs < noise * 10.0f)
    {
        measureMessage = "No clear ping in the return. Check cables, channels and levels, then try again.";
        return;
    }

    const int roundTrip = peakIndex - measureSafety;

    if (roundTrip < 0)
    {
        measureMessage = "Unexpected result (the return arrived before the send). Check the routing.";
        return;
    }

    const int rate = (int) std::lround (dawRate.load());

    {
        const juce::ScopedLock sl (latencyLock);
        measuredRoundTrip[rate] = roundTrip;
    }

    const bool inverted = measureCapture[(size_t) peakIndex] < 0.0f;

    measureMessage = "Ping: hardware round trip " + juce::String (roundTrip) + " samples ("
                   + juce::String (roundTrip * 1000.0 / juce::jmax (1, rate), 2) + " ms), peak "
                   + juce::String (juce::Decibels::gainToDecibels (peakAbs), 1) + " dB, polarity "
                   + (inverted ? "INVERTED" : "normal") + ".";

    updateReportedLatency();
}

int HardInsertProcessor::getMeasuredRoundTrip() const
{
    const int rate = (int) std::lround (dawRate.load());
    const juce::ScopedLock sl (latencyLock);

    if (auto it = measuredRoundTrip.find (rate); it != measuredRoundTrip.end())
        return it->second;

    return -1;
}

void HardInsertProcessor::updateReportedLatency()
{
    int roundTrip = getMeasuredRoundTrip();

    if (roundTrip < 0)
        roundTrip = port.getDeviceRoundTripEstimate();

    // The port's copy is atomic: this may be called from prepareToPlay on the audio thread.
    const int latency = port.getSafetyBuffer() + roundTrip;

    if (reportedLatency.exchange (latency) != latency)
        setLatencySamples (latency);
}

//==============================================================================
juce::String HardInsertProcessor::getStatusText() const
{
    if (! connected)
        return "Waiting for the DAW to start audio...";

    auto* device = sharedDevice->getManager().getCurrentAudioDevice();

    if (device == nullptr)
    {
        juce::String text = "No audio device is open. Choose one on the 'Audio Device' tab.";

        if (sharedDevice->getInitError().isNotEmpty())
            text << "\nError: " << sharedDevice->getInitError();

        return text;
    }

    juce::String text;
    text << device->getTypeName() << ": " << device->getName() << ", "
         << (int) device->getCurrentSampleRate() << " Hz, buffer "
         << device->getCurrentBufferSizeSamples() << "\n";

    const bool noSend   = getChannel (Channel::sendLeft) < 0;
    const bool noReturn = getChannel (Channel::returnLeft) < 0;

    if (rateMismatch.load())
        text << "Sample rate mismatch: DAW " << (int) dawRate.load() << " Hz, device "
             << (int) port.getDeviceSampleRate() << " Hz. Set the same rate in both.";
    else if (noSend || noReturn)
        text << "Choose the Send and Return channels.";
    else if (port.isLoopRunning())
        text << "Loop running.";
    else if (! port.isDeviceRunning())
        text << "The device is not running.";
    else
        text << "Waiting for audio from the DAW...";

    if (port.getResyncCount() > 0)
        text << "  Resyncs: " << port.getResyncCount();

    return text;
}

juce::String HardInsertProcessor::getLatencyText() const
{
    const int rate = juce::jmax (1, (int) std::lround (dawRate.load()));
    const int latency = reportedLatency.load();
    const bool measured = getMeasuredRoundTrip() >= 0;

    juce::String text;
    text << "PDC: " << latency << " samples (" << juce::String (latency * 1000.0 / rate, 1) << " ms) = buffer "
         << port.getSafetyBuffer() << " + hardware " << (latency - port.getSafetyBuffer()) << ", "
         << (measured ? "measured" : "estimated - press Measure Latency");

    if (measureMessage.isNotEmpty())
        text << "\n" << measureMessage;

    return text;
}

//==============================================================================
void HardInsertProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    juce::ValueTree latency ("MeasuredRoundTrip");
    {
        const juce::ScopedLock sl (latencyLock);

        for (const auto& [rate, samples] : measuredRoundTrip)
            latency.setProperty (juce::Identifier ("rate" + juce::String (rate)), samples, nullptr);
    }

    state.removeChild (state.getChildWithName ("MeasuredRoundTrip"), nullptr);
    state.addChild (latency, -1, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void HardInsertProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType().toString()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    const auto latency = state.getChildWithName ("MeasuredRoundTrip");

    {
        const juce::ScopedLock sl (latencyLock);
        measuredRoundTrip.clear();

        for (int i = 0; i < latency.getNumProperties(); ++i)
        {
            const auto name = latency.getPropertyName (i);

            if (name.toString().startsWith ("rate"))
                measuredRoundTrip[name.toString().substring (4).getIntValue()] = (int) latency.getProperty (name);
        }
    }

    state.removeChild (latency, nullptr);
    apvts.replaceState (state);

    applySettingsToPort();
    updateReportedLatency();
}

//==============================================================================
juce::AudioProcessorEditor* HardInsertProcessor::createEditor()
{
    return new HardInsertEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HardInsertProcessor();
}
