// Simulation test for hi::HardwarePort.
// DAW thread and device thread run on the same clock with different block sizes and
// scheduling jitter; a fake "hardware" returns device output index j at input index j + D.
// Every impulse sent by the DAW must come back exactly P + D DAW samples later,
// including after dropouts on either side.
#include "../Source/HardwareIO.h"
#include <cstdio>
#include <deque>
#include <random>
#include <vector>
#include <cstdlib>

using namespace hi;
static bool trace = false;

struct FakeDevice final : juce::AudioIODevice
{
    FakeDevice (double r, int b) : AudioIODevice ("Fake", "Test"), rate (r), block (b) {}
    juce::StringArray getOutputChannelNames() override       { return { "1", "2", "3", "4" }; }
    juce::StringArray getInputChannelNames() override        { return { "1", "2", "3", "4" }; }
    juce::Array<double> getAvailableSampleRates() override   { return { rate }; }
    juce::Array<int> getAvailableBufferSizes() override      { return { block }; }
    int getDefaultBufferSize() override                      { return block; }
    juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override                                   { return true; }
    void start (juce::AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override                                { return true; }
    juce::String getLastError() override                     { return {}; }
    int getCurrentBufferSizeSamples() override               { return block; }
    double getCurrentSampleRate() override                   { return rate; }
    int getCurrentBitDepth() override                        { return 24; }
    juce::BigInteger getActiveOutputChannels() const override { juce::BigInteger b; b.setRange (0, 4, true); return b; }
    juce::BigInteger getActiveInputChannels() const override  { juce::BigInteger b; b.setRange (0, 4, true); return b; }
    int getOutputLatencyInSamples() override                 { return 0; }
    int getInputLatencyInSamples() override                  { return 0; }
    double rate; int block;
};

enum class Stall { none, dawCatchUp, dawLost, deviceCatchUp, deviceLost };

struct Scenario
{
    const char* name;
    int P, D, dawBlock; bool variableDaw; int devBlock, jitter;
    Stall stall; long stallAt, stallLen;
    bool allowLoss = false;      // pings may be lost during loop resets (P too small)
    int repeats = 1;             // stall repeats every 250 ms
};

struct Result { long expected; std::vector<long> found; int resyncs, underruns; bool rightOk; };

static Result run (const Scenario& sc, unsigned seed)
{
    const double rate = 48000;
    HardwarePort port;
    port.setChannels (2, 3, 2, 3);            // send: outs 3/4, return: ins 3/4
    const int P = sc.P > 0 ? sc.P : HardwarePort::autoSafetyBuffer (sc.dawBlock, sc.devBlock);
    port.setSafetyBuffer (P);

    FakeDevice dev (rate, sc.devBlock);
    port.audioDeviceAboutToStart (&dev);

    std::mt19937 rng (seed);
    std::uniform_int_distribution<int> jit (0, sc.jitter);
    std::uniform_int_distribution<int> dawSize (1, sc.dawBlock);

    std::deque<float> lineL ((size_t) sc.D, 0.0f), lineR ((size_t) sc.D, 0.0f);

    const long total = 48000L * 16;
    std::vector<long> impulses;
    for (long t = 48000L * 2; t < total - 48000; t += 48000L * 2)
        impulses.push_back (t);

    std::vector<float> sL (65536), sR (65536), rL (65536), rR (65536);
    std::vector<std::vector<float>> out (4, std::vector<float> ((size_t) sc.devBlock)),
                                    in  (4, std::vector<float> ((size_t) sc.devBlock));

    Result res { P + sc.D, {}, 0, 0, true };

    long dawPos = 0, devPos = 0, dawLag = 0, devLag = 0;
    int dawN = sc.variableDaw ? dawSize (rng) : sc.dawBlock;
    long dawTime = dawN + jit (rng);
    long devTime = sc.devBlock + jit (rng);

    const auto inStall = [&] (long t)
    {
        for (int r = 0; r < sc.repeats; ++r)
        {
            const long a = sc.stallAt + r * 12000L;
            if (t >= a && t < a + sc.stallLen) return true;
        }
        return false;
    };
    const auto stallEndFor = [&] (long t)
    {
        for (int r = 0; r < sc.repeats; ++r)
        {
            const long a = sc.stallAt + r * 12000L;
            if (t >= a && t < a + sc.stallLen) return a + sc.stallLen;
        }
        return t;
    };

    while (dawPos < total)
    {
        // Apply stalls to whichever event is next.
        if (sc.stall == Stall::dawCatchUp && inStall (dawTime))       dawTime = stallEndFor (dawTime);
        if (sc.stall == Stall::dawLost && inStall (dawTime))          { dawLag += sc.stallLen; dawTime += sc.stallLen; }
        if (sc.stall == Stall::deviceCatchUp && inStall (devTime))    devTime = stallEndFor (devTime);
        if (sc.stall == Stall::deviceLost && inStall (devTime))       { devLag += sc.stallLen; devTime += sc.stallLen; }

        if (dawTime < devTime)
        {
            const int n = dawN;

            for (int i = 0; i < n; ++i)
            {
                float v = 0.0f;
                for (long a : impulses) if (dawPos + i == a) v = 1.0f;
                sL[(size_t) i] = v;
                sR[(size_t) i] = -v;
            }

            { const int u0 = port.getUnderrunCount(), r0 = port.getResyncCount();
            port.exchange (sL.data(), sR.data(), rL.data(), rR.data(), n);
            if (trace && (port.getUnderrunCount() != u0 || port.getResyncCount() != r0)) std::printf ("  t=%ld DAW K=%ld n=%d underruns %d->%d resyncs %d->%d\n", dawTime, dawPos, n, u0, port.getUnderrunCount(), r0, port.getResyncCount()); }

            for (int i = 0; i < n; ++i)
            {
                if (rL[(size_t) i] > 0.5f)
                {
                    long origin = -1;
                    for (long a : impulses) if (a <= dawPos + i) origin = a;
                    res.found.push_back (dawPos + i - origin);
                    if (trace) std::printf ("  ping from %ld back after %ld\n", origin, dawPos + i - origin);
                    res.rightOk = res.rightOk && rR[(size_t) i] < -0.5f;
                }
            }

            dawPos += n;
            dawN = sc.variableDaw ? dawSize (rng) : sc.dawBlock;
            dawTime = dawPos + dawN + dawLag + jit (rng);
        }
        else
        {
            for (int i = 0; i < sc.devBlock; ++i)
            {
                in[2][(size_t) i] = lineL.front(); lineL.pop_front();
                in[3][(size_t) i] = lineR.front(); lineR.pop_front();
                in[0][(size_t) i] = in[1][(size_t) i] = 0.0f;
            }

            const float* ins[4] = { in[0].data(), in[1].data(), in[2].data(), in[3].data() };
            float* outs[4]      = { out[0].data(), out[1].data(), out[2].data(), out[3].data() };
            { const int u0 = port.getUnderrunCount(), r0 = port.getResyncCount();
            port.audioDeviceIOCallbackWithContext (ins, 4, outs, 4, sc.devBlock, {});
            if (trace && (port.getUnderrunCount() != u0 || port.getResyncCount() != r0)) std::printf ("  t=%ld DEV J=%ld underruns %d->%d resyncs %d->%d\n", devTime, devPos, u0, port.getUnderrunCount(), r0, port.getResyncCount()); }

            for (int i = 0; i < sc.devBlock; ++i)
            {
                lineL.push_back (outs[2][i]);
                lineR.push_back (outs[3][i]);
            }

            devPos += sc.devBlock;
            devTime = devPos + sc.devBlock + devLag + jit (rng);
        }
    }

    res.resyncs = port.getResyncCount();
    res.underruns = port.getUnderrunCount();
    return res;
}

int main()
{
    trace = std::getenv ("TRACE") != nullptr;
    const long s3 = 48000L * 3;   // stalls start at 3 s, between the pings at 2 s and 4 s

    const Scenario scenarios[] =
    {
        { "DAW 512 / device 480, no jitter (Auto P)",       -1, 1300, 512, false, 480,   0, Stall::none, 0, 0 },
        { "DAW 512 / device 480, jitter 300 (Auto P)",      -1, 1300, 512, false, 480, 300, Stall::none, 0, 0 },
        { "FL 1..1024 variable / device 256 (Auto P)",      -1, 1234, 1024, true, 256, 200, Stall::none, 0, 0 },
        { "P=1024, DAW 256 / device 128",                    1024, 777, 256, false, 128, 100, Stall::none, 0, 0 },
        { "DAW 1024 / device 1024, jitter 1500, P=8192",     8192, 2500, 1024, false, 1024, 1500, Stall::none, 0, 0 },
        { "P too small for jitter (resets, found must be exact)", 1024, 2500, 1024, false, 1024, 1500, Stall::none, 0, 0, true },
        { "WASAPI-like device 1440 / FL 1..2048 (Auto P)",  -1, 4000, 2048, true, 1440, 960, Stall::none, 0, 0 },
        { "DAW late 100 ms, then catches up",                -1, 1300, 512, false, 480, 100, Stall::dawCatchUp, s3, 4800 },
        { "DAW loses 100 ms for good (FL dropout)",          -1, 1300, 512, false, 480, 100, Stall::dawLost, s3, 4800 },
        { "DAW loses 30 ms x1 (FL dropout, small)",          -1, 1300, 512, false, 480, 100, Stall::dawLost, s3, 1440 },
        { "Device late 100 ms, then catches up",             -1, 1300, 512, false, 480, 100, Stall::deviceCatchUp, s3, 4800 },
        { "Device loses 100 ms for good",                    -1, 1300, 512, false, 480, 100, Stall::deviceLost, s3, 4800 },
        { "DAW loses 1 s (loop reset)",                      -1, 1300, 512, false, 480, 100, Stall::dawLost, s3, 48000 },
        { "Device loses 1 s (loop reset)",                   -1, 1300, 512, false, 480, 100, Stall::deviceLost, s3, 48000 },
        { "4 FL dropouts of 10 ms within 1 s",               -1, 1300, 512, false, 480, 100, Stall::dawLost, s3, 480, false, 4 },
        { "4 FL dropouts of 25 ms within 1 s",               -1, 1300, 512, false, 480, 100, Stall::dawLost, s3, 1200, false, 4 },
        { "4 device dropouts of 25 ms within 1 s",           -1, 1300, 512, false, 480, 100, Stall::deviceLost, s3, 1200, false, 4 },
        { "FL 1..2048 / ASIO 64, jitter 64 (Auto P)",        -1, 300, 2048, true, 64, 64, Stall::none, 0, 0 },
        { "FL 128 / ASIO 128 (Auto P, low latency)",         -1, 420, 128, false, 128, 32, Stall::none, 0, 0 },
    };

    bool all = true;
    unsigned seed = 1;

    for (const auto& sc : scenarios)
    {
        for (int rep = 0; rep < 5; ++rep)
        {
            if (std::getenv ("ONLY") && std::atoi (std::getenv ("ONLY")) != (int) seed) { ++seed; continue; }
            const auto r = run (sc, seed++);
            bool ok = r.found.size() == 7 && r.rightOk;
            for (long f : r.found) ok = ok && f == r.expected;
            if (! sc.allowLoss) all = all && ok;

            if (rep == 0 || ! ok)
            {
                std::printf ("%-50s seed %2u: expect %ld, got [", sc.name, seed - 1, r.expected);
                for (size_t i = 0; i < r.found.size(); ++i) std::printf ("%s%ld", i ? " " : "", r.found[i]);
                std::printf ("] resyncs=%d underruns=%d %s\n", r.resyncs, r.underruns, ok ? "OK" : (sc.allowLoss ? "(info: P too small, resets expected)" : "FAIL"));
            }
        }
    }

    std::printf ("\n%s\n", all ? "ALL OK (5 random seeds per scenario)" : "SOME FAILED");
    return all ? 0 : 1;
}
