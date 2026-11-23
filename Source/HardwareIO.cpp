#include "HardwareIO.h"

namespace hi
{

//==============================================================================
SharedDevice::SharedDevice()
{
    juce::PropertiesFile::Options options;
    options.applicationName     = "Hard-Insert";
    options.filenameSuffix      = ".settings";
    options.folderName          = "Hard-Insert";
    options.osxLibrarySubFolder = "Application Support";
    settings = std::make_unique<juce::PropertiesFile> (options);
}

SharedDevice::~SharedDevice()
{
    if (opened)
    {
        manager.removeChangeListener (this);
        manager.closeAudioDevice();
    }
}

void SharedDevice::ensureOpen()
{
    if (opened)
        return;

    opened = true;

    // Request every channel, so any output/input can be used for send and return.
    const auto saved = settings->getXmlValue ("audioDevice");
    initError = manager.initialise (kMaxChannels, kMaxChannels, saved.get(), false);

    manager.addChangeListener (this);
}

void SharedDevice::changeListenerCallback (juce::ChangeBroadcaster*)
{
    initError.clear();

    // Only settings chosen by the user are returned here, so a temporary
    // default device never overwrites the saved choice.
    if (auto xml = manager.createStateXml())
    {
        settings->setValue ("audioDevice", xml.get());
        settings->saveIfNeeded();
    }
}

//==============================================================================
StereoFifo::StereoFifo (int capacity)
    : fifo (capacity)
{
    storage.setSize (2, capacity);
    storage.clear();
}

void StereoFifo::write (const float* left, const float* right, int n) noexcept
{
    const auto scope = fifo.write (n);

    const auto copyBlock = [this] (const float* src, int channel, int fifoStart, int srcOffset, int num)
    {
        if (num <= 0)
            return;

        auto* dest = storage.getWritePointer (channel, fifoStart);

        if (src != nullptr)
            juce::FloatVectorOperations::copy (dest, src + srcOffset, num);
        else
            juce::FloatVectorOperations::clear (dest, num);
    };

    copyBlock (left,  0, scope.startIndex1, 0, scope.blockSize1);
    copyBlock (right, 1, scope.startIndex1, 0, scope.blockSize1);
    copyBlock (left,  0, scope.startIndex2, scope.blockSize1, scope.blockSize2);
    copyBlock (right, 1, scope.startIndex2, scope.blockSize1, scope.blockSize2);
}

int StereoFifo::read (float* left, float* right, int n) noexcept
{
    const int num = juce::jmin (n, fifo.getNumReady());

    if (num <= 0)
        return 0;

    const auto scope = fifo.read (num);

    const auto copyBlock = [this] (float* dest, int channel, int fifoStart, int destOffset, int count)
    {
        if (dest != nullptr && count > 0)
            juce::FloatVectorOperations::copy (dest + destOffset, storage.getReadPointer (channel, fifoStart), count);
    };

    copyBlock (left,  0, scope.startIndex1, 0, scope.blockSize1);
    copyBlock (right, 1, scope.startIndex1, 0, scope.blockSize1);
    copyBlock (left,  0, scope.startIndex2, scope.blockSize1, scope.blockSize2);
    copyBlock (right, 1, scope.startIndex2, scope.blockSize1, scope.blockSize2);

    return num;
}

//==============================================================================
HardwarePort::HardwarePort()
{
    outputIndex.fill (-1);
    inputIndex.fill (-1);
}

void HardwarePort::setChannels (int sendLeft, int sendRight, int returnLeft, int returnRight) noexcept
{
    sendLeftChannel.store (sendLeft);
    sendRightChannel.store (sendRight);
    returnLeftChannel.store (returnLeft);
    returnRightChannel.store (returnRight);
}

void HardwarePort::setSafetyBuffer (int samples) noexcept
{
    safetyBuffer.store (juce::jlimit (64, kMaxSafetyBuffer, samples));
    requestResync();
}

//==============================================================================
void HardwarePort::resetLoop() noexcept
{
    // Total pre-fill P, split between both FIFOs: the device gets a cushion against
    // DAW jitter (irregular FL Studio blocks), the DAW one against device jitter.
    const int p = safetyBuffer.load();
    sendFifo.reset();
    returnFifo.reset();
    sendFifo.write (nullptr, nullptr, p / 2);
    returnFifo.write (nullptr, nullptr, p - p / 2);
    sendCushion.store (p / 2);
    dawTracker.reset();
    dawResyncRequest.store (false);
    generation.fetch_add (1);
}

void HardwarePort::requestStopFromDaw() noexcept
{
    int expected = kRunning;

    if (state.compare_exchange_strong (expected, kStopping))
        resyncCount.fetch_add (1);
}

void HardwarePort::requestResetFromDevice() noexcept
{
    int expected = kRunning;

    if (state.compare_exchange_strong (expected, kResetPending))
        resyncCount.fetch_add (1);
}

bool HardwarePort::trackUnderrun (UnderrunTracker& t, int missing, int blockSize) noexcept
{
    t.sinceLast += blockSize;

    if (missing > 0)
    {
        // Count episodes, not blocks: one long stall is a single underrun.
        if (t.debt == 0)
        {
            underrunCount.fetch_add (1);
            t.streak = (t.sinceLast < streakWindow.load()) ? t.streak + 1 : 1;
        }

        t.debt += missing;
        t.sinceLast = 0;
    }

    t.debtAge = t.debt > 0 ? t.debtAge + blockSize : 0;

    return t.debt > resyncThreshold.load()      // the other side stopped
        || t.debtAge > stuckLimit.load()        // the late frames never arrive (time lost for good)
        || t.streak >= 5;                       // underruns ~100 ms apart or closer: the cushion is used up
}

//==============================================================================
bool HardwarePort::exchange (const float* sendLeft, const float* sendRight,
                             float* returnLeft, float* returnRight, int n) noexcept
{
    const auto silence = [&]
    {
        juce::FloatVectorOperations::clear (returnLeft, n);
        juce::FloatVectorOperations::clear (returnRight, n);
        return false;
    };

    int s = state.load();

    if (s == kResetPending)
    {
        // The device thread does not touch the FIFOs in this state.
        resetLoop();
        state.store (kRunning);
        s = kRunning;
    }
    else if (s == kRunning && dawResyncRequest.exchange (false))
    {
        requestStopFromDaw();
        return silence();
    }

    if (s != kRunning)
        return silence();

    if (sendFifo.getFreeSpace() < n)
    {
        // The device stopped reading.
        requestStopFromDaw();
        return silence();
    }

    sendFifo.write (sendLeft, sendRight, n);

    // Frames that arrived too late for their slot were replaced by silence: drop them.
    if (dawTracker.debt > 0)
        dawTracker.debt -= returnFifo.discard (dawTracker.debt);

    const int got = returnFifo.read (returnLeft, returnRight, n);

    if (got < n)
    {
        juce::FloatVectorOperations::clear (returnLeft + got, n - got);
        juce::FloatVectorOperations::clear (returnRight + got, n - got);
    }

    if (trackUnderrun (dawTracker, n - got, n))
        requestStopFromDaw();

    return true;
}

//==============================================================================
void HardwarePort::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    outputIndex.fill (-1);
    inputIndex.fill (-1);

