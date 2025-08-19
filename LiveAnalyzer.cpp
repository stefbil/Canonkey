#include "LiveAnalyzer.h"
#include <cmath>
#include <numeric>

LiveAnalyzer::LiveAnalyzer(RingBuffer& fifo,
    std::atomic<double>& sampleRateRef,
    const Settings& s)
    : settings(s),
    rb(fifo),
    srRef(sampleRateRef)
{
    // Defer constructing analyzers until we know a valid sample rate in start()
}

LiveAnalyzer::~LiveAnalyzer()
{
    stop();
}

void LiveAnalyzer::start()
{
    if (running.exchange(true)) return;

    worker = std::thread([this]
        {
            juce::Thread::setCurrentThreadName("LiveAnalyzer");

            // Wait for a sane sample rate
            double sr = 0.0;
            for (int tries = 0; tries < 200 && sr < 8000.0; ++tries)
            {
                sr = srRef.load(std::memory_order_relaxed);
                if (sr >= 8000.0) break;
                juce::Thread::sleep(5);
            }
            if (sr < 8000.0) sr = 44100.0; // fallback

            // Construct analyzers at current SR
            bpm = std::make_unique<BpmTracker>(sr);   // uses its own internal settings  
            key = std::make_unique<KeyDetector>(sr);  // default KeyDetector::Settings  

            // Key publishing directly via callback to preserve real-time feel
            key->setCallback([this](const KeyDetector::Result& r)
                {
                    if (onKey && r.keyIndex >= 0)
                        onKey(r.keyIndex, r.isMinor, r.confidence);
                });

            // Work buffers
            constexpr int chunk = 1024;           // pop in ~20–23ms sips @ 44.1/48k
            std::vector<float> mono((size_t)chunk);

            const double periodMs = 1000.0 / std::max(0.5f, settings.updateHz);
            double nextUi = juce::Time::getMillisecondCounterHiRes();

            while (running.load())
            {
                // React to SR changes
                const double nowSr = srRef.load(std::memory_order_relaxed);
                if (nowSr >= 8000.0 && std::abs(nowSr - sr) > 1.0)
                {
                    sr = nowSr;
                    if (bpm) bpm->reset(sr);   // rebuild internals for new SR  
                    if (key) key->reset(sr);   // retune chroma bins / EMAs     
                    bpmEMA = 0.0;
                }

                // Honor reset requests (Stop Listening)
                if (resetRequested.exchange(false))
                {
                    if (bpm) bpm->reset(sr);
                    if (key) key->reset(sr);
                    bpmEMA = 0.0;
                }

                // Consume audio
                const size_t got = rb.pop(mono.data(), (size_t)chunk); // lock-free pop  
                if (got == 0)
                {
                    juce::Thread::sleep(2);
                }
                else
                {
                    // Feed analyzers (mono)
                    if (bpm) bpm->processMono(mono.data(), (int)got);   // RT-safe API  
                    if (key) key->processMono(mono.data(), (int)got);   // RT-safe API  
                }

                // Publish at UI cadence
                const double t = juce::Time::getMillisecondCounterHiRes();
                if (t >= nextUi)
                {
                    if (onBpm && bpm)
                    {
                        const float raw = bpm->getBpm();      // stable/locked output  
                        const float conf = bpm->getConfidence();

                        if (raw > 0.0f)
                        {
                            if (bpmEMA <= 0.0) bpmEMA = raw;
                            bpmEMA = settings.bpmSmoothingEMA * bpmEMA
                                + (1.0 - settings.bpmSmoothingEMA) * (double)raw;

                            onBpm(bpmEMA, (double)conf);
                        }
                    }
                    nextUi = t + periodMs;
                }
            }
        });
}

void LiveAnalyzer::stop()
{
    if (!running.exchange(false)) return;
    if (worker.joinable()) worker.join();

    // Drop analyzers to free FFT memory
    bpm.reset();
    key.reset();
    bpmEMA = 0.0;
}

void LiveAnalyzer::requestReset()
{
    resetRequested.store(true, std::memory_order_relaxed);
}
