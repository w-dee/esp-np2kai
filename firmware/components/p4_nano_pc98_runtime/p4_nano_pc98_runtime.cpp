#include "p4_nano_pc98_runtime/p4_nano_pc98_runtime.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

extern "C" {
#include <compiler.h>
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
#include <cpumem.h>
#include <result_v1_parser.h>
#endif
#include <diskimage/fddfile.h>
#include <scrnmng.h>
}

#include "np2host/dosio_esp.h"
#include "np2runtime/np2runtime.hpp"
#include "p4_nano_live_display_session/session.hpp"
#include "p4_nano_pc98_runtime/runtime_contract.hpp"
#include "storage_fatfs/storage_fatfs.hpp"
#include "storage_sdmmc/storage_sdmmc.hpp"

namespace {

constexpr std::size_t kRuntimeStackBytes = 32768U;
constexpr UBaseType_t kRuntimePriority = tskIDLE_PRIORITY + 3U;
constexpr BaseType_t kRuntimeCore = 1;
constexpr TickType_t kStartupTimeoutTicks = pdMS_TO_TICKS(30000U) == 0U
                                                 ? 1U
                                                 : pdMS_TO_TICKS(30000U);
constexpr TickType_t kConsumerDelayTicks = 1U;
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
constexpr std::uintptr_t kResultPhysicalAddress = 0x29000U;
#endif

enum class GuestCompletion : std::uint8_t {
    Unknown,
    Pass,
    Fail,
};

struct Composition final {
    explicit Composition(bool validation_profile, bool emu_backend) noexcept
        : validation(validation_profile),
          emu(emu_backend),
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
          media(validation_profile
                    ? (emu_backend
                           ? p4_nano_pc98_runtime::validation_media_config()
                           : p4_nano_pc98_runtime::hardware_validation_media_config())
                    : p4_nano_pc98_runtime::production_media_config())
#else
          media(p4_nano_pc98_runtime::production_media_config())
#endif
    {
    }

