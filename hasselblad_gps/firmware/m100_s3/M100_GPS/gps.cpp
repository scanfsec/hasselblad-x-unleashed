#include <Arduino.h>
#include <TinyGPS++.h>
#include <NimBLEDevice.h>
#include <atomic>
#include "raw_stream.h"

namespace {
constexpr int kGpsRx = 18;
constexpr int kGpsTx = 17;
constexpr uint32_t kGpsBaud = 115200;
constexpr uint32_t kFreshMs = 2500;
HardwareSerial gpsSerial(1);
TinyGPSPlus nmea;

struct Fix {
    bool valid;
    double lat;
    double lon;
    unsigned satellites;
};

// Consume complete UBX frames, storing only the NAV-PVT payload.
class UbxReceiver {
public:
    // Returns true for bytes consumed by UBX, to keep binary out of NMEA.
    bool feed(uint8_t byte) {
        if (state == Sync1) {
            if (byte != 0xb5) return false;
            state = Sync2;
            return true;
        }
        if (state == Sync2) {
            state = byte == 0x62 ? Header : byte == 0xb5 ? Sync2 : Sync1;
            index = ckA = ckB = 0;
            return state != Sync1;
        }
        if (state == Header) {
            checksum(byte);
            header[index++] = byte;
            if (index == sizeof(header)) {
                length = header[2] | (uint16_t(header[3]) << 8);
                isPvt = header[0] == 1 && header[1] == 7 && length == sizeof(payload);
                index = 0;
                state = length > 4096 ? Sync1 : length ? Payload : CheckA;
            }
        } else if (state == Payload) {
            checksum(byte);
            if (isPvt) payload[index] = byte;
            if (++index == length) state = CheckA;
        } else if (state == CheckA) {
            state = byte == ckA ? CheckB : byte == 0xb5 ? Sync2 : Sync1;
        } else {
            if (byte == ckB) {
                ++frames;
                if (isPvt) {
                    const auto type = payload[20];
                    fix = {(payload[21] & 1) && !(payload[78] & 1) &&
                               (type == 2 || type == 3 || type == 4),
                           coordinate(28), coordinate(24), payload[23]};
                    lastPvt = millis();
                    hasPvt = true;
                }
                state = Sync1;
            } else {
                state = byte == 0xb5 ? Sync2 : Sync1;
            }
        }
        return true;
    }

    void resetFrame() { state = Sync1; }
    uint32_t frames = 0;
    uint32_t lastPvt = 0;
    bool hasPvt = false;
    Fix fix{};

private:
    void checksum(uint8_t byte) {
        ckA += byte;
        ckB += ckA;
    }

    double coordinate(unsigned offset) const {
        uint32_t raw = 0;
        for (unsigned i = 0; i < 4; ++i)
            raw |= uint32_t(payload[offset + i]) << (8 * i);
        const int64_t value = raw < 0x80000000u ? int64_t(raw) : int64_t(raw) - 0x100000000LL;
        return value * 1e-7;
    }

