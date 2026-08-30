#include "np2audio86_guest_adapter.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/*
 * This is deliberately a guest-domain model.  There is no generator object,
 * PCM sample array, task, lock, or host-time call in this file.
 */
typedef struct {
    uint8_t regs[0x200];
    uint8_t capabilities;
    uint8_t address_low;
    uint8_t address_extended;
    uint8_t data;
    uint8_t extension;
    uint8_t timer_control;
    uint16_t timer_a_value;
    uint8_t timer_b_value;
    uint8_t timer_status;
    uint8_t timer_irq_enable;
    uint8_t timer_a_running;
    uint8_t timer_b_running;
    uint8_t timer_a_event;
    uint8_t timer_b_event;
    uint32_t irq;
    uint16_t base;
    uint8_t channels;
    uint8_t joy;
    uint8_t soundrom_rejected;
    uint8_t pcm_soundflags;
    uint8_t pcm_fifo;
    uint8_t pcm_dactrl;
    uint8_t pcm_volume;
    uint8_t pcm_rate;
    uint8_t pcm_irq;
    uint8_t pcm_reqirq;
    uint32_t pcm_rescue;
    uint8_t pcm_stepbit;
    uint16_t pcm_fifo_size;
    uint16_t pcm_fifo_level;
    uint16_t pcm_stepmask;
    uint32_t pcm_virtual_buffer;
    uint32_t pcm_read_position;
    uint32_t pcm_consume_remainder;
    uint64_t guest_cycles;
    uint64_t frame_timestamp;
    uint64_t sequence;
    uint32_t frame_remainder;
    uint32_t last_cpu_position;
    uint64_t timer_a_due;
    uint64_t timer_b_due;
    uint8_t timer_a_due_valid;
    uint8_t timer_b_due_valid;
    uint8_t cpu_position_valid;
    uint8_t bound;
} np2audio86_guest_state_t;

static np2audio86_guest_state_t g_state;
static np2audio86_guest_trace_t *g_trace;
static np2audio86_guest_cpu_position_fn g_cpu_position;
static uint32_t g_manual_cpu_position;
static uint32_t g_baseclock = 2457600u;
static uint32_t g_multiple = 20u;
static np2audio86_guest_timer_schedule_fn g_schedule;
static np2audio86_guest_timer_cancel_fn g_cancel;
static np2audio86_guest_irq_fn g_irq;
static void (*g_extension_callback)(uint8_t enabled);
static uint8_t g_failed;
static char g_failure_reason[96];

static uint32_t current_cpu_position(void)
{
    return g_cpu_position ? g_cpu_position() : g_manual_cpu_position;
}

static void fail(const char *reason)
{
    if (!g_failed) {
        g_failed = 1;
        (void)snprintf(g_failure_reason, sizeof(g_failure_reason), "%s", reason);
    }
}

static void append_io(uint16_t port, uint8_t direction, uint8_t value,
                      uint8_t result)
{
    np2audio86_guest_io_trace_t *item;
    if (!g_trace || g_failed) {
        return;
    }
    if (g_trace->io_count >= g_trace->io_capacity) {
        fail("guest I/O trace capacity");
        return;
    }
    item = &g_trace->io[g_trace->io_count++];
    item->frame_timestamp = g_state.frame_timestamp;
    item->sequence = g_state.sequence;
    item->port = port;
    item->direction = direction;
    item->value = value;
    item->result = result;
    memset(item->reserved, 0, sizeof(item->reserved));
}

static void append_event(uint32_t opcode, uint32_t payload)
{
    np2audio86_guest_event_t *item;
    if (g_failed) {
        return;
    }
    if (!g_trace) {
        if (g_state.sequence == UINT64_MAX) {
            fail("sequence overflow");
            return;
        }
        g_state.sequence++;
        return;
    }
    if (g_state.sequence == UINT64_MAX ||
        g_trace->event_count >= g_trace->event_capacity) {
        fail("event trace capacity or sequence overflow");
        return;
    }
    item = &g_trace->events[g_trace->event_count++];
    item->frame_timestamp = g_state.frame_timestamp;
    item->sequence = g_state.sequence++;
    item->opcode = opcode;
    item->payload = payload;
}

/* The pending run is kept outside the principal state to keep the state small. */
static uint8_t g_run_pending;
static uint64_t g_run_timestamp;
static uint64_t g_run_sequence;
static size_t g_run_offset;
static size_t g_run_count;

