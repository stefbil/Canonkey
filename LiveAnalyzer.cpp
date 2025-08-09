/*
  ==============================================================================

    LiveAnalyzer.cpp
    Created: 10 Aug 2025 12:40:39am
    Author:  stefbil

  ==============================================================================
*/

#include "LiveAnalyzer.h"
#include <algorithm>

LiveAnalyzer::LiveAnalyzer (RingBuffer& fifo, std::atomic<double>& sampleRateRef, const Settings& s)
    : settings (s),
      rb (fifo),
      srRef (sampleRateRef),
      fftOrder (settings.fftOrder),
      fftSize  (1 << settings.fftOrder),
      hop      (settings.hop),
      fft      (settings.fftOrder)
{
    window.resize (fftSize);
    for (int i = 0; i < fftSize; ++i) window[i] = hann (i, fftSize);

    frame.resize (fftSize, 0.0f);
    overlap.resize (fftSize, 0.0f);
    prevMag.resize (fftSize / 2 + 1, 0.0f);
    fftRealImag.resize ((size_t) fftSize * 2, 0.0f); // JUCE FFT expects interleaved R,I
    mag.resize (fftSize / 2 + 1, 0.0f);

    // Flux history length (~analysisSeconds with hop)
    const int fluxLen = std::max (64, (int) std::round ((settings.analysisSeconds * 44100.0) / (double) hop));
    fluxHist.assign (fluxLen, 0.0f);
    acf.assign (fluxLen, 0.0f);
    scratch.assign (fluxLen, 0.0f);
    maxFlux = fluxLen;
}

LiveAnalyzer::~LiveAnalyzer()
{
    stop();
}

void LiveAnalyzer::start()
{
    if (running.exchange(true)) return;
    worker = std::thread (&LiveAnalyzer::threadFunc, this);
}

void LiveAnalyzer::stop()
{
    if (! running.exchange(false)) return;
    if (worker.joinable()) worker.join();
}

void LiveAnalyzer::threadFunc()
{
    juce::Thread::setCurrentThreadName ("LiveAnalyzer");

    const int chunk = hop; // how many samples we try to pop each loop
    std::vector<float> temp ((size_t) chunk);

    double nextUpdate = juce::Time::getMillisecondCounterHiRes();

    while (running.load())
    {
        const double sr = srRef.load (std::memory_order_relaxed);
        if (sr < 8000.0) { juce::Thread::sleep (10); continue; } // wait for real SR

        // Pop a small chunk; if none, sleep briefly
        const size_t got = rb.pop (temp.data(), (size_t) chunk);
        if (got == 0) { juce::Thread::sleep (3); continue; }

        processFrames (temp.data(), (int) got);

        // throttle UI updates to ~updateHz
        const double now = juce::Time::getMillisecondCounterHiRes();
        const double periodMs = 1000.0 / std::max (0.5f, settings.updateHz);
        if (now >= nextUpdate)
        {
            computeBpmAndPublish();
            nextUpdate = now + periodMs;
        }
    }
}

void LiveAnalyzer::processFrames (const float* input, int num)
{
    int idx = 0;
    while (idx < num)
    {
        // Fill overlap buffer until we have a full frame
        const int need = fftSize - overlapFill;
        const int take = std::min (need, num - idx);
        std::copy (input + idx, input + idx + take, overlap.data() + overlapFill);
        overlapFill += take;
        idx += take;

        if (overlapFill < fftSize)
            continue;

        // copy with window into frame and slide by hop
        for (int i = 0; i < fftSize; ++i)
            frame[i] = overlap[i] * window[i];

        // shift left by hop
        const int remain = fftSize - hop;
        std::memmove (overlap.data(), overlap.data() + hop, (size_t) remain * sizeof (float));
        overlapFill = remain;

        // FFT
        std::fill (fftRealImag.begin(), fftRealImag.end(), 0.0f);
        for (int i = 0; i < fftSize; ++i)
            fftRealImag[(size_t) (2 * i)] = frame[i]; // real = frame, imag = 0
        fft.performRealOnlyForwardTransform (fftRealImag.data());

        // Magnitude spectrum (N/2 + 1 bins)
        const int bins = fftSize / 2;
        for (int k = 0; k <= bins; ++k)
        {
            const float re = fftRealImag[(size_t) (2 * k)];
            const float im = fftRealImag[(size_t) (2 * k + 1)];
            mag[(size_t) k] = std::sqrt (re * re + im * im);
        }

        // Spectral flux: positive changes only
        float flux = 0.0f;
        for (int k = 0; k <= bins; ++k)
        {
            const float diff = mag[(size_t) k] - prevMag[(size_t) k];
            if (diff > 0.0f) flux += diff;
            prevMag[(size_t) k] = mag[(size_t) k];
        }

        // Normalize a bit by energy to reduce loudness bias (simple)
        float energy = 0.0f;
        for (int k = 0; k <= bins; ++k) energy += mag[(size_t) k];
        if (energy > 1e-6f) flux /= energy;

        // Smooth flux via simple one-pole IIR
        flux = settings.fluxSmoothing * fluxPrev + (1.0f - settings.fluxSmoothing) * flux;
        fluxPrev = flux;

        // Write to flux history ring
        fluxHist[(size_t) fluxWrite] = flux;
        fluxWrite = (fluxWrite + 1) % maxFlux;
        if (fluxCount < maxFlux) ++fluxCount;
    }
}