    enum State { Sync1, Sync2, Header, Payload, CheckA, CheckB };
    State state = Sync1;
    uint8_t header[4]{};
    uint8_t payload[92]{};
    uint8_t ckA = 0, ckB = 0;
    uint16_t index = 0, length = 0;
    bool isPvt = false;
};

UbxReceiver ubx;
uint32_t bytes = 0;
uint32_t nmeaFrames = 0;
uint32_t lastByte = 0;
uint32_t lastReport = 0;
bool nmeaActive = false;
NimBLECharacteristic *rawStream;
std::atomic<unsigned> requestedPolls{0};
uint32_t lastPoll = 0;
RawGpsStream streamQueue;
std::atomic<uint32_t> reportedBytes{0};
std::atomic<uint32_t> notificationsAccepted{0};
std::atomic<uint32_t> notificationFailures{0};
std::atomic<bool> queueOverflowed{false};

struct Subscription {
    uint32_t generation = 0;
    uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
    unsigned payloadLimit = 0;
};

Subscription subscription;
portMUX_TYPE subscriptionMutex = portMUX_INITIALIZER_UNLOCKED;

unsigned rawPayloadLimit(uint16_t mtu) {
    return mtu > 7 ? min(RawGpsStream::kMaxPayload, unsigned(mtu) - 7) : 0;
}

Subscription subscriptionSnapshot() {
    portENTER_CRITICAL(&subscriptionMutex);
    const Subscription snapshot = subscription;
    portEXIT_CRITICAL(&subscriptionMutex);
    return snapshot;
}

void releaseSubscription(uint16_t handle) {
    portENTER_CRITICAL(&subscriptionMutex);
    if (subscription.handle == handle) {
        ++subscription.generation;
        subscription.handle = BLE_HS_CONN_HANDLE_NONE;
        subscription.payloadLimit = 0;
    }
    portEXIT_CRITICAL(&subscriptionMutex);
}

class StreamCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &peer, uint16_t value) override {
        const uint16_t handle = peer.getConnHandle();
        if (!(value & 1)) {
            releaseSubscription(handle);
            return;
        }
        const unsigned limit = rawPayloadLimit(peer.getMTU());
        portENTER_CRITICAL(&subscriptionMutex);
        if (subscription.handle == BLE_HS_CONN_HANDLE_NONE) {
            ++subscription.generation;
            subscription.handle = handle;
        }
        if (subscription.handle == handle) subscription.payloadLimit = limit;
        portEXIT_CRITICAL(&subscriptionMutex);
    }
};

class ServerCallbacks : public NimBLEServerCallbacks {
    void onDisconnect(NimBLEServer *, NimBLEConnInfo &peer, int) override {
        releaseSubscription(peer.getConnHandle());
    }
    void onMTUChange(uint16_t mtu, NimBLEConnInfo &peer) override {
        portENTER_CRITICAL(&subscriptionMutex);
        if (subscription.handle == peer.getConnHandle()) subscription.payloadLimit = rawPayloadLimit(mtu);
        portEXIT_CRITICAL(&subscriptionMutex);
    }
};

class QueryCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &) override {
        const auto value = characteristic->getValue();
        if (value.size() == 1 && value.data()[0] >= 1 && value.data()[0] <= 2) {
            requestedPolls.fetch_or(1u << (value.data()[0] - 1));
        }
    }
};

class StatusCallbacks : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic *characteristic, NimBLEConnInfo &peer) override {
        const auto current = subscriptionSnapshot();
        const uint16_t mtu = peer.getMTU();
        const uint8_t flags = (current.payloadLimit ? 1 : 0) |
            (current.handle == peer.getConnHandle() ? 2 : 0) | (queueOverflowed.load() ? 4 : 0);
        uint8_t status[20]{1, flags, static_cast<uint8_t>(mtu), static_cast<uint8_t>(mtu >> 8)};
        const uint32_t fields[]{millis(), reportedBytes.load(), notificationsAccepted.load(),
                                notificationFailures.load()};
        for (unsigned i = 0; i < 4; ++i)
            for (unsigned j = 0; j < 4; ++j)
                status[4 + i * 4 + j] = static_cast<uint8_t>(fields[i] >> (8 * j));
        characteristic->setValue(status, sizeof(status));
    }
};

StreamCallbacks streamCallbacks;
ServerCallbacks serverCallbacks;
QueryCallbacks queryCallbacks;
StatusCallbacks statusCallbacks;

void pollGps(uint8_t cls, uint8_t id) {
    // Both supported polls have empty payloads.
    const uint8_t a = cls + id;
    const uint8_t b = cls + 3 * a;
    const uint8_t frame[]{0xb5, 0x62, cls, id, 0, 0, a, b};
    gpsSerial.write(frame, sizeof(frame));
}

void pumpRaw(uint32_t now) {
    const auto current = subscriptionSnapshot();
    streamQueue.select(current.generation);
    streamQueue.pump(now, current.payloadLimit, now - lastByte >= 20,
        [current](const uint8_t *packet, size_t size) {
            const auto active = subscriptionSnapshot();
            if (active.generation != current.generation || size - RawGpsStream::kHeaderSize > active.payloadLimit)
                return false;
            const bool accepted = rawStream->notify(packet, size, current.handle);
            if (accepted) ++notificationsAccepted;
            else ++notificationFailures;
            return accepted;
        });
}
}  // namespace

