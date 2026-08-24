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

Result Runtime::initialize() noexcept
{
    if (!lifecycle_.begin_initialization()) {
        return Result::InvalidState;
    }

    if (lifecycle_.stop_requested()) {
        (void)cleanup();
        return Result::Stopped;
    }

    apply_production_machine_config();
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
    if (!lifecycle_.begin_running()) {
        if (lifecycle_.state() == State::StopRequested) {
            return cleanup();
        }
        return Result::InvalidState;
    }

    while (!lifecycle_.stop_requested()) {
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

    return cleanup();
}

bool Runtime::request_stop() noexcept
{
    return lifecycle_.request_stop();
}

Result Runtime::cleanup() noexcept
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
        pccore_term();
        core_initialized_ = false;
    }
    (void)lifecycle_.finish_cleanup();
    return lifecycle_.failure() ? Result::RuntimeFailed : Result::Stopped;
}

} // namespace np2runtime
