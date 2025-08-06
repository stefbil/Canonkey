#include "MainComponent.h"
#include <iomanip>

#ifdef _WIN32
#include <Windows.h>
#include <io.h>
#include <fcntl.h>
#include <iostream>
#endif

//==============================================================================
MainComponent::MainComponent()
{
#ifdef _WIN32
    // Allocate a console for this GUI application
    AllocConsole();
    FILE* pCout;
    freopen_s(&pCout, "CONOUT$", "w", stdout);
    SetConsoleTitle("CanonKey Debug Console");
#endif

    setSize(500, 600);

    // Initialize the device manager and add this component as the audio callback.
    deviceManager.initialiseWithDefaultDevices(2, 0);
    deviceManager.addAudioCallback(this);

    // Create the audio device selector component.
    audioSetupComp = std::make_unique<juce::AudioDeviceSelectorComponent>(
        deviceManager,
        0, 2, 0, 0, false, false, true, false);

    addAndMakeVisible(*audioSetupComp);
    startTimer(50);
}

MainComponent::~MainComponent()
{
    deviceManager.removeAudioCallback(this);
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds();
    audioSetupComp->setBounds(bounds.removeFromTop(400));
}

void MainComponent::timerCallback()
{
    // This timer callback simply triggers a repaint to update the UI.
    repaint();
}

void MainComponent::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    double sampleRate = device->getCurrentSampleRate();
    int samplesPerBlock = device->getCurrentBufferSizeSamples();

    std::cout << "Audio device starting: " << device->getName().toStdString() << std::endl;
    std::cout << "Sample Rate: " << sampleRate << ", Block Size: " << samplesPerBlock << std::endl;

    bpmDetector.prepare(sampleRate, samplesPerBlock);
}

void MainComponent::audioDeviceStopped()
{
    std::cout << "Audio device stopped." << std::endl;
    bpmDetector.reset();
}

void MainComponent::audioDeviceIOCallback(const float** inputChannelData, int numInputChannels,
    float** outputChannelData, int numOutputChannels, int numSamples)
{
    if (numInputChannels > 0 && inputChannelData[0] != nullptr)
    {
        juce::AudioBuffer<float> tempBuffer((float**)inputChannelData, 1, numSamples);
        auto level = tempBuffer.getRMSLevel(0, 0, numSamples);
        inputLevel = (std::max)(level, inputLevel.load() * 0.95f);
        bpmDetector.processBlock(inputChannelData[0], numSamples);
    }

    for (int i = 0; i < numOutputChannels; ++i)
        if (outputChannelData[i] != nullptr)
            juce::FloatVectorOperations::clear(outputChannelData[i], numSamples);
}


void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    auto area = getLocalBounds();
    area.removeFromTop(audioSetupComp->getBottom());
    area = area.reduced(20);

    // --- BPM Display ---
    auto bpmArea = area.removeFromTop(area.getHeight() / 2);
    g.setColour(juce::Colours::white);
    g.setFont(48.0f);
    float currentBPM = bpmDetector.getCurrentBPM();
    juce::String bpmText = (currentBPM > 0.0f) ? juce::String(currentBPM, 1) + " BPM" : "--- BPM";
    g.drawFittedText(bpmText, bpmArea, juce::Justification::centred, 1);

    // --- Meter Area ---
    auto meterArea = area.removeFromBottom(50);
    g.setColour(juce::Colours::grey);
    g.drawRect(meterArea);

    float levelDb = juce::Decibels::gainToDecibels(inputLevel.load(), -60.0f);
    juce::NormalisableRange<float> dbRange(-60.0f, 6.0f);
    float meterWidth = dbRange.convertTo0to1(levelDb) * meterArea.getWidth();
    meterWidth = (std::max)(0.0f, meterWidth);

    juce::Rectangle<float> meterBar((float)meterArea.getX(), (float)meterArea.getY(), meterWidth, (float)meterArea.getHeight());

    juce::ColourGradient gradient(juce::Colours::green, (float)meterArea.getX(), 0.0f,
        juce::Colours::red, (float)meterArea.getRight(), 0.0f,
        false);
    g.setGradientFill(gradient);
    g.fillRect(meterBar);
}
