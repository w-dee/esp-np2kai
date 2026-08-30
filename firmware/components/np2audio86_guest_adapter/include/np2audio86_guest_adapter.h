/*
 * Narrow PC-9801-86 guest/audio ownership seam.
 *
 * This header is an API contract for the 86R.1 source/build closure.  It is
 * intentionally opaque: no worker-owned OPNA/PCM86 object or sample buffer
 * is exposed to the guest-side board sources.
 */
#ifndef NP2AUDIO86_GUEST_ADAPTER_H
#define NP2AUDIO86_GUEST_ADAPTER_H

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
                                 uint8_t timer_a_event, uint8_t timer_b_event);
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

#ifdef __cplusplus
}
#endif

#endif /* NP2AUDIO86_GUEST_ADAPTER_H */
