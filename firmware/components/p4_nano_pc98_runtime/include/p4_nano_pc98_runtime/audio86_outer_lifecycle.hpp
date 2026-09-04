#pragma once

#include <atomic>
#include <cstdint>

namespace p4_nano_pc98_runtime::audio86_outer {

enum class OuterState : std::uint32_t {
    Created = 0U,
    WaitingReady,
    WaitingComplete,
    Complete,
    TimedOut,
};

enum class StartupState : std::uint32_t {
    Pending = 0U,
    Ready,
    Failed,
    TimedOut,
};

enum class CompletionState : std::uint32_t {
    Pending = 0U,
    Succeeded,
    Failed,
    TimedOut,
};

enum class TerminalOwner : std::uint32_t {
    None = 0U,
    InnerComplete,
    InnerFailed,
    ReadyTimeout,
    CompletionTimeout,
};

struct Lifecycle {
    std::atomic<std::uint32_t> outer_state{
        static_cast<std::uint32_t>(OuterState::Created)};
    std::atomic<std::uint32_t> startup_state{
        static_cast<std::uint32_t>(StartupState::Pending)};
    std::atomic<std::int32_t> startup_result{-1};
    std::atomic<std::uint32_t> completion_state{
        static_cast<std::uint32_t>(CompletionState::Pending)};
    std::atomic<std::int32_t> completion_result{-1};
    std::atomic<std::uint32_t> terminal_owner{
        static_cast<std::uint32_t>(TerminalOwner::None)};
    std::atomic<std::uint32_t> timeout_snapshot_claimed{0U};

    void reset(std::int32_t pending_result) noexcept
    {
        startup_result.store(pending_result, std::memory_order_relaxed);
        completion_result.store(pending_result, std::memory_order_relaxed);
        startup_state.store(static_cast<std::uint32_t>(StartupState::Pending),
                            std::memory_order_relaxed);
        completion_state.store(
            static_cast<std::uint32_t>(CompletionState::Pending),
            std::memory_order_relaxed);
        terminal_owner.store(static_cast<std::uint32_t>(TerminalOwner::None),
                             std::memory_order_relaxed);
        timeout_snapshot_claimed.store(0U, std::memory_order_relaxed);
        outer_state.store(static_cast<std::uint32_t>(OuterState::Created),
                          std::memory_order_release);
    }

    void begin_ready_wait() noexcept
    {
        outer_state.store(static_cast<std::uint32_t>(OuterState::WaitingReady),
                          std::memory_order_release);
    }

    bool publish_startup(std::int32_t result, bool success) noexcept
    {
        std::uint32_t expected =
            static_cast<std::uint32_t>(StartupState::Pending);
        if (!startup_state.compare_exchange_strong(
                expected,
                static_cast<std::uint32_t>(success ? StartupState::Ready
                                                   : StartupState::Failed),
                std::memory_order_acq_rel, std::memory_order_acquire))
            return false;
        startup_result.store(result, std::memory_order_release);
        return true;
    }

    bool mark_startup_timeout(std::int32_t timeout_result) noexcept
    {
        std::uint32_t expected =
            static_cast<std::uint32_t>(StartupState::Pending);
        if (!startup_state.compare_exchange_strong(
                expected,
                static_cast<std::uint32_t>(StartupState::TimedOut),
                std::memory_order_acq_rel, std::memory_order_acquire))
            return false;
        startup_result.store(timeout_result, std::memory_order_release);
        outer_state.store(static_cast<std::uint32_t>(OuterState::TimedOut),
                          std::memory_order_release);
        return true;
    }

    void begin_completion_wait() noexcept
    {
        outer_state.store(
            static_cast<std::uint32_t>(OuterState::WaitingComplete),
            std::memory_order_release);
    }

    bool publish_completion(std::int32_t result, bool success) noexcept
    {
        std::uint32_t expected =
            static_cast<std::uint32_t>(CompletionState::Pending);
        if (!completion_state.compare_exchange_strong(
                expected,
                static_cast<std::uint32_t>(success
                                               ? CompletionState::Succeeded
                                               : CompletionState::Failed),
                std::memory_order_acq_rel, std::memory_order_acquire))
            return false;
        completion_result.store(result, std::memory_order_release);
        return true;
    }

    bool mark_completion_timeout(std::int32_t timeout_result) noexcept
    {
        std::uint32_t expected =
            static_cast<std::uint32_t>(CompletionState::Pending);
        if (!completion_state.compare_exchange_strong(
                expected,
                static_cast<std::uint32_t>(CompletionState::TimedOut),
                std::memory_order_acq_rel, std::memory_order_acquire))
            return false;
        completion_result.store(timeout_result, std::memory_order_release);
        outer_state.store(static_cast<std::uint32_t>(OuterState::TimedOut),
                          std::memory_order_release);
        return true;
    }

    void mark_complete() noexcept
    {
        outer_state.store(static_cast<std::uint32_t>(OuterState::Complete),
                          std::memory_order_release);
    }

    bool claim_terminal(TerminalOwner owner) noexcept
    {
        std::uint32_t expected =
            static_cast<std::uint32_t>(TerminalOwner::None);
        return terminal_owner.compare_exchange_strong(
            expected, static_cast<std::uint32_t>(owner),
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool claim_timeout_snapshot() noexcept
    {
        std::uint32_t expected = 0U;
        return timeout_snapshot_claimed.compare_exchange_strong(
            expected, 1U, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    [[nodiscard]] bool owner_delete_allowed(bool completion_observed,
                                             bool owner_done) const noexcept
    {
        const auto state = static_cast<CompletionState>(
            completion_state.load(std::memory_order_acquire));
        return completion_observed && owner_done &&
               (state == CompletionState::Succeeded ||
                state == CompletionState::Failed);
    }
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::int32_t>::is_always_lock_free);

} // namespace p4_nano_pc98_runtime::audio86_outer
