#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compiler.h>
#include <cbus/board86.h>
#include <io/iocore.h>
#include <i286c/cpucore.h>
#include <mem/memtram.h>
#include <pccore.h>
#include <np2audio86_guest_adapter.h>
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
RESET_STUB(pic_reset) BIND_STUB(pic_bind)
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

static void put8(size_t *at, uint8_t value)
{
    mem[*at] = value;
    ++*at;
}

static void put16(size_t *at, uint16_t value)
{
    put8(at, (uint8_t)value);
    put8(at, (uint8_t)(value >> 8));
}

static void mov_dx_out(size_t *at, uint16_t port, uint8_t value)
{
    put8(at, 0xbau); put16(at, port);
    put8(at, 0xb0u); put8(at, value);
    put8(at, 0xeeu); /* OUT DX,AL */
}

static void mov_dx_in_store(size_t *at, uint16_t port, uint16_t address)
{
    put8(at, 0xbau); put16(at, port);
    put8(at, 0xecu); /* IN AL,DX */
    put8(at, 0xa2u); put16(at, address);
}

static size_t build_guest_program(void)
{
    size_t at = 0;
    /* OPNA timer A: smallest period, enable and IRQ, then read it back. */
    mov_dx_out(&at, 0x188, 0x24); mov_dx_out(&at, 0x18a, 0xff);
    mov_dx_out(&at, 0x188, 0x0f); mov_dx_out(&at, 0x18a, 0x55);
    mov_dx_out(&at, 0x188, 0x25); mov_dx_out(&at, 0x18a, 0x03);
    mov_dx_out(&at, 0x188, 0x27); mov_dx_out(&at, 0x18a, 0x05);
    mov_dx_out(&at, 0x188, 0x0f); mov_dx_in_store(&at, 0x18a, 0x8000);
    mov_dx_out(&at, 0x188, 0x26); mov_dx_out(&at, 0x18a, 0xff);
    mov_dx_out(&at, 0x188, 0x27); mov_dx_out(&at, 0x18a, 0x0f);
    /* OPNA PSG/FM/rhythm writes are ordered register events. */
    mov_dx_out(&at, 0x188, 0xa0); mov_dx_out(&at, 0x18a, 0x34);
    mov_dx_out(&at, 0x188, 0x28); mov_dx_out(&at, 0x18a, 0xf0);
    mov_dx_out(&at, 0x188, 0x10); mov_dx_out(&at, 0x18a, 0x7f);
    /* Plain PCM86 setup, controls, eight byte OUTs, and every read boundary. */
    mov_dx_out(&at, 0xa460, 0x01);
    mov_dx_out(&at, 0xa46a, 0x00);
    mov_dx_out(&at, 0xa468, 0x11);
    mov_dx_out(&at, 0xa466, 0xa5);
    mov_dx_out(&at, 0xa46c, 0x10); mov_dx_out(&at, 0xa46c, 0x20);
    mov_dx_out(&at, 0xa46c, 0x30); mov_dx_out(&at, 0xa46c, 0x40);
    mov_dx_out(&at, 0xa46c, 0x50); mov_dx_out(&at, 0xa46c, 0x60);
    mov_dx_out(&at, 0xa46c, 0x70); mov_dx_out(&at, 0xa46c, 0x80);
    mov_dx_in_store(&at, 0xa462, 0x8001);
    mov_dx_in_store(&at, 0xa464, 0x8002);
    mov_dx_in_store(&at, 0xa466, 0x8003);
    mov_dx_in_store(&at, 0xa468, 0x8004);
    mov_dx_in_store(&at, 0xa46a, 0x8005);
    mov_dx_in_store(&at, 0xa46c, 0x8006);
    mov_dx_in_store(&at, 0xa46e, 0x8007);
    /* Advance enough real 8086 instructions for Timer A to overflow. */
    for (unsigned i = 0; i < 400; ++i) put8(&at, 0x90u);
    mov_dx_out(&at, 0x188, 0x00); mov_dx_in_store(&at, 0x18a, 0x8008);
    put8(&at, 0xf4u); /* HLT sentinel */
    return at;
}

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

static void le16(uint8_t *out, uint16_t value)
{ out[0] = (uint8_t)value; out[1] = (uint8_t)(value >> 8); }
static void le32(uint8_t *out, uint32_t value)
{ le16(out, (uint16_t)value); le16(out + 2, (uint16_t)(value >> 16)); }
static void le64(uint8_t *out, uint64_t value)
{ le32(out, (uint32_t)value); le32(out + 4, (uint32_t)(value >> 32)); }