static void flush_pending_run(void)
{
    np2audio86_guest_data_run_t *run;
    if (!g_run_pending) {
        return;
    }
    if (!g_trace || g_trace->data_run_count >= g_trace->data_run_capacity) {
        fail("PCM DATA_RUN trace capacity");
        return;
    }
    run = &g_trace->data_runs[g_trace->data_run_count++];
    run->frame_timestamp = g_run_timestamp;
    run->sequence = g_run_sequence;
    run->byte_offset = g_run_offset;
    run->count = (uint32_t)g_run_count;
    g_run_pending = 0;
    g_run_count = 0;
}

static void start_or_append_byte(uint8_t value)
{
    if (!g_trace || g_failed) {
        return;
    }
    if (g_trace->pcm_count >= g_trace->pcm_capacity) {
        fail("PCM byte trace capacity");
        return;
    }
    if (!g_run_pending || g_run_timestamp != g_state.frame_timestamp ||
        g_run_count >= 32768u) {
        flush_pending_run();
        if (g_state.sequence == UINT64_MAX) {
            fail("sequence overflow");
            return;
        }
        g_run_pending = 1;
        g_run_timestamp = g_state.frame_timestamp;
        g_run_sequence = g_state.sequence++;
        g_run_offset = g_trace->pcm_count;
        g_run_count = 0;
    }
    g_trace->pcm_bytes[g_trace->pcm_count++] = value;
    g_run_count++;
}

static void timer_trace(uint8_t timer, uint8_t level)
{
    np2audio86_guest_timer_trace_t *item;
    if (!g_trace || g_failed) {
        return;
    }
    if (g_trace->timer_count >= g_trace->timer_capacity) {
        fail("timer trace capacity");
        return;
    }
    item = &g_trace->timers[g_trace->timer_count++];
    item->frame_timestamp = g_state.frame_timestamp;
    item->guest_cycles = g_state.guest_cycles;
    item->timer = timer;
    item->status = g_state.timer_status;
    item->irq = (uint8_t)g_state.irq;
    item->level = level;
    item->reserved = 0;
}

static uint64_t timer_period(uint8_t timer)
{
    uint64_t multiple = g_multiple ? g_multiple : 1u;
    if (timer == NP2AUDIO86_TRACE_TIMER_A) {
        uint32_t value = ((uint32_t)g_state.regs[0x24] << 2) |
                         (g_state.regs[0x25] & 3u);
        return 18u * (1024u - (value & 0x0fffu)) * multiple;
    }
    return 288u * (256u - g_state.regs[0x26]) * multiple;
}

static void set_irq(uint8_t timer, uint8_t level)
{
    uint32_t irq = g_state.irq;
    if (g_irq) {
        g_irq(irq, level);
    }
    timer_trace(timer, level);
}

static void timer_expire(uint8_t timer)
{
    uint8_t enabled;
    if (timer == NP2AUDIO86_TRACE_TIMER_A) {
        g_state.timer_status |= 1u;
        enabled = (uint8_t)((g_state.timer_control >> 2) & 1u);
        if (g_state.timer_a_running) {
            if (g_state.timer_a_due > UINT64_MAX - timer_period(timer)) {
                fail("Timer A due-cycle overflow");
                return;
            }
            g_state.timer_a_due += timer_period(timer);
            if (g_schedule) {
                g_schedule(timer, g_state.timer_a_due);
            }
        }
    } else {
        g_state.timer_status |= 2u;
        enabled = (uint8_t)((g_state.timer_control >> 3) & 1u);
        if (g_state.timer_b_running) {
            if (g_state.timer_b_due > UINT64_MAX - timer_period(timer)) {
                fail("Timer B due-cycle overflow");
                return;
            }
            g_state.timer_b_due += timer_period(timer);
            if (g_schedule) {
                g_schedule(timer, g_state.timer_b_due);
            }
        }
    }
    if (enabled) {
        set_irq(timer, 1);
    } else {
        timer_trace(timer, 0);
    }
    if (timer == NP2AUDIO86_TRACE_TIMER_A &&
        (g_state.timer_control & 0x80u)) {
        append_event(NP2AUDIO86_TRACE_OPNA_CSM, 0);
    }
}

static void process_timers(void)
{
    if (g_state.timer_a_due_valid && g_state.timer_a_running &&
        g_state.guest_cycles >= g_state.timer_a_due) {
        g_state.timer_a_due_valid = 0;
        timer_expire(NP2AUDIO86_TRACE_TIMER_A);
        g_state.timer_a_due_valid = g_state.timer_a_running;
    }
    if (g_state.timer_b_due_valid && g_state.timer_b_running &&
        g_state.guest_cycles >= g_state.timer_b_due) {
        g_state.timer_b_due_valid = 0;
        timer_expire(NP2AUDIO86_TRACE_TIMER_B);
        g_state.timer_b_due_valid = g_state.timer_b_running;
    }
}

