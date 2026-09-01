/*
 * Project-owned PC-9801-86 guest-domain boundary.
 *
 * The production side of this interface contains only bounded guest state.
 * Host tests may attach an explicitly sized observation buffer; those buffers
 * are not part of the guest state and are never used as a waveform store.
 */
#ifndef NP2AUDIO86_GUEST_ADAPTER_H
#define NP2AUDIO86_GUEST_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NP2AUDIO86_OPNA_CAPS_2608 = 0x01,
    NP2AUDIO86_OPNA_CAPS_TIMER = 0x02,
    NP2AUDIO86_OPNA_CAPS_S98 = 0x04,
    NP2AUDIO86_OPNA_CAPS_ADPCM = 0x08,
};

enum {
    NP2AUDIO86_TRACE_OPNA_REGISTER = 1,
    NP2AUDIO86_TRACE_OPNA_CSM = 2,
    NP2AUDIO86_TRACE_PCM_CONTROL = 3,
    NP2AUDIO86_TRACE_RESET_BARRIER = 0x80000000u,
    NP2AUDIO86_TRACE_TIMER_A = 1,
    NP2AUDIO86_TRACE_TIMER_B = 2,
    NP2AUDIO86_TRACE_PCM = 3,
};

typedef struct {
    uint64_t frame_timestamp;
    uint64_t sequence;
    uint32_t opcode;
    uint32_t payload;
} np2audio86_guest_event_t;

typedef struct {
    uint64_t frame_timestamp;
    uint64_t sequence;
    uint64_t byte_offset;
    uint32_t count;
} np2audio86_guest_data_run_t;

/* A token is allocated by the caller and filled only by the bound sink.  Its
 * representation is intentionally opaque to the adapter: it conveys one
 * exclusive, single-producer reservation rather than transport addresses or
 * FreeRTOS state. */
typedef struct {
    uintptr_t opaque[4];
} np2audio86_guest_transaction_t;

enum {
    NP2AUDIO86_GUEST_TRANSACTION_EVENT = 1,
    NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN = 2,
    NP2AUDIO86_GUEST_TRANSACTION_RESET = 3,
};

/* A narrow semantic publication seam.  The guest remains the sole owner of
 * sequence, timestamp, pending-run, timer, and PCM accounting state.  A sink
 * may synchronously block a semantic handler, which is the required boundary
 * for a lossless live producer; it never owns or mutates guest state.
 *
 * reserve/extend are the only recoverable operations.  Once reserve succeeds
 * and the adapter performs the guest effect, every commit callback is void:
 * the sink has already guaranteed the event/byte/horizon capacity.  cancel is
 * valid only before that guest effect.  The legacy callbacks are retained
 * temporarily for pre-transaction host consumers; new sinks must implement
 * the transaction callbacks. */
typedef struct {
    void *opaque;
    int (*reserve)(void *opaque, uint32_t kind, size_t initial_bytes,
                   np2audio86_guest_transaction_t *transaction);
    int (*extend)(void *opaque, np2audio86_guest_transaction_t *transaction,
                  size_t additional_bytes);
    int (*recheck)(void *opaque,
                   const np2audio86_guest_transaction_t *transaction);
    void (*cancel)(void *opaque, np2audio86_guest_transaction_t *transaction);
    void (*commit_event)(void *opaque, np2audio86_guest_transaction_t *transaction,
                         const np2audio86_guest_event_t *event);
    void (*commit_pcm_byte)(void *opaque,
                            np2audio86_guest_transaction_t *transaction,
                            uint64_t frame_timestamp, uint64_t sequence,
                            uint8_t value);
    void (*commit_data_run)(void *opaque,
                            np2audio86_guest_transaction_t *transaction,
                            const np2audio86_guest_data_run_t *run);
    void (*commit_horizon)(void *opaque,
                           np2audio86_guest_transaction_t *transaction,
                           uint64_t frame_timestamp);
    /* Compatibility-only callbacks.  They are used only by sinks which have
     * not opted into reserve; production Slice 2 must bind the transaction
     * half above. */
    int (*publish_event)(void *opaque, const np2audio86_guest_event_t *event);
    int (*publish_pcm_byte)(void *opaque, uint64_t frame_timestamp,
                            uint64_t sequence, uint8_t value);
    int (*publish_data_run)(void *opaque,
                            const np2audio86_guest_data_run_t *run);
} np2audio86_guest_sink_t;

