#include "np2opngen_synthetic_workload.h"

#include <limits.h>
#include <string.h>

#define SYNTHETIC_SECONDS_SLOTS 200U
#define SYNTHETIC_BOOTSTRAP_CONFIG_EVENTS 84U
#define SYNTHETIC_BOOTSTRAP_KEY_EVENTS 3U
#define SYNTHETIC_BOOTSTRAP_EVENTS \
    (SYNTHETIC_BOOTSTRAP_CONFIG_EVENTS + SYNTHETIC_BOOTSTRAP_KEY_EVENTS)

static const uint16_t operator_register_groups[6] = {
    0x30U, 0x40U, 0x50U, 0x60U, 0x70U, 0x80U,
};

static int profile_rate(enum np2opngen_synthetic_profile profile)
{
    switch (profile) {
    case NP2_OPNGEN_SYNTHETIC_LIGHT:
        return 69;
    case NP2_OPNGEN_SYNTHETIC_HEAVY:
        return 237;
    case NP2_OPNGEN_SYNTHETIC_STRESS:
        return 684;
    }
    return -1;
}

const char *np2opngen_synthetic_profile_name(
    enum np2opngen_synthetic_profile profile)
{
    switch (profile) {
    case NP2_OPNGEN_SYNTHETIC_LIGHT:
        return "SYNTHETIC-LIGHT";
    case NP2_OPNGEN_SYNTHETIC_HEAVY:
        return "SYNTHETIC-HEAVY";
    case NP2_OPNGEN_SYNTHETIC_STRESS:
        return "STRESS";
    }
    return "INVALID";
}

uint64_t np2opngen_synthetic_workload_expected_events(
    enum np2opngen_synthetic_profile profile, uint32_t duration_seconds)
{
    const int rate = profile_rate(profile);
    if (rate < 0 || duration_seconds >
                         (UINT64_MAX - SYNTHETIC_BOOTSTRAP_EVENTS) /
                             (uint64_t)rate) {
        return UINT64_MAX;
    }
    return SYNTHETIC_BOOTSTRAP_EVENTS + (uint64_t)rate * duration_seconds;
}

static void set_register_event(struct np2opngen_synth_event *event,
                               uint64_t timestamp, uint64_t sequence,
                               uint16_t reg, uint8_t value)
{
    *event = (struct np2opngen_synth_event){
        timestamp,
        sequence,
        NP2_SYNTH_EVENT_REGISTER_WRITE,
        { .register_write = { 0U, reg, value } },
    };
}

static void set_key_event(struct np2opngen_synth_event *event,
                          uint64_t timestamp, uint64_t sequence,
                          uint8_t channel, uint8_t value)
{
    *event = (struct np2opngen_synth_event){
        timestamp,
        sequence,
        NP2_SYNTH_EVENT_KEY_EVENT,
        { .key_event = { channel, value, 0U } },
    };
}

static void bootstrap_event(uint32_t index, struct np2opngen_synth_event *event,
                            uint64_t sequence)
{
    if (index < SYNTHETIC_BOOTSTRAP_CONFIG_EVENTS) {
        const uint32_t channel = index / 28U;
        const uint32_t local = index % 28U;
        if (local < 24U) {
            const uint32_t operator_index = local / 6U;
            const uint32_t group = local % 6U;
            const uint16_t reg = (uint16_t)(operator_register_groups[group] +
                                            operator_index * 4U + channel);
            const uint8_t values[6][4] = {
                { 0x01U, 0x12U, 0x05U, 0x11U },
                { 0x18U, 0x28U, 0x38U, 0x48U },
                { 0x1fU, 0x1aU, 0x16U, 0x12U },
                { 0x0eU, 0x0aU, 0x08U, 0x06U },
                { 0x06U, 0x05U, 0x04U, 0x03U },
                { 0x1fU, 0x1fU, 0x1fU, 0x1fU },
            };
            set_register_event(event, 0U, sequence, reg,
                               values[group][operator_index]);
            return;
        }
        switch (local - 24U) {
        case 0U:
            set_register_event(event, 0U, sequence,
                               (uint16_t)(0xb0U + channel),
                               (uint8_t)(0x20U + channel * 0x12U + channel));
            return;
        case 1U:
            set_register_event(event, 0U, sequence,
                               (uint16_t)(0xa4U + channel),
                               (uint8_t)(0x20U + channel));
            return;
        case 2U:
            set_register_event(event, 0U, sequence,
                               (uint16_t)(0xa0U + channel),
                               (uint8_t)(0x30U + channel * 0x19U));
            return;
        default:
            /* Fourth channel control remains a YM2203 DT/MUL write; no
             * OPNA-only pan/LFO register is used. */
            set_register_event(event, 0U, sequence,
                               (uint16_t)(0x30U + channel),
                               (uint8_t)(0x01U + channel));
            return;
        }
    }
    set_key_event(event, 240U, sequence,
                  (uint8_t)(index - SYNTHETIC_BOOTSTRAP_CONFIG_EVENTS),
                  0xf0U);
}

