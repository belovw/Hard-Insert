#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_data_structures/juce_data_structures.h>

#include <array>
#include <atomic>

namespace hi
{

//==============================================================================
/** The audio interface, opened by the plugin itself (ASIO, WASAPI, DirectSound).

    One device per process, shared by every Hard-Insert instance: each instance
    registers its own HardwarePort as a callback, and the AudioDeviceManager sums
    the outputs of all callbacks. The device choice is stored in
    %APPDATA%/Hard-Insert/Hard-Insert.settings.
*/
class SharedDevice final : private juce::ChangeListener
{
public:
    static constexpr int kMaxChannels = 64;

    SharedDevice();
    ~SharedDevice() override;

    /** Opens the saved (or the default) device the first time it is called.
        Message thread. Not done in the constructor, so that plugin scans in the
        host do not open the audio interface. */
    void ensureOpen();

    juce::AudioDeviceManager& getManager() noexcept      { return manager; }
    const juce::String& getInitError() const noexcept    { return initError; }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    juce::AudioDeviceManager manager;
    std::unique_ptr<juce::PropertiesFile> settings;
    juce::String initError;
    bool opened = false;

    JUCE_DECLARE_NON_COPYABLE (SharedDevice)
};

//==============================================================================
/** Lock-free single-producer / single-consumer stereo FIFO. */
class StereoFifo
{
public:
    explicit StereoFifo (int capacity);

    int getNumReady() const noexcept    { return fifo.getNumReady(); }
    int getFreeSpace() const noexcept   { return fifo.getFreeSpace(); }

    /** Not thread-safe: only call while the other side is not using the FIFO. */
    void reset() noexcept               { fifo.reset(); }

    /** Writes n frames. A null channel pointer writes silence. Caller checks free space. */
    void write (const float* left, const float* right, int n) noexcept;

    /** Reads up to n frames and returns how many were read. A null pointer discards that channel. */
    int read (float* left, float* right, int n) noexcept;

    int discard (int n) noexcept        { return read (nullptr, nullptr, n); }

private:
    juce::AbstractFifo fifo;
    juce::AudioBuffer<float> storage;
};

//==============================================================================
/** Connects one plugin instance (the DAW audio thread) to the audio interface
    (the device audio thread).

    DAW thread:    track audio -> send FIFO -> [device] -> hardware output channels
    Device thread: hardware input channels -> return FIFO -> [DAW] -> track output

    Timing model. Each DAW block writes N frames to the send FIFO and reads N
    frames from the return FIFO; each device callback reads M frames from the send
    FIFO and writes M frames to the return FIFO. So the total number of frames held
    in both FIFOs never changes. At start the FIFOs are pre-filled with P frames of
    silence in total (the "safety buffer", half on each side, so both threads have
    a cushion against the other one's jitter), so every sample comes back exactly
        P + D
    samples later, where D is the device round trip (DAC + cable + gear + ADC).
    This value is constant for as long as the loop runs, which is what makes plugin
    delay compensation possible.

    Dropouts. When a reader finds its FIFO short by U frames, it outputs U frames
    of silence in their place and drops the next U frames when they arrive late,
    so every frame stays in its own time slot and the loop stays at P + D.
    If the late frames never arrive (one side lost time for good, e.g. an FL Studio
    dropout longer than the cushion) or a FIFO overflows, the loop is reset with
    the same P, so the latency is still P + D after the reset.
*/
class HardwarePort final : public juce::AudioIODeviceCallback
{
public:
    static constexpr int kFifoCapacity = 1 << 17;
    static constexpr int kMaxSafetyBuffer = 16384;

    HardwarePort();

    //==============================================================================
    // Message thread

    /** Physical channel indices (0-based), -1 = not connected. */
    void setChannels (int sendLeft, int sendRight, int returnLeft, int returnRight) noexcept;

    /** P, in samples. Resets the loop. */
    void setSafetyBuffer (int samples) noexcept;

