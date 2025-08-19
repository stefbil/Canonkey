#pragma once
#include <JuceHeader.h>
#include <vector>
#include <deque>
#include <atomic>
#include <cmath>
#include <algorithm>

// Pipeline (per hop):
//   STFT (Hann) -> Mel-like triangular bands (log-compressed)
//   -> Spectral Flux (positive diff) with adaptive threshold (mean+k*std)
//   -> Whitening (subtract moving average) & smoothing (EMA)
//   -> Autocorrelation over recent envelope (≈ 8–12 s)
//   -> Peak picking + comb-filter verification (harmonics & subharmonics)
//   -> Debounced BPM estimate + confidence
class BpmTracker
{
public:
    explicit BpmTracker(double sampleRate);
    ~BpmTracker() = default;

    // Single reset with default argument (no overload, avoid ambiguity)
    void reset(bool hard = true);

    // Feed time-domain mono samples
    void processMono(const float* samples, int numSamples);

    // Results (thread-safe)
    float getBpm() const noexcept { return currentBpm.load(); }
    float getConfidence() const noexcept { return currentConf.load(); }

    // Exposed so helper functions
    struct Tri { int a, b, c; };

private:
    // ---------------- Config ----------------
    double sr = 44100.0;
    int frameSize = 2048;        // STFT size
    int hopSize = 512;         // hop; envRate ≈ sr/hop
    int fftOrder = 11;          // 2^11 = 2048
    int numBands = 6;           // mel-like bands for flux
    float logCompression = 1.0f; // log1p(lambda*x), lambda=1
    float threshK = 1.0f;        // adaptive threshold = mean + k*std
    float emaAlpha = 0.25f;      // envelope smoothing (EMA)
    float maSeconds = 0.8f;      // whitening window
    float minBPM = 60.0f;
    float maxBPM = 200.0f;
    float analysisSeconds = 10.0f; // ACF window
    float reestimateEvery = 0.25f; // seconds between ACF runs
    int   topPeaks = 5;

    // ---------------- State ----------------
    // FFT
    juce::dsp::FFT fft;
    juce::HeapBlock<float> window;       // Hann
    juce::HeapBlock<float> fftBuffer;    // 2*frameSize for in-place JUCE FFT
    std::vector<float> inFrame;          // time-domain frame
    std::vector<float> mag;              // magnitude spectrum

    std::vector<Tri> bands;
    std::vector<float> bandMag, prevBandMag;

    // Input FIFO to form overlapping frames
    std::vector<float> fifo;
    int fifoFill = 0;

    // Spectral flux & envelope
    double envRate = 0.0;                // sr / hop
    std::deque<float> fluxRaw;           // last N fluxes
    std::deque<float> fluxMA;            // moving average for whitening
    std::deque<float> onsetEnv;          // whitened + smoothed
    int maLen = 1;                       // samples for moving average in env domain
    int envMaxLen = 1;                   // analysis window length (frames)
    float emaState = 0.0f;
    double lastACFTime = 0.0;            // in env frames

    // ACF buffer reused
    std::vector<float> acfBuf;

    // Debounce / history
    std::deque<float> bpmHistory;        // small median filter
    int bpmHistLen = 8;

    // Results
    std::atomic<float> currentBpm{ 0.0f };
    std::atomic<float> currentConf{ 0.0f };

    // ---------------- Impl helpers ----------------
    void buildWindow();
    void buildBands();
    void ensureFifoCapacity(int needed);
    void pushEnvelope(float fluxVal);
    void maybeComputeTempo(); // runs ACF at intervals

    static float median(std::deque<float> v);
    static float mean(const std::deque<float>& v);
    static float stddev(const std::deque<float>& v, float m);

    void computeAcf(const std::vector<float>& x, int minLag, int maxLag, std::vector<float>& out);
    float combScoreAtLag(const std::vector<float>& acf, int idx); // idx is (lag - minLag)
    float lagToBpm(int lag) const { return (float)(60.0 * envRate / (double)lag); }
    int   bpmToLag(float bpm) const
    {
        bpm = juce::jlimit(minBPM, maxBPM, bpm);
        return juce::jmax(1, (int)std::round((60.0 * envRate) / (double)bpm));
    }
};
