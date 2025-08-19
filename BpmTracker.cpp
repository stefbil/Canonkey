#include "BpmTracker.h"

// ---------------- Utilities ----------------
static inline float hann(int n, int N) noexcept
{
    return 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * (float)n / (float)(N - 1)));
}

static inline float triWeight(int i, int a, int b, int c) noexcept
{
    if (i <= a || i >= c) return 0.0f;
    if (i == b) return 1.0f;
    if (i < b) return (float)(i - a) / (float)(b - a);
    return (float)(c - i) / (float)(c - b);
}

static inline double hzToMel(double f)
{
    // HTK mel
    return 2595.0 * std::log10(1.0 + f / 700.0);
}
static inline double melToHz(double m)
{
    return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0);
}

// ---------------- BpmTracker ----------------
BpmTracker::BpmTracker(double sampleRate)
    : sr(sampleRate > 0.0 ? sampleRate : 44100.0),
    fft(juce::jlimit(4, 16, (int)std::round(std::log2((double)frameSize))))
{
    // Recompute derived sizes
    fftOrder = (int)std::round(std::log2((double)frameSize));
    frameSize = 1 << fftOrder;
    hopSize = juce::jmax(128, hopSize);
    envRate = sr / (double)hopSize;

    // Envelope windows
    maLen = juce::jmax(3, (int)std::round(maSeconds * envRate));
    envMaxLen = juce::jmax((int)std::round(analysisSeconds * envRate), maLen + 1);

    // Buffers
    window.allocate(frameSize, true);
    fftBuffer.allocate((size_t)(2 * frameSize), true);
    inFrame.assign(frameSize, 0.0f);
    mag.assign(frameSize / 2 + 1, 0.0f);

    bandMag.assign((size_t)numBands, 0.0f);
    prevBandMag.assign((size_t)numBands, 0.0f);
    buildWindow();
    buildBands();

    fifo.resize((size_t)(4 * frameSize));
    fifoFill = 0;

    onsetEnv.clear(); fluxRaw.clear(); fluxMA.clear();
    acfBuf.resize(1);
    bpmHistory.clear();

    currentBpm.store(0.0f);
    currentConf.store(0.0f);
}

void BpmTracker::reset(bool /*hard*/)
{
    std::fill(inFrame.begin(), inFrame.end(), 0.0f);
    std::fill(mag.begin(), mag.end(), 0.0f);
    std::fill(bandMag.begin(), bandMag.end(), 0.0f);
    std::fill(prevBandMag.begin(), prevBandMag.end(), 0.0f);
    fifoFill = 0;
    emaState = 0.0f;
    onsetEnv.clear();
    fluxRaw.clear();
    fluxMA.clear();
    bpmHistory.clear();
    currentBpm.store(0.0f);
    currentConf.store(0.0f);
    lastACFTime = 0.0;
}

void BpmTracker::buildWindow()
{
    for (int n = 0; n < frameSize; ++n)
        window[n] = hann(n, frameSize);
}

void BpmTracker::buildBands()
{
    bands.clear();

    // Mel-spaced triangular bands between 30 Hz and 8 kHz (or Nyquist)
    const double ny = sr * 0.5;
    const double fMin = 30.0;
    const double fMax = std::min(8000.0, ny - 1.0);

    const double mMin = hzToMel(fMin);
    const double mMax = hzToMel(fMax);
    const int B = juce::jlimit(3, 12, numBands);

    std::vector<int> centers;
    centers.reserve(B);
    for (int b = 0; b < B; ++b)
    {
        double m = mMin + (mMax - mMin) * (double)(b + 1) / (double)(B + 1);
        double f = melToHz(m);
        int bin = (int)std::round(f * (double)frameSize / sr);
        bin = juce::jlimit(1, frameSize / 2 - 1, bin);
        centers.push_back(bin);
    }

    for (int i = 0; i < B; ++i)
    {
        int c = centers[i];
        int a = juce::jlimit(1, frameSize / 2 - 1, c - juce::jmax(2, c / 3));
        int c2 = juce::jlimit(2, frameSize / 2, c + juce::jmax(2, c / 3));
        bands.push_back({ a, c, c2 });
    }
}

void BpmTracker::ensureFifoCapacity(int needed)
{
    if ((int)fifo.size() - fifoFill < needed)
        fifo.resize((size_t)(fifoFill + needed + frameSize));
}

static inline void magnitudeFromFFT(float* fftData, int frameSize, std::vector<float>& magOut)
{
    const int nBins = frameSize / 2 + 1;
    magOut.resize((size_t)nBins);
    // JUCE real FFT format:
    // [ Re0, ReN/2, Re1, Im1, Re2, Im2, ... Re(N/2-1), Im(N/2-1) ]
    magOut[0] = std::abs(fftData[0]);
    magOut[nBins - 1] = std::abs(fftData[1]);
    for (int k = 1; k < nBins - 1; ++k)
    {
        const float re = fftData[2 * k];
        const float im = fftData[2 * k + 1];
        magOut[(size_t)k] = std::sqrt(re * re + im * im);
    }
}

