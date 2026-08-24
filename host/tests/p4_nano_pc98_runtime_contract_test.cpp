#include <cassert>
#include <cstdint>

#include "p4_nano_pc98_runtime/runtime_contract.hpp"

int main()
{
    const auto production =
        p4_nano_pc98_runtime::production_media_config();
    const auto validation =
        p4_nano_pc98_runtime::validation_media_config();
    const auto hardware_validation =
        p4_nano_pc98_runtime::hardware_validation_media_config();
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
    assert(production.physical_path != validation.physical_path);
    assert(production.physical_path != hardware_validation.physical_path);
    assert(validation.physical_path != hardware_validation.physical_path);
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
    return 0;
}
