#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <cstdint> // for int64_t

#include "AudioEngine.h"
#include "PeakMeter.h"
#include "RingBuffer.h"
#include "LiveAnalyzer.h"
#include "BpmTracker.h"
#include "KeyDetector.h"

namespace CanonkeyTheme
{
    static constexpr float cardCornerRadius = 18.0f;
    static constexpr int   cardGap = 18;
    static constexpr int   outerPad = 16;

    static inline juce::Colour bg() { return juce::Colour(0xfff5f7fb); }
    static inline juce::Colour card() { return juce::Colours::white; }
    static inline juce::Colour title() { return juce::Colour(0xff0b1220); }
    static inline juce::Colour subtitle() { return juce::Colour(0xff4b5563); }
    static inline juce::Colour accent() { return juce::Colour(0xff2f6df6); }
    static inline juce::Colour dropZone() { return juce::Colour(0xffe8eefc); }
    static inline juce::Colour dropZoneActive() { return juce::Colour(0xffd7e4ff); }
}

class MainComponent final : public juce::Component,
    public juce::FileDragAndDropTarget,
    public juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    // OS file Drag & Drop
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragMove(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override; // <-- FIXED SIGNATURE
    void filesDropped(const juce::StringArray& files, int x, int y) override;

private:
    // -------- Live Audio --------
    std::unique_ptr<AudioEngine> audio;
    bool listening = false;

    StereoPeakMeter liveMeter;
    juce::Label     liveFrames{ {}, "0 blocks" };
    std::atomic<uint64_t> liveBlockCounter{ 0 };

    juce::Label liveTitle, liveSubtitle;
    juce::TextButton startListeningButton{ "Start Listening" };
    juce::TextButton audioSourceButton{ "Audio Source" };
    juce::Label      currentSource{ {}, "" };

    // File card UI
    juce::Label fileTitle, fileSubtitle;
    juce::Label dropZone;
    juce::TextButton browseButton{ "Browse…" };
    juce::TextButton cancelButton{ "Cancel" };

    // Result badges
    juce::Label liveResultBpm{ {}, "BPM -" }, liveResultKey{ {}, "Key -" };
    juce::Label fileResultBpm{ {}, "BPM -" }, fileResultKey{ {}, "Key -" };

    // Card bounds
    juce::Rectangle<float> liveCardBounds, fileCardBounds;

    // ---- Live analyzers & FIFO ----
    RingBuffer monoFifo{ 1u << 16 };  // ~65k samples (~1.5 s at 44.1k)
    std::atomic<double> currentSampleRate{ 0.0 };
    std::unique_ptr<LiveAnalyzer> analyzer;

    std::unique_ptr<BpmTracker> bpm;
    std::unique_ptr<KeyDetector> keydet;

    // ---- Offline (file) analysis ----
    class FileAnalyzerThread;
    std::unique_ptr<FileAnalyzerThread> fileWorker;
    std::atomic<bool>  fileAnalyzing{ false };
    std::atomic<float> fileProgress{ 0.0f };
    juce::File         currentFile;

    // Keep chooser alive during async browse on Windows
    std::unique_ptr<juce::FileChooser> fileChooser;

    // Helpers
    void beginFileAnalysis(const juce::File& f);
    void cancelFileAnalysis();
    static juce::String keyIndexToString(int idx, bool isMinor);

    void timerCallback() override;
    void drawCard(juce::Graphics& g, const juce::Rectangle<float>& bounds);

    // DnD visual state
    bool isDragOver = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
