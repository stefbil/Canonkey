#include <JuceHeader.h>
#include "MainComponent.h"
#include <algorithm>
#include <cmath>
#include <cstdint> // int64_t

#include "BpmTracker.h"
#include "KeyDetector.h"


// Offline File Analyzer
class MainComponent::FileAnalyzerThread : public juce::Thread
{
public:
    FileAnalyzerThread(MainComponent& ownerRef, const juce::File& f)
        : juce::Thread("FileAnalyzer"), owner(ownerRef), file(f) {}

    void run() override
    {
        owner.fileAnalyzing.store(true);
        owner.fileProgress.store(0.0f);

        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
        if (!reader)
        {
            postError("Unsupported or unreadable audio file.");
            owner.fileAnalyzing.store(false);
            return;
        }

        const double sr = reader->sampleRate > 8000.0 ? reader->sampleRate : 44100.0;
        BpmTracker bpm((double)sr);
        KeyDetector kd((double)sr);

        const int64_t total = static_cast<int64_t>(reader->lengthInSamples);
        const int     block = 32768; // ~0.74s @ 44.1k per read
        juce::AudioBuffer<float> buf((int)std::min<int>(reader->numChannels, 2), block);
        std::vector<float> mono((size_t)block, 0.0f);

        int64_t pos = 0;
        while (!threadShouldExit() && pos < total)
        {
            const int toRead = (int)std::min<int64_t>((int64_t)block, total - pos);
            if (!reader->read(&buf, 0, toRead, pos, true, true))
            {
                postError("Read failed during decoding.");
                owner.fileAnalyzing.store(false);
                return;
            }

            const int numCh = buf.getNumChannels();
            const float* L = buf.getReadPointer(0);
            const float* R = (numCh > 1 ? buf.getReadPointer(1) : L);

            for (int i = 0; i < toRead; ++i)
                mono[(size_t)i] = 0.5f * (L[i] + R[i]);

            bpm.processMono(mono.data(), toRead);
            kd.processMono(mono.data(), toRead);

            pos += toRead;
            owner.fileProgress.store((float)((double)pos / (double)total));

            juce::MessageManager::callAsync([this]
                {
                    owner.dropZone.setText("Analyzing: " + owner.currentFile.getFileName()
                        + juce::String::formatted("  (%.0f%%)", owner.fileProgress.load() * 100.0f),
                        juce::dontSendNotification);
                    owner.fileResultBpm.setText("Analyzing…", juce::dontSendNotification);
                    owner.fileResultKey.setText("-", juce::dontSendNotification);
                });
        }

        if (threadShouldExit())
        {
            owner.fileAnalyzing.store(false);
            return;
        }

        const float outBpm = bpm.getBpm();
        const auto  keyRes = kd.getLast();

        juce::MessageManager::callAsync([this, outBpm, keyRes]
            {
                if (threadShouldExit()) return;

                if (outBpm > 0.0f)
                    owner.fileResultBpm.setText(juce::String((int)std::round(outBpm)) + " BPM", juce::dontSendNotification);
                else
                    owner.fileResultBpm.setText("BPM -", juce::dontSendNotification);

                if (keyRes.keyIndex >= 0)
                    owner.fileResultKey.setText(owner.keyIndexToString(keyRes.keyIndex, keyRes.isMinor),
                        juce::dontSendNotification);
                else
                    owner.fileResultKey.setText("Key -", juce::dontSendNotification);

                owner.dropZone.setText("Drop audio file", juce::dontSendNotification);
            });

        owner.fileAnalyzing.store(false);
    }

    void postError(const juce::String& msg)
    {
        juce::MessageManager::callAsync([this, msg]
            {
                owner.dropZone.setText("Error: " + msg, juce::dontSendNotification);
                owner.fileResultBpm.setText("BPM -", juce::dontSendNotification);
                owner.fileResultKey.setText("Key -", juce::dontSendNotification);
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "File Analysis", msg);
            });
    }

