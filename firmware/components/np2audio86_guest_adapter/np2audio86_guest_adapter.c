#include "np2audio86_guest_adapter.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Domain G owns semantic register/counter state only.  No generator, task,
 * lock, transport ring, or PCM sample array is permitted here. */
typedef struct {
    uint8_t regs[0x200];
    uint8_t capabilities, address_low, address_extended, data, extension;
    uint8_t timer_control, timer_b_value, timer_status, timer_irq_enable;
    uint16_t timer_a_value;
    uint8_t timer_a_running, timer_b_running, timer_a_event, timer_b_event;
    uint32_t opna_irq, pcm_irq_line, base;
    uint8_t channels, joy, soundrom_rejected;
    uint8_t pcm_soundflags, pcm_fifo, pcm_dactrl, pcm_volume;
    uint8_t pcm_irq, pcm_reqirq, pcm_clock_valid;
    uint8_t opna_pic_source, pcm_pic_source;
    uint32_t pcm_rescue, pcm_rateval;
    uint16_t pcm_fifo_size, pcm_stepmask;
    uint8_t pcm_stepbit, reserved0;
    uint32_t pcm_virtual_buffer, pcm_real_buffer, pcm_write_position;
    uint32_t pcm_read_position, pcm_step_remainder;
    uint64_t pcm_stepclock, pcm_lastclock, pcm_lastclockforwait;
    uint64_t guest_cycles, frame_timestamp, sequence;
    uint32_t frame_remainder, last_cpu_position;
    uint8_t cpu_position_valid, bound;
} np2audio86_guest_state_t;

static np2audio86_guest_state_t g_state;
static np2audio86_guest_trace_t *g_trace;
static const np2audio86_guest_sink_t *g_sink;
static np2audio86_guest_cpu_position_fn g_cpu_position;
static uint32_t g_manual_cpu_position;
static uint32_t g_baseclock = 2457600u, g_multiple = 20u;
static uint32_t g_cpumode = 0x20u; /* CPUMODE_8MHZ production default. */
static np2audio86_guest_timer_schedule_fn g_schedule;
static np2audio86_guest_timer_cancel_fn g_cancel;
static np2audio86_guest_timer_iswork_fn g_iswork;
static np2audio86_guest_irq_fn g_irq;
static void (*g_extension_callback)(uint8_t enabled);
static uint8_t g_failed;
static char g_failure_reason[96];
static uint8_t g_run_pending;
static uint64_t g_run_timestamp, g_run_sequence;
static size_t g_run_offset, g_run_count;
static np2audio86_guest_transaction_t g_run_transaction;
static uint8_t g_run_transaction_active;
/* A DATA_RUN owns transport state across calls; this guard instead covers one
 * synchronous guest semantic call.  They deliberately have independent
 * lifetimes. */
static uint8_t g_semantic_call_active;
static uint8_t g_contract_violation;
static uint8_t g_termination_pending;
#if defined(NP2AUDIO86_GUEST_TEST)
static size_t g_io_cycle_observation_count;
static uint64_t g_first_io_guest_cycle;
static uint64_t g_last_io_guest_cycle;
#endif

static int flush_pending_run(void);

typedef struct {
    uint32_t cpu_position;
    uint32_t last_cpu_position;
    uint32_t pcm_step_remainder;
    uint32_t pcm_virtual_buffer;
    uint32_t frame_remainder;
    uint64_t pcm_lastclock;
    uint64_t guest_cycles;
    uint64_t frame_timestamp;
    uint8_t cpu_position_valid;
    uint8_t pcm_clock_valid;
} np2audio86_guest_sync_plan_t;

static int sync_plan_compute(np2audio86_guest_sync_plan_t *plan);
static void sync_plan_apply(const np2audio86_guest_sync_plan_t *plan);
static void fail_sync_plan(int status);

/* These are the pinned pcm86.c fixed-point delays.  The first MVP keeps the
 * waveform-owned underflow history out of Domain G, so the supported boundary
 * is the pinned zero-history (non-underflow) branch. */
static const uint32_t g_pcm_clk25_128[8] = {
    0x00001bdeu, 0x00002527u, 0x000037bbu, 0x00004a4eu,
    0x00006f75u, 0x0000949cu, 0x0000df5fu, 0x00012938u
};
static const uint32_t g_pcm_clk20_128[8] = {
    0x000016a4u, 0x00001e30u, 0x00002d48u, 0x00003c60u,
    0x00005a8fu, 0x000078bfu, 0x0000b57du, 0x0000f17du
};

static uint32_t current_cpu_position(void)
{ return g_cpu_position ? g_cpu_position() : g_manual_cpu_position; }

static void fail(const char *reason)
{
    if (!g_failed) {
        g_failed = 1;
        (void)snprintf(g_failure_reason, sizeof(g_failure_reason), "%s", reason);
    }
}

static int semantic_enter(void)
{
    if (g_semantic_call_active) {
        /* Do not turn this into g_failed here: an already-authorized outer
         * operation must still reach its infallible commits. */
        g_contract_violation = 1U;
        return -1;
    }
    if (g_failed) return -1;
    if (g_contract_violation) {
        fail("guest semantic reentrancy");
        return -1;
    }
    if (g_termination_pending) {
        fail("guest transaction terminated");
        return -1;
    }
    g_semantic_call_active = 1U;
    return 0;
}

static void semantic_leave(void) { g_semantic_call_active = 0U; }

static void append_io(uint16_t port, uint8_t direction, uint8_t value,
                      uint8_t result)
{
    np2audio86_guest_io_trace_t *item;
    if (!g_trace || g_failed) return;
    if (g_trace->io_count >= g_trace->io_capacity) {
        fail("guest I/O trace capacity"); return;
    }
    item = &g_trace->io[g_trace->io_count++];
    item->frame_timestamp = g_state.frame_timestamp;
    item->sequence = g_state.sequence;
    item->port = port; item->direction = direction;
    item->value = value; item->result = result;
    memset(item->reserved, 0, sizeof(item->reserved));
#if defined(NP2AUDIO86_GUEST_TEST)
    if (g_io_cycle_observation_count == 0U)
        g_first_io_guest_cycle = g_state.guest_cycles;
    g_last_io_guest_cycle = g_state.guest_cycles;
    ++g_io_cycle_observation_count;
#endif
}

