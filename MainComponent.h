#pragma once
#include <JuceHeader.h>
#include "BPMDetector.h"
#include <atomic>
#include <algorithm>

#ifdef _WIN32
#include <Windows.h>
#include <io.h>
#include <fcntl.h>
#include <iostream>
#endif

class MainComponent : public juce::Component,
    public juce::AudioIODeviceCallback, // We'll implement the audio callback directly
    public juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    // --- Component overrides ---
    void paint(juce::Graphics&) override;
    void resized() override;

    // --- AudioIODeviceCallback overrides ---
    void audioDeviceIOCallback(const float** inputChannelData, int numInputChannels,
        float** outputChannelData, int numOutputChannels, int numSamples) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    // --- Timer callback ---
    void timerCallback() override { repaint(); }

private:
    // This manager handles all the audio device logic.
    juce::AudioDeviceManager deviceManager;
    // This is the pre-built JUCE component that provides the UI for device selection.
    std::unique_ptr<juce::AudioDeviceSelectorComponent> audioSetupComp;

    BPMDetector bpmDetector;
    std::atomic<float> inputLevel{ 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