static inline float triangularBandEnergy(const std::vector<float>& mag, const BpmTracker::Tri& t)
{
    float acc = 0.0f, wsum = 0.0f;
    for (int i = t.a; i <= t.c; ++i)
    {
        const float w = triWeight(i, t.a, t.b, t.c);
        acc += w * mag[(size_t)i];
        wsum += w;
    }
    return (wsum > 0.0f ? acc / wsum : 0.0f);
}

float BpmTracker::median(std::deque<float> v)
{
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n & 1) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

float BpmTracker::mean(const std::deque<float>& v)
{
    if (v.empty()) return 0.0f;
    double s = 0.0; for (float x : v) s += x;
    return (float)(s / (double)v.size());
}

float BpmTracker::stddev(const std::deque<float>& v, float m)
{
    if (v.size() < 2) return 0.0f;
    double s = 0.0; for (float x : v) { const double d = x - m; s += d * d; }
    return (float)std::sqrt(s / (double)(v.size() - 1));
}

void BpmTracker::processMono(const float* samples, int numSamples)
{
    if (!samples || numSamples <= 0) return;

    ensureFifoCapacity(numSamples);
    std::memcpy(fifo.data() + fifoFill, samples, (size_t)numSamples * sizeof(float));
    fifoFill += numSamples;

    // Form overlapping frames at hopSize
    while (fifoFill >= frameSize)
    {
        // Window
        for (int n = 0; n < frameSize; ++n)
            inFrame[(size_t)n] = fifo[(size_t)n] * window[n];

        // Copy to FFT buffer (real in-place)
        std::memset(fftBuffer.getData(), 0, (size_t)(2 * frameSize) * sizeof(float));
        for (int i = 0; i < frameSize; ++i)
            fftBuffer[i] = inFrame[(size_t)i];

        fft.performRealOnlyForwardTransform(fftBuffer.getData());

        // Magnitude spectrum
        magnitudeFromFFT(fftBuffer.getData(), frameSize, mag);

        // Log compression of magnitudes
        for (auto& v : mag)
            v = std::log1p(logCompression * v);

        // Band energies
        for (size_t b = 0; b < bands.size(); ++b)
            bandMag[b] = triangularBandEnergy(mag, bands[b]);

        // Spectral flux across bands (positive diffs only)
        float flux = 0.0f;
        if (!prevBandMag.empty())
        {
            for (size_t b = 0; b < bands.size(); ++b)
            {
                const float d = bandMag[b] - prevBandMag[b];
                if (d > 0.0f) flux += d;
            }
        }
        prevBandMag = bandMag;

        pushEnvelope(flux);

        // Advance FIFO by hop
        if (fifoFill > hopSize)
            std::memmove(fifo.data(), fifo.data() + hopSize, (size_t)(fifoFill - hopSize) * sizeof(float));
        fifoFill -= hopSize;
    }
}

void BpmTracker::pushEnvelope(float fluxVal)
{
    // Keep short history for adaptive threshold (≈ 1.5 s)
    const int adaptLen = juce::jmax(maLen, (int)std::round(1.5 * envRate));
    fluxRaw.push_back(fluxVal);
    if ((int)fluxRaw.size() > adaptLen) fluxRaw.pop_front();

    const float m = mean(fluxRaw);
    const float sd = stddev(fluxRaw, m);
    const float thr = m + threshK * sd;

    float onset = juce::jmax(0.0f, fluxVal - thr);

    // Whitening via moving average subtraction (slow trend removal)
    fluxMA.push_back(fluxVal);
    if ((int)fluxMA.size() > maLen) fluxMA.pop_front();
    const float ma = mean(fluxMA);
    onset = juce::jmax(0.0f, onset - 0.5f * ma);

    // EMA smoothing
    emaState = (1.0f - emaAlpha) * emaState + emaAlpha * onset;
    const float env = emaState;

    onsetEnv.push_back(env);
    if ((int)onsetEnv.size() > envMaxLen) onsetEnv.pop_front();

    // Determine if it's time to recompute ACF (≈ every reestimateEvery seconds)
    lastACFTime += 1.0;
    const double framesPerUpdate = reestimateEvery * envRate;
    if (lastACFTime >= framesPerUpdate)
    {
        lastACFTime = 0.0;
        maybeComputeTempo();
    }
}

