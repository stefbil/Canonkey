#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <algorithm>

// Single-producer / single-consumer lock-free ring buffer (float, mono).
// Producer: audio thread (pushPlanarToMono)
// Consumer: analyzer/timer thread (pop)
// Capacity is rounded to power-of-two for fast masking.
class RingBuffer
{
public:
    explicit RingBuffer(size_t capacityPow2 = (1u << 16)) { reset(capacityPow2); }

    void reset(size_t capacityPow2)
    {
        size_t cap = 256;
        while (cap < capacityPow2) cap <<= 1;
        capacity_ = cap;
        mask_ = capacity_ - 1;
        buffer_.assign(capacity_, 0.0f);
        write_.store(0, std::memory_order_relaxed);
        read_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
    }

    size_t capacity() const noexcept { return capacity_; }

    size_t size() const noexcept
    {
        auto w = write_.load(std::memory_order_acquire);
        auto r = read_.load(std::memory_order_acquire);
        return w - r;
    }

    size_t freeSpace() const noexcept { return capacity_ - 1 - size(); }

    void clear() noexcept
    {
        auto w = write_.load(std::memory_order_relaxed);
        read_.store(w, std::memory_order_relaxed);
    }

    // Mix planar input to mono and push. Returns samples written (<= numSamples).
    size_t pushPlanarToMono(const float* const* input, int numCh, int numSamples, float gain = 1.0f) noexcept
    {
        if (input == nullptr || numCh <= 0 || numSamples <= 0) return 0;

        auto w = write_.load(std::memory_order_relaxed);
        auto r = read_.load(std::memory_order_acquire);
        size_t free = capacity_ - 1 - (w - r);
        size_t toWrite = (size_t)std::min<int>(numSamples, (int)free);

        for (size_t i = 0; i < toWrite; ++i)
        {
            float s = 0.0f;
            for (int c = 0; c < numCh; ++c) s += input[c][(int)i];
            s = (s / (float)numCh) * gain; // average to avoid clipping
            buffer_[(w + i) & mask_] = s;
        }

        write_.store(w + toWrite, std::memory_order_release);

        size_t dropped = (size_t)std::max<int>(0, numSamples - (int)toWrite);
        if (dropped) dropped_.fetch_add(dropped, std::memory_order_relaxed);

        return toWrite;
    }

    // Pop up to numSamples into dst. Returns actual popped.
    size_t pop(float* dst, size_t numSamples) noexcept
    {
        if (dst == nullptr || numSamples == 0) return 0;

        auto r = read_.load(std::memory_order_relaxed);
        auto w = write_.load(std::memory_order_acquire);
        size_t avail = (size_t)(w - r);
        size_t toRead = std::min(avail, numSamples);

        for (size_t i = 0; i < toRead; ++i)
            dst[i] = buffer_[(r + i) & mask_];

        read_.store(r + toRead, std::memory_order_release);
        return toRead;
    }

    size_t droppedSamples() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    std::vector<float> buffer_;
    size_t capacity_ = 0;
    size_t mask_ = 0;

    std::atomic<size_t> write_{ 0 };
    std::atomic<size_t> read_{ 0 };
    std::atomic<size_t> dropped_{ 0 };
};
