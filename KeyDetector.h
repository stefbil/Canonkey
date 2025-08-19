#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <functional>
#include <vector>

// 24-key real-time detector (C maj..B maj, C min..B min)
// HPCP from spectral peaks + online Viterbi smoothing + dwell/margin gating
class KeyDetector {
public:
    struct Result {
        int   keyIndex = -1;   // 0..11=C..B (C-based), -1 unknown
        bool  isMinor = false;
        float confidence = 0.0f; // 0..1 (margin)
    };

    struct Settings {
        // FFT / framing
        int    fftOrder = 12;     // 4096
        int    hop = 2048;   // 50% overlap
        double minHz = 55.0;
        double maxHz = 5000.0;
        float  refA4 = 440.0f;

        // Peak→HPCP
        float  gamma = 0.67f;   // magnitude compression
        float  peakRelThreshDb = -36.0f;  // local-peak threshold
        int    maxPeaks = 64;
        float  kernelWidth = 0.75f;   // semitone width for cosine kernel

        // Smoothing
        double chromaDecaySec = 6.0;     // EMA for chroma
        double tuningDecaySec = 12.0;    // EMA for tuning (cents)

        // Viterbi + publishing
        double publishMinIntervalSec = 0.5;
        double dwellRequiredSec = 2.5;
        float  marginRequired = 0.08f; // leader – runner-up
        float  stayBias = 0.04f; // prefer staying
        float  neighborBonus = 0.02f; // allow related moves
        float  transitionPenalty = 0.04f; // default penalty for large jumps
    };

    KeyDetector(double sampleRate, const Settings& s = {});
    void reset(double newSampleRate = 0.0);

    // Feed **mono** audio (any block size). RT-safe.
    void processMono(const float* samples, int numSamples);

    // Optional callback (called on caller thread)
    void setCallback(std::function<void(const Result&)> cb) { onResult = std::move(cb); }

    Result getLast() const { return lastResult.load(); }

private:
    // pipeline
    void analyzeFrame();
    void computePeaksAndHpcp();
    void score24();            // cosine against KS templates -> instScore[24]
    void viterbiStep();        // online Viterbi update
    void maybePublish();       // dwell/margin/rate-limit to UI

    // helpers
    void ensureBuffers();
    static inline float hann(int n, int N) {
        return 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * (float)n / (float)(N - 1)));
    }
    static inline int wrap12(int x) { x %= 12; return x < 0 ? x + 12 : x; }
    static inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
    static double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

    // config/state
    Settings cfg;
    double   sr = 44100.0;
    int      fftOrder, fftSize, hop;

    // STFT
    juce::dsp::FFT fft;
    std::vector<float> window, overlap;
    int overlapFill = 0;
    std::vector<float> fftBuf;   // interleaved re/im
    std::vector<float> magRaw, mag; // raw & compressed

    // HPCP
    std::array<float, 12> frameChroma{ {} };
    std::array<float, 12> chromaEMA{ {} }; // smoothed
    float tuningCentsEMA = 0.0f;

    // KS profiles (normalized)
    std::array<float, 12> profMaj{ {} };
    std::array<float, 12> profMin{ {} };

    // 24 instantaneous scores (0..11 maj, 12..23 min)
    std::array<float, 24> instScore{ {} }; // ~0..1 cosine

    // Viterbi state (log-likelihood-ish but just do additive scores)
    std::array<float, 24> viterbi{ {} };    // accumulated score per state
    int   vitCurrent = -1;                // current best state 0..23
    bool  vitInit = false;

    // Publishing gate
    int    pendingKey = -1;
    double pendingSinceMs = 0.0;
    double lastPublishMs = 0.0;

    // output
    std::atomic<Result> lastResult{};
    std::function<void(const Result&)> onResult;
};
