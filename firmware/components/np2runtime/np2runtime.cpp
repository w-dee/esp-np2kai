#include "np2runtime/np2runtime.hpp"

extern "C" {
#include <compiler.h>
#include <common.h>
#include <pccore.h>
#include <scrnmng.h>
#include <taskmng.h>
#include <np2host/taskmng_esp.h>
}

namespace np2runtime {

Runtime::~Runtime()
{
    if (core_initialized_) {
        (void)lifecycle_.request_stop();
        (void)cleanup();
    } else if (lifecycle_.state() != State::Created &&
               lifecycle_.state() != State::Stopped &&
               lifecycle_.state() != State::Failed) {
        (void)lifecycle_.request_stop();
        (void)cleanup();
    }
}

Result Runtime::initialize(
    const std::optional<std::uint8_t> fddequip_override) noexcept
{
    /* An external stop received before initialization is a valid, no-op
     * cancellation.  Finalize it without touching the NP2 core. */
    if (lifecycle_.state() == State::StopRequested && !core_initialized_) {
        return cleanup();
    }

    if (!lifecycle_.begin_initialization()) {
        return Result::InvalidState;
    }

    if (lifecycle_.stop_requested()) {
        return cleanup();
    }

    apply_production_machine_config(fddequip_override);
    np2_host_taskmng_reset();
    pccore_init();
    core_initialized_ = true;
    pccore_reset();

    if (!lifecycle_.complete_initialization()) {
        (void)cleanup();
        return lifecycle_.failure() ? Result::RuntimeFailed : Result::Stopped;
    }
    return Result::Ok;
}

Result Runtime::run() noexcept
{
    return run(nullptr, nullptr);
}

Result Runtime::run(const OwnerIterationObserver observer,
                    void *const observer_context) noexcept
{
    owner_observer_ = observer;
    owner_observer_context_ = observer_context;
    if (!lifecycle_.begin_running()) {
        if (lifecycle_.state() == State::StopRequested) {
            return cleanup(observer, observer_context);
        }
        return Result::InvalidState;
    }

    while (!lifecycle_.stop_requested()) {
        /* The caller owns this task-affine pre-exec boundary.  It is sampled
         * on the same task that calls pccore_exec(), so producer state can be
         * drained and any stop-time cleanup can be serialized with the core. */
        if (observer != nullptr && observer(observer_context)) {
            (void)lifecycle_.request_stop();
            break;
        }
        pccore_exec(TRUE);
        np2_host_taskmng_cooperate();

        if (scrnmng_haserror()) {
            lifecycle_.mark_failure();
            break;
        }
        if (np2_host_taskmng_exit_requested()) {
            (void)lifecycle_.request_stop();
            break;
        }
    }

    return cleanup(observer, observer_context);
}

bool Runtime::request_stop() noexcept
{
    return lifecycle_.request_stop();
}

bool Runtime::mark_failure() noexcept
{
    return lifecycle_.mark_failure();
}

Result Runtime::cleanup(const OwnerIterationObserver observer,
                        void *const observer_context) noexcept
{
    if (!lifecycle_.begin_stopping()) {
        if (lifecycle_.state() == State::Stopped) {
            return Result::Stopped;
        }
        if (lifecycle_.state() == State::Failed) {
            return Result::RuntimeFailed;
        }
        return Result::InvalidState;
    }

    if (core_initialized_) {
        const OwnerIterationObserver cleanup_observer =
            observer != nullptr ? observer : owner_observer_;
        void *const cleanup_context =
            observer != nullptr ? observer_context : owner_observer_context_;
        if (cleanup_observer != nullptr) {
            (void)cleanup_observer(cleanup_context);
        }
        pccore_term();
        core_initialized_ = false;
    }
    owner_observer_ = nullptr;
    owner_observer_context_ = nullptr;
    (void)lifecycle_.finish_cleanup();
    return lifecycle_.failure() ? Result::RuntimeFailed : Result::Stopped;
}

} // namespace np2runtime
