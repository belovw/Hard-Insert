#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "HardwareIO.h"

#include <map>
#include <vector>

//==============================================================================
/** Hard-Insert: a hardware insert for outboard gear.

    The plugin opens the audio interface itself (ASIO / WASAPI), sends the track
    to the chosen physical outputs, takes the hardware return from the chosen
    physical inputs and puts it back on the track. The loop latency is measured
    with a ping and reported to the host for delay compensation (PDC).
*/
class HardInsertProcessor final : public juce::AudioProcessor,
                                  private juce::Timer
{
public:
    enum class Channel { sendLeft, sendRight, returnLeft, returnRight };

    HardInsertProcessor();
    ~HardInsertProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                      { return true; }

    const juce::String getName() const override          { return "Hard-Insert"; }
    bool acceptsMidi() const override                    { return false; }
    bool producesMidi() const override                   { return false; }
    double getTailLengthSeconds() const override         { return 0.0; }

    int getNumPrograms() override                        { return 1; }
    int getCurrentProgram() override                     { return 0; }
    void setCurrentProgram (int) override                {}
    const juce::String getProgramName (int) override     { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==============================================================================
    // Message thread (editor)

    juce::AudioDeviceManager& getDeviceManager() noexcept   { return sharedDevice->getManager(); }

    /** Opens the shared device (if needed) and starts receiving its callbacks. */
    void connectToDevice();

    int getChannel (Channel) const;
    void setChannel (Channel, int physicalChannel);

    /** Safety buffer setting: 0 = Auto, otherwise samples. */
    int getSafetyBuffer() const;
    void setSafetyBuffer (int samples);

    /** The safety buffer P actually in use (Auto resolved), in samples. */
    int getEffectiveSafetyBuffer() const noexcept           { return port.getSafetyBuffer(); }

    void startLatencyMeasurement();
    bool isMeasuring() const noexcept                       { return measureStage.load() != kMeasureIdle; }

    juce::String getStatusText() const;
    juce::String getLatencyText() const;

    float takeSendPeak() noexcept                           { return sendPeak.exchange (0.0f); }
    float takeReturnPeak() noexcept                         { return returnPeak.exchange (0.0f); }

    juce::AudioProcessorValueTreeState apvts;

private:
    //==============================================================================
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void timerCallback() override;
    void processChunk (float* left, float* right, int numSamples) noexcept;
    void applySettingsToPort();
    void refreshSafetyBuffer() noexcept;
    void analyseMeasurement();
    void updateReportedLatency();
    int getMeasuredRoundTrip() const;       // -1 when not measured for the current rate

    static void updatePeak (std::atomic<float>& peak, const float* data, int numSamples) noexcept;

    //==============================================================================
    juce::SharedResourcePointer<hi::SharedDevice> sharedDevice;
    hi::HardwarePort port;

    std::atomic<float>* inputTrimParam  = nullptr;
    std::atomic<float>* outputTrimParam = nullptr;
    std::atomic<float>* monoParam       = nullptr;

    juce::AudioBuffer<float> sendBuffer, returnBuffer;
    juce::SmoothedValue<float> inputGain, outputGain;
    std::atomic<double> dawRate { 0.0 };
    std::atomic<int> dawBlockSize { 512 };
    std::atomic<int> safetySetting { 0 };   // copy of the setting for the audio thread (0 = Auto)
    std::atomic<bool> deviceWanted { false };
    bool connected = false;                  // message thread
    std::atomic<bool> rateMismatch { false };
    std::atomic<int> reportedLatency { -1 };

    // Latency measurement (ping)
    enum MeasureStage { kMeasureIdle = 0, kMeasureRequested, kMeasureCapturing, kMeasureCaptured };
    std::atomic<int> measureStage { kMeasureIdle };
    std::atomic<bool> measureFailed { false };
    std::vector<float> measureCapture;
    int measurePos = 0, measureLength = 0, measureSafety = 0, measureResyncs = 0;   // written by the DAW thread before kMeasureCaptured
    juce::String measureMessage;                                                     // message thread

    // Measured device round trip D (total PDC = safety buffer + D), per sample rate, saved with the project.
    mutable juce::CriticalSection latencyLock;
    std::map<int, int> measuredRoundTrip;

    std::atomic<float> sendPeak { 0.0f }, returnPeak { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HardInsertProcessor)
};