private:
    MainComponent& owner;
    juce::File file;
};

//===================================BABABOOOOI=======================================
// Constructor / Destructor
MainComponent::MainComponent()
{
    setOpaque(true);
    setWantsKeyboardFocus(true);
    setSize(980, 600);

    // ----- Audio engine & meter feed -----
    audio = std::make_unique<AudioEngine>();

    audio->onSampleRateChanged = [this](double sr)
        {
            currentSampleRate.store(sr, std::memory_order_relaxed);

            if (analyzer) analyzer->requestReset();

            juce::MessageManager::callAsync([this]
                {
                    if (listening)
                    {
                        liveResultBpm.setText("Listening...", juce::dontSendNotification);
                        liveResultKey.setText("-", juce::dontSendNotification);
                    }
                    else
                    {
                        liveResultBpm.setText("-", juce::dontSendNotification);
                        liveResultKey.setText("-", juce::dontSendNotification);
                    }
                });

            bpm = std::make_unique<BpmTracker>(sr);
            keydet = std::make_unique<KeyDetector>(sr);
        };

    audio->onAudioBlock = [this](const float* const* input, int numCh, int numSamples, double sr)
        {
            currentSampleRate.store(sr, std::memory_order_relaxed);

            if (numCh <= 0 || numSamples <= 0 || input == nullptr) return;

            monoFifo.pushPlanarToMono(input, numCh, numSamples);

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

            static std::vector<float> monoScratch;
            monoScratch.resize((size_t)numSamples);

            const float* L = input[0];
            const float* R = (numCh > 1 ? input[1] : input[0]);
            for (int i = 0; i < numSamples; ++i)
                monoScratch[(size_t)i] = 0.5f * (L[i] + R[i]);

            if (bpm)    bpm->processMono(monoScratch.data(), numSamples);
            if (keydet) keydet->processMono(monoScratch.data(), numSamples);

            auto n = ++liveBlockCounter;
            if ((n % 30) == 0)
                juce::MessageManager::callAsync([this, n]
                    {
                        liveFrames.setText(juce::String(n) + " blocks", juce::dontSendNotification);
                    });
        };

    // UI update timer
    startTimerHz(20);

    // Analyzer thread for legacy live callbacks (optional)
    analyzer = std::make_unique<LiveAnalyzer>(monoFifo, currentSampleRate);
    analyzer->setBpmCallback([this](double bpmVal, double /*conf*/)
        {
            juce::MessageManager::callAsync([this, bpmVal]
                {
                    liveResultBpm.setText(juce::String((int)std::round(bpmVal)) + " BPM", juce::dontSendNotification);
                });
        });
    analyzer->setKeyCallback([this](int keyIndex, bool isMinor, double /*conf*/)
        {
            juce::MessageManager::callAsync([this, keyIndex, isMinor]
                {
                    liveResultKey.setText(keyIndexToString(keyIndex, isMinor), juce::dontSendNotification);
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

                    if (analyzer)
                    {
                        analyzer->requestReset();
                        if (!analyzer->isRunning())
                            analyzer->start();
                    }

                    if (bpm)    bpm->reset();
                    if (keydet) keydet->reset();

                    liveResultBpm.setText("Listening...", juce::dontSendNotification);
                    liveResultKey.setText("-", juce::dontSendNotification);
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

                if (analyzer)
                {
                    analyzer->requestReset();
                    analyzer->stop();
                }

                if (bpm)    bpm->reset();
                if (keydet) keydet->reset();

                liveMeter.setLevels(0.0f, 0.0f);
                liveResultBpm.setText("-", juce::dontSendNotification);
                liveResultKey.setText("-", juce::dontSendNotification);
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

                                if (analyzer)
                                {
                                    analyzer->requestReset();
                                    if (!analyzer->isRunning())
                                        analyzer->start();
                                }

                                if (bpm)    bpm->reset();
                                if (keydet) keydet->reset();

                                liveResultBpm.setText("Listening...", juce::dontSendNotification);
                                liveResultKey.setText("-", juce::dontSendNotification);
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

    // Drop zone label (visual only; the whole MainComponent is a drop target)
    dropZone.setText("Drop audio file", juce::dontSendNotification);
    dropZone.setJustificationType(juce::Justification::centred);
    dropZone.setColour(juce::Label::backgroundColourId, CanonkeyTheme::dropZone().withAlpha(0.6f));
    dropZone.setColour(juce::Label::textColourId, CanonkeyTheme::subtitle());
    dropZone.setFont(juce::Font(14.0f));
    addAndMakeVisible(dropZone);

    // --- Browse (keep chooser alive while dialog is open) ---
    browseButton.onClick = [this]
        {
            if (fileAnalyzing.load())
                cancelFileAnalysis();

            fileChooser = std::make_unique<juce::FileChooser>(
                "Select an audio file",
                juce::File(),
                "*.wav;*.mp3;*.aiff;*.aif;*.flac;*.ogg;*.m4a");

            auto flags = juce::FileBrowserComponent::openMode
                | juce::FileBrowserComponent::canSelectFiles;

            fileChooser->launchAsync(flags,
                [this](const juce::FileChooser& fc)
                {
                    auto f = fc.getResult();
                    fileChooser.reset(); // dialog is closed

                    if (f.existsAsFile())
                        beginFileAnalysis(f);
                });
        };
    addAndMakeVisible(browseButton);

    // Cancel button
    cancelButton.onClick = [this] { cancelFileAnalysis(); };
    cancelButton.setEnabled(false);
    addAndMakeVisible(cancelButton);

    for (auto* l : { &fileResultBpm, &fileResultKey })
    {
        l->setFont(juce::Font(14.0f, juce::Font::bold));
        l->setColour(juce::Label::backgroundColourId, CanonkeyTheme::dropZone());
        l->setColour(juce::Label::textColourId, CanonkeyTheme::title());
        l->setJustificationType(juce::Justification::centred);
        addAndMakeVisible(*l);
    }
}

MainComponent::~MainComponent()
{
    cancelFileAnalysis(); // ensure any worker is stopped before destruction
}

// ============ Layout / Paint ============
void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(CanonkeyTheme::bg());
    drawCard(g, liveCardBounds);
    drawCard(g, fileCardBounds);

    // Optional subtle outline when dragging over window
    if (isDragOver)
    {
        g.setColour(CanonkeyTheme::accent().withAlpha(0.25f));
        g.drawRoundedRectangle(fileCardBounds.reduced(6.0f), CanonkeyTheme::cardCornerRadius, 3.0f);
    }
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

    auto meterArea = live.removeFromTop(120.0f);
    liveMeter.setBounds(meterArea.toNearestInt().reduced(6));

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
    auto leftControls = fileBottom.removeFromLeft(260.0f);
    browseButton.setBounds(leftControls.removeFromLeft(120.0f).toNearestInt().reduced(0, 4));
    cancelButton.setBounds(leftControls.removeFromLeft(120.0f).toNearestInt().reduced(6, 4));

    auto fileBadges = fileBottom.removeFromRight(220.0f);
    fileResultBpm.setBounds(fileBadges.removeFromLeft(100.0f).toNearestInt().reduced(6));
    fileResultKey.setBounds(fileBadges.toNearestInt().reduced(6));

    dropZone.setBounds(file.toNearestInt().reduced(4));
}

// ============ OS File Drag & Drop ============
bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    if (files.isEmpty())
        return true;

    for (auto& f : files)
        if (f.endsWithIgnoreCase(".wav") || f.endsWithIgnoreCase(".mp3") ||
            f.endsWithIgnoreCase(".aiff") || f.endsWithIgnoreCase(".aif") ||
            f.endsWithIgnoreCase(".flac") || f.endsWithIgnoreCase(".ogg") ||
            f.endsWithIgnoreCase(".m4a"))
            return true;

    return false;
}

void MainComponent::fileDragEnter(const juce::StringArray& files, int, int)
{
    if (isInterestedInFileDrag(files))
    {
        isDragOver = true;
        dropZone.setColour(juce::Label::backgroundColourId, CanonkeyTheme::dropZoneActive());
        dropZone.setText("Release to analyze…", juce::dontSendNotification);
        repaint();
    }
}

void MainComponent::fileDragMove(const juce::StringArray& files, int, int)
{
    if (!isInterestedInFileDrag(files))
        fileDragExit(files);
}

void MainComponent::fileDragExit(const juce::StringArray&)
{
    isDragOver = false;
    dropZone.setColour(juce::Label::backgroundColourId, CanonkeyTheme::dropZone().withAlpha(0.6f));
    dropZone.setText("Drop audio file", juce::dontSendNotification);
    repaint();
}

void MainComponent::filesDropped(const juce::StringArray& files, int, int)
{
    isDragOver = false;
    dropZone.setColour(juce::Label::backgroundColourId, CanonkeyTheme::dropZone().withAlpha(0.6f));
    repaint();

    if (files.isEmpty()) return;
    juce::File f(files[0]);
    if (f.existsAsFile())
        beginFileAnalysis(f);
}

// ============ Analysis Control ============
void MainComponent::beginFileAnalysis(const juce::File& f)
{
    cancelFileAnalysis();

    fileAnalyzing.store(false);
    fileProgress.store(0.0f);
    currentFile = f;

    fileResultBpm.setText("Analyzing…", juce::dontSendNotification);
    fileResultKey.setText("-", juce::dontSendNotification);
    dropZone.setText("Analyzing: " + f.getFileName(), juce::dontSendNotification);

    fileWorker = std::make_unique<FileAnalyzerThread>(*this, f);
    fileWorker->startThread();
    cancelButton.setEnabled(true);
}

void MainComponent::cancelFileAnalysis()
{
    if (fileWorker)
    {
        fileWorker->signalThreadShouldExit();
        fileWorker->stopThread(2000);
        fileWorker.reset();
    }
    fileAnalyzing.store(false);
    fileProgress.store(0.0f);

    dropZone.setText("Drop audio file", juce::dontSendNotification);
    fileResultBpm.setText("BPM -", juce::dontSendNotification);
    fileResultKey.setText("Key -", juce::dontSendNotification);
    cancelButton.setEnabled(false);
}

juce::String MainComponent::keyIndexToString(int idx, bool isMinor)
{
    static const char* names[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    if (idx < 0 || idx >= 12) return "Key -";
    return juce::String(names[idx]) + (isMinor ? "m" : " maj");
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
// Debug + UI updater: also refreshes BPM/Key labels
void MainComponent::timerCallback()
{
    if (bpm)
    {
        const float b = bpm->getBpm();
        if (b > 0.0f)
            liveResultBpm.setText(juce::String((int)std::round(b)) + " BPM", juce::dontSendNotification);
        else if (listening)
            liveResultBpm.setText("Listening...", juce::dontSendNotification);
    }

    if (keydet)
    {
        auto r = keydet->getLast();
        if (r.keyIndex >= 0)
            liveResultKey.setText(keyIndexToString(r.keyIndex, r.isMinor), juce::dontSendNotification);
    }

    const bool analyzing = fileAnalyzing.load();
    if (analyzing)
    {
        const float p = fileProgress.load();
        dropZone.setText("Analyzing: " + currentFile.getFileName()
            + juce::String::formatted("  (%.0f%%)", p * 100.0f),
            juce::dontSendNotification);
    }
    cancelButton.setEnabled(analyzing);
}