static int transaction_begin(uint32_t kind, size_t initial_bytes,
                             np2audio86_guest_transaction_t *transaction)
{
    np2audio86_guest_transaction_t candidate;
    if (transaction == NULL || g_failed) return -1;
    memset(&candidate, 0, sizeof(candidate));
    if (kind == NP2AUDIO86_GUEST_TRANSACTION_EVENT ||
        kind == NP2AUDIO86_GUEST_TRANSACTION_RESET) {
        if (g_trace && g_trace->event_count >= g_trace->event_capacity) {
            if (!g_sink) fail("event trace capacity");
            return NP2AUDIO86_GUEST_TRANSACTION_RETRY;
        }
    } else if (kind == NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN) {
        if (initial_bytes == 0U) {
            fail("PCM DATA_RUN length");
            return NP2AUDIO86_GUEST_TRANSACTION_CONTRACT;
        }
        if (g_trace && g_trace->pcm_count >= g_trace->pcm_capacity) {
            if (!g_sink) fail("PCM byte trace capacity");
            return NP2AUDIO86_GUEST_TRANSACTION_RETRY;
        }
        if (g_trace && g_trace->data_run_count >= g_trace->data_run_capacity) {
            if (!g_sink) fail("PCM DATA_RUN trace capacity");
            return NP2AUDIO86_GUEST_TRANSACTION_RETRY;
        }
    }
    if (g_sink) {
        const int result = g_sink->reserve_checked(g_sink->opaque, kind,
                                                   initial_bytes, &candidate);
        if (result != NP2AUDIO86_GUEST_TRANSACTION_OK) return result;
    }
    /* A rejected checked reservation must not even overwrite the retained
     * DATA_RUN token.  Publish the new opaque authorization to adapter state
     * only after its successful linearization point. */
    *transaction = candidate;
    return NP2AUDIO86_GUEST_TRANSACTION_OK;
}

static int transaction_extend(np2audio86_guest_transaction_t *transaction,
                              size_t additional_bytes)
{
    if (transaction == NULL || additional_bytes == 0U || g_failed)
        return NP2AUDIO86_GUEST_TRANSACTION_CONTRACT;
    if (g_trace && (additional_bytes > g_trace->pcm_capacity ||
                    g_trace->pcm_count > g_trace->pcm_capacity - additional_bytes)) {
        if (!g_sink) fail("PCM byte trace capacity");
        return NP2AUDIO86_GUEST_TRANSACTION_RETRY;
    }
    if (g_sink) return g_sink->extend_checked(g_sink->opaque, transaction,
                                               additional_bytes);
    return NP2AUDIO86_GUEST_TRANSACTION_OK;
}

static int prepare_event_transaction(uint32_t kind,
                                     np2audio86_guest_transaction_t *transaction)
{
    if (flush_pending_run() != 0 || g_state.sequence == UINT64_MAX) {
        if (g_state.sequence == UINT64_MAX) fail("event sequence overflow");
        return -1;
    }
    return transaction_begin(kind, 0U, transaction);
}

static void commit_event(np2audio86_guest_transaction_t *transaction,
                         uint32_t opcode, uint32_t payload)
{
    np2audio86_guest_event_t item;
    /* The caller has reserved capacity before the guest effect.  No commit
     * path below returns a recoverable condition. */
    item.frame_timestamp = g_state.frame_timestamp;
    item.sequence = g_state.sequence;
    item.opcode = opcode;
    item.payload = payload;
    if (g_trace) g_trace->events[g_trace->event_count++] = item;
    if (g_sink) {
        g_sink->commit_event(g_sink->opaque, transaction, &item);
        if (g_sink->commit_horizon)
            g_sink->commit_horizon(g_sink->opaque, transaction,
                                   item.frame_timestamp);
    }
    ++g_state.sequence;
}

static int flush_pending_run(void)
{
    np2audio86_guest_data_run_t run;
    if (!g_run_pending) return 0;
    if (!g_trace && !g_sink) {
        g_run_pending = 0; g_run_count = 0;
        return 0;
    }
    if (g_run_count == 0U || g_run_count > 32768U) {
        fail("PCM DATA_RUN length"); return -1;
    }
    run.frame_timestamp = g_run_timestamp;
    run.sequence = g_run_sequence;
    run.byte_offset = g_run_offset;
    run.count = (uint32_t)g_run_count;
    /* A DATA_RUN is reserved before its first byte is allowed to mutate
     * Domain G.  Keep this defensive guard as the final containment boundary
     * for host observation buffers, which are externally owned and can be
     * detached/re-attached around a pending run. */
    if (g_trace && g_trace->data_run_count >= g_trace->data_run_capacity) {
        fail("reserved PCM DATA_RUN trace capacity lost");
        return -1;
    }
    if (g_trace) g_trace->data_runs[g_trace->data_run_count++] = run;
    if (g_sink && g_run_transaction_active) {
        g_sink->commit_data_run(g_sink->opaque, &g_run_transaction, &run);
        if (g_sink->commit_horizon)
            g_sink->commit_horizon(g_sink->opaque, &g_run_transaction,
                                   run.frame_timestamp);
    }
    g_run_pending = 0; g_run_count = 0; g_run_transaction_active = 0;
    return 0;
}

static int prepare_byte_transaction(np2audio86_guest_transaction_t **transaction,
                                    uint8_t *starting)
{
    int result;
    *starting = 0U;
    if (g_failed) return -1;
    if (!g_run_pending || g_run_timestamp != g_state.frame_timestamp ||
        g_run_count >= 32768u) {
        if (flush_pending_run() != 0) return -1;
        if (g_state.sequence == UINT64_MAX) {
            fail("sequence overflow");
            return -1;
        }
        result = transaction_begin(NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN, 1U,
                                   &g_run_transaction);
        if (result != NP2AUDIO86_GUEST_TRANSACTION_OK) return result;
        g_run_transaction_active = g_sink != NULL;
        *starting = 1U;
    } else {
        result = transaction_extend(&g_run_transaction, 1U);
        if (result != NP2AUDIO86_GUEST_TRANSACTION_OK) return result;
    }
    *transaction = &g_run_transaction;
    return 0;
}

static void commit_pcm_byte(np2audio86_guest_transaction_t *transaction,
                            uint8_t starting, uint8_t value)
{
    if (starting) {
        g_run_pending = 1; g_run_timestamp = g_state.frame_timestamp;
        g_run_sequence = g_state.sequence++;
        g_run_offset = g_trace ? g_trace->pcm_offset_base + g_trace->pcm_count : 0U;
        g_run_count = 0U;
    }
    if (g_trace) {
        g_trace->pcm_bytes[g_trace->pcm_count++] = value;
    }
    if (g_sink && g_run_transaction_active) {
        g_sink->commit_pcm_byte(g_sink->opaque, transaction, g_run_timestamp,
                                g_run_sequence, value);
    }
    ++g_run_count;
}

static void timer_trace(uint8_t timer, uint8_t cause, uint8_t level,
                        uint8_t transition)
{
    np2audio86_guest_timer_trace_t *item;
    if (!g_trace || g_failed) return;
    if (g_trace->timer_count >= g_trace->timer_capacity) {
        fail("timer trace capacity"); return;
    }
    item = &g_trace->timers[g_trace->timer_count++];
    item->frame_timestamp = g_state.frame_timestamp;
    item->guest_cycles = g_state.guest_cycles; item->timer = timer;
    item->status = g_state.timer_status;
    item->irq = (uint8_t)(timer == NP2AUDIO86_TRACE_PCM ?
                          g_state.pcm_irq_line : g_state.opna_irq);
    item->level = level; item->cause = cause; item->pic_transition = transition;
    item->pcm_irqflag = g_state.pcm_irq; item->pcm_reqirq = g_state.pcm_reqirq;
}