static void advance_pcm(uint64_t delta)
{
    static const uint32_t rates[8] = { 15625u, 18750u, 22050u, 31250u,
                                       37500u, 44100u, 48000u, 0u };
    uint64_t numerator;
    uint32_t consumed;
    uint32_t rate = rates[g_state.pcm_rate & 7u];
    if (!rate || !(g_state.pcm_dactrl & 1u) || !g_state.pcm_soundflags) {
        return;
    }
    numerator = delta * rate + g_state.pcm_consume_remainder;
    consumed = (uint32_t)(numerator / (49152000u));
    g_state.pcm_consume_remainder = (uint32_t)(numerator % 49152000u);
    if (consumed > g_state.pcm_fifo_level) {
        consumed = g_state.pcm_fifo_level;
    }
    g_state.pcm_fifo_level = (uint16_t)(g_state.pcm_fifo_level - consumed);
    g_state.pcm_read_position =
        (g_state.pcm_read_position + consumed) & 0x7fffu;
    if (g_state.pcm_fifo_level <= g_state.pcm_fifo_size) {
        g_state.pcm_irq = 1;
    }
}

void np2audio86_guest_audio_sync(void)
{
    uint32_t position = current_cpu_position();
    uint32_t delta;
    uint64_t total;
    if (!g_state.cpu_position_valid) {
        g_state.last_cpu_position = position;
        g_state.cpu_position_valid = 1;
        return;
    }
    delta = (uint32_t)(position - g_state.last_cpu_position);
    g_state.last_cpu_position = position;
    if (g_state.guest_cycles > UINT64_MAX - delta) {
        fail("guest cycle overflow");
        return;
    }
    g_state.guest_cycles += delta;
    advance_pcm(delta);
    total = (uint64_t)g_state.frame_remainder + delta;
    if (g_state.frame_timestamp > UINT64_MAX - total / 1024u) {
        fail("frame timestamp overflow");
        return;
    }
    g_state.frame_timestamp += total / 1024u;
    g_state.frame_remainder = (uint32_t)(total % 1024u);
    process_timers();
}

void np2audio86_guest_host_trace_attach(np2audio86_guest_trace_t *trace)
{
    g_trace = trace;
    if (g_trace) {
        g_trace->event_count = 0;
        g_trace->data_run_count = 0;
        g_trace->pcm_count = 0;
        g_trace->timer_count = 0;
        g_trace->io_count = 0;
    }
}

void np2audio86_guest_host_trace_detach(void)
{
    flush_pending_run();
    g_trace = NULL;
}

void np2audio86_guest_host_set_cpu_position_fn(
    np2audio86_guest_cpu_position_fn position)
{
    g_cpu_position = position;
}

void np2audio86_guest_host_set_cpu_position(uint32_t position)
{
    g_manual_cpu_position = position;
}

void np2audio86_guest_host_set_clock(uint32_t baseclock, uint32_t multiple)
{
    if ((uint64_t)baseclock * multiple != 49152000u) {
        fail("unsupported guest clock");
        return;
    }
    if (g_state.bound &&
        (baseclock != g_baseclock || multiple != g_multiple)) {
        fail("dynamic clock change unsupported");
        return;
    }
    g_baseclock = baseclock;
    g_multiple = multiple;
    (void)g_baseclock;
}

void np2audio86_guest_host_set_timer_hooks(
    np2audio86_guest_timer_schedule_fn schedule,
    np2audio86_guest_timer_cancel_fn cancel, np2audio86_guest_irq_fn irq)
{
    g_schedule = schedule;
    g_cancel = cancel;
    g_irq = irq;
}

void np2audio86_guest_host_timer_tick(uint8_t timer)
{
    np2audio86_guest_audio_sync();
    timer_expire(timer);
}

void np2audio86_guest_host_flush_data_run(void)
{
    flush_pending_run();
}

void np2audio86_guest_host_test_seed(uint64_t frame_timestamp,
                                     uint64_t sequence)
{
    flush_pending_run();
    g_state.frame_timestamp = frame_timestamp;
    g_state.sequence = sequence;
    g_state.guest_cycles = 0;
    g_state.frame_remainder = 0;
    g_state.cpu_position_valid = 0;
    g_failed = 0;
    g_failure_reason[0] = '\0';
}

