#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <thread>
#include <vector>
#include <functional>
#include "RingBuffer.h"
#include "BpmTracker.h"
#include "KeyDetector.h"

//------------------------------------------------------------------------------
class LiveAnalyzer
{
public:
    struct Settings
    {
        // UI cadence and BPM smoothing only (BpmTracker has its own config)
        float updateHz = 2.0f;   // how often to publish to UI
        float bpmSmoothingEMA = 0.70f;  // extra tiny EMA on displayed BPM
    };

    using BpmCallback = std::function<void(double bpm, double confidence)>;
    using KeyCallback = std::function<void(int keyIndex, bool isMinor, double confidence)>;

    LiveAnalyzer(RingBuffer& fifo, std::atomic<double>& sampleRateRef, const Settings& s = {});
    ~LiveAnalyzer();

    void start();
    void stop();
    bool isRunning() const noexcept { return running.load(); }

    // Ask the worker to clear all internal state on the next loop (fresh session)
    void requestReset();

    // UI hooks
    void setBpmCallback(BpmCallback cb) { onBpm = std::move(cb); }
    void setKeyCallback(KeyCallback cb) { onKey = std::move(cb); }

private:
    void threadFunc();

    // config
    Settings            settings;

    // I/O
    RingBuffer& rb;      // mono producer → consumer FIFO (float)  
    std::atomic<double>& srRef;  // live device sample-rate

    // thread
    std::thread         worker;
    std::atomic<bool>   running{ false };
    std::atomic<bool>   resetRequested{ false };

    // analyzers
    std::unique_ptr<BpmTracker> bpm;   // refined tracker  
    std::unique_ptr<KeyDetector> key;  // HPCP+Viterbi    

    // light UI smoothing
    double bpmEMA = 0.0;

    // callbacks
    BpmCallback         onBpm;
    KeyCallback         onKey;
};
