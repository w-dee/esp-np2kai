#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compiler.h>
#include <cbus/board86.h>
#include <io/iocore.h>
#include <nevent.h>
#include <i286c/cpucore.h>
#include <mem/memtram.h>
#include <pccore.h>
#include <np2audio86_guest_adapter.h>
#include <np2audio86_guest_evidence.h>
#include <np2audio86_guest_program.h>
#include "np2audio86_guest_runtime_capture.h"
#include <np2_crc32.h>
#include <np2_sha256.h>

NP2CFG np2cfg;
PCCORE pccore;
PCSTAT pcstat;
UINT8 soundrenewal;
UINT drawcount;
UINT8 mem[0x200000];
const OEMCHAR np2version[] = "86R.2-host";

static uint64_t host_cycles;

REG8 MEMCALL memp_read8(UINT32 address) { return mem[address & 0x1fffffu]; }
REG16 MEMCALL memp_read16(UINT32 address)
{ return (REG16)(memp_read8(address) | (memp_read8(address + 1) << 8)); }
UINT32 MEMCALL memp_read32(UINT32 address)
{ return (UINT32)memp_read16(address) | ((UINT32)memp_read16(address + 2) << 16); }
void MEMCALL memp_write8(UINT32 address, REG8 value) { mem[address & 0x1fffffu] = value; }
void MEMCALL memp_write16(UINT32 address, REG16 value)
{ memp_write8(address, (REG8)value); memp_write8(address + 1, (REG8)(value >> 8)); }
void MEMCALL memp_write32(UINT32 address, UINT32 value)
{ memp_write16(address, (REG16)value); memp_write16(address + 2, (REG16)(value >> 16)); }
void MEMCALL memp_reads(UINT32 address, void *data, UINT length)
{ for (UINT i = 0; i < length; ++i) ((UINT8 *)data)[i] = memp_read8(address + i); }
void MEMCALL memp_writes(UINT32 address, const void *data, UINT length)
{ for (UINT i = 0; i < length; ++i) memp_write8(address + i, ((const UINT8 *)data)[i]); }
void dmax86(void) {}
UINT MEMCALL biosfunc(UINT32 value) { (void)value; return 0; }
void dipsw_w8(UINT port, REG8 value) { (void)port; (void)value; }
REG8 dipsw_r8(UINT port) { (void)port; return 0xff; }
void egc_w16(UINT port, REG16 value) { (void)port; (void)value; }
REG16 artic_r16(UINT port) { (void)port; return 0xffff; }

#define RESET_STUB(name) void name(const NP2CFG *config) { (void)config; }
#define BIND_STUB(name) void name(void) {}
RESET_STUB(mpu98ii_reset) BIND_STUB(mpu98ii_bind)
RESET_STUB(cgrom_reset) BIND_STUB(cgrom_bind)
 BIND_STUB(cpuio_bind)
RESET_STUB(crtc_reset) BIND_STUB(crtc_bind)
RESET_STUB(dmac_reset) BIND_STUB(dmac_bind)
RESET_STUB(gdc_reset) BIND_STUB(gdc_bind)
RESET_STUB(fdc_reset) BIND_STUB(fdc_bind)
RESET_STUB(keyboard_reset) BIND_STUB(keyboard_bind)
RESET_STUB(nmiio_reset) BIND_STUB(nmiio_bind)
RESET_STUB(printif_reset) BIND_STUB(printif_bind)
RESET_STUB(rs232c_reset) BIND_STUB(rs232c_bind)
RESET_STUB(systemport_reset) BIND_STUB(systemport_bind)
RESET_STUB(uPD4990_reset) BIND_STUB(uPD4990_bind)
RESET_STUB(fdd320_reset) BIND_STUB(fdd320_bind)
RESET_STUB(itimer_reset) BIND_STUB(itimer_bind)
RESET_STUB(mouseif_reset) BIND_STUB(mouseif_bind)
RESET_STUB(artic_reset) BIND_STUB(artic_bind)
RESET_STUB(egc_reset) BIND_STUB(egc_bind)
RESET_STUB(np2sysp_reset) BIND_STUB(np2sysp_bind)
RESET_STUB(necio_reset) BIND_STUB(necio_bind)
RESET_STUB(epsonio_reset) BIND_STUB(epsonio_bind)
RESET_STUB(emsio_reset) BIND_STUB(emsio_bind)
#undef RESET_STUB
#undef BIND_STUB

UINT32 np2_host_gettick(void) { return (UINT32)host_cycles; }

static uint32_t trace_crc(const uint8_t *bytes, size_t length)
{
    return np2_crc32_iso_hdlc(bytes, length);
}

static void print_digest(const char *name, const uint8_t *bytes, size_t length)
{
    uint8_t digest[NP2_SHA256_DIGEST_SIZE];
    np2_sha256_context context;
    printf("%s_COUNT=%zu\n", name, length);
    printf("%s_CRC32=%08" PRIx32 "\n", name, trace_crc(bytes, length));
    np2_sha256_init(&context);
    np2_sha256_update(&context, bytes, length);
    np2_sha256_final(&context, digest);
    printf("%s_SHA256=", name);
    for (size_t i = 0; i < sizeof(digest); ++i) printf("%02x", digest[i]);
    putchar('\n');
}

static uint8_t irq_levels[256];
static unsigned pic_set_transitions;
static unsigned pic_reset_transitions;

static uint8_t actual_pic_level(uint32_t irq)
{
    assert(irq < 16u);
    return (uint8_t)((pic.pi[(irq >> 3) & 1u].irr >> (irq & 7u)) & 1u);
}

static void assert_pic_level(uint32_t irq, uint8_t expected)
{
    assert(irq < sizeof(irq_levels));
    assert(actual_pic_level(irq) == expected);
    assert(irq_levels[irq] == expected);
}

static void irq_hook(uint32_t irq, uint8_t level)
{
    assert(irq < sizeof(irq_levels));
    if (level) {
        ++pic_set_transitions;
        pic_setirq((REG8)irq);
    } else {
        ++pic_reset_transitions;
        pic_resetirq((REG8)irq);
    }
    irq_levels[irq] = level;
    assert(actual_pic_level(irq) == level);
}

static NEVENTID event_id(uint8_t timer)
{
    return timer == NP2AUDIO86_TRACE_TIMER_A ? NEVENT_FMTIMERA :
           timer == NP2AUDIO86_TRACE_TIMER_B ? NEVENT_FMTIMERB : NEVENT_86PCM;
}

static void guest_event_callback(NEVENTITEM item)
{
    np2audio86_guest_host_timer_dispatch((uint8_t)item->userData);
}

static unsigned guest_schedule_count[4];
static uint8_t a46a_invalid_format_pass;
static uint8_t a46a_invalid_nevent_pass;
static uint8_t a46a_invalid_audio_event_pass;
static uint8_t a46a_guest_time_sync_pass;

static void guest_event_schedule(uint8_t timer, uint64_t clock, uint8_t absolute)
{
    assert(clock <= INT32_MAX);
    assert(timer < 4u);
    ++guest_schedule_count[timer];
    nevent_set(event_id(timer), (SINT32)clock, guest_event_callback,
               absolute ? NEVENT_ABSOLUTE : NEVENT_RELATIVE);
    g_nevent.item[event_id(timer)].userData = timer;
}

static void guest_event_cancel(uint8_t timer)
{
    nevent_reset(event_id(timer));
}

static uint8_t guest_event_iswork(uint8_t timer)
{
    return nevent_iswork(event_id(timer)) ? 1u : 0u;
}

static np2audio86_guest_trace_t *drain_queue;
static np2audio86_guest_trace_t *drain_sink;

/* This is an actual consumer: published records are copied to a separately
 * owned sink and then removed from the attached producer trace.  A producer
 * DATA_RUN that is still open is intentionally left open; its later flush
 * preserves canonical run coalescing.  The queue's base preserves global
 * PCM byte offsets across drains. */
static void drain_guest_trace(void)
{
    np2audio86_guest_trace_t *queue = drain_queue;
    np2audio86_guest_trace_t *sink = drain_sink;
    size_t pcm_count;
    if (!queue || !sink) return;
    assert(sink->event_count + queue->event_count <= sink->event_capacity);
    assert(sink->data_run_count + queue->data_run_count <=
           sink->data_run_capacity);
    assert(sink->pcm_count + queue->pcm_count <= sink->pcm_capacity);
    assert(sink->timer_count + queue->timer_count <= sink->timer_capacity);
    assert(sink->io_count + queue->io_count <= sink->io_capacity);
    memcpy(sink->events + sink->event_count, queue->events,
           queue->event_count * sizeof(queue->events[0]));
    sink->event_count += queue->event_count;
    memcpy(sink->data_runs + sink->data_run_count, queue->data_runs,
           queue->data_run_count * sizeof(queue->data_runs[0]));
    sink->data_run_count += queue->data_run_count;
    pcm_count = queue->pcm_count;
    memcpy(sink->pcm_bytes + sink->pcm_count, queue->pcm_bytes, pcm_count);
    sink->pcm_count += pcm_count;
    memcpy(sink->timers + sink->timer_count, queue->timers,
           queue->timer_count * sizeof(queue->timers[0]));
    sink->timer_count += queue->timer_count;
    memcpy(sink->io + sink->io_count, queue->io,
           queue->io_count * sizeof(queue->io[0]));
    sink->io_count += queue->io_count;
    queue->pcm_offset_base += queue->pcm_count;
    queue->event_count = 0;
    queue->data_run_count = 0;
    queue->pcm_count = 0;
    queue->timer_count = 0;
    queue->io_count = 0;
}

static uint64_t arithmetic_schedule_clock[4];
static uint8_t arithmetic_schedule_absolute[4];
static unsigned arithmetic_schedule_count;