static uint64_t timer_period(uint8_t timer)
{
    uint32_t l;
    if (timer == NP2AUDIO86_TRACE_TIMER_A)
        l = 18u * (1024u - (((uint32_t)g_state.regs[0x24] << 2) |
                            (g_state.regs[0x25] & 3u)));
    else l = 288u * (256u - g_state.regs[0x26]);
    if (g_cpumode & 0x20u) l = (l * 1248u) / 625u;
    else l = (l * 1536u) / 625u;
    return (uint64_t)l * (g_multiple ? g_multiple : 1u);
}

static uint8_t timer_is_work(uint8_t timer)
{
    return g_iswork ? g_iswork(timer) :
        (timer == 1u ? g_state.timer_a_running : timer == 2u ?
         g_state.timer_b_running : 0u);
}

static void schedule_event(uint8_t timer, uint64_t period, uint8_t absolute)
{
    if (!g_schedule) return;
    if (period > UINT32_MAX) { fail("event clock overflow"); return; }
    /* NEVENT's ABSOLUTE entry point consumes the period in the current
     * CPU slice, exactly as the upstream opntimer.c helper does. */
    g_schedule(timer, period, absolute);
}

static void cancel_event(uint8_t timer) { if (g_cancel) g_cancel(timer); }

static uint8_t pic_set_line(uint8_t *source, const uint8_t *other,
                            uint32_t irq)
{
    uint8_t was_asserted = *source;
    uint8_t transition;
    if (irq == 0xffu) return 0;
    if (other) was_asserted = (uint8_t)(was_asserted || *other);
    transition = (uint8_t)!was_asserted;
    if (transition && g_irq) g_irq(irq, 1);
    *source = 1;
    return transition;
}

static uint8_t pic_set(uint8_t cause)
{
    uint8_t transition = 0;
    uint8_t shared = (uint8_t)(g_state.opna_irq == g_state.pcm_irq_line);
    if (cause & 3u)
        transition |= pic_set_line(&g_state.opna_pic_source,
                                   shared ? &g_state.pcm_pic_source : NULL,
                                   g_state.opna_irq);
    if (cause & 4u)
        transition |= pic_set_line(&g_state.pcm_pic_source,
                                   shared ? &g_state.opna_pic_source : NULL,
                                   g_state.pcm_irq_line);
    return transition;
}

static uint8_t pic_reset_if_idle(uint8_t timer)
{
    uint8_t is_pcm = timer == NP2AUDIO86_TRACE_PCM;
    uint8_t *source = is_pcm ? &g_state.pcm_pic_source :
                               &g_state.opna_pic_source;
    uint32_t irq = is_pcm ? g_state.pcm_irq_line : g_state.opna_irq;
    uint8_t other_source;
    uint8_t had_source;
    uint8_t transitioned = 0;
    /* A PCM IRQ on a distinct line does not prevent the OPNA line from
     * being reset; only a pending source sharing that line keeps it high. */
    if (!is_pcm && g_state.timer_status) return 0;
    had_source = *source;
    *source = 0;
    if (is_pcm && g_state.timer_status &&
        g_state.pcm_irq_line == g_state.opna_irq) return 0;
    other_source = is_pcm ? g_state.opna_pic_source : g_state.pcm_pic_source;
    if (g_state.pcm_irq_line == g_state.opna_irq && other_source) return 0;
    if (irq != 0xffu && g_irq && had_source) {
        g_irq(irq, 0); transitioned = 1;
    }
    timer_trace(timer, 0, 0, transitioned ? 2u : 0u);
    return transitioned;
}

static uint64_t pcm_current_clock(void) { return current_cpu_position(); }

static void pcm_recalc_position(void)
{
    const uint64_t past_cycle = (uint64_t)UINT_MAX << 6;
    uint64_t cur, past, count;
    if (!g_state.pcm_rateval) return;
    cur = pcm_current_clock() << 6;
    if (!g_state.pcm_clock_valid) {
        g_state.pcm_lastclock = cur; g_state.pcm_clock_valid = 1;
        g_state.pcm_step_remainder = 0; return;
    }
    past = (cur + past_cycle - g_state.pcm_lastclock) % past_cycle;
    if (past > past_cycle / 2u) {
        if (past < past_cycle - g_state.pcm_stepclock * 4u) {
            past = 1; g_state.pcm_lastclock = cur - 1u;
        } else past = 0;
    }
    if (past >= g_state.pcm_stepclock) {
        count = past / g_state.pcm_stepclock;
        g_state.pcm_lastclock = (g_state.pcm_lastclock +
                                 count * g_state.pcm_stepclock) % past_cycle;
        g_state.pcm_step_remainder = (uint32_t)(past % g_state.pcm_stepclock);
        if (g_state.pcm_fifo & 0x80u) {
            uint64_t consumed = count << g_state.pcm_stepbit;
            if (consumed < g_state.pcm_virtual_buffer)
                g_state.pcm_virtual_buffer -= (uint32_t)consumed;
            else g_state.pcm_virtual_buffer &= g_state.pcm_stepmask;
        }
    } else g_state.pcm_step_remainder = (uint32_t)past;
}

static void pcm_set_rate(uint8_t rate_index)
{
    static const uint32_t rate8[8] = {352800u,264600u,176400u,132300u,
                                      88200u,66150u,44010u,33075u};
    static const uint32_t rescue[8] = {20u*32u,20u*24u,20u*16u,20u*12u,
                                       20u*8u,20u*6u,20u*4u,20u*3u};
    uint8_t index = rate_index & 7u;
    g_state.pcm_rateval = rate8[index];
    g_state.pcm_stepclock = ((uint64_t)g_baseclock << 6) /
                            g_state.pcm_rateval;
    g_state.pcm_stepclock *= ((uint64_t)g_multiple << 3);
    g_state.pcm_rescue = rescue[index] << g_state.pcm_stepbit;
}

static int64_t pcm_select_next_count(int64_t cntv, int64_t cntr,
                                     uint32_t real_buffer,
                                     uint16_t stepmask)
{
    /* pcm86.c's bufunferflag/vbufunferflag are written by the waveform
     * consumer.  They are deliberately not Domain-G state in this MVP.  With
     * both pinned histories at zero, every source branch selects cntv,
     * including the cntr < cntv branch. */
    if (real_buffer > stepmask && cntr < cntv) return cntv;
    return cntv;
}

static uint8_t pcmgen_intrq(uint8_t from_timer)
{
    /* GUEST_SEMANTICS_PRESERVED: only counter/IRQ predicates are retained;
     * waveform generation remains outside Domain G. */
    uint64_t now = pcm_current_clock(); (void)from_timer;
    if (!(g_state.pcm_fifo & 0x20u) ||
        now - g_state.pcm_lastclockforwait < 20000u * g_multiple) return 0;
    if (g_state.pcm_irq) return 1;
    if (!timer_is_work(NP2AUDIO86_TRACE_PCM)) {
        pcm_recalc_position();
        if (g_state.pcm_virtual_buffer <= g_state.pcm_fifo_size ||
            (g_state.pcm_real_buffer > g_state.pcm_stepmask &&
             g_state.pcm_real_buffer <= g_state.pcm_fifo_size)) {
            g_state.pcm_irq = 1; return 1;
        }
    }
    return 0;
}

