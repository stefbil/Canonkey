#include <JuceHeader.h>

#if JUCE_WINDOWS
#include "WasapiLoopback.h"

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Mmdevapi.lib")
//#pragma comment(lib, "audioclient.lib")
#pragma comment(lib, "Uuid.lib")

WasapiLoopback::WasapiLoopback() {}
WasapiLoopback::~WasapiLoopback() { stop(); }

bool WasapiLoopback::start(BlockCB cb, juce::String& error)
{
    if (running.load()) return true;
    onBlock = std::move(cb);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) { error = "CoInitializeEx failed"; return false; }

    if (!initClient(error)) { releaseAll(); return false; }

    running.store(true);
    worker = std::thread(&WasapiLoopback::threadProc, this);
    return true;
}

void WasapiLoopback::stop()
{
    if (!running.exchange(false)) return;
    if (worker.joinable()) worker.join();
    releaseAll();
    CoUninitialize();
}

bool WasapiLoopback::initClient(juce::String& error)
{
    HRESULT hr = CoCreateInstance(__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof (IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) { error = "MMDeviceEnumerator create failed"; return false; }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &renderDevice);
    if (FAILED(hr)) { error = "GetDefaultAudioEndpoint failed"; return false; }

    hr = renderDevice->Activate(__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (FAILED(hr)) { error = "IAudioClient activate failed"; return false; }

    hr = client->GetMixFormat(&mixFmt);
    if (FAILED(hr)) { error = "GetMixFormat failed"; return false; }

    REFERENCE_TIME hnsBuffer = 20 * 10000; // 20ms
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        hnsBuffer, 0, mixFmt, nullptr);
    if (FAILED(hr)) { error = "IAudioClient Initialize (loopback) failed"; return false; }

    hr = client->GetBufferSize(&bufferFrames);
    if (FAILED(hr)) { error = "GetBufferSize failed"; return false; }

    hr = client->GetService(__uuidof (IAudioCaptureClient), (void**)&capture);
    if (FAILED(hr)) { error = "GetService(IID_IAudioCaptureClient) failed"; return false; }

    sampleRate = (double)mixFmt->nSamplesPerSec;

    const int channels = mixFmt->nChannels;
    planar.resize((size_t)channels);
    for (auto& ch : planar) ch.assign((size_t)bufferFrames, 0.0f);

    hr = client->Start();
    if (FAILED(hr)) { error = "IAudioClient Start failed"; return false; }

    return true;
}

void WasapiLoopback::threadProc()
{
    const int channels = (int)mixFmt->nChannels;

    while (running.load())
    {
        UINT32 packetFrames = 0;
        DWORD flags = 0;
        BYTE* data = nullptr;

        HRESULT hr = capture->GetNextPacketSize(&packetFrames);
        if (FAILED(hr)) { juce::Thread::sleep(2); continue; }

        if (packetFrames == 0) { juce::Thread::sleep(2); continue; }

        hr = capture->GetBuffer(&data, &packetFrames, &flags, nullptr, nullptr);
        if (FAILED(hr)) { juce::Thread::sleep(2); continue; }

        const int bps = mixFmt->wBitsPerSample;
        const bool isFloat = (bps == 32);

        for (auto& ch : planar) if ((int)ch.size() < (int)packetFrames) ch.resize(packetFrames);

        if (isFloat)
        {
            const float* interleaved = reinterpret_cast<const float*> (data);
            for (int c = 0; c < channels; ++c)
            {
                float* dst = planar[(size_t)c].data();
                for (UINT32 i = 0; i < packetFrames; ++i)
                    dst[i] = interleaved[i * channels + c];
            }
        }
        else
        {
            const int16_t* inter = reinterpret_cast<const int16_t*> (data);
            for (int c = 0; c < channels; ++c)
            {
                float* dst = planar[(size_t)c].data();
                for (UINT32 i = 0; i < packetFrames; ++i)
                    dst[i] = (float)inter[i * channels + c] / 32768.0f;
            }
        }

        if (onBlock)
        {
            const float* ptrs[16] = {};
            for (int c = 0; c < channels && c < 16; ++c)
                ptrs[c] = planar[(size_t)c].data();
            onBlock(ptrs, channels, (int)packetFrames, sampleRate);
        }

        capture->ReleaseBuffer(packetFrames);
    }

    if (client) client->Stop();
}

void WasapiLoopback::releaseAll()
{
    if (mixFmt) { CoTaskMemFree(mixFmt); mixFmt = nullptr; }
    if (capture) { capture->Release();     capture = nullptr; }
    if (client) { client->Release();      client = nullptr; }
    if (renderDevice) { renderDevice->Release(); renderDevice = nullptr; }
    if (enumerator) { enumerator->Release();  enumerator = nullptr; }
}
#endif // JUCE_WINDOWS