static void arithmetic_schedule(uint8_t timer, uint64_t clock,
                                uint8_t absolute)
{
    assert(timer < 4u);
    arithmetic_schedule_clock[timer] = clock;
    arithmetic_schedule_absolute[timer] = absolute;
    ++arithmetic_schedule_count;
}

static void arithmetic_cancel(uint8_t timer) { (void)timer; }
static uint8_t arithmetic_iswork(uint8_t timer) { (void)timer; return 0; }

static uint64_t expected_timer_period(uint8_t timer, uint8_t reg24,
                                      uint8_t reg25, uint8_t reg26,
                                      uint32_t cpumode, uint32_t multiple)
{
    uint32_t l = timer == NP2AUDIO86_TRACE_TIMER_A
        ? 18u * (1024u - (((uint32_t)reg24 << 2) | (reg25 & 3u)))
        : 288u * (256u - reg26);
    l = (cpumode & CPUMODE_8MHZ) ? (l * 1248u) / 625u
                                 : (l * 1536u) / 625u;
    return (uint64_t)l * multiple;
}

static uint64_t expected_pcm_schedule(uint8_t fifo, uint32_t virtual_buffer,
                                      uint16_t fifo_size, uint8_t stepbit,
                                      uint16_t stepmask, uint32_t cpumode,
                                      uint32_t multiple)
{
    static const uint32_t clk25[8] = {
        0x1bdeu, 0x2527u, 0x37bbu, 0x4a4eu,
        0x6f75u, 0x949cu, 0xdf5fu, 0x12938u
    };
    static const uint32_t clk20[8] = {
        0x16a4u, 0x1e30u, 0x2d48u, 0x3c60u,
        0x5a8fu, 0x78bfu, 0xb57du, 0xf17du
    };
    int64_t count = (int64_t)virtual_buffer - fifo_size;
    uint32_t base;
    assert(count > 0);
    count = (count + stepmask) >> stepbit;
    base = (cpumode & CPUMODE_8MHZ) ? clk20[fifo & 7u] : clk25[fifo & 7u];
    return ((uint64_t)base * (uint64_t)count >> 7) * multiple;
}

/* Independent zero-history transcription of pcm86_setnextintr(). */
static uint64_t expected_pcm_next(uint8_t fifo, uint32_t virtual_buffer,
                                  uint16_t fifo_size, uint8_t stepbit,
                                  uint16_t stepmask, uint32_t cpumode,
                                  uint8_t reqirq)
{
    int64_t count = (int64_t)virtual_buffer - fifo_size;
    if (count <= 0) return reqirq ? 1u : 2000u;
    return expected_pcm_schedule(fifo, virtual_buffer, fifo_size, stepbit,
                                 stepmask, cpumode, 20u);
}

static void assert_pcm_next(uint32_t virtual_buffer, uint32_t real_buffer,
                            uint16_t fifo_size, uint8_t fifo,
                            uint8_t stepbit, uint8_t reqirq)
{
    uint16_t stepmask = (uint16_t)((1u << stepbit) - 1u);
    nevent_allreset();
    np2audio86_guest_test_set_pcm_state(virtual_buffer, real_buffer,
                                        fifo_size, fifo, stepbit, reqirq,
                                        0u, 0u);
    np2audio86_guest_test_schedule_pcm();
    assert(nevent_iswork(NEVENT_86PCM));
    assert((uint64_t)g_nevent.item[NEVENT_86PCM].clock ==
           expected_pcm_next(fifo, virtual_buffer, fifo_size, stepbit,
                             stepmask, pccore.cpumode, reqirq));
    np2audio86_guest_host_snapshot(&(np2audio86_guest_state_snapshot_t){0});
}

static uint32_t production_cpu_position(void)
{
    /* Production-equivalent position: CPU_CLOCK + CPU_BASECLOCK - CPU_REMCLOCK. */
    return (uint32_t)(CPU_CLOCK + CPU_BASECLOCK - CPU_REMCLOCK);
}

static uint8_t run_program(size_t program_size)
{
    (void)program_size;
    i286c_initialize();
    i286c_reset();
    i286core.s.r.w.cs = 0; i286core.s.cs_base = 0;
    i286core.s.r.w.ds = 0; i286core.s.ds_base = 0;
    i286core.s.r.w.ss = 0; i286core.s.ss_base = 0;
    i286core.s.r.w.ip = 0; i286core.s.r.w.flag = I_FLAG;
    i286core.s.adrsmask = 0xfffffu;
    nevent_get1stevent();
    for (;;) {
        if (mem[i286core.s.r.w.ip] == 0xf4u) return 1;
        if (i286core.s.remainclock <= 0) {
            nevent_progress();
            host_cycles = CPU_CLOCK;
            drain_guest_trace();
            continue;
        }
        i286c_step();
        if (CPU_CLOCK > 100000000u) return 0;
        drain_guest_trace();
    }
}

