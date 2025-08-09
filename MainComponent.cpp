#include "MainComponent.h"
#include <algorithm>
#include <cmath>

//==============================================================================
// Constructor sets up the UI and audio plumbing.
MainComponent::MainComponent()
{
    setOpaque(true);
    setSize(980, 600);

    // ----- Audio engine & meter feed -----
    audio = std::make_unique<AudioEngine>();

    audio->onAudioBlock = [this](const float* const* input, int numCh, int numSamples, double sr)
        {
            // keep sample rate fresh for analyzer
            currentSampleRate.store(sr, std::memory_order_relaxed);

            if (numCh <= 0 || numSamples <= 0 || input == nullptr) return;

            // Feed analyzer FIFO (mono mix) — lock-free
            monoFifo.pushPlanarToMono(input, numCh, numSamples);

            // Visual peak meter (fast + light)
            float l = 0.0f, r = 0.0f;

            if (numCh == 1)
            {
                const float* ch = input[0];
                for (int i = 0; i < numSamples; ++i)
                    l = juce::jmax(l, std::abs(ch[i]));
                r = l;
            }
            else
            {
                const float* ch0 = input[0];
                const float* ch1 = input[1];
                for (int i = 0; i < numSamples; ++i)
                {
                    l = juce::jmax(l, std::abs(ch0[i]));
                    r = juce::jmax(r, std::abs(ch1[i]));
                }
            }

            l = juce::jmin(l, 1.0f);
            r = juce::jmin(r, 1.0f);
            liveMeter.setLevels(l, r);

            auto n = ++liveBlockCounter;
            if ((n % 30) == 0)
                juce::MessageManager::callAsync([this, n]
                    {
                        liveFrames.setText(juce::String(n) + " blocks", juce::dontSendNotification);
                    });
        };

    // Small debug timer to "sip" the FIFO and log RMS (once/sec)
    startTimerHz(20);

    // Analyzer (creates thread but starts when audio starts)
    analyzer = std::make_unique<LiveAnalyzer>(monoFifo, currentSampleRate);
    analyzer->setBpmCallback([this](double bpm, double /*conf*/)
        {
            juce::MessageManager::callAsync([this, bpm]
                {
                    liveResultBpm.setText(juce::String((int)std::round(bpm)) + " BPM", juce::dontSendNotification);
                });
        });

    // ----- Live card -----
    liveTitle.setText("Live Analysis", juce::dontSendNotification);
    liveTitle.setFont(juce::Font(20.0f, juce::Font::bold));
    liveTitle.setColour(juce::Label::textColourId, CanonkeyTheme::title());
    addAndMakeVisible(liveTitle);

    liveSubtitle.setText("Get the key and BPM of any song", juce::dontSendNotification);
    liveSubtitle.setFont(juce::Font(14.0f));
    liveSubtitle.setColour(juce::Label::textColourId, CanonkeyTheme::subtitle());
    addAndMakeVisible(liveSubtitle);

    addAndMakeVisible(liveMeter);

    liveFrames.setColour(juce::Label::textColourId, CanonkeyTheme::subtitle());
    liveFrames.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(liveFrames);

    startListeningButton.setColour(juce::TextButton::buttonColourId, CanonkeyTheme::accent());
    startListeningButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    startListeningButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    startListeningButton.onClick = [this]
        {
            if (!listening)
            {
                juce::String err;
                if (audio->startBestInputForLive(err))
                {
                    listening = true;
                    startListeningButton.setButtonText("Stop Listening");
                    liveBlockCounter.store(0);
                    liveFrames.setText("0 blocks", juce::dontSendNotification);
                    currentSource.setText(audio->getCurrentDeviceInfo().inputName, juce::dontSendNotification);

                    if (analyzer && !analyzer->isRunning())
                        analyzer->start();
                }
                else
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Audio error",
                        err.isEmpty() ? "Failed to start audio." : err);
                }
            }
            else
            {
                audio->stop();
                listening = false;
                startListeningButton.setButtonText("Start Listening");
                liveMeter.setLevels(0.0f, 0.0f); // ensure smooth decay to zero

                // Optional: stop analyzer when stopping audio
                // if (analyzer) analyzer->stop();
            }
        };
    addAndMakeVisible(startListeningButton);

    // ---- Audio Source ----
    audioSourceButton.onClick = [this]
        {
            auto devices = audio->enumerateDevices();

            juce::PopupMenu root, outputsMenu, inputsMenu;

            int itemId = 1;
            struct Item { int id; AudioEngine::DeviceEntry entry; };
            juce::Array<Item> idMap;

            for (auto& d : devices)
            {
                juce::String label = d.name;
                if (d.type.containsIgnoreCase("ASIO"))
                    label = "[ASIO] " + label;

                if (d.isLoopback) outputsMenu.addItem(itemId, label);
                else              inputsMenu.addItem(itemId, label);

                idMap.add({ itemId, d });
                ++itemId;
            }

            if (outputsMenu.getNumItems() == 0)
                outputsMenu.addItem(9001, "(no loopback devices)", false, false);
            if (inputsMenu.getNumItems() == 0)
                inputsMenu.addItem(9002, "(no inputs)", false, false);

            root.addSubMenu("Outputs (Loopback)", outputsMenu);
            root.addSubMenu("Inputs", inputsMenu);

            root.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(audioSourceButton),
                [this, idMap](int chosenId)
                {
                    if (chosenId <= 0) return;

                    for (auto& it : idMap)
                    {
                        if (it.id == chosenId)
                        {
                            juce::String err;
                            if (audio->startWithDevice(it.entry, err))
                            {
                                listening = true;
                                startListeningButton.setButtonText("Stop Listening");
                                currentSource.setText(it.entry.name, juce::dontSendNotification);
                                liveBlockCounter.store(0);
                                liveFrames.setText("0 blocks", juce::dontSendNotification);

                                if (analyzer && !analyzer->isRunning())
                                    analyzer->start();
                            }
                            else
                            {
                                juce::AlertWindow::showMessageBoxAsync(
                                    juce::AlertWindow::WarningIcon,
                                    "Audio error",
                                    err.isEmpty() ? "Failed to start device." : err);
                            }
                            break;
                        }
                    }
                });
        };
    addAndMakeVisible(audioSourceButton);

    currentSource.setColour(juce::Label::textColourId, CanonkeyTheme::subtitle());
    currentSource.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(currentSource);

    // Live results placeholders
    for (auto* l : { &liveResultBpm, &liveResultKey })
    {
        l->setFont(juce::Font(14.0f, juce::Font::bold));
        l->setColour(juce::Label::backgroundColourId, CanonkeyTheme::dropZone());
        l->setColour(juce::Label::textColourId, CanonkeyTheme::title());
        l->setJustificationType(juce::Justification::centred);
        addAndMakeVisible(*l);
    }

    // ----- File card -----
    fileTitle.setText("File Analysis", juce::dontSendNotification);
    fileTitle.setFont(juce::Font(20.0f, juce::Font::bold));
    fileTitle.setColour(juce::Label::textColourId, CanonkeyTheme::title());
    addAndMakeVisible(fileTitle);

    fileSubtitle.setText("Drag & drop your file here", juce::dontSendNotification);
    fileSubtitle.setFont(juce::Font(14.0f));
    fileSubtitle.setColour(juce::Label::textColourId, CanonkeyTheme::subtitle());
    addAndMakeVisible(fileSubtitle);

    dropZone.setInterceptsMouseClicks(false, false);
    dropZone.setText("Drop audio file", juce::dontSendNotification);
    dropZone.setJustificationType(juce::Justification::centred);
    dropZone.setColour(juce::Label::backgroundColourId, CanonkeyTheme::dropZone().withAlpha(0.6f));
    dropZone.setColour(juce::Label::textColourId, CanonkeyTheme::subtitle());
    dropZone.setFont(juce::Font(14.0f));
    addAndMakeVisible(dropZone);

    browseButton.onClick = [this]
        {
            juce::FileChooser chooser("Select an audio file",
                juce::File(),
                "*.wav;*.mp3;*.aiff;*.aif;*.flac;*.ogg;*.m4a");

            chooser.launchAsync(juce::FileBrowserComponent::openMode
                | juce::FileBrowserComponent::canSelectFiles,
                [this](const juce::FileChooser& fc)
                {
                    auto f = fc.getResult();
                    if (f.existsAsFile())
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::InfoIcon,
                            "Stub",
                            "Selected file:\n" + f.getFullPathName());
                });
        };
    addAndMakeVisible(browseButton);

    for (auto* l : { &fileResultBpm, &fileResultKey })
    {
        l->setFont(juce::Font(14.0f, juce::Font::bold));
        l->setColour(juce::Label::backgroundColourId, CanonkeyTheme::dropZone());
        l->setColour(juce::Label::textColourId, CanonkeyTheme::title());
        l->setJustificationType(juce::Justification::centred);
        addAndMakeVisible(*l);
    }
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(CanonkeyTheme::bg());
    drawCard(g, liveCardBounds);
    drawCard(g, fileCardBounds);
}