static void pcm_set_next_interrupt(void)
{
    int64_t cntv, cntr, count; uint64_t clocks;
    const uint32_t *clock_table;
    if (!(g_state.pcm_fifo & 0x80u)) return;
    cntv = (int64_t)g_state.pcm_virtual_buffer - g_state.pcm_fifo_size;
    cntr = (int64_t)g_state.pcm_real_buffer - g_state.pcm_fifo_size;
    count = pcm_select_next_count(cntv, cntr, g_state.pcm_real_buffer,
                                  g_state.pcm_stepmask);
    if (count > 0) {
        count = (count + g_state.pcm_stepmask) >> g_state.pcm_stepbit;
        clock_table = (g_cpumode & 0x20u) ? g_pcm_clk20_128 : g_pcm_clk25_128;
        clocks = ((uint64_t)clock_table[g_state.pcm_fifo & 7u] *
                  (uint64_t)count) >> 7;
        clocks *= g_multiple;
        schedule_event(NP2AUDIO86_TRACE_PCM, clocks ? clocks : 1u, 1);
    } else if (g_state.pcm_reqirq) schedule_event(NP2AUDIO86_TRACE_PCM, 1u, 1);
    else { g_state.pcm_reqirq = 1; schedule_event(NP2AUDIO86_TRACE_PCM,
                                                      100u * g_multiple, 1); }
}

static void pcm_expire(void)
{
    /* WAVEFORM_SIDE_EFFECT_DEFERRED_TO_EVENT_ORACLE. */
    int64_t adjust;
    if (g_state.pcm_reqirq) {
        np2audio86_guest_audio_sync();
        adjust = ((int64_t)g_state.pcm_virtual_buffer * 4 +
                  g_state.pcm_real_buffer) / 5;
        if (g_state.pcm_virtual_buffer <= g_state.pcm_fifo_size ||
            (g_state.pcm_real_buffer > g_state.pcm_stepmask &&
             adjust <= g_state.pcm_fifo_size)) {
            g_state.pcm_reqirq = 0; g_state.pcm_irq = 1;
            if (g_state.pcm_irq_line != 0xffu) {
                timer_trace(NP2AUDIO86_TRACE_PCM, 4u, 1,
                            pic_set(4u) ? 1u : 0u);
            }
        } else pcm_set_next_interrupt();
    } else pcm_set_next_interrupt();
}

static void timer_expire(uint8_t timer)
{
    np2audio86_guest_transaction_t transaction;
    np2audio86_guest_sync_plan_t sync_plan;
    uint8_t bit = timer == NP2AUDIO86_TRACE_TIMER_A ? 1u : 2u;
    uint8_t enable = timer == NP2AUDIO86_TRACE_TIMER_A ?
        (uint8_t)((g_state.timer_control >> 2) & 1u) :
        (uint8_t)((g_state.timer_control >> 3) & 1u);
    uint8_t cause = 0, intreq = 0;
    int sync_status;
    const uint8_t csm = timer == NP2AUDIO86_TRACE_TIMER_A &&
                        (g_state.timer_control & 0xc0u) == 0x80u;
    if (csm) {
        if (flush_pending_run() != 0) return;
        sync_status = sync_plan_compute(&sync_plan);
        if (sync_status != 0) {
            if (!g_failed) fail_sync_plan(sync_status);
            return;
        }
        if (g_state.sequence == UINT64_MAX) {
            fail("event sequence overflow");
            return;
        }
        if (transaction_begin(NP2AUDIO86_GUEST_TRANSACTION_EVENT, 0U,
                              &transaction) != 0) return;
        /* Checked CSM authorization precedes all timer/PIC/nevent state. */
        sync_plan_apply(&sync_plan);
    } else {
        np2audio86_guest_audio_sync();
        if (g_failed) return;
    }
    if (g_state.pcm_irq_line == g_state.opna_irq) {
        if (pcmgen_intrq(1)) { cause |= 4u; intreq = 1; }
        if (!(g_state.timer_status & bit) && g_state.pcm_irq) {
            cause |= 4u;
            intreq = 1;
        }
    }
    if (enable && !(g_state.timer_status & bit)) {
        g_state.timer_status |= bit; cause |= bit; intreq = 1;
    }
    if (intreq && (((cause & 4u) && g_state.pcm_irq_line != 0xffu) ||
                   ((cause & 3u) && g_state.opna_irq != 0xffu))) {
        timer_trace(timer, cause, 1, pic_set(cause) ? 1u : 0u); /* PIC SET */
    } else timer_trace(timer, cause, 0, 0);
    /* Upstream uses a relative periodic reschedule even if disabled. */
    schedule_event(timer, timer_period(timer), 0);
    if (csm) {
        /* WAVEFORM_SIDE_EFFECT_DEFERRED_TO_EVENT_ORACLE. */
        commit_event(&transaction, NP2AUDIO86_TRACE_OPNA_CSM, 0);
    }
}

static void sync_plan_recalc_pcm(np2audio86_guest_sync_plan_t *plan)
{
    const uint64_t past_cycle = (uint64_t)UINT_MAX << 6;
    uint64_t cur, past, count;
    if (!g_state.pcm_rateval) return;
    cur = (uint64_t)plan->cpu_position << 6;
    if (!plan->pcm_clock_valid) {
        plan->pcm_lastclock = cur;
        plan->pcm_clock_valid = 1U;
        plan->pcm_step_remainder = 0U;
        return;
    }
    past = (cur + past_cycle - plan->pcm_lastclock) % past_cycle;
    if (past > past_cycle / 2U) {
        if (past < past_cycle - g_state.pcm_stepclock * 4U) {
            past = 1U;
            plan->pcm_lastclock = cur - 1U;
        } else {
            past = 0U;
        }
    }
    if (past >= g_state.pcm_stepclock) {
        count = past / g_state.pcm_stepclock;
        plan->pcm_lastclock = (plan->pcm_lastclock +
                               count * g_state.pcm_stepclock) % past_cycle;
        plan->pcm_step_remainder = (uint32_t)(past % g_state.pcm_stepclock);
        if (g_state.pcm_fifo & 0x80U) {
            const uint64_t consumed = count << g_state.pcm_stepbit;
            if (consumed < plan->pcm_virtual_buffer)
                plan->pcm_virtual_buffer -= (uint32_t)consumed;
            else
                plan->pcm_virtual_buffer &= g_state.pcm_stepmask;
        }
    } else {
        plan->pcm_step_remainder = (uint32_t)past;
    }
}

