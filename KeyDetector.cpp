#include "KeyDetector.h"
#include <cmath>
#include <numeric>
#include <algorithm>

namespace {
    // Krumhansl–Schmuckler (C maj/min), normalized later.
    static const float KS_MAJOR[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
    static const float KS_MINOR[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };

    inline float cosineSim(const float* a, const float* b, int n) {
        double ab = 0, aa = 0, bb = 0;
        for (int i = 0; i < n; ++i) { ab += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i]; }
        if (aa <= 1e-12 || bb <= 1e-12) return 0.0f;
        return (float)(ab / std::sqrt(aa * bb));
    }
    inline float cosineKernel(float semitoneDelta, float width) {
        const float x = std::abs(semitoneDelta) / std::max(1e-6f, width);
        if (x >= 1.0f) return 0.0f;
        return 0.5f * (1.0f + std::cos(juce::MathConstants<float>::pi * x));
    }
}

KeyDetector::KeyDetector(double sampleRate, const Settings& s)
    : cfg(s),
    sr(sampleRate > 0.0 ? sampleRate : 44100.0),
    fftOrder(cfg.fftOrder),
    fftSize(1 << cfg.fftOrder),
    hop(cfg.hop),
    fft(cfg.fftOrder)
{
    // normalize KS
    float sumMaj = std::accumulate(std::begin(KS_MAJOR), std::end(KS_MAJOR), 0.0f);
    float sumMin = std::accumulate(std::begin(KS_MINOR), std::end(KS_MINOR), 0.0f);
    for (int i = 0; i < 12; ++i) { profMaj[i] = KS_MAJOR[i] / sumMaj; profMin[i] = KS_MINOR[i] / sumMin; }

    window.resize(fftSize);
    for (int i = 0; i < fftSize; ++i) window[(size_t)i] = hann(i, fftSize);

    ensureBuffers();
    reset(sr);
}

void KeyDetector::reset(double newSampleRate) {
    if (newSampleRate > 0.0 && std::abs(newSampleRate - sr) > 1e-6) sr = newSampleRate;

    overlap.assign(fftSize, 0.0f);
    overlapFill = 0;
    chromaEMA.fill(0.0f);
    tuningCentsEMA = 0.0f;
    instScore.fill(0.0f);
    viterbi.fill(0.0f);
    vitInit = false;
    vitCurrent = -1;
    pendingKey = -1;
    pendingSinceMs = 0.0;
    lastPublishMs = 0.0;
    lastResult.store(Result{ -1, false, 0.0f });
}

void KeyDetector::ensureBuffers() {
    fftBuf.assign((size_t)fftSize * 2, 0.0f);
    magRaw.assign((size_t)fftSize / 2 + 1, 0.0f);
    mag.assign((size_t)fftSize / 2 + 1, 0.0f);
}

void KeyDetector::processMono(const float* samples, int numSamples) {
    if (!samples || numSamples <= 0) return;
    int idx = 0;
    while (idx < numSamples) {
        const int need = fftSize - overlapFill;
        const int take = std::min(need, numSamples - idx);
        std::memcpy(overlap.data() + overlapFill, samples + idx, (size_t)take * sizeof(float));
        overlapFill += take; idx += take;
        if (overlapFill == fftSize) {
            analyzeFrame();
            // slide
            const int remain = fftSize - hop;
            std::memmove(overlap.data(), overlap.data() + hop, (size_t)remain * sizeof(float));
            overlapFill = remain;
        }
    }
}

void KeyDetector::analyzeFrame() {
    for (int i = 0; i < fftSize; ++i) {
        fftBuf[(size_t)(2 * i)] = overlap[(size_t)i] * window[(size_t)i];
        fftBuf[(size_t)(2 * i + 1)] = 0.0f;
    }
    fft.performRealOnlyForwardTransform(fftBuf.data());

    const int bins = fftSize / 2;
    double peakMag = 0.0;
    for (int k = 0; k <= bins; ++k) {
        const float re = fftBuf[(size_t)(2 * k)];
        const float im = fftBuf[(size_t)(2 * k + 1)];
        const float m = std::sqrt(re * re + im * im);
        magRaw[(size_t)k] = m;
        if (m > peakMag) peakMag = m;
    }
    const float g = std::max(1e-12f, (float)peakMag);
    for (int k = 0; k <= bins; ++k) {
        const float m = magRaw[(size_t)k] / g;
        mag[(size_t)k] = std::pow(m, cfg.gamma);
    }

    computePeaksAndHpcp();
    score24();
    viterbiStep();
    maybePublish();
}

