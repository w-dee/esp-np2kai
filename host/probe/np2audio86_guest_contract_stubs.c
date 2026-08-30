/*
 * 86R.1 compile/link-contract stubs only.
 *
 * This translation unit is never part of firmware.  It supplies the narrow
 * adapter symbols and I/O attachment symbols needed to link the prepared
 * guest sources without constructing upstream OPNA or PCM86 objects.
 */
#include <compiler.h>
#include <pccore.h>
#include <io/iocore.h>
#include <cbus/cbuscore.h>
#include <np2audio86_guest_adapter.h>

void np2audio86_guest_opna_write_address_low(uint8_t value) { (void)value; }
void np2audio86_guest_opna_write_data_low(uint8_t value) { (void)value; }
void np2audio86_guest_opna_write_address_extended(uint8_t value) { (void)value; }
void np2audio86_guest_opna_write_data_extended(uint8_t value) { (void)value; }
uint8_t np2audio86_guest_opna_read_status(void) { return 0; }
uint8_t np2audio86_guest_opna_read_data(void) { return 0; }
uint8_t np2audio86_guest_opna_read_extended_status(void) { return 0xff; }
uint8_t np2audio86_guest_opna_read_extended_data(void) { return 0xff; }
uint8_t np2audio86_guest_opna_read_joy(void) { return 0; }
void np2audio86_guest_opna_set_extension(uint8_t enabled) { (void)enabled; }
void np2audio86_guest_opna_reset(uint8_t capabilities, uint32_t irq,
                                 uint8_t timer_a_event, uint8_t timer_b_event) {
    (void)capabilities;
    (void)irq;
    (void)timer_a_event;
    (void)timer_b_event;
}
void np2audio86_guest_opna_set_config(uint8_t channels, uint32_t mode) {
    (void)channels;
    (void)mode;
}
void np2audio86_guest_opna_set_base(uint16_t base) { (void)base; }
uint16_t np2audio86_guest_opna_base(void) { return 0x100; }
void np2audio86_guest_opna_register_extension(void (*callback)(uint8_t enabled)) {
    (void)callback;
}
void np2audio86_guest_opna_bind(void) {}
void np2audio86_guest_opna_unbind(void) {}
void np2audio86_guest_soundrom_load(uint32_t address, const char *name) {
    (void)address;
    (void)name;
}
void np2audio86_guest_audio_sync(void) {}
void np2audio86_guest_host_record_io(uint16_t port, uint8_t direction,
                                     uint8_t value, uint8_t result) {
    (void)port;
    (void)direction;
    (void)value;
    (void)result;
}

void np2audio86_guest_pcm86_write(uint8_t register_index, uint8_t value) {
    (void)register_index;
    (void)value;
}
void np2audio86_guest_pcm86_write_data(uint8_t value) { (void)value; }
void np2audio86_guest_pcm86_set_mixer_volume(uint8_t value) { (void)value; }
uint8_t np2audio86_guest_pcm86_read(uint8_t register_index) {
    (void)register_index;
    return 0;
}
void np2audio86_guest_pcm86_set_options(uint8_t dip_switch) {
    (void)dip_switch;
}
void np2audio86_guest_pcm86_stream_bind(void) {}
void np2audio86_guest_pcm86_stream_unbind(void) {}

void cbuscore_attachsndex(UINT port, const IOOUT *out, const IOINP *inp) {
    (void)port;
    (void)out;
    (void)inp;
}
void cbuscore_detachsndex(UINT port) { (void)port; }

BRESULT iocore_attachout(UINT port, IOOUT func) {
    (void)port;
    (void)func;
    return TRUE;
}
BRESULT iocore_detachout(UINT port) {
    (void)port;
    return TRUE;
}
BRESULT iocore_attachinp(UINT port, IOINP func) {
    (void)port;
    (void)func;
    return TRUE;
}
BRESULT iocore_detachinp(UINT port) {
    (void)port;
    return TRUE;
}
