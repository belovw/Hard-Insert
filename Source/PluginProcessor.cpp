#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr const char* kInputTrimId  = "inputTrim";
    constexpr const char* kOutputTrimId = "outputTrim";
    constexpr const char* kMonoModeId   = "monoMode";
}

HardInsertProcessor::HardInsertProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Track",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Track",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Send",   juce::AudioChannelSet::stereo(), false)
                          .withInput  ("Return", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout HardInsertProcessor::createParameterLayout()
{
    using Range = juce::NormalisableRange<float>;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { kInputTrimId, 1 }, "Input Trim", Range (-24.0f, 24.0f, 0.1f), 0.0f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { kOutputTrimId, 1 }, "Output Trim", Range (-24.0f, 24.0f, 0.1f), 0.0f));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { kMonoModeId, 1 }, "Force Mono", false));
    return layout;
}

void HardInsertProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentRate.store ((int) sampleRate);
    maxMeasureSamples = (int) sampleRate; // 1 s timeout for the measurement
    scratch.setSize (2, samplesPerBlock, false, false, true);
    measuring.store (false);
    measureCount = 0;

    int latency = 0;
    {
        const juce::ScopedLock sl (latencyLock);
        if (auto it = latencyByRate.find ((int) sampleRate); it != latencyByRate.end())
            latency = it->second;
    }
    setLatencySamples (latency);
}

bool HardInsertProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto stereo = juce::AudioChannelSet::stereo();

    if (layouts.getMainInputChannelSet() != stereo || layouts.getMainOutputChannelSet() != stereo)
        return false;

    for (auto set : layouts.inputBuses)
        if (set != stereo && ! set.isDisabled())
            return false;

    for (auto set : layouts.outputBuses)
        if (set != stereo && ! set.isDisabled())
            return false;

    return true;
}

void HardInsertProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();

    auto trackIn  = getBusBuffer (buffer, true,  0);
    auto trackOut = getBusBuffer (buffer, false, 0);
    auto sendOut  = getBusBuffer (buffer, false, 1);
    auto returnIn = getBusBuffer (buffer, true,  1);

    const bool hasSend   = sendOut.getNumChannels() >= 2;
    const bool hasReturn = returnIn.getNumChannels() >= 2;

    if (scratch.getNumSamples() < numSamples)
        scratch.setSize (2, numSamples, false, false, true);

    // The return bus and the send bus use the same channels in the host buffer,
    // so copy the return signal out before anything is written to the send bus.
    if (hasReturn)
    {
        scratch.copyFrom (0, 0, returnIn, 0, 0, numSamples);
        scratch.copyFrom (1, 0, returnIn, 1, 0, numSamples);
    }

    const float inGain  = juce::Decibels::decibelsToGain (apvts.getRawParameterValue (kInputTrimId)->load());
    const float outGain = juce::Decibels::decibelsToGain (apvts.getRawParameterValue (kOutputTrimId)->load());
    const bool  mono    = apvts.getRawParameterValue (kMonoModeId)->load() > 0.5f;

    if (measureRequested.exchange (false))
    {
        measureCount = 0;
        measuring.store (true);
    }

    if (measuring.load())
    {
        // Send one impulse, mute the track path, and wait for it to come back.
        if (hasSend)
            sendOut.clear();

        if (measureCount == 0 && hasSend)
            sendOut.setSample (0, 0, 1.0f);

        trackOut.clear();

        if (hasReturn)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                if (std::abs (scratch.getSample (0, i)) > kMeasureThreshold)
                {
                    finishMeasurement (measureCount + i);
                    return;
                }
            }
        }

        measureCount += numSamples;

        if (measureCount >= maxMeasureSamples)
            finishMeasurement (-1);

        return;
    }

    // Normal operation. The track input is read before the track output is
    // written, because both use the same channels in the host buffer.
    if (hasSend)
    {
        if (mono)
        {
            sendOut.copyFrom (0, 0, trackIn, 0, 0, numSamples);
            sendOut.addFrom  (0, 0, trackIn, 1, 0, numSamples);
            sendOut.applyGain (0, 0, numSamples, 0.5f * inGain);
            sendOut.clear (1, 0, numSamples);
        }
        else
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                sendOut.copyFrom (ch, 0, trackIn, ch, 0, numSamples);
                sendOut.applyGain (ch, 0, numSamples, inGain);
            }
        }
    }

    // Without a return bus connected, the track passes through unchanged.
    if (hasReturn)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            const int srcCh = mono ? 0 : ch;
            trackOut.copyFrom (ch, 0, scratch, srcCh, 0, numSamples);
            trackOut.applyGain (ch, 0, numSamples, outGain);
        }
    }
}

void HardInsertProcessor::finishMeasurement (int result) noexcept
{
    measureResult.store (result);
    measureReady.store (true);
    measuring.store (false);
}

void HardInsertProcessor::requestLatencyMeasurement()
{
    measureRequested.store (true);
}

void HardInsertProcessor::pollMeasurement()
{
    if (! measureReady.exchange (false))
        return;

    const int result = measureResult.load();
    const juce::ScopedLock sl (latencyLock);

    if (result < 0)
    {
        statusText = "No return signal. Check the loop cable and levels.";
        return;
    }

    latencyByRate[currentRate.load()] = result;
    setLatencySamples (result);
    statusText = "Latency: " + juce::String (result) + " samples @ "
                 + juce::String (currentRate.load()) + " Hz";
}

juce::String HardInsertProcessor::getStatusText() const
{
    if (isMeasuring())
        return "Measuring... keep the loop connected.";

    const juce::ScopedLock sl (latencyLock);

    if (statusText.isNotEmpty())
        return statusText;

    if (auto it = latencyByRate.find (currentRate.load()); it != latencyByRate.end())
        return "Latency: " + juce::String (it->second) + " samples (stored)";

    return "Latency: not measured";
}

void HardInsertProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    juce::ValueTree latency ("Latency");
    {
        const juce::ScopedLock sl (latencyLock);
        for (const auto& [rate, samples] : latencyByRate)
            latency.setProperty (juce::Identifier ("rate" + juce::String (rate)), samples, nullptr);
    }
    state.addChild (latency, -1, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void HardInsertProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType().toString()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    const auto latency = state.getChildWithName ("Latency");

    {
        const juce::ScopedLock sl (latencyLock);
        latencyByRate.clear();

        for (int i = 0; i < latency.getNumProperties(); ++i)
        {
            const auto name = latency.getPropertyName (i).toString();

            if (name.startsWith ("rate"))
                latencyByRate[name.substring (4).getIntValue()] =
                    (int) latency.getProperty (latency.getPropertyName (i));
        }
    }

    apvts.replaceState (state);
}

juce::AudioProcessorEditor* HardInsertProcessor::createEditor()
{
    return new HardInsertEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HardInsertProcessor();
}