void MainComponent::resized()
{
    auto area = getLocalBounds().toFloat().reduced((float)CanonkeyTheme::outerPad);

    const auto halfHeight = (area.getHeight() - CanonkeyTheme::cardGap) * 0.5f;
    liveCardBounds = { area.getX(), area.getY(), area.getWidth(), halfHeight };
    fileCardBounds = { area.getX(), area.getY() + halfHeight + CanonkeyTheme::cardGap,
                       area.getWidth(), halfHeight };

    // ----- Live card layout -----
    auto live = liveCardBounds.reduced(20.0f);
    auto liveTop = live.removeFromTop(36.0f);
    liveTitle.setBounds(liveTop.removeFromLeft(220.0f).toNearestInt());
    liveSubtitle.setBounds(liveTop.toNearestInt());

    // Big visible meter strip
    auto meterArea = live.removeFromTop(120.0f);
    liveMeter.setBounds(meterArea.toNearestInt().reduced(6));

    // Counter at top-right of the meter
    auto framesBox = meterArea.removeFromTop(20.0f).removeFromRight(140.0f);
    liveFrames.setBounds(framesBox.toNearestInt());

    auto liveBottom = live.removeFromBottom(44.0f);
    startListeningButton.setBounds(liveBottom.removeFromLeft(160.0f).toNearestInt().reduced(0, 4));
    audioSourceButton.setBounds(liveBottom.removeFromLeft(140.0f).toNearestInt().reduced(6, 4));
    currentSource.setBounds(liveBottom.removeFromLeft(360.0f).toNearestInt());

    auto liveBadges = liveBottom.removeFromRight(220.0f);
    liveResultBpm.setBounds(liveBadges.removeFromLeft(100.0f).toNearestInt().reduced(6));
    liveResultKey.setBounds(liveBadges.toNearestInt().reduced(6));

    // ----- File card layout -----
    auto file = fileCardBounds.reduced(20.0f);
    auto fileTop = file.removeFromTop(36.0f);
    fileTitle.setBounds(fileTop.removeFromLeft(220.0f).toNearestInt());
    fileSubtitle.setBounds(fileTop.toNearestInt());

    auto fileBottom = file.removeFromBottom(44.0f);
    browseButton.setBounds(fileBottom.removeFromLeft(120.0f).toNearestInt().reduced(0, 4));
    auto fileBadges = fileBottom.removeFromRight(220.0f);
    fileResultBpm.setBounds(fileBadges.removeFromLeft(100.0f).toNearestInt().reduced(6));
    fileResultKey.setBounds(fileBadges.toNearestInt().reduced(6));

    dropZone.setBounds(file.toNearestInt().reduced(4));
}

bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (auto& f : files)
        if (f.endsWithIgnoreCase(".wav") || f.endsWithIgnoreCase(".mp3") ||
            f.endsWithIgnoreCase(".aiff") || f.endsWithIgnoreCase(".aif") ||
            f.endsWithIgnoreCase(".flac") || f.endsWithIgnoreCase(".ogg") ||
            f.endsWithIgnoreCase(".m4a"))
            return true;

    return false;
}

void MainComponent::filesDropped(const juce::StringArray& files, int, int)
{
    if (files.isEmpty()) return;

    juce::AlertWindow::showMessageBoxAsync(
        juce::AlertWindow::InfoIcon,
        "Stub",
        "Dropped file:\n" + files[0]);
}

void MainComponent::drawCard(juce::Graphics& g, const juce::Rectangle<float>& bounds)
{
    juce::Path p;
    p.addRoundedRectangle(bounds, CanonkeyTheme::cardCornerRadius);

    g.setColour(CanonkeyTheme::card());
    g.fillPath(p);

    g.setColour(juce::Colour(0x14000000));
    g.strokePath(p, juce::PathStrokeType(1.0f));
}

//------------------------------------------------------------------------------
// Debug sip from the FIFO: prints ~once/sec so you can verify buffered audio.
void MainComponent::timerCallback()
{
    static int tick = 0;
    float scratch[1024];
    const size_t got = monoFifo.pop(scratch, (size_t)std::size(scratch));
    if (got > 0 && (++tick % 20) == 0) // 20 Hz timer → ~1 s
    {
        double acc = 0.0;
        for (size_t i = 0; i < got; ++i) acc += (double)scratch[i] * (double)scratch[i];
        const double rms = std::sqrt(acc / (double)got);
        const double db = (rms > 0.0) ? 20.0 * std::log10(rms + 1e-12) : -120.0;
        DBG("FIFO pop: " << (int)got << " samples, RMS ~ " << juce::String(db, 1) << " dB");
    }
}