static int sync_plan_compute(np2audio86_guest_sync_plan_t *plan)
{
    uint32_t delta;
    uint64_t total;
    if (plan == NULL) return -1;
    plan->cpu_position = current_cpu_position();
    plan->last_cpu_position = g_state.last_cpu_position;
    plan->pcm_step_remainder = g_state.pcm_step_remainder;
    plan->pcm_virtual_buffer = g_state.pcm_virtual_buffer;
    plan->frame_remainder = g_state.frame_remainder;
    plan->pcm_lastclock = g_state.pcm_lastclock;
    plan->guest_cycles = g_state.guest_cycles;
    plan->frame_timestamp = g_state.frame_timestamp;
    plan->cpu_position_valid = g_state.cpu_position_valid;
    plan->pcm_clock_valid = g_state.pcm_clock_valid;
    if (!plan->cpu_position_valid) {
        plan->last_cpu_position = plan->cpu_position;
        plan->cpu_position_valid = 1U;
        plan->pcm_lastclock = (uint64_t)plan->cpu_position << 6;
        plan->pcm_clock_valid = 1U;
        return 0;
    }
    delta = (uint32_t)(plan->cpu_position - plan->last_cpu_position);
    plan->last_cpu_position = plan->cpu_position;
    if (plan->guest_cycles > UINT64_MAX - delta) return -1;
    plan->guest_cycles += delta;
    sync_plan_recalc_pcm(plan);
    total = (uint64_t)plan->frame_remainder + delta;
    if (plan->frame_timestamp > UINT64_MAX - total / 1024U) return -2;
    plan->frame_timestamp += total / 1024U;
    plan->frame_remainder = (uint32_t)(total % 1024U);
    return 0;
}

/* The prepared legacy PCM handlers used to sample guest time immediately
 * before entering the adapter for 0x06/0x08/0x0a.  Preserve that established
 * guest-time sampling contract without applying either sample before checked
 * authorization: both samples remain prospective and are applied once. */
static int sync_plan_compute_following(np2audio86_guest_sync_plan_t *plan)
{
    uint32_t delta;
    uint64_t total;
    if (plan == NULL) return -1;
    plan->cpu_position = current_cpu_position();
    delta = (uint32_t)(plan->cpu_position - plan->last_cpu_position);
    plan->last_cpu_position = plan->cpu_position;
    if (plan->guest_cycles > UINT64_MAX - delta) return -1;
    plan->guest_cycles += delta;
    sync_plan_recalc_pcm(plan);
    total = (uint64_t)plan->frame_remainder + delta;
    if (plan->frame_timestamp > UINT64_MAX - total / 1024U) return -2;
    plan->frame_timestamp += total / 1024U;
    plan->frame_remainder = (uint32_t)(total % 1024U);
    return 0;
}

static void sync_plan_apply(const np2audio86_guest_sync_plan_t *plan)
{
    g_state.last_cpu_position = plan->last_cpu_position;
    g_state.pcm_step_remainder = plan->pcm_step_remainder;
    g_state.pcm_virtual_buffer = plan->pcm_virtual_buffer;
    g_state.frame_remainder = plan->frame_remainder;
    g_state.pcm_lastclock = plan->pcm_lastclock;
    g_state.guest_cycles = plan->guest_cycles;
    g_state.frame_timestamp = plan->frame_timestamp;
    g_state.cpu_position_valid = plan->cpu_position_valid;
    g_state.pcm_clock_valid = plan->pcm_clock_valid;
}

static void fail_sync_plan(int status)
{
    if (status == -1) fail("guest cycle overflow");
    else if (status == -2) fail("frame timestamp overflow");
    else fail("guest audio sync overflow");
}

void np2audio86_guest_audio_sync(void)
{
    np2audio86_guest_sync_plan_t plan;
    const int status = sync_plan_compute(&plan);
    if (status != 0) { fail_sync_plan(status); return; }
    sync_plan_apply(&plan);
}

void np2audio86_guest_host_trace_attach(np2audio86_guest_trace_t *trace)
{
    g_trace = trace;
    if (g_trace) { g_trace->event_count = 0; g_trace->data_run_count = 0;
        g_trace->pcm_count = 0; g_trace->timer_count = 0; g_trace->io_count = 0; }
}
void np2audio86_guest_host_trace_detach(void) { flush_pending_run(); g_trace = NULL; }
void np2audio86_guest_sink_bind(const np2audio86_guest_sink_t *sink)
{
    if (sink &&
        (!sink->reserve_checked || !sink->extend_checked || !sink->commit_event ||
         !sink->commit_pcm_byte || !sink->commit_data_run ||
         !sink->commit_horizon)) {
        g_sink = NULL;
        fail("transaction sink contract");
        return;
    }
    g_sink = sink;
}
void np2audio86_guest_sink_unbind(void) { g_sink = NULL; }
void np2audio86_guest_host_set_cpu_position_fn(np2audio86_guest_cpu_position_fn fn)
{ g_cpu_position = fn; }
void np2audio86_guest_host_set_cpu_position(uint32_t position)
{ g_manual_cpu_position = position; }
#if defined(NP2AUDIO86_GUEST_TEST)
uint32_t np2audio86_guest_host_current_cpu_position(void)
{ return current_cpu_position(); }
int np2audio86_guest_host_sink_is_bound(void)
{ return g_sink != NULL; }
size_t np2audio86_guest_test_full_snapshot_size(void)
{
    return sizeof(g_state) + sizeof(g_run_timestamp) + sizeof(g_run_sequence) +
           sizeof(g_run_offset) + sizeof(g_run_count) + sizeof(g_run_transaction) +
           sizeof(g_run_pending) + sizeof(g_run_transaction_active) +
           sizeof(g_semantic_call_active) + sizeof(g_contract_violation) +
           sizeof(g_termination_pending) + sizeof(g_failed);
}
int np2audio86_guest_test_full_snapshot(void *out, size_t bytes)
{
    uint8_t *cursor = out;
    if (out == NULL || bytes != np2audio86_guest_test_full_snapshot_size()) return -1;
    memcpy(cursor, &g_state, sizeof(g_state)); cursor += sizeof(g_state);
    memcpy(cursor, &g_run_timestamp, sizeof(g_run_timestamp)); cursor += sizeof(g_run_timestamp);
    memcpy(cursor, &g_run_sequence, sizeof(g_run_sequence)); cursor += sizeof(g_run_sequence);
    memcpy(cursor, &g_run_offset, sizeof(g_run_offset)); cursor += sizeof(g_run_offset);
    memcpy(cursor, &g_run_count, sizeof(g_run_count)); cursor += sizeof(g_run_count);
    memcpy(cursor, &g_run_transaction, sizeof(g_run_transaction)); cursor += sizeof(g_run_transaction);
    *cursor++ = g_run_pending;
    *cursor++ = g_run_transaction_active;
    *cursor++ = g_semantic_call_active;
    *cursor++ = g_contract_violation;
    *cursor++ = g_termination_pending;
    *cursor = g_failed;
    return 0;
}
uint8_t np2audio86_guest_test_contract_violation(void)
{ return g_contract_violation; }
void np2audio86_guest_test_reset_io_cycle_observation(void)
{
    g_io_cycle_observation_count = 0U;
    g_first_io_guest_cycle = 0U;
    g_last_io_guest_cycle = 0U;
}
size_t np2audio86_guest_test_io_cycle_observation_count(void)
{ return g_io_cycle_observation_count; }
uint64_t np2audio86_guest_test_first_io_guest_cycle(void)
{ return g_first_io_guest_cycle; }
uint64_t np2audio86_guest_test_last_io_guest_cycle(void)
{ return g_last_io_guest_cycle; }
#endif
void np2audio86_guest_host_set_clock(uint32_t baseclock, uint32_t multiple)
{
    if ((uint64_t)baseclock * multiple != 49152000u) {
        fail("unsupported guest clock"); return;
    }
    if (g_state.bound && (baseclock != g_baseclock || multiple != g_multiple)) {
        fail("dynamic clock change unsupported"); return;
    }
    g_baseclock = baseclock; g_multiple = multiple;
    if (g_state.pcm_rateval) pcm_set_rate(g_state.pcm_fifo);
}
void np2audio86_guest_host_set_cpumode(uint32_t cpumode) { g_cpumode = cpumode; }
void np2audio86_guest_host_set_timer_hooks(
    np2audio86_guest_timer_schedule_fn schedule,
    np2audio86_guest_timer_cancel_fn cancel,
    np2audio86_guest_timer_iswork_fn iswork,
    np2audio86_guest_irq_fn irq)
{ g_schedule = schedule; g_cancel = cancel; g_iswork = iswork; g_irq = irq; }
void np2audio86_guest_host_timer_dispatch(uint8_t timer)
{
    if (semantic_enter() != 0) return;
    if (timer == NP2AUDIO86_TRACE_PCM) {
        np2audio86_guest_audio_sync();
        if (!g_failed) pcm_expire();
    } else if (timer == NP2AUDIO86_TRACE_TIMER_A || timer == NP2AUDIO86_TRACE_TIMER_B) {
        timer_expire(timer);
    }
    semantic_leave();
}
void np2audio86_guest_host_timer_tick(uint8_t timer)
{ np2audio86_guest_host_timer_dispatch(timer); }
void np2audio86_guest_host_flush_data_run(void) { flush_pending_run(); }
void np2audio86_guest_host_test_seed(uint64_t frame_timestamp, uint64_t sequence)
{
    flush_pending_run(); g_state.frame_timestamp = frame_timestamp;
    g_state.sequence = sequence; g_state.guest_cycles = 0;
    g_state.frame_remainder = 0; g_state.cpu_position_valid = 0;
    g_state.pcm_clock_valid = 0; g_failed = 0; g_failure_reason[0] = '\0';
    g_semantic_call_active = 0U;
    g_contract_violation = 0U;
    g_termination_pending = 0U;
}

