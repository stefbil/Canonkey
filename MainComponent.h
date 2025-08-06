#pragma once

#include <JuceHeader.h>
#include "BPMDetector.h"
#include <atomic>
#include <algorithm>

class MainComponent : public juce::Component,
    public juce::AudioIODeviceCallback,
    public juce::Timer
{
public:
    //==============================================================================
    MainComponent();
    ~MainComponent() override;

    //==============================================================================
    void paint(juce::Graphics& g) override;
    void resized() override;

    //==============================================================================
    // These are the pure virtual functions from AudioIODeviceCallback that we must override.
    void audioDeviceIOCallback(const float** inputChannelData, int numInputChannels,
        float** outputChannelData, int numOutputChannels,
        int numSamples) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    //==============================================================================
    // This overrides the virtual function from the Timer class.
    void timerCallback() override;

private:
    //==============================================================================
    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> audioSetupComp;

    BPMDetector bpmDetector;
    std::atomic<float> inputLevel{ 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