static int slot_matches(uint32_t slot, uint32_t first, uint32_t step,
                        uint32_t count, uint32_t *index)
{
    uint32_t delta;
    if (slot < first) {
        return 0;
    }
    delta = slot - first;
    if (step == 0U || delta % step != 0U || delta / step >= count) {
        return 0;
    }
    if (index != 0) {
        *index = delta / step;
    }
    return 1;
}

static int light_slot_event_count(uint32_t slot)
{
    uint32_t index;
    if (slot_matches(slot, 8U, 11U, 18U, &index)) {
        return 2;
    }
    if (slot_matches(slot, 3U, 11U, 18U, &index) ||
        slot == 10U || slot == 43U || slot == 76U || slot == 109U ||
        slot == 142U || slot == 175U || slot == 33U || slot == 99U ||
        slot == 165U || slot == 55U || slot == 56U || slot == 121U ||
        slot == 122U || slot == 187U || slot == 188U) {
        return 1;
    }
    return 0;
}

static int heavy_slot_event_count(uint32_t slot)
{
    uint32_t index;
    int count = 0;
    if (slot_matches(slot, 2U, 2U, 36U, &index)) {
        count += 2;
    }
    if (slot_matches(slot, 1U, 2U, 72U, &index)) {
        ++count;
    }
    if (slot_matches(slot, 74U, 2U, 36U, &index)) {
        ++count;
    }
    if (slot_matches(slot, 145U, 1U, 18U, &index)) {
        ++count;
    }
    if (slot >= 163U && slot <= 198U) {
        ++count;
    }
    if (slot == 143U || slot == 144U || slot == 199U) {
        ++count;
    }
    return count;
}

static int stress_slot_event_count(uint32_t slot)
{
    if (slot >= 2U && slot <= 170U && (slot - 2U) % 2U == 0U) {
        return 8;
    }
    return slot == 176U || slot == 177U || slot == 182U || slot == 183U;
}

static int slot_event_count(enum np2opngen_synthetic_profile profile,
                            uint32_t slot)
{
    switch (profile) {
    case NP2_OPNGEN_SYNTHETIC_LIGHT:
        return light_slot_event_count(slot);
    case NP2_OPNGEN_SYNTHETIC_HEAVY:
        return heavy_slot_event_count(slot);
    case NP2_OPNGEN_SYNTHETIC_STRESS:
        return stress_slot_event_count(slot);
    }
    return 0;
}

static void profile_register_event(
    const struct np2opngen_synthetic_workload *workload,
    struct np2opngen_synth_event *event, uint64_t sequence, uint16_t reg,
    uint8_t value)
{
    const uint64_t timestamp = (uint64_t)workload->second *
                                    NP2_OPNGEN_SYNTHETIC_RATE_HZ +
                                (uint64_t)workload->slot *
                                    NP2_OPNGEN_SYNTHETIC_QUANTUM;
    set_register_event(event, timestamp, sequence, reg, value);
}

static void profile_key_event(
    const struct np2opngen_synthetic_workload *workload,
    struct np2opngen_synth_event *event, uint64_t sequence, uint8_t channel,
    uint8_t value)
{
    const uint64_t timestamp = (uint64_t)workload->second *
                                    NP2_OPNGEN_SYNTHETIC_RATE_HZ +
                                (uint64_t)workload->slot *
                                    NP2_OPNGEN_SYNTHETIC_QUANTUM;
    set_key_event(event, timestamp, sequence, channel, value);
}