void np2audio86_guest_host_snapshot(np2audio86_guest_state_snapshot_t *snapshot)
{
    if (!snapshot) return;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->frame_timestamp = g_state.frame_timestamp;
    snapshot->guest_cycles = g_state.guest_cycles; snapshot->sequence = g_state.sequence;
    snapshot->cpu_remainder = g_state.frame_remainder; snapshot->opna_base = g_state.base;
    snapshot->opna_address_low = g_state.address_low;
    snapshot->opna_address_extended = g_state.address_extended;
    snapshot->opna_data = g_state.data; snapshot->opna_extension = g_state.extension;
    snapshot->opna_capabilities = g_state.capabilities; snapshot->opna_status = g_state.timer_status;
    snapshot->timer_control = g_state.timer_control; snapshot->timer_a_value = g_state.timer_a_value;
    snapshot->timer_b_value = g_state.timer_b_value; snapshot->timer_irq = (uint8_t)g_state.opna_irq;
    snapshot->pcm_soundflags = g_state.pcm_soundflags; snapshot->pcm_fifo = g_state.pcm_fifo;
    snapshot->pcm_dactrl = g_state.pcm_dactrl; snapshot->pcm_volume = g_state.pcm_volume;
    snapshot->pcm_rate = (uint8_t)(g_state.pcm_fifo & 7u); snapshot->pcm_fifo_size = g_state.pcm_fifo_size;
    snapshot->pcm_fifo_level = (uint16_t)(g_state.pcm_virtual_buffer > UINT16_MAX ?
                                          UINT16_MAX : g_state.pcm_virtual_buffer);
    snapshot->pcm_virtual_buffer = g_state.pcm_virtual_buffer;
    snapshot->pcm_read_position = g_state.pcm_read_position; snapshot->pcm_irq = g_state.pcm_irq;
    snapshot->pcm_reqirq = g_state.pcm_reqirq; snapshot->pcm_rescue = g_state.pcm_rescue;
    snapshot->pcm_irq_line = (uint8_t)g_state.pcm_irq_line;
    snapshot->pcm_stepbit = g_state.pcm_stepbit;
    snapshot->pcm_stepmask = g_state.pcm_stepmask;
    snapshot->pcm_rateval = g_state.pcm_rateval; snapshot->pcm_stepclock = g_state.pcm_stepclock;
    snapshot->pcm_lastclock = g_state.pcm_lastclock;
    snapshot->pcm_lastclockforwait = g_state.pcm_lastclockforwait;
    snapshot->pcm_real_buffer = g_state.pcm_real_buffer;
    snapshot->pcm_write_position = g_state.pcm_write_position;
    snapshot->pcm_step_remainder = g_state.pcm_step_remainder;
    snapshot->soundrom_rejected = g_state.soundrom_rejected; snapshot->bound = g_state.bound;
}
size_t np2audio86_guest_host_state_size(void) { return sizeof(g_state); }
uint8_t np2audio86_guest_host_failed(void) { return g_failed; }
const char *np2audio86_guest_host_failure_reason(void) { return g_failure_reason; }
uint8_t np2audio86_guest_host_save_load_supported(void) { return 0; }
void np2audio86_guest_host_record_io(uint16_t port, uint8_t direction,
                                     uint8_t value, uint8_t result)
{ np2audio86_guest_audio_sync(); append_io(port, direction, value, result); }
void np2audio86_guest_host_record_io_accepted(uint16_t port, uint8_t direction,
                                              uint8_t value, uint8_t result)
{ append_io(port, direction, value, result); }

void np2audio86_guest_opna_write_address_low(uint8_t value)
{ g_state.address_low = value; g_state.data = value; }