void LiveAnalyzer::computeBpmAndPublish()
{
    if (fluxCount < 32 || onBpm == nullptr)
        return;

    // unwrap history into scratch[0..fluxCount-1], newest at the end
    const int M = fluxCount;
    for (int i = 0; i < M; ++i)
    {
        const int idx = (fluxWrite - M + i + maxFlux) % maxFlux;
        scratch[(size_t)i] = fluxHist[(size_t)idx];
    }

    // remove DC (mean)
    double mean = 0.0;
    for (int i = 0; i < M; ++i) mean += scratch[(size_t)i];
    mean /= (double)M;
    for (int i = 0; i < M; ++i) scratch[(size_t)i] = (float)(scratch[(size_t)i] - mean);

    // energy for normalization (zero-lag)
    double energy0 = 0.0;
    for (int i = 0; i < M; ++i) energy0 += (double)scratch[(size_t)i] * (double)scratch[(size_t)i];
    if (energy0 < 1e-12) return;

    // autocorrelation (simple) and normalize by energy
    std::fill(acf.begin(), acf.end(), 0.0f);
    for (int lag = 1; lag < M; ++lag)
    {
        double acc = 0.0;
        const int N = M - lag;
        const float* x = scratch.data();
        for (int i = 0; i < N; ++i)
            acc += (double)x[i] * (double)x[i + lag];

        acf[(size_t)lag] = (float)(acc / energy0); // normalized ACF
    }

    // hop time and BPM bounds
    const double sr = std::max(8000.0, srRef.load(std::memory_order_relaxed));
    const double hopSec = (double)hop / sr;

    const double minT = 60.0 / settings.minBPM;
    const double maxT = 60.0 / settings.maxBPM;

    const int minLag = (int)std::floor(minT / hopSec);
    const int maxLag = (int)std::ceil(maxT / hopSec);

    // Search with harmonic-sum score:
    // score(l) = ACF(l) + 0.5*ACF(2l) + 0.25*ACF(3l)
    int   bestLag = -1;
    float bestScore = -1.0f;

    for (int lag = std::max(1, maxLag); lag <= std::min(M - 1, minLag); ++lag)
    {
        float s = acf[(size_t)lag];
        const int l2 = lag * 2;
        const int l3 = lag * 3;
        if (l2 < M) s += 0.5f * acf[(size_t)l2];
        if (l3 < M) s += 0.25f * acf[(size_t)l3];

        if (s > bestScore) { bestScore = s; bestLag = lag; }
    }
    if (bestLag <= 0) return;

    // Parabolic interpolation for sub-lag peak (around bestLag) on the base ACF
    double lagf = (double)bestLag;
    if (bestLag > 1 && bestLag + 1 < M)
    {
        const double ym1 = acf[(size_t)(bestLag - 1)];
        const double y0 = acf[(size_t)bestLag];
        const double yp1 = acf[(size_t)(bestLag + 1)];
        const double denom = (ym1 - 2.0 * y0 + yp1);
        if (std::abs(denom) > 1e-12)
            lagf = (double)bestLag - 0.5 * (yp1 - ym1) / denom;
    }

    // Convert to BPM and clamp
    const double period = lagf * hopSec;
    const double rawBpm = 60.0 / std::max(1e-6, period);
    double bpm = juce::jlimit((double)settings.minBPM,
        (double)settings.maxBPM,
        rawBpm);

    // Gentle octave correction (favor 80–160)
    if (bpm < 80.0)      bpm *= 2.0;
    else if (bpm > 160.) bpm *= 0.5;

    // --- Short median stabilizer (over recent 8 picks) ---
    recentBpm[(size_t)recentWrite] = bpm;
    recentWrite = (recentWrite + 1) % (int)recentBpm.size();
    if (recentCount < (int)recentBpm.size()) ++recentCount;

    // copy and median
    double median = bpm;
    {
        std::vector<double> tmp;
        tmp.reserve((size_t)recentCount);
        for (int i = 0; i < recentCount; ++i) tmp.push_back(recentBpm[(size_t)i]);
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        median = tmp[tmp.size() / 2];
        // (Optional) average middle two if even count:
        if ((tmp.size() % 2) == 0)
        {
            auto maxBelow = *std::max_element(tmp.begin(), tmp.begin() + tmp.size() / 2);
            median = 0.5 * (median + maxBelow);
        }
    }

    // confidence: normalized peak vs neighborhood (use normalized ACF)
    double neigh = 0.0;
    int nb = 0;
    for (int d = -4; d <= 4; ++d)
    {
        if (d == 0) continue;
        const int k = juce::jlimit(1, M - 1, bestLag + d);
        neigh += acf[(size_t)k];
        ++nb;
    }
    const double conf = juce::jlimit(0.0, 1.0, (double)bestScore / (1e-9 + neigh / std::max(1, nb)));

    // Smooth final BPM a bit (EMA)
    if (bpmEMA <= 0.0) bpmEMA = median;
    bpmEMA = settings.bpmSmoothing * bpmEMA + (1.0 - settings.bpmSmoothing) * median;

    onBpm(bpmEMA, conf);
}