    bool validation;
    bool emu;
    p4_nano_pc98_runtime::MediaConfig media;
    storage_sdmmc::SdmmcMountProvider sd_provider{};
    storage_fatfs::MountProvider persist_provider{};
    storage_fatfs::FatfsMountBackend *mount_backend = nullptr;
    p4_nano_live_display_session::Session session{};
    np2runtime::Runtime runtime{};
    SemaphoreHandle_t ready_semaphore = nullptr;
    SemaphoreHandle_t stopped_semaphore = nullptr;
    TaskHandle_t owner_task = nullptr;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> owner_done{false};
    std::atomic<GuestCompletion> guest_completion{GuestCompletion::Unknown};
    esp_err_t owner_result = ESP_FAIL;
    bool mounted = false;
    bool scrnmng_initialized = false;
    bool dosio_attached = false;
    bool fdd_attached = false;
    bool ready_signaled = false;
};

void emit(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fflush(stdout);
}

#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
void observe_guest_completion(Composition *composition) noexcept
{
    if (composition == nullptr || !composition->validation) {
        return;
    }

    /* The guest publishes CRC-protected result-v1 with state last.  A local
     * snapshot prevents the parser from observing a moving memory window;
     * the parser's CRC and reserved-field checks reject torn snapshots. */
    std::uint8_t snapshot[NP2_RESULT_V1_SIZE];
    std::memcpy(snapshot, mem + kResultPhysicalAddress, sizeof(snapshot));
    np2_result_v1_result parsed{};
    const np2_result_v1_observation observation = np2_result_v1_parse(
        snapshot, sizeof(snapshot), &parsed);
    GuestCompletion completion = GuestCompletion::Unknown;
    if (observation == NP2_RESULT_V1_PASS) {
        completion = GuestCompletion::Pass;
    } else if (observation == NP2_RESULT_V1_FAIL) {
        completion = GuestCompletion::Fail;
    }
    if (completion == GuestCompletion::Unknown) {
        return;
    }
    if (composition->guest_completion.exchange(completion,
                                               std::memory_order_acq_rel) ==
        completion) {
        return;
    }
    emit("P4_NANO_RUNTIME_GUEST_COMPLETION=%s completed=%u passed=%u "
         "failed=%u\n",
         completion == GuestCompletion::Pass ? "PASS" : "FAIL",
         static_cast<unsigned>(parsed.completed_count),
         static_cast<unsigned>(parsed.passed_count),
         static_cast<unsigned>(parsed.failed_count));
}
#endif

bool stop_observer(void *context) noexcept
{
    auto *composition = static_cast<Composition *>(context);
    if (composition == nullptr) {
        return false;
    }
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
    observe_guest_completion(composition);
    const GuestCompletion guest_completion =
        composition->guest_completion.load(std::memory_order_acquire);
    const bool guest_terminal =
        guest_completion != GuestCompletion::Unknown;
#else
    constexpr bool guest_terminal = false;
#endif
    if (!composition->stop_requested.load(std::memory_order_acquire) &&
        !guest_terminal) {
        return false;
    }
    /* Runtime calls this on the owner task immediately before its next
     * pccore_exec().  Detaching here is the synchronization boundary that
     * prevents a new scrnmng publish from entering Session after stop.  The
     * FDD remains attached until Runtime::run() returns and pccore_term()
     * has completed; the composition then ejects it before DOSIO/unmount. */
    composition->session.detach_source();
    (void)composition->runtime.request_stop();
    return true;
}

void signal_ready(Composition *composition, esp_err_t result) noexcept
{
    if (composition == nullptr || composition->ready_signaled) {
        return;
    }
    composition->owner_result = result;
    composition->ready_signaled = true;
    if (composition->ready_semaphore != nullptr) {
        (void)xSemaphoreGive(composition->ready_semaphore);
    }
}

void cleanup_after_owner_join(Composition *composition) noexcept
{
    if (composition == nullptr) {
        return;
    }

    /* Runtime's observer normally detaches the hook before stopping.  This
     * fallback covers initialization/fatal paths and is also the first step
     * after the owner task has joined. */
    composition->session.detach_source();

    /* The owner task has joined, and Runtime::run() has already returned, so
     * pccore_term() no longer uses the drive. */
    if (composition->fdd_attached) {
        (void)fdd_eject(composition->media.fdd_unit);
        composition->fdd_attached = false;
    }
    if (composition->scrnmng_initialized) {
        scrnmng_shutdown();
        composition->scrnmng_initialized = false;
    }
    if (composition->dosio_attached) {
        np2_dosio_detach_vfs_file();
        composition->dosio_attached = false;
    }
}

void owner_task(void *context)
{
    auto *composition = static_cast<Composition *>(context);
    if (composition == nullptr) {
        vTaskDelete(nullptr);
        return;
    }

    const np2runtime::Result runtime_init =
        composition->runtime.initialize(
            p4_nano_pc98_runtime::kFdd0OnlyEquipment);
    if (runtime_init != np2runtime::Result::Ok) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=RUNTIME_INIT_FAILED\n");
        signal_ready(composition, ESP_FAIL);
        composition->owner_result = ESP_FAIL;
        goto done;
    }

    if (!scrnmng_initialize()) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=SCRNMNG_INIT_FAILED\n");
        signal_ready(composition, ESP_FAIL);
        (void)composition->runtime.request_stop();
        (void)composition->runtime.run();
        composition->owner_result = ESP_FAIL;
        goto done;
    }
    composition->scrnmng_initialized = true;

    if (composition->session.attach_source() != ESP_OK) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=DISPLAY_PUBLISH_FAILED\n");
        signal_ready(composition, ESP_FAIL);
        (void)composition->runtime.request_stop();
        (void)composition->runtime.run();
        composition->owner_result = ESP_FAIL;
        goto done;
    }

    if (!np2_dosio_attach_vfs_file(composition->media.logical_path.data(),
                                   composition->media.physical_path.data())) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=DOSIO_MAP_FAILED\n");
        signal_ready(composition, ESP_FAIL);
        (void)composition->runtime.request_stop();
        (void)composition->runtime.run();
        composition->owner_result = ESP_FAIL;
        goto done;
    }
    composition->dosio_attached = true;
    np2_dosio_stats_reset();
    emit("P4_NANO_RUNTIME_DOSIO=READY logical=%s physical=%s\n",
         composition->media.logical_path.data(),
         composition->media.physical_path.data());

    if (fdd_set(composition->media.fdd_unit,
                composition->media.logical_path.data(), FTYPE_NONE, 1) !=
            SUCCESS ||
        !fdd_diskready(composition->media.fdd_unit)) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=FDD_ATTACH_FAILED\n");
        signal_ready(composition, ESP_FAIL);
        (void)composition->runtime.request_stop();
        (void)composition->runtime.run();
        composition->owner_result = ESP_FAIL;
        goto done;
    }
    composition->fdd_attached = true;
    emit("P4_NANO_RUNTIME_FDD0=ATTACHED type=autodetect readonly=1 fddequip=0x%02x\n",
         p4_nano_pc98_runtime::kFdd0OnlyEquipment);

    signal_ready(composition, ESP_OK);
    emit("P4_NANO_RUNTIME_CORE=RUNNING\n");
    if (!composition->validation) {
        emit("P4_NANO_RUNTIME_RESULT=RUNNING\n");
    }
    (void)composition->runtime.run(stop_observer, composition);
    if (composition->runtime.failure()) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=RUNTIME_FATAL\n");
        composition->owner_result = ESP_FAIL;
    } else {
        composition->owner_result = ESP_OK;
    }