static int opna_write_register(uint16_t address, uint8_t value)
{
    np2audio86_guest_transaction_t transaction;
    if (prepare_event_transaction(NP2AUDIO86_GUEST_TRANSACTION_EVENT,
                                  &transaction) != 0) return 0;
    /* The checked call above is the success linearization point.  The data
     * latch is therefore not visible when a finite sink/control rejects the
     * operation. */
    g_state.data = value;
    g_state.regs[address & 0x1ffu] = value;
    if (address < 0x100u) {
        if (address == 0x24u)
            g_state.timer_a_value = (uint16_t)((g_state.regs[0x24] << 2) |
                                               (g_state.regs[0x25] & 3u));
        else if (address == 0x25u)
            g_state.timer_a_value = (uint16_t)((g_state.regs[0x24] << 2) |
                                               (value & 3u));
        else if (address == 0x26u) g_state.timer_b_value = value;
        else if (address == 0x27u) {
            uint8_t old = g_state.timer_control;
            uint8_t persistent = (uint8_t)(value & (uint8_t)~0x30u);
            if (value & 0x10u) g_state.timer_status &= (uint8_t)~1u;
            if (value & 0x20u) g_state.timer_status &= (uint8_t)~2u;
            g_state.timer_control = persistent; g_state.regs[0x27] = persistent;
            g_state.timer_a_running = value & 1u; g_state.timer_b_running = (value >> 1) & 1u;
            if ((value & 1u) && !(old & 1u) && !timer_is_work(1)) schedule_event(1, timer_period(1), 1);
            else if (!(value & 1u) && (old & 1u)) cancel_event(1);
            if ((value & 2u) && !(old & 2u) && !timer_is_work(2)) schedule_event(2, timer_period(2), 1);
            else if (!(value & 2u) && (old & 2u)) cancel_event(2);
            if (!(value & 3u) || (value & 0x30u)) pic_reset_if_idle(0);
        }
    }
    commit_event(&transaction, NP2AUDIO86_TRACE_OPNA_REGISTER,
                 ((uint32_t)address << 8) | value);
    return 1;
}
void np2audio86_guest_opna_write_data_low(uint8_t value)
{
    if (semantic_enter() != 0) return;
    (void)opna_write_register(g_state.address_low, value);
    semantic_leave();
}
void np2audio86_guest_opna_write_address_extended(uint8_t value)
{ if (g_state.extension) { g_state.address_extended = value; g_state.data = value; } }
void np2audio86_guest_opna_write_data_extended(uint8_t value)
{
    if (!g_state.extension || semantic_enter() != 0) return;
    (void)opna_write_register((uint16_t)(0x100u | g_state.address_extended), value);
    semantic_leave();
}
uint8_t np2audio86_guest_opna_read_status(void)
{ flush_pending_run(); return g_state.timer_status; }
uint8_t np2audio86_guest_opna_read_data(void)
{
    flush_pending_run();
    if (g_state.address_low == 0x0e) return g_state.joy;
    if (g_state.address_low < 0x10) return g_state.regs[g_state.address_low];
    if (g_state.address_low == 0xff) return 1;
    return g_state.data;
}
uint8_t np2audio86_guest_opna_read_extended_status(void)
{ flush_pending_run(); return g_state.extension ? g_state.timer_status : 0xff; }
uint8_t np2audio86_guest_opna_read_extended_data(void)
{
    flush_pending_run(); if (!g_state.extension) return 0xff;
    if (g_state.address_extended == 0x08 || g_state.address_extended == 0x0f)
        return g_state.regs[0x100u | g_state.address_extended];
    return g_state.data;
}
uint8_t np2audio86_guest_opna_read_joy(void)
{ flush_pending_run(); return g_state.joy; }
void np2audio86_guest_opna_set_extension(uint8_t enabled)
{ g_state.extension = enabled ? 1u : 0u; if (g_extension_callback) g_extension_callback(g_state.extension); }

void np2audio86_guest_opna_reset(uint8_t capabilities, uint32_t irq,
                                 uint8_t timer_a_event, uint8_t timer_b_event)
{
    uint64_t frame = g_state.frame_timestamp, cycles = g_state.guest_cycles;
    uint64_t sequence = g_state.sequence; uint32_t remainder = g_state.frame_remainder;
    uint8_t had_state = g_state.bound;
    np2audio86_guest_transaction_t transaction;
    if (had_state) {
        if (semantic_enter() != 0) return;
        if (prepare_event_transaction(NP2AUDIO86_GUEST_TRANSACTION_RESET,
                                      &transaction) != 0) {
            semantic_leave();
            return;
        }
    }
    cancel_event(1); cancel_event(2); cancel_event(NP2AUDIO86_TRACE_PCM);
    memset(&g_state, 0, sizeof(g_state));
    g_state.frame_timestamp = frame; g_state.guest_cycles = cycles;
    g_state.sequence = sequence; g_state.frame_remainder = remainder;
    static const uint8_t irq_table[4] = {0x03, 0x0d, 0x0a, 0x0c};
    g_state.capabilities = capabilities;
    g_state.opna_irq = (irq & 0x10u) ? irq_table[(irq >> 6) & 3u] : 0xffu;
    g_state.pcm_irq_line = 0xffu;
    g_state.timer_a_event = timer_a_event; g_state.timer_b_event = timer_b_event;
    g_state.pcm_fifo_size = 0x80u; g_state.pcm_dactrl = 0x32u;
    g_state.pcm_stepbit = 2u; g_state.pcm_stepmask = 3u; g_state.bound = had_state;
    pcm_set_rate(0); g_failed = 0; g_failure_reason[0] = '\0';
    g_run_pending = 0; g_run_count = 0; g_run_transaction_active = 0;
    g_termination_pending = 0U;
    if (had_state) {
        commit_event(&transaction, NP2AUDIO86_TRACE_RESET_BARRIER, 0);
        semantic_leave();
    }
}
void np2audio86_guest_opna_set_config(uint8_t channels, uint32_t mode)
{ g_state.channels = channels; (void)mode; }
void np2audio86_guest_opna_set_base(uint16_t base) { g_state.base = base; }
uint16_t np2audio86_guest_opna_base(void) { return g_state.base; }
void np2audio86_guest_opna_register_extension(void (*callback)(uint8_t enabled))
{ g_extension_callback = callback; }
void np2audio86_guest_opna_bind(void) { g_state.bound = 1; }
void np2audio86_guest_opna_unbind(void) { g_state.bound = 0; }
void np2audio86_guest_soundrom_load(uint32_t address, const char *name)
{ (void)address; (void)name; g_state.soundrom_rejected = 1; }

