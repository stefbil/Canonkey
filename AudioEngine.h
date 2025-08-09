#pragma once
#include <JuceHeader.h>
#include <functional>
#if JUCE_WINDOWS
#include "WasapiLoopback.h"
#endif

class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

#if JUCE_WINDOWS
    std::unique_ptr<WasapiLoopback> wasapiLoopback;
#endif

    // Tries: WASAPI loopback → native WASAPI loopback fallback → ASIO input → WASAPI input
    bool startBestInputForLive(juce::String& error);
    bool startWASAPILoopback(juce::String& error);
    bool startASIOInputDefault(juce::String& error);
    bool startWASAPIInputDefault(juce::String& error);
    void stop();

    bool isRunning() const { return running.load(); }

    struct DeviceEntry
    {
        juce::String type;   // "ASIO", "Windows Audio", etc.
        juce::String name;   // device name as seen by JUCE
        bool isInput = true;
        bool isLoopback = false;
    };

    std::vector<DeviceEntry> enumerateDevices();
    bool startWithDevice(const DeviceEntry& entry, juce::String& error);

    struct DeviceInfo
    {
        juce::String type, inputName;
        double sampleRate = 0.0;
        int blockSize = 0;
        int numIn = 0;
    };
    DeviceInfo getCurrentDeviceInfo() const;

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }

    // Audio callback: delivered on realtime thread
    std::function<void(const float* const* input, int numCh, int numSamples, double sampleRate)>
        onAudioBlock;

    // AudioIODeviceCallback (JUCE 8)
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext(const float* const* input, int numIn,
        float* const* output, int numOut,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& context) override;

private:
    juce::AudioDeviceManager deviceManager;
    std::atomic<bool> running{ false };

    mutable std::mutex infoMutex;
    DeviceInfo info;

    bool setDeviceType(const juce::String& typeName);
    juce::String findWASAPILoopbackName();
    juce::String findDefaultInputForType(const juce::String& typeName);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
