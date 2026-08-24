#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>

namespace np2runtime {

enum class State : std::uint8_t {
    Created,
    Initializing,
    Ready,
    Running,
    StopRequested,
    Stopping,
    Stopped,
    Failed,
};

enum class StopReason : std::uint8_t {
    None,
    External,
    Fatal,
};

enum class Result : std::uint8_t {
    Ok,
    Stopped,
    InvalidState,
    InitializationFailed,
    RuntimeFailed,
};

/* The machine values are deliberately a value object so host tests can
 * inspect the production policy without linking the NP2 core.  FDD equipment
 * remains unset until the FDD composition slice validates the drive mask. */
struct ProductionMachineConfig final {
    std::string_view model = "VX";
    std::uint32_t baseclock = 2457600U;
    std::uint32_t multiple = 20U;
    std::array<std::uint8_t, 3> dipsw{0x3e, 0xe3, 0x7b};
    std::array<std::uint8_t, 8> memsw{0x48, 0x05, 0x04, 0x08,
                                      0x01, 0x00, 0x00, 0x6e};
    std::uint16_t extmem_mb = 8U;
    std::optional<std::uint8_t> fddequip_override{};
    std::uint8_t memcheckspeed = 8U;
    std::uint8_t itf_work = 1U;
    std::uint32_t emuspeed = 100U;
    std::uint8_t dispsync = 1U;
    std::array<std::uint8_t, 6> wait{1U, 1U, 6U, 1U, 8U, 1U};
    bool usebios = false;
    bool disable_sound = true;
    bool disable_midi = true;
    bool disable_optional_devices = true;
    bool clear_disk_paths = true;
};

constexpr ProductionMachineConfig production_machine_config() noexcept
{
    return {};
}

/* Applies pccore_setdefault() followed by the explicit production overrides.
 * No disk path is attached by this function. */
void apply_production_machine_config() noexcept;

/* Small atomic state machine shared by Runtime and host lifecycle tests.  It
 * contains no fixture, timeout, result-block, or platform resource policy. */
class Lifecycle final {
public:
    Lifecycle() noexcept = default;
    Lifecycle(const Lifecycle &) = delete;
    Lifecycle &operator=(const Lifecycle &) = delete;

    State state() const noexcept;
    StopReason stop_reason() const noexcept;
    bool failure() const noexcept;
    bool stop_requested() const noexcept;

    bool begin_initialization() noexcept;
    bool complete_initialization() noexcept;
    bool begin_running() noexcept;
    bool request_stop() noexcept;
    bool mark_failure() noexcept;
    bool begin_stopping() noexcept;
    bool finish_cleanup() noexcept;

private:
    static bool active_state(State state) noexcept;
    static int stop_reason_rank(StopReason reason) noexcept;
    bool promote_stop_reason(StopReason reason) noexcept;
    void lock_terminalization() noexcept;
    void unlock_terminalization() noexcept;

    std::atomic<State> state_{State::Created};
    std::atomic<StopReason> stop_reason_{StopReason::None};
    std::atomic<bool> failure_{false};
    std::atomic_flag terminalization_lock_ = ATOMIC_FLAG_INIT;
};

/* Runtime owns only NP2 core state in Slice A.  The caller supplies the task
 * on which initialize() and run() execute; no task or heap object is created
 * by this class. */
class Runtime final {
public:
    Runtime() noexcept = default;
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;
    ~Runtime();

    Result initialize() noexcept;
    Result run() noexcept;
    bool request_stop() noexcept;

    State state() const noexcept { return lifecycle_.state(); }
    StopReason stop_reason() const noexcept { return lifecycle_.stop_reason(); }
    bool failure() const noexcept { return lifecycle_.failure(); }

private:
    Result cleanup() noexcept;

    Lifecycle lifecycle_;
    bool core_initialized_ = false;
};

} // namespace np2runtime
