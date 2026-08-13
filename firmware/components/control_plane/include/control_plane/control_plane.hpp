#pragma once

#include <cstddef>
#include <cstdint>

namespace control_plane {

inline constexpr std::size_t kMaxFrameBytes = 512;
inline constexpr std::size_t kFrameStorageBytes = kMaxFrameBytes + 1;
inline constexpr std::size_t kMaxOutputFrameBytes = 1024;
inline constexpr char kFramePrefix[] = "@ESP-NP2 ";

struct OutputSink {
    void *context;
    bool (*write)(void *context, const char *data, std::size_t length);
};

struct Metadata {
    const char *project;
    const char *firmware_version;
    const char *idf_version;
    const char *target;
};

struct ControlPlane {
    OutputSink output{};
    Metadata metadata{};
    char frame[kFrameStorageBytes]{};
    std::size_t frame_length = 0;
    bool discarding = false;
    bool prefix_candidate = true;
};

void init(ControlPlane *control, OutputSink output, Metadata metadata);

void feed(ControlPlane *control, const std::uint8_t *data, std::size_t length);

} // namespace control_plane