void np2audio86_guest_host_snapshot(
    np2audio86_guest_state_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->frame_timestamp = g_state.frame_timestamp;
    snapshot->guest_cycles = g_state.guest_cycles;
    snapshot->sequence = g_state.sequence;
    snapshot->cpu_remainder = g_state.frame_remainder;
    snapshot->opna_base = g_state.base;
    snapshot->opna_address_low = g_state.address_low;
    snapshot->opna_address_extended = g_state.address_extended;
    snapshot->opna_data = g_state.data;
    snapshot->opna_extension = g_state.extension;
    snapshot->opna_capabilities = g_state.capabilities;
    snapshot->opna_status = g_state.timer_status;
    snapshot->timer_control = g_state.timer_control;
    snapshot->timer_a_value = g_state.timer_a_value;
    snapshot->timer_b_value = g_state.timer_b_value;
    snapshot->timer_irq = (uint8_t)g_state.irq;
    snapshot->pcm_soundflags = g_state.pcm_soundflags;
    snapshot->pcm_fifo = g_state.pcm_fifo;
    snapshot->pcm_dactrl = g_state.pcm_dactrl;
    snapshot->pcm_volume = g_state.pcm_volume;
    snapshot->pcm_rate = g_state.pcm_rate;
    snapshot->pcm_fifo_size = g_state.pcm_fifo_size;
    snapshot->pcm_fifo_level = g_state.pcm_fifo_level;
    snapshot->pcm_virtual_buffer = g_state.pcm_virtual_buffer;
    snapshot->pcm_read_position = g_state.pcm_read_position;
    snapshot->pcm_irq = g_state.pcm_irq;
    snapshot->pcm_reqirq = g_state.pcm_reqirq;
    snapshot->pcm_rescue = g_state.pcm_rescue;
    snapshot->soundrom_rejected = g_state.soundrom_rejected;
    snapshot->bound = g_state.bound;
}

size_t np2audio86_guest_host_state_size(void)
{
    return sizeof(g_state);
}

uint8_t np2audio86_guest_host_failed(void)
{
    return g_failed;
}

const char *np2audio86_guest_host_failure_reason(void)
{
    return g_failure_reason;
}

uint8_t np2audio86_guest_host_save_load_supported(void)
{
    /* SAVE_LOAD_DEFERRED_WITH_EXPLICIT_UNSUPPORTED_BOUNDARY. */
    return 0;
}

void np2audio86_guest_host_record_io(uint16_t port, uint8_t direction,
                                     uint8_t value, uint8_t result)
{
    np2audio86_guest_audio_sync();
    append_io(port, direction, value, result);
}

void np2audio86_guest_opna_write_address_low(uint8_t value)
{
    g_state.address_low = value;
    g_state.data = value;
}

static void opna_write_register(uint16_t address, uint8_t value)
{
    g_state.regs[address & 0x1ffu] = value;
    if (address < 0x100u) {
        if (address == 0x24u) {
            g_state.timer_a_value = value;
        } else if (address == 0x25u) {
            g_state.timer_a_value =
                (uint16_t)(((uint16_t)g_state.regs[0x24] << 2) |
                           (value & 3u));
        } else if (address == 0x26u) {
            g_state.timer_b_value = value;
        } else if (address == 0x27u) {
            uint8_t old = g_state.timer_control;
            g_state.timer_control = value;
            if (value & 0x10u) {
                g_state.timer_status &= (uint8_t)~1u;
                set_irq(NP2AUDIO86_TRACE_TIMER_A, 0);
            }
            if (value & 0x20u) {
                g_state.timer_status &= (uint8_t)~2u;
                set_irq(NP2AUDIO86_TRACE_TIMER_B, 0);
            }
            g_state.timer_a_running = (uint8_t)(value & 1u);
            g_state.timer_b_running = (uint8_t)((value >> 1) & 1u);
            if (g_state.timer_a_running && !(old & 1u)) {
                g_state.timer_a_due = g_state.guest_cycles + timer_period(1);
                g_state.timer_a_due_valid = 1;
                if (g_schedule) g_schedule(1, g_state.timer_a_due);
            } else if (!g_state.timer_a_running && (old & 1u)) {
                g_state.timer_a_due_valid = 0;
                if (g_cancel) g_cancel(1);
            }
            if (g_state.timer_b_running && !(old & 2u)) {
                g_state.timer_b_due = g_state.guest_cycles + timer_period(2);
                g_state.timer_b_due_valid = 1;
                if (g_schedule) g_schedule(2, g_state.timer_b_due);
            } else if (!g_state.timer_b_running && (old & 2u)) {
                g_state.timer_b_due_valid = 0;
                if (g_cancel) g_cancel(2);
            }
        }
    }
    append_event(NP2AUDIO86_TRACE_OPNA_REGISTER,
                 ((uint32_t)address << 8) | value);
}

