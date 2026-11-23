#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <map>

// Hard-Insert: sends the track audio to a physical hardware unit through
// the host's aux routing and returns the processed signal to the track.
//
// Buses:
//   Input  0 "Track"  (stereo) - track audio coming from the DAW
//   Input  1 "Return" (stereo) - signal coming back from the hardware
//   Output 0 "Track"  (stereo) - signal sent back to the track
//   Output 1 "Send"   (stereo) - signal sent to the hardware
//
// In "Force Mono" mode the track is summed to (L+R)/2 on Send ch 1 only,
// and Return ch 1 is duplicated to both Track outputs.
class HardInsertProcessor final : public juce::AudioProcessor
{
public:
    HardInsertProcessor();
    ~HardInsertProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Hard-Insert"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Message thread only.
    void requestLatencyMeasurement();
    void pollMeasurement();
    bool isMeasuring() const { return measuring.load() || measureRequested.load(); }
    juce::String getStatusText() const;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void finishMeasurement (int result) noexcept;

    // Impulse amplitude is 1.0; the hardware return may be much quieter.
    static constexpr float kMeasureThreshold = 0.02f;

    std::atomic<bool> measureRequested { false };
    std::atomic<bool> measuring { false };
    std::atomic<bool> measureReady { false };
    std::atomic<int> measureResult { -1 };
    std::atomic<int> currentRate { 44100 };

    // Audio thread only.
    int measureCount = 0;
    int maxMeasureSamples = 44100;
    juce::AudioBuffer<float> scratch;

    // Shared between threads, guarded by latencyLock.
    mutable juce::CriticalSection latencyLock;
    std::map<int, int> latencyByRate;
    juce::String statusText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HardInsertProcessor)
};