typedef struct {
    uint64_t frame_timestamp;
    uint64_t guest_cycles;
    uint32_t timer;
    uint8_t status;
    uint8_t irq;
    uint8_t level;
    uint8_t cause;
    uint8_t pic_transition;
    uint8_t pcm_irqflag;
    uint8_t pcm_reqirq;
} np2audio86_guest_timer_trace_t;

typedef struct {
    uint64_t frame_timestamp;
    uint64_t sequence;
    uint16_t port;
    uint8_t direction; /* 0 = IN, 1 = OUT */
    uint8_t value;
    uint8_t result;
    uint8_t reserved[3];
} np2audio86_guest_io_trace_t;

typedef struct {
    np2audio86_guest_event_t *events;
    size_t event_capacity;
    size_t event_count;
    np2audio86_guest_data_run_t *data_runs;
    size_t data_run_capacity;
    size_t data_run_count;
    uint8_t *pcm_bytes;
    size_t pcm_capacity;
    size_t pcm_count;
    np2audio86_guest_timer_trace_t *timers;
    size_t timer_capacity;
    size_t timer_count;
    np2audio86_guest_io_trace_t *io;
    size_t io_capacity;
    size_t io_count;
    /* Host-only consumer cursor: accumulated PCM runs may be drained from
     * the attached producer trace without changing their global offsets. */
    size_t pcm_offset_base;
} np2audio86_guest_trace_t;

typedef struct {
    uint64_t frame_timestamp;
    uint64_t guest_cycles;
    uint64_t sequence;
    uint32_t cpu_remainder;
    uint32_t opna_base;
    uint8_t opna_address_low;
    uint8_t opna_address_extended;
    uint8_t opna_data;
    uint8_t opna_extension;
    uint8_t opna_capabilities;
    uint8_t opna_status;
    uint8_t timer_control;
    uint16_t timer_a_value;
    uint8_t timer_b_value;
    uint8_t timer_irq;
    uint8_t pcm_soundflags;
    uint8_t pcm_fifo;
    uint8_t pcm_dactrl;
    uint8_t pcm_volume;
    uint8_t pcm_rate;
    uint16_t pcm_fifo_size;
    uint16_t pcm_fifo_level;
    uint32_t pcm_virtual_buffer;
    uint32_t pcm_read_position;
    uint8_t pcm_irq;
    uint8_t pcm_reqirq;
    uint32_t pcm_rescue;
    uint8_t pcm_irq_line;
    uint8_t pcm_stepbit;
    uint16_t pcm_stepmask;
    uint32_t pcm_rateval;
    uint64_t pcm_stepclock;
    uint64_t pcm_lastclock;
    uint64_t pcm_lastclockforwait;
    uint32_t pcm_real_buffer;
    uint32_t pcm_write_position;
    uint32_t pcm_step_remainder;
    uint8_t soundrom_rejected;
    uint8_t bound;
    uint8_t reserved[2];
} np2audio86_guest_state_snapshot_t;

typedef uint32_t (*np2audio86_guest_cpu_position_fn)(void);
typedef void (*np2audio86_guest_timer_schedule_fn)(uint8_t timer,
                                                    uint64_t clock,
                                                    uint8_t absolute);
typedef void (*np2audio86_guest_timer_cancel_fn)(uint8_t timer);
typedef uint8_t (*np2audio86_guest_timer_iswork_fn)(uint8_t timer);
typedef void (*np2audio86_guest_irq_fn)(uint32_t irq, uint8_t level);

void np2audio86_guest_host_trace_attach(np2audio86_guest_trace_t *trace);
void np2audio86_guest_host_trace_detach(void);
void np2audio86_guest_sink_bind(const np2audio86_guest_sink_t *sink);
void np2audio86_guest_sink_unbind(void);
void np2audio86_guest_host_set_cpu_position_fn(
    np2audio86_guest_cpu_position_fn position);