void np2audio86_guest_opna_write_data_low(uint8_t value)
{
    g_state.data = value;
    opna_write_register(g_state.address_low, value);
}

void np2audio86_guest_opna_write_address_extended(uint8_t value)
{
    if (g_state.extension) {
        g_state.address_extended = value;
        g_state.data = value;
    }
}

void np2audio86_guest_opna_write_data_extended(uint8_t value)
{
    if (g_state.extension) {
        g_state.data = value;
        opna_write_register((uint16_t)(0x100u | g_state.address_extended), value);
    }
}

uint8_t np2audio86_guest_opna_read_status(void)
{
    flush_pending_run();
    return g_state.timer_status;
}

uint8_t np2audio86_guest_opna_read_data(void)
{
    flush_pending_run();
    if (g_state.address_low == 0x0e) return g_state.joy;
    if (g_state.address_low < 0x10) return g_state.regs[g_state.address_low];
    if (g_state.address_low == 0xff) return 1;
    return g_state.data;
}

uint8_t np2audio86_guest_opna_read_extended_status(void)
{
    flush_pending_run();
    return g_state.extension ? g_state.timer_status : 0xff;
}

uint8_t np2audio86_guest_opna_read_extended_data(void)
{
    flush_pending_run();
    if (!g_state.extension) return 0xff;
    if (g_state.address_extended == 0x08 || g_state.address_extended == 0x0f)
        return g_state.regs[0x100u | g_state.address_extended];
    return g_state.data;
}

uint8_t np2audio86_guest_opna_read_joy(void)
{
    flush_pending_run();
    return g_state.joy;
}

void np2audio86_guest_opna_set_extension(uint8_t enabled)
{
    g_state.extension = enabled ? 1u : 0u;
    if (g_extension_callback && g_extension_callback != NULL) {
        /* The callback is registered for observability only; do not recurse. */
    }
}

void np2audio86_guest_opna_reset(uint8_t capabilities, uint32_t irq,
                                 uint8_t timer_a_event,
                                 uint8_t timer_b_event)
{
    uint64_t frame = g_state.frame_timestamp;
    uint64_t cycles = g_state.guest_cycles;
    uint64_t sequence = g_state.sequence;
    uint32_t remainder = g_state.frame_remainder;
    uint8_t had_state = g_state.bound;
    flush_pending_run();
    memset(&g_state, 0, sizeof(g_state));
    g_state.frame_timestamp = frame;
    g_state.guest_cycles = cycles;
    g_state.sequence = sequence;
    g_state.frame_remainder = remainder;
    g_state.capabilities = capabilities;
    g_state.irq = irq;
    g_state.timer_a_event = timer_a_event;
    g_state.timer_b_event = timer_b_event;
    g_state.joy = 0;
    g_state.pcm_fifo_size = 0x80;
    g_state.pcm_fifo = 0;
    g_state.pcm_dactrl = 0x32;
    g_state.pcm_stepbit = 2;
    g_state.pcm_stepmask = 3;
    g_state.pcm_rescue = 20u * 32u * 4u;
    g_state.bound = had_state;
    g_failed = 0;
    g_failure_reason[0] = '\0';
    g_run_pending = 0;
    g_run_count = 0;
    if (had_state) {
        append_event(NP2AUDIO86_TRACE_RESET_BARRIER, 0);
    }
}

void np2audio86_guest_opna_set_config(uint8_t channels, uint32_t mode)
{
    g_state.channels = channels;
    (void)mode;
}

void np2audio86_guest_opna_set_base(uint16_t base)
{
    g_state.base = base;
}

uint16_t np2audio86_guest_opna_base(void)
{
    return g_state.base;
}

void np2audio86_guest_opna_register_extension(void (*callback)(uint8_t enabled))
{
    g_extension_callback = callback;
}

void np2audio86_guest_opna_bind(void)
{
    g_state.bound = 1;
}

void np2audio86_guest_opna_unbind(void)
{
    g_state.bound = 0;
}

void np2audio86_guest_soundrom_load(uint32_t address, const char *name)
{
    (void)address;
    (void)name;
    g_state.soundrom_rejected = 1;
}