done:
    composition->owner_done.store(true, std::memory_order_release);
    if (composition->stopped_semaphore != nullptr) {
        (void)xSemaphoreGive(composition->stopped_semaphore);
    }
    /* The caller joins this task after the stopped semaphore; blocking here
     * keeps the task alive without a spin until vTaskDelete() is issued. */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelete(nullptr);
}

bool media_exists(const p4_nano_pc98_runtime::MediaConfig &media) noexcept
{
    struct stat status{};
    return stat(media.physical_path.data(), &status) == 0 &&
           S_ISREG(status.st_mode) && status.st_size > 0;
}

void destroy_sync(Composition *composition) noexcept
{
    if (composition == nullptr) {
        return;
    }
    if (composition->ready_semaphore != nullptr) {
        vSemaphoreDelete(composition->ready_semaphore);
        composition->ready_semaphore = nullptr;
    }
    if (composition->stopped_semaphore != nullptr) {
        vSemaphoreDelete(composition->stopped_semaphore);
        composition->stopped_semaphore = nullptr;
    }
}

esp_err_t run_composition(bool validation_profile, bool emu_backend) noexcept
{
    Composition composition(validation_profile, emu_backend);
    const auto &media = composition.media;

#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
    if (validation_profile && !emu_backend) {
        if (composition.sd_provider.mount() != ESP_OK) {
            emit("P4_NANO_RUNTIME_SD_MOUNT=FAIL reason=SD_MOUNT_FAILED\n");
            return ESP_FAIL;
        }
        composition.mount_backend = &composition.sd_provider;
    } else if (validation_profile) {
        if (composition.persist_provider.mount() != ESP_OK) {
            emit("P4_NANO_RUNTIME_RESULT=FAIL reason=SPI_NOR_MOUNT_FAILED\n");
            return ESP_FAIL;
        }
        composition.mount_backend = &composition.persist_provider;
    } else
#else
    (void)validation_profile;
    (void)emu_backend;
#endif
    {
        if (composition.sd_provider.mount() != ESP_OK) {
            emit("P4_NANO_RUNTIME_SD_MOUNT=FAIL reason=SD_MOUNT_FAILED\n");
            return ESP_FAIL;
        }
        composition.mount_backend = &composition.sd_provider;
    }
    composition.mounted = true;
    emit("P4_NANO_RUNTIME_SD_MOUNT=PASS mount=%s\n",
         composition.mount_backend == &composition.persist_provider
             ? storage_fatfs::kMountPath
             : storage_sdmmc::kMountPath);

    if (!media_exists(media)) {
        emit("P4_NANO_RUNTIME_MEDIA result=NOT_FOUND path=%s\n",
             media.physical_path.data());
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=BOOT_MEDIA_NOT_FOUND\n");
        (void)composition.mount_backend->unmount();
        composition.mounted = false;
        return ESP_ERR_NOT_FOUND;
    }
    emit("P4_NANO_RUNTIME_MEDIA result=FOUND path=%s\n",
         media.physical_path.data());

    if (composition.session.initialize() != ESP_OK ||
        !composition.session.native_framebuffer_external()) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=DISPLAY_INIT_FAILED\n");
        (void)composition.session.shutdown();
        (void)composition.mount_backend->unmount();
        composition.mounted = false;
        return ESP_FAIL;
    }
    composition.ready_semaphore = xSemaphoreCreateBinary();
    composition.stopped_semaphore = xSemaphoreCreateBinary();
    if (composition.ready_semaphore == nullptr ||
        composition.stopped_semaphore == nullptr) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=TASK_START_FAILED\n");
        destroy_sync(&composition);
        (void)composition.session.shutdown();
        (void)composition.mount_backend->unmount();
        composition.mounted = false;
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t task_result = xTaskCreatePinnedToCore(
        owner_task, "p4_nano_pc98", kRuntimeStackBytes, &composition,
        kRuntimePriority, &composition.owner_task, kRuntimeCore);
    if (task_result != pdPASS) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=TASK_START_FAILED\n");
        destroy_sync(&composition);
        (void)composition.session.shutdown();
        (void)composition.mount_backend->unmount();
        composition.mounted = false;
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(composition.ready_semaphore,
                       kStartupTimeoutTicks) != pdTRUE) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=TASK_START_TIMEOUT\n");
        composition.stop_requested.store(true, std::memory_order_release);
    }

    bool visible_reported = false;
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
    const std::int64_t validation_deadline =
        esp_timer_get_time() + 30LL * 1000LL * 1000LL;
