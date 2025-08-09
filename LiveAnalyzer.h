/*
  ==============================================================================

    LiveAnalyzer.h
    Created: 10 Aug 2025 12:40:09am
    Author:  stefbil

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <vector>
#include <thread>
#include <functional>
#include "RingBuffer.h"
#include <array>

// Real-time BPM estimator using spectral-flux + autocorrelation.
//  - Pops mono audio from RingBuffer
//  - Frames: N = 2048, hop = 512 (75% overlap)
//  - Spectral flux onset envelope
//  - Autocorrelation over last ~8 s to estimate BPM in [60..200]


class LiveAnalyzer
{
public:
    struct Settings
    {
        int   fftOrder   = 11;   // 2^11 = 2048
        int   hop        = 512;  // samples
        float minBPM     = 60.0f;
        float maxBPM     = 200.0f;
        float updateHz   = 2.0f; // UI update rate
        float fluxSmoothing = 0.4f; // IIR smoothing for onset envelope
        float bpmSmoothing  = 0.7f; // EMA on BPM output
        float analysisSeconds = 10.0f; // history for ACF
    };

    using BpmCallback = std::function<void (double bpm, double confidence)>;

    LiveAnalyzer (RingBuffer& fifo, std::atomic<double>& sampleRateRef, const Settings& s = {});
    ~LiveAnalyzer();

    void start();
    void stop();
    bool isRunning() const noexcept { return running.load(); }

    void setBpmCallback (BpmCallback cb) { onBpm = std::move (cb); }

private:
    void threadFunc();
    void processFrames (const float* input, int num);
    void computeBpmAndPublish();

    // config
    Settings settings;

    // I/O
    RingBuffer&          rb;
    std::atomic<double>& srRef;

    // thread
    std::thread        worker;
    std::atomic<bool>  running { false };

    // framing / FFT
    const int          fftOrder;
    const int          fftSize;
    const int          hop;
    juce::dsp::FFT     fft;
    std::vector<float> window;           // Hann
    std::vector<float> frame;            // time-domain frame
    std::vector<float> prevMag;          // previous magnitude spectrum
    std::vector<float> fftRealImag;      // interleaved real/imag
    std::vector<float> mag;              // magnitude spectrum

    // flux envelope history (ring buffer of last M values)
    std::vector<float> fluxHist;
    int                fluxWrite = 0;
    int                fluxCount = 0;
    int                maxFlux   = 0;
    float              fluxPrev  = 0.0f;

    // frame overlap state
    std::vector<float> overlap; // keeps tail between hops
    int                overlapFill = 0;

    // output
    double             bpmEMA = 0.0;
    BpmCallback        onBpm;

    // scratch
    std::vector<float> acf;     // autocorrelation over flux history
    std::vector<float> scratch;

    // helpers
    static inline float hann (int n, int N)
    {
        return 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi * (float) n / (float) (N - 1)));
    }

    // recent BPM median stabilizer (very light)
    std::array<double, 8> recentBpm{};  // circular buffer
    int recentCount = 0;
    int recentWrite = 0;
};