void np2audio86_guest_pcm86_write(uint8_t register_index, uint8_t value)
{
    flush_pending_run();
    switch (register_index) {
    case 0x00:
        g_state.pcm_soundflags = value & 1u;
        break;
    case 0x06:
        if ((value & 0xe0u) == 0xa0u)
            g_state.pcm_volume = (uint8_t)((~value) & 15u);
        break;
    case 0x08: {
        uint8_t old = g_state.pcm_fifo;
        if ((value & 8u) && !(old & 8u)) {
            g_state.pcm_fifo_level = 0;
            g_state.pcm_virtual_buffer = 0;
            g_state.pcm_read_position = 0;
            g_state.pcm_irq = 0;
        }
        if (!(value & 0x10u)) g_state.pcm_irq = 0;
        if (g_state.pcm_fifo_level <= g_state.pcm_fifo_size)
            g_state.pcm_irq = 1;
        g_state.pcm_fifo = value;
        g_state.pcm_rate = value & 7u;
        if ((old ^ value) & 7u) {
            static const uint32_t rescue[8] = {20u * 32u, 20u * 24u,
                                                20u * 16u, 20u * 12u,
                                                20u * 8u, 20u * 6u,
                                                20u * 4u, 20u * 3u};
            g_state.pcm_rescue = rescue[value & 7u] << g_state.pcm_stepbit;
        }
        break;
    }
    case 0x0a:
        if (g_state.pcm_fifo & 0x20u) {
            if (value != 0xffu)
                g_state.pcm_fifo_size = (uint16_t)((value + 1u) << 7);
            else
                g_state.pcm_fifo_size = 0x7ffcu;
        } else if ((value & 15u) != 15u) {
            static const uint8_t bits[8] = {1, 1, 1, 2, 0, 0, 0, 1};
            static const uint32_t rescue[8] = {20u * 32u, 20u * 24u,
                                                20u * 16u, 20u * 12u,
                                                20u * 8u, 20u * 6u,
                                                20u * 4u, 20u * 3u};
            g_state.pcm_dactrl = value;
            g_state.pcm_stepbit = bits[(value >> 4) & 7u];
            g_state.pcm_stepmask = (uint16_t)((1u << g_state.pcm_stepbit) - 1u);
            g_state.pcm_rescue = rescue[g_state.pcm_fifo & 7u]
                                 << g_state.pcm_stepbit;
        }
        break;
    default:
        break;
    }
}

void np2audio86_guest_pcm86_write_data(uint8_t value)
{
    if (g_state.pcm_virtual_buffer < 0x8000u)
        g_state.pcm_virtual_buffer++;
    if (g_state.pcm_fifo_level < 0x8000u)
        g_state.pcm_fifo_level++;
    g_state.pcm_reqirq = 1;
    start_or_append_byte(value);
}

void np2audio86_guest_pcm86_set_mixer_volume(uint8_t value)
{
    g_state.pcm_volume = value & 15u;
}

uint8_t np2audio86_guest_pcm86_read(uint8_t register_index)
{
    uint8_t value;
    flush_pending_run();
    switch (register_index) {
    case 0x00:
        value = g_state.pcm_soundflags;
        break;
    case 0x06:
        value = (uint8_t)((g_state.pcm_read_position >> 9) & 0x3fu);
        if (g_state.pcm_fifo_level >= 0x8000u) value |= 0x80u;
        if (g_state.pcm_fifo_level <= g_state.pcm_stepmask) value |= 0x40u;
        break;
    case 0x08:
        value = g_state.pcm_fifo & (uint8_t)~0x10u;
        if (g_state.pcm_irq) value |= 0x10u;
        break;
    case 0x0a:
        value = g_state.pcm_dactrl;
        break;
    default:
        value = 0;
        break;
    }
    return value;
}

void np2audio86_guest_pcm86_set_options(uint8_t dip_switch)
{
    static const uint8_t irq_table[8] = {0xff, 0xff, 0xff, 0xff,
                                         0x03, 0x0a, 0x0d, 0x0c};
    g_state.pcm_soundflags = (uint8_t)(((~dip_switch) >> 1) & 0x70u);
    g_state.irq = irq_table[(dip_switch >> 2) & 7u];
}

void np2audio86_guest_pcm86_stream_bind(void)
{
    g_state.bound = 1;
}

void np2audio86_guest_pcm86_stream_unbind(void)
{
    g_state.bound = 0;
    flush_pending_run();
}
