#include "AudioEngine.h"

static bool isWASAPITypeName(const juce::String& s)
{
    return s.containsIgnoreCase("Windows Audio") || s.containsIgnoreCase("WASAPI");
}

AudioEngine::AudioEngine()
{
    deviceManager.initialise(0, 0, nullptr, true);
}

AudioEngine::~AudioEngine()
{
    stop();
}

bool AudioEngine::setDeviceType(const juce::String& typeName)
{
    deviceManager.setCurrentAudioDeviceType(typeName, true);
    return deviceManager.getCurrentAudioDeviceType() == typeName;
}

juce::String AudioEngine::findWASAPILoopbackName()
{
    juce::OwnedArray<juce::AudioIODeviceType> types;
    deviceManager.createAudioDeviceTypes(types);

    for (auto* t : types)
    {
        if (!isWASAPITypeName(t->getTypeName()))
            continue;

        t->scanForDevices();
        auto inputs = t->getDeviceNames(true);

        for (auto& name : inputs)
            if (name.containsIgnoreCase("loopback"))
                return name;
    }
    return {};
}

juce::String AudioEngine::findDefaultInputForType(const juce::String& typeName)
{
    juce::OwnedArray<juce::AudioIODeviceType> types;
    deviceManager.createAudioDeviceTypes(types);

    for (auto* t : types)
    {
        if (t->getTypeName() != typeName)
            continue;

        t->scanForDevices();
        auto inputs = t->getDeviceNames(true);
        if (inputs.isEmpty())
            return {};

        for (auto& n : inputs)
            if (!n.containsIgnoreCase("loopback"))
                return n;

        return inputs[0];
    }
    return {};
}

bool AudioEngine::startWASAPILoopback(juce::String& error)
{
    error.clear();

    juce::OwnedArray<juce::AudioIODeviceType> types;
    deviceManager.createAudioDeviceTypes(types);

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager.getAudioDeviceSetup(setup);

    for (auto* t : types)
    {
        if (!isWASAPITypeName(t->getTypeName()))
            continue;

        if (!setDeviceType(t->getTypeName()))
            continue;

        auto loopbackName = findWASAPILoopbackName();
        if (loopbackName.isEmpty())
        {
            error = "No WASAPI loopback device found.";
            return false;
        }

        setup.inputDeviceName = loopbackName;
        setup.outputDeviceName.clear();
        setup.useDefaultInputChannels = true;
        setup.useDefaultOutputChannels = false;
        setup.sampleRate = 44100.0;
        setup.bufferSize = 512;

        auto err = deviceManager.setAudioDeviceSetup(setup, true);
        if (err.isNotEmpty()) { error = err; return false; }

        deviceManager.addAudioCallback(this);
        running.store(true);
        return true;
    }

    error = "WASAPI device type not available.";
    return false;
}

bool AudioEngine::startASIOInputDefault(juce::String& error)
{
    error.clear();

    juce::OwnedArray<juce::AudioIODeviceType> types;
    deviceManager.createAudioDeviceTypes(types);

    for (auto* t : types)
    {
        if (!t->getTypeName().containsIgnoreCase("ASIO"))
            continue;

        if (!setDeviceType(t->getTypeName()))
            continue;

        auto inName = findDefaultInputForType(t->getTypeName());
        if (inName.isEmpty())
        {
            error = "No ASIO input device found.";
            return false;
        }

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager.getAudioDeviceSetup(setup);
        setup.inputDeviceName = inName;
        setup.outputDeviceName.clear();
        setup.useDefaultInputChannels = true;
        setup.useDefaultOutputChannels = false;

        auto err = deviceManager.setAudioDeviceSetup(setup, true);
        if (err.isNotEmpty()) { error = err; return false; }

        deviceManager.addAudioCallback(this);
        running.store(true);
        return true;
    }

    error = "ASIO device type not available.";
    return false;
}

bool AudioEngine::startWASAPIInputDefault(juce::String& error)
{
    error.clear();

    juce::OwnedArray<juce::AudioIODeviceType> types;
    deviceManager.createAudioDeviceTypes(types);

    for (auto* t : types)
    {
        if (!isWASAPITypeName(t->getTypeName()))
            continue;

        if (!setDeviceType(t->getTypeName()))
            continue;

        auto inName = findDefaultInputForType(t->getTypeName());
        if (inName.isEmpty())
        {
            error = "No WASAPI input devices available.";
            return false;
        }

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager.getAudioDeviceSetup(setup);
        setup.inputDeviceName = inName;
        setup.outputDeviceName.clear();
        setup.useDefaultInputChannels = true;
        setup.useDefaultOutputChannels = false;

        auto err = deviceManager.setAudioDeviceSetup(setup, true);
        if (err.isNotEmpty()) { error = err; return false; }

        deviceManager.addAudioCallback(this);
        running.store(true);
        return true;
    }

    error = "WASAPI device type not available.";
    return false;
}

