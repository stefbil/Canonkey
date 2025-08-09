#pragma once
#include <JuceHeader.h>   // <-- define JUCE_WINDOWS first

#if JUCE_WINDOWS

// Keep Windows headers tidy and stop min/max macros
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <functional>
#include <atomic>
#include <thread>
#include <vector>

// Very small WASAPI loopback capturer (default render "what‑you‑hear").
// Calls onBlock with planar float** buffers.
class WasapiLoopback
{
public:
    using BlockCB = std::function<void(const float* const* input, int numCh, int numSamples, double sampleRate)>;

    WasapiLoopback();
    ~WasapiLoopback();

    bool start(BlockCB cb, juce::String& error);
    void stop();
    bool isRunning() const { return running.load(); }

private:
    void threadProc();
    bool initClient(juce::String& error);
    void releaseAll();

    std::thread       worker;
    std::atomic<bool> running{ false };
    BlockCB           onBlock;

    // COM interfaces
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* renderDevice = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;

    WAVEFORMATEX* mixFmt = nullptr; // freed via CoTaskMemFree
    UINT32        bufferFrames = 0;
    double        sampleRate = 0.0;

    // scratch planar buffers
    std::vector<std::vector<float>> planar;
};

#endif // JUCE_WINDOWS
