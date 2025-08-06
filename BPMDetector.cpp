/*
  ==============================================================================

    BPMDetector.cpp
    Created: 6 Aug 2025 8:02:37pm
    Author:  stefbil

  ==============================================================================
*/

#include "BPMDetector.h"
#include <algorithm>
#include <numeric>
#include <cmath>

BPMDetector::BPMDetector() : fft(FFT_ORDER)
{
    fftBuffer.resize(FFT_SIZE * 2, 0.0f);
    magnitudeSpectrum.resize(FFT_SIZE / 2, 0.0f);
    previousMagnitudes.resize(FFT_SIZE / 2, 0.0f);
}

BPMDetector::~BPMDetector()
{
}

void BPMDetector::prepare(double newSampleRate, int samplesPerBlock)
{
    sampleRate = newSampleRate;
    blockSize = samplesPerBlock;
    reset();
}

void BPMDetector::reset()
{
    std::fill(fftBuffer.begin(), fftBuffer.end(), 0.0f);
    std::fill(magnitudeSpectrum.begin(), magnitudeSpectrum.end(), 0.0f);
    std::fill(previousMagnitudes.begin(), previousMagnitudes.end(), 0.0f);

    onsetTimes.clear();
    onsetStrengths.clear();

    currentBPM = 0.0f;
    confidence = 0.0f;
    beatDetected = false;
    currentTimeSeconds = 0.0;
    samplesProcessed = 0;
    adaptiveThreshold = 0.0f;
}

void BPMDetector::processBlock(const float* audioData, int numSamples)
{
    // Process the audio block through FFT for onset detection
    processFFT(audioData, numSamples);

    // Calculate spectral flux for onset detection
    float spectralFluxValue = calculateSpectralFlux();

    // Update timing
    currentTimeSeconds = static_cast<double>(samplesProcessed) / sampleRate;
    samplesProcessed += numSamples;

    // Detect onset
    detectOnset(spectralFluxValue);

    // Update BPM calculation every few frames
    static int updateCounter = 0;
    if (++updateCounter >= 10) // Update every ~10 audio blocks
    {
        updateBPM();
        updateCounter = 0;
    }
}

void BPMDetector::processFFT(const float* audioData, int numSamples)
{
    // Shift existing data and add new samples
    int samplesToShift = std::min(numSamples, FFT_SIZE);

    // Shift the buffer
    for (int i = 0; i < FFT_SIZE - samplesToShift; ++i)
    {
        fftBuffer[i * 2] = fftBuffer[(i + samplesToShift) * 2];
        fftBuffer[i * 2 + 1] = 0.0f; // Imaginary part
    }

    // Add new samples
    for (int i = 0; i < samplesToShift; ++i)
    {
        int bufferIndex = (FFT_SIZE - samplesToShift + i) * 2;
        fftBuffer[bufferIndex] = audioData[i];
        fftBuffer[bufferIndex + 1] = 0.0f; // Imaginary part
    }

    // Apply window function (Hann window)
    for (int i = 0; i < FFT_SIZE; ++i)
    {
        float window = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * i / (FFT_SIZE - 1)));
        fftBuffer[i * 2] *= window;
    }

    // Perform FFT
    fft.performFrequencyOnlyForwardTransform(fftBuffer.data());

    // Calculate magnitude spectrum
    for (int i = 0; i < FFT_SIZE / 2; ++i)
    {
        float real = fftBuffer[i * 2];
        float imag = fftBuffer[i * 2 + 1];
        magnitudeSpectrum[i] = std::sqrt(real * real + imag * imag);
    }
}

float BPMDetector::calculateSpectralFlux()
{
    float flux = 0.0f;

    // Calculate spectral flux (sum of positive differences)
    for (int i = 1; i < static_cast<int>(magnitudeSpectrum.size()); ++i)
    {
        float diff = magnitudeSpectrum[i] - previousMagnitudes[i];
        if (diff > 0.0f)
            flux += diff;
    }

    // Update previous magnitudes
    previousMagnitudes = magnitudeSpectrum;

    return flux;
}