void BpmTracker::maybeComputeTempo()
{
    if ((int)onsetEnv.size() < (int)(2.5 * envRate)) // need a few seconds first
        return;

    // Copy env to contiguous vector (demean & weight by energy)
    const int N = (int)onsetEnv.size();
    std::vector<float> x((size_t)N);
    double s = 0.0;
    for (int i = 0; i < N; ++i) { x[(size_t)i] = onsetEnv[(size_t)i]; s += x[(size_t)i]; }
    const float mu = (float)(s / (double)N);
    for (int i = 0; i < N; ++i) x[(size_t)i] = juce::jmax(0.0f, x[(size_t)i] - mu);

    // Lags corresponding to BPM range
    const int minLag = bpmToLag(maxBPM);
    const int maxLag = bpmToLag(minBPM);
    const int L = juce::jlimit(2, juce::jmax(2, N - 2), maxLag);

    // Compute ACF over [minLag, L]
    computeAcf(x, minLag, L, acfBuf);

    if (acfBuf.empty()) return;

    // Peak picking: find top K peaks within [minLag..L]
    struct Peak { int lag; float val; };
    std::vector<Peak> peaks;
    peaks.reserve((size_t)topPeaks + 4);

    auto isLocalMax = [&](int i)->bool {
        const float v = acfBuf[(size_t)i];
        return v > acfBuf[(size_t)juce::jmax(0, i - 1)] && v >= acfBuf[(size_t)juce::jmin((int)acfBuf.size() - 1, i + 1)];
        };

    for (int i = 1; i < (int)acfBuf.size() - 1; ++i)
        if (isLocalMax(i))
            peaks.push_back({ i + minLag, acfBuf[(size_t)i] });

    if (peaks.empty()) return;

    std::sort(peaks.begin(), peaks.end(), [](const Peak& a, const Peak& b) { return a.val > b.val; });
    if ((int)peaks.size() > topPeaks) peaks.resize((size_t)topPeaks);

    // Comb-filter scoring with harmonic & subharmonic consideration
    float bestScore = -1.0f;
    int bestLag = peaks.front().lag;
    for (const auto& pk : peaks)
    {
        int L1 = pk.lag;
        float s1 = combScoreAtLag(acfBuf, L1 - minLag);

        // Penalize clear octave errors if not significantly higher
        int L2 = juce::jlimit(minLag, L, L1 * 2);
        int Lh = juce::jlimit(minLag, L, juce::jmax(2, L1 / 2));

        float s2 = combScoreAtLag(acfBuf, L2 - minLag);
        float sh = combScoreAtLag(acfBuf, Lh - minLag);

        float score = s1;
        if (s2 > score && (s2 - score) > 0.1f) score = s2 * 0.95f;
        if (sh > score && (sh - score) > 0.08f) score = sh * 0.92f;

        if (score > bestScore) { bestScore = score; bestLag = L1; }
    }

    float candBpm = juce::jlimit(minBPM, maxBPM, lagToBpm(bestLag));

    // Confidence: peak vs. median ACF (robust)
    std::deque<float> acfCopy;
    acfCopy.resize(acfBuf.size());
    for (size_t i = 0; i < acfBuf.size(); ++i) acfCopy[i] = acfBuf[i];
    const float acfMed = median(acfCopy);
    float conf = 0.0f;
    if (acfMed > 1e-6f) conf = juce::jlimit(0.0f, 1.0f, (bestScore - acfMed) / (bestScore + acfMed + 1e-6f));

    // Debounce via short median
    bpmHistory.push_back(candBpm);
    if ((int)bpmHistory.size() > bpmHistLen) bpmHistory.pop_front();
    const float smoothBpm = median(bpmHistory);

    currentBpm.store(smoothBpm);
    currentConf.store(conf);
}

void BpmTracker::computeAcf(const std::vector<float>& x, int minLag, int maxLag, std::vector<float>& out)
{
    const int N = (int)x.size();
    const int L = juce::jlimit(1, N - 1, maxLag);
    const int l0 = juce::jlimit(1, L - 1, minLag);

    out.assign((size_t)(L - l0 + 1), 0.0f);

    // Simple normalized autocorrelation
    double denom = 0.0;
    for (int i = 0; i < N; ++i) denom += (double)x[(size_t)i] * (double)x[(size_t)i];
    if (denom < 1e-12) return;

    for (int lag = l0; lag <= L; ++lag)
    {
        double s = 0.0;
        const int M = N - lag;
        const float* a = x.data();
        const float* b = x.data() + lag;
        for (int i = 0; i < M; ++i)
            s += (double)a[i] * (double)b[i];
        out[(size_t)(lag - l0)] = (float)(s / denom);
    }
}

float BpmTracker::combScoreAtLag(const std::vector<float>& acf, int idx)
{
    // Combine fundamental + first 3 harmonics with decaying weights
    const int N = (int)acf.size();
    const int i1 = idx;
    const int i2 = (idx >= 0) ? (idx * 2) : -1;
    const int i3 = (idx >= 0) ? (idx * 3) : -1;

    auto valAt = [&](int i)->float { return (i >= 0 && i < N) ? acf[(size_t)i] : 0.0f; };

    const float s =
        1.00f * valAt(i1) +
        0.50f * valAt(i2) +
        0.33f * valAt(i3);

    return s / (1.0f + 0.5f + 0.33f);
}