static void run_boundary_tests(void)
{
    static np2audio86_guest_event_t events[8];
    static np2audio86_guest_data_run_t runs[2];
    static uint8_t pcm_bytes[32769];
    static np2audio86_guest_io_trace_t io[2];
    static np2audio86_guest_timer_trace_t timers[2];
    np2audio86_guest_trace_t trace = {
        events, 2, 0, runs, 2, 0, pcm_bytes, sizeof(pcm_bytes), 0,
        timers, 2, 0, io, 2, 0, 0, 0, {0}
    };
    np2audio86_guest_state_snapshot_t state;
    np2audio86_guest_state_snapshot_t baseline;
    uint64_t expected_total;
    uint64_t before_wrap;
    np2audio86_guest_host_trace_detach();
    np2audio86_guest_host_set_cpu_position_fn(NULL);
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608 |
                                NP2AUDIO86_OPNA_CAPS_TIMER,
                                3u, 5u, 6u);

    /* Exhaustive representative Timer-A/B arithmetic over both pinned CPU
     * scale paths.  The fake callback records requests only; it is not a
     * scheduler and is used solely for this isolated arithmetic proof. */
    np2audio86_guest_host_set_timer_hooks(arithmetic_schedule,
                                          arithmetic_cancel,
                                          arithmetic_iswork, irq_hook);
    {
        static const uint32_t modes[] = {CPUMODE_8MHZ, 0u};
        static const uint8_t highs[] = {0u, 1u, 0x80u, 0xffu};
        static const uint8_t lows[] = {0u, 1u, 3u};
        static const uint8_t timer_b_values[] = {0u, 1u, 0x80u, 0xffu};
        for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); ++m) {
            np2audio86_guest_host_set_cpumode(modes[m]);
            for (size_t h = 0; h < sizeof(highs) / sizeof(highs[0]); ++h) {
                for (size_t l = 0; l < sizeof(lows) / sizeof(lows[0]); ++l) {
                    arithmetic_schedule_count = 0;
                    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_TIMER,
                                                0u, 1u, 2u);
                    np2audio86_guest_opna_write_address_low(0x24u);
                    np2audio86_guest_opna_write_data_low(highs[h]);
                    np2audio86_guest_opna_write_address_low(0x25u);
                    np2audio86_guest_opna_write_data_low(lows[l]);
                    np2audio86_guest_opna_write_address_low(0x27u);
                    np2audio86_guest_opna_write_data_low(0x05u);
                    assert(arithmetic_schedule_count == 1u);
                    assert(arithmetic_schedule_clock[1] ==
                           expected_timer_period(1u, highs[h], lows[l], 0u,
                                                 modes[m], 20u));
                    assert(arithmetic_schedule_absolute[1] == 1u);
                }
            }
            for (size_t b = 0; b < sizeof(timer_b_values) / sizeof(timer_b_values[0]); ++b) {
                arithmetic_schedule_count = 0;
                np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_TIMER,
                                            0u, 1u, 2u);
                np2audio86_guest_opna_write_address_low(0x26u);
                np2audio86_guest_opna_write_data_low(timer_b_values[b]);
                np2audio86_guest_opna_write_address_low(0x27u);
                np2audio86_guest_opna_write_data_low(0x0au);
                assert(arithmetic_schedule_count == 1u);
                assert(arithmetic_schedule_clock[2] ==
                       expected_timer_period(2u, 0u, 0u,
                                             timer_b_values[b], modes[m], 20u));
                assert(arithmetic_schedule_absolute[2] == 1u);
            }
        }
    }

    /* All eight authoritative PCM rates and the format/step mapping are
     * checked through the counter-only snapshot, with no sample storage. */
    np2audio86_guest_host_set_clock(2457600u, 20u);
    np2audio86_guest_host_set_cpumode(CPUMODE_8MHZ);
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608 |
                                NP2AUDIO86_OPNA_CAPS_TIMER,
                                0u, 1u, 2u);
    {
        static const uint32_t rates[] = {352800u, 264600u, 176400u, 132300u,
                                         88200u, 66150u, 44010u, 33075u};
        for (uint8_t index = 0; index < 8u; ++index) {
            np2audio86_guest_pcm86_write(0x08u, (uint8_t)(0x80u | index));
            np2audio86_guest_host_snapshot(&state);
            assert(state.pcm_rateval == rates[index]);
            assert(state.pcm_stepclock ==
                   (((uint64_t)2457600u << 6) / rates[index]) * (20u << 3));
        }
    }
    np2audio86_guest_pcm86_write(0x08u, 0x80u);
    {
        static const uint8_t formats[] = {0x10u, 0x20u, 0x30u, 0x50u,
                                          0x60u, 0x70u};
        static const uint8_t steps[] = {1u, 1u, 2u, 0u, 0u, 1u};
        for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
            np2audio86_guest_pcm86_write(0x0au, formats[i]);
            np2audio86_guest_host_snapshot(&state);
            assert(state.pcm_stepbit == steps[i]);
            assert(state.pcm_stepmask == ((1u << steps[i]) - 1u));
        }
    }
    np2audio86_guest_host_snapshot(&baseline);
    np2audio86_guest_pcm86_write(0x0au, 0x1fu);
    np2audio86_guest_host_snapshot(&state);
    assert(state.pcm_dactrl == baseline.pcm_dactrl);
    np2audio86_guest_pcm86_write(0x08u, 0xa0u);
    np2audio86_guest_pcm86_write(0x0au, 2u);
    np2audio86_guest_host_snapshot(&state);
    assert(state.pcm_fifo_size == 384u);

    /* A460 changes only soundflags bit 0 and invokes the extension chain. */
    np2audio86_guest_pcm86_set_options(0xd1u);
    np2audio86_guest_host_snapshot(&baseline);
    np2audio86_guest_pcm86_write(0x00u, 1u);
    np2audio86_guest_host_snapshot(&state);
    assert((state.pcm_soundflags & 0xfeu) == (baseline.pcm_soundflags & 0xfeu));
    assert((state.pcm_soundflags & 1u) == 1u && state.opna_extension == 1u);
    np2audio86_guest_pcm86_write(0x00u, 0u);
    np2audio86_guest_host_snapshot(&state);
    assert((state.pcm_soundflags & 0xfeu) == (baseline.pcm_soundflags & 0xfeu));
    assert((state.pcm_soundflags & 1u) == 0u && state.opna_extension == 0u);

    /* A product-preserving clock tuple is accepted before bind, while a
     * bound device rejects changing 2.4576 MHz x20 to 1.2288 MHz x40. */
    np2audio86_guest_opna_unbind();
    np2audio86_guest_host_set_clock(1228800u, 40u);
    assert(!np2audio86_guest_host_failed());
    np2audio86_guest_host_set_clock(2457600u, 20u);
    assert(!np2audio86_guest_host_failed());
    np2audio86_guest_opna_bind();
    np2audio86_guest_host_set_clock(1228800u, 40u);
    assert(np2audio86_guest_host_failed());
    assert(strstr(np2audio86_guest_host_failure_reason(), "dynamic clock") != NULL);
    np2audio86_guest_host_test_seed(0u, 0u);

    /* Restore the production bridges for the remaining focused semantic
     * checks. */
    np2audio86_guest_host_set_timer_hooks(guest_event_schedule,
                                          guest_event_cancel,
                                          guest_event_iswork, irq_hook);
    np2audio86_guest_host_set_cpumode(CPUMODE_8MHZ);

    /* Shared OPNA/PCM86 IRQ ownership: clearing one cause must retain the
     * actual PIC level until the final cause is gone, in either order. */
    pic_reset(&np2cfg);
    memset(irq_levels, 0, sizeof(irq_levels));
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608 |
                                NP2AUDIO86_OPNA_CAPS_TIMER,
                                0x10u, 1u, 2u);
    np2audio86_guest_pcm86_set_options(0xd1u);
    np2audio86_guest_host_set_cpu_position(0u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_pcm86_write(0x08u, 0xb0u);
    np2audio86_guest_pcm86_write(0x0au, 0u);
    for (unsigned i = 0; i < 256u; ++i) np2audio86_guest_pcm86_write_data(0x55u);
    np2audio86_guest_opna_write_address_low(0x24u);
    np2audio86_guest_opna_write_data_low(0xffu);
    np2audio86_guest_opna_write_address_low(0x25u);
    np2audio86_guest_opna_write_data_low(3u);
    np2audio86_guest_opna_write_address_low(0x27u);
    np2audio86_guest_opna_write_data_low(0x05u);
    np2audio86_guest_host_timer_tick(NP2AUDIO86_TRACE_TIMER_A);
    assert_pic_level(3u, 1u);
    {
        unsigned set_count = pic_set_transitions;
        np2audio86_guest_host_timer_tick(NP2AUDIO86_TRACE_TIMER_A);
        assert(pic_set_transitions == set_count);
    }
    /* Timer status clears, but PCM86 remains pending on the shared line. */
    np2audio86_guest_opna_write_address_low(0x27u);
    np2audio86_guest_opna_write_data_low(0x10u);
    assert_pic_level(3u, 1u);
    /* Once PCM is cleared too, the actual PIC bridge observes RESET. */
    np2audio86_guest_pcm86_write(0x08u, 0x80u);
    assert_pic_level(3u, 0u);
    assert(pic_reset_transitions > 0u);

    /* Inverse ordering: PCM clears first while the timer latch remains. */
    pic_reset(&np2cfg);
    memset(irq_levels, 0, sizeof(irq_levels));
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608 |
                                NP2AUDIO86_OPNA_CAPS_TIMER,
                                0x10u, 1u, 2u);
    np2audio86_guest_pcm86_set_options(0xd1u);
    np2audio86_guest_host_set_cpu_position(0u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_opna_write_address_low(0x24u);
    np2audio86_guest_opna_write_data_low(0xffu);
    np2audio86_guest_opna_write_address_low(0x25u);
    np2audio86_guest_opna_write_data_low(3u);
    np2audio86_guest_opna_write_address_low(0x27u);
    np2audio86_guest_opna_write_data_low(0x05u);
    np2audio86_guest_host_timer_tick(NP2AUDIO86_TRACE_TIMER_A);
    assert_pic_level(3u, 1u);
    np2audio86_guest_pcm86_write(0x08u, 0xb0u);
    np2audio86_guest_pcm86_write(0x0au, 0u);
    for (unsigned i = 0; i < 256u; ++i) np2audio86_guest_pcm86_write_data(0x66u);
    np2audio86_guest_host_timer_tick(NP2AUDIO86_TRACE_TIMER_A);
    np2audio86_guest_pcm86_write(0x08u, 0x80u);
    assert_pic_level(3u, 1u);
    np2audio86_guest_opna_write_address_low(0x27u);
    np2audio86_guest_opna_write_data_low(0x10u);
    assert_pic_level(3u, 0u);

    /* PCM86-only IRQ ownership uses its configured line when it differs from
     * OPNA; clearing the PCM cause must reset that line, not OPNA's line. */
    pic_reset(&np2cfg);
    memset(irq_levels, 0, sizeof(irq_levels));
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608 |
                                NP2AUDIO86_OPNA_CAPS_TIMER,
                                0x10u, 1u, 2u);
    np2audio86_guest_pcm86_set_options(0xd5u); /* PCM86 IRQ10, OPNA IRQ3. */
    np2audio86_guest_host_set_cpu_position(0u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_pcm86_write(0x08u, 0xb0u);
    np2audio86_guest_pcm86_write(0x0au, 1u); /* fifo threshold = 256. */
    for (unsigned i = 0; i < 256u; ++i) np2audio86_guest_pcm86_write_data(0x77u);
    np2audio86_guest_host_timer_dispatch(NP2AUDIO86_TRACE_PCM);
    assert_pic_level(10u, 1u);
    assert_pic_level(3u, 0u);
    /* Move above the threshold before clearing; the pinned A468 forced-
     * interrupt rule otherwise immediately re-latches an empty FIFO. */
    np2audio86_guest_pcm86_write_data(0x88u);
    np2audio86_guest_pcm86_write(0x08u, 0x80u);
    assert_pic_level(10u, 0u);
    assert_pic_level(3u, 0u);

    /* A468/A46A/A46C boundary matrix.  This exercises the reset edge,
     * threshold-vs-DAC interpretation, invalid DAC values, the exact
     * pinned non-underflow NEVENT count selection, both CPU clock tables,
     * and the A46C proportional/full wait override. */
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608 |
                                NP2AUDIO86_OPNA_CAPS_TIMER,
                                0x10u, 1u, 2u);
    np2audio86_guest_pcm86_set_options(0xd1u);
    np2audio86_guest_host_set_cpu_position(1000u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_pcm86_write(0x08u, 0x20u); /* FIFO-threshold mode. */
    np2audio86_guest_pcm86_write(0x0au, 0xffu);
    np2audio86_guest_host_snapshot(&state);
    assert(state.pcm_fifo_size == 0x7ffcu);
    np2audio86_guest_pcm86_write(0x0au, 0x02u);
    np2audio86_guest_host_snapshot(&state);
    assert(state.pcm_fifo_size == 384u);
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608 |
                                NP2AUDIO86_OPNA_CAPS_TIMER,
                                0x10u, 1u, 2u);
    np2audio86_guest_pcm86_set_options(0xd1u);
    np2audio86_guest_host_set_cpu_position(1000u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_pcm86_write(0x08u, 0x80u); /* DAC/playback mode. */
    np2audio86_guest_pcm86_write(0x0au, 0x33u);
    np2audio86_guest_host_snapshot(&state);
    assert(state.pcm_dactrl == 0x33u && state.pcm_stepbit == 2u &&
           state.pcm_stepmask == 3u && state.pcm_rescue != 0u);
    np2audio86_guest_pcm86_write(0x0au, 0x3fu); /* low nibble 0xf rejected. */
    np2audio86_guest_host_snapshot(&baseline);
    assert(baseline.pcm_dactrl == 0x33u && baseline.pcm_stepbit == 2u &&
           baseline.pcm_stepmask == 3u);
    np2audio86_guest_pcm86_write_data(0x5au);
    np2audio86_guest_host_snapshot(&baseline);
    assert(baseline.pcm_lastclockforwait == 1000u + 400000u - 6250u);
    for (unsigned i = 1; i <= 256u; ++i) np2audio86_guest_pcm86_write_data(0x5au);
    np2audio86_guest_host_snapshot(&state);
    assert(state.pcm_virtual_buffer == 257u);
    assert(state.pcm_lastclockforwait == 1000u + 400000u);
    np2audio86_guest_pcm86_write(0x08u, 0x88u); /* rising reset bit. */
    np2audio86_guest_host_snapshot(&state);
    assert(state.pcm_write_position == 0u && state.pcm_read_position == 0u &&
           state.pcm_real_buffer == 0u && state.pcm_virtual_buffer == 0u &&
           state.pcm_lastclock == ((uint64_t)1000u << 6) &&
           state.pcm_lastclockforwait == 1000u);
    assert(state.pcm_irq != 0u);
    np2audio86_guest_pcm86_write(0x08u, 0x80u); /* clear irq + reset bit. */
    np2audio86_guest_host_snapshot(&state);
    assert(state.pcm_irq != 0u); /* empty FIFO is pinned forced-IRQ state. */
    np2audio86_guest_pcm86_write(0x08u, 0x01u); /* stop before restart edge */
    np2audio86_guest_host_set_cpu_position(1234u);
    np2audio86_guest_pcm86_write(0x08u, 0x81u); /* rate change while playing. */
    np2audio86_guest_host_snapshot(&state);
    assert(state.pcm_rate == 1u && state.pcm_stepbit == 2u &&
           state.pcm_lastclock == ((uint64_t)1234u << 6));

    for (unsigned mode = 0; mode < 2u; ++mode) {
        static const uint8_t rates[] = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u};
        np2audio86_guest_host_set_cpumode(mode ? 0u : CPUMODE_8MHZ);
        for (size_t i = 0; i < sizeof(rates); ++i) {
            np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608 |
                                        NP2AUDIO86_OPNA_CAPS_TIMER,
                                        0x10u, 1u, 2u);
            np2audio86_guest_pcm86_set_options(0xd1u);
            nevent_allreset();
            nevent_get1stevent();
            np2audio86_guest_pcm86_write(0x08u, (uint8_t)(0x80u | rates[i]));
            for (unsigned n = 0; n < 256u; ++n)
                np2audio86_guest_pcm86_write_data((uint8_t)n);
            np2audio86_guest_pcm86_write(0x08u, (uint8_t)(0x80u | rates[i]));
            assert(nevent_iswork(NEVENT_86PCM));
            assert((uint64_t)g_nevent.item[NEVENT_86PCM].clock ==
                   expected_pcm_schedule((uint8_t)(0x80u | rates[i]), 256u,
                                         128u, 2u, 3u,
                                         mode ? 0u : CPUMODE_8MHZ, 20u));
        }
    }
    np2audio86_guest_host_set_cpumode(CPUMODE_8MHZ);
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608 |
                                NP2AUDIO86_OPNA_CAPS_TIMER,
                                0x10u, 1u, 2u);
    np2audio86_guest_pcm86_set_options(0xd1u);
    nevent_allreset();
    np2audio86_guest_pcm86_write(0x08u, 0x80u);
    np2audio86_guest_pcm86_write_data(0x44u);
    np2audio86_guest_pcm86_write(0x08u, 0x80u);
    assert(nevent_iswork(NEVENT_86PCM));
    np2audio86_guest_host_timer_dispatch(NP2AUDIO86_TRACE_PCM);
    np2audio86_guest_host_snapshot(&state);
    assert(state.pcm_irq != 0u || nevent_iswork(NEVENT_86PCM));

    np2audio86_guest_host_set_cpu_position(0u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_host_snapshot(&baseline);
    np2audio86_guest_host_set_cpu_position(1023u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_host_snapshot(&state);
    expected_total = baseline.cpu_remainder + 1023u;
    assert(state.frame_timestamp ==
           baseline.frame_timestamp + expected_total / 1024u);
    assert(state.cpu_remainder == expected_total % 1024u);
    np2audio86_guest_host_set_cpu_position(1024u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_host_snapshot(&state);
    expected_total = baseline.cpu_remainder + 1024u;
    assert(state.frame_timestamp ==
           baseline.frame_timestamp + expected_total / 1024u);
    assert(state.cpu_remainder == expected_total % 1024u);

    /* Counter-only PCM accounting is driven by the exact stepclock and the
     * playback bit (fifo&0x80), not by an event consumer. */
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608 |
                                NP2AUDIO86_OPNA_CAPS_TIMER,
                                3u, 5u, 6u);
    np2audio86_guest_pcm86_set_options(0xd1u);
    np2audio86_guest_pcm86_write(0x0au, 0x01u);
    np2audio86_guest_pcm86_write(0x08u, 0x96u);
    np2audio86_guest_host_set_cpu_position(0u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_pcm86_write_data(0x11u);
    np2audio86_guest_pcm86_write_data(0x22u);
    np2audio86_guest_host_set_cpu_position(1024u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_host_snapshot(&state);
    assert(state.pcm_fifo_level == 2u);

    before_wrap = state.guest_cycles;
    np2audio86_guest_host_set_cpu_position(0xffffff00u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_host_snapshot(&baseline);
    assert(baseline.guest_cycles == before_wrap +
           (uint32_t)(0xffffff00u - 1024u));
    np2audio86_guest_host_set_cpu_position(0x00000100u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_host_snapshot(&state);
    assert(state.guest_cycles == baseline.guest_cycles +
           (uint32_t)(0x00000100u - 0xffffff00u));

    /* Sequence and recorder limits fail closed instead of wrapping. */
    np2audio86_guest_host_trace_attach(&trace);
    np2audio86_guest_host_test_seed(0u, UINT64_MAX);
    np2audio86_guest_opna_write_address_low(0u);
    np2audio86_guest_opna_write_data_low(0x12u);
    assert(np2audio86_guest_host_failed());
    assert(strstr(np2audio86_guest_host_failure_reason(), "sequence") != NULL);
    np2audio86_guest_host_trace_detach();

    np2audio86_guest_host_trace_attach(&trace);
    np2audio86_guest_host_test_seed(UINT64_MAX, 0u);
    np2audio86_guest_host_set_cpu_position(0u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_host_set_cpu_position(1024u);
    np2audio86_guest_audio_sync();
    assert(np2audio86_guest_host_failed());
    assert(strstr(np2audio86_guest_host_failure_reason(), "timestamp") != NULL);
    np2audio86_guest_host_trace_detach();

    trace.event_capacity = 0;
    np2audio86_guest_host_trace_attach(&trace);
    np2audio86_guest_host_test_seed(0u, 0u);
    np2audio86_guest_opna_write_address_low(0u);
    np2audio86_guest_opna_write_data_low(0x12u);
    assert(np2audio86_guest_host_failed());
    assert(strstr(np2audio86_guest_host_failure_reason(), "event trace") != NULL);
    np2audio86_guest_host_trace_detach();

    trace.event_capacity = 2;
    trace.pcm_capacity = 0;
    np2audio86_guest_host_trace_attach(&trace);
    np2audio86_guest_host_test_seed(0u, 0u);
    np2audio86_guest_pcm86_write_data(0x34u);
    assert(np2audio86_guest_host_failed());
    assert(strstr(np2audio86_guest_host_failure_reason(), "PCM byte") != NULL);
    np2audio86_guest_host_trace_detach();

    trace.pcm_capacity = sizeof(pcm_bytes);
    np2audio86_guest_host_trace_attach(&trace);
    np2audio86_guest_host_test_seed(0u, 0u);
    for (size_t i = 0; i < sizeof(pcm_bytes); ++i)
        np2audio86_guest_pcm86_write_data((uint8_t)i);
    np2audio86_guest_host_flush_data_run();
    assert(!np2audio86_guest_host_failed());
    assert(trace.data_run_count == 2u);
    assert(trace.data_runs[0].count == 32768u);
    assert(trace.data_runs[1].count == 1u);
    np2audio86_guest_host_trace_detach();

    /* Timestamp changes and semantic boundaries flush DATA_RUNs. */
    trace.event_capacity = 2;
    trace.pcm_capacity = sizeof(pcm_bytes);
    np2audio86_guest_host_trace_attach(&trace);
    np2audio86_guest_host_test_seed(0u, 0u);
    np2audio86_guest_host_set_cpu_position(0u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_pcm86_write_data(1u);
    np2audio86_guest_host_set_cpu_position(1024u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_pcm86_write_data(2u);
    assert(trace.data_run_count == 1u);
    (void)np2audio86_guest_pcm86_read(0x06);
    assert(trace.data_run_count == 2u);
    assert(trace.data_runs[0].count == 1u && trace.data_runs[1].count == 1u);
    np2audio86_guest_host_trace_detach();

    /* Reset with an unpublished DATA_RUN publishes the run before the reset
     * barrier, preserves the time/sequence epoch, and cancels all NEVENTs. */
    trace.event_capacity = 8;
    trace.pcm_capacity = sizeof(pcm_bytes);
    np2audio86_guest_host_trace_attach(&trace);
    np2audio86_guest_host_test_seed(7u, 20u);
    np2audio86_guest_host_set_cpu_position(0u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_pcm86_write_data(0xc3u);
    np2audio86_guest_host_snapshot(&baseline);
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608 |
                                NP2AUDIO86_OPNA_CAPS_TIMER,
                                0x10u, 1u, 2u);
    np2audio86_guest_host_snapshot(&state);
    assert(trace.data_run_count == 1u && trace.event_count == 1u);
    assert(trace.events[0].opcode == NP2AUDIO86_TRACE_RESET_BARRIER);
    assert(trace.data_runs[0].sequence < trace.events[0].sequence);
    assert(state.frame_timestamp >= baseline.frame_timestamp);
    assert(state.sequence > baseline.sequence);
    assert(!nevent_iswork(NEVENT_FMTIMERA));
    assert(!nevent_iswork(NEVENT_FMTIMERB));
    assert(!nevent_iswork(NEVENT_86PCM));
    np2audio86_guest_host_trace_detach();

    /* A control mutation is an ordering boundary even without a timestamp
     * change: DATA_RUN A, control event, then DATA_RUN B. */
    trace.event_capacity = 2;
    trace.pcm_capacity = sizeof(pcm_bytes);
    np2audio86_guest_host_trace_attach(&trace);
    np2audio86_guest_host_test_seed(0u, 0u);
    np2audio86_guest_host_set_cpu_position(0u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_pcm86_write_data(0xa1u);
    np2audio86_guest_pcm86_write(0x08u, 0x80u);
    np2audio86_guest_pcm86_write_data(0xb2u);
    np2audio86_guest_host_flush_data_run();
    assert(trace.data_run_count == 2u && trace.event_count == 1u);
    assert(trace.data_runs[0].sequence < trace.events[0].sequence);
    assert(trace.events[0].sequence < trace.data_runs[1].sequence);
    assert(trace.data_runs[0].byte_offset == 0u &&
           trace.data_runs[1].byte_offset == 1u);
    assert(trace.data_runs[0].count == 1u && trace.data_runs[1].count == 1u);
    np2audio86_guest_host_trace_detach();

    /* CSM is a guest-timer boundary only: record the future trigger without
     * mutating a waveform generator. */
    trace.event_capacity = 8;
    trace.pcm_capacity = sizeof(pcm_bytes);
    np2audio86_guest_host_trace_attach(&trace);
    np2audio86_guest_host_test_seed(0u, 0u);
    np2audio86_guest_host_set_cpu_position(0u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_opna_write_address_low(0x24u);
    np2audio86_guest_opna_write_data_low(0xffu);
    np2audio86_guest_opna_write_address_low(0x25u);
    np2audio86_guest_opna_write_data_low(3u);
    np2audio86_guest_opna_write_address_low(0x27u);
    np2audio86_guest_opna_write_data_low(0x85u);
    np2audio86_guest_host_timer_tick(NP2AUDIO86_TRACE_TIMER_A);
    assert(trace.event_count >= 4u);
    assert(trace.events[trace.event_count - 1u].opcode ==
           NP2AUDIO86_TRACE_OPNA_CSM);
    {
        size_t before = trace.event_count;
        np2audio86_guest_opna_write_address_low(0x27u);
        np2audio86_guest_opna_write_data_low(0xc5u);
        np2audio86_guest_host_timer_tick(NP2AUDIO86_TRACE_TIMER_A);
        for (size_t i = before; i < trace.event_count; ++i)
            assert(trace.events[i].opcode != NP2AUDIO86_TRACE_OPNA_CSM);
    }
    np2audio86_guest_host_trace_detach();

    /* Plain-board boundaries: no ADPCM, neutral joystick, ROM rejection,
     * and explicit dynamic-clock rejection. */
    np2audio86_guest_host_snapshot(&state);
    assert(!(state.opna_capabilities & NP2AUDIO86_OPNA_CAPS_ADPCM));
    assert(np2audio86_guest_opna_read_joy() == 0u);
    np2audio86_guest_soundrom_load(0xcc000u, "unsupported-test");
    np2audio86_guest_host_snapshot(&state);
    assert(state.soundrom_rejected);
    assert(np2audio86_guest_host_save_load_supported() == 0u);
    np2audio86_guest_host_set_clock(1u, 1u);
    assert(np2audio86_guest_host_failed());
    np2audio86_guest_host_set_clock(2457600u, 20u);

    /* The board's secondary 12-bit base is a focused supplemental I/O test,
     * while the primary oracle above remains the real guest IN/OUT path. */
    board86_unbind();
    np2cfg.snd86opt = 0xd0u;
    board86_reset(&np2cfg, FALSE);
    board86_bind();
    iocore_out8(0x288u, 0x0fu);
    iocore_out8(0x28au, 0x66u);
    assert(iocore_inp8(0x28au) == 0x66u);
    assert(iocore_inp8(0x18au) != 0x66u);

    /* The plain-board dip-switch table is authoritative for the shared IRQ. */
    {
        static const struct { uint8_t dip; uint8_t irq; } options[] = {
            {0xd1u, 0x03u}, {0xd5u, 0x0au}, {0xd9u, 0x0du}, {0xddu, 0x0cu}
        };
        for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
            np2cfg.snd86opt = options[i].dip;
            board86_reset(&np2cfg, FALSE);
            np2audio86_guest_host_snapshot(&state);
            assert(state.timer_irq == options[i].irq);
        }
    }
    np2cfg.snd86opt = 0xd1u;
    board86_reset(&np2cfg, FALSE);
    board86_bind();
    assert(np2audio86_guest_opna_base() == 0u);
}

static void run_86r2c_evidence_tests(void)
{
    np2audio86_guest_state_snapshot_t before, after;

    /* These cases use explicitly supplied host-clock positions. */
    np2audio86_guest_host_set_cpu_position_fn(NULL);
    np2audio86_guest_host_set_timer_hooks(guest_event_schedule,
                                          guest_event_cancel,
                                          guest_event_iswork, irq_hook);

    /* A46C: the logical-full override wins over the proportional wait. */
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608, 0u, 1u, 2u);
    np2audio86_guest_host_set_cpu_position(700u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_test_set_pcm_state(0x7fffu, 0x7fffu, 128u, 0x80u,
                                        2u, 0u, 0u, 700u << 6);
    np2audio86_guest_pcm86_write_data(0x11u);
    np2audio86_guest_host_snapshot(&after);
    assert(after.pcm_virtual_buffer == 0x8000u);
    assert(after.pcm_lastclockforwait == 700u + 400000u);

    /* Independent zero-history selection matrix, including the old mismatch. */
    pccore.cpumode = CPUMODE_8MHZ;
    assert_pcm_next(256u, 200u, 128u, 0x80u, 0u, 1u);
    assert_pcm_next(256u, 256u, 128u, 0x81u, 2u, 1u);
    assert_pcm_next(256u, 300u, 128u, 0x82u, 1u, 1u);
    assert_pcm_next(129u, 1u, 128u, 0x83u, 2u, 1u);
    assert_pcm_next(128u, 64u, 128u, 0x80u, 2u, 1u);
    assert_pcm_next(100u, 64u, 128u, 0x80u, 2u, 0u);
    np2audio86_guest_test_set_pcm_state(100u, 64u, 128u, 0x80u,
                                        2u, 0u, 0u, 0u);
    nevent_allreset(); np2audio86_guest_test_schedule_pcm();
    np2audio86_guest_host_snapshot(&after);
    assert(after.pcm_reqirq == 1u);

    /* Actual Timer-B NEVENT callback: set, retained set, clear, and stop. */
    pic_reset(&np2cfg); memset(irq_levels, 0, sizeof(irq_levels));
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_TIMER, 0x10u, 1u, 2u);
    np2audio86_guest_opna_write_address_low(0x26u);
    np2audio86_guest_opna_write_data_low(0xffu);
    np2audio86_guest_opna_write_address_low(0x27u);
    np2audio86_guest_opna_write_data_low(0x0au);
    assert(nevent_iswork(NEVENT_FMTIMERB));
    guest_event_callback(&g_nevent.item[NEVENT_FMTIMERB]);
    np2audio86_guest_host_snapshot(&after);
    assert((after.opna_status & 2u) != 0u); assert_pic_level(3u, 1u);
    { unsigned sets = pic_set_transitions;
      guest_event_callback(&g_nevent.item[NEVENT_FMTIMERB]);
      assert(pic_set_transitions == sets); }
    np2audio86_guest_opna_write_address_low(0x27u);
    np2audio86_guest_opna_write_data_low(0x20u);
    assert_pic_level(3u, 0u);
    np2audio86_guest_opna_write_address_low(0x27u);
    np2audio86_guest_opna_write_data_low(0u);
    assert(!nevent_iswork(NEVENT_FMTIMERB));

    /* A466: playback phase, full/empty flags, multiple format classes, wrap. */
    np2audio86_guest_host_set_cpu_position(0u);
    np2audio86_guest_test_set_pcm_state(0u, 0u, 128u, 0x00u, 2u, 0u, 0u, 0u);
    assert((np2audio86_guest_pcm86_read(0x06u) & 0x40u) != 0u);
    np2audio86_guest_test_set_pcm_state(0x8000u, 0x8000u, 128u, 0x80u, 0u, 0u, 0u, 0u);
    assert((np2audio86_guest_pcm86_read(0x06u) & 0x80u) != 0u);
    np2audio86_guest_test_set_pcm_state(256u, 256u, 128u, 0x80u, 2u, 0u, 0u, 0u);
    np2audio86_guest_host_set_cpu_position(100u);
    assert((np2audio86_guest_pcm86_read(0x06u) & 1u) == 0u);
    np2audio86_guest_host_set_cpu_position(1000u);
    assert((np2audio86_guest_pcm86_read(0x06u) & 1u) != 0u);
    np2audio86_guest_host_snapshot(&before);
    np2audio86_guest_host_set_cpu_position(4000u);
    (void)np2audio86_guest_pcm86_read(0x06u);
    np2audio86_guest_host_snapshot(&after);
    assert(after.pcm_virtual_buffer < before.pcm_virtual_buffer);
    np2audio86_guest_test_set_pcm_state(256u, 256u, 128u, 0x86u, 1u, 0u, 0u,
                                        ((uint64_t)UINT_MAX - 8u) << 6);
    np2audio86_guest_host_set_cpu_position(100000u);
    (void)np2audio86_guest_pcm86_read(0x06u);
    np2audio86_guest_host_snapshot(&after);
    assert(after.pcm_lastclock != (((uint64_t)UINT_MAX - 8u) << 6));

    /* A468 clear and forced-reassert are distinguishable input states. */
    np2audio86_guest_host_set_cpu_position(0u);
    np2audio86_guest_test_set_pcm_state(256u, 256u, 128u, 0x90u, 2u, 0u, 1u, 0u);
    np2audio86_guest_pcm86_write(0x08u, 0x80u);
    np2audio86_guest_host_snapshot(&after); assert(after.pcm_irq == 0u);
    np2audio86_guest_test_set_pcm_state(0u, 0u, 128u, 0x90u, 2u, 0u, 1u, 0u);
    np2audio86_guest_pcm86_write(0x08u, 0x80u);
    np2audio86_guest_host_snapshot(&after); assert(after.pcm_irq == 1u);
    np2audio86_guest_pcm86_write(0x0au, 0x33u);
    np2audio86_guest_pcm86_write(0x08u, 0x81u);
    np2audio86_guest_host_snapshot(&after); assert(after.pcm_rescue == 1920u);
    np2audio86_guest_pcm86_write(0x0au, 0x50u);
    np2audio86_guest_pcm86_write(0x08u, 0x86u);
    np2audio86_guest_host_snapshot(&after); assert(after.pcm_rescue == 80u);
    np2audio86_guest_test_set_pcm_state(256u, 256u, 128u, 0x80u, 2u, 1u, 0u, 0u);
    nevent_allreset();
    np2audio86_guest_pcm86_write(0x08u, 0x80u);
    assert(nevent_iswork(NEVENT_86PCM));
    assert((uint64_t)g_nevent.item[NEVENT_86PCM].clock ==
           expected_pcm_next(0x80u, 256u, 128u, 2u, 3u, pccore.cpumode, 1u));
    {
        uint64_t nevent_now = (uint64_t)(CPU_BASECLOCK - CPU_REMCLOCK);
        np2audio86_guest_pcm86_write(0x08u, 0x88u);
        np2audio86_guest_host_snapshot(&after);
        assert(after.pcm_virtual_buffer == 0u && after.pcm_real_buffer == 0u);
        assert(nevent_iswork(NEVENT_86PCM));
        assert((uint64_t)g_nevent.item[NEVENT_86PCM].clock == nevent_now + 1u);
    }

    /* A46A exact rescue, complete invalid preservation, and reqirq schedule. */
    np2audio86_guest_test_set_pcm_state(256u, 256u, 128u, 0x81u, 2u, 1u, 0u, 0u);
    nevent_allreset(); np2audio86_guest_pcm86_write(0x0au, 0x33u);
    np2audio86_guest_host_snapshot(&before); assert(before.pcm_rescue == 1920u);
    assert(nevent_iswork(NEVENT_86PCM));
    assert((uint64_t)g_nevent.item[NEVENT_86PCM].clock ==
           expected_pcm_next(0x81u, 256u, 128u, 2u, 3u, pccore.cpumode, 1u));
    np2audio86_guest_pcm86_write(0x0au, 0x50u);
    np2audio86_guest_host_snapshot(&after);
    assert(after.pcm_rescue == 480u && after.pcm_stepbit == 0u &&
           after.pcm_stepmask == 0u);
    np2audio86_guest_pcm86_write(0x0au, 0x3fu);
    {
        np2audio86_guest_state_snapshot_t invalid_before = after;
        np2audio86_guest_host_snapshot(&after);
        assert(after.pcm_dactrl == invalid_before.pcm_dactrl &&
               after.pcm_stepbit == invalid_before.pcm_stepbit &&
               after.pcm_stepmask == invalid_before.pcm_stepmask &&
               after.pcm_rescue == invalid_before.pcm_rescue &&
               after.pcm_reqirq == invalid_before.pcm_reqirq &&
               nevent_iswork(NEVENT_86PCM));
    }

    /* Focused pre-fix reproduction: an invalid DAC/format write must retain
     * the active NEVENT deadline, avoid a second schedule operation, avoid a
     * Domain-T control event, and leave an open DATA_RUN unpublished. */
    {
        static np2audio86_guest_event_t invalid_events[8];
        static np2audio86_guest_data_run_t invalid_runs[8];
        static uint8_t invalid_pcm[8];
        np2audio86_guest_trace_t invalid_trace = {
            invalid_events, 8, 0, invalid_runs, 8, 0,
            invalid_pcm, sizeof(invalid_pcm), 0, NULL, 0, 0, NULL, 0, 0, 0,
            0, {0}
        };
        uint64_t before_clock;
        unsigned before_schedules;
        size_t before_events, before_runs, before_pcm;

        memset(invalid_events, 0, sizeof(invalid_events));
        memset(invalid_runs, 0, sizeof(invalid_runs));
        memset(invalid_pcm, 0, sizeof(invalid_pcm));
        np2audio86_guest_host_trace_detach();
        np2audio86_guest_host_test_seed(0u, 0u);
        np2audio86_guest_host_set_cpu_position(0u);
        np2audio86_guest_test_set_pcm_state(4u, 7u, 128u, 0x81u,
                                            2u, 1u, 0u, 0u);
        np2audio86_guest_audio_sync();
        np2audio86_guest_host_trace_attach(&invalid_trace);
        np2audio86_guest_pcm86_write_data(0x5au);
        np2audio86_guest_test_set_pcm_state(4u, 7u, 128u, 0x81u,
                                            2u, 1u, 0u, 0u);
        np2audio86_guest_host_set_cpu_position(0u);
        np2audio86_guest_audio_sync();
        nevent_allreset();
        guest_schedule_count[NP2AUDIO86_TRACE_PCM] = 0u;
        np2audio86_guest_test_schedule_pcm();
        np2audio86_guest_host_snapshot(&before);
        before_clock = (uint64_t)g_nevent.item[NEVENT_86PCM].clock;
        before_schedules = guest_schedule_count[NP2AUDIO86_TRACE_PCM];
        before_events = invalid_trace.event_count;
        before_runs = invalid_trace.data_run_count;
        before_pcm = invalid_trace.pcm_count;
        np2audio86_guest_host_set_cpu_position(1024u);
        np2audio86_guest_pcm86_write(0x0au, 0x3fu);
        np2audio86_guest_host_snapshot(&after);
        assert(after.guest_cycles == before.guest_cycles + 1024u);
        assert(after.frame_timestamp == before.frame_timestamp + 1u);
        assert(after.cpu_remainder == before.cpu_remainder);
        a46a_guest_time_sync_pass = 1u;
        assert(after.pcm_dactrl == before.pcm_dactrl &&
               after.pcm_soundflags == before.pcm_soundflags &&
               after.pcm_volume == before.pcm_volume &&
               after.pcm_rate == before.pcm_rate &&
               after.pcm_stepbit == before.pcm_stepbit &&
               after.pcm_stepmask == before.pcm_stepmask &&
               after.pcm_fifo_level == before.pcm_fifo_level &&
               after.pcm_rescue == before.pcm_rescue &&
               after.pcm_reqirq == before.pcm_reqirq &&
               after.pcm_irq == before.pcm_irq &&
               after.pcm_irq_line == before.pcm_irq_line &&
               after.pcm_fifo == before.pcm_fifo &&
               after.pcm_fifo_size == before.pcm_fifo_size &&
               after.pcm_virtual_buffer == before.pcm_virtual_buffer &&
               after.pcm_real_buffer == before.pcm_real_buffer &&
               after.pcm_write_position == before.pcm_write_position &&
               after.pcm_read_position == before.pcm_read_position &&
               after.pcm_rateval == before.pcm_rateval &&
               after.pcm_stepclock == before.pcm_stepclock &&
               after.pcm_lastclock == before.pcm_lastclock &&
               after.pcm_lastclockforwait == before.pcm_lastclockforwait);
        assert(after.pcm_step_remainder ==
               (uint32_t)(((uint64_t)1024u << 6) % before.pcm_stepclock));
        assert(after.sequence == before.sequence);
        assert(invalid_trace.event_count == before_events);
        assert(invalid_trace.data_run_count == before_runs);
        assert(invalid_trace.pcm_count == before_pcm);
        assert(nevent_iswork(NEVENT_86PCM));
        assert((uint64_t)g_nevent.item[NEVENT_86PCM].clock == before_clock);
        assert(guest_schedule_count[NP2AUDIO86_TRACE_PCM] == before_schedules);
        a46a_invalid_format_pass = 1u;
        a46a_invalid_nevent_pass = 1u;
        a46a_invalid_audio_event_pass = 1u;
        np2audio86_guest_host_trace_detach();
    }

    printf("AUDIO86_GUEST_A46C_LOGICAL_FULL_WAIT=PASS\n");
    printf("AUDIO86_GUEST_PCM86_SETNEXTINTR_SELECTION=PASS\n");
    printf("AUDIO86_GUEST_PCM86_NEVENT_IMMEDIATE=PASS\n");
    printf("AUDIO86_GUEST_PCM86_NEVENT_FALLBACK=PASS\n");
    printf("AUDIO86_GUEST_PCM86_UNDERFLOW_HISTORY_BOUNDARY=ZERO_HISTORY_MVP\n");
    printf("AUDIO86_GUEST_TIMER_B_ACTUAL_PIC=PASS\n");
    printf("A466_END_TO_END=PASS\nA468_IRQ_CLEAR_BRANCH=PASS\nA468_FORCED_IRQ_BRANCH=PASS\n");
    printf("A468_RESCUE=PASS\nA468_NEVENT_CONTROL=PASS\nA46A_RESCUE=PASS\n");
    printf("A46A_INVALID_FORMAT_PRESERVATION=%s\n",
           a46a_invalid_format_pass ? "PASS" : "FAIL");
    printf("A46A_INVALID_NEVENT_PRESERVATION=%s\n",
           a46a_invalid_nevent_pass ? "PASS" : "FAIL");
    printf("INVALID_A46A_AUDIO_EVENT_EMITTED=%s\n",
           a46a_invalid_audio_event_pass ? "NO" : "YES");
    printf("A46A_GUEST_TIME_SYNC=%s\n",
           a46a_guest_time_sync_pass ? "PASS" : "FAIL");
    printf("A46A_VALID_REQIRQ_RESCHEDULE=PASS\n");
    printf("A46A_REQIRQ_RESCHEDULE=PASS\n");
}

static int np2audio86_guest_runtime_run(
    np2audio86_guest_trace_t *trace, np2audio86_guest_state_snapshot_t *state,
    const np2audio86_guest_sink_t *sink,
    size_t (*program_builder)(uint8_t *, size_t),
    np2audio86_guest_execution_evidence_t *evidence)
{
    size_t program_size;

    if (trace == NULL || state == NULL || program_builder == NULL) {
        return -1;
    }
    if (evidence != NULL) memset(evidence, 0, sizeof(*evidence));
    trace->event_count = 0;
    trace->data_run_count = 0;
    trace->pcm_count = 0;
    trace->timer_count = 0;
    trace->io_count = 0;
    trace->pcm_offset_base = 0;
    memset(mem, 0, sizeof(mem));
    memset(irq_levels, 0, sizeof(irq_levels));
    pic_set_transitions = 0;
    pic_reset_transitions = 0;
    memset(&np2cfg, 0, sizeof(np2cfg));
    np2cfg.snd86opt = 0xd1;
    pccore.baseclock = 2457600u;
    pccore.multiple = 20u;
    pccore.realclock = 49152000u;
    pccore.sound = SOUNDID_PC_9801_86;
    pccore.cpumode = CPUMODE_8MHZ;
    np2audio86_guest_host_set_clock(pccore.baseclock, pccore.multiple);
    np2audio86_guest_host_set_cpumode(pccore.cpumode);
    np2audio86_guest_host_set_cpu_position_fn(production_cpu_position);
    np2audio86_guest_host_set_timer_hooks(guest_event_schedule,
                                          guest_event_cancel,
                                          guest_event_iswork, irq_hook);
    np2audio86_guest_host_trace_attach(trace);
    np2audio86_guest_sink_bind(sink);
    iocore_create();
    if (iocore_build() != SUCCESS) {
        np2audio86_guest_sink_unbind();
        np2audio86_guest_host_trace_detach();
        return -1;
    }
    nevent_allreset();
    pic_reset(&np2cfg);
    board86_reset(&np2cfg, FALSE);
    board86_bind();
    program_size = program_builder(mem, sizeof(mem));
    np2audio86_guest_test_reset_io_cycle_observation();
    if (program_size >= 0x90000u || !run_program(program_size)) {
        board86_unbind();
        np2audio86_guest_sink_unbind();
        np2audio86_guest_host_trace_detach();
        return -1;
    }
    board86_reset(&np2cfg, FALSE);
    np2audio86_guest_audio_sync();
    np2audio86_guest_host_flush_data_run();
    np2audio86_guest_host_snapshot(state);
    if (evidence != NULL) {
        if (i286core.s.r.w.ip != program_size - 1U ||
            mem[i286core.s.r.w.ip] != UINT8_C(0xf4)) {
            board86_unbind();
            np2audio86_guest_sink_unbind();
            np2audio86_guest_host_trace_detach();
            return -1;
        }
        evidence->program_bytes = program_size;
        evidence->io_observation_count =
            np2audio86_guest_test_io_cycle_observation_count();
        evidence->first_io_guest_cycle =
            np2audio86_guest_test_first_io_guest_cycle();
        evidence->last_io_guest_cycle =
            np2audio86_guest_test_last_io_guest_cycle();
        evidence->termination_ip = i286core.s.r.w.ip;
        evidence->terminated_at_hlt = 1U;
    }
    board86_unbind();
    np2audio86_guest_sink_unbind();
    np2audio86_guest_host_trace_detach();
    return np2audio86_guest_host_failed() ? -1 : 0;
}

int np2audio86_guest_runtime_capture(np2audio86_guest_trace_t *trace,
                                     np2audio86_guest_state_snapshot_t *state)
{
    return np2audio86_guest_runtime_run(trace, state, NULL,
                                        np2audio86_guest_program_build, NULL);
}

int np2audio86_guest_runtime_capture_sustained_2s(
    np2audio86_guest_trace_t *trace, np2audio86_guest_state_snapshot_t *state,
    np2audio86_guest_execution_evidence_t *evidence)
{
    if (evidence == NULL) return -1;
    return np2audio86_guest_runtime_run(
        trace, state, NULL, np2audio86_guest_program_build_sustained_2s,
        evidence);
}

int np2audio86_guest_runtime_live(
    np2audio86_guest_trace_t *trace, np2audio86_guest_state_snapshot_t *state,
    const np2audio86_guest_sink_t *sink)
{
    if (sink == NULL) {
        return -1;
    }
    return np2audio86_guest_runtime_run(trace, state, sink,
                                        np2audio86_guest_program_build, NULL);
}

#ifndef NP2AUDIO86_GUEST_RUNTIME_NO_MAIN
int main(void)
{
    static np2audio86_guest_event_t events[4096];
    static np2audio86_guest_data_run_t runs[4096];
    static uint8_t pcm_bytes[65536];
    static np2audio86_guest_timer_trace_t timers[4096];
    static np2audio86_guest_io_trace_t io[16384];
    static np2audio86_guest_event_t events2[4096];
    static np2audio86_guest_data_run_t runs2[4096];
    static uint8_t pcm_bytes2[65536];
    static np2audio86_guest_timer_trace_t timers2[4096];
    static np2audio86_guest_io_trace_t io2[16384];
    static np2audio86_guest_event_t events3[4096];
    static np2audio86_guest_data_run_t runs3[4096];
    static uint8_t pcm_bytes3[65536];
    static np2audio86_guest_timer_trace_t timers3[4096];
    static np2audio86_guest_io_trace_t io3[16384];
    static np2audio86_guest_event_t events3_sink[4096];
    static np2audio86_guest_data_run_t runs3_sink[4096];
    static uint8_t pcm_bytes3_sink[65536];
    static np2audio86_guest_timer_trace_t timers3_sink[4096];
    static np2audio86_guest_io_trace_t io3_sink[16384];
    static uint8_t serialized[200000];
    np2audio86_guest_trace_t trace = {
        events, 4096, 0, runs, 4096, 0, pcm_bytes, sizeof(pcm_bytes), 0,
        timers, 4096, 0, io, 16384, 0, 0, 0, {0}
    };
    np2audio86_guest_state_snapshot_t snapshot;
    np2audio86_guest_state_snapshot_t snapshot2;
    np2audio86_guest_state_snapshot_t snapshot3;
    np2audio86_guest_trace_t trace2;
    np2audio86_guest_trace_t trace3;
    np2audio86_guest_trace_t trace3_sink;
    size_t program_size;
    size_t length;

    memset(mem, 0, sizeof(mem));
    memset(irq_levels, 0, sizeof(irq_levels));
    pic_set_transitions = 0;
    pic_reset_transitions = 0;
    memset(&np2cfg, 0, sizeof(np2cfg));
    np2cfg.snd86opt = 0xd1;
    pccore.baseclock = 2457600u; pccore.multiple = 20u;
    pccore.realclock = 49152000u; pccore.sound = SOUNDID_PC_9801_86;
    pccore.cpumode = CPUMODE_8MHZ;
    np2audio86_guest_host_set_clock(pccore.baseclock, pccore.multiple);
    np2audio86_guest_host_set_cpumode(pccore.cpumode);
    np2audio86_guest_host_set_cpu_position_fn(production_cpu_position);
    np2audio86_guest_host_set_timer_hooks(guest_event_schedule,
                                          guest_event_cancel,
                                          guest_event_iswork, irq_hook);
    np2audio86_guest_host_trace_attach(&trace);
    iocore_create();
    assert(iocore_build() == SUCCESS);
    nevent_allreset();
    pic_reset(&np2cfg);
    board86_reset(&np2cfg, FALSE);
    board86_bind();
    program_size = np2audio86_guest_program_build(mem, sizeof(mem));
    assert(program_size < 0x90000u);
    assert(run_program(program_size));
    /* The reset is deliberately after guest execution: it proves the
     * RESET_BARRIER ordering without erasing the principal trace. */
    board86_reset(&np2cfg, FALSE);
    np2audio86_guest_audio_sync();
    np2audio86_guest_host_flush_data_run();
    np2audio86_guest_host_snapshot(&snapshot);
    assert(mem[0x8000] == 0x55u);
    assert(mem[0x8001] == 0 && mem[0x8002] == 0);
    assert(mem[0x8009] == 0xffu && mem[0x800a] == 0xffu);
    assert(mem[0x800b] == 0x5au && mem[0x800c] == 0xffu &&
           mem[0x800d] == 0xffu);
    assert((mem[0x800e] & 0x03u) != 0u);
    assert(mem[0x800f] == 0u && mem[0x8010] == 0u);
    assert(mem[0x8006] == 0 && mem[0x8007] == 0);
    assert(trace.pcm_count == 8u);
    assert(trace.data_run_count >= 1u);
    assert(trace.event_count >= 7u);
    assert(trace.io_count >= 30u);
    {
        size_t offset = 0;
        for (size_t i = 0; i < trace.data_run_count; ++i) {
            assert(trace.data_runs[i].count > 0u);
            assert(trace.data_runs[i].count <= 32768u);
            assert(trace.data_runs[i].byte_offset == offset);
            offset += trace.data_runs[i].count;
        }
        assert(offset == trace.pcm_count);
    }
    for (size_t i = 1; i < trace.event_count; ++i) {
        assert(trace.events[i - 1].sequence < trace.events[i].sequence);
        assert(trace.events[i - 1].frame_timestamp <=
               trace.events[i].frame_timestamp);
    }
    assert(snapshot.frame_timestamp > 0u);
    assert(np2audio86_guest_host_state_size() < 2048u);
    assert(!np2audio86_guest_host_failed());

    /* Consumer-independence baseline: retain one complete accumulated trace
     * for comparison with the independently drained producer run below. */
    np2audio86_guest_host_trace_detach();
    board86_unbind();
    np2audio86_guest_host_test_seed(0u, 0u);
    memset(mem, 0, sizeof(mem));
    memset(irq_levels, 0, sizeof(irq_levels));
    nevent_allreset();
    pic_reset(&np2cfg);
    board86_reset(&np2cfg, FALSE);
    board86_bind();
    trace2 = (np2audio86_guest_trace_t){
        events2, 4096, 0, runs2, 4096, 0, pcm_bytes2, sizeof(pcm_bytes2), 0,
        timers2, 4096, 0, io2, 16384, 0, 0
    };
    np2audio86_guest_host_trace_attach(&trace2);
    assert(run_program(np2audio86_guest_program_build(mem, sizeof(mem))));
    board86_reset(&np2cfg, FALSE);
    np2audio86_guest_audio_sync();
    np2audio86_guest_host_flush_data_run();
    np2audio86_guest_host_snapshot(&snapshot2);
    assert(trace.io_count == trace2.io_count &&
           trace.event_count == trace2.event_count &&
           trace.pcm_count == trace2.pcm_count &&
           trace.data_run_count == trace2.data_run_count &&
           trace.timer_count == trace2.timer_count);
    assert(memcmp(trace.io, trace2.io,
                  trace.io_count * sizeof(trace.io[0])) == 0);
    assert(memcmp(trace.events, trace2.events,
                  trace.event_count * sizeof(trace.events[0])) == 0);
    assert(memcmp(trace.pcm_bytes, trace2.pcm_bytes, trace.pcm_count) == 0);
    assert(memcmp(trace.data_runs, trace2.data_runs,
                  trace.data_run_count * sizeof(trace.data_runs[0])) == 0);
    assert(memcmp(trace.timers, trace2.timers,
                  trace.timer_count * sizeof(trace.timers[0])) == 0);
    assert(memcmp(&snapshot, &snapshot2, sizeof(snapshot)) == 0);
    np2audio86_guest_host_trace_detach();
    printf("AUDIO86_GUEST_CONSUMER_INDEPENDENCE=PASS\n");

    /* Real producer/consumer run: drain_guest_trace() removes records from
     * the attached producer after each CPU/event step and appends them to a
     * separately owned sink.  The sink must compare byte-for-byte with the
     * accumulated baseline, including global PCM DATA_RUN offsets. */
    board86_unbind();
    np2audio86_guest_host_test_seed(0u, 0u);
    memset(mem, 0, sizeof(mem));
    memset(irq_levels, 0, sizeof(irq_levels));
    nevent_allreset();
    pic_reset(&np2cfg);
    board86_reset(&np2cfg, FALSE);
    board86_bind();
    trace3 = (np2audio86_guest_trace_t){
        events3, 4096, 0, runs3, 4096, 0, pcm_bytes3, sizeof(pcm_bytes3), 0,
        timers3, 4096, 0, io3, 16384, 0, 0
    };
    trace3_sink = (np2audio86_guest_trace_t){
        events3_sink, 4096, 0, runs3_sink, 4096, 0,
        pcm_bytes3_sink, sizeof(pcm_bytes3_sink), 0,
        timers3_sink, 4096, 0, io3_sink, 16384, 0, 0
    };
    np2audio86_guest_host_trace_attach(&trace3);
    drain_queue = &trace3;
    drain_sink = &trace3_sink;
    assert(run_program(np2audio86_guest_program_build(mem, sizeof(mem))));
    board86_reset(&np2cfg, FALSE);
    np2audio86_guest_audio_sync();
    np2audio86_guest_host_flush_data_run();
    drain_guest_trace();
    np2audio86_guest_host_snapshot(&snapshot3);
    drain_queue = NULL;
    drain_sink = NULL;
    assert(trace3.event_count == 0u && trace3.data_run_count == 0u &&
           trace3.pcm_count == 0u && trace3.timer_count == 0u &&
           trace3.io_count == 0u);
    assert(trace3_sink.io_count == trace2.io_count &&
           trace3_sink.event_count == trace2.event_count &&
           trace3_sink.pcm_count == trace2.pcm_count &&
           trace3_sink.data_run_count == trace2.data_run_count &&
           trace3_sink.timer_count == trace2.timer_count);
    assert(memcmp(trace3_sink.io, trace2.io,
                  trace2.io_count * sizeof(trace2.io[0])) == 0);
    assert(memcmp(trace3_sink.events, trace2.events,
                  trace2.event_count * sizeof(trace2.events[0])) == 0);
    assert(memcmp(trace3_sink.pcm_bytes, trace2.pcm_bytes,
                  trace2.pcm_count) == 0);
    assert(memcmp(trace3_sink.data_runs, trace2.data_runs,
                  trace2.data_run_count * sizeof(trace2.data_runs[0])) == 0);
    assert(memcmp(trace3_sink.timers, trace2.timers,
                  trace2.timer_count * sizeof(trace2.timers[0])) == 0);
    assert(memcmp(&snapshot2, &snapshot3, sizeof(snapshot2)) == 0);
    np2audio86_guest_host_trace_detach();
    printf("AUDIO86_GUEST_REAL_CONSUMER_DRAIN=PASS\n");

    length = np2audio86_guest_evidence_serialize_io(&trace, serialized);
    print_digest("GUEST_IO", serialized, length);
    printf("GUEST_IO_SEMANTIC_COUNT=%zu\nGUEST_IO_SERIALIZED_BYTES=%zu\n", trace.io_count, length);
    length = np2audio86_guest_evidence_serialize_events(&trace, serialized);
    print_digest("AUDIO_EVENTS", serialized, length);
    printf("AUDIO_EVENTS_SEMANTIC_COUNT=%zu\nAUDIO_EVENTS_SERIALIZED_BYTES=%zu\n", trace.event_count, length);
    print_digest("PCM86_BYTES", trace.pcm_bytes, trace.pcm_count);
    printf("PCM86_BYTES_PAYLOAD_BYTES=%zu\nPCM86_BYTES_SERIALIZED_BYTES=%zu\n", trace.pcm_count, trace.pcm_count);
    length = np2audio86_guest_evidence_serialize_runs(&trace, serialized);
    print_digest("PCM86_DATA_RUNS", serialized, length);
    printf("PCM86_DATA_RUNS_SEMANTIC_COUNT=%zu\nPCM86_DATA_RUNS_PAYLOAD_BYTES=%zu\nPCM86_DATA_RUNS_SERIALIZED_BYTES=%zu\n", trace.data_run_count, trace.pcm_count, length);
    length = np2audio86_guest_evidence_serialize_timers(&trace, serialized);
    print_digest("TIMER_PIC", serialized, length);
    printf("TIMER_PIC_SEMANTIC_COUNT=%zu\nTIMER_PIC_SERIALIZED_BYTES=%zu\n", trace.timer_count, length);
    length = np2audio86_guest_evidence_serialize_state(&snapshot, serialized);
    print_digest("FINAL_G_STATE", serialized, length);
    printf("FINAL_G_STATE_SEMANTIC_COUNT=1\nFINAL_G_STATE_SERIALIZED_BYTES=%zu\n", length);
    run_boundary_tests();
    run_86r2c_evidence_tests();
    if (!a46a_invalid_format_pass || !a46a_invalid_nevent_pass ||
        !a46a_invalid_audio_event_pass || !a46a_guest_time_sync_pass) {
        printf("AUDIO86_GUEST_PCM86_ACCOUNTING=FAIL\n");
        printf("AUDIO86_GUEST_BOUNDARY_TESTS=FAIL\n");
        printf("AUDIO86_GUEST_RUNTIME_RESULT=FAIL\n");
        return 1;
    }
    printf("ACTUAL_PIC_AUTHORITY=PASS\n");
    printf("SHARED_IRQ_SEMANTICS=PASS\n");
    printf("A46C_GUEST_COUNTER_SEMANTICS=PASS\n");
    printf("PCM86_NEVENT_SEMANTICS=PASS\n");
    printf("A466_SEMANTICS=PASS\n");
    printf("A468_SEMANTICS=PASS\n");
    printf("A46A_SEMANTICS=PASS\n");
    printf("DOMAIN_G_CONSUMER_INDEPENDENCE=PASS\n");
    printf("NP2AUDIO86_GUEST_STATE_SIZE=%zu\n", np2audio86_guest_host_state_size());
    printf("AUDIO86_GUEST_REAL_IO_PATH=PASS\n");
    printf("AUDIO86_GUEST_TIMER_PIC=PASS\n");
    printf("AUDIO86_GUEST_PCM86_ACCOUNTING=PASS\n");
    printf("AUDIO86_GUEST_TIMESTAMPING=PASS\n");
    printf("AUDIO86_GUEST_EVENT_ORACLE=PASS\n");
    printf("AUDIO86_GUEST_ACTUAL_PIC=PASS\n");
    printf("AUDIO86_GUEST_PCM86_NEVENT=PASS\n");
    printf("AUDIO86_GUEST_BOUNDARY_TESTS=PASS\n");
    printf("AUDIO86_GUEST_RUNTIME_RESULT=PASS\n");
    return 0;
}
#endif
