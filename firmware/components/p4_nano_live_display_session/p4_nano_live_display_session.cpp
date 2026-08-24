/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_live_display_session/session.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

#include "p4_nano_board/p4_nano_board.hpp"
#include "p4_nano_display/p4_nano_display_transform.hpp"
#include "scrnmng.h"

namespace {

bool ranges_overlap(const void *first, std::size_t first_size,
                    const void *second, std::size_t second_size) noexcept
{
    const auto first_start = reinterpret_cast<std::uintptr_t>(first);
    const auto second_start = reinterpret_cast<std::uintptr_t>(second);
    if (first_start > UINTPTR_MAX - first_size ||
        second_start > UINTPTR_MAX - second_size) {
        return true;
    }
    return first_start < second_start + second_size &&
           second_start < first_start + first_size;
}

} // namespace

namespace p4_nano_live_display_session {

class Session::LeaseGuard final {
public:
    LeaseGuard(Session *session, np2_presentation_token *token) noexcept
        : session_(session), token_(token)
    {
    }

    LeaseGuard(const LeaseGuard &) = delete;
    LeaseGuard &operator=(const LeaseGuard &) = delete;

    ~LeaseGuard()
    {
        if (active_) {
            (void)session_->release(token_);
        }
    }

    bool release_now() noexcept
    {
        if (!active_) {
            return true;
        }
        active_ = false;
        return session_->release(token_);
    }

private:
    Session *session_;
    np2_presentation_token *token_;
    bool active_ = true;
};

Session::~Session()
{
    (void)shutdown();
}

void Session::fail(const esp_err_t error) noexcept
{
    failed_.store(true, std::memory_order_release);
    if (last_error_ == ESP_OK) {
        last_error_ = error;
    }
}

void Session::free_slots() noexcept
{
    for (auto &slot : slots_) {
        if (slot.ptr != nullptr) {
            heap_caps_free(slot.ptr);
            slot.ptr = nullptr;
        }
        slot.capacity = 0U;
    }
}

bool Session::native_framebuffer_external() const noexcept
{
    return display_.framebuffer != nullptr &&
           esp_ptr_external_ram(display_.framebuffer);
}

void Session::refresh_publisher_counters() noexcept
{
    counters_.coalesced = np2_presentation_coalesced_count(&publisher_);
    counters_.dropped = np2_presentation_dropped_count(&publisher_);
}

esp_err_t Session::initialize() noexcept
{
    if (initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    failed_.store(false, std::memory_order_release);
    last_error_ = ESP_OK;
    counters_ = {};
    last_frame_ = {};
    first_frame_.reset();
    outstanding_lease_ = false;
    outstanding_token_ = {};

    for (auto &slot : slots_) {
        slot.ptr = static_cast<std::uint8_t *>(heap_caps_calloc(
            1U, kPresentationSlotBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        slot.capacity = kPresentationSlotBytes;
        if (slot.ptr == nullptr) {
            free_slots();
            fail(ESP_ERR_NO_MEM);
            return ESP_ERR_NO_MEM;
        }
    }
    if (!esp_ptr_external_ram(slots_[0].ptr) ||
        !esp_ptr_external_ram(slots_[1].ptr) ||
        ranges_overlap(slots_[0].ptr, kPresentationSlotBytes,
                       slots_[1].ptr, kPresentationSlotBytes)) {
        free_slots();
        fail(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }

    if (np2_presentation_init(&publisher_, slots_) != NP2_PRESENTATION_OK) {
        free_slots();
        fail(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = p4_nano_display::display_session_initialize(&display_);
    if (result != ESP_OK) {
        free_slots();
        fail(result);
        return result;
    }
    if (ranges_overlap(slots_[0].ptr, kPresentationSlotBytes,
                       display_.framebuffer,
                       p4_nano_display::kNativeFramebufferBytes) ||
        ranges_overlap(slots_[1].ptr, kPresentationSlotBytes,
                       display_.framebuffer,
                       p4_nano_display::kNativeFramebufferBytes)) {
        (void)p4_nano_display::display_session_cleanup(&display_);
        free_slots();
        fail(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }

    initialized_ = true;
    return ESP_OK;
}

esp_err_t Session::attach_source() noexcept
{
    if (!initialized_ || failed()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!hook_registered_) {
        scrnmng_set_publish_hook(&Session::publish_hook, this);
        hook_registered_ = true;
    }
    return ESP_OK;
}

void Session::detach_source() noexcept
{
    if (!hook_registered_) {
        return;
    }
    scrnmng_set_publish_hook(nullptr, nullptr);
    hook_registered_ = false;
}

void Session::publish_hook(const SCRNMNG_PUBLISH_VIEW *view, void *context)
{
    auto *session = static_cast<Session *>(context);
    if (session == nullptr || view == nullptr || !session->initialized_) {
        return;
    }

    const np2_presentation_source_view source{
        .ptr = view->ptr,
        .width = static_cast<std::uint32_t>(view->width),
        .height = static_cast<std::uint32_t>(view->height),
        .pitch = view->pitch,
        .bpp = view->bpp,
        .pixel_format = static_cast<np2_presentation_pixel_format>(
            view->pixel_format),
        .source_generation = view->surface_generation,
        .source_update_sequence = view->surface_update_sequence,
    };
    const np2_presentation_status status = np2_presentation_submit(
        &session->publisher_, &source);
    if (status == NP2_PRESENTATION_OK) {
        ++session->counters_.submitted;
    } else if (status != NP2_PRESENTATION_DROPPED) {
        session->failed_.store(true, std::memory_order_release);
    }
}

bool Session::validate_frame(
    const np2_presentation_frame_view &view) const noexcept
{
    return view.ptr != nullptr &&
           source_geometry_valid(view.width, view.height, view.pitch,
                                 view.bpp,
                                 static_cast<std::uint32_t>(view.pixel_format));
}

bool Session::release(np2_presentation_token *token) noexcept
{
    if (token == nullptr || token->lease == 0U) {
        return true;
    }
    const np2_presentation_status status = np2_presentation_release(
        &publisher_, token);
    if (status != NP2_PRESENTATION_OK) {
        failed_.store(true, std::memory_order_release);
        if (last_error_ == ESP_OK) {
            last_error_ = ESP_ERR_INVALID_STATE;
        }
        return false;
    }
    ++counters_.released;
    outstanding_lease_ = false;
    return true;
}

ConsumeResult Session::consume_one(FrameObserver observer,
                                   void *observer_context) noexcept
{
    if (!initialized_) {
        return ConsumeResult::Failed;
    }
    refresh_publisher_counters();
    if (failed()) {
        return ConsumeResult::Failed;
    }

    np2_presentation_frame_view view{};
    outstanding_token_ = {};
    const np2_presentation_status acquire = np2_presentation_acquire(
        &publisher_, &view, &outstanding_token_);
    if (acquire == NP2_PRESENTATION_NO_FRAME) {
        return ConsumeResult::NoFrame;
    }
    if (acquire != NP2_PRESENTATION_OK) {
        fail(ESP_ERR_INVALID_STATE);
        return ConsumeResult::Failed;
    }
    outstanding_lease_ = true;
    ++counters_.acquired;
    LeaseGuard lease(this, &outstanding_token_);

    if (!validate_frame(view)) {
        fail(ESP_ERR_INVALID_SIZE);
        return ConsumeResult::Failed;
    }
    if (observer != nullptr) {
        const FrameObservation observation{
            .stage = FrameStage::Acquired,
            .source = view,
            .native_framebuffer = nullptr,
            .native_framebuffer_bytes = 0U,
        };
        if (!observer(observation, observer_context)) {
            fail(ESP_ERR_INVALID_SIZE);
            return ConsumeResult::Failed;
        }
    }

    const auto source = std::span<const std::uint16_t>(
        reinterpret_cast<const std::uint16_t *>(view.ptr),
        p4_nano_display::kTransformSourcePixelCount);
    const auto destination = std::span<std::uint16_t>(
        display_.framebuffer,
        p4_nano_display::kTransformDestinationPixelCount);
    if (!p4_nano_display::transform_to_native(
            source, destination,
            p4_nano_display::QuarterTurn::CounterClockwise)) {
        fail(ESP_ERR_INVALID_SIZE);
        return ConsumeResult::Failed;
    }
    if (p4_nano_display::display_session_sync_framebuffer(&display_) !=
        ESP_OK) {
        fail(ESP_ERR_INVALID_STATE);
        return ConsumeResult::Failed;
    }
    if (observer != nullptr) {
        const FrameObservation observation{
            .stage = FrameStage::Transformed,
            .source = view,
            .native_framebuffer = display_.framebuffer,
            .native_framebuffer_bytes = kNativeFramebufferBytes,
        };
        if (!observer(observation, observer_context)) {
            fail(ESP_ERR_INVALID_SIZE);
            return ConsumeResult::Failed;
        }
    }

    ++counters_.transformed;
    last_frame_.source_generation = view.source_generation;
    last_frame_.source_update_sequence = view.source_update_sequence;
    last_frame_.published_sequence = view.published_sequence;
    if (!lease.release_now()) {
        return ConsumeResult::Failed;
    }

    if (!first_frame_.visible) {
#if defined(P4_NANO_RUNTIME_EMU_BACKEND)
        (void)first_frame_.mark_valid_frame();
#else
        if (p4_nano_board::display_backlight_set(
                p4_nano_board::kBacklightConservative) != ESP_OK) {
            fail(ESP_ERR_INVALID_STATE);
            return ConsumeResult::Failed;
        }
        (void)first_frame_.mark_valid_frame();
#endif
    }
    return ConsumeResult::Consumed;
}

esp_err_t Session::shutdown() noexcept
{
    detach_source();
    esp_err_t first_error = ESP_OK;
    if (outstanding_lease_) {
        if (!release(&outstanding_token_) && first_error == ESP_OK) {
            first_error = ESP_ERR_INVALID_STATE;
        }
    }
    if (initialized_) {
        const esp_err_t display_error =
            p4_nano_display::display_session_cleanup(&display_);
        if (display_error != ESP_OK && first_error == ESP_OK) {
            first_error = display_error;
        }
    }
    free_slots();
    initialized_ = false;
    publisher_.initialized = false;
    first_frame_.reset();
    return first_error;
}

} // namespace p4_nano_live_display_session
