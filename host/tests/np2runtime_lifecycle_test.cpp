#include <cassert>
#include <barrier>
#include <thread>

#include "np2runtime/np2runtime.hpp"

int main()
{
    using np2runtime::Lifecycle;
    using np2runtime::State;
    using np2runtime::StopReason;

    Lifecycle lifecycle;
    assert(lifecycle.state() == State::Created);
    assert(!lifecycle.stop_requested());
    assert(lifecycle.begin_initialization());
    assert(lifecycle.state() == State::Initializing);
    assert(lifecycle.complete_initialization());
    assert(lifecycle.state() == State::Ready);
    assert(lifecycle.begin_running());
    assert(lifecycle.state() == State::Running);
    assert(lifecycle.request_stop());
    assert(lifecycle.request_stop());
    assert(lifecycle.state() == State::StopRequested);
    assert(lifecycle.stop_reason() == StopReason::External);
    assert(lifecycle.begin_stopping());
    assert(lifecycle.finish_cleanup());
    assert(lifecycle.state() == State::Stopped);
    assert(!lifecycle.request_stop());

    Lifecycle stop_before_init;
    assert(stop_before_init.request_stop());
    assert(stop_before_init.request_stop());
    assert(stop_before_init.state() == State::StopRequested);
    assert(stop_before_init.begin_stopping());
    assert(stop_before_init.finish_cleanup());
    assert(stop_before_init.state() == State::Stopped);

    Lifecycle init_failure;
    assert(init_failure.begin_initialization());
    assert(init_failure.mark_failure());
    assert(init_failure.state() == State::StopRequested);
    assert(init_failure.stop_reason() == StopReason::Fatal);
    assert(init_failure.begin_stopping());
    assert(init_failure.finish_cleanup());
    assert(init_failure.state() == State::Failed);
    assert(init_failure.failure());
    assert(!init_failure.request_stop());
    assert(init_failure.stop_reason() == StopReason::Fatal);

    Lifecycle external_then_fatal;
    assert(external_then_fatal.begin_initialization());
    assert(external_then_fatal.request_stop());
    assert(external_then_fatal.stop_reason() == StopReason::External);
    assert(external_then_fatal.mark_failure());
    assert(external_then_fatal.stop_reason() == StopReason::Fatal);
    assert(external_then_fatal.begin_stopping());
    assert(external_then_fatal.finish_cleanup());
    assert(external_then_fatal.state() == State::Failed);

    Lifecycle fatal_then_external;
    assert(fatal_then_external.begin_initialization());
    assert(fatal_then_external.mark_failure());
    assert(fatal_then_external.stop_reason() == StopReason::Fatal);
    assert(fatal_then_external.request_stop());
    assert(fatal_then_external.stop_reason() == StopReason::Fatal);
    assert(fatal_then_external.begin_stopping());
    assert(fatal_then_external.finish_cleanup());
    assert(fatal_then_external.state() == State::Failed);
    assert(!fatal_then_external.request_stop());
    assert(fatal_then_external.stop_reason() == StopReason::Fatal);

    Lifecycle repeated_failure;
    assert(repeated_failure.begin_initialization());
    assert(repeated_failure.mark_failure());
    assert(repeated_failure.mark_failure());
    assert(repeated_failure.begin_stopping());
    assert(repeated_failure.finish_cleanup());
    assert(repeated_failure.state() == State::Failed);

    for (int iteration = 0; iteration < 256; ++iteration) {
        Lifecycle concurrent;
        assert(concurrent.begin_initialization());
        std::barrier start_line(2);
        std::thread external([&]() {
            start_line.arrive_and_wait();
            (void)concurrent.request_stop();
        });
        std::thread fatal([&]() {
            start_line.arrive_and_wait();
            (void)concurrent.mark_failure();
        });
        external.join();
        fatal.join();

        assert(concurrent.failure());
        assert(concurrent.stop_reason() == StopReason::Fatal);
        assert(concurrent.state() == State::StopRequested);
        assert(concurrent.begin_stopping());
        assert(concurrent.finish_cleanup());
        assert(concurrent.state() == State::Failed);
    }

    return 0;
}
