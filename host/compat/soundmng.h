#ifndef NP2_HOST_SOUNDMNG_H
#define NP2_HOST_SOUNDMNG_H

enum {
    SOUND_PCMSEEK = 0,
    SOUND_PCMSEEK1 = 1,
    SOUND_RELAY1 = 2
};

void soundmng_play(void);
void soundmng_stop(void);
void soundmng_sync(void);
BRESULT soundmng_pcmplay(UINT number, BOOL loop);
void soundmng_pcmstop(UINT number);

#endif /* NP2_HOST_SOUNDMNG_H */
