/*
  ==============================================================================

    PeakMeter.h
    Created: 9 Aug 2025 7:55:22pm
    Author:  stefbil

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>

// Simple, thread-safe stereo peak meter.
// Call setLevels() from the audio thread; the component repaints at 60 Hz.
class StereoPeakMeter : public juce::Component, private juce::Timer
{
public:
    StereoPeakMeter()
    {
        setInterceptsMouseClicks (false, false);
        startTimerHz (60); // UI refresh
    }

    // Audio-thread safe
    void setLevels (float left, float right) noexcept
    {
        lTarget.store (juce::jlimit (0.0f, 1.0f, left));
        rTarget.store (juce::jlimit (0.0f, 1.0f, right));
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        auto gap    = 8.0f;
        auto w      = (bounds.getWidth() - gap) * 0.5f;
        auto leftR  = bounds.removeFromLeft (w);
        bounds.removeFromLeft (gap);
        auto rightR = bounds;

        drawBar (g, leftR,  lNow);
        drawBar (g, rightR, rNow);
    }

private:
    std::atomic<float> lTarget { 0.0f }, rTarget { 0.0f };
    float lNow = 0.0f, rNow = 0.0f;

    void timerCallback() override
    {
        // fast attack, slow release
        smoothTo (lNow, lTarget.load());
        smoothTo (rNow, rTarget.load());
        repaint();
    }

    static void smoothTo (float& now, float target) noexcept
    {
        if (target > now) now = target;
        else              now *= 0.92f;
        if (now < 0.001f) now = 0.0f;
    }

    void drawBar (juce::Graphics& g, juce::Rectangle<float> r, float level)
    {
        g.setColour (juce::Colour (0xffe8eefc));
        g.fillRoundedRectangle (r, 6.0f);

        const float v = juce::jlimit (0.0f, 1.0f, level);
        auto filled = r.withY (r.getBottom() - r.getHeight() * v)
                      .withHeight (r.getHeight() * v);

        g.setColour (juce::Colour (0xff2f6df6));
        g.fillRoundedRectangle (filled, 6.0f);

        g.setColour (juce::Colour (0x14000000));
        g.drawRoundedRectangle (r, 6.0f, 1.0f);
    }
};