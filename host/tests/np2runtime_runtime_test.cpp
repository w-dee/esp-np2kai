#include <cassert>
#include <chrono>
#include <thread>

#include "np2runtime/np2runtime.hpp"

extern "C" {
#include <compiler.h>
#include <pccore.h>
}

int main()
{
    np2runtime::Runtime runtime;
    assert(runtime.initialize() == np2runtime::Result::Ok);
    assert(runtime.state() == np2runtime::State::Ready);
    assert(np2cfg.EXTMEM == 8U);
    for (const auto &path : np2cfg.fddfile) {
        assert(path[0] == '\0');
    }
    for (const auto &path : np2cfg.sasihdd) {
        assert(path[0] == '\0');
    }

    np2runtime::Result run_result = np2runtime::Result::InvalidState;
    std::thread worker([&runtime, &run_result]() {
        run_result = runtime.run();
    });

    bool running = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (runtime.state() == np2runtime::State::Running) {
            running = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(running);
    assert(runtime.request_stop());
    const bool repeated_stop = runtime.request_stop();
    assert(repeated_stop || runtime.state() == np2runtime::State::Stopping ||
           runtime.state() == np2runtime::State::Stopped);
    worker.join();

    assert(run_result == np2runtime::Result::Stopped);
    assert(runtime.state() == np2runtime::State::Stopped);
    assert(!runtime.failure());
    return 0;
}
