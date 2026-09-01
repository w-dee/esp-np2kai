#include "np2audio86_guest_evidence.h"

#include <string.h>

static void le16(uint8_t *out, uint16_t value)
{ out[0] = (uint8_t)value; out[1] = (uint8_t)(value >> 8); }
static void le32(uint8_t *out, uint32_t value)
{ le16(out, (uint16_t)value); le16(out + 2, (uint16_t)(value >> 16)); }
static void le64(uint8_t *out, uint64_t value)
{ le32(out, (uint32_t)value); le32(out + 4, (uint32_t)(value >> 32)); }

size_t np2audio86_guest_evidence_serialize_events(
    const np2audio86_guest_trace_t *trace, uint8_t *out)
{
    size_t at = 0U, i;
    for (i = 0U; i < trace->event_count; ++i) {
        const np2audio86_guest_event_t *e = &trace->events[i];
        le64(out + at, e->frame_timestamp); le64(out + at + 8U, e->sequence);
        le32(out + at + 16U, e->opcode); le32(out + at + 20U, e->payload); at += 24U;
    }
    return at;
}

size_t np2audio86_guest_evidence_serialize_runs(
    const np2audio86_guest_trace_t *trace, uint8_t *out)
{
    size_t at = 0U, i;
    for (i = 0U; i < trace->data_run_count; ++i) {
        const np2audio86_guest_data_run_t *r = &trace->data_runs[i];
        le64(out + at, r->frame_timestamp); le64(out + at + 8U, r->sequence);
        le64(out + at + 16U, r->byte_offset); le32(out + at + 24U, r->count);
        le32(out + at + 28U, 0U); at += 32U;
    }
    return at;
}

size_t np2audio86_guest_evidence_serialize_timers(
    const np2audio86_guest_trace_t *trace, uint8_t *out)
{
    size_t at = 0U, i;
    for (i = 0U; i < trace->timer_count; ++i) {
        const np2audio86_guest_timer_trace_t *t = &trace->timers[i];
        le64(out + at, t->frame_timestamp); le64(out + at + 8U, t->guest_cycles);
        le32(out + at + 16U, t->timer);
        out[at + 20U] = t->status; out[at + 21U] = t->irq; out[at + 22U] = t->level;
        out[at + 23U] = t->cause; out[at + 24U] = t->pic_transition;
        out[at + 25U] = t->pcm_irqflag; out[at + 26U] = t->pcm_reqirq;
        out[at + 27U] = 0U; at += 28U;
    }
    return at;
}

size_t np2audio86_guest_evidence_serialize_io(
    const np2audio86_guest_trace_t *trace, uint8_t *out)
{
    size_t at = 0U, i;
    for (i = 0U; i < trace->io_count; ++i) {
        const np2audio86_guest_io_trace_t *io = &trace->io[i];
        le64(out + at, io->frame_timestamp); le64(out + at + 8U, io->sequence);
        le16(out + at + 16U, io->port); out[at + 18U] = io->direction;
        out[at + 19U] = io->value; out[at + 20U] = io->result;
        memset(out + at + 21U, 0, 3U); at += 24U;
    }
    return at;
}

size_t np2audio86_guest_evidence_serialize_state(
    const np2audio86_guest_state_snapshot_t *s, uint8_t *out)
{
    size_t at = 0U;
    le64(out + at, s->frame_timestamp); at += 8U; le64(out + at, s->guest_cycles); at += 8U;
    le64(out + at, s->sequence); at += 8U; le32(out + at, s->cpu_remainder); at += 4U;
    le32(out + at, s->opna_base); at += 4U;
    out[at++] = s->opna_address_low; out[at++] = s->opna_address_extended;
    out[at++] = s->opna_data; out[at++] = s->opna_extension;
    out[at++] = s->opna_capabilities; out[at++] = s->opna_status; out[at++] = s->timer_control;
    le16(out + at, s->timer_a_value); at += 2U; out[at++] = s->timer_b_value; out[at++] = s->timer_irq;
    out[at++] = s->pcm_soundflags; out[at++] = s->pcm_fifo; out[at++] = s->pcm_dactrl;
    out[at++] = s->pcm_volume; out[at++] = s->pcm_rate;
    le16(out + at, s->pcm_fifo_size); at += 2U; le16(out + at, s->pcm_fifo_level); at += 2U;
    le32(out + at, s->pcm_virtual_buffer); at += 4U; le32(out + at, s->pcm_read_position); at += 4U;
    out[at++] = s->pcm_irq; out[at++] = s->pcm_reqirq; le32(out + at, s->pcm_rescue); at += 4U;
    out[at++] = s->pcm_irq_line; out[at++] = s->pcm_stepbit; le16(out + at, s->pcm_stepmask); at += 2U;
    le32(out + at, s->pcm_rateval); at += 4U; le64(out + at, s->pcm_stepclock); at += 8U;
    le64(out + at, s->pcm_lastclock); at += 8U; le64(out + at, s->pcm_lastclockforwait); at += 8U;
    le32(out + at, s->pcm_real_buffer); at += 4U; le32(out + at, s->pcm_write_position); at += 4U;
    le32(out + at, s->pcm_step_remainder); at += 4U; out[at++] = s->soundrom_rejected;
    out[at++] = s->bound; out[at++] = 0U; out[at++] = 0U;
    return at;
}