void KeyDetector::computePeaksAndHpcp() {
    std::fill(frameChroma.begin(), frameChroma.end(), 0.0f);
    const int bins = fftSize / 2;
    const double binHz = sr / (double)fftSize;

    const float thr = std::pow(10.0f, cfg.peakRelThreshDb / 20.0f);
    struct Peak { int bin; float m; double hz; };
    std::vector<Peak> peaks; peaks.reserve((size_t)cfg.maxPeaks);

    for (int k = 2; k < bins - 2; ++k) {
        const float m0 = mag[(size_t)k];
        if (m0 < thr) continue;
        if (m0 > mag[(size_t)k - 1] && m0 > mag[(size_t)k + 1] && m0 > mag[(size_t)k - 2] && m0 > mag[(size_t)k + 2]) {
            const float m1 = mag[(size_t)k - 1], m2 = mag[(size_t)k + 1];
            const float denom = std::max(1e-12f, 2.0f * (m1 - 2.0f * m0 + m2));
            const float delta = juce::jlimit(-0.5f, 0.5f, (m1 - m2) / denom);
            const double hz = (k + delta) * binHz;
            peaks.push_back({ k, m0, hz });
            if ((int)peaks.size() >= cfg.maxPeaks) break;
        }
    }
    // tuning estimate
    if (!peaks.empty()) {
        double sumC = 0.0; int n = 0;
        for (auto& p : peaks) {
            const double midi = 69.0 + 12.0 * std::log2(p.hz / cfg.refA4);
            const double cents = (midi - std::round(midi)) * 100.0;
            if (std::isfinite(cents)) { sumC += cents; ++n; }
        }
        if (n > 0) {
            const double hopSec = (double)hop / sr;
            const double a = std::clamp(hopSec / cfg.tuningDecaySec, 0.01, 0.2);
            tuningCentsEMA = (float)((1.0 - a) * tuningCentsEMA + a * (sumC / (double)n));
        }
    }
    // peaks -> HPCP (C-based)
    auto hzToPc = [&](double hz) {
        double midi = 69.0 + 12.0 * std::log2(hz / cfg.refA4) + tuningCentsEMA / 100.0;
        return std::fmod(midi - 69.0 + 9.0 + 1200.0, 12.0); // C=0
        };
    for (auto& p : peaks) {
        if (p.hz < cfg.minHz || p.hz > cfg.maxHz) continue;
        const double pc = hzToPc(p.hz);
        const int center = (int)std::floor(pc + 0.5);
        for (int off = -1; off <= +1; ++off) {
            const int idx = wrap12(center + off);
            const float d = (float)((pc - center) - off);
            const float w = cosineKernel(d, cfg.kernelWidth);
            if (w > 0.0f) frameChroma[(size_t)idx] += p.m * w;
        }
    }
    // L2 norm + EMA
    double s2 = 0.0; for (float v : frameChroma) s2 += (double)v * v;
    if (s2 > 1e-12) for (auto& v : frameChroma) v = (float)(v / std::sqrt(s2));
    const double hopSec = (double)hop / sr;
    const double a = std::clamp(hopSec / cfg.chromaDecaySec, 0.02, 0.25);
    for (int i = 0; i < 12; ++i) chromaEMA[i] = (float)((1.0 - a) * chromaEMA[i] + a * frameChroma[i]);
}

void KeyDetector::score24() {
    float rot[12];
    auto scoreMode = [&](const float* tmpl, int base) {
        for (int key = 0; key < 12; ++key) {
            for (int i = 0; i < 12; ++i) rot[i] = chromaEMA[(size_t)wrap12(i - key)];
            instScore[(size_t)(base + key)] = cosineSim(rot, tmpl, 12);
        }
        };
    scoreMode(profMaj.data(), 0);   // 0..11
    scoreMode(profMin.data(), 12);  // 12..23
}

void KeyDetector::viterbiStep() {
    // Transition model: prefer stay, allow neighbors/relatives with small bonus.
    std::array<float, 24> next{};
    int best = -1; float bestVal = -1e9f;

    auto related = [&](int a, int b)->bool {
        const bool minA = a >= 12, minB = b >= 12;
        const int  pcA = a % 12, pcB = b % 12;
        if (a == b) return true;
        if (!minA && minB && pcB == (pcA + 9) % 12) return true; // rel minor
        if (minA && !minB && pcB == (pcA + 3) % 12) return true; // rel major
        if (minA == minB && (pcB == (pcA + 7) % 12 || pcB == (pcA + 5) % 12)) return true; // V, IV
        return false;
        };

    if (!vitInit) {
        // Cold-start with current inst scores
        for (int k = 0; k < 24; ++k) viterbi[(size_t)k] = instScore[(size_t)k];
        vitInit = true;
    }
    else {
        for (int k = 0; k < 24; ++k) {
            float bestPrev = -1e9f;
            for (int p = 0; p < 24; ++p) {
                float t = 0.0f;
                if (k == p) t += cfg.stayBias;
                else if (related(p, k)) t += cfg.neighborBonus;
                else t -= cfg.transitionPenalty;
                const float cand = viterbi[(size_t)p] + t;
                if (cand > bestPrev) bestPrev = cand;
            }
            next[(size_t)k] = bestPrev + instScore[(size_t)k];
        }
        viterbi = next;
    }
    for (int k = 0; k < 24; ++k) if (viterbi[(size_t)k] > bestVal) { bestVal = viterbi[(size_t)k]; best = k; }

    vitCurrent = best;
}

void KeyDetector::maybePublish() {
    // Find runner-up & margin on *instant* scores with bias (so UI reflects present evidence)
    int best = -1, second = -1; float sBest = -1e9f, sSecond = -1e9f;
    for (int k = 0; k < 24; ++k) {
        float s = instScore[(size_t)k];
        if (k == vitCurrent) s += cfg.stayBias; // tiny coherence with Viterbi state
        if (s > sBest) { sSecond = sBest; second = best; sBest = s; best = k; }
        else if (s > sSecond) { sSecond = s; second = k; }
    }
    const float margin = (sSecond <= 1e-6f) ? 1.0f : (sBest - sSecond);

    const double now = nowMs();
    if (pendingKey != best) { pendingKey = best; pendingSinceMs = now; }

    const bool dwellOK = (now - pendingSinceMs) >= cfg.dwellRequiredSec * 1000.0;
    const bool marginOK = (margin >= cfg.marginRequired);
    const bool rateOK = (now - lastPublishMs) >= cfg.publishMinIntervalSec * 1000.0;

    if (dwellOK && marginOK && rateOK) {
        Result r;
        r.keyIndex = best % 12;
        r.isMinor = best >= 12;
        r.confidence = clamp01(margin);
        lastResult.store(r);
        lastPublishMs = now;
        if (onResult) onResult(r);
    }
}