    /** A safe P for the given block sizes: each side's cushion covers one DAW block,
        one device block and some scheduling jitter. */
    static int autoSafetyBuffer (int dawBlockSize, int deviceBlockSize) noexcept
    {
        const int blocks = juce::jmax (64, dawBlockSize) + juce::jmax (64, deviceBlockSize);
        const int cushion = blocks + juce::jmax (256, blocks / 2);
        return juce::jlimit (512, kMaxSafetyBuffer, ((2 * cushion + 63) / 64) * 64);
    }

    //==============================================================================
    // DAW audio thread

    /** Asks the DAW side to reset the loop at the next block. Any thread. */
    void requestResync() noexcept                    { dawResyncRequest.store (true); }

    /** Sends numSamples frames to the hardware and receives numSamples frames back.
        A null send pointer sends silence on that channel. Returns false, with silent
        return buffers, while the loop is not running.
    */
    bool exchange (const float* sendLeft, const float* sendRight,
                   float* returnLeft, float* returnRight, int numSamples) noexcept;

    //==============================================================================
    // Status, any thread

    bool isDeviceRunning() const noexcept            { return deviceRunning.load(); }
    bool isLoopRunning() const noexcept              { return state.load() == kRunning; }
    double getDeviceSampleRate() const noexcept      { return deviceRate.load(); }
    int getDeviceBufferSize() const noexcept         { return deviceBufferSize.load(); }
    int getDeviceRoundTripEstimate() const noexcept  { return deviceRoundTrip.load(); }
    int getSafetyBuffer() const noexcept             { return safetyBuffer.load(); }
    int getResyncCount() const noexcept              { return resyncCount.load(); }
    int getUnderrunCount() const noexcept            { return underrunCount.load(); }

    //==============================================================================
    // juce::AudioIODeviceCallback (device thread)

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    // Who may touch the FIFOs:
    //   kIdle         - nobody (device not running)
    //   kResetPending - only the DAW thread, to reset them; then it sets kRunning
    //   kRunning      - DAW thread (its ends) and device thread (its ends)
    //   kStopping     - DAW asked for a reset; the device acknowledges by setting
    //                   kResetPending at the start of its next callback
    enum State { kIdle = 0, kResetPending, kRunning, kStopping };

    void resetLoop() noexcept;                 // DAW thread
    void requestStopFromDaw() noexcept;        // DAW thread
    void requestResetFromDevice() noexcept;    // device thread

    StereoFifo sendFifo   { kFifoCapacity };
    StereoFifo returnFifo { kFifoCapacity };

    std::atomic<int> state { kIdle };
    std::atomic<bool> dawResyncRequest { false };
    std::atomic<bool> deviceRunning { false };
    std::atomic<double> deviceRate { 0.0 };
    std::atomic<int> deviceBufferSize { 0 };
    std::atomic<int> deviceRoundTrip { 0 };
    std::atomic<int> safetyBuffer { 2048 };
    std::atomic<int> sendLeftChannel { -1 }, sendRightChannel { -1 };
    std::atomic<int> returnLeftChannel { -1 }, returnRightChannel { -1 };
    std::atomic<int> resyncCount { 0 }, underrunCount { 0 };
    std::atomic<int> resyncThreshold { 48000 };
    std::atomic<int> stuckLimit { 4800 };        // ~100 ms: a debt older than this will never be paid
    std::atomic<int> streakWindow { 4800 };      // ~100 ms: underruns this close together = cushion used up
    std::atomic<int> generation { 0 };           // incremented by every loop reset
    std::atomic<int> sendCushion { 1024 };       // send pre-fill of the current loop

    /** Tracks one reader's underruns (each side owns one). */
    struct UnderrunTracker
    {
        int debt = 0, debtAge = 0, streak = 0, sinceLast = 0;
        void reset() noexcept { *this = {}; }
    };

    // Returns true when the loop has to be reset.
    bool trackUnderrun (UnderrunTracker&, int missing, int blockSize) noexcept;

    // Device thread only
    UnderrunTracker deviceTracker;
    int deviceGeneration = -1;
    std::array<int, SharedDevice::kMaxChannels> outputIndex {}, inputIndex {};

    // DAW thread only
    UnderrunTracker dawTracker;

    JUCE_DECLARE_NON_COPYABLE (HardwarePort)
};

} // namespace hi
