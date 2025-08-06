/*
  ==============================================================================

    BPMDetector.h
    Author:  stefbil

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include <vector>
#include <deque>

class BPMDetector
{
public:
    BPMDetector();
    ~BPMDetector();

    void prepare(double sampleRate, int samplesPerBlock);
    void processBlock(const float* audioData, int numSamples);

    float getCurrentBPM() const { return currentBPM; }
    float getConfidence() const { return confidence; }
    bool isBeatDetected() const { return beatDetected; }

    void reset();

private:
    // Audio processing
    double sampleRate = 44100.0;
    int blockSize = 512;

    // Onset detection
    juce::dsp::FFT fft;
    std::vector<float> fftBuffer;
    std::vector<float> magnitudeSpectrum;
    std::vector<float> previousMagnitudes;
    std::vector<float> spectralFlux;

    // Beat tracking
    std::deque<double> onsetTimes;
    std::deque<float> onsetStrengths;
    std::vector<float> tempogram;

    // BPM analysis
    float currentBPM = 0.0f;
    float confidence = 0.0f;
    bool beatDetected = false;

    // Parameters
    static const int FFT_ORDER = 10; // 2^10 = 1024 samples
    static const int FFT_SIZE = 1 << FFT_ORDER;
    static const int ONSET_HISTORY_SIZE = 200; // ~5 seconds at 40 onsets/sec
    static const int MIN_BPM = 60;
    static const int MAX_BPM = 200;

    // Timing
    double currentTimeSeconds = 0.0;
    int samplesProcessed = 0;

    // Onset detection parameters
    float onsetThreshold = 0.3f;
    float adaptiveThreshold = 0.0f;

    // BPM calculation
    void detectOnset(float spectralFluxValue);
    void updateBPM();
    float calculateBPMFromOnsets();
    float autocorrelateIntervals(const std::vector<float>& intervals);

    // Utility functions
    void updateAdaptiveThreshold(float currentFlux);
    float calculateSpectralFlux();
    void processFFT(const float* audioData, int numSamples);
};
