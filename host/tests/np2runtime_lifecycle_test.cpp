#include <cassert>

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

    Lifecycle repeated_failure;
    assert(repeated_failure.begin_initialization());
    assert(repeated_failure.mark_failure());
    assert(repeated_failure.mark_failure());
    assert(repeated_failure.begin_stopping());
    assert(repeated_failure.finish_cleanup());
    assert(repeated_failure.state() == State::Failed);

    return 0;
}
