#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>

#include "p4_nano_pc98_runtime/audio86_outer_lifecycle.hpp"
#include "p4_nano_pc98_runtime/runtime_contract.hpp"

int main()
{
    const auto production =
        p4_nano_pc98_runtime::production_media_config();
    const auto validation =
        p4_nano_pc98_runtime::validation_media_config();
    const auto hardware_validation =
        p4_nano_pc98_runtime::hardware_validation_media_config();
    const auto keyboard_validation =
        p4_nano_pc98_runtime::keyboard_validation_media_config();
    const auto hardware_keyboard_validation =
        p4_nano_pc98_runtime::hardware_keyboard_validation_media_config();
    assert(production.physical_path == "/sdcard/files/pc98/fdd/boot.hdm");
    assert(production.logical_path == "./runtime-fdd0.hdm");
    assert(production.fdd_unit == 0U);
    assert(validation.physical_path == "/persist/fixtures/np2test-fd1232.hdm");
    assert(validation.logical_path == "./runtime-validation-fdd0.hdm");
    assert(validation.fdd_unit == 0U);
    assert(hardware_validation.physical_path ==
           "/sdcard/files/pc98/fdd/runtime-validation.hdm");
    assert(hardware_validation.logical_path ==
           "./runtime-validation-fdd0.hdm");
    assert(hardware_validation.fdd_unit == 0U);
    assert(keyboard_validation.physical_path ==
           "/persist/fixtures/np2kbdtest-fd1232.hdm");
    assert(keyboard_validation.logical_path ==
           "./runtime-keyboard-validation-fdd0.hdm");
    assert(keyboard_validation.fdd_unit == 0U);
    assert(hardware_keyboard_validation.physical_path ==
           "/sdcard/files/pc98/fdd/runtime-keyboard-validation.hdm");
    assert(hardware_keyboard_validation.logical_path ==
           "./runtime-keyboard-validation-fdd0.hdm");
    assert(hardware_keyboard_validation.fdd_unit == 0U);
    assert(production.physical_path != validation.physical_path);
    assert(production.physical_path != hardware_validation.physical_path);
    assert(validation.physical_path != hardware_validation.physical_path);
    assert(keyboard_validation.physical_path != validation.physical_path);
    assert(hardware_keyboard_validation.physical_path !=
           hardware_validation.physical_path);
    assert(p4_nano_pc98_runtime::kFdd0OnlyEquipment == 1U);

    using p4_nano_pc98_runtime::CleanupStage;
    assert(p4_nano_pc98_runtime::cleanup_precedes(
        CleanupStage::SourceDetached, CleanupStage::StopRequested));
    assert(p4_nano_pc98_runtime::cleanup_precedes(
        CleanupStage::StopRequested, CleanupStage::CoreTerminated));
    assert(p4_nano_pc98_runtime::cleanup_precedes(
        CleanupStage::CoreTerminated, CleanupStage::OwnerJoined));
    assert(p4_nano_pc98_runtime::cleanup_precedes(
        CleanupStage::OwnerJoined, CleanupStage::LeasesReleased));
    assert(p4_nano_pc98_runtime::cleanup_precedes(
        CleanupStage::LeasesReleased, CleanupStage::FddEjected));
    assert(p4_nano_pc98_runtime::cleanup_precedes(
        CleanupStage::FddEjected, CleanupStage::ScrnmngShutdown));
    assert(p4_nano_pc98_runtime::cleanup_precedes(
        CleanupStage::OwnerJoined, CleanupStage::FddEjected));
    assert(p4_nano_pc98_runtime::cleanup_precedes(
        CleanupStage::CoreTerminated, CleanupStage::ScrnmngShutdown));
    assert(p4_nano_pc98_runtime::cleanup_precedes(
        CleanupStage::ScrnmngShutdown, CleanupStage::DosioReset));
    assert(p4_nano_pc98_runtime::cleanup_precedes(
        CleanupStage::DosioReset, CleanupStage::SessionShutdown));
    assert(p4_nano_pc98_runtime::cleanup_precedes(
        CleanupStage::SessionShutdown, CleanupStage::StorageUnmounted));

    using namespace p4_nano_pc98_runtime::audio86_outer;
    Lifecycle lifecycle;
    lifecycle.reset(-1);
    lifecycle.begin_ready_wait();
    assert(lifecycle.publish_startup(0, true));
    lifecycle.begin_completion_wait();
    assert(lifecycle.publish_completion(0, true));
    assert(lifecycle.completion_result.load() == 0);
    assert(lifecycle.owner_delete_allowed(true, true));

    lifecycle.reset(-1);
    assert(lifecycle.publish_startup(0, true));
    assert(lifecycle.publish_completion(-7, false));
    assert(lifecycle.completion_result.load() == -7);
    assert(lifecycle.owner_delete_allowed(true, true));

    lifecycle.reset(-1);
    assert(lifecycle.publish_startup(0, true));
    assert(lifecycle.mark_completion_timeout(-2));
    assert(lifecycle.completion_result.load() == -2);
    assert(!lifecycle.publish_completion(0, true));
    assert(!lifecycle.owner_delete_allowed(false, false));
    assert(!lifecycle.owner_delete_allowed(true, true));

    lifecycle.reset(-1);
    assert(lifecycle.mark_startup_timeout(-2));
    assert(lifecycle.startup_result.load() == -2);
    assert(!lifecycle.publish_startup(0, true));

    lifecycle.reset(-1);
    assert(lifecycle.publish_startup(-7, false));
    assert(lifecycle.startup_result.load() == -7);
    assert(lifecycle.completion_result.load() == -1);

    for (const TerminalOwner inner : {TerminalOwner::InnerComplete,
                                      TerminalOwner::InnerFailed}) {
        lifecycle.reset(-1);
        std::atomic<std::uint32_t> wins{0U};
        std::vector<std::thread> contenders;
        contenders.emplace_back([&] {
            if (lifecycle.claim_terminal(inner))
                wins.fetch_add(1U);
        });
        contenders.emplace_back([&] {
            if (lifecycle.claim_terminal(TerminalOwner::CompletionTimeout))
                wins.fetch_add(1U);
        });
        for (auto &thread : contenders)
            thread.join();
        assert(wins.load() == 1U);
        assert(lifecycle.terminal_owner.load() !=
               static_cast<std::uint32_t>(TerminalOwner::None));
        assert(!lifecycle.claim_terminal(TerminalOwner::ReadyTimeout));
    }

    lifecycle.reset(-1);
    assert(lifecycle.claim_terminal(TerminalOwner::CompletionTimeout));
    assert(!lifecycle.claim_terminal(TerminalOwner::InnerComplete));
    assert(!lifecycle.claim_terminal(TerminalOwner::InnerFailed));
    assert(lifecycle.claim_timeout_snapshot());
    assert(!lifecycle.claim_timeout_snapshot());
    return 0;
}
