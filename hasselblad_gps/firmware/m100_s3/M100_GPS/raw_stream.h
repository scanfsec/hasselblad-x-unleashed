#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

class RawGpsStream {
public:
    static constexpr unsigned kMaxPayload = 240;
    static constexpr unsigned kHeaderSize = 4;
    static constexpr unsigned kCapacity = 8192;
    using Packet = std::array<uint8_t, kHeaderSize + kMaxPayload>;

    void select(uint32_t generation) {
        if (generation == generation_) return;
        generation_ = generation;
        head_ = size_ = 0;
        delayMs_ = 0;
    }

    bool push(uint8_t byte, uint32_t offset) {
        const bool full = size_ == kCapacity;
        if (full) consume(1);
        if (!size_) offset_ = offset;
        data_[(head_ + size_) % kCapacity] = byte;
        ++size_;
        return full;
    }

    template <class Send>
    void pump(uint32_t now, unsigned limit, bool flushTail, Send send) {
        if (!limit || limit > kMaxPayload || now - lastAttempt_ < delayMs_) return;
        for (unsigned i = 0; i < 2 && size_ && (size_ >= limit || flushTail); ++i) {
            const unsigned length = size_ < limit ? size_ : limit;
            Packet packet;
            for (unsigned j = 0; j < kHeaderSize; ++j)
                packet[j] = static_cast<uint8_t>(offset_ >> (8 * j));
            for (unsigned j = 0; j < length; ++j)
                packet[kHeaderSize + j] = data_[(head_ + j) % kCapacity];

            lastAttempt_ = now;
            if (!send(packet.data(), kHeaderSize + length)) {
                delayMs_ = 8;
                return;
            }
            consume(length);
            delayMs_ = limit >= 32 ? 5 : 1;
        }
    }

private:
    void consume(unsigned length) {
        head_ = (head_ + length) % kCapacity;
        size_ -= length;
        offset_ += length;
    }

    std::array<uint8_t, kCapacity> data_{};
    unsigned head_ = 0;
    unsigned size_ = 0;
    uint32_t offset_ = 0;
    uint32_t generation_ = 0;
    uint32_t lastAttempt_ = 0;
    uint32_t delayMs_ = 0;
};