int np2audio86_guest_pcm86_write(uint8_t register_index, uint8_t value)
{
    uint8_t old;
    uint64_t cur;
    int accepted = 0;
    static const uint8_t bits[8] = {1,1,1,2,0,0,0,1};
    np2audio86_guest_transaction_t transaction;
    np2audio86_guest_sync_plan_t sync_plan;
    int sync_status;
    if (semantic_enter() != 0) return 0;
    /* Pinned pcm86_oa46a() synchronizes guest time before rejecting an
     * invalid DAC format.  The rejected write is a true no-op at the
     * publication boundary: do not flush a pending data run, append a
     * control event, or reach the reqirq reschedule below. */
    if (register_index == 0x0au && !(g_state.pcm_fifo & 0x20u) &&
        ((value & 15u) == 15u)) {
        np2audio86_guest_audio_sync();
        semantic_leave();
        return g_failed ? 0 : 1;
    }
    if (flush_pending_run() != 0) {
        semantic_leave();
        return 0;
    }
    sync_status = sync_plan_compute(&sync_plan);
    if (sync_status == 0 &&
        (register_index == 0x06U || register_index == 0x08U ||
         register_index == 0x0aU)) {
        sync_status = sync_plan_compute_following(&sync_plan);
    }
    if (sync_status != 0) {
        if (!g_failed) fail_sync_plan(sync_status);
        semantic_leave();
        return 0;
    }
    if (prepare_event_transaction(NP2AUDIO86_GUEST_TRANSACTION_EVENT,
                                  &transaction) != 0) {
        semantic_leave();
        return 0;
    }
    /* No guest time/accounting field changes before the checked success. */
    sync_plan_apply(&sync_plan);
    switch (register_index) {
    case 0x00:
        g_state.pcm_soundflags = (uint8_t)((g_state.pcm_soundflags & 0xfeu) | (value & 1u));
        g_state.extension = value & 1u;
        if (g_extension_callback) g_extension_callback(g_state.extension);
        break;
    case 0x06: if ((value & 0xe0u) == 0xa0u) g_state.pcm_volume = (~value) & 15u; break;
    case 0x08:
        old = g_state.pcm_fifo; cur = sync_plan.cpu_position;
        if ((value & 8u) && !(old & 8u)) {
            g_state.pcm_write_position = g_state.pcm_read_position = 0;
            g_state.pcm_real_buffer = g_state.pcm_virtual_buffer = 0;
            g_state.pcm_lastclock = cur << 6; g_state.pcm_lastclockforwait = cur;
            g_state.pcm_clock_valid = 1;
        }
        if (!(value & 0x10u)) {
            g_state.pcm_irq = 0;
            if (!g_state.pcm_virtual_buffer) g_state.pcm_lastclockforwait = cur;
        }
        if (g_state.pcm_virtual_buffer <= g_state.pcm_fifo_size) g_state.pcm_irq = 1;
        if ((old ^ value) & 7u) pcm_set_rate(value);
        g_state.pcm_fifo = value;
        if ((old ^ value) & 0x80u && (value & 0x80u)) {
            g_state.pcm_lastclock = cur << 6; g_state.pcm_clock_valid = 1;
        }
        if (g_state.pcm_reqirq) pcm_set_next_interrupt();
        if (!(value & 0x10u)) pic_reset_if_idle(NP2AUDIO86_TRACE_PCM);
        break;
    case 0x0a:
        if (g_state.pcm_fifo & 0x20u)
            g_state.pcm_fifo_size = value == 0xffu ? 0x7ffcu : (uint16_t)((value + 1u) << 7);
        else if ((value & 15u) != 15u) {
            g_state.pcm_dactrl = value; g_state.pcm_stepbit = bits[(value >> 4) & 7u];
            g_state.pcm_stepmask = (uint16_t)((1u << g_state.pcm_stepbit) - 1u);
            pcm_set_rate(g_state.pcm_fifo);
        }
        if (g_state.pcm_reqirq) pcm_set_next_interrupt();
        break;
    default: break;
    }
    commit_event(&transaction, NP2AUDIO86_TRACE_PCM_CONTROL,
                 ((uint32_t)register_index << 8) | value);
    accepted = 1;
    semantic_leave();
    return accepted;
}
int np2audio86_guest_pcm86_write_data(uint8_t value)
{
    np2audio86_guest_transaction_t *transaction;
    uint8_t starting;
    int result;
    uint64_t cur = pcm_current_clock(), wait = 20000u * (uint64_t)g_multiple;
    uint64_t add_clock = 0;
    if (semantic_enter() != 0) return 0;
    result = prepare_byte_transaction(&transaction, &starting);
    if (result != NP2AUDIO86_GUEST_TRANSACTION_OK) {
        if (result == NP2AUDIO86_GUEST_TRANSACTION_TERMINATED && g_run_pending) {
            /* A later STOP cannot discard already-authorized bytes. */
            if (flush_pending_run() == 0) g_termination_pending = 1U;
        }
        semantic_leave();
        return 0;
    }
    if (g_state.pcm_virtual_buffer < 0x8000u) ++g_state.pcm_virtual_buffer;
    ++g_state.pcm_real_buffer;
    if (g_state.pcm_real_buffer >= 0x8000u + g_state.pcm_rescue) {
        g_state.pcm_real_buffer -= 4u; g_state.pcm_read_position = (g_state.pcm_read_position + 4u) & 0xffffu;
    }
    g_state.pcm_write_position = (g_state.pcm_write_position + 1u) & 0xffffu;
    g_state.pcm_reqirq = 1;
    if (g_state.pcm_fifo_size < 8192u)
        add_clock = wait - (wait * g_state.pcm_fifo_size) / 8192u;
    if (g_state.pcm_virtual_buffer > (uint32_t)g_state.pcm_fifo_size * 2u ||
        g_state.pcm_virtual_buffer >= 0x8000u)
        add_clock = wait;
    g_state.pcm_lastclockforwait = cur + add_clock;
    commit_pcm_byte(transaction, starting, value);
    semantic_leave();
    return 1;
}
void np2audio86_guest_pcm86_set_mixer_volume(uint8_t value) { g_state.pcm_volume = value & 15u; }
uint8_t np2audio86_guest_pcm86_read(uint8_t register_index)
{
    uint8_t value; flush_pending_run(); np2audio86_guest_audio_sync();
    switch (register_index) {
    case 0x00: value = g_state.pcm_soundflags; break;
    case 0x06: {
        uint64_t cur = pcm_current_clock() << 6, pc = (uint64_t)UINT_MAX << 6;
        uint64_t past = (cur + pc - g_state.pcm_lastclock) % pc;
        value = (past << 1) >= g_state.pcm_stepclock ? 1u : 0u;
        if (g_state.pcm_virtual_buffer >= 0x8000u) value |= 0x80u;
        else if (g_state.pcm_virtual_buffer <= g_state.pcm_stepmask) value |= 0x40u;
        break;
    }
    case 0x08: value = g_state.pcm_fifo & (uint8_t)~0x10u; if (pcmgen_intrq(0)) value |= 0x10u; break;
    case 0x0a: value = g_state.pcm_dactrl; break;
    default: value = 0; break;
    }
    return value;
}
void np2audio86_guest_pcm86_set_options(uint8_t dip_switch)
{
    static const uint8_t irq_table[8] = {0xff,0xff,0xff,0xff,0x03,0x0a,0x0d,0x0c};
    g_state.pcm_soundflags = (uint8_t)(((~dip_switch) >> 1) & 0x70u);
    g_state.pcm_irq_line = irq_table[(dip_switch >> 2) & 7u];
}
void np2audio86_guest_pcm86_stream_bind(void) { g_state.bound = 1; }
void np2audio86_guest_pcm86_stream_unbind(void) { g_state.bound = 0; flush_pending_run(); }

#if defined(NP2AUDIO86_GUEST_TEST)
void np2audio86_guest_test_set_pcm_state(uint32_t virtual_buffer,
                                         uint32_t real_buffer,
                                         uint16_t fifo_size, uint8_t fifo,
                                         uint8_t stepbit, uint8_t reqirq,
                                         uint8_t irqflag,
                                         uint64_t lastclock)
{
    g_state.pcm_virtual_buffer = virtual_buffer;
    g_state.pcm_real_buffer = real_buffer;
    g_state.pcm_fifo_size = fifo_size;
    g_state.pcm_fifo = fifo;
    g_state.pcm_stepbit = stepbit;
    g_state.pcm_stepmask = (uint16_t)((1u << stepbit) - 1u);
    g_state.pcm_reqirq = reqirq;
    g_state.pcm_irq = irqflag;
    g_state.pcm_lastclock = lastclock;
    g_state.pcm_clock_valid = 1;
    pcm_set_rate(fifo);
}

void np2audio86_guest_test_schedule_pcm(void) { pcm_set_next_interrupt(); }
#endif
