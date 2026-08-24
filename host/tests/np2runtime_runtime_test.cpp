#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>

#include "np2runtime/np2runtime.hpp"

extern "C" {
#include <compiler.h>
#include <pccore.h>
}

int main()
{
    pccore_setdefault();
    np2cfg.EXTMEM = 99U;
    np2cfg.fddfile[0][0] = 'X';
    np2cfg.fddfile[0][1] = '\0';

    np2runtime::Runtime stop_before_init;
    assert(stop_before_init.request_stop());
    assert(stop_before_init.request_stop());
    assert(stop_before_init.initialize() == np2runtime::Result::Stopped);
    assert(stop_before_init.state() == np2runtime::State::Stopped);
    assert(stop_before_init.stop_reason() == np2runtime::StopReason::External);
    assert(!stop_before_init.failure());
    assert(np2cfg.EXTMEM == 99U);
    assert(np2cfg.fddfile[0][0] == 'X');
    assert(stop_before_init.initialize() == np2runtime::Result::InvalidState);
    assert(stop_before_init.run() == np2runtime::Result::InvalidState);

    pccore_setdefault();
    const auto default_fddequip = np2cfg.fddequip;

    np2runtime::Runtime runtime;
    assert(runtime.initialize() == np2runtime::Result::Ok);
    assert(runtime.state() == np2runtime::State::Ready);
    assert(std::strcmp(np2cfg.model, "VX") == 0);
    assert(np2cfg.baseclock == PCBASECLOCK25);
    assert(np2cfg.multiple == 20U);
    const UINT8 expected_dipsw[3] = {0x3e, 0xe3, 0x7b};
    const UINT8 expected_memsw[8] = {0x48, 0x05, 0x04, 0x08,
                                     0x01, 0x00, 0x00, 0x6e};
    const UINT8 expected_wait[6] = {1U, 1U, 6U, 1U, 8U, 1U};
    for (int index = 0; index < 3; ++index) {
        assert(np2cfg.dipsw[index] == expected_dipsw[index]);
    }
    for (int index = 0; index < 8; ++index) {
        assert(np2cfg.memsw[index] == expected_memsw[index]);
    }
    assert(np2cfg.EXTMEM == 8U);
    assert(np2cfg.fddequip == default_fddequip);
    assert(np2cfg.memcheckspeed == 8U);
    assert(np2cfg.ITF_WORK == 1U);
    assert(np2cfg.emuspeed == 100U);
    assert(np2cfg.DISPSYNC == 1U);
    for (int index = 0; index < 6; ++index) {
        assert(np2cfg.wait[index] == expected_wait[index]);
    }
    assert(np2cfg.usebios == 0U);
    assert(np2cfg.biospath[0] == '\0');
    assert(np2cfg.fontfile[0] == '\0');
    assert(np2cfg.fontface[0] == '\0');
    assert(np2cfg.SOUND_SW == 0U);
    assert(np2cfg.MOTOR == 0U);
    assert(np2cfg.MOTORVOL == 0U);
    assert(np2cfg.mpuenable == 0U);
    assert(np2cfg.pc9861enable == 0U);
    assert(np2cfg.hdrvenable == 0U);
    assert(np2cfg.hdrvntenable == 0U);
    assert(np2cfg.usefd144 == 0U);
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