static void light_event(const struct np2opngen_synthetic_workload *workload,
                        struct np2opngen_synth_event *event, uint64_t sequence,
                        uint32_t occurrence)
{
    uint32_t index;
    if (slot_matches(workload->slot, 8U, 11U, 18U, &index)) {
        const uint32_t channel = index % 3U;
        const uint16_t reg = (uint16_t)((occurrence == 0U ? 0xa4U : 0xa0U) +
                                        channel);
        profile_register_event(workload, event, sequence, reg,
                               occurrence == 0U
                                   ? (uint8_t)(0x20U + (index % 8U))
                                   : (uint8_t)(0x30U + (index * 13U) % 0x80U));
        return;
    }
    if (slot_matches(workload->slot, 3U, 11U, 18U, &index)) {
        profile_register_event(workload, event, sequence,
                               (uint16_t)(0x40U + (index % 4U) * 4U +
                                           index % 3U),
                               (uint8_t)(0x18U + (index * 7U) % 0x60U));
        return;
    }
    if (workload->slot == 10U || workload->slot == 43U ||
        workload->slot == 76U || workload->slot == 109U ||
        workload->slot == 142U || workload->slot == 175U) {
        index = (workload->slot - 10U) / 33U;
        profile_register_event(workload, event, sequence,
                               (uint16_t)(0x50U + (index % 4U) * 4U +
                                           index % 3U),
                               (uint8_t)(0x10U + index * 3U));
        return;
    }
    if (workload->slot == 33U || workload->slot == 99U ||
        workload->slot == 165U) {
        index = (workload->slot - 33U) / 66U;
        profile_register_event(workload, event, sequence,
                               (uint16_t)(0xb0U + index % 3U),
                               (uint8_t)(0x20U + index * 0x12U));
        return;
    }
    index = workload->slot == 55U || workload->slot == 56U
                ? 0U
                : workload->slot == 121U || workload->slot == 122U ? 1U : 2U;
    profile_key_event(workload, event, sequence, (uint8_t)index,
                      workload->slot == 55U || workload->slot == 121U ||
                              workload->slot == 187U
                          ? 0U
                          : 0xf0U);
}

static void heavy_event(const struct np2opngen_synthetic_workload *workload,
                        struct np2opngen_synth_event *event, uint64_t sequence,
                        uint32_t occurrence)
{
    uint32_t index;
    if (slot_matches(workload->slot, 2U, 2U, 36U, &index)) {
        const uint32_t channel = index % 3U;
        const uint16_t reg = (uint16_t)((occurrence == 0U ? 0xa4U : 0xa0U) +
                                        channel);
        profile_register_event(workload, event, sequence, reg,
                               occurrence == 0U
                                   ? (uint8_t)(0x20U + (index % 8U))
                                   : (uint8_t)(0x30U + (index * 9U) % 0x80U));
        return;
    }
    if (occurrence == 0U &&
        slot_matches(workload->slot, 1U, 2U, 72U, &index)) {
        profile_register_event(workload, event, sequence,
                               (uint16_t)(0x40U + (index % 4U) * 4U +
                                           index % 3U),
                               (uint8_t)(0x08U + (index * 5U) % 0x70U));
        return;
    }
    if (occurrence == 0U &&
        slot_matches(workload->slot, 74U, 2U, 36U, &index)) {
        profile_register_event(workload, event, sequence,
                               (uint16_t)(0x50U + (index % 4U) * 4U +
                                           index % 3U),
                               (uint8_t)(0x0cU + (index * 3U) % 0x30U));
        return;
    }
    if (occurrence == 0U &&
        slot_matches(workload->slot, 145U, 1U, 18U, &index)) {
        profile_register_event(workload, event, sequence,
                               (uint16_t)(0xb0U + index % 3U),
                               (uint8_t)(0x20U + (index * 7U) % 0x60U));
        return;
    }
    if (workload->slot >= 163U && workload->slot <= 198U) {
        index = (workload->slot - 163U) / 2U;
        profile_key_event(workload, event, sequence, (uint8_t)(index % 3U),
                          (workload->slot - 163U) % 2U == 0U ? 0U : 0xf0U);
        return;
    }
    /* The three former pan/modulation slots are deterministic DT/MUL writes. */
    index = workload->slot == 143U ? 0U : workload->slot == 144U ? 1U : 2U;
    profile_register_event(workload, event, sequence,
                           (uint16_t)(0x30U + (index % 4U) * 4U + index % 3U),
                           (uint8_t)(0x02U + index));
}

