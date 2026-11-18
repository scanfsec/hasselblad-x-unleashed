#include "../M100_GPS/raw_stream.h"

#include <cassert>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;

void retriesAnUnacceptedPacketWithItsOriginalOffset() {
    RawGpsStream queue;
    queue.select(1);
    for (unsigned i = 0; i < 8; ++i) queue.push(static_cast<uint8_t>(i), 500 + i);

    std::vector<Bytes> attempts;
    auto sender = [&attempts](const uint8_t *bytes, size_t size) {
        attempts.emplace_back(bytes, bytes + size);
        return attempts.size() != 1;
    };
    queue.pump(10, 4, false, sender);
    queue.pump(17, 4, false, sender);
    assert(attempts.size() == 1);
    queue.pump(18, 4, false, sender);
    assert(attempts.size() == 3);
    assert(attempts[0] == Bytes({0xf4, 1, 0, 0, 0, 1, 2, 3}));
    assert(attempts[0] == attempts[1]);
    assert(attempts[2] == Bytes({0xf8, 1, 0, 0, 4, 5, 6, 7}));
}

void wrapsOffsetsAndDrainsTailUnderSmallerMtu() {
    RawGpsStream queue;
    queue.select(1);
    queue.push(1, UINT32_MAX);
    queue.push(2, 0);
    queue.push(3, 1);
    std::vector<Bytes> attempts;
    auto sender = [&attempts](const uint8_t *bytes, size_t size) {
        attempts.emplace_back(bytes, bytes + size);
        return true;
    };
    queue.pump(20, 240, false, sender);
    assert(attempts.empty());
    queue.pump(21, 1, true, sender);
    assert(attempts.size() == 2);
    assert(attempts[0] == Bytes({255, 255, 255, 255, 1}));
    assert(attempts[1] == Bytes({0, 0, 0, 0, 2}));
    queue.pump(21, 16, true, sender);
    assert(attempts.size() == 2);
    queue.pump(22, 16, true, sender);
    assert(attempts[2] == Bytes({1, 0, 0, 0, 3}));
}

void retainsNewestBytesOnOverflowAndClearsOnNewSubscription() {
    RawGpsStream queue;
    queue.select(1);
    for (unsigned i = 0; i < RawGpsStream::kCapacity; ++i)
        assert(!queue.push(static_cast<uint8_t>(i), 100 + i));
    assert(queue.push(0, 100 + RawGpsStream::kCapacity));
    std::vector<Bytes> attempts;
    auto sender = [&attempts](const uint8_t *bytes, size_t size) {
        attempts.emplace_back(bytes, bytes + size);
        return true;
    };
    queue.pump(1, 1, false, sender);
    assert(attempts[0] == Bytes({101, 0, 0, 0, 1}));
    assert(attempts[1] == Bytes({102, 0, 0, 0, 2}));
    queue.select(2);
    queue.pump(2, 1, true, sender);
    assert(attempts.size() == 2);
    queue.push(42, 4097);
    queue.pump(3, 1, true, sender);
    assert(attempts[2] == Bytes({1, 16, 0, 0, 42}));
}
}

int main() {
    retriesAnUnacceptedPacketWithItsOriginalOffset();
    wrapsOffsetsAndDrainsTailUnderSmallerMtu();
    retainsNewestBytesOnOverflowAndClearsOnNewSubscription();
}