static size_t serialize_events(const np2audio86_guest_trace_t *trace,
                               uint8_t *out)
{
    size_t at = 0;
    for (size_t i = 0; i < trace->event_count; ++i) {
        const np2audio86_guest_event_t *e = &trace->events[i];
        le64(out + at, e->frame_timestamp); le64(out + at + 8, e->sequence);
        le32(out + at + 16, e->opcode); le32(out + at + 20, e->payload); at += 24;
    }
    return at;
}

static size_t serialize_runs(const np2audio86_guest_trace_t *trace,
                             uint8_t *out)
{
    size_t at = 0;
    for (size_t i = 0; i < trace->data_run_count; ++i) {
        const np2audio86_guest_data_run_t *r = &trace->data_runs[i];
        le64(out + at, r->frame_timestamp); le64(out + at + 8, r->sequence);
        le64(out + at + 16, r->byte_offset); le32(out + at + 24, r->count);
        le32(out + at + 28, 0); at += 32;
    }
    return at;
}

static size_t serialize_timers(const np2audio86_guest_trace_t *trace,
                               uint8_t *out)
{
    size_t at = 0;
    for (size_t i = 0; i < trace->timer_count; ++i) {
        const np2audio86_guest_timer_trace_t *t = &trace->timers[i];
        le64(out + at, t->frame_timestamp); le64(out + at + 8, t->guest_cycles);
        le32(out + at + 16, t->timer);
        out[at + 20] = t->status; out[at + 21] = t->irq; out[at + 22] = t->level;
        out[at + 23] = 0; at += 24;
    }
    return at;
}

static size_t serialize_io(const np2audio86_guest_trace_t *trace,
                           uint8_t *out)
{
    size_t at = 0;
    for (size_t i = 0; i < trace->io_count; ++i) {
        const np2audio86_guest_io_trace_t *io = &trace->io[i];
        le64(out + at, io->frame_timestamp); le64(out + at + 8, io->sequence);
        le16(out + at + 16, io->port); out[at + 18] = io->direction;
        out[at + 19] = io->value; out[at + 20] = io->result;
        memset(out + at + 21, 0, 3); at += 24;
    }
    return at;
}

static size_t serialize_state(const np2audio86_guest_state_snapshot_t *s,
                              uint8_t *out)
{
    size_t at = 0;
    le64(out + at, s->frame_timestamp); at += 8;
    le64(out + at, s->guest_cycles); at += 8;
    le64(out + at, s->sequence); at += 8;
    le32(out + at, s->cpu_remainder); at += 4;
    le32(out + at, s->opna_base); at += 4;
    out[at++] = s->opna_address_low; out[at++] = s->opna_address_extended;
    out[at++] = s->opna_data; out[at++] = s->opna_extension;
    out[at++] = s->opna_capabilities; out[at++] = s->opna_status;
    out[at++] = s->timer_control;
    le16(out + at, s->timer_a_value); at += 2;
    out[at++] = s->timer_b_value;
    out[at++] = s->timer_irq; out[at++] = s->pcm_soundflags;
    out[at++] = s->pcm_fifo;
    out[at++] = s->pcm_dactrl; out[at++] = s->pcm_volume; out[at++] = s->pcm_rate;
    le16(out + at, s->pcm_fifo_size); at += 2; le16(out + at, s->pcm_fifo_level); at += 2;
    le32(out + at, s->pcm_virtual_buffer); at += 4;
    le32(out + at, s->pcm_read_position); at += 4;
    out[at++] = s->pcm_irq; out[at++] = s->pcm_reqirq;
    le32(out + at, s->pcm_rescue); at += 4;
    out[at++] = s->soundrom_rejected; out[at++] = s->bound;
    out[at++] = 0; out[at++] = 0;
    return at;
}

static uint8_t irq_levels[256];

static void irq_hook(uint32_t irq, uint8_t level)
{
    assert(irq < sizeof(irq_levels));
    irq_levels[irq] = level;
}