void BPMDetector::detectOnset(float spectralFluxValue)
{
    updateAdaptiveThreshold(spectralFluxValue);

    // Reset beat detection flag
    beatDetected = false;

    // Check if we have an onset
    if (spectralFluxValue > adaptiveThreshold && spectralFluxValue > onsetThreshold)
    {
        // Check if enough time has passed since last onset (prevent double detection)
        if (onsetTimes.empty() || (currentTimeSeconds - onsetTimes.back()) > 0.05) // 50ms minimum
        {
            onsetTimes.push_back(currentTimeSeconds);
            onsetStrengths.push_back(spectralFluxValue);
            beatDetected = true;

            // Limit history size
            while (onsetTimes.size() > ONSET_HISTORY_SIZE)
            {
                onsetTimes.pop_front();
                onsetStrengths.pop_front();
            }
        }
    }
}

void BPMDetector::updateAdaptiveThreshold(float currentFlux)
{
    // Simple adaptive threshold: running average with decay
    const float alpha = 0.95f;
    adaptiveThreshold = alpha * adaptiveThreshold + (1.0f - alpha) * currentFlux;
}

void BPMDetector::updateBPM()
{
    if (onsetTimes.size() < 8) // Need at least 8 onsets for reliable BPM
    {
        confidence = 0.0f;
        return;
    }

    // Calculate BPM from recent onsets
    float detectedBPM = calculateBPMFromOnsets();

    if (detectedBPM > 0.0f && detectedBPM >= MIN_BPM && detectedBPM <= MAX_BPM)
    {
        // Smooth BPM changes
        if (currentBPM == 0.0f)
        {
            currentBPM = detectedBPM;
        }
        else
        {
            // Use a simple low-pass filter for smoothing
            float alpha = 0.15f; // Smoothing factor
            currentBPM = alpha * detectedBPM + (1.0f - alpha) * currentBPM;
        }

        // Calculate confidence based on consistency
        float bpmVariation = std::abs(detectedBPM - currentBPM) / currentBPM;
        confidence = std::max(0.0f, 1.0f - bpmVariation * 5.0f); // Higher variation = lower confidence
        confidence = std::min(1.0f, confidence);
    }
}

float BPMDetector::calculateBPMFromOnsets()
{
    if (onsetTimes.size() < 4)
        return 0.0f;

    // Calculate intervals between consecutive onsets
    std::vector<float> intervals;
    for (size_t i = 1; i < onsetTimes.size(); ++i)
    {
        float interval = static_cast<float>(onsetTimes[i] - onsetTimes[i - 1]);
        if (interval > 0.1f && interval < 2.0f) // Valid interval range (30-600 BPM)
        {
            intervals.push_back(interval);
        }
    }

    if (intervals.empty())
        return 0.0f;

    // Find the most common interval using autocorrelation
    float bestInterval = autocorrelateIntervals(intervals);

    if (bestInterval > 0.0f)
    {
        float bpm = 60.0f / bestInterval;

        // Handle half-time and double-time
        while (bpm < MIN_BPM && bpm > 0)
            bpm *= 2.0f;
        while (bpm > MAX_BPM)
            bpm *= 0.5f;

        return bpm;
    }

    return 0.0f;
}

float BPMDetector::autocorrelateIntervals(const std::vector<float>& intervals)
{
    if (intervals.size() < 3)
        return 0.0f;

    // Simple approach: find the median interval
    std::vector<float> sortedIntervals = intervals;
    std::sort(sortedIntervals.begin(), sortedIntervals.end());

    float medianInterval;
    size_t size = sortedIntervals.size();
    if (size % 2 == 0)
    {
        medianInterval = (sortedIntervals[size / 2 - 1] + sortedIntervals[size / 2]) * 0.5f;
    }
    else
    {
        medianInterval = sortedIntervals[size / 2];
    }

    return medianInterval;
}
