#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace p4_nano_audio86_notifications {

inline constexpr UBaseType_t kAudio86ProducerNotificationIndex = 1U;
inline constexpr UBaseType_t kAudio86WorkerNotificationIndex = 0U;

static_assert(configTASK_NOTIFICATION_ARRAY_ENTRIES >
              kAudio86ProducerNotificationIndex);
static_assert(configTASK_NOTIFICATION_ARRAY_ENTRIES >
              kAudio86WorkerNotificationIndex);

inline BaseType_t notify_producer(TaskHandle_t producer)
{
    return xTaskNotifyGiveIndexed(producer, kAudio86ProducerNotificationIndex);
}

inline uint32_t wait_producer(TickType_t ticks_to_wait = portMAX_DELAY)
{
    return ulTaskNotifyTakeIndexed(kAudio86ProducerNotificationIndex, pdTRUE,
                                   ticks_to_wait);
}

inline BaseType_t notify_worker(TaskHandle_t worker)
{
    return xTaskNotifyGiveIndexed(worker, kAudio86WorkerNotificationIndex);
}

inline uint32_t wait_worker(TickType_t ticks_to_wait = portMAX_DELAY)
{
    return ulTaskNotifyTakeIndexed(kAudio86WorkerNotificationIndex, pdTRUE,
                                   ticks_to_wait);
}

} // namespace p4_nano_audio86_notifications
