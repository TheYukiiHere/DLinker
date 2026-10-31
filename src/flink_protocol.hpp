#pragma once

#include <cstdint>

namespace flink {

constexpr std::uint16_t kPort = 5002;
constexpr std::uint32_t kMagic = 0x464C4E4B; // "FLNK"
constexpr std::uint16_t kVersion = 2;

#pragma pack(push, 1)
struct PacketHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t type;
    std::uint32_t payloadSize;
    std::uint32_t sequence;
};
#pragma pack(pop)

enum class PacketType : std::uint16_t {
    Hello = 1,
    Ping = 2,
    Pong = 3,
    ManifestRequest = 4,
    Manifest = 5,
    FilePut = 6,
    SyncReady = 7,
    SyncDone = 8,
    VideoFrame = 9,
    AudioFrame = 10,
    Input = 11,
};

#pragma pack(push, 1)
struct VideoFrameInfo {
    std::uint16_t width;
    std::uint16_t height;
    std::uint16_t bytesPerPixel;
    std::uint16_t reserved;
};

struct AudioFrameInfo {
    std::uint32_t sampleRate;
    std::uint16_t channels;
    std::uint16_t bytesPerSample;
};

struct InputInfo {
    std::int32_t mouseDx;
    std::int32_t mouseDy;
    std::uint8_t mouseButtons;
    std::uint8_t keyDown;
    std::uint16_t key;
    std::uint8_t mouseButton;
    std::uint8_t buttonDown;
};
#pragma pack(pop)

} // namespace flink