static void stress_event(const struct np2opngen_synthetic_workload *workload,
                         struct np2opngen_synth_event *event, uint64_t sequence,
                         uint32_t occurrence)
{
    uint32_t index;
    if (workload->slot >= 2U && workload->slot <= 170U &&
        (workload->slot - 2U) % 2U == 0U) {
        index = (workload->slot - 2U) / 2U;
        if (occurrence < 6U) {
            const uint32_t channel = occurrence / 2U;
            const uint16_t reg = (uint16_t)((occurrence % 2U == 0U ? 0xa4U
                                                                   : 0xa0U) +
                                            channel);
            profile_register_event(workload, event, sequence, reg,
                                   occurrence % 2U == 0U
                                       ? (uint8_t)(0x20U + index % 8U)
                                       : (uint8_t)(0x30U + index * 3U % 0x80U));
        } else if (occurrence == 6U) {
            profile_register_event(workload, event, sequence,
                                   (uint16_t)(0x50U + (index % 4U) * 4U +
                                               index % 3U),
                                   (uint8_t)(0x10U + index % 0x30U));
            if (index % 5U != 0U) {
                event->payload.register_write.reg =
                    (uint16_t)(0x40U + (index % 4U) * 4U + index % 3U);
            }
        } else {
            profile_register_event(workload, event, sequence,
                                   (uint16_t)(index % 7U == 0U
                                                  ? 0xb0U + index % 3U
                                                  : 0x40U + (index % 4U) * 4U +
                                                        index % 3U),
                                   (uint8_t)(0x20U + index * 5U % 0x60U));
        }
        return;
    }
    index = workload->slot == 176U || workload->slot == 177U ? 0U : 1U;
    profile_key_event(workload, event, sequence, (uint8_t)index,
                      workload->slot == 176U || workload->slot == 182U
                          ? 0U
                          : 0xf0U);
}

static void program_event(const struct np2opngen_synthetic_workload *workload,
                          struct np2opngen_synth_event *event,
                          uint64_t sequence)
{
    switch (workload->profile) {
    case NP2_OPNGEN_SYNTHETIC_LIGHT:
        light_event(workload, event, sequence, workload->slot_event);
        return;
    case NP2_OPNGEN_SYNTHETIC_HEAVY:
        heavy_event(workload, event, sequence, workload->slot_event);
        return;
    case NP2_OPNGEN_SYNTHETIC_STRESS:
        stress_event(workload, event, sequence, workload->slot_event);
        return;
    }
}

static void seek_next_slot(struct np2opngen_synthetic_workload *workload)
{
    while (!workload->complete && workload->slot < SYNTHETIC_SECONDS_SLOTS &&
           slot_event_count(workload->profile, workload->slot) == 0) {
        ++workload->slot;
    }
    while (!workload->complete && workload->slot >= SYNTHETIC_SECONDS_SLOTS) {
        ++workload->second;
        workload->slot = 0U;
        workload->slot_event = 0U;
        if (workload->second >= workload->duration_seconds) {
            workload->complete = true;
            return;
        }
        while (workload->slot < SYNTHETIC_SECONDS_SLOTS &&
               slot_event_count(workload->profile, workload->slot) == 0) {
            ++workload->slot;
        }
    }
}

int np2opngen_synthetic_workload_init(
    struct np2opngen_synthetic_workload *workload,
    enum np2opngen_synthetic_profile profile, uint32_t duration_seconds)
{
    if (workload == 0 || profile_rate(profile) < 0 || duration_seconds == 0U ||
        np2opngen_synthetic_workload_expected_events(profile,
                                                     duration_seconds) ==
            UINT64_MAX) {
        return -1;
    }
    memset(workload, 0, sizeof(*workload));
    workload->profile = profile;
    workload->duration_seconds = duration_seconds;
    workload->bootstrap = true;
    return 0;
}

int np2opngen_synthetic_workload_peek(
    const struct np2opngen_synthetic_workload *workload,
    struct np2opngen_synth_event *event)
{
    if (workload == 0 || event == 0 || workload->complete) {
        return workload != 0 && workload->complete ? 0 : -1;
    }
    if (workload->bootstrap) {
        bootstrap_event((uint32_t)workload->emitted_count, event,
                        workload->next_sequence);
        return 1;
    }
    program_event(workload, event, workload->next_sequence);
    return 1;
}

int np2opngen_synthetic_workload_commit(
    struct np2opngen_synthetic_workload *workload)
{
    int count;
    if (workload == 0 || workload->complete ||
        workload->next_sequence == UINT64_MAX) {
        return -1;
    }
    ++workload->next_sequence;
    ++workload->emitted_count;
    if (workload->bootstrap) {
        if (workload->emitted_count == SYNTHETIC_BOOTSTRAP_EVENTS) {
            workload->bootstrap = false;
            workload->slot = 0U;
            workload->slot_event = 0U;
            seek_next_slot(workload);
        }
        return 0;
    }
    count = slot_event_count(workload->profile, workload->slot);
    ++workload->slot_event;
    if (workload->slot_event >= (uint32_t)count) {
        ++workload->slot;
        workload->slot_event = 0U;
        seek_next_slot(workload);
    }
    return 0;
}
