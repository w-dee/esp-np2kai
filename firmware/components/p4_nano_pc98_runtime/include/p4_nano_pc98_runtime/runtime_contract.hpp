#pragma once

#include <cstdint>
#include <string_view>

namespace p4_nano_pc98_runtime {

struct MediaConfig final {
    std::string_view physical_path;
    std::string_view logical_path;
    std::uint8_t fdd_unit;
};

inline constexpr std::uint8_t kFdd0OnlyEquipment = 1U;

inline constexpr MediaConfig production_media_config() noexcept
{
    return {"/sdcard/files/pc98/fdd/boot.hdm", "./runtime-fdd0.hdm", 0U};
}

inline constexpr MediaConfig validation_media_config() noexcept
{
    return {"/persist/fixtures/np2test-fd1232.hdm",
            "./runtime-validation-fdd0.hdm", 0U};
}

inline constexpr MediaConfig hardware_validation_media_config() noexcept
{
    return {"/sdcard/files/pc98/fdd/runtime-validation.hdm",
            "./runtime-validation-fdd0.hdm", 0U};
}

#if !defined(ESP_PLATFORM) || defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
inline constexpr MediaConfig keyboard_validation_media_config() noexcept
{
    return {"/persist/fixtures/np2kbdtest-fd1232.hdm",
            "./runtime-keyboard-validation-fdd0.hdm", 0U};
}

inline constexpr MediaConfig hardware_keyboard_validation_media_config() noexcept
{
    return {"/sdcard/files/pc98/fdd/runtime-keyboard-validation.hdm",
            "./runtime-keyboard-validation-fdd0.hdm", 0U};
}
#endif

/* Runtime::cleanup() terminates pccore on the owner task before that task is
 * joined; only then can the composition eject the drive and tear down VFS.
 * This is the dependency-safe realization of the public cleanup contract. */
enum class CleanupStage : std::uint8_t {
    SourceDetached,
    StopRequested,
    CoreTerminated,
    OwnerJoined,
    LeasesReleased,
    FddEjected,
    ScrnmngShutdown,
    DosioReset,
    SessionShutdown,
    StorageUnmounted,
};

constexpr bool cleanup_precedes(CleanupStage first,
                                CleanupStage second) noexcept
{
    return static_cast<std::uint8_t>(first) <
           static_cast<std::uint8_t>(second);
}

} // namespace p4_nano_pc98_runtime