#endif
    while (!composition.owner_done.load(std::memory_order_acquire)) {
        const auto consume = composition.session.consume_one();
        if (consume == p4_nano_live_display_session::ConsumeResult::Failed) {
            emit("P4_NANO_RUNTIME_RESULT=FAIL reason=DISPLAY_CONSUME_FAILED\n");
            composition.stop_requested.store(true, std::memory_order_release);
        }
        if (!visible_reported && composition.session.visible()) {
            visible_reported = true;
            emit("P4_NANO_RUNTIME_DISPLAY=VISIBLE\n");
        }

        np2_dosio_stats stats{};
        np2_dosio_stats_get(&stats);
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
        if (validation_profile && esp_timer_get_time() >= validation_deadline) {
            emit("P4_NANO_RUNTIME_RESULT=FAIL reason=VALIDATION_TIMEOUT\n");
            composition.stop_requested.store(true, std::memory_order_release);
        }
#endif
        vTaskDelay(kConsumerDelayTicks);
    }

    if (composition.stopped_semaphore != nullptr) {
        (void)xSemaphoreTake(composition.stopped_semaphore,
                             kStartupTimeoutTicks);
    }
    if (composition.owner_task != nullptr) {
        vTaskDelete(composition.owner_task);
        composition.owner_task = nullptr;
    }

    cleanup_after_owner_join(&composition);

    np2_dosio_stats stats{};
    np2_dosio_stats_get(&stats);
    const auto &counters = composition.session.counters();
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
    const bool validation_pass =
        validation_profile &&
        composition.guest_completion.load(std::memory_order_acquire) ==
            GuestCompletion::Pass &&
        !composition.session.failed() &&
        stats.read_bytes > 0U && counters.submitted > 0U &&
        counters.transformed > 0U && counters.released > 0U;
#endif

    (void)composition.session.shutdown();
    if (composition.mounted && composition.mount_backend != nullptr) {
        (void)composition.mount_backend->unmount();
        composition.mounted = false;
    }
    emit("P4_NANO_RUNTIME_DISK_READS opens=%" PRIu64 " calls=%" PRIu64
         " bytes=%" PRIu64 "\n",
         stats.open_count, stats.read_calls, stats.read_bytes);
    emit("P4_NANO_RUNTIME_SESSION submitted=%" PRIu32 " acquired=%" PRIu32
         " transformed=%" PRIu32 " released=%" PRIu32 " dropped=%" PRIu32
         "\n",
         counters.submitted, counters.acquired, counters.transformed,
         counters.released, counters.dropped);
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
    if (validation_profile) {
        emit("P4_NANO_RUNTIME_VALIDATION_RESULT=%s\n",
             validation_pass ? "PASS" : "FAIL");
    }
#endif
    destroy_sync(&composition);
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
    return validation_profile ? (validation_pass ? ESP_OK : ESP_FAIL)
                              : composition.owner_result;
#else
    return composition.owner_result;
#endif
}

} // namespace

namespace p4_nano_pc98_runtime {

esp_err_t run_production() noexcept
{
    return run_composition(false, false);
}

esp_err_t run_validation() noexcept
{
    return run_composition(true,
#if defined(P4_NANO_RUNTIME_EMU_BACKEND)
                           true
#else
                           false
#endif
    );
}

} // namespace p4_nano_pc98_runtime
