#pragma once
#include <JuceHeader.h>
#include <atomic>
#include "AudioEngine.h"
#include "PeakMeter.h"
#include "RingBuffer.h"   // <-- add

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
}

class MainComponent final : public juce::Component,
    public juce::FileDragAndDropTarget,
    public juce::Timer          // <-- add (tiny debug tap)
{
public:
    MainComponent();
    ~MainComponent() override = default;

    void paint(juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

private:
    std::unique_ptr<AudioEngine> audio;
    bool listening = false;

    StereoPeakMeter liveMeter;
    juce::Label     liveFrames{ {}, "0 blocks" };
    std::atomic<uint64_t> liveBlockCounter{ 0 };

    juce::Label liveTitle, liveSubtitle;
    juce::TextButton startListeningButton{ "Start Listening" };
    juce::TextButton audioSettingsButton{ "Audio Settings…" };

    juce::Label fileTitle, fileSubtitle;
    juce::Label dropZone;
    juce::TextButton browseButton{ "Browse…" };

    juce::TextButton audioSourceButton{ "Audio Source" };
    juce::Label      currentSource{ {}, "" };

    juce::Rectangle<float> liveCardBounds, fileCardBounds;

    juce::Label liveResultBpm{ {}, "BPM -" }, liveResultKey{ {}, "Key -" };
    juce::Label fileResultBpm{ {}, "BPM -" }, fileResultKey{ {}, "Key -" };

    // ---- NEW: mono FIFO for analysis ----
    RingBuffer monoFifo{ 1u << 16 };  // ~65k samples (~1.5 s at 44.1k)

    // tiny debug tap to prove the FIFO has data (prints RMS occasionally)
    void timerCallback() override;

    void drawCard(juce::Graphics& g, const juce::Rectangle<float>& bounds);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