    const auto activeOutputs = device->getActiveOutputChannels();
    const auto activeInputs  = device->getActiveInputChannels();

    // The callback receives only the active channels, packed in order.
    for (int ch = 0, index = 0; ch < SharedDevice::kMaxChannels; ++ch)
        if (activeOutputs[ch])
            outputIndex[(size_t) ch] = index++;

    for (int ch = 0, index = 0; ch < SharedDevice::kMaxChannels; ++ch)
        if (activeInputs[ch])
            inputIndex[(size_t) ch] = index++;

    const double rate = device->getCurrentSampleRate();
    deviceRate.store (rate);
    deviceBufferSize.store (device->getCurrentBufferSizeSamples());
    deviceRoundTrip.store (device->getInputLatencyInSamples() + device->getOutputLatencyInSamples());
    resyncThreshold.store (juce::jmax (8192, (int) (rate * 0.25)));
    stuckLimit.store (juce::jmax (2048, (int) (rate * 0.1)));
    streakWindow.store (juce::jmax (4096, (int) (rate * 0.1)));

    deviceGeneration = -1;
    deviceTracker.reset();
    deviceRunning.store (true);
    state.store (kResetPending);
}

void HardwarePort::audioDeviceStopped()
{
    deviceRunning.store (false);
    state.store (kIdle);
}

void HardwarePort::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                     int numInputChannels,
                                                     float* const* outputChannelData,
                                                     int numOutputChannels,
                                                     int n,
                                                     const juce::AudioIODeviceCallbackContext&)
{
    // The device manager sums all instances, so every unused output must be silent.
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], n);

    int s = state.load();

    if (s == kStopping)
    {
        // Acknowledge: from now on only the DAW thread touches the FIFOs.
        state.compare_exchange_strong (s, kResetPending);
        return;
    }

    if (s != kRunning)
        return;

    if (const int g = generation.load(); g != deviceGeneration)
    {
        // First callback of a new loop. The DAW may have run ahead since the reset,
        // which moved cushion from the return side to the send side. Move it back:
        // drop e frames of send pre-fill and add e frames of silence to the return.
        // Both shift the loop by e in opposite directions, so it stays exactly P + D.
        deviceGeneration = g;
        deviceTracker.reset();

        const int excess = juce::jlimit (0, returnFifo.getFreeSpace(),
                                         sendFifo.getNumReady() - sendCushion.load());
        sendFifo.discard (excess);
        returnFifo.write (nullptr, nullptr, excess);
    }

    const auto outputFor = [&] (int physical) -> float*
    {
        if (! juce::isPositiveAndBelow (physical, SharedDevice::kMaxChannels))
            return nullptr;

        const int index = outputIndex[(size_t) physical];
        return juce::isPositiveAndBelow (index, numOutputChannels) ? outputChannelData[index] : nullptr;
    };

    const auto inputFor = [&] (int physical) -> const float*
    {
        if (! juce::isPositiveAndBelow (physical, SharedDevice::kMaxChannels))
            return nullptr;

        const int index = inputIndex[(size_t) physical];
        return juce::isPositiveAndBelow (index, numInputChannels) ? inputChannelData[index] : nullptr;
    };

    float* outLeft  = outputFor (sendLeftChannel.load());
    float* outRight = outputFor (sendRightChannel.load());

    if (outRight == outLeft)
        outRight = nullptr;

    const float* inLeft  = inputFor (returnLeftChannel.load());
    const float* inRight = inputFor (returnRightChannel.load());

    // 1. Send FIFO -> hardware outputs. Frames that arrived too late for their slot
    //    were replaced by silence: drop them.
    if (deviceTracker.debt > 0)
        deviceTracker.debt -= sendFifo.discard (deviceTracker.debt);

    const int got = sendFifo.read (outLeft, outRight, n);

    // 2. Hardware inputs -> return FIFO.
    if (returnFifo.getFreeSpace() < n)
    {
        // The DAW stopped reading.
        requestResetFromDevice();
        return;
    }

    returnFifo.write (inLeft, inRight, n);

    if (trackUnderrun (deviceTracker, n - got, n))
        requestResetFromDevice();
}

} // namespace hi