static uint8_t run_program(size_t program_size)
{
    uint64_t previous;
    (void)program_size;
    i286c_initialize();
    i286c_reset();
    i286core.s.r.w.cs = 0; i286core.s.cs_base = 0;
    i286core.s.r.w.ds = 0; i286core.s.ds_base = 0;
    i286core.s.r.w.ss = 0; i286core.s.ss_base = 0;
    i286core.s.r.w.ip = 0; i286core.s.r.w.flag = I_FLAG;
    i286core.s.adrsmask = 0xfffffu;
    for (;;) {
        if (mem[i286core.s.r.w.ip] == 0xf4u) return 1;
        previous = host_cycles;
        np2audio86_guest_host_set_cpu_position((uint32_t)host_cycles);
        i286core.s.remainclock = 100000;
        i286core.s.baseclock = 100000;
        i286c_step();
        host_cycles += (uint64_t)(100000 - i286core.s.remainclock);
        if (host_cycles - previous > 1000000u) return 0;
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
        timers, 2, 0, io, 2, 0
    };
    np2audio86_guest_state_snapshot_t state;
    np2audio86_guest_state_snapshot_t baseline;
    uint64_t expected_total;
    uint64_t before_wrap;
    np2audio86_guest_host_trace_detach();
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608 |
                                NP2AUDIO86_OPNA_CAPS_TIMER,
                                3u, 5u, 6u);
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

    /* Counter-only PCM accounting is driven by guest cycles, not by an
     * event consumer.  One 48 kHz sample is consumed after exactly 1024
     * guest cycles at the fixed 49.152 MHz clock. */
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608 |
                                NP2AUDIO86_OPNA_CAPS_TIMER,
                                3u, 5u, 6u);
    np2audio86_guest_pcm86_set_options(0xd1u);
    np2audio86_guest_pcm86_write(0x0au, 0x01u);
    np2audio86_guest_pcm86_write(0x08u, 0x16u);
    np2audio86_guest_host_set_cpu_position(0u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_pcm86_write_data(0x11u);
    np2audio86_guest_pcm86_write_data(0x22u);
    np2audio86_guest_host_set_cpu_position(1024u);
    np2audio86_guest_audio_sync();
    np2audio86_guest_host_snapshot(&state);
    assert(state.pcm_fifo_level == 1u);

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
    printf("AUDIO86_GUEST_BOUNDARY_TESTS=PASS\n");
}

int main(void)
{
    static np2audio86_guest_event_t events[4096];
    static np2audio86_guest_data_run_t runs[4096];
    static uint8_t pcm_bytes[65536];
    static np2audio86_guest_timer_trace_t timers[4096];
    static np2audio86_guest_io_trace_t io[16384];
    static uint8_t serialized[200000];
    np2audio86_guest_trace_t trace = {
        events, 4096, 0, runs, 4096, 0, pcm_bytes, sizeof(pcm_bytes), 0,
        timers, 4096, 0, io, 16384, 0
    };
    np2audio86_guest_state_snapshot_t snapshot;
    size_t program_size;
    size_t length;

    memset(mem, 0, sizeof(mem));
    memset(&np2cfg, 0, sizeof(np2cfg));
    np2cfg.snd86opt = 0xd1;
    pccore.baseclock = 2457600u; pccore.multiple = 20u;
    pccore.realclock = 49152000u; pccore.sound = SOUNDID_PC_9801_86;
    np2audio86_guest_host_set_clock(pccore.baseclock, pccore.multiple);
    np2audio86_guest_host_set_timer_hooks(NULL, NULL, irq_hook);
    np2audio86_guest_host_trace_attach(&trace);
    iocore_create();
    assert(iocore_build() == SUCCESS);
    board86_reset(&np2cfg, FALSE);
    board86_bind();
    program_size = build_guest_program();
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

    length = serialize_io(&trace, serialized);
    print_digest("GUEST_IO", serialized, length);
    length = serialize_events(&trace, serialized);
    print_digest("AUDIO_EVENTS", serialized, length);
    print_digest("PCM86_BYTES", trace.pcm_bytes, trace.pcm_count);
    length = serialize_runs(&trace, serialized);
    print_digest("PCM86_DATA_RUNS", serialized, length);
    length = serialize_timers(&trace, serialized);
    print_digest("TIMER_PIC", serialized, length);
    length = serialize_state(&snapshot, serialized);
    print_digest("FINAL_G_STATE", serialized, length);
    run_boundary_tests();
    printf("NP2AUDIO86_GUEST_STATE_SIZE=%zu\n", np2audio86_guest_host_state_size());
    printf("AUDIO86_GUEST_REAL_IO_PATH=PASS\n");
    printf("AUDIO86_GUEST_TIMER_PIC=PASS\n");
    printf("AUDIO86_GUEST_PCM86_ACCOUNTING=PASS\n");
    printf("AUDIO86_GUEST_TIMESTAMPING=PASS\n");
    printf("AUDIO86_GUEST_EVENT_ORACLE=PASS\n");
    printf("AUDIO86_GUEST_RUNTIME_RESULT=PASS\n");
    return 0;
}