bool AudioEngine::startBestInputForLive(juce::String& error)
{
    // 1) Try JUCE WASAPI loopback (if Windows exposes it)
    if (startWASAPILoopback(error))  return true;

#if JUCE_WINDOWS
    // 2) Fallback: native WASAPI loopback of default render endpoint
    wasapiLoopback = std::make_unique<WasapiLoopback>();
    bool ok = wasapiLoopback->start(
        [this](const float* const* input, int numCh, int numSamples, double sr)
        {
            if (onAudioBlock) onAudioBlock(input, numCh, numSamples, sr);
        }, error);
    if (ok) { running.store(true); return true; }
    wasapiLoopback.reset();
#endif

    // 3) Try hardware inputs
    if (startASIOInputDefault(error)) return true;
    if (startWASAPIInputDefault(error)) return true;
    return false;
}

void AudioEngine::stop()
{
#if JUCE_WINDOWS
    if (wasapiLoopback) { wasapiLoopback->stop(); wasapiLoopback.reset(); }
#endif

    // If we were using JUCE device I/O, tear it down.
    deviceManager.removeAudioCallback(this);
    if (auto* d = deviceManager.getCurrentAudioDevice())
        d->stop();
    deviceManager.closeAudioDevice();

    running.store(false);
}

AudioEngine::DeviceInfo AudioEngine::getCurrentDeviceInfo() const
{
    std::scoped_lock lk(infoMutex);
    return info;
}

//==============================================================================
// callback with context
void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* input, int numIn,
    float* const* output, int numOut,
    int numSamples,
    const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOut; ++ch)
        if (output[ch] != nullptr)
            juce::FloatVectorOperations::clear(output[ch], numSamples);

    if (onAudioBlock != nullptr && numIn > 0)
    {
        auto* dev = deviceManager.getCurrentAudioDevice();
        const double sr = dev ? dev->getCurrentSampleRate() : 0.0;
        onAudioBlock(input, numIn, numSamples, sr);
    }
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    std::scoped_lock lk(infoMutex);
    info.type = deviceManager.getCurrentAudioDeviceType();
    info.inputName = deviceManager.getAudioDeviceSetup().inputDeviceName;
    info.sampleRate = device ? device->getCurrentSampleRate() : 0.0;
    info.blockSize = device ? device->getCurrentBufferSizeSamples() : 0;
    info.numIn = device ? device->getActiveInputChannels().countNumberOfSetBits() : 0;
}

void AudioEngine::audioDeviceStopped()
{
    std::scoped_lock lk(infoMutex);
    info = {};
}

// --------- enumeration + validated startWithDevice ---------

std::vector<AudioEngine::DeviceEntry> AudioEngine::enumerateDevices()
{
    std::vector<DeviceEntry> out;

    juce::OwnedArray<juce::AudioIODeviceType> types;
    deviceManager.createAudioDeviceTypes(types);

    for (auto* t : types)
    {
        t->scanForDevices();

        // Input names (JUCE includes "(loopback)" entries here on Windows if available)
        auto inputs = t->getDeviceNames(true);
        for (auto& n : inputs)
        {
            DeviceEntry e;
            e.type = t->getTypeName();
            e.name = n;
            e.isInput = true;
            e.isLoopback = n.containsIgnoreCase("loopback");
            out.push_back(e);
        }
    }
    return out;
}

bool AudioEngine::startWithDevice(const DeviceEntry& entry, juce::String& error)
{
    stop();
    error.clear();

    if (!setDeviceType(entry.type))
    {
        error = "Device type not available: " + entry.type;
        return false;
    }

    // Validate that the device exists for this type
    juce::OwnedArray<juce::AudioIODeviceType> types;
    deviceManager.createAudioDeviceTypes(types);

    bool nameExists = false;
    for (auto* t : types)
    {
        if (t->getTypeName() != entry.type)
            continue;

        t->scanForDevices();
        auto inputs = t->getDeviceNames(true);
        for (auto& n : inputs)
            if (n == entry.name) { nameExists = true; break; }
        break;
    }

    if (!nameExists)
    {
        error = "No such device: " + entry.name;
        return false;
    }

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager.getAudioDeviceSetup(setup);

    setup.inputDeviceName = entry.name;
    setup.outputDeviceName = {};
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = false;

    auto err = deviceManager.setAudioDeviceSetup(setup, true);
    if (err.isNotEmpty()) { error = err; return false; }

    deviceManager.addAudioCallback(this);
    running.store(true);
    return true;
}