void np2audio86_guest_host_set_cpu_position(uint32_t position);
#if defined(NP2AUDIO86_GUEST_TEST)
/* Host-only observation seam for transport-blocking proofs.  Production
 * callers neither build nor expose this test instrumentation. */
uint32_t np2audio86_guest_host_current_cpu_position(void);
int np2audio86_guest_host_sink_is_bound(void);
#endif
void np2audio86_guest_host_set_clock(uint32_t baseclock, uint32_t multiple);
void np2audio86_guest_host_set_cpumode(uint32_t cpumode);
void np2audio86_guest_host_set_timer_hooks(
    np2audio86_guest_timer_schedule_fn schedule,
    np2audio86_guest_timer_cancel_fn cancel,
    np2audio86_guest_timer_iswork_fn iswork,
    np2audio86_guest_irq_fn irq);
void np2audio86_guest_host_timer_dispatch(uint8_t timer);
void np2audio86_guest_host_timer_tick(uint8_t timer);
void np2audio86_guest_host_flush_data_run(void);
void np2audio86_guest_host_test_seed(uint64_t frame_timestamp,
                                     uint64_t sequence);
void np2audio86_guest_host_snapshot(
    np2audio86_guest_state_snapshot_t *snapshot);
size_t np2audio86_guest_host_state_size(void);
uint8_t np2audio86_guest_host_failed(void);
const char *np2audio86_guest_host_failure_reason(void);

/* 86R.2 intentionally has no state serialization entry point. */
uint8_t np2audio86_guest_host_save_load_supported(void);

/* Optional host-only hooks used by the prepared board/PCM handlers. */
void np2audio86_guest_host_record_io(uint16_t port, uint8_t direction,
                                     uint8_t value, uint8_t result);

/* OPNA register/address shadow and Domain-A hand-off. */
void np2audio86_guest_opna_write_address_low(uint8_t value);
void np2audio86_guest_opna_write_data_low(uint8_t value);
void np2audio86_guest_opna_write_address_extended(uint8_t value);
void np2audio86_guest_opna_write_data_extended(uint8_t value);
uint8_t np2audio86_guest_opna_read_status(void);
uint8_t np2audio86_guest_opna_read_data(void);
uint8_t np2audio86_guest_opna_read_extended_status(void);
uint8_t np2audio86_guest_opna_read_extended_data(void);
uint8_t np2audio86_guest_opna_read_joy(void);
void np2audio86_guest_opna_set_extension(uint8_t enabled);
void np2audio86_guest_opna_reset(uint8_t capabilities, uint32_t irq,
                                 uint8_t timer_a_event,
                                 uint8_t timer_b_event);
void np2audio86_guest_opna_set_config(uint8_t channels, uint32_t mode);
void np2audio86_guest_opna_set_base(uint16_t base);
uint16_t np2audio86_guest_opna_base(void);
void np2audio86_guest_opna_register_extension(void (*callback)(uint8_t enabled));
void np2audio86_guest_opna_bind(void);
void np2audio86_guest_opna_unbind(void);
void np2audio86_guest_soundrom_load(uint32_t address, const char *name);
void np2audio86_guest_audio_sync(void);

/* Plain PC-9801-86 PCM86 guest/accounting hand-off. */
void np2audio86_guest_pcm86_write(uint8_t register_index, uint8_t value);
void np2audio86_guest_pcm86_write_data(uint8_t value);
void np2audio86_guest_pcm86_set_mixer_volume(uint8_t value);
uint8_t np2audio86_guest_pcm86_read(uint8_t register_index);
void np2audio86_guest_pcm86_set_options(uint8_t dip_switch);
void np2audio86_guest_pcm86_stream_bind(void);
void np2audio86_guest_pcm86_stream_unbind(void);

#if defined(NP2AUDIO86_GUEST_TEST)
/* Test-only state seam.  It is deliberately absent from production builds. */
void np2audio86_guest_test_set_pcm_state(uint32_t virtual_buffer,
                                         uint32_t real_buffer,
                                         uint16_t fifo_size, uint8_t fifo,
                                         uint8_t stepbit, uint8_t reqirq,
                                         uint8_t irqflag,
                                         uint64_t lastclock);
void np2audio86_guest_test_schedule_pcm(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NP2AUDIO86_GUEST_ADAPTER_H */