void setup() {
    Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
    Serial.setTxTimeoutMs(0);
#endif
    gpsSerial.setRxBufferSize(4096);
    gpsSerial.begin(kGpsBaud, SERIAL_8N1, kGpsRx, kGpsTx);
    NimBLEDevice::init("M100-GPS");
    NimBLEDevice::setMTU(247);
    auto *server = NimBLEDevice::createServer();
    server->setCallbacks(&serverCallbacks, false);
    server->advertiseOnDisconnect(true);
    auto *service = server->createService("a5720001-0d52-4d11-a825-8b94b63bf10c");
    rawStream = service->createCharacteristic("a5720002-0d52-4d11-a825-8b94b63bf10c",
                                              NIMBLE_PROPERTY::NOTIFY);
    rawStream->setCallbacks(&streamCallbacks);
    auto *query = service->createCharacteristic("a5720003-0d52-4d11-a825-8b94b63bf10c",
                                                NIMBLE_PROPERTY::WRITE);
    query->setCallbacks(&queryCallbacks);
    auto *status = service->createCharacteristic("a5720004-0d52-4d11-a825-8b94b63bf10c",
                                                 NIMBLE_PROPERTY::READ);
    status->setCallbacks(&statusCallbacks);
    server->start();
    auto *advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(service->getUUID());
    advertising->enableScanResponse(true);
    advertising->setName("M100-GPS");
    advertising->start();
}

void loop() {
    if (millis() - lastPoll >= 500) {
        const unsigned polls = requestedPolls.exchange(0);
        if (polls) {
            lastPoll = millis();
            if (polls & 1) pollGps(0x0a, 0x04); // MON-VER
            if (polls & 2) pollGps(0x01, 0x35); // NAV-SAT
        }
    }
    const auto current = subscriptionSnapshot();
    streamQueue.select(current.generation);
    for (unsigned i = 0; i < 1024; ++i) {
        const int byte = gpsSerial.read();
        if (byte < 0) break;
        const uint32_t now = millis();
        if (now - lastByte > 250) {
            ubx.resetFrame();
            nmeaActive = false;
        }
        lastByte = now;
        if (current.payloadLimit && streamQueue.push(byte, bytes)) queueOverflowed = true;
        ++bytes;
        if (ubx.feed(byte)) {
            nmeaActive = false;
        } else {
            if (byte == '$') nmeaActive = true;
            if (nmeaActive && nmea.encode(char(byte))) ++nmeaFrames;
            if (byte == '\r' || byte == '\n') nmeaActive = false;
        }
    }
    reportedBytes = bytes;

    const uint32_t now = millis();
    pumpRaw(now);
    delay(1);
    if (now - lastReport < 1000) return;
    lastReport = now;
    const bool active = bytes != 0 && now - lastByte < kFreshMs;
    const bool useUbx = active && ubx.hasPvt && now - ubx.lastPvt < kFreshMs;
    // TinyGPS++ keeps the last valid coordinates when newer sentences lose fix.
    // Expire them using the age of the last valid coordinate update.
    const bool nmeaFix = active && nmea.location.isValid() && nmea.location.age() < kFreshMs;
    Fix fix = useUbx ? ubx.fix : Fix{nmeaFix, nmea.location.lat(), nmea.location.lng(),
        nmea.satellites.age() < kFreshMs ? unsigned(nmea.satellites.value()) : 0u};
    char line[160];
    int length = snprintf(line, sizeof(line), "gps=%s rx=%lu nmea=%lu ubx=%lu fix=%u sats=%u",
        active ? "receiving" : "no-data", static_cast<unsigned long>(bytes),
        static_cast<unsigned long>(nmeaFrames), static_cast<unsigned long>(ubx.frames),
        unsigned(fix.valid), fix.satellites);
    if (length > 0 && length < static_cast<int>(sizeof(line)) && fix.valid)
        length += snprintf(line + length, sizeof(line) - length, " lat=%.7f lon=%.7f", fix.lat, fix.lon);
    if (length > 0 && length + 2 <= static_cast<int>(sizeof(line)) && Serial.availableForWrite() >= length + 2) {
        line[length++] = '\r';
        line[length++] = '\n';
        Serial.write(reinterpret_cast<const uint8_t *>(line), length);
    }
}
